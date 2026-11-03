#include "interior_scene_lighting.hpp"
#include <ranges>
#include "collision_geometry.hpp"

namespace opent1::world {
namespace {
UInt16 add(UInt16 a, UInt16 b) {
    UInt32 result{};
    for (UInt32 shift = 0; shift < 16; shift += 4) {
        result |= std::min(15U, ((UInt32(a) >> shift) & 15) + ((UInt32(b) >> shift) & 15)) << shift;
    }
    return UInt16(result);
}

UInt16 packed(Point3F color) {
    color.x = std::min(color.x, 1.f);
    color.y = std::min(color.y, 1.f);
    color.z = std::min(color.z, 1.f);
    const auto mono = Flt32(.3 * color.x + .6 * color.y + .1 * color.z);
    return UInt16((UInt16(mono * 15.99) << 12) | (UInt16(color.x * 15.99) << 8) |
                  (UInt16(color.y * 15.99) << 4) | UInt16(color.z * 15.99));
}

struct SquareStepper {
    Flt32 value{}, twiceKX{}, odd{}, twiceK2{};

    SquareStepper(Flt32 x, Flt32 k)
        : value(x * x), twiceKX(Flt32(k * 2.0 * x)), odd(k * k), twiceK2(odd + odd) {}

    Flt32 next() {
        value += odd;
        odd += twiceK2;
        return value -= twiceKX;
    }
};

std::optional<Point2F>
textureCoordinate(const InteriorGeometry& g, const InteriorSurface& s, Point3F point) {
    if (s.vertexCount < 3 || s.firstVertex > g.vertices.size() ||
        s.vertexCount > g.vertices.size() - s.firstVertex) {
        return {};
    }
    std::array<Point3F, 3> world;
    std::array<Point2F, 3> uv;
    for (auto i : std::views::iota(std::size_t{0}, std::size_t{3})) {
        const auto v = g.vertices[s.firstVertex + i];
        if (v.point >= g.points.size() || v.texture >= g.texturePoints.size()) {
            return {};
        }
        world[i] = g.boundaryPoint(g.points[v.point]);
        uv[i] = g.texturePoints[v.texture];
        if (!collision_detail::finite(world[i]) || !std::isfinite(uv[i].x) ||
            !std::isfinite(uv[i].y)) {
            return {};
        }
    }
    auto a = world[0] - world[1], b = world[2] - world[1];
    const auto lengthB = b.len();
    if (!std::isfinite(lengthB) || lengthB < 1e-12f) {
        return {};
    }
    b /= lengthB;
    const auto nonOrtho = m_dot(a, b);
    if (nonOrtho != 0) {
        a -= b * nonOrtho;
    }
    auto u = uv[0] - uv[1], v = uv[2] - uv[1], base = uv[1];
    for (auto* p : {&u, &v, &base}) {
        p->x *= UInt32(s.width) + 1;
        p->y *= UInt32(s.height) + 1;
    }
    if (nonOrtho != 0) {
        u -= v * (nonOrtho / lengthB);
    }
    const auto lengthA = a.len();
    if (!std::isfinite(lengthA) || lengthA < 1e-12f) {
        return {};
    }
    a /= lengthA;
    const auto relative = point - world[1];
    auto result = u * (m_dot(a, relative) / lengthA);
    result += v * (m_dot(b, relative) / lengthB);
    result += base;
    return std::isfinite(result.x) && std::isfinite(result.y) ? std::optional(result)
                                                              : std::nullopt;
}
}

std::optional<InteriorSceneLights> InteriorSceneLights::prepare(const InteriorGeometry& g,
                                                                const InteriorLighting& l,
                                                                const TsLighting& scene,
                                                                bool exterior) {
    if (!scene.valid() || g.surfaces.size() > 65536 || l.surfaces.size() != g.surfaces.size() ||
        g.highestMipLevel < 0 || g.highestMipLevel > 15 || l.scaleShift < 0 || l.scaleShift > 15 ||
        !std::isfinite(g.textureScale) || g.textureScale <= 0) {
        return {};
    }
    InteriorSceneLights result;
    result.geometry_ = &g;
    result.lighting_ = &l;
    result.scene_ = &scene;
    result.exterior_ = exterior;
    result.points_.resize(g.surfaces.size(), {-1, -1, -1, -1});
    std::optional<InteriorCollision> collision;
    std::size_t entries{};
    try {
        for (auto i : std::views::iota(std::size_t{0}, scene.lights.size())) {
            const auto& light = scene.lights[i];
            if (light.type != TsLight::Type::Point) {
                continue;
            }
            if (!collision) {
                collision = g.collision();
                if (!collision) {
                    return {};
                }
            }
            CollisionSurfaceList hits;
            (void)collision->sphere(light.position, light.range, hits);
            for (const auto& hit : hits.surfaces) {
                if (hit.surface < 0 || std::size_t(hit.surface) >= result.points_.size()) {
                    return {};
                }
                auto& indices = result.points_[std::size_t(hit.surface)];
                const auto slot = std::ranges::find(indices, Int16{-1});
                if (slot == indices.end()) {
                    continue;
                }
                // findAffectedSurfaces returns for this light when the native
                // 1200-entry cache fills. Collision order decides the cutoff.
                if (entries == 1200) {
                    break;
                }
                ++entries;
                if (slot == indices.begin()) {
                    *slot = Int16(i);
                } else {
                    // MultiHashMap::insert inserts after the first matching
                    // entry, retaining the first source at the head.
                    std::move_backward(indices.begin() + 1, slot, slot + 1);
                    indices[1] = Int16(i);
                }
            }
            const auto color = packed(light.intensity);
            const auto maximum = Flt32(std::sqrt(64.f + .001f));
            for (auto index : std::views::iota(UInt32{0}, UInt32{64})) {
                UInt32 value{};
                for (UInt32 shift = 0; shift < 16; shift += 4) {
                    const auto component = (color >> shift) & 15;
                    if (component) {
                        value |=
                            UInt32(UInt8(Flt32(Flt32(component) + .5f) -
                                         (std::sqrt(Flt32(index)) / maximum) * Flt32(component)))
                            << shift;
                    }
                }
                result.tables_[i][index] = UInt16(value);
            }
        }
    } catch (const std::invalid_argument&) {
        return {};
    } catch (const std::length_error&) {
        return {};
    } catch (const std::overflow_error&) {
        return {};
    }
    return result;
}

std::span<const Int16> InteriorSceneLights::pointLights(UInt32 surface) const {
    if (surface >= points_.size()) {
        return {};
    }
    const auto& indices = points_[surface];
    return {indices.begin(), std::ranges::find(indices, Int16{-1})};
}

bool InteriorSceneLights::merge(UInt32 index, std::span<UInt16> colors, std::size_t& work) const {
    if (index >= points_.size()) {
        return false;
    }
    const auto& g = *geometry_;
    const auto& l = *lighting_;
    const auto& s = g.surfaces[index];
    const auto& map = l.surfaces[index];
    if (s.plane >= g.planes.size()) {
        return false;
    }
    const auto& plane = g.planes[s.plane];
    if (!collision_detail::finite(plane) || !std::isfinite(plane.d) ||
        std::abs(m_dot(plane, plane) - 1) > .0001f) {
        return false;
    }
    const auto visits = std::max(1U, UInt32(map.width)) * std::max(1U, UInt32(map.height));
    if (colors.size() != visits) {
        return false;
    }
    for (const auto lightIndex : points_[index]) {
        if (lightIndex < 0) {
            break;
        }
        if (l.scaleShift < g.highestMipLevel || !map.width || !map.height) {
            return false;
        }
        work += visits;
        if (work > 16'777'216) {
            return false;
        }
        const auto& light = scene_->lights[std::size_t(lightIndex)];
        const auto distance = m_dot(plane, light.position) + plane.d;
        const auto coordinate = textureCoordinate(g, s, light.position - Point3F(plane) * distance);
        if (!coordinate) {
            return false;
        }
        const auto shift = (s.flags >> 1) & 15U;
        auto scale = 1.f / g.textureScale;
        scale = shift < 8 ? scale / Flt32(1U << shift) : scale * Flt32(1U << (shift - 8));
        scale = (scale * scale) * Flt32(1U << (g.highestMipLevel * 2));
        const auto radius = (light.range * light.range) / scale,
                   planeDistance = (distance * distance) / scale;
        const auto step = Flt32(1U << (l.scaleShift - g.highestMipLevel));
        const auto remapScale = 64.f / (radius + .001f);
        if (!std::isfinite(radius) || radius <= 0 || !std::isfinite(planeDistance) ||
            !std::isfinite(remapScale)) {
            return false;
        }
        const auto startX = coordinate->x + map.offsetX, startY = coordinate->y + map.offsetY;
        if (!std::isfinite(startX * startX + startY * startY)) {
            return false;
        }
        SquareStepper ySquared(startY, step);
        const SquareStepper xStart(startX, step);
        std::size_t at{};
        for ([[maybe_unused]] auto y :
             std::views::iota(UInt32{0}) | std::views::take_while([&](UInt32 y) {
                 return y < map.height;
             })) {
            auto xSquared = xStart;
            for ([[maybe_unused]] const auto x : std::views::iota(UInt32{0}, map.width)) {
                const auto d = xSquared.value + ySquared.value;
                if (d < radius - planeDistance) {
                    const auto bucket = (d + planeDistance) * remapScale;
                    if (!std::isfinite(bucket)) {
                        return false;
                    }
                    colors[at] = add(
                        colors[at],
                        tables_[std::size_t(lightIndex)][UInt32(std::clamp(bucket, 0.f, 63.f))]);
                }
                xSquared.next();
                ++at;
            }
            ySquared.next();
        }
    }
    if (!exterior_ || !(s.flags & 0x40U)) {
        return true;
    }
    auto color = scene_->ambient;
    const auto normal = Point3F(plane) * (s.planeFront() ? 1.f : -1.f);
    bool found = false;
    for (const auto& light : scene_->lights) {
        if (light.type != TsLight::Type::Directional &&
            light.type != TsLight::Type::DirectionalWrap) {
            continue;
        }
        auto amount = -m_dot(light.direction, normal);
        if (light.type == TsLight::Type::DirectionalWrap) {
            amount = Flt32((amount + 1.0) / 2.0);
        }
        if (amount > 0) {
            found = true;
            color += light.intensity * amount;
        }
    }
    if (!found) {
        return true;
    }
    if (!std::isfinite(color.x) || !std::isfinite(color.y) || !std::isfinite(color.z)) {
        return false;
    }
    // Retain the original float clamp and 16x vector packing, including the
    // exact upper boundary. Mono is computed before RGB components clamp.
    const auto mono = std::min(Flt32(.3 * color.x + .6 * color.y + .1 * color.z), 1.f);
    const UInt32 m = UInt16(UInt16(mono * 16.0) << 12),
                 r = UInt16(UInt16(std::min(color.x, 1.f) * 16.0) << 8),
                 green = UInt16(UInt16(std::min(color.y, 1.f) * 16.0) << 4),
                 b = UInt16(std::min(color.z, 1.f) * 16.0);
    work += visits;
    if (work > 16'777'216) {
        return false;
    }
    for (auto& c : colors) {
        c = UInt16(std::min(0xf000U, (c & 0xf000U) + m) | std::min(0xf00U, (c & 0xf00U) + r) |
                   std::min(0xf0U, (c & 0xf0U) + green) | std::min(0xfU, (c & 0xfU) + b));
    }
    return true;
}
}
