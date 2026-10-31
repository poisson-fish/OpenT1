#include <opent1/world/terrain_collision.hpp>
#include "collision_geometry.hpp"

namespace opent1::world {
using namespace collision_detail;

std::optional<TerrainCollision>
TerrainCollision::prepare(UInt32 width,
                          UInt32 height,
                          Flt32 size,
                          std::span<const Flt32> heights,
                          std::span<const TerrainCollisionCell> cells) {
    if (!width || !height || width > 4096 || height > 4096 || UInt64(width) * height > 1'048'576 ||
        heights.size() != UInt64(width + 1) * (height + 1) ||
        cells.size() != UInt64(width) * height || !std::isfinite(size) || size < .0001f ||
        size * std::max(width, height) > 1e12f ||
        std::ranges::any_of(heights,
                            [](auto h) {
                                return !std::isfinite(h) || std::abs(h) > 1e12f;
                            }) ||
        std::ranges::any_of(cells, [](const auto& c) {
            return c.material < 0;
        })) {
        return {};
    }
    TerrainCollision result;
    result.width_ = width;
    result.height_ = height;
    result.size_ = size;
    result.heights_.assign(heights.begin(), heights.end());
    result.cells_.assign(cells.begin(), cells.end());
    const auto [low, high] = std::ranges::minmax_element(heights);
    result.bounds_ = {{0, 0, *low}, {size * width, size * height, *high}};
    return result;
}

std::array<std::array<Point3F, 3>, 2> TerrainCollision::triangles(UInt32 x, UInt32 y) const {
    const auto vertex = [&](UInt32 dx, UInt32 dy) {
        return Point3F{
            (x + dx) * size_, (y + dy) * size_, heights_[(y + dy) * (width_ + 1) + x + dx]};
    };
    const auto a = vertex(0, 0), b = vertex(1, 0), c = vertex(0, 1), d = vertex(1, 1);
    // Clockwise winding for the original m_normal/pointInTriangle convention.
    if ((x ^ y) & 1U) {
        return {{{a, c, b}, {b, c, d}}};
    }
    return {{{a, c, d}, {a, d, b}}};
}

std::optional<CollisionSurface> TerrainCollision::surface(Point2F p, bool hitHoles) const {
    check({p.x, p.y, 0}, {});
    if (p.x < 0 || p.y < 0 || p.x >= bounds_.fMax.x || p.y >= bounds_.fMax.y) {
        return {};
    }
    const auto x = std::min(width_ - 1, UInt32(std::floor(p.x / size_))),
               y = std::min(height_ - 1, UInt32(std::floor(p.y / size_)));
    const auto& cell = cells_[y * width_ + x];
    if (cell.hole && !hitHoles) {
        return {};
    }
    const auto u = p.x - x * size_, v = p.y - y * size_;
    const auto face = ((x ^ y) & 1U) ? std::size_t(size_ - u <= v) : std::size_t(u > v);
    const auto tri = triangles(x, y)[face];
    Point3F n;
    m_normal(tri[0], tri[1], tri[2], n);
    n = unit(n);
    CollisionSurface result;
    result.position = {
        p.x, p.y, tri[0].z - (n.x * (p.x - tri[0].x) + n.y * (p.y - tri[0].y)) / n.z};
    result.normal = n;
    result.material = cell.material;
    result.time = 1;
    return result;
}

bool TerrainCollision::sphere(Point3F center, Flt32 radius, CollisionSurfaceList& output) const {
    check(center, {}, radius);
    auto hit = surface({center.x, center.y});
    if (!hit) {
        return false;
    }
    const auto distance = std::abs(m_dot(center - hit->position, hit->normal));
    if (distance >= radius) {
        return false;
    }
    hit->distance = distance;
    CollisionSurfaceList result;
    append(result, *hit);
    return publish(output, std::move(result));
}

bool TerrainCollision::segment(
    Point3F start, Point3F end, Flt32 radius, bool holes, CollisionSurfaceList& output) const {
    check(start, end, radius);
    const auto delta = end - start;
    const auto loX = std::max(0.f, std::min(start.x, end.x) - radius),
               loY = std::max(0.f, std::min(start.y, end.y) - radius);
    const auto hiX = std::min(bounds_.fMax.x, std::max(start.x, end.x) + radius),
               hiY = std::min(bounds_.fMax.y, std::max(start.y, end.y) + radius);
    if (loX > hiX || loY > hiY || loX >= bounds_.fMax.x || loY >= bounds_.fMax.y) {
        return false;
    }
    const auto x0 = std::min(width_ - 1, UInt32(std::floor(loX / size_))),
               y0 = std::min(height_ - 1, UInt32(std::floor(loY / size_)));
    const auto x1 = std::min(width_ - 1, UInt32(std::floor(hiX / size_))),
               y1 = std::min(height_ - 1, UInt32(std::floor(hiY / size_)));
    CollisionSurfaceList result;
    CollisionSurface best;
    best.time = 2;
    std::size_t remaining = MaxWork;
    std::ranges::for_each(std::views::iota(y0, y1 + 1), [&](auto y) {
        std::ranges::for_each(std::views::iota(x0, x1 + 1), [&](auto x) {
            spend(remaining, 2);
            const auto& cell = cells_[y * width_ + x];
            if (cell.hole && !holes) {
                return;
            }
            std::ranges::for_each(triangles(x, y), [&](const auto& tri) {
                Point3F n;
                m_normal(tri[0], tri[1], tri[2], n);
                n = unit(n);
                const auto distance = m_dot(n, start - tri[0]), denom = m_dot(n, delta);
                if (denom == 0) {
                    return;
                }
                const auto offset = distance >= 0 ? radius : -radius;
                const auto t = (offset - distance) / denom;
                if (t < 0 || t > 1 || t >= best.time) {
                    return;
                }
                const auto center = start + delta * t, contact = center - n * offset;
                // Heightfield triangles never fold in XY. Test all three edge
                // signs, including collinear extensions of a triangle edge.
                std::array<Flt32, 3> signs{};
                std::ranges::for_each(axes(), [&](auto i) {
                    const auto a = tri[i], b = tri[(i + 1) % 3];
                    signs[i] = (b.x - a.x) * (contact.y - a.y) - (b.y - a.y) * (contact.x - a.x);
                });
                const auto tolerance = size_ * size_ * 1e-6f;
                if (!std::ranges::all_of(signs,
                                         [&](auto d) {
                                             return d >= -tolerance;
                                         }) &&
                    !std::ranges::all_of(signs, [&](auto d) {
                        return d <= tolerance;
                    })) {
                    return;
                }
                best = {};
                best.time = t;
                best.position = center;
                best.normal = n;
                best.material = cell.material;
            });
        });
    });
    if (best.time <= 1) {
        append(result, best);
    }
    return publish(output, std::move(result));
}

bool TerrainCollision::line(Point3F a, Point3F b, CollisionSurfaceList& output, bool holes) const {
    return segment(a, b, 0, holes, output);
}

bool TerrainCollision::tube(Point3F a,
                            Point3F b,
                            Flt32 radius,
                            CollisionSurfaceList& output) const {
    check(a, b, radius);
    if (a == b) {
        return sphere(a, radius, output);
    }
    return segment(a, b, radius, false, output);
}

bool TerrainCollision::box(const Box3F& box, const TMat3F& t, CollisionSurfaceList& output) const {
    if (!valid(box) || !rigid(t)) {
        throw std::invalid_argument("invalid terrain box");
    }
    const auto center = point((box.fMin + box.fMax) * .5f, t), radii = (box.fMax - box.fMin) * .5f;
    auto hit = surface({center.x, center.y});
    if (!hit) {
        return false;
    }
    Flt32 support{};
    std::ranges::for_each(axes(), [&](auto i) {
        support +=
            radii[i] * std::abs(m_dot(hit->normal, Point3F{t.m[i][0], t.m[i][1], t.m[i][2]}));
    });
    const auto penetration = support - m_dot(center - hit->position, hit->normal);
    if (penetration <= 0) {
        return false;
    }
    hit->distance = penetration;
    CollisionSurfaceList result;
    append(result, *hit);
    return publish(output, std::move(result));
}
} // namespace opent1::world
