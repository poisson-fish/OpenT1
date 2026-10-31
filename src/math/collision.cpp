#include <opent1/math/collision.hpp>
#include <ranges>

namespace opent1 {
namespace {
constexpr auto axes() {
    return std::views::iota(std::size_t{0}, std::size_t{3});
}

void checkBox(const Point3F& min, const Point3F& max) {
    OPENT1_ASSERT_FATAL(min.x <= max.x && min.y <= max.y && min.z <= max.z, "Invalid box bounds");
}

void checkRadii(const Point3F& radii) {
    OPENT1_ASSERT_FATAL(radii.x >= 0 && radii.y >= 0 && radii.z >= 0, "Invalid box radii");
}

std::array<std::size_t, 2> projection(const Point3F& n) {
    if (std::abs(n.y) > std::abs(n.x) && std::abs(n.y) > std::abs(n.z)) {
        return {0, 2};
    }
    if (std::abs(n.x) > std::abs(n.y) && std::abs(n.x) > std::abs(n.z)) {
        return {1, 2};
    }
    return {0, 1};
}

RealF edgeDot(const Point3F& a,
              const Point3F& b,
              const Point3F& p,
              const std::array<std::size_t, 2>& dims) {
    const auto x = dims[0], y = dims[1];
    return (a[x] - p[x]) * (a[y] - b[y]) + (a[y] - p[y]) * (b[x] - a[x]);
}

bool checkSide(RealF num,
               RealF den,
               RealF a1,
               RealF a2,
               RealF b1,
               RealF b2,
               RealF min1,
               RealF min2,
               RealF max1,
               RealF max2,
               RealF& time) {
    if (num * (b1 - a1) < den * (max1 - a1) && num * (b1 - a1) > den * (min1 - a1) &&
        num * (b2 - a2) < den * (max2 - a2) && num * (b2 - a2) > den * (min2 - a2)) {
        time = num / den;
        return true;
    }
    return false;
}
}

void m_normal(const Point3F& a, const Point3F& b, const Point3F& c, Point3F& out) {
    m_cross(b - c, a - c, out);
}

bool m_pointInTriangle(
    const Point3F& p, const Point3F& n, const Point3F& a, const Point3F& b, const Point3F& c) {
    const auto dims = projection(n);
    const auto d1 = edgeDot(a, b, p, dims), d2 = edgeDot(b, c, p, dims),
               d3 = edgeDot(c, a, p, dims);
    return d1 * d2 >= 0 && d2 * d3 >= 0;
}

bool m_pointInPoly(const Point3F& p, const Point3F& n, std::span<const Point3F> vertices) {
    if (vertices.size() < 3) {
        return false;
    }
    const auto dims = projection(n);
    auto last = edgeDot(vertices.back(), vertices.front(), p, dims);
    return std::ranges::all_of(std::views::iota(std::size_t{0}, vertices.size() - 1), [&](auto i) {
        const auto dot = edgeDot(vertices[i], vertices[i + 1], p, dims);
        const auto same = dot * last >= 0;
        last = dot;
        return same;
    });
}

bool m_lineAABox(
    const Point3F& a, const Point3F& b, const Point3F& min, const Point3F& max, RealF& time) {
    checkBox(min, max);
    bool outside = false;
    // Each face has two remaining dimensions, in original x/y/z priority.
    const auto hit = std::ranges::any_of(axes(), [&](auto i) {
        const auto j = i == 0 ? 1U : 0U, k = i == 2 ? 1U : 2U;
        if (a[i] < min[i]) {
            outside = true;
            return b[i] >= min[i] && checkSide(min[i] - a[i],
                                               b[i] - a[i],
                                               a[j],
                                               a[k],
                                               b[j],
                                               b[k],
                                               min[j],
                                               min[k],
                                               max[j],
                                               max[k],
                                               time);
        }
        if (a[i] > max[i]) {
            outside = true;
            return b[i] <= max[i] && checkSide(a[i] - max[i],
                                               a[i] - b[i],
                                               a[j],
                                               a[k],
                                               b[j],
                                               b[k],
                                               min[j],
                                               min[k],
                                               max[j],
                                               max[k],
                                               time);
        }
        return false;
    });
    if (hit) {
        return true;
    }
    if (!outside) {
        time = 0;
        return true;
    }
    return false;
}

bool m_tubeAABox(const Point3F& a,
                 const Point3F& b,
                 RealF radius,
                 const Point3F& min,
                 const Point3F& max,
                 RealF& time) {
    OPENT1_ASSERT_FATAL(radius >= 0, "Negative tube radius");
    return m_lineAABox(a, b, min - radius, max + radius, time);
}

bool m_sphereAABox(const Point3F& center,
                   RealF radiusSquared,
                   const Point3F& min,
                   const Point3F& max,
                   RealF& distance) {
    checkBox(min, max);
    OPENT1_ASSERT_FATAL(radiusSquared >= 0, "Negative squared radius");
    RealF value = 0;
    std::ranges::for_each(axes(), [&](auto i) {
        const auto delta = center[i] < min[i]   ? center[i] - min[i]
                           : center[i] > max[i] ? center[i] - max[i]
                                                : 0;
        value += delta * delta;
    });
    distance = value;
    return value < radiusSquared;
}

bool m_OBoxOBox(
    const Point3F& a, const Point3F& b, const TMat3F& t, Point3F& aOverlap, Point3F& bOverlap) {
    checkRadii(a);
    checkRadii(b);
    OPENT1_ASSERT_FATAL(!(t.flags & RMat3F::Matrix_HasScale),
                        "Box collision requires rigid transform");
    std::array<std::array<RealF, 3>, 3> abs{};
    std::ranges::for_each(axes(), [&](auto i) {
        std::ranges::transform(t.m[i], abs[i].begin(), [](auto v) {
            return std::abs(v);
        });
    });
    Point3F ao, bo;
    const auto separatedA = std::ranges::any_of(axes(), [&](auto i) {
        ao[i] = a[i] + b.x * abs[0][i] + b.y * abs[1][i] + b.z * abs[2][i] - std::abs(t.p[i]);
        return ao[i] < 0;
    });
    if (separatedA) {
        return false;
    }
    const auto separatedB = std::ranges::any_of(axes(), [&](auto i) {
        bo[i] = a.x * abs[i][0] + a.y * abs[i][1] + a.z * abs[i][2] + b[i] -
                std::abs(t.p.x * t.m[i][0] + t.p.y * t.m[i][1] + t.p.z * t.m[i][2]);
        return bo[i] < 0;
    });
    if (separatedB) {
        return false;
    }
    const auto crossSeparated = std::ranges::any_of(axes(), [&](auto i) {
        return std::ranges::any_of(axes(), [&](auto j) {
            const auto u = (i + 1) % 3, v = (i + 2) % 3, k = (j + 1) % 3, l = (j + 2) % 3;
            const auto distance = std::abs(t.p[v] * t.m[j][u] - t.p[u] * t.m[j][v]);
            const auto radius =
                a[u] * abs[j][v] + a[v] * abs[j][u] + b[k] * abs[l][i] + b[l] * abs[k][i];
            return distance > radius;
        });
    });
    if (crossSeparated) {
        return false;
    }
    aOverlap = ao;
    bOverlap = bo;
    return true;
}

bool m_rectOBox(const Point3F& a, const Point3F& b, const TMat3F& t, Point3F& ao, Point3F& bo) {
    return m_OBoxOBox({a.x, a.y, 0}, b, t, ao, bo);
}

bool m_lineOBox(const Point3F& a, const Point3F& b, const TMat3F& t, Point3F& ao, Point3F& bo) {
    return m_OBoxOBox({0, a.y, 0}, b, t, ao, bo);
}

bool m_planeOBox(
    const Point3F& radii, const TMat3F& t, const Point3F& n, const Point3F& p, RealF& overlap) {
    checkRadii(radii);
    const auto planeDistance = m_dot(n, p), negativeDistance = m_dot(n, t.p);
    if (planeDistance + negativeDistance < 0) {
        return false;
    }
    overlap = radii.x * std::abs(t.m[0][0] * n.x + t.m[0][1] * n.y + t.m[0][2] * n.z) +
              radii.y * std::abs(t.m[1][0] * n.x + t.m[1][1] * n.y + t.m[1][2] * n.z) +
              radii.z * std::abs(t.m[2][0] * n.x + t.m[2][1] * n.y + t.m[2][2] * n.z) -
              planeDistance + negativeDistance;
    return overlap > 0;
}

bool m_polyOBox(const Point3F& radii,
                const TMat3F& t,
                const TMat3F& inv,
                const Point3F& normal,
                std::span<const Point3F> vertices,
                RealF& overlap) {
    if (vertices.size() < 3) {
        return false;
    }
    checkRadii(radii);
    OPENT1_ASSERT_FATAL(!(t.flags & RMat3F::Matrix_HasScale) &&
                            !(inv.flags & RMat3F::Matrix_HasScale),
                        "Polygon collision requires rigid transforms");
    const auto projectedRadius =
        radii.x * std::abs(t.m[0][0] * normal.x + t.m[0][1] * normal.y + t.m[0][2] * normal.z) +
        radii.y * std::abs(t.m[1][0] * normal.x + t.m[1][1] * normal.y + t.m[1][2] * normal.z) +
        radii.z * std::abs(t.m[2][0] * normal.x + t.m[2][1] * normal.y + t.m[2][2] * normal.z);
    const auto depth =
        projectedRadius - std::abs(m_dot(normal, vertices.front()) - m_dot(normal, t.p));
    if (!(depth > 0)) {
        return false;
    }
    std::vector<Point3F> local(vertices.size());
    std::ranges::transform(vertices, local.begin(), [&](const auto& v) {
        Point3F p;
        m_mul(v, inv, p);
        return p;
    });
    Point3F n;
    m_mul(normal, static_cast<const RMat3F&>(inv), n);
    const auto separated = [&](const Point3F& axis) {
        const auto projected = local | std::views::transform([&](const auto& v) {
                                   return m_dot(v, axis);
                               });
        const auto bounds = std::ranges::minmax(projected);
        const auto radius =
            radii.x * std::abs(axis.x) + radii.y * std::abs(axis.y) + radii.z * std::abs(axis.z);
        return bounds.min > radius || bounds.max < -radius;
    };
    const std::array<Point3F, 3> basis{{{1, 0, 0}, {0, 1, 0}, {0, 0, 1}}};
    if (separated(n) || std::ranges::any_of(basis, separated)) {
        return false;
    }
    const auto edgeSeparates =
        std::ranges::any_of(std::views::iota(std::size_t{0}, local.size()), [&](auto i) {
            const auto edge = local[(i + 1) % local.size()] - local[i];
            return std::ranges::any_of(basis, [&](const auto& axis) {
                Point3F cross;
                m_cross(axis, edge, cross);
                return separated(cross);
            });
        });
    if (edgeSeparates) {
        return false;
    }
    overlap = depth;
    return true;
}

void CollisionSurface::transform(const TMat3F& m) {
    OPENT1_ASSERT_FATAL(!(m.flags & RMat3F::Matrix_HasScale),
                        "Surface transform requires rigid matrix");
    m_mul(position, m, position);
    m_mul(normal, static_cast<const RMat3F&>(m), normal);
}

void CollisionSurfaceList::transform(const TMat3F& m) {
    std::ranges::for_each(surfaces, [&](auto& s) {
        s.transform(m);
    });
}
} // namespace opent1
