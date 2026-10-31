#include "terrain_scene_lighting.hpp"
#include <bit>
#include <cmath>

namespace opent1::world::terrain_detail {
namespace {
bool spend(std::size_t& work, std::size_t count) {
    if (count > work) {
        return false;
    }
    work -= count;
    return true;
}

struct SquareStepper {
    Flt32 value{}, twiceKX{}, odd{}, twiceK2{};

    Flt32 init(Flt32 x, Flt32 k) {
        twiceKX = Flt32(k * 2.0 * x);
        odd = k * k;
        twiceK2 = odd + odd;
        return value = x * x;
    }

    Flt32 next() {
        value += odd;
        odd += twiceK2;
        return value -= twiceKX;
    }
};

UInt16 add(UInt16 a, UInt16 b) {
    UInt32 result{};
    std::ranges::for_each(std::views::iota(0U, 4U), [&](UInt32 c) {
        const auto shift = c * 4;
        result |= std::min(15U, ((UInt32(a) >> shift) & 15) + ((UInt32(b) >> shift) & 15)) << shift;
    });
    return UInt16(result);
}

std::array<UInt16, 64> mapper(Point3F color) {
    color.x = std::min(color.x, 1.f);
    color.y = std::min(color.y, 1.f);
    color.z = std::min(color.z, 1.f);
    const auto mono = Flt32(.3 * color.x + .6 * color.y + .1 * color.z);
    const auto packed = UInt16((UInt16(mono * 15.99) << 12) | (UInt16(color.x * 15.99) << 8) |
                               (UInt16(color.y * 15.99) << 4) | UInt16(color.z * 15.99));
    std::array<UInt16, 64> result{};
    const auto maxDistance = Flt32(std::sqrt(64 + .001f));
    std::ranges::for_each(std::views::iota(0U, 64U), [&](UInt32 i) {
        std::ranges::for_each(std::views::iota(0U, 4U), [&](UInt32 c) {
            const auto shift = c * 4, n = UInt32(packed >> shift) & 15;
            // Preserve the original float table and half-unit truncation.
            const auto amount =
                UInt16(Flt32(Flt32(n) + .5f) - (std::sqrt(Flt32(i)) / maxDistance) * Flt32(n));
            result[i] |= UInt16(amount << shift);
        });
    });
    return result;
}
}

bool validScene(const TsLighting& scene) {
    return scene.valid() && std::ranges::all_of(scene.lights, [](const auto& l) {
               return l.type != TsLight::Type::Point ||
                      (l.range <= 1e8f && std::abs(l.position.x) <= 1e8f &&
                       std::abs(l.position.y) <= 1e8f && std::abs(l.position.z) <= 1e8f);
           });
}

std::optional<bool>
PointLightMask::under(UInt32 x, UInt32 y, UInt32 cells, std::size_t& work) const {
    if (!cells || !std::has_single_bit(cells) || cells > width || x > width - cells ||
        y > width - cells || x % cells || y % cells) {
        return {};
    }
    if (!squares || !lights || cells > 64) {
        return false;
    }
    // Charge the worst-case byte visits even when the first set bit ends testing.
    if (!spend(work, std::size_t(cells) * std::max(cells / 8, 1U))) {
        return {};
    }
    return squares->testPwr2Sqr(Int32(x), Int32(y), Int32(std::countr_zero(cells)));
}

std::optional<PointLightMask> pointLightMask(
    UInt32 width, Int32 scale, Point3F origin, const TsLighting& scene, std::size_t& work) {
    if (!width || width > 1024 || !std::has_single_bit(width) || scale < 0 || scale > 16 ||
        !validScene(scene) || !std::isfinite(origin.x) || !std::isfinite(origin.y) ||
        !std::isfinite(origin.z)) {
        return {};
    }
    PointLightMask result;
    result.width = width;
    if (width > 256) {
        return result;
    }
    const auto bits = std::max(width, 8U);
    if (!spend(work, std::size_t(bits) * bits / 8)) {
        return {};
    }
    result.squares.emplace(Int32(std::countr_zero(width)));
    const auto extent = Flt32(width) * Flt32(1U << scale);
    const auto end = origin + Point3F{extent, extent, extent};
    if (!std::isfinite(end.x) || !std::isfinite(end.y) || !std::isfinite(end.z)) {
        return {};
    }
    for (const auto& light : scene.lights) {
        if (!spend(work, 1)) {
            return {};
        }
        if (light.type != TsLight::Type::Point) {
            continue;
        }
        const auto lo = light.position - Point3F{light.range, light.range, light.range},
                   hi = light.position + Point3F{light.range, light.range, light.range};
        // Native Box3F includes tangencies and uses a fixed block Z cube.
        if (lo.x > end.x || lo.y > end.y || lo.z > end.z || hi.x < origin.x || hi.y < origin.y ||
            hi.z < origin.z) {
            continue;
        }
        const auto localLow = Point3F{std::max(lo.x, origin.x),
                                      std::max(lo.y, origin.y),
                                      std::max(lo.z, origin.z)} -
                              origin,
                   localHigh = Point3F{std::min(hi.x, end.x),
                                       std::min(hi.y, end.y),
                                       std::min(hi.z, end.z)} -
                               origin;
        // Intersect before converting: all resulting coordinates fit native int.
        const auto add = Int32((1U << scale) - 1);
        const auto x0 = Int32(localLow.x) >> scale, y0 = Int32(localLow.y) >> scale,
                   x1 = (Int32(localHigh.x) + add) >> scale,
                   y1 = (Int32(localHigh.y) + add) >> scale;
        if (!spend(work, std::size_t(y1 - y0 + 1) * (std::size_t(x1 - x0) / 8 + 2))) {
            return {};
        }
        result.squares->set(x0, y0, x1 - x0 + 1, y1 - y0 + 1);
        ++result.lights;
    }
    return result;
}

std::optional<std::vector<PointLightRegion>> pointLightRegions(const TerrainGridBlock& block,
                                                               UInt32 x,
                                                               UInt32 y,
                                                               UInt32 cells,
                                                               Int32 scale,
                                                               Point3F origin,
                                                               const TsLighting& scene,
                                                               std::size_t& work) {
    std::vector<PointLightRegion> result;
    // Original lightBitsetMax=8 and MaxSquareLightLevel=6 pre-scan gates.
    if (block.width > 256 || cells > 64) {
        return result;
    }
    const auto square = Int32(1U << scale);
    const auto n = block.width;
    for (UInt32 i = 0; i < scene.lights.size(); ++i) {
        if (!spend(work, 1)) {
            return {};
        }
        const auto& light = scene.lights[i];
        if (light.type != TsLight::Type::Point) {
            continue;
        }
        const auto p = light.position - origin;
        const auto range = light.range;
        // Very distant block placements can exceed native signed coordinates.
        // Exclude them with a conservative two-cell margin before conversion.
        const auto margin = Flt32(square) * 2, extent = Flt32(n) * Flt32(square) + margin;
        if (p.x + range < -margin || p.y + range < -margin || p.x - range > extent ||
            p.y - range > extent) {
            continue;
        }
        // Native truncation toward zero precedes the signed grid shift. Bounds
        // are now representable after validScene and the distant-block gate.
        const auto low = [&](Flt32 v) {
            return Int32(v - range) >> scale;
        };
        const auto high = [&](Flt32 v) {
            return (Int32(v + range) + square) >> scale;
        };
        const auto x0 = std::max(Int32(x), low(p.x)), y0 = std::max(Int32(y), low(p.y)),
                   x1 = std::min(Int32(x + cells), high(p.x)),
                   y1 = std::min(Int32(y + cells), high(p.y));
        if (x0 >= x1 || y0 >= y1) {
            continue;
        }
        Flt32 lowest = 1e8f, highest = -lowest;
        for (auto yy = y0; yy <= y1; ++yy) {
            if (!spend(work, std::size_t(x1 - x0 + 1))) {
                return {};
            }
            std::ranges::for_each(std::views::iota(x0, x1 + 1), [&](Int32 xx) {
                const auto height = block.heights[std::size_t(yy) * (n + 1) + UInt32(xx)];
                lowest = std::min(lowest, height);
                highest = std::max(highest, height);
            });
        }
        if (p.z - range <= highest && p.z + range >= lowest) {
            result.push_back({i, UInt32(x0), UInt32(y0), UInt32(x1), UInt32(y1)});
        }
    }
    return result;
}

std::optional<std::shared_ptr<const gfx::LightMap>>
mergePointLights(const TerrainGridBlock& block,
                 UInt32 x,
                 UInt32 y,
                 UInt32 cells,
                 Int32 scale,
                 Point3F origin,
                 const TsLighting& scene,
                 std::span<const PointLightRegion> regions,
                 const gfx::LightMap& base,
                 std::size_t& work) {
    if (!base.valid() || base.size.x != base.size.y || base.size.x < 2 ||
        UInt32(base.size.x - 1) % cells || !std::has_single_bit(UInt32(base.size.x - 1) / cells)) {
        return {};
    }
    const auto rate = UInt32(base.size.x - 1) / cells, side = UInt32(base.size.x), n = block.width;
    if (rate > 16 || side > 129) {
        return {};
    }
    const auto bits = UInt32(std::countr_zero(rate)), mask = rate - 1;
    auto result = std::make_shared<gfx::LightMap>(base);
    for (const auto& region : regions) {
        const auto& light = scene.lights[region.source];
        auto loc = light.position - origin;
        const auto columns = region.x1 - region.x0 + 1, stepsX = (columns - 1) * rate,
                   stepsY = (region.y1 - region.y0) * rate;
        if (!spend(work, std::size_t(stepsX + 1) * (stepsY + 1))) {
            return {};
        }
        const auto gridStep = Flt32(1U << scale) / Flt32(rate),
                   maxDistance = light.range * light.range;
        const auto colors = mapper(light.intensity);
        const auto colorScale = 64.f / (maxDistance + .001f);
        std::array<Flt32, 129> rowA{}, rowB{}, increments{};
        auto* current = rowB.data();
        auto* next = rowA.data();
        auto gridY = region.y0;
        const auto heights = [&](Flt32* row, UInt32 yy) {
            std::ranges::copy_n(
                block.heights.begin() + std::size_t(yy) * (n + 1) + region.x0, columns, row);
        };
        const bool single = cells == 1;
        if (single) {
            heights(rowA.data(), gridY);
            heights(rowB.data(), ++gridY);
            // Composite textures have canonical Plain orientation. Native level
            // zero reflects the light and heights because it steps map rows +Y.
            loc.y = Flt32(2 * y + 1) * Flt32(1U << scale) - loc.y;
            std::swap(rowA[0], rowB[0]);
            std::swap(rowA[1], rowB[1]);
        } else {
            heights(next, gridY);
        }
        SquareStepper startX, stepY, zStep, lastZ;
        startX.init(loc.x - Flt32(region.x0) * Flt32(1U << scale), gridStep);
        auto dy = stepY.init(loc.y - Flt32(region.y0) * Flt32(1U << scale), gridStep);
        for (UInt32 yy = 0; yy <= stepsY; ++yy) {
            if (mask) {
                if (yy & mask) {
                    std::ranges::for_each(std::views::iota(0U, columns), [&](UInt32 c) {
                        current[c] += increments[c];
                    });
                } else {
                    std::swap(current, next);
                    if (yy != stepsY) {
                        if (!single) {
                            heights(next, ++gridY);
                        }
                        std::ranges::for_each(std::views::iota(0U, columns), [&](UInt32 c) {
                            increments[c] = (next[c] - current[c]) / Flt32(rate);
                        });
                    }
                }
            } else {
                heights(current, gridY++);
            }
            auto stepX = startX;
            auto dx = stepX.value;
            for (UInt32 xx = 0; xx <= stepsX; ++xx) {
                Flt32 dz{};
                if (mask) {
                    if (xx & mask) {
                        dz = zStep.next();
                    } else if (xx == stepsX) {
                        if (yy & mask) {
                            dz = lastZ.next();
                        } else if (yy == stepsY) {
                            const auto z = loc.z - current[xx >> bits];
                            dz = z * z;
                        } else {
                            dz = lastZ.init(loc.z - current[xx >> bits], increments[xx >> bits]);
                        }
                    } else {
                        const auto c = xx >> bits;
                        dz = zStep.init(loc.z - current[c],
                                        (current[c + 1] - current[c]) / Flt32(rate));
                    }
                } else {
                    const auto z = loc.z - current[xx];
                    dz = z * z;
                }
                const auto distance = dx + dy + dz;
                if (!std::isfinite(distance)) {
                    return {};
                }
                if (distance <= maxDistance) {
                    const auto index = UInt32(std::clamp(distance * colorScale, 0.f, 63.f));
                    const auto mapY = single ? yy : (y + cells - region.y0) * rate - yy;
                    auto& color =
                        result->samples[std::size_t(mapY) * side + (region.x0 - x) * rate + xx];
                    color = add(color, colors[index]);
                }
                dx = stepX.next();
            }
            dy = stepY.next();
        }
    }
    return std::shared_ptr<const gfx::LightMap>(std::move(result));
}
} // namespace opent1::world::terrain_detail
