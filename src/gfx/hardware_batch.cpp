#include <opent1/gfx/render_limits.hpp>
#include <opent1/gfx/hardware_batch.hpp>
#include <opent1/core/reusable_scratch.hpp>
#include <atomic>
#include <algorithm>
#include <cmath>
#include <list>
#include <memory_resource>
#include <ranges>
#include <unordered_map>

namespace opent1::gfx {
namespace {
struct BatchScratch {
    // Common scene lookups fit here. Larger batches use ordinary scoped heap
    // allocations, released on return rather than retained without a bound.
    std::array<std::byte, 256 * 1024> storage;
    std::pmr::monotonic_buffer_resource memory{storage.data(), storage.size()};

    void clear() {
        memory.release();
    }
};

bool unit(Flt32 v) {
    return std::isfinite(v) && v >= 0 && v <= 1;
}

bool unit(Point3F v) {
    return unit(v.x) && unit(v.y) && unit(v.z);
}

bool bounded(Flt32 v, Flt32 limit) {
    return std::isfinite(v) && std::abs(v) <= limit;
}

bool validGamma(Flt32 v) {
    return std::isfinite(v) && v >= .05f && v <= 20;
}

bool vertexAlpha(HardwareAlphaSource source) {
    return source == HardwareAlphaSource::Vertex || source == HardwareAlphaSource::TextureVertex;
}

bool valid(const HardwareMaterial& m) {
    return unit(m.color) && unit(m.opacity) && unit(m.shade) && unit(m.haze) &&
           m.shadeSource <= PolygonSource::Vertex && m.hazeSource <= PolygonSource::Vertex &&
           m.blend <= HardwareBlend::Multiply && m.depth <= HardwareDepth::WriteOnly &&
           m.alphaSource <= HardwareAlphaSource::TextureVertex && unit(m.alphaCutoff) &&
           m.paletteMode <= HardwarePaletteMode::OpenGLPacked && validGamma(m.paletteGamma) &&
           m.lightMapMode <= HardwareLightMapMode::OpenGLPacked &&
           (m.lightMap || m.lightMapMode == HardwareLightMapMode::OpenGL) &&
           (m.paletteMode == HardwarePaletteMode::Native ? m.paletteGamma == 1
                                                         : (m.texture && !m.paletteAlpha)) &&
           m.mipFilter <= HardwareMipFilter::Linear && bounded(m.lodBias, 16) &&
           (m.texture ||
            (!m.wrap && !m.linear && !m.magnificationLinear && !m.transparent && !m.paletteAlpha &&
             m.mipFilter == HardwareMipFilter::None && m.lodBias == 0));
}

bool validMeshPolygon(const HardwarePolygon& p) {
    const auto& t = p.transform;
    const auto finite = [](const auto& values) {
        return std::ranges::all_of(values, [](auto v) {
            return std::isfinite(v);
        });
    };
    const auto integer = [](Flt32 v, Flt32 last) {
        return v >= 0 && v <= last && std::trunc(v) == v;
    };
    if (p.material.twoSided || p.material.shadeSource != PolygonSource::None || !p.indexCount ||
        p.indexCount % 3 || p.firstIndex % 3 || p.firstIndex > p.mesh->indices().size() ||
        p.indexCount > p.mesh->indices().size() - p.firstIndex ||
        !std::ranges::all_of(t.camera, finite) || !finite(t.optics) || !finite(t.clip) ||
        !std::ranges::all_of(t.parameters, finite) || !finite(t.geometry) || !finite(t.ambient) ||
        !finite(t.mapping) || t.optics[0] <= 0 || t.optics[1] <= 0 || t.clip[0] < 1.f / 65536 ||
        t.clip[1] > 65536 || t.clip[1] <= t.clip[0] || t.clip[2] != 1)
        return false;
    if (t.geometry[2] == 0)
        return p.mesh->encoding() == HardwareMesh::Encoding::Color &&
               p.material.hazeSource != PolygonSource::Constant;
    if (t.geometry[2] == 1) {
        return p.mesh->encoding() == HardwareMesh::Encoding::Color && t.geometry[0] > 0 &&
               t.geometry[1] > 0 && integer(t.geometry[3], 31) && p.mesh->vertices().size() == 18 &&
               std::ranges::all_of(p.mesh->vertices(),
                                   [&](const auto& v) {
                                       return integer(v.inverseDepth, 17);
                                   }) &&
               std::ranges::all_of(t.parameters, [](const auto& h) {
                   return bounded(h[0], 1e12f) && bounded(h[1], 1e12f) && h[2] >= 0 && h[3] >= 0;
               });
    }
    if (t.geometry[2] != 2 || p.mesh->encoding() != HardwareMesh::Encoding::TsNormals ||
        !unit(t.geometry[0]) || !integer(t.ambient[3], 2) || t.ambient[0] < 0 || t.ambient[1] < 0 ||
        t.ambient[2] < 0)
        return false;
    for (auto i : std::views::iota(std::size_t{0}) | std::views::take_while([&](std::size_t i) {
                      return i < std::size_t(t.ambient[3]);
                  })) {
        const auto& source = t.parameters[i * 4];
        const auto& intensity = t.parameters[i * 4 + 1];
        const auto& attenuation = t.parameters[i * 4 + 2];
        if (!integer(source[3], 3) || intensity[0] < 0 || intensity[1] < 0 || intensity[2] < 0 ||
            t.parameters[i * 4 + 3][3] < 0 ||
            (source[3] == 3 && (intensity[3] <= 0 || !integer(attenuation[3], 3) ||
                                attenuation[0] < 0 || attenuation[1] < 0 || attenuation[2] < 0)))
            return false;
    }
    return true;
}

double cross(Point2F a, Point2F b, Point2F c) {
    return (double(b.x) - a.x) * (double(c.y) - a.y) - (double(b.y) - a.y) * (double(c.x) - a.x);
}

std::optional<double> winding(std::span<const PolygonVertex> vertices) {
    double area{};
    if (!std::ranges::all_of(std::views::iota(std::size_t{0}, vertices.size()), [&](auto i) {
            const auto a = vertices[i].position, b = vertices[(i + 1) % vertices.size()].position;
            if (std::ranges::any_of(vertices.subspan(i + 1), [&](const auto& v) {
                    return v.position == a;
                })) {
                return false;
            }
            area += double(a.x) * b.y - double(a.y) * b.x;
            return true;
        })) {
        return {};
    }
    if (area == 0) {
        return {};
    }
    // All corners must lie on the same side of every edge. Testing local turns
    // alone accepts self-intersecting stars and can hide invalid fan geometry.
    if (!std::ranges::all_of(std::views::iota(std::size_t{0}, vertices.size()), [&](auto i) {
            return std::ranges::all_of(vertices, [&](const auto& v) {
                return cross(vertices[i].position,
                             vertices[(i + 1) % vertices.size()].position,
                             v.position) *
                           area >=
                       0;
            });
        })) {
        return {};
    }
    return area;
}

UInt64 expandedBytes(const Bitmap& bitmap) {
    if (!bitmap.valid()) {
        return UInt64(Bitmap::MaxFileBytes) + 1;
    }
    UInt64 result{};
    for (auto level : std::views::iota(Int32{0}) | std::views::take_while([&](Int32 level) {
                          return level < bitmap.detailLevels;
                      })) {
        result += UInt64(UInt32(std::max(1, bitmap.width >> level))) *
                  UInt32(std::max(1, bitmap.height >> level)) * 4;
    }
    return result;
}

const Palette*
texturePalette(const Bitmap& bitmap, const HardwareMaterial& material, const Palette* surface) {
    if (material.useSurfacePalette && surface) {
        return surface;
    }
    return bitmap.palette ? &*bitmap.palette : surface;
}

std::optional<HardwareTexture>
expand(const Bitmap& bitmap, const HardwareMaterial& material, const Palette* fallback) {
    const auto transparent = material.transparent, paletteAlpha = material.paletteAlpha;
    const bool legacy = material.paletteMode != HardwarePaletteMode::Native;
    if (!bitmap.valid()) {
        return {};
    }
    const bool alpha88 = bitmap.bitDepth == 16 && (bitmap.attributes & BitmapAlpha88);
    const auto* palette = texturePalette(bitmap, material, fallback);
    // prepare() validates each shared palette once for this synchronous batch.
    if ((bitmap.bitDepth == 8 || alpha88) && !palette) {
        return {};
    }
    if (transparent && bitmap.bitDepth != 8 && !alpha88) {
        return {};
    }
    if (paletteAlpha && bitmap.bitDepth != 8) {
        return {};
    }
    if (legacy && bitmap.bitDepth != 8) {
        return {};
    }
    const auto bytes = expandedBytes(bitmap);
    if (bytes > Bitmap::MaxFileBytes) {
        return {};
    }
    HardwareTexture result{bitmap.width, bitmap.height, {}, {}};
    result.dynamic = material.dynamicTexture;
    const auto* sub = palette && (bitmap.bitDepth == 8 || alpha88)
                          ? &palette->find(bitmap.paletteIndex)
                          : nullptr;
    const auto* colors = sub ? &sub->colors : nullptr;
    if (legacy && sub->type != PaletteType::NoRemap && sub->type != PaletteType::ShadeHaze &&
        sub->type != PaletteType::Translucent && sub->type != PaletteType::Additive &&
        sub->type != PaletteType::Subtractive) {
        return {};
    }
    std::array<PaletteColor, 256> translated{};
    if (legacy) {
        std::array<UInt8, 256> gammaMap{};
        const Flt32 inverseGamma = 1.f / material.paletteGamma;
        for (auto i : std::views::iota(std::size_t{0}, gammaMap.size())) {
            // Original setPalette truncates the pow result to GLubyte.
            gammaMap[i] = UInt8(std::pow(Flt32(i) / 255.f, double(inverseGamma)) * 255.);
        }
        for (auto i : std::views::iota(std::size_t{0}, translated.size())) {
            const auto& c = (*colors)[i];
            auto& t = translated[i];
            t = {gammaMap[c.red], gammaMap[c.green], gammaMap[c.blue], c.flags};
            if (material.paletteMode == HardwarePaletteMode::OpenGL) {
                if (transparent) {
                    t.flags = i == 0 ? 0 : 255;
                }
            } else if (sub->type == PaletteType::Translucent) {
                // Original RGBA4444 transparent table deliberately retains
                // palette opacity, including entry zero.
                t = {UInt8((t.red >> 4) * 17),
                     UInt8((t.green >> 4) * 17),
                     UInt8((t.blue >> 4) * 17),
                     UInt8((t.flags >> 4) * 17)};
            } else {
                const auto five = [](UInt8 v) {
                    return UInt8(((v >> 3) * 255U + 15U) / 31U);
                };
                t = {five(t.red),
                     five(t.green),
                     five(t.blue),
                     UInt8(transparent && i == 0 ? 0 : 255)};
            }
        }
    }
    std::array<std::array<std::byte, 4>, 256> indexed{};
    if (bitmap.bitDepth == 8) {
        for (auto i : std::views::iota(std::size_t{0}, indexed.size())) {
            const auto& color = legacy ? translated[i] : (*colors)[i];
            const UInt8 alpha = legacy ? color.flags
                                       : UInt8(transparent && i == 0 ? 0
                                               : paletteAlpha        ? color.flags
                                                                     : 255);
            indexed[i] = {std::byte(color.red),
                          std::byte(color.green),
                          std::byte(color.blue),
                          std::byte(alpha)};
        }
    }
    for (auto level : std::views::iota(Int32{0}) | std::views::take_while([&](Int32 level) {
                          return level < bitmap.detailLevels;
                      })) {
        const auto width = std::max(1, bitmap.width >> level),
                   height = std::max(1, bitmap.height >> level);
        std::vector<std::byte> rgba(std::size_t(width) * std::size_t(height) * 4);
        std::ranges::for_each(std::views::iota(0, height), [&](auto y) {
            const auto row = bitmap.row(y, level);
            if (bitmap.bitDepth == 8) {
                auto* destination = rgba.data() + std::size_t(y) * std::size_t(width) * 4;
                for (auto x : std::views::iota(Int32{0}) | std::views::take_while([&](Int32 x) {
                                  return x < width;
                              })) {
                    const auto& pixel = indexed[std::to_integer<UInt8>(row[std::size_t(x)])];
                    std::ranges::copy(pixel, destination + std::size_t(x) * 4);
                }
                return;
            }
            std::ranges::for_each(std::views::iota(0, width), [&](auto x) {
                const auto offset = std::size_t(x) * std::size_t(bitmap.bitDepth / 8);
                const auto byte = [&](std::size_t i) {
                    return std::to_integer<UInt8>(row[offset + i]);
                };
                UInt8 red{}, green{}, blue{}, alpha{255};
                if (colors) {
                    const auto index = byte(0);
                    const auto& c = (*colors)[index];
                    if (legacy) {
                        const auto& t = translated[index];
                        red = t.red;
                        green = t.green;
                        blue = t.blue;
                        alpha = t.flags;
                    } else {
                        red = c.red;
                        green = c.green;
                        blue = c.blue;
                        if (paletteAlpha) {
                            alpha = c.flags;
                        }
                        if (alpha88) {
                            alpha = byte(1);
                        }
                        if (transparent && index == 0) {
                            alpha = 0;
                        }
                    }
                } else if (bitmap.bitDepth == 24) {
                    blue = byte(0);
                    green = byte(1);
                    red = byte(2);
                } else {
                    const auto rgb = UInt32(byte(0)) | (UInt32(byte(1)) << 8);
                    red = UInt8(((rgb >> 11) * 255 + 15) / 31);
                    green = UInt8((((rgb >> 5) & 63) * 255 + 31) / 63);
                    blue = UInt8(((rgb & 31) * 255 + 15) / 31);
                }
                const auto dest = (std::size_t(y) * std::size_t(width) + std::size_t(x)) * 4;
                std::ranges::copy(
                    std::array{std::byte(red), std::byte(green), std::byte(blue), std::byte(alpha)},
                    rgba.begin() + std::ptrdiff_t(dest));
            });
        });
        if (level == 0) {
            result.rgba = std::move(rgba);
        } else {
            result.mips.push_back({width, height, std::move(rgba)});
        }
    }
    return result;
}

HardwareTexture expand(const LightMap& map, HardwareLightMapMode mode) {
    HardwareTexture result{
        map.size.x, map.size.y, std::vector<std::byte>(map.samples.size() * 4), {}};
    result.lightMap = true;
    result.dynamic = map.dynamic;
    for (auto i : std::views::iota(std::size_t{0}, map.samples.size())) {
        if (map.format == LightMap::Format::Rgb565) {
            const auto v = map.samples[i];
            result.rgba[i * 4] = std::byte(((v >> 11) * 255 + 15) / 31);
            result.rgba[i * 4 + 1] = std::byte((((v >> 5) & 63) * 255 + 31) / 63);
            result.rgba[i * 4 + 2] = std::byte(((v & 31) * 255 + 15) / 31);
            result.rgba[i * 4 + 3] = std::byte{255};
            continue;
        }
        // Original gOGLTx.cpp dumpOGLLightmap expands each RGB nibble as
        // (n << 4) | 15, not n*17. The alpha nibble is unused by hardware.
        for (auto c : std::views::iota(std::size_t{0}, std::size_t{3})) {
            const auto nibble = (map.samples[i] >> ((2 - c) * 4)) & 15;
            result.rgba[i * 4 + c] = mode == HardwareLightMapMode::OpenGLPacked
                                         ? std::byte(((nibble * 2 + 1) * 255 + 15) / 31)
                                         : std::byte(nibble * 16 + 15);
        }
        result.rgba[i * 4 + 3] = std::byte{255};
    }
    return result;
}

UInt64 fingerprint(const HardwareTexture& texture) {
    UInt64 hash = 14695981039346656037ULL;
    const auto add = [&](const auto& rgba) {
        for (std::size_t i = 0; i < rgba.size(); i += 4) {
            const UInt32 pixel = std::to_integer<UInt8>(rgba[i]) |
                                 (UInt32(std::to_integer<UInt8>(rgba[i + 1])) << 8) |
                                 (UInt32(std::to_integer<UInt8>(rgba[i + 2])) << 16) |
                                 (UInt32(std::to_integer<UInt8>(rgba[i + 3])) << 24);
            hash = (hash ^ pixel) * 1099511628211ULL;
        }
    };
    add(texture.rgba);
    for (const auto& mip : texture.mips) {
        add(mip.rgba);
    }
    return hash;
}

std::shared_ptr<const HardwareTexture> immutable(HardwareTexture texture) {
    texture.hash = fingerprint(texture);
    return std::make_shared<const HardwareTexture>(std::move(texture));
}

struct ConversionKey {
    const void* source; // identity only; never dereferenced after prepare()
    UInt32 policy;
    Flt32 gamma{1};
    bool operator==(const ConversionKey&) const = default;
};

struct ConversionHash {
    std::size_t operator()(const ConversionKey& key) const noexcept {
        return std::hash<const void*>{}(key.source) ^ (std::hash<Flt32>{}(key.gamma) << 1) ^
               (key.policy * 0x9e3779b9U);
    }
};

ConversionKey conversionKey(const HardwareMaterial& m) {
    return {m.texture,
            UInt32(m.transparent) | (UInt32(m.paletteAlpha) << 1) | (UInt32(m.paletteMode) << 2) |
                (UInt32(m.useSurfacePalette) << 4) | (UInt32(m.dynamicTexture) << 16),
            m.paletteGamma};
}

ConversionKey lightMapKey(const HardwareMaterial& m) {
    return {m.lightMap, 256U + UInt32(m.lightMapMode) + (m.lightMap->dynamic ? 512U : 0U)};
}

struct SourceLayout {
    Int32 width{}, height{}, bitDepth{}, levels{};
    UInt32 attributes{}, paletteIndex{};
    bool operator==(const SourceLayout&) const = default;
};

// Bitmap and LightMap expose mutable vectors, so pointer/revision-only hits
// cannot be trusted. Compare the compact source snapshot, then share the
// immutable RGBA allocation and its precomputed hash. No RGBA scan on hits.
class ConvertedTextures {
    struct Entry {
        ConversionKey key;
        SourceLayout layout;
        std::vector<std::byte> source;
        std::array<PaletteColor, 256> colors{};
        PaletteType type{};
        std::shared_ptr<const HardwareTexture> texture;
        std::size_t bytes;
    };

    std::list<Entry> entries_;
    std::unordered_map<ConversionKey, std::list<Entry>::iterator, ConversionHash> index_;
    std::size_t bytes_{};
    std::size_t budgetBytes_{64U * 1024U * 1024U}, budgetEntries_{4096};
    UInt64 hits_{}, builds_{};

    void erase(std::list<Entry>::iterator entry) {
        bytes_ -= entry->bytes;
        index_.erase(entry->key);
        entries_.erase(entry);
    }

public:
    HardwareConversionCacheStats stats() const {
        return {entries_.size(), bytes_, budgetBytes_, budgetEntries_, hits_, builds_};
    }

    void budget(std::size_t bytes, std::size_t entries) {
        budgetBytes_ = bytes;
        budgetEntries_ = entries;
        while (!entries_.empty() && (entries_.size() > entries || bytes_ > bytes))
            erase(std::prev(entries_.end()));
    }

    void clear() {
        index_.clear();
        entries_.clear();
        bytes_ = 0;
    }

    template <class Convert>
    std::shared_ptr<const HardwareTexture> get(ConversionKey key,
                                               SourceLayout layout,
                                               std::span<const std::byte> source,
                                               const SubPalette* palette,
                                               Convert convert) {
        const auto found = index_.find(key);
        if (found != index_.end()) {
            const auto entry = found->second;
            if (entry->layout == layout &&
                (!palette || (entry->type == palette->type && entry->colors == palette->colors)) &&
                std::ranges::equal(entry->source, source)) {
                entries_.splice(entries_.begin(), entries_, entry);
                ++hits_;
                return entry->texture;
            }
        }
        auto expanded = convert();
        if (!expanded) {
            return {};
        }
        auto texture = immutable(std::move(*expanded));
        ++builds_;
        auto bytes = sizeof(Entry) + source.size() + texture->rgba.size();
        for (const auto& mip : texture->mips) {
            bytes += sizeof(HardwareMip) + mip.rgba.size();
        }
        if (found != index_.end()) {
            erase(found->second);
        }
        if (bytes > budgetBytes_ || !budgetEntries_) {
            return texture;
        }
        while (!entries_.empty() &&
               (entries_.size() >= budgetEntries_ || bytes_ > budgetBytes_ - bytes)) {
            erase(std::prev(entries_.end()));
        }
        Entry entry{key, layout, {source.begin(), source.end()}, {}, {}, texture, bytes};
        if (palette) {
            entry.colors = palette->colors;
            entry.type = palette->type;
        }
        entries_.push_front(std::move(entry));
        index_.emplace(key, entries_.begin());
        bytes_ += bytes;
        return texture;
    }
};

thread_local ConvertedTextures convertedTextures;
const auto whiteTexture = immutable({1, 1, std::vector<std::byte>(4, std::byte{255}), {}});
}

void clearHardwareTextureConversions() {
    convertedTextures.clear();
}

HardwareConversionCacheStats hardwareTextureConversionStats() {
    return convertedTextures.stats();
}

void setHardwareTextureConversionBudget(std::size_t bytes, std::size_t entries) {
    convertedTextures.budget(bytes, entries);
}

std::optional<HardwareMaterial>
hardwarePaletteFill(const Palette& palette, UInt8 color, UInt32 paletteIndex, Flt32 gamma) {
    if (!palette.valid() || !validGamma(gamma)) {
        return {};
    }
    const auto& c = palette.find(paletteIndex).colors[color];
    HardwareMaterial result;
    result.color = {Flt32(c.red) / 255, Flt32(c.green) / 255, Flt32(c.blue) / 255};
    if (gamma != 1) {
        for (auto i : std::views::iota(std::size_t{0}, std::size_t{3})) {
            result.color[i] = Flt32(std::pow(result.color[i], double(1.f / gamma)));
        }
    }
    result.opacity = Flt32(c.flags) / 255;
    result.alphaSource = HardwareAlphaSource::Constant;
    result.blend = HardwareBlend::Alpha;
    return result;
}

std::optional<HardwareBatch> HardwareBatch::prepare(std::span<const HardwarePolygon> polygons,
                                                    const Palette* palette) {
    constexpr auto indexLimit = std::size_t(std::numeric_limits<UInt32>::max());
    if (polygons.size() > std::min(renderLimits().polygons, indexLimit) ||
        renderLimits().textureBytes < 4) {
        return {};
    }
    // A mission can expose hundreds of terrain tiles plus shape materials.
    // Keep the aggregate byte bound and one texture per admitted polygon.
    const std::size_t maximumTextures = renderLimits().textures;
    HardwareBatch result;
    result.textures_.push_back(whiteTexture);
    detail::ScratchLease<BatchScratch> workspace;
    auto* memory = &workspace.get().memory;
    std::pmr::unordered_map<ConversionKey, UInt32, ConversionHash> textures{memory};
    std::pmr::vector<const Palette*> validatedPalettes{memory};
    std::pmr::unordered_map<ConversionKey, UInt32, ConversionHash> lightMaps{memory};
    std::pmr::unordered_multimap<UInt64, UInt32> meshInstances{memory};
    std::size_t inputVertices{}, textureBytes{4};
    if (!std::ranges::all_of(polygons, [&](const auto& p) {
            const auto& m = p.material;
            if (p.mesh && !validMeshPolygon(p))
                return false;
            if (p.vertices.size() < 3 || p.vertices.size() > 100 ||
                p.vertices.size() > renderLimits().vertices - inputVertices || !valid(m)) {
                return false;
            }
            inputVertices += p.vertices.size();
            if (m.lightMap
                    ? (p.lightMapCoordinates.size() != p.vertices.size() || !m.lightMap->valid())
                    : !p.lightMapCoordinates.empty()) {
                return false;
            }
            if (!std::ranges::all_of(p.lightMapCoordinates, [](auto uv) {
                    return bounded(uv.x, 65536) && bounded(uv.y, 65536);
                })) {
                return false;
            }
            if (vertexAlpha(m.alphaSource) ? p.vertexOpacity.size() != p.vertices.size()
                                           : !p.vertexOpacity.empty()) {
                return false;
            }
            if (!std::ranges::all_of(p.vertexOpacity, [](auto alpha) {
                    return unit(alpha);
                })) {
                return false;
            }
            if (!std::ranges::all_of(p.vertices, [](const auto& v) {
                    return bounded(v.position.x, 16384) && bounded(v.position.y, 16384) &&
                           bounded(v.texture.x, 65536) && bounded(v.texture.y, 65536) &&
                           unit(v.color) && unit(v.haze) && std::isfinite(v.inverseDepth) &&
                           v.inverseDepth >= 1.f / 65536 && v.inverseDepth <= 65536;
                })) {
                return false;
            }
            const auto area = winding(p.vertices);
            if (!area) {
                return false;
            }
            UInt32 texture{};
            if (m.texture) {
                const auto key = conversionKey(m);
                const auto found = textures.find(key);
                if (found == textures.end()) {
                    if (textures.size() == maximumTextures ||
                        result.textures_.size() == indexLimit) {
                        return false;
                    }
                    if (m.texture->bitDepth == 8 ||
                        (m.texture->bitDepth == 16 && (m.texture->attributes & BitmapAlpha88))) {
                        const auto* colors = texturePalette(*m.texture, m, palette);
                        if (!colors) {
                            return false;
                        }
                        if (std::ranges::find(validatedPalettes, colors) ==
                            validatedPalettes.end()) {
                            if (!colors->valid()) {
                                return false;
                            }
                            validatedPalettes.push_back(colors);
                        }
                    }
                    // Bound the aggregate before allocating an expanded texture.
                    const auto bytes = expandedBytes(*m.texture);
                    if (bytes > renderLimits().textureBytes - textureBytes ||
                        result.textures_.size() == indexLimit) {
                        return false;
                    }
                    const auto& bitmap = *m.texture;
                    const auto* colors = texturePalette(bitmap, m, palette);
                    const bool indexed =
                        bitmap.bitDepth == 8 ||
                        (bitmap.bitDepth == 16 && (bitmap.attributes & BitmapAlpha88));
                    auto expanded = convertedTextures.get(
                        key,
                        {bitmap.width,
                         bitmap.height,
                         bitmap.bitDepth,
                         bitmap.detailLevels,
                         bitmap.attributes,
                         bitmap.paletteIndex},
                        bitmap.pixels,
                        indexed ? &colors->find(bitmap.paletteIndex) : nullptr,
                        [&] {
                            return expand(bitmap, m, palette);
                        });
                    if (!expanded) {
                        return false;
                    }
                    textureBytes += std::size_t(bytes);
                    texture = UInt32(result.textures_.size());
                    result.textures_.push_back(std::move(expanded));
                    textures.emplace(key, texture);
                } else {
                    texture = found->second;
                }
            }
            UInt32 lightMap{};
            if (m.lightMap) {
                const auto key = lightMapKey(m);
                const auto found = lightMaps.find(key);
                if (found != lightMaps.end()) {
                    lightMap = found->second;
                } else {
                    const auto bytes = m.lightMap->samples.size() * 4;
                    if (bytes > renderLimits().textureBytes - textureBytes) {
                        return false;
                    }
                    lightMap = UInt32(result.textures_.size());
                    textureBytes += bytes;
                    const auto& map = *m.lightMap;
                    result.textures_.push_back(
                        convertedTextures.get(key,
                                              {map.size.x, map.size.y, Int32(map.format)},
                                              std::as_bytes(std::span(map.samples)),
                                              nullptr,
                                              [&] {
                                                  return std::optional(expand(map, m.lightMapMode));
                                              }));
                    lightMaps.emplace(key, lightMap);
                }
            }
            if (*area < 0 && !m.twoSided) {
                return true;
            }
            HardwareDraw draw{UInt32(result.vertices_.size()),
                              UInt32((p.vertices.size() - 2) * 3),
                              texture,
                              m.color,
                              m.opacity,
                              m.blend,
                              m.depth,
                              m.perspective,
                              m.wrap,
                              m.linear,
                              lightMap,
                              m.mipFilter,
                              m.lodBias,
                              m.alphaSource,
                              m.alphaCutoff,
                              m.transparent,
                              m.magnificationLinear.value_or(m.linear)};
            if (p.mesh) {
                if (p.transform.geometry[2] == 1) {
                    // Selection submits each native terrain patch once, in traversal order.
                    result.meshes_.push_back({p.mesh, p.transform});
                    draw.mesh = UInt32(result.meshes_.size());
                } else if (!result.meshes_.empty() && result.meshes_.back().mesh == p.mesh &&
                           result.meshes_.back().transform == p.transform) {
                    draw.mesh = UInt32(result.meshes_.size());
                } else {
                    UInt64 key = p.mesh->revision();
                    for (const auto b : std::as_bytes(std::span(&p.transform, 1)))
                        key = (key ^ std::to_integer<UInt8>(b)) * 1099511628211ULL;
                    const auto [begin, end] = meshInstances.equal_range(key);
                    auto instance = std::find_if(begin, end, [&](const auto& entry) {
                        const auto& value = result.meshes_[entry.second];
                        return value.mesh == p.mesh && value.transform == p.transform;
                    });
                    if (instance == end) {
                        result.meshes_.push_back({p.mesh, p.transform});
                        draw.mesh = UInt32(result.meshes_.size());
                        meshInstances.emplace(key, draw.mesh - 1);
                    } else {
                        draw.mesh = instance->second + 1;
                    }
                }
                draw.firstVertex = p.firstIndex;
                draw.vertexCount = p.indexCount;
                result.draws_.push_back(draw);
                return true;
            }
            const auto outputCount = (p.vertices.size() - 2) * 3;
            if (outputCount > renderLimits().outputVertices - result.vertices_.size() ||
                outputCount >
                    std::size_t(std::numeric_limits<UInt32>::max()) - result.vertices_.size()) {
                return false;
            }
            const auto add = [&](std::size_t i) {
                auto v = p.vertices[i];
                if (m.shadeSource != PolygonSource::Vertex) {
                    const auto shade = m.shadeSource == PolygonSource::Constant ? m.shade : 1.f;
                    v.color = {shade, shade, shade};
                }
                if (m.hazeSource != PolygonSource::Vertex) {
                    v.haze = m.hazeSource == PolygonSource::Constant ? m.haze : 0;
                }
                result.vertices_.push_back({v.position,
                                            v.texture,
                                            v.color,
                                            v.haze,
                                            v.inverseDepth,
                                            m.lightMap ? p.lightMapCoordinates[i] : Point2F{},
                                            vertexAlpha(m.alphaSource) ? p.vertexOpacity[i] : 1.f});
            };
            std::ranges::for_each(std::views::iota(std::size_t{1}, p.vertices.size() - 1),
                                  [&](auto i) {
                                      add(0);
                                      add(i);
                                      add(i + 1);
                                  });
            result.draws_.push_back(draw);
            return true;
        })) {
        return {};
    }
    for (const auto& t : result.textures_) {
        result.textureHashes_.push_back(t->hash);
    }
    result.layerEnds_.push_back(UInt32(result.draws_.size()));
    return result;
}

std::optional<HardwareBatch> HardwareBatch::compose(std::span<const HardwareBatch* const> layers) {
    HardwareBatch result;
    constexpr auto indexLimit = std::size_t(std::numeric_limits<UInt32>::max());
    std::size_t bytes{};
    for (const auto* layer : layers) {
        if (!layer || layer->textures_.empty() || layer->layerEnds_.empty() ||
            layer->vertices_.size() >
                std::min(renderLimits().outputVertices, indexLimit) - result.vertices_.size() ||
            layer->draws_.size() >
                std::min(renderLimits().polygons, indexLimit) - result.draws_.size() ||
            layer->textures_.size() >
                std::min(renderLimits().textures, indexLimit) - result.textures_.size()) {
            return {};
        }
        for (const auto& texture : layer->textures()) {
            const auto add = [&](std::size_t size) {
                if (size > renderLimits().textureBytes - bytes) {
                    return false;
                }
                bytes += size;
                return true;
            };
            if (!add(texture.rgba.size())) {
                return {};
            }
            for (const auto& mip : texture.mips) {
                if (!add(mip.rgba.size())) {
                    return {};
                }
            }
        }
        const auto firstVertex = UInt32(result.vertices_.size()),
                   firstTexture = UInt32(result.textures_.size());
        const auto firstDraw = UInt32(result.draws_.size());
        const auto firstMesh = UInt32(result.meshes_.size());
        result.meshes_.insert(result.meshes_.end(), layer->meshes_.begin(), layer->meshes_.end());
        for (const auto end : layer->layerEnds_) {
            result.layerEnds_.push_back(firstDraw + end);
        }
        result.vertices_.insert(
            result.vertices_.end(), layer->vertices_.begin(), layer->vertices_.end());
        result.textures_.insert(
            result.textures_.end(), layer->textures_.begin(), layer->textures_.end());
        result.textureHashes_.insert(result.textureHashes_.end(),
                                     layer->textureHashes_.begin(),
                                     layer->textureHashes_.end());
        for (auto draw : layer->draws_) {
            if (draw.mesh)
                draw.mesh += firstMesh;
            else
                draw.firstVertex += firstVertex;
            draw.texture += firstTexture;
            draw.lightMap += firstTexture;
            result.draws_.push_back(draw);
        }
    }
    return result;
}

std::shared_ptr<const HardwareMesh> HardwareMesh::create(std::vector<HardwareVertex> vertices,
                                                         std::vector<UInt32> indices,
                                                         Encoding encoding) {
    if ((encoding != Encoding::Color && encoding != Encoding::TsNormals) || vertices.empty() ||
        vertices.size() > 4'194'304 || indices.empty() || indices.size() > 12'582'912 ||
        indices.size() % 3 ||
        !std::ranges::all_of(indices,
                             [&](auto i) {
                                 return i < vertices.size();
                             }) ||
        !std::ranges::all_of(vertices, [encoding](const auto& v) {
            return bounded(v.position.x, 1e12f) && bounded(v.position.y, 1e12f) &&
                   bounded(v.inverseDepth, 1e12f) && bounded(v.texture.x, 65536) &&
                   bounded(v.texture.y, 65536) && bounded(v.lightMap.x, 65536) &&
                   bounded(v.lightMap.y, 65536) && unit(v.haze) &&
                   (encoding == Encoding::Color
                        ? unit(v.color) && unit(v.opacity)
                        : bounded(v.color.x, 1) && bounded(v.color.y, 1) && bounded(v.color.z, 1) &&
                              (v.opacity == 0 || v.opacity == 1 || v.opacity == 2));
        }))
        return {};
    static std::atomic<UInt64> next{1};
    auto mesh = std::make_shared<HardwareMesh>();
    mesh->vertices_ = std::move(vertices);
    mesh->indices_ = std::move(indices);
    mesh->revision_ = next.fetch_add(1, std::memory_order_relaxed);
    mesh->encoding_ = encoding;
    return mesh;
}

HardwareBatch::MemoryStats HardwareBatch::memoryStats() const {
    MemoryStats result;
    result.batchBytes =
        sizeof(*this) + vertices_.capacity() * sizeof(HardwareVertex) +
        textures_.capacity() * sizeof(textures_[0]) + textureHashes_.capacity() * sizeof(UInt64) +
        draws_.capacity() * sizeof(HardwareDraw) + layerEnds_.capacity() * sizeof(UInt32) +
        meshes_.capacity() * sizeof(HardwareMeshInstance);
    std::unordered_map<const HardwareMesh*, bool> meshes;
    for (const auto& instance : meshes_)
        if (meshes.emplace(instance.mesh.get(), true).second)
            result.meshBytes += instance.mesh->residentBytes();
    std::unordered_map<const HardwareTexture*, bool> textures;
    for (const auto& t : textures_)
        if (textures.emplace(t.get(), true).second) {
            result.textureBytes += sizeof(HardwareTexture) + t->rgba.capacity() +
                                   t->mips.capacity() * sizeof(HardwareMip);
            for (const auto& mip : t->mips)
                result.textureBytes += mip.rgba.capacity();
        }
    return result;
}
} // namespace opent1::gfx
