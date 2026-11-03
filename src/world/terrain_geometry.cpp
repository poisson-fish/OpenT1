#include <opent1/world/terrain_geometry.hpp>
#include <bit>
#include <cmath>

namespace opent1::world {
std::optional<TerrainGeometryTable> terrainGeometryDetails(Int32 scale,
                                                           const TerrainGeometryView& view) {
    if (scale < 0 || scale > 16 || !std::isfinite(view.focalLength) || view.focalLength <= 0 ||
        view.focalLength > 65536 || !std::isfinite(view.screenSize) || view.screenSize <= 0 ||
        view.screenSize > 65536 || !std::isfinite(view.pixelScale) || view.pixelScale <= 0 ||
        view.pixelScale > 16 || !std::isfinite(view.nearDistance) || view.nearDistance <= 0 ||
        !std::isfinite(view.farDistance) || view.farDistance <= view.nearDistance) {
        return {};
    }
    TerrainGeometryTable result;
    const auto pixels = view.screenSize / view.pixelScale;
    if (!std::isfinite(pixels) || pixels <= 0) {
        return {};
    }
    for (Int32 i = 7; i >= 0; --i) {
        auto& d = result[std::size_t(i)];
        const auto size = Flt32(1U << (UInt32(i) + UInt32(scale)));
        d.detailDistance =
            view.perspective
                ? (size * view.focalLength) / pixels
                : (size * view.focalLength > pixels ? view.farDistance : view.nearDistance);
        if (i < 6) {
            d.maxDistance = result[std::size_t(i + 1)].detailDistance - size;
            d.minDistance = (d.maxDistance + d.detailDistance) * .5f;
        } else {
            d.minDistance = d.detailDistance + size * 2.f;
            d.maxDistance = d.minDistance * 2.f;
        }
        const auto delta = d.maxDistance - d.minDistance;
        d.distanceScale = delta > 0 ? Flt32(1.0 / delta) : 1.f;
        if (!std::isfinite(d.detailDistance) || !std::isfinite(d.minDistance) ||
            !std::isfinite(d.maxDistance) || !std::isfinite(d.distanceScale)) {
            return {};
        }
    }
    return result;
}

std::optional<std::array<Flt32, 4>> terrainMorphInputs(const TerrainGridBlock& block,
                                                       UInt32 x,
                                                       UInt32 y,
                                                       UInt32 level,
                                                       Flt32 distance,
                                                       const TerrainGeometryDetail& d) {
    const auto n = block.width;
    if (!std::has_single_bit(n) || n > 1024 || level > 7 || (1U << level) > n || x > n || y > n ||
        (x % (1U << level)) || (y % (1U << level)) ||
        block.heights.size() != UInt64(n + 1) * (n + 1) || !std::isfinite(distance) ||
        distance < 0 || !std::isfinite(d.minDistance) || !std::isfinite(d.distanceScale) ||
        d.distanceScale <= 0) {
        return {};
    }
    const auto at = std::size_t(y) * (n + 1) + x;
    const auto h = block.heights[at];
    if (!std::isfinite(h) || std::abs(h) > 1e12f) {
        return {};
    }
    const auto& pins = block.pins[0];
    if (!pins.empty() && pins.size() < (block.heights.size() + 7) / 8) {
        return {};
    }
    if (distance <= d.minDistance ||
        (!pins.empty() && (std::to_integer<UInt32>(pins[at >> 3]) & (1U << (at & 7))))) {
        return std::array{h, h, 0.f, distance};
    }
    const auto step = 1U << level, px = x >> level, py = y >> level;
    Int32 dx{}, dy{};
    switch (((py & 1U) << 1) | (px & 1U)) {
        case 1:
            dx = Int32(step);
            break;
        case 2:
            dy = Int32(step);
            break;
        case 3:
            dx = ((px ^ py) & 2U) ? -Int32(step) : Int32(step);
            dy = Int32(step);
            break;
        default:
            return std::array{h, h, 0.f, distance};
    }
    const auto ax = Int32(x) + dx, ay = Int32(y) + dy, bx = Int32(x) - dx, by = Int32(y) - dy;
    if (ax < 0 || ay < 0 || bx < 0 || by < 0 || ax > Int32(n) || ay > Int32(n) || bx > Int32(n) ||
        by > Int32(n)) {
        return {};
    }
    const auto a = block.heights[std::size_t(ay) * (n + 1) + std::size_t(ax)],
               b = block.heights[std::size_t(by) * (n + 1) + std::size_t(bx)];
    if (!std::isfinite(a) || !std::isfinite(b) || std::abs(a) > 1e12f || std::abs(b) > 1e12f) {
        return {};
    }
    const auto target = (a + b) * .5f, frac = (distance - d.minDistance) * d.distanceScale;
    return std::array{h, target, frac, distance};
}

std::optional<Flt32> morphTerrainHeight(const TerrainGridBlock& block,
                                        UInt32 x,
                                        UInt32 y,
                                        UInt32 level,
                                        Flt32 distance,
                                        const TerrainGeometryDetail& detail) {
    const auto inputs = terrainMorphInputs(block, x, y, level, distance, detail);
    if (!inputs)
        return {};
    const auto& v = *inputs;
    return v[2] >= 1.f ? v[1] : v[0] + (v[1] - v[0]) * v[2];
}
} // namespace opent1::world
