#include <opent1/gfx/texture_cache.hpp>
#include <opent1/math/random.hpp>
#include <algorithm>
#include <bit>
#include <ranges>

namespace opent1::gfx {
std::span<const Int32> defaultTextureNoise() {
    static const auto noise = [] {
        std::array<Int32, TextureNoiseSize> result{};
        Random random;
        for (auto& value : result) {
            const auto sample = random.getFloat();
            value = sample <= 1.f / 3.f ? 0 : sample <= 2.f / 3.f ? 0x800 : -0x800;
        }
        return result;
    }();
    return noise;
}

namespace {
struct Layout {
    Int32 mip{}, width{}, height{}, sourceWidth{}, sourceHeight{}, cell{}, shift{};
    Int64 offsetX{}, offsetY{};
    Int32 mapX{}, mapY{};
    bool aligned{};
};

// Explicit arithmetic shift, including negative offsets, without signed-shift assumptions.
Int64 floorShift(Int64 value, Int32 shift) {
    const auto divisor = Int64{1} << shift;
    return value >= 0 ? value / divisor : -((-value + divisor - 1) / divisor);
}

bool noiseValid(std::span<const Int32> noise) {
    return (noise.empty() || noise.size() == TextureNoiseSize) &&
           std::ranges::all_of(noise, [](Int32 value) {
               return value >= -65535 && value <= 65535;
           });
}

bool callbackValid(const LightMap& map, const TextureSettings& settings) {
    return map.valid() && settings.size.x >= 4 && settings.size.x <= Bitmap::MaxDimension &&
           settings.size.x == settings.size.y &&
           UInt64(settings.size.x) * UInt64(settings.size.y) <= Bitmap::MaxFileBytes &&
           std::has_single_bit(UInt32(settings.size.x)) && settings.mipLevel == 0 &&
           settings.lightScale >= 0 && settings.lightScale <= 14 && settings.offset == Point2I{} &&
           map.offset == Point2I{} &&
           Int64(map.size.x - 1) * (Int64{1} << settings.lightScale) == settings.size.x &&
           Int64(map.size.y - 1) * (Int64{1} << settings.lightScale) == settings.size.y;
}

std::optional<Layout>
layout(const Bitmap& bitmap, const LightMap& map, const TextureSettings& settings) {
    if (map.format != LightMap::Format::Rgb444) {
        return {};
    }
    if (!bitmap.valid() || bitmap.bitDepth != 8 || !map.valid() ||
        !std::has_single_bit(UInt32(bitmap.width)) || !std::has_single_bit(UInt32(bitmap.height)) ||
        settings.size.x <= 0 || settings.size.y <= 0 || settings.size.x > Bitmap::MaxDimension ||
        settings.size.y > Bitmap::MaxDimension || settings.lightScale < 0 ||
        settings.lightScale > 14 || settings.mipLevel < 0) {
        return {};
    }
    Layout result;
    result.mip = std::min({settings.mipLevel, bitmap.detailLevels - 1, settings.lightScale});
    result.width = settings.size.x >> result.mip;
    result.height = settings.size.y >> result.mip;
    if (result.width == 0 || result.height == 0 ||
        UInt64((result.width + 3) & ~3) * UInt64(result.height) > Bitmap::MaxFileBytes) {
        return {};
    }
    result.sourceWidth = std::max(1, bitmap.width >> result.mip);
    result.sourceHeight = std::max(1, bitmap.height >> result.mip);
    result.shift = settings.lightScale - result.mip;
    result.cell = Int32{1} << result.shift;
    result.offsetX = floorShift(settings.offset.x, result.mip);
    result.offsetY = floorShift(settings.offset.y, result.mip);
    const Int32 rounding = result.mip ? Int32{1} << (result.mip - 1) : 0;
    result.mapX = (map.offset.x + rounding) >> result.mip;
    result.mapY = (map.offset.y + rounding) >> result.mip;
    // Every destination pixel must have a light cell. Legacy partial coverage
    // otherwise exposed uninitialized cache bytes.
    if (Int64(map.size.x - 1) * result.cell - result.mapX < result.width ||
        Int64(map.size.y - 1) * result.cell - result.mapY < result.height) {
        return {};
    }
    // The original fast assembly traverses a square grid with tightly packed
    // source rows. Rectangular and padded small textures use the wrapping path.
    result.aligned = bitmap.width == bitmap.height && bitmap.width >= 4 &&
                     settings.size == Point2I(bitmap.width, bitmap.height) &&
                     settings.offset == Point2I(0, 0) && map.offset == Point2I(0, 0) &&
                     Int64(map.size.x - 1) * (Int64{1} << settings.lightScale) == bitmap.width &&
                     Int64(map.size.y - 1) * (Int64{1} << settings.lightScale) == bitmap.height;
    return result;
}
}

bool LightMap::valid() const noexcept {
    return format <= Format::Rgb565 && size.x >= 2 && size.y >= 2 && size.x <= 256 &&
           size.y <= 256 && offset.x >= 0 && offset.y >= 0 && offset.x <= 255 && offset.y <= 255 &&
           samples.size() == std::size_t(size.x) * std::size_t(size.y);
}

std::unique_ptr<Bitmap> lightTexture(const Bitmap& texture,
                                     const LightMap& map,
                                     const Palette& palette,
                                     const TextureSettings& settings,
                                     std::span<const Int32> noise) {
    const auto info = layout(texture, map, settings);
    if (!info || !palette.valid() || palette.shadeShift > 8 || !noiseValid(noise)) {
        return {};
    }
    const auto& sub = palette.find(texture.paletteIndex);
    const std::size_t levels = std::size_t{1} << palette.shadeShift;
    if (sub.type != PaletteType::ShadeHaze || sub.remap.size() < levels * 256) {
        return {};
    }
    const auto& l = *info;
    auto output = Bitmap::create(l.width, l.height);
    if (!output) {
        return {};
    }
    output->paletteIndex = texture.paletteIndex; // cached texels are already lit
    const auto source = texture.level(l.mip);
    const Int32 sourceStride = l.mip == 0 ? texture.stride() : l.sourceWidth;
    std::size_t noiseIndex = settings.noiseKey & UInt32(TextureNoiseSize);
    const auto advanceNoise = [&]() {
        if (++noiseIndex >= TextureNoiseSize) {
            noiseIndex = 0;
        }
    };
    advanceNoise(); // original setNoiseStart discards one sample
    const auto sampleNoise = [&]() {
        advanceNoise();
        return noise.empty() ? 0 : noise[noiseIndex];
    };
    const auto corner = [&](Int32 x, Int32 y) {
        const auto alpha =
            map.samples[std::size_t(y) * std::size_t(map.size.x) + std::size_t(x)] >> 12;
        return Int32(alpha) * (l.aligned ? 0x1111 : 0x1000);
    };
    const auto step = [&](Int32 difference) {
        const Int32 value = Int32(floorShift(difference, l.shift));
        // p_txrc.asm increments every negative arithmetic-shift result, even
        // when the dividend was exactly divisible. Wrapping C++ does not.
        return value + (l.aligned && value < 0 ? 1 : 0);
    };
    for (Int32 cy = 0; cy < map.size.y - 1; ++cy) {
        const Int32 dy = cy * l.cell - l.mapY;
        const Int32 startY = std::max(0, -dy), endY = std::min(l.cell, l.height - dy);
        if (endY <= startY) {
            continue;
        }
        for (Int32 cx = 0; cx < map.size.x - 1; ++cx) {
            const Int32 dx = cx * l.cell - l.mapX;
            const Int32 startX = std::max(0, -dx), endX = std::min(l.cell, l.width - dx);
            if (endX <= startX) {
                continue;
            }
            Int32 left = corner(cx, cy), right = corner(cx + 1, cy);
            const Int32 leftStep = step(corner(cx, cy + 1) - left),
                        rightStep = step(corner(cx + 1, cy + 1) - right);
            left += leftStep * startY;
            right += rightStep * startY;
            for (Int32 iy = startY; iy < endY; ++iy) {
                const Int32 lightStep = step(right - left);
                Int32 light = left + lightStep * startX;
                const auto sy = UInt64(Int64(dy) + iy + l.offsetY) & UInt64(l.sourceHeight - 1);
                const auto srcRow = source.subspan(std::size_t(sy) * std::size_t(sourceStride),
                                                   std::size_t(l.sourceWidth));
                auto dstRow = output->row(dy + iy);
                for (Int32 ix = startX; ix < endX; ++ix) {
                    const Int32 lit =
                        std::clamp(light + sampleNoise(), 0, l.aligned ? 0xffff : 0xf000);
                    const auto level = UInt32(lit) >> (16 - palette.shadeShift);
                    const auto sx = UInt64(Int64(dx) + ix + l.offsetX) & UInt64(l.sourceWidth - 1);
                    const auto texel = std::to_integer<UInt8>(srcRow[std::size_t(sx)]);
                    dstRow[std::size_t(dx + ix)] = sub.remap[std::size_t(level) * 256 + texel];
                    light += lightStep;
                }
                left += leftStep;
                right += rightStep;
            }
        }
    }
    return output;
}

struct TextureCache::Entry {
    TextureHandle handle;
    Bitmap texture;
    LightMap map;
    TextureSettings settings;
    Generator generator;
    std::shared_ptr<const Bitmap> prepared;

    [[nodiscard]] std::size_t bytes() const {
        return texture.pixels.size() + map.samples.size() * sizeof(UInt16);
    }
};

TextureCache::TextureCache() : TextureCache(Limits{}) {}

TextureCache::TextureCache(Limits limits)
    : limits_{std::min(limits.entries, std::size_t{3000}),
              std::min(limits.sourceBytes, std::size_t(Bitmap::MaxFileBytes)),
              std::min(limits.preparedBytes, std::size_t(Bitmap::MaxFileBytes))} {}

TextureCache::~TextureCache() = default;

std::size_t TextureCache::size() const noexcept {
    return entries_.size();
}

bool TextureCache::setPalette(const Palette& palette) {
    if (!palette.valid()) {
        return false;
    }
    auto snapshot = std::make_shared<const Palette>(palette);
    palette_ = std::move(snapshot);
    clearPrepared();
    return true;
}

bool TextureCache::setNoise(std::span<const Int32> noise) {
    if (!noiseValid(noise)) {
        return false;
    }
    std::vector<Int32> snapshot(noise.begin(), noise.end());
    noise_ = std::move(snapshot);
    clearPrepared();
    return true;
}

void TextureCache::erase(std::list<Entry>::iterator it) noexcept {
    sourceBytes_ -= it->bytes();
    if (it->prepared) {
        preparedBytes_ -= it->prepared->pixels.size();
    }
    entries_.erase(it);
}

bool TextureCache::registerTexture(TextureHandle handle,
                                   const Bitmap& texture,
                                   const LightMap& map,
                                   const TextureSettings& settings) {
    if (!layout(texture, map, settings) || limits_.entries == 0) {
        return false;
    }
    const auto bytes = texture.pixels.size() + map.samples.size() * sizeof(UInt16);
    if (bytes > limits_.sourceBytes) {
        return false;
    }
    // Embedded source palettes are not used by lighting. Retain only the indexed
    // image metadata/pixels, with the cache's explicit palette as the authority.
    Bitmap snapshot;
    snapshot.width = texture.width;
    snapshot.height = texture.height;
    snapshot.bitDepth = texture.bitDepth;
    snapshot.detailLevels = texture.detailLevels;
    snapshot.attributes = texture.attributes;
    snapshot.paletteIndex = texture.paletteIndex;
    snapshot.pixels = texture.pixels;
    return insert({handle, std::move(snapshot), map, settings, {}, {}});
}

bool TextureCache::registerTexture(TextureHandle handle,
                                   Generator generator,
                                   const LightMap& map,
                                   const TextureSettings& settings) {
    if (!generator || !callbackValid(map, settings) || limits_.entries == 0 ||
        map.samples.size() * sizeof(UInt16) > limits_.sourceBytes) {
        return false;
    }
    return insert({handle, {}, map, settings, std::move(generator), {}});
}

bool TextureCache::insert(Entry candidate) {
    const auto existing = std::ranges::find(entries_, candidate.handle, &Entry::handle);
    const auto bytes = candidate.bytes();
    entries_.push_front(std::move(candidate)); // all allocations precede eviction
    sourceBytes_ += bytes;
    if (existing != entries_.end()) {
        erase(existing);
    }
    while (entries_.size() > limits_.entries || sourceBytes_ > limits_.sourceBytes) {
        erase(std::prev(entries_.end()));
    }
    ++revision_;
    return true;
}

std::shared_ptr<const Bitmap> TextureCache::prepare(TextureHandle handle) {
    if (std::ranges::find(inFlight_, handle) != inFlight_.end()) {
        return {};
    }
    auto it = std::ranges::find(entries_, handle, &Entry::handle);
    if (it == entries_.end() || !palette_) {
        return {};
    }
    if (!it->prepared) {
        std::shared_ptr<const Bitmap> prepared;
        if (it->generator) {
            if (std::size_t(it->settings.size.x) * std::size_t(it->settings.size.y) >
                limits_.preparedBytes) {
                return {};
            }
            // Never keep a list iterator or borrowed input across user code.
            // The callback may remove/replace this entry or change the palette.
            auto generate = it->generator;
            const auto map = it->map;
            const auto settings = it->settings;
            const auto palette = palette_;
            const auto noise = noise_;
            const auto revision = revision_;
            inFlight_.push_back(handle);

            struct Guard {
                std::vector<TextureHandle>& active;

                ~Guard() {
                    active.pop_back();
                }
            } guard{inFlight_};

            const auto generated = generate(handle, settings.size);
            if (revision_ != revision || !generated || generated->width != settings.size.x ||
                generated->height != settings.size.y || generated->detailLevels != 1) {
                return {};
            }
            prepared = lightTexture(*generated, map, *palette, settings, noise);
            it = std::ranges::find(entries_, handle, &Entry::handle);
            if (it == entries_.end()) {
                return {};
            }
        } else {
            const auto info = layout(it->texture, it->map, it->settings);
            if (!info || std::size_t((info->width + 3) & ~3) * std::size_t(info->height) >
                             limits_.preparedBytes) {
                return {};
            }
            prepared = lightTexture(it->texture, it->map, *palette_, it->settings, noise_);
        }
        if (!prepared) {
            return {};
        }
        const auto bytes = prepared->pixels.size();
        for (auto old = entries_.rbegin();
             preparedBytes_ + bytes > limits_.preparedBytes && old != entries_.rend();
             ++old) {
            if (old->prepared) {
                preparedBytes_ -= old->prepared->pixels.size();
                old->prepared.reset();
            }
        }
        it->prepared = std::move(prepared);
        preparedBytes_ += bytes;
    }
    entries_.splice(entries_.begin(), entries_, it);
    return entries_.front().prepared;
}

bool TextureCache::remove(TextureHandle handle) {
    const auto it = std::ranges::find(entries_, handle, &Entry::handle);
    if (it == entries_.end()) {
        return false;
    }
    erase(it);
    ++revision_;
    return true;
}

void TextureCache::clearPrepared() noexcept {
    std::ranges::for_each(entries_, [](Entry& entry) {
        entry.prepared.reset();
    });
    preparedBytes_ = 0;
    ++revision_;
}

void TextureCache::clear() noexcept {
    entries_.clear();
    sourceBytes_ = preparedBytes_ = 0;
    ++revision_;
}
} // namespace opent1::gfx
