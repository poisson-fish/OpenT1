#include <opent1/world/terrain_textures.hpp>
#include <bit>
#include <cmath>
#include <ranges>

namespace opent1::world {
namespace {
bool supported(const TsMaterialResources& materials, UInt32 index) {
    if (index >= UInt32(std::max(0, materials.list.materialsPerDetail)) ||
        index >= materials.list.materials.size() || index >= materials.textures.size()) {
        return false;
    }
    const auto& m = materials.list.materials[index];
    const auto* b = materials.textures[index].get();
    return m.type() == 3 && !(m.flags & 0x3000U) && b && b->valid() && b->bitDepth == 8 &&
           b->width == b->height && b->width >= 2 && b->width <= 256 &&
           std::has_single_bit(UInt32(b->width)) &&
           !(b->attributes & (gfx::BitmapTransparent | gfx::BitmapTranslucent |
                              gfx::BitmapAdditive | gfx::BitmapSubtractive | gfx::BitmapAlpha88));
}

// Original MipBlt traversal, expressed as bounded source coordinates.
Point2I source(UInt32 x, UInt32 y, UInt32 n, UInt8 flags) {
    const auto a = Int32(x), b = Int32(y), last = Int32(n) - 1;
    switch (flags & 7U) {
        case 1:
            return {b, last - a};
        case 2:
            return {last - a, b};
        case 3:
            return {last - b, last - a};
        case 4:
            return {a, last - b};
        case 5:
            return {b, a};
        case 6:
            return {last - a, last - b};
        case 7:
            return {last - b, a};
        default:
            return {a, b};
    }
}

UInt16 sample(std::span<const UInt16> samples, UInt32 side, Flt64 x, Flt64 y) {
    x = std::clamp(x, 0., Flt64(side - 1));
    y = std::clamp(y, 0., Flt64(side - 1));
    const auto ix = UInt32(x), iy = UInt32(y), jx = std::min(ix + 1, side - 1),
               jy = std::min(iy + 1, side - 1);
    UInt16 result{};
    for (UInt32 shift = 0; shift < 16; shift += 4) {
        const auto at = [&](UInt32 xx, UInt32 yy) {
            return Flt64((samples[std::size_t(yy) * side + xx] >> shift) & 15);
        };
        const auto top = std::lerp(at(ix, iy), at(jx, iy), x - ix),
                   bottom = std::lerp(at(ix, jy), at(jx, jy), x - ix);
        result |= UInt16(
            UInt16(std::clamp(std::floor(std::lerp(top, bottom, y - iy) + .5), 0., 15.)) << shift);
    }
    return result;
}
}

TerrainTextureCache::TerrainTextureCache(TerrainGridResources resources,
                                         std::size_t bytes,
                                         UInt32 entries)
    : resources_(std::move(resources)), maximumBytes_(bytes), maximumEntries_(entries) {
    if (!bytes || bytes > 64U * 1024U * 1024U || !entries || entries > 4096) {
        throw std::invalid_argument("invalid terrain cache limits");
    }
    clear();
}

TerrainTextureCache::TerrainTextureCache(const TerrainTextureCache& other)
    : resources_(other.resources_), supported_(other.supported_),
      maximumBytes_(other.maximumBytes_), bytes_(other.bytes_),
      maximumEntries_(other.maximumEntries_), builds_(other.builds_), hits_(other.hits_),
      evictions_(other.evictions_), entries_(other.entries_), handles_(other.handles_),
      cursor_(other.cursor_), wrap_(other.wrap_) {
    rebuildIndexes();
}

TerrainTextureCache& TerrainTextureCache::operator=(const TerrainTextureCache& other) {
    if (this != &other) {
        *this = TerrainTextureCache(other);
    }
    return *this;
}

std::size_t TerrainTextureCache::KeyHash::operator()(const Key& key) const noexcept {
    std::size_t hash{};
    const auto add = [&](UInt32 value) {
        hash ^= std::size_t(value) + 0x9e3779b9U + (hash << 6) + (hash >> 2);
    };
    add(key.block);
    add(key.x);
    add(key.y);
    add(key.cells);
    add(key.pixels);
    add(UInt32(key.baked) | (UInt32(key.hires) << 1) | (UInt32(key.density) << 2) |
        (UInt32(key.dynamicLighting) << 4) | (UInt32(key.nativeEmptyLevels) << 5) |
        (UInt32(key.timing) << 6));
    return hash;
}

TerrainTextureCache::Key TerrainTextureCache::lookupKey(Key key) {
    // Native callbacks identify a tile independently of its current download
    // size. The entry/handle still records pixels for resize and ring validity.
    if (key.timing != TerrainTextureTiming::RetainedLru) {
        key.pixels = 0;
    }
    return key;
}

bool TerrainTextureCache::sharedMap(const Key& key) {
    return key.baked && key.density == TerrainLightMapDensity::NativeOpenGL && !key.dynamicLighting;
}

UInt64 TerrainTextureCache::sharedMapKey(const Key& key) const {
    const auto n = resources_.blocks[key.block]->width;
    return (UInt64(key.block) << 3) | (UInt64(key.hires) << 2) |
           (UInt64(n == 256 && key.y + key.cells > 255) << 1) |
           UInt64(n == 256 && key.x + key.cells > 255);
}

void TerrainTextureCache::retain(const Entry& entry) {
    if (entry.tile.lightMap) {
        ++mapReferences_[entry.tile.lightMap.get()];
        if (sharedMap(entry.key)) {
            sharedMaps_[sharedMapKey(entry.key)] = entry.tile.lightMap;
        }
    }
    if (entry.samples) {
        auto& decoded = decodedSamples_[entry.key.block];
        decoded.samples = entry.samples;
        ++decoded.references;
    }
}

void TerrainTextureCache::rebuildIndexes() {
    // Staged stream caches share immutable payloads, but never list iterators.
    entryIndex_.clear();
    handleIndex_.clear();
    mapReferences_.clear();
    sharedMaps_.clear();
    decodedSamples_.clear();
    for (auto at = entries_.begin(); at != entries_.end(); ++at) {
        entryIndex_.emplace(lookupKey(at->key), at);
        retain(*at);
    }
    for (auto at = handles_.begin(); at != handles_.end(); ++at) {
        handleIndex_.emplace(lookupKey(at->requested), at);
    }
}

void TerrainTextureCache::clear() {
    entries_.clear();
    handles_.clear();
    entryIndex_.clear();
    handleIndex_.clear();
    mapReferences_.clear();
    sharedMaps_.clear();
    decodedSamples_.clear();
    cursor_ = 0;
    wrap_ = 0;
    bytes_ = 0;
    supported_.resize(resources_.materials.list.materials.size());
    for (UInt32 i = 0; i < supported_.size(); ++i) {
        supported_[i] = supported(resources_.materials, i);
    }
}

bool TerrainTextureCache::matches(const TerrainGridResources& r) const {
    if (r.grid.get() != resources_.grid.get() || r.blocks.size() != resources_.blocks.size() ||
        r.materials.textures.size() != resources_.materials.textures.size() ||
        r.materials.list.materials.size() != resources_.materials.list.materials.size()) {
        return false;
    }
    for (std::size_t i = 0; i < r.blocks.size(); ++i) {
        if (r.blocks[i].get() != resources_.blocks[i].get()) {
            return false;
        }
    }
    for (std::size_t i = 0; i < r.materials.textures.size(); ++i) {
        if (r.materials.textures[i].get() != resources_.materials.textures[i].get()) {
            return false;
        }
    }
    // Material records are copied into the bound snapshot; compare rendering
    // policy too, so reusing a cache across a changed material list is rejected.
    for (std::size_t i = 0; i < r.materials.list.materials.size(); ++i) {
        if (r.materials.list.materials[i].flags != resources_.materials.list.materials[i].flags) {
            return false;
        }
    }
    return r.materials.list.materialsPerDetail == resources_.materials.list.materialsPerDetail;
}

void TerrainTextureCache::rebind(TerrainGridResources resources) {
    for (auto at = handles_.begin(); at != handles_.end();) {
        if (resources.blocks[at->requested.block].get() ==
            resources_.blocks[at->requested.block].get()) {
            ++at;
        } else {
            handleIndex_.erase(lookupKey(at->requested));
            at = handles_.erase(at);
        }
    }
    for (auto at = entries_.begin(); at != entries_.end();) {
        if (resources.blocks[at->key.block].get() == resources_.blocks[at->key.block].get()) {
            ++at;
        } else {
            at = erase(at);
        }
    }
    resources_ = std::move(resources);
}

std::size_t TerrainTextureCache::additionalBytes(const BuiltTile& built, UInt32 block) const {
    const auto& tile = built.tile;
    return tile.texture->pixels.size() +
           (tile.lightMap && !mapReferences_.contains(tile.lightMap.get())
                ? tile.lightMap->samples.size() * 2
                : 0) +
           (built.samples && !decodedSamples_.contains(block)
                ? built.samples->size() * sizeof(UInt16)
                : 0);
}

std::list<TerrainTextureCache::Entry>::iterator
TerrainTextureCache::erase(std::list<Entry>::iterator at) {
    ++evictions_;
    bytes_ -= at->tile.texture->pixels.size();
    if (at->tile.lightMap) {
        const auto map = mapReferences_.find(at->tile.lightMap.get());
        if (!--map->second) {
            bytes_ -= at->tile.lightMap->samples.size() * 2;
            mapReferences_.erase(map);
            if (sharedMap(at->key)) {
                sharedMaps_.erase(sharedMapKey(at->key));
            }
        }
    }
    if (at->samples) {
        const auto decoded = decodedSamples_.find(at->key.block);
        if (!--decoded->second.references) {
            bytes_ -= at->samples->size() * sizeof(UInt16);
            decodedSamples_.erase(decoded);
        }
    }
    entryIndex_.erase(lookupKey(at->key));
    return entries_.erase(at);
}

bool TerrainTextureCache::compatible(UInt32 material, UInt32 other) const {
    const auto& m = resources_.materials;
    return material < supported_.size() && other < supported_.size() && supported_[material] &&
           supported_[other] && m.textures[material]->width == m.textures[other]->width &&
           m.textures[material]->paletteIndex == m.textures[other]->paletteIndex;
}

std::optional<TerrainTextureTile> TerrainTextureCache::tile(UInt32 block,
                                                            UInt32 x,
                                                            UInt32 y,
                                                            UInt32 cells,
                                                            UInt32 pixels,
                                                            bool baked,
                                                            bool hires,
                                                            TerrainLightMapDensity density,
                                                            bool dynamicLighting,
                                                            bool nativeEmptyLevels,
                                                            TerrainTextureTiming timing) {
    const Key key{block,
                  x,
                  y,
                  cells,
                  pixels,
                  baked,
                  hires,
                  density,
                  dynamicLighting,
                  nativeEmptyLevels,
                  timing};
    // OpenGL can reuse a download at a different requested size. Validate the
    // request before that hit path as well as before invoking a new callback.
    if (!std::has_single_bit(cells) || cells > 128 || !std::has_single_bit(pixels) ||
        pixels > 256 || pixels < cells || x % cells || y % cells) {
        return {};
    }
    if (timing != TerrainTextureTiming::RetainedLru &&
        timing != TerrainTextureTiming::NativeSoftware &&
        timing != TerrainTextureTiming::NativeOpenGL) {
        return {};
    }
    const bool native = timing != TerrainTextureTiming::RetainedLru;
    const auto lookup = lookupKey(key);
    const auto hi = handleIndex_.find(lookup);
    const auto ei = entryIndex_.find(lookup);
    auto handle = hi == handleIndex_.end() ? handles_.end() : hi->second;
    const auto found = ei == entryIndex_.end() ? entries_.end() : ei->second;
    const bool valid =
        found != entries_.end() &&
        (!native ||
         (handle != handles_.end() &&
          (timing == TerrainTextureTiming::NativeOpenGL ||
           (found->key.pixels == pixels &&
            (found->wrap == wrap_ || (found->wrap + 1 == wrap_ && found->offset >= cursor_))))));
    if (valid) {
        auto result = found->tile;
        entries_.splice(entries_.begin(), entries_, found);
        ++hits_;
        if (native) {
            handle->requested = key;
            handles_.splice(handles_.begin(), handles_, handle);
        }
        return result;
    }
    auto result = build(key);
    if (!result) {
        return {};
    }
    const auto& tile = result->tile;
    if (tile.texture->pixels.size() + (tile.lightMap ? tile.lightMap->samples.size() * 2 : 0) +
            (result->samples ? result->samples->size() * sizeof(UInt16) : 0) >
        maximumBytes_) {
        return {};
    }
    std::size_t offset{};
    if (native) {
        // Software callback resize forces reallocation. OpenGL keeps a valid
        // download at its old size until eviction; the next callback uses the
        // most recently requested size. Both retain immutable published tiles.
        if (found != entries_.end()) {
            erase(found);
        }
        if (handle == handles_.end()) {
            handles_.push_front({key});
            handle = handles_.begin();
            handleIndex_.emplace(lookup, handle);
        } else {
            handle->requested = key;
            handles_.splice(handles_.begin(), handles_, handle);
        }
        if (timing == TerrainTextureTiming::NativeSoftware) {
            const auto ringBytes = std::size_t(key.pixels) *
                                   key.pixels; // tight native cache rows; owned Bitmap may pad rows
            cursor_ = (cursor_ + 3) & ~std::size_t{3};
            if (ringBytes > maximumBytes_ - std::min(cursor_, maximumBytes_)) {
                cursor_ = 0;
                ++wrap_;
            }
            offset = cursor_;
            cursor_ += ringBytes;
            for (auto at = entries_.begin(); at != entries_.end();) {
                if (at->key.timing == timing && at->wrap != wrap_ &&
                    !(at->wrap + 1 == wrap_ && at->offset >= cursor_)) {
                    at = erase(at);
                } else {
                    ++at;
                }
            }
        }
        while (handles_.size() > maximumEntries_) {
            const auto old = lookupKey(handles_.back().requested);
            if (const auto at = entryIndex_.find(old); at != entryIndex_.end()) {
                erase(at->second);
            }
            handleIndex_.erase(old);
            handles_.pop_back();
        }
    }
    while (!entries_.empty() && (entries_.size() >= maximumEntries_ ||
                                 additionalBytes(*result, block) > maximumBytes_ - bytes_)) {
        erase(std::prev(entries_.end()));
    }
    const auto bytes = additionalBytes(*result, block);
    entries_.push_front({key, tile, result->samples, offset, wrap_});
    entryIndex_.emplace(lookup, entries_.begin());
    retain(entries_.front());
    bytes_ += bytes;
    ++builds_;
    return tile;
}

std::optional<TerrainTextureCache::BuiltTile> TerrainTextureCache::build(const Key& key) const {
    if (key.block >= resources_.blocks.size() || !resources_.blocks[key.block] ||
        !std::has_single_bit(key.cells) || key.cells > 128 || !std::has_single_bit(key.pixels) ||
        key.pixels > 256 || key.cells > key.pixels || key.x % key.cells || key.y % key.cells ||
        (key.density != TerrainLightMapDensity::Expanded &&
         key.density != TerrainLightMapDensity::Native &&
         key.density != TerrainLightMapDensity::NativeOpenGL) ||
        (key.dynamicLighting && (!key.baked || key.density == TerrainLightMapDensity::Expanded))) {
        return {};
    }
    const auto& block = *resources_.blocks[key.block];
    const auto n = block.width;
    if (!n || n > 1024 || key.cells > n || key.x > n - key.cells || key.y > n - key.cells ||
        block.materials.size() != UInt64(n) * n || block.hiresMaps.size() > 50000) {
        return {};
    }
    const auto first = block.materials[std::size_t(key.y) * n + key.x].index;
    if (!compatible(first, first)) {
        return {};
    }
    const auto unit = key.pixels / key.cells,
               textureWidth = UInt32(resources_.materials.textures[first]->width);
    if (unit > textureWidth) {
        return {};
    }
    const auto mip = Int32(std::countr_zero(textureWidth / unit));
    auto bitmap = gfx::Bitmap::create(Int32(key.pixels), Int32(key.pixels));
    if (!bitmap) {
        return {};
    }
    bitmap->paletteIndex = resources_.materials.textures[first]->paletteIndex;
    for (UInt32 y = 0; y < key.cells; ++y) {
        for (UInt32 x = 0; x < key.cells; ++x) {
            const auto material =
                block.materials[std::size_t(key.y + key.cells - 1 - y) * n + key.x + x];
            if ((!key.nativeEmptyLevels && material.hole()) || !compatible(material.index, first)) {
                return {};
            }
            const auto& texture = *resources_.materials.textures[material.index];
            const auto src = texture.level(mip);
            if (mip >= texture.detailLevels || src.empty()) {
                return {};
            }
            const auto stride = texture.detailLevels == 1 ? UInt32(texture.stride()) : unit;
            for (UInt32 dy = 0; dy < unit; ++dy) {
                auto dst = bitmap->row(Int32(y * unit + dy));
                for (UInt32 dx = 0; dx < unit; ++dx) {
                    const auto p = source(dx, dy, unit, material.flags);
                    dst[x * unit + dx] = src[std::size_t(p.y) * stride + std::size_t(p.x)];
                }
            }
        }
    }
    std::shared_ptr<const gfx::LightMap> lightMap;
    Samples decoded;
    Point2F lightMapOrigin{}, lightMapScale{1, 1};
    const bool openGL = key.density == TerrainLightMapDensity::NativeOpenGL;
    if (key.baked) {
        std::optional<std::span<const UInt16>> low;
        if (key.hires && !block.hiresDelta.empty()) {
            if (const auto found = decodedSamples_.find(key.block);
                found != decodedSamples_.end()) {
                decoded = found->second.samples;
            } else {
                auto samples = block.lightSamples(true);
                if (!samples) {
                    return {};
                }
                decoded = std::make_shared<const std::vector<UInt16>>(std::move(*samples));
            }
            low = std::span<const UInt16>(*decoded);
        } else {
            low = block.baseLightSamples();
        }
        if (!low) {
            return {};
        }
        if (!low->empty()) {
            std::vector<UInt32> indices;
            if (key.hires && !openGL) {
                auto selected = block.hiresMapIndices(key.x, key.y, key.cells);
                if (!selected) {
                    return {};
                }
                indices = std::move(*selected);
            }
            const auto scale = 1U << block.lightScale, lowSide = n * scale + 1;
            auto rate = std::min(std::max(16U, scale), 128U / key.cells);
            if (key.density != TerrainLightMapDensity::Expanded) {
                // buildLightMap asserts this invariant. Nonzero lightScale is
                // decoded by the asset layer, but was never supported here.
                if (block.lightScale != 0 || (openGL && (!std::has_single_bit(n) || n > 256))) {
                    return {};
                }
                const auto level = UInt32(std::countr_zero(key.cells));
                const auto falloff = key.dynamicLighting ? (indices.empty() ? 0U : 2U) : 1U;
                auto increment = indices.empty() && !key.dynamicLighting
                                     ? 0U
                                     : 4U - std::min(4U, level > falloff ? level - falloff : 0U);
                // Native level-zero squares register their authored texture
                // directly and do not pass a composite texture-size cap.
                if (level) {
                    increment = std::min(increment, UInt32(std::countr_zero(key.pixels)) - level);
                }
                rate = 1U << increment;
            }
            if (openGL && !key.dynamicLighting) {
                // Native 257x257 blocks use four overlapping 256x256 maps,
                // starting at (0,0), (1,0), (0,1), (1,1). Other blocks pad
                // to a power of two; the GL upload has a minimum side of 8.
                const UInt32 ox = n == 256 && key.x + key.cells > 255 ? 1U : 0U,
                             oy = n == 256 && key.y + key.cells > 255 ? 1U : 0U;
                const auto side = std::max(8U, std::min(256U, std::bit_ceil(n + 1)));
                const auto same = sharedMaps_.find(sharedMapKey(key));
                if (same != sharedMaps_.end()) {
                    lightMap = same->second;
                } else {
                    auto map = std::make_shared<gfx::LightMap>();
                    map->size = {Int32(side), Int32(side)};
                    map->samples.resize(std::size_t(side) * side);
                    std::ranges::for_each(std::views::iota(0U, side), [&](UInt32 y) {
                        std::ranges::for_each(std::views::iota(0U, side), [&](UInt32 x) {
                            // The original padding was uninitialized. Repeat
                            // the authored border for deterministic filtering.
                            map->samples[std::size_t(y) * side + x] =
                                (*low)[std::size_t(std::min(y + oy, n)) * lowSide +
                                       std::min(x + ox, n)];
                        });
                    });
                    lightMap = std::move(map);
                }
                lightMapOrigin = {(Flt32(key.x - ox) + .5f) / side,
                                  (Flt32(key.y + key.cells - oy) + .5f) / side};
                lightMapScale = {Flt32(key.cells) / side, -Flt32(key.cells) / side};
            } else {
                const auto side = key.cells * rate + 1;
                auto map = std::make_shared<gfx::LightMap>();
                map->size = {Int32(side), Int32(side)};
                map->samples.resize(std::size_t(side) * side);
                for (UInt32 y = 0; y < side; ++y) {
                    for (UInt32 x = 0; x < side; ++x) {
                        map->samples[std::size_t(y) * side + x] =
                            sample(*low,
                                   lowSide,
                                   (key.x + Flt64(x) / rate) * scale,
                                   (key.y + key.cells - Flt64(y) / rate) * scale);
                    }
                }
                if (key.hires && !openGL) {
                    // Native expansion visits rows from +Y to -Y and cells from
                    // -X to +X. Right/lower maps own shared edges and corners.
                    std::ranges::sort(indices, [&](UInt32 a, UInt32 b) {
                        const auto& aa = block.hiresMaps[a];
                        const auto& bb = block.hiresMaps[b];
                        return aa.y != bb.y ? aa.y > bb.y : aa.x < bb.x;
                    });
                    for (const auto i : indices) {
                        const auto& h = block.hiresMaps[i];
                        // fillLightMap dumps the low grid when a multi-cell patch
                        // has no expansion; it does not fetch any high-res cells.
                        if (key.cells > 1 && rate == 1) {
                            break;
                        }
                        UInt32 res = std::min(5U, UInt32(std::countr_zero(rate)) + 1);
                        while (res < 5 && !(h.resolutions & (1U << res))) {
                            ++res;
                        }
                        // Uniform maps are independent of resolution. Native
                        // nonuniform maps require a slot at least as fine as output.
                        if (h.resolutions == 1) {
                            res = 0;
                        } else if (!(h.resolutions & (1U << res))) {
                            return {};
                        }
                        auto colors = block.hiresSamples(i, res);
                        if (!colors) {
                            return {};
                        }
                        const auto hside = res ? (1U << (res - 1)) + 1 : 1;
                        const auto ox = (UInt32(h.x) - key.x) * rate,
                                   oy = (key.y + key.cells - UInt32(h.y) - 1) * rate;
                        for (UInt32 y = 0; y <= rate; ++y) {
                            for (UInt32 x = 0; x <= rate; ++x) {
                                map->samples[std::size_t(oy + y) * side + ox + x] =
                                    sample(*colors,
                                           hside,
                                           Flt64(x) * (hside - 1) / rate,
                                           Flt64(rate - y) * (hside - 1) / rate);
                            }
                        }
                    }
                }
                lightMap = std::move(map);
                if (openGL) {
                    // Equivalent texel centers without the original upload's POT
                    // padding; dynamic merging still receives the native grid.
                    lightMapOrigin = {.5f / side, .5f / side};
                    lightMapScale = {Flt32(side - 1) / side, Flt32(side - 1) / side};
                }
            }
        }
    }
    ResourceManager owner;
    if (!owner.add("terrain-tile.bmp", std::move(bitmap))) {
        return {};
    }
    return BuiltTile{{owner.load<gfx::Bitmap>("terrain-tile.bmp"),
                      std::move(lightMap),
                      lightMapOrigin,
                      lightMapScale},
                     std::move(decoded)};
}
} // namespace opent1::world
