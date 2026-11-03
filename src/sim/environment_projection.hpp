#pragma once
#include <opent1/sim/sim_environment_camera.hpp>
#include <opent1/gfx/software_rasterizer.hpp>
#include <algorithm>
#include <cmath>
#include <iterator>
#include <ranges>

namespace opent1::sim::environment_detail {
inline bool finite(double value) {
    return std::isfinite(value) && std::abs(value) <= 1e12;
}

inline bool finite(Point3F p) {
    return finite(p.x) && finite(p.y) && finite(p.z);
}

inline bool finite(Point2F p) {
    return finite(p.x) && finite(p.y);
}

inline bool unit(Flt32 value) {
    return std::isfinite(value) && value >= 0 && value <= 1;
}

inline bool valid(const EnvironmentCamera& c) {
    return c.viewport.valid() && c.viewport.left >= -16384 && c.viewport.top >= -16384 &&
           c.viewport.right < 16384 && c.viewport.bottom < 16384 && finite(c.center) &&
           finite(c.focal) && c.focal.x > 0 && c.focal.y > 0 && c.focal.x <= 16384 &&
           c.focal.y <= 16384 && c.nearDistance >= 1.f / 65536 && c.farDistance <= 65536 &&
           c.nearDistance < c.farDistance && finite(c.worldToCamera.p) &&
           std::ranges::all_of(
               c.worldToCamera.m,
               [](const auto& row) {
                   return std::ranges::all_of(row, [](auto v) {
                       return finite(v);
                   });
               });
}

struct Vertex {
    std::array<double, 3> position;
    Point2F texture;
    Flt32 haze{};
};

using Polygon = std::vector<Vertex>;

// Verify the supplied boundary before clipping can hide concavity, duplicate
// corners or a noncoplanar input. Double relative coordinates avoid cancellation
// at large world translations. Tiny coplanarity/edge errors allow CPU clipping
// results, whose public storage is float.
inline bool convex(const Polygon& polygon) {
    const auto delta = [](const auto& a, const auto& b) {
        return std::array{a[0] - b[0], a[1] - b[1], a[2] - b[2]};
    };
    const auto cross = [](const auto& a, const auto& b) {
        return std::array{
            a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
    };
    const auto dot = [](const auto& a, const auto& b) {
        return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
    };
    const auto& origin = polygon.front().position;
    std::array<double, 3> normal{};
    double scale = 0;
    std::ranges::for_each(std::views::iota(std::size_t{1}, polygon.size() - 1), [&](auto i) {
        const auto n =
            cross(delta(polygon[i].position, origin), delta(polygon[i + 1].position, origin));
        std::ranges::for_each(std::views::iota(0U, 3U), [&](auto j) {
            normal[j] += n[j];
        });
    });
    std::ranges::for_each(polygon, [&](const auto& v) {
        const auto d = delta(v.position, origin);
        scale = std::max(scale, std::sqrt(dot(d, d)));
    });
    const auto length = std::sqrt(dot(normal, normal));
    if (!std::isfinite(length) || length == 0) {
        return false;
    }
    std::ranges::for_each(normal, [&](auto& v) {
        v /= length;
    });
    if (!std::ranges::all_of(polygon, [&](const auto& v) {
            return std::abs(dot(delta(v.position, origin), normal)) <= std::max(1e-6, scale * 2e-6);
        })) {
        return false;
    }
    return std::ranges::all_of(std::views::iota(std::size_t{0}, polygon.size()), [&](auto i) {
        const auto& a = polygon[i].position;
        if (std::ranges::any_of(std::span(polygon).subspan(i + 1), [&](const auto& v) {
                return v.position == a;
            })) {
            return false;
        }
        const auto edge = delta(polygon[(i + 1) % polygon.size()].position, a);
        const auto edgeLength = std::sqrt(dot(edge, edge));
        if (edgeLength == 0) {
            return false;
        }
        const auto inward = cross(normal, edge);
        return std::ranges::all_of(polygon, [&](const auto& v) {
            return dot(delta(v.position, a), inward) >= -scale * edgeLength * 2e-6;
        });
    });
}

inline double distance(const Vertex& v, const EnvironmentCamera& c, unsigned plane) {
    const auto& p = v.position;
    const double depth = c.perspective ? p[1] : 1;
    const double x = c.center.x * depth + c.focal.x * p[0];
    const double y = c.center.y * depth - c.focal.y * p[2];
    switch (plane) {
        case 0:
            return p[1] - c.nearDistance;
        case 1:
            return c.farDistance - p[1];
        case 2:
            return x - c.viewport.left * depth;
        case 3:
            return (double(c.viewport.right) + 1) * depth - x;
        case 4:
            return y - c.viewport.top * depth;
        default:
            return (double(c.viewport.bottom) + 1) * depth - y;
    }
}

inline Polygon clip(Polygon polygon, const EnvironmentCamera& c, unsigned plane) {
    if (polygon.empty()) {
        return {};
    }
    Polygon result;
    result.reserve(polygon.size() + 1);
    auto previous = polygon.back();
    auto previousDistance = distance(previous, c, plane);
    std::ranges::for_each(polygon, [&](const auto& current) {
        const auto currentDistance = distance(current, c, plane);
        if ((previousDistance >= 0) != (currentDistance >= 0)) {
            const auto t = previousDistance / (previousDistance - currentDistance);
            Vertex intersection{};
            std::ranges::for_each(std::views::iota(0U, 3U), [&](auto i) {
                intersection.position[i] =
                    previous.position[i] + (current.position[i] - previous.position[i]) * t;
            });
            intersection.texture = {
                Flt32(previous.texture.x + (double(current.texture.x) - previous.texture.x) * t),
                Flt32(previous.texture.y + (double(current.texture.y) - previous.texture.y) * t)};
            intersection.haze = Flt32(previous.haze + (double(current.haze) - previous.haze) * t);
            result.push_back(intersection);
        }
        if (currentDistance >= 0) {
            result.push_back(current);
        }
        previous = current;
        previousDistance = currentDistance;
    });
    return result;
}

template <class Input>
std::optional<std::vector<gfx::PolygonVertex>> project(const Input& input,
                                                       const EnvironmentCamera& c,
                                                       bool twoSided,
                                                       bool textured,
                                                       bool repeat = false) {
    if (input.size() < 3 || input.size() > 36) {
        return {};
    }
    Polygon polygon;
    polygon.reserve(input.size());
    const bool good = std::ranges::all_of(input, [&](const auto& v) {
        if (!finite(v.position) || !finite(v.texture) ||
            (textured && !repeat &&
             (v.texture.x < -1e-5f || v.texture.x > 1.00001f || v.texture.y < -1e-5f ||
              v.texture.y > 1.00001f))) {
            return false;
        }
        Vertex vertex{};
        std::ranges::for_each(std::views::iota(0U, 3U), [&](auto j) {
            vertex.position[j] = double(v.position.x) * c.worldToCamera.m[0][j] +
                                 double(v.position.y) * c.worldToCamera.m[1][j] +
                                 double(v.position.z) * c.worldToCamera.m[2][j] +
                                 c.worldToCamera.p[j];
        });
        vertex.texture = textured ? (repeat ? v.texture
                                            : Point2F{std::clamp(v.texture.x, 0.f, 1.f),
                                                      std::clamp(v.texture.y, 0.f, 1.f)})
                                  : Point2F{};
        if constexpr (requires { v.haze; }) {
            if (!unit(v.haze)) {
                return false;
            }
            vertex.haze = v.haze;
        }
        polygon.push_back(vertex);
        return true;
    });
    if (!good || !convex(polygon)) {
        return {};
    }
    std::ranges::for_each(std::views::iota(0U, 6U), [&](auto plane) {
        polygon = clip(std::move(polygon), c, plane);
    });
    std::vector<gfx::PolygonVertex> result;
    result.reserve(polygon.size());
    const auto screen = [](double value, double lower, double upper) {
        if (std::abs(value - lower) < 1e-7) {
            return Flt32(lower);
        }
        if (std::abs(value - upper) < 1e-7) {
            return Flt32(upper);
        }
        return Flt32(std::clamp(value, lower, upper));
    };
    std::ranges::transform(polygon, std::back_inserter(result), [&](const auto& vertex) {
        const auto& p = vertex.position;
        const auto depth = c.perspective ? p[1] : 1;
        gfx::PolygonVertex output;
        output.position = {screen(c.center.x + c.focal.x * p[0] / depth,
                                  double(c.viewport.left),
                                  double(c.viewport.right) + 1),
                           screen(c.center.y - c.focal.y * p[2] / depth,
                                  double(c.viewport.top),
                                  double(c.viewport.bottom) + 1)};
        output.texture = vertex.texture;
        output.haze = vertex.haze;
        const double depthValue = c.perspective ? 1 / p[1]
                                                : (double(c.nearDistance) + c.farDistance - p[1]) /
                                                      (double(c.nearDistance) * c.farDistance);
        output.inverseDepth = Flt32(std::clamp(depthValue, 1. / 65536, 65536.));
        return output;
    });
    // Remove boundary contacts and float-rounding-sized turns before passing a
    // convex polygon to the exact software scanner. The tolerance is measured
    // relative to the adjacent edges (2e-6), not world coordinates.
    bool changed = true;
    while (changed && result.size() >= 3) {
        changed = false;
        for (auto i : std::views::iota(std::size_t{0}) | std::views::take_while([&](std::size_t i) {
                          return i < result.size();
                      })) {
            const auto a = result[(i + result.size() - 1) % result.size()].position;
            const auto b = result[i].position, d = result[(i + 1) % result.size()].position;
            const double ax = double(b.x) - a.x, ay = double(b.y) - a.y, bx = double(d.x) - b.x,
                         by = double(d.y) - b.y;
            if (std::abs(ax * by - ay * bx) <= 2e-6 * std::hypot(ax, ay) * std::hypot(bx, by)) {
                result.erase(result.begin() + std::ptrdiff_t(i));
                changed = true;
                break;
            }
        }
    }
    if (result.size() < 3) {
        result.clear();
        return result;
    }
    double area{};
    auto previous = result.back().position;
    std::ranges::for_each(result, [&](const auto& v) {
        area += (double(previous.x) - v.position.x) * (double(previous.y) + v.position.y);
        previous = v.position;
    });
    if (area == 0 || (area < 0 && !twoSided)) {
        result.clear();
    } else if (area < 0) {
        std::ranges::reverse(result);
    }
    return result;
}
} // namespace opent1::sim::environment_detail
