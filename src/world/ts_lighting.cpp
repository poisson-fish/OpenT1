#include <opent1/world/ts_lighting.hpp>
#include <algorithm>
#include <cmath>

namespace opent1::world {
namespace {
bool finite(Point3F p) {
    return std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z);
}

bool positive(Point3F p) {
    return finite(p) && p.x >= 0 && p.y >= 0 && p.z >= 0;
}

constexpr std::array<Point3F, 256> normals{{
#include "ts_normals.inc"
}};
}

Point3F decodeTsNormal(UInt8 index) noexcept {
    return normals[index];
}

bool TsLighting::valid() const {
    return (worldSources.empty() || worldSources.size() == lights.size()) &&
           std::ranges::all_of(worldSources,
                               [](const auto& s) {
                                   return finite(s.position) && finite(s.direction);
                               }) &&
           finite(shapeToWorld.p) &&
           std::ranges::all_of(shapeToWorld.m,
                               [](const auto& row) {
                                   return std::ranges::all_of(row, [](auto v) {
                                       return std::isfinite(v);
                                   });
                               }) &&
           positive(ambient) && lights.size() <= 64 &&
           std::ranges::all_of(lights, [](const auto& l) {
               if (!positive(l.intensity) || !std::isfinite(l.shadowAttenuation) ||
                   l.shadowAttenuation < 0 || l.shadowAttenuation > 1) {
                   return false;
               }
               if (l.shadowMap && !l.shadowMap->valid()) {
                   return false;
               }
               switch (l.type) {
                   case TsLight::Type::Directional:
                   case TsLight::Type::DirectionalNonlinear:
                   case TsLight::Type::DirectionalWrap:
                       return finite(l.direction) &&
                              std::abs(m_dot(l.direction, l.direction) - 1) < .0001f;
                   case TsLight::Type::Point:
                       return finite(l.position) && std::isfinite(l.range) && l.range > 0 &&
                              positive(l.attenuation);
               }
               return false;
           });
}

namespace {
std::optional<Point3F> evaluate(const TsLighting& lighting,
                                Point3F position,
                                Point3F normal,
                                std::span<const TsPreparedLighting::State> states = {},
                                bool validate = true) {
    // Quat16 node matrices can slightly shorten packed normals. Preserve those
    // authored values, as the original inverse-light transform does; requiring
    // exact unit length would reject ordinary animated character meshes.
    const auto lengthSquared = m_dot(normal, normal);
    if ((validate && !lighting.valid()) || !finite(position) || !finite(normal) ||
        !std::isfinite(lengthSquared) || lengthSquared <= 0) {
        return {};
    }
    auto result = lighting.ambient;
    std::size_t index{};
    if (!std::ranges::all_of(lighting.lights, [&](const auto& l) {
            const auto* state = states.empty() ? nullptr : &states[index++];
            Flt32 amount{};
            if (l.type == TsLight::Type::Point) {
                using Mode = TsPreparedLighting::PointMode;
                if (state && state->mode == Mode::OutOfRange) {
                    return true;
                }
                if (state &&
                    (state->mode == Mode::MostlyOptimized || state->mode == Mode::FullyOptimized)) {
                    if (state->mode == Mode::MostlyOptimized) {
                        const auto delta = position - l.position;
                        if (!finite(delta) || !std::isfinite(m_dot(delta, delta))) {
                            return false;
                        }
                        if (delta.lenf() > l.range) {
                            return true;
                        }
                    }
                    amount = -m_dot(state->direction, normal);
                    if (amount < 0) {
                        return true;
                    }
                    amount *= state->attenuation;
                    result += l.intensity * amount;
                    return finite(result);
                }
                const auto direction = position - l.position;
                if (!finite(direction) || !std::isfinite(m_dot(direction, direction))) {
                    return false;
                }
                const auto distance = direction.lenf(); // original bit-approximate length
                if (!std::isfinite(distance)) {
                    return false;
                }
                if (distance > l.range) {
                    return true;
                }
                amount = -m_dot(direction, normal);
                if (amount < 0) {
                    return true;
                }
                if (distance > .001f) {
                    amount /= distance;
                    const auto formula =
                        l.attenuation.x + (l.attenuation.y + l.attenuation.z * distance) * distance;
                    if (!std::isfinite(formula)) {
                        return false;
                    }
                    if (formula > 1) {
                        amount /= formula;
                    }
                } else {
                    amount = 1;
                }
            } else {
                amount = -m_dot(l.direction, normal);
                if (l.type == TsLight::Type::DirectionalWrap) {
                    amount = (amount + 1) * .5f;
                } else {
                    if (amount < 0) {
                        return true;
                    }
                    if (l.type == TsLight::Type::DirectionalNonlinear) {
                        const auto a = amount - 1;
                        amount = 1 + a * a * a;
                    }
                }
                amount *= l.shadowAttenuation;
            }
            result += l.intensity * amount;
            return finite(result);
        })) {
        return {};
    }
    return Point3F{std::clamp(result.x, 0.f, 1.f),
                   std::clamp(result.y, 0.f, 1.f),
                   std::clamp(result.z, 0.f, 1.f)};
}
}

std::optional<Point3F> lightTsVertex(const TsLighting& lighting, Point3F position, Point3F normal) {
    return evaluate(lighting, position, normal);
}

std::optional<TsPreparedLighting> TsPreparedLighting::prepare(const TsLighting& lighting,
                                                              Point3F center,
                                                              Flt32 radius,
                                                              const TMat3F& objectToShape) {
    if (!lighting.valid() || !finite(center) || !std::isfinite(radius) || radius < 0 ||
        !finite(objectToShape.p) ||
        !std::ranges::all_of(
            objectToShape.m,
            [](const auto& row) {
                return std::ranges::all_of(row, [](auto v) {
                    return std::isfinite(v);
                });
            }) ||
        !std::isfinite(objectToShape.determinant()) ||
        std::abs(objectToShape.determinant()) < 1e-8f) {
        return {};
    }
    TsPreparedLighting result;
    result.lighting_.ambient = lighting.ambient;
    result.lighting_.lights.reserve(lighting.lights.size());
    result.states_.reserve(lighting.lights.size());
    auto inverse = objectToShape;
    inverse.inverse();
    TMat3F toWorld;
    m_mul(objectToShape, lighting.shapeToWorld, toWorld);
    const auto worldSources = !lighting.worldSources.empty();
    if (worldSources) {
        inverse = toWorld;
        inverse.inverse();
    }
    Point3F world;
    m_mul(center, toWorld, world);
    if (!finite(world)) {
        return {};
    }
    std::size_t index{};
    for (const auto& input : lighting.lights) {
        // The prepared mesh needs scalar state only. Sample the source map in
        // place instead of cloning potentially large byte maps for every mesh.
        TsLight light{.type = input.type,
                      .direction = input.direction,
                      .position = input.position,
                      .intensity = input.intensity,
                      .range = input.range,
                      .shadowAttenuation = input.shadowAttenuation,
                      .attenuation = input.attenuation};
        State state;
        if (light.type == TsLight::Type::Point) {
            const auto source =
                worldSources ? lighting.worldSources[index].position : light.position;
            Point3F local;
            m_mul(source, inverse, local);
            light.position = local;
            state.direction = center - local;
            if (!finite(state.direction) ||
                !std::isfinite(m_dot(state.direction, state.direction))) {
                return {};
            }
            const auto distance = state.direction.lenf();
            if (distance > light.range + radius) {
                state.mode = PointMode::OutOfRange;
            } else if (distance > radius * 20.f && distance > 1.f) {
                state.direction /= distance;
                const auto formula =
                    light.attenuation.x +
                    (light.attenuation.y + light.attenuation.z * distance) * distance;
                if (!std::isfinite(formula) || double(formula) >= 2147483648.) {
                    return {};
                }
                const auto atten = Int32(formula);
                state.attenuation = atten ? 1.f / Flt32(atten) : 0.f;
                state.mode = distance > light.range - 2 * radius ? PointMode::MostlyOptimized
                                                                 : PointMode::FullyOptimized;
            }
        } else {
            const auto source =
                worldSources ? lighting.worldSources[index].direction : light.direction;
            Point3F local;
            m_mul(source, static_cast<const RMat3F&>(inverse), local);
            light.direction = local;
            // Native directionalPrepare replaces any prior attenuation on each
            // mesh. Keep the explicit adapter attenuation when no map is bound.
            if (input.shadowMap) {
                const auto atten = input.shadowMap->attenuation(world);
                if (!atten) {
                    return {};
                }
                light.shadowAttenuation = *atten;
            }
        }
        result.lighting_.lights.push_back(std::move(light));
        result.states_.push_back(state);
        ++index;
    }
    // Input directions are unit length, but the original Quat16 object inverse
    // introduces small length drift. These are prepared mesh-space vectors:
    // retain that drift (as the native renderer does), instead of applying the
    // public unit-vector validation again and rejecting an entire lit scene.
    for (const auto& light : result.lighting_.lights) {
        if (light.type == TsLight::Type::Point) {
            if (!finite(light.position)) {
                return {};
            }
        } else if (!finite(light.direction) ||
                   !std::isfinite(m_dot(light.direction, light.direction)) ||
                   m_dot(light.direction, light.direction) <= 0) {
            return {};
        }
    }
    return result;
}

std::optional<Point3F> TsPreparedLighting::lightVertex(Point3F position, Point3F normal) const {
    return evaluate(lighting_, position, normal, states_, false);
}
} // namespace opent1::world
