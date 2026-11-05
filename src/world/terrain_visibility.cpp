#include <opent1/world/terrain_visibility.hpp>
#include <opent1/math/plane.hpp>
#include <algorithm>
#include <cmath>
#include <ranges>

namespace opent1::world {
bool TerrainVisibilityTable::visible(Int32 x, Int32 y) const {
    const auto row = Int64(y) - camera.y - startY;
    if (row < 0 || UInt64(row) >= rows.size()) {
        return false;
    }
    const auto relative = Int64(x) - camera.x;
    const auto& span = rows[std::size_t(row)];
    return relative >= span.minimum && relative <= span.maximum;
}

std::optional<UInt32> terrainGeometryScanLevel(const TerrainGeometryTable& table, Flt32 distance) {
    if (!std::isfinite(distance) || distance <= 0 || std::ranges::any_of(table, [](const auto& d) {
            return !std::isfinite(d.maxDistance);
        })) {
        return {};
    }
    UInt32 level{};
    while (level < 7 && table[level].maxDistance != 0 && distance > table[level].maxDistance) {
        ++level;
    }
    return level;
}

namespace {
bool finite(Flt32 v) {
    return std::isfinite(v) && std::abs(v) <= 1e9f;
}

bool rotation(const RMat3F& m) {
    for (auto i : std::views::iota(std::size_t{0}, std::size_t{3})) {
        for (auto j : std::views::iota(std::size_t{0}, std::size_t{3})) {
            if (!finite(m.m[i][j])) {
                return false;
            }
            Flt32 dot{};
            for (auto k : std::views::iota(std::size_t{0}, std::size_t{3})) {
                dot += m.m[i][k] * m.m[j][k];
            }
            if (std::abs(dot - (i == j ? 1.f : 0.f)) > 1e-4f) {
                return false;
            }
        }
    }
    return std::abs(m.determinant() - 1.f) < 1e-4f;
}

// Footprint vertices often lie on two view planes at once. Classifying an
// endpoint and then independently intersecting its edge in float can disagree
// about whether the crossing is on the segment. Losing both crossings removes
// an entire corner of the footprint, including otherwise visible near terrain.
// Use the same signed distances for admission and interpolation, and keep the
// crossing even when its rounded position lands just outside the next plane.
std::vector<Point3F> clipFootprint(std::span<const Point3F> points, const PlaneF& plane) {
    std::vector<Point3F> result;
    if (points.empty())
        return result;
    result.reserve(points.size() + 1);
    const auto distance = [&](Point3F point) {
        return Flt64(plane.x) * point.x + Flt64(plane.y) * point.y + Flt64(plane.z) * point.z +
               plane.d;
    };
    auto previous = points.back();
    auto previousDistance = distance(previous);
    for (const auto point : points) {
        const auto currentDistance = distance(point);
        if ((previousDistance < 0) != (currentDistance < 0)) {
            const auto t = previousDistance / (previousDistance - currentDistance);
            result.push_back({Flt32(Flt64(previous.x) + (Flt64(point.x) - previous.x) * t),
                              Flt32(Flt64(previous.y) + (Flt64(point.y) - previous.y) * t),
                              Flt32(Flt64(previous.z) + (Flt64(point.z) - previous.z) * t)});
        }
        if (currentDistance >= 0)
            result.push_back(point);
        previous = point;
        previousDistance = currentDistance;
    }
    return result;
}

// Preserve truncating division, arithmetic right shift and inclusive boundary
// rules without the original signed-left-shift / accumulator overflow.
struct Raster {
    Int64 value{}, increment{};

    void start(Point2I a, Point2I b) {
        increment = (Int64(b.x) - a.x) * 65536 / (b.y - a.y);
        value = Int64(a.x) * 65536 - increment + 65535;
    }

    Int32 next() {
        value += increment;
        return Int32(value >> 16);
    }
};

bool rasterize(std::span<const Point2I> polygon, TerrainVisibilityTable& table) {
    if (polygon.size() < 3) {
        return true;
    }
    const auto top =
        std::size_t(std::ranges::min_element(polygon, {}, &Point2I::y) - polygon.begin());
    const auto bottom = std::ranges::max(polygon, {}, &Point2I::y).y;
    table.startY = polygon[top].y;
    if (bottom == table.startY) {
        return true;
    }
    if (bottom - table.startY > 32768) {
        return false;
    }
    const auto next = [&](std::size_t& a, std::size_t& b, Int32 y, bool left) {
        for ([[maybe_unused]] auto visits : std::views::iota(std::size_t{0}, polygon.size())) {
            if (polygon[b].y > y) {
                return true;
            }
            a = b;
            b = (b + polygon.size() + (left ? -1 : 1)) % polygon.size();
        }
        return false;
    };
    auto la = top, lb = top, ra = top, rb = top;
    if (!next(la, lb, table.startY, true) || !next(ra, rb, table.startY, false)) {
        return false;
    }
    Raster left, right;
    left.start(polygon[la], polygon[lb]);
    right.start(polygon[ra], polygon[rb]);
    auto y = table.startY;
    while (y <= bottom) {
        const auto end = std::min(polygon[lb].y, polygon[rb].y);
        const auto count = end - y + (end == bottom ? 1 : 0);
        if (count <= 0) {
            return false;
        }
        std::ranges::for_each(std::views::iota(0, count), [&](auto) {
            table.rows.push_back({right.next() - 1, left.next()});
        });
        y += count;
        if (end == bottom) {
            break;
        }
        if (polygon[lb].y == end) {
            if (!next(la, lb, y, true)) {
                return false;
            }
            left.start(polygon[la], polygon[lb]);
        }
        if (polygon[rb].y == end) {
            if (!next(ra, rb, y, false)) {
                return false;
            }
            right.start(polygon[ra], polygon[rb]);
        }
    }
    return true;
}
}

std::optional<TerrainVisibility> terrainVisibility(Int32 scale,
                                                   UInt32 scanLevel,
                                                   const Point3F& camera,
                                                   const Point2F& heights,
                                                   const TerrainVisibilityView& view) {
    if (scale < 0 || scale > 16 || scanLevel > 7 || !finite(camera.x) || !finite(camera.y) ||
        !finite(camera.z) || !finite(heights.x) || !finite(heights.y) || heights.x > heights.y ||
        !rotation(view.cameraToGrid) || !finite(view.halfExtent.x) || !finite(view.halfExtent.y) ||
        view.halfExtent.x <= 0 || view.halfExtent.y <= 0 || !finite(view.viewportCenter.x) ||
        !finite(view.viewportCenter.y) || !finite(view.nearDistance) || view.nearDistance <= 0 ||
        !finite(view.visibleDistance) || view.visibleDistance <= view.nearDistance) {
        return {};
    }
    TerrainVisibility result;
    result.scanLevel = scanLevel;
    for (auto level : std::views::iota(UInt32{0}) | std::views::take_while([&](UInt32 level) {
                          return level <= scanLevel;
                      })) {
        result.tables[level].camera = {Int32(camera.x) >> (UInt32(scale) + level),
                                       Int32(camera.y) >> (UInt32(scale) + level)};
    }
    std::vector<Point2I> polygon;
    const auto quantize = [&](Flt32 x, Flt32 y, bool ortho) {
        if (!finite(x) || !finite(y)) {
            return false;
        }
        const auto rounding = scale > 1 ? 1 << (scale - 1) : 0;
        const Point2I p =
            ortho ? Point2I{Int32(x), Int32(y)}
                  : Point2I{(Int32(x) + rounding) >> scale, (Int32(y) + rounding) >> scale};
        if (std::abs(Int64(p.x)) > 16384 || std::abs(Int64(p.y)) > 16384) {
            return false;
        }
        polygon.push_back(p);
        return true;
    };
    if (!view.perspective) {
        const auto cell = Flt32(1U << scale), inverse = 1.f / cell;
        const auto left = (view.viewportCenter.x - view.halfExtent.x - cell) * inverse,
                   right = (view.viewportCenter.x + view.halfExtent.x + cell) * inverse,
                   top = (view.viewportCenter.y + view.halfExtent.y + cell) * inverse,
                   bottom = (view.viewportCenter.y - view.halfExtent.y - cell) * inverse;
        if (!quantize(left, top, true) || !quantize(right, top, true) ||
            !quantize(right, bottom, true) || !quantize(left, bottom, true)) {
            return {};
        }
    } else {
        const auto distance = view.visibleDistance, corner = Flt32(Flt64(distance) * .41421356),
                   expand = Flt32(1U << (scale + 1));
        std::vector<Point3F> points{{distance, corner, -camera.z},
                                    {distance, -corner, -camera.z},
                                    {corner, -distance, -camera.z},
                                    {-corner, -distance, -camera.z},
                                    {-distance, -corner, -camera.z},
                                    {-distance, corner, -camera.z},
                                    {-corner, distance, -camera.z},
                                    {corner, distance, -camera.z}};
        auto matrix = view.cameraToGrid;
        matrix.flags = RMat3F::Matrix_HasRotation;
        const auto rotate = [&](Point3F p) {
            Point3F out;
            m_mul(p, matrix, out);
            return out;
        };
        const auto inverse = 1.f / view.nearDistance;
        const auto dx = view.halfExtent.x * distance * inverse,
                   dy = view.halfExtent.y * distance * inverse;
        if (!finite(dx) || !finite(dy)) {
            return {};
        }
        // Project the frustum over the authored height range onto Z=0, then
        // clip left/right/top/bottom/front (the octagon supplies the back).
        // Both extrema translate by their negated height: d += normal.z * h.
        std::array<PlaneF, 5> planes{PlaneF(rotate({-expand, 0, 0}), rotate({distance, dx, 0})),
                                     PlaneF(rotate({expand, 0, 0}), rotate({-distance, dx, 0})),
                                     PlaneF(rotate({0, 0, expand}), rotate({0, dy, -distance})),
                                     PlaneF(rotate({0, 0, -expand}), rotate({0, dy, distance})),
                                     PlaneF(rotate({0, -expand, 0}), rotate({0, distance, 0}))};
        for (auto& plane : planes) {
            if (!plane.isVertical()) {
                plane.shiftZ(plane.z > 0 ? -heights.y : -heights.x);
            }
            points = clipFootprint(points, plane);
            if (points.empty()) {
                return result;
            }
            if (points.size() > 20) {
                return {};
            }
        }
        for (const auto& point : points) {
            if (!quantize(point.x, point.y, false)) {
                return {};
            }
        }
    }
    if (!rasterize(polygon, result.tables[0])) {
        return {};
    }
    for (auto level : std::views::iota(UInt32{1}) | std::views::take_while([&](UInt32 level) {
                          return level <= scanLevel;
                      })) {
        const auto& previous = result.tables[level - 1];
        auto& table = result.tables[level];
        if (previous.rows.empty()) {
            continue;
        }
        const Point2I offset{previous.camera.x - table.camera.x * 2,
                             previous.camera.y - table.camera.y * 2};
        table.startY = (previous.startY + offset.y) >> 1;
        const auto end = (previous.startY + offset.y + Int32(previous.rows.size()) - 1) >> 1;
        for (auto y : std::views::iota(table.startY) | std::views::take_while([&](auto y) {
                          return y <= end;
                      })) {
            const auto row = y * 2 - previous.startY - offset.y;
            auto span = previous.rows[std::size_t(std::max(row, 0))];
            if (row >= 0 && row + 1 < Int32(previous.rows.size())) {
                span.minimum = std::min(span.minimum, previous.rows[std::size_t(row + 1)].minimum);
                span.maximum = std::max(span.maximum, previous.rows[std::size_t(row + 1)].maximum);
            }
            table.rows.push_back(
                {(span.minimum + offset.x) >> 1, (span.maximum + offset.x + 1) >> 1});
        }
    }
    return result;
}
} // namespace opent1::world
