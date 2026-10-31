#pragma once
#include <opent1/math/collision.hpp>
#include <algorithm>
#include <cmath>
#include <ranges>
#include <stdexcept>

namespace opent1::world::collision_detail {
inline constexpr std::size_t MaxWork = 4'194'304, MaxSurfaces = 4096;

inline auto axes() {
    return std::views::iota(std::size_t{0}, std::size_t{3});
}

inline bool finite(Point3F p) {
    return std::ranges::all_of(axes(), [&](auto i) {
        return std::isfinite(p[i]) && std::abs(p[i]) <= 1e12f;
    });
}

inline bool valid(const Box3F& box) {
    return finite(box.fMin) && finite(box.fMax) && box.isValid();
}

inline bool rigid(const TMat3F& t) {
    if ((t.flags & RMat3F::Matrix_HasScale) ||
        (!t.p.isZero() && !(t.flags & RMat3F::Matrix_HasTranslation))) {
        return false;
    }
    if (!(t.flags & RMat3F::Matrix_HasRotation) && !std::ranges::all_of(axes(), [&](auto i) {
            return std::ranges::all_of(axes(), [&](auto j) {
                return std::abs(t.m[i][j] - (i == j ? 1.f : 0.f)) < 1e-6f;
            });
        })) {
        return false;
    }
    return finite(t.p) && std::abs(t.determinant() - 1) < .0003f &&
           std::ranges::all_of(
               axes(),
               [&](auto i) {
                   const Point3F a{t.m[i][0], t.m[i][1], t.m[i][2]};
                   return finite(a) && std::abs(m_dot(a, a) - 1) < .0001f &&
                          std::ranges::all_of(
                              axes(),
                              [&](auto j) {
                                  return i == j ||
                                         std::abs(m_dot(
                                             a, Point3F{t.m[j][0], t.m[j][1], t.m[j][2]})) < .0001f;
                              });
               });
}

inline void check(Point3F a, Point3F b, Flt32 radius = 0) {
    if (!finite(a) || !finite(b) || !std::isfinite(radius) || radius < 0 || radius > 1e12f) {
        throw std::invalid_argument("invalid world collision primitive");
    }
}

inline void spend(std::size_t& remaining, std::size_t amount = 1) {
    if (amount > remaining) {
        throw std::length_error("world collision work limit");
    }
    remaining -= amount;
}

inline void append(CollisionSurfaceList& list, CollisionSurface surface) {
    if (list.surfaces.size() == MaxSurfaces) {
        throw std::length_error("world collision surface limit");
    }
    list.surfaces.push_back(surface);
}

inline bool publish(CollisionSurfaceList& output, CollisionSurfaceList result) {
    if (result.surfaces.empty() && !result.contained) {
        return false;
    }
    output.surfaces = std::move(result.surfaces);
    output.contained = result.contained;
    output.stepVector = result.stepVector;
    return true;
}

inline Point3F point(Point3F p, const TMat3F& t) {
    Point3F result;
    m_mul(p, t, result);
    return result;
}

inline Point3F unit(Point3F p) {
    const auto scale = std::max({std::abs(p.x), std::abs(p.y), std::abs(p.z)});
    if (!scale) {
        return {0, 0, 1};
    }
    p /= scale;
    return p / p.len();
}
} // namespace opent1::world::collision_detail
