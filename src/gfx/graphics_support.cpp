#include <opent1/gfx/graphics_support.hpp>
#include <opent1/core/mem_stream.hpp>
#include <algorithm>
#include <cmath>

namespace opent1::gfx {
bool clipScaledBlit(
    RectI& source, RectI& destination, RectI clip, Point2I& adjustment, UInt8 flip) noexcept {
    if (!source.valid() || !destination.valid() || !clip.valid() || flip > 3 ||
        source.width() > INT32_MAX || source.height() > INT32_MAX ||
        destination.width() > INT32_MAX || destination.height() > INT32_MAX) {
        return false;
    }
    auto src = source, dst = destination;
    Point2I phase{};
    const auto axis = [](Int32& first,
                         Int32& last,
                         Int32& low,
                         Int32& high,
                         Int32 clipLow,
                         Int32 clipHigh,
                         bool reverse,
                         Int32& adj) {
        const Int64 srcSize = Int64(last) - first + 1, dstSize = Int64(high) - low + 1;
        Int64 a = first, b = last, p = 0;
        if (high > clipHigh) {
            const auto delta = Int64(clipHigh) - high;
            p = reverse ? delta : 0;
            b += delta * srcSize / dstSize;
            high = clipHigh;
        }
        if (low < clipLow) {
            p = reverse ? 0 : Int64(clipLow) - low;
            a += p * srcSize / dstSize;
            low = clipLow;
        }
        if (a > b || low > high || a < INT32_MIN || a > INT32_MAX || b < INT32_MIN ||
            b > INT32_MAX || p < INT32_MIN || p > INT32_MAX) {
            return false;
        }
        first = Int32(a);
        last = Int32(b);
        adj = Int32(p);
        return true;
    };
    if (!axis(src.left,
              src.right,
              dst.left,
              dst.right,
              clip.left,
              clip.right,
              (flip & 1) != 0,
              phase.x) ||
        !axis(src.top,
              src.bottom,
              dst.top,
              dst.bottom,
              clip.top,
              clip.bottom,
              (flip & 2) != 0,
              phase.y)) {
        return false;
    }
    source = src;
    destination = dst;
    adjustment = phase;
    return true;
}

std::optional<RectI> clipRasters(std::span<RasterSpan> rasters, RectI clip) noexcept {
    std::optional<RectI> bounds;
    for (auto& raster : rasters) {
        const Int64 end = Int64(raster.x) + raster.count;
        raster.dead = !clip.valid() || raster.count < 0 || raster.y < clip.top ||
                      raster.y > clip.bottom || raster.x > clip.right || end < clip.left;
        raster.leftClipped = raster.rightClipped = false;
        raster.left = raster.x;
        raster.right = end >= INT32_MIN && end <= INT32_MAX ? Int32(end) : raster.x;
        if (raster.dead) {
            continue;
        }
        raster.left = std::max(raster.x, clip.left);
        raster.right = Int32(std::min(end, Int64(clip.right)));
        raster.leftClipped = raster.left != raster.x;
        raster.rightClipped = Int64(raster.right) != end;
        if (!bounds) {
            bounds = RectI{raster.left, raster.y, raster.right, raster.y};
        } else {
            bounds->left = std::min(bounds->left, raster.left);
            bounds->right = std::max(bounds->right, raster.right);
            bounds->top = std::min(bounds->top, raster.y);
            bounds->bottom = std::max(bounds->bottom, raster.y);
        }
    }
    return bounds;
}

bool PaletteMap::Range::allows(UInt8 source, UInt8 candidate) const noexcept {
    return (inclusion[source][candidate / 32] & (0x80000000U >> (candidate % 32))) != 0;
}

void PaletteMap::Range::allow(UInt8 source, UInt8 candidate, bool enabled) noexcept {
    auto& word = inclusion[source][candidate / 32];
    const auto bit = 0x80000000U >> (candidate % 32);
    if (enabled) {
        word |= bit;
    } else {
        word &= ~bit;
    }
}

PaletteMap::PaletteMap() {
    for (auto* range : {&shade, &haze}) {
        for (auto& bits : range->inclusion) {
            bits.fill(0xffffffffU);
        }
    }
    shade.levels = 32;
    haze.levels = 16;
}

bool PaletteMap::valid() const noexcept {
    const auto rangeValid = [](const Range& r) {
        return std::isfinite(r.maximum) && r.maximum >= 0 && r.maximum <= 1 && r.levels > 0 &&
               r.levels <= 256;
    };
    return rangeValid(shade) && rangeValid(haze) && std::isfinite(gamma) && gamma >= 0.05f &&
           gamma <= 20.f;
}

bool PaletteMap::legacyRepresentable() const noexcept {
    for (const auto* range : {&shade, &haze}) {
        for (const auto& bits : range->inclusion) {
            if (!std::all_of(bits.begin() + 2, bits.end(), [](auto word) {
                    return word == 0xffffffffU;
                })) {
                return false;
            }
        }
    }
    return hazeColor.flags == 0;
}

namespace {
constexpr UInt32 mapTag = makeFourCC('P', 'M', 'A', 'P'), mapPayload = 4124,
                 mapBytes = mapPayload + 8;
}

std::unique_ptr<PaletteMap> PaletteMap::load(StreamIO& input, UInt32 size) {
    if (size != mapBytes) {
        return {};
    }
    std::array<std::byte, mapBytes> bytes{};
    if (!input.readBytes(bytes)) {
        return {};
    }
    MemRWStream stream{std::span<const std::byte>(bytes)};
    UInt32 tag{}, length{}, version{};
    if (!stream.read(tag) || tag != mapTag || !stream.read(length) || length != mapPayload ||
        !stream.read(version) || version != 1) {
        return {};
    }
    auto map = std::make_unique<PaletteMap>();
    for (std::size_t i = 0; i < 256; ++i) {
        for (auto* range : {&map->haze, &map->shade}) {
            if (!stream.read(range->inclusion[i][0]) || !stream.read(range->inclusion[i][1])) {
                return {};
            }
        }
    }
    UInt8 luv{};
    if (!stream.read(map->shade.maximum) || !stream.read(map->shade.levels) ||
        !stream.read(map->haze.maximum) || !stream.read(map->haze.levels) ||
        !stream.read(map->hazeColor.red) || !stream.read(map->hazeColor.green) ||
        !stream.read(map->hazeColor.blue) || !stream.read(map->gamma) || !stream.read(luv) ||
        luv > 1 || !map->valid()) {
        return {};
    }
    map->useLuv = luv != 0;
    return map;
}

bool PaletteMap::write(StreamIO& output) const {
    if (!valid() || !legacyRepresentable()) {
        return false;
    }
    std::array<std::byte, mapBytes> bytes{};
    MemRWStream stream{std::span<std::byte>(bytes)};
    if (!stream.write(mapTag) || !stream.write(mapPayload) || !stream.write(UInt32{1})) {
        return false;
    }
    for (std::size_t i = 0; i < 256; ++i) {
        for (const auto* range : {&haze, &shade}) {
            if (!stream.write(range->inclusion[i][0]) || !stream.write(range->inclusion[i][1])) {
                return false;
            }
        }
    }
    return stream.write(shade.maximum) && stream.write(shade.levels) &&
           stream.write(haze.maximum) && stream.write(haze.levels) && stream.write(hazeColor.red) &&
           stream.write(hazeColor.green) && stream.write(hazeColor.blue) && stream.write(gamma) &&
           stream.write(useLuv) && output.writeBytes(bytes);
}

bool registerPaletteMapLoader(ResourceManager& resources) {
    return resources.registerLoader<PaletteMap>(".pmp", [](StreamIO& s, UInt32 size) {
        return PaletteMap::load(s, size);
    });
}

bool GraphicsTimer::push(Clock::time_point now) noexcept {
    if (depth_ == Capacity) {
        return false;
    }
    starts_[depth_++] = now;
    return true;
}

std::optional<std::chrono::nanoseconds> GraphicsTimer::pop(Clock::time_point now) noexcept {
    if (!depth_ || now < starts_[depth_ - 1]) {
        return {};
    }
    return std::chrono::duration_cast<std::chrono::nanoseconds>(now - starts_[--depth_]);
}
} // namespace opent1::gfx
