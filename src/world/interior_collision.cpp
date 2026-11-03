#include <opent1/world/interior_collision.hpp>
#include <ranges>
#include "collision_geometry.hpp"
#include <functional>
#include <numeric>
#include <memory_resource>

namespace opent1::world {
using namespace collision_detail;

namespace {
constexpr Flt32 Precision = .00001f;

Flt32 distance(const PlaneF& p, Point3F v) {
    return m_dot(p, v) + p.d;
}

Int32 side(const PlaneF& p, Point3F v) {
    const auto d = distance(p, v);
    return d > Precision ? 1 : d < -Precision ? -1 : 0;
}

// Original ITR convex-polygon circle test (radiusSquared is already planar).
bool polygonSphere(std::span<const Point3F> vertices,
                   Point3F normal,
                   Point3F center,
                   Flt32 radiusSquared) {
    UInt32 outside{};
    std::ranges::for_each(std::views::iota(std::size_t{0}, vertices.size()), [&](auto i) {
        const auto a = vertices[(i + vertices.size() - 1) % vertices.size()], b = vertices[i];
        Point3F edgeNormal;
        m_cross(b - a, normal, edgeNormal);
        const auto dot = m_dot(center - a, edgeNormal);
        if (dot < 0) {
            outside |= UInt32{1} << i;
        }
    });
    if (!outside) {
        return true;
    }
    bool rejected = false;
    std::ranges::for_each(std::views::iota(std::size_t{0}, vertices.size()), [&](auto i) {
        if (!(outside & (UInt32{1} << i))) {
            return;
        }
        const auto a = vertices[(i + vertices.size() - 1) % vertices.size()], b = vertices[i];
        Point3F edgeNormal;
        m_cross(b - a, normal, edgeNormal);
        const auto dot = m_dot(center - a, edgeNormal);
        if (Flt64(dot) * dot > Flt64(radiusSquared) * m_dot(edgeNormal, edgeNormal)) {
            rejected = true;
        }
    });
    if (rejected) {
        return false;
    }
    return std::ranges::any_of(std::views::iota(std::size_t{0}, vertices.size()), [&](auto i) {
        if (!(outside & (UInt32{1} << i))) {
            return false;
        }
        const auto a = vertices[(i + vertices.size() - 1) % vertices.size()], b = vertices[i],
                   edge = b - a;
        const auto dot = m_dot(center - a, edge), length2 = m_dot(edge, edge);
        if (dot < 0) {
            return m_dot(center - a, center - a) < radiusSquared;
        }
        if (dot > length2) {
            return m_dot(center - b, center - b) < radiusSquared;
        }
        return true;
    });
}

struct Face {
    std::pmr::vector<Point3F> vertices;
    Int32 plane{-1};
    Point3F normal;

    explicit Face(std::pmr::memory_resource* memory) : vertices(memory) {}
};

using Polyhedron = std::pmr::vector<Face>;

// Fixed per-query-thread scratch, never a retained expanded collision mesh.
// Depth-first traversal rewinds each node's temporaries; overflow uses ordinary
// allocations with their original lifetimes. A nested query gets an independent
// allocator so it cannot overwrite an active query's storage.
class BspScratch : public std::pmr::memory_resource {
    struct Storage {
        std::array<std::byte, 32 * 1024> bytes;
        bool busy{};
    };

    static Storage& storage() {
        static thread_local Storage value;
        return value;
    }

    bool acquired_{!storage().busy};
    std::size_t used_{};

    void* do_allocate(std::size_t bytes, std::size_t alignment) override {
        if (acquired_) {
            auto& buffer = storage().bytes;
            void* next = buffer.data() + used_;
            auto available = buffer.size() - used_;
            if (std::align(alignment, bytes, next, available)) {
                used_ = std::size_t(static_cast<std::byte*>(next) - buffer.data()) + bytes;
                return next;
            }
        }
        return std::pmr::new_delete_resource()->allocate(bytes, alignment);
    }

    void do_deallocate(void* p, std::size_t bytes, std::size_t alignment) override {
        const auto address = reinterpret_cast<std::uintptr_t>(p);
        const auto base = reinterpret_cast<std::uintptr_t>(storage().bytes.data());
        if (!acquired_ || address < base || address - base >= storage().bytes.size())
            std::pmr::new_delete_resource()->deallocate(p, bytes, alignment);
    }

    bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override {
        return this == &other;
    }

public:
    BspScratch() {
        if (acquired_)
            storage().busy = true;
    }

    ~BspScratch() override {
        if (acquired_)
            storage().busy = false;
    }

    struct Mark {
        BspScratch& memory;
        std::size_t offset{memory.used_};

        ~Mark() {
            memory.used_ = offset;
        }
    };
};

enum class ClipResult { Empty, Unchanged, Clipped };

// Clip a convex polyhedron, retaining the BSP cap as a collision face. Every
// temporary vertex visit is charged before publication of caller results.
ClipResult clip(const Polyhedron& input,
                const PlaneF& plane,
                UInt32 planeIndex,
                bool front,
                std::size_t& work,
                Polyhedron& result) {
    const auto sign = front ? 1.f : -1.f;
    auto* memory = result.get_allocator().resource();
    std::pmr::vector<Point3F> cap(memory);
    bool positive = false, negative = false;
    std::ranges::for_each(input, [&](const auto& face) {
        spend(work, face.vertices.size());
        std::ranges::for_each(face.vertices, [&](auto v) {
            const auto d = distance(plane, v) * sign;
            positive |= d > Precision;
            negative |= d < -Precision;
        });
    });
    if (!positive) {
        return ClipResult::Empty;
    } // Zero-volume tangency is excluded.
    if (!negative) {
        return ClipResult::Unchanged;
    }
    std::ranges::for_each(input, [&](const auto& face) {
        Face out(memory);
        out.plane = face.plane;
        out.normal = face.normal;
        auto a = face.vertices.back();
        auto da = distance(plane, a) * sign;
        std::ranges::for_each(face.vertices, [&](auto b) {
            spend(work);
            const auto db = distance(plane, b) * sign;
            if ((da >= 0) != (db >= 0)) {
                const auto p = a + (b - a) * (da / (da - db));
                out.vertices.push_back(p);
                if (std::ranges::none_of(cap, [&](auto q) {
                        spend(work);
                        return (p - q).len() < Precision;
                    })) {
                    cap.push_back(p);
                }
            }
            if (db >= 0) {
                out.vertices.push_back(b);
            }
            a = b;
            da = db;
        });
        if (out.vertices.size() >= 3) {
            result.push_back(std::move(out));
        }
    });
    if (cap.size() >= 3) {
        const auto center = std::accumulate(cap.begin(), cap.end(), Point3F{}) / Flt32(cap.size());
        const auto u = unit(cap.front() - center);
        Point3F v;
        m_cross(plane, u, v);
        std::ranges::sort(cap, [&](auto a, auto b) {
            return std::atan2(m_dot(a - center, v), m_dot(a - center, u)) <
                   std::atan2(m_dot(b - center, v), m_dot(b - center, u));
        });
        Face face(memory);
        face.vertices = std::move(cap);
        face.plane = Int32(planeIndex);
        face.normal = Point3F(plane) * (-sign);
        result.push_back(std::move(face));
    }
    return ClipResult::Clipped;
}
}

std::optional<InteriorCollision>
InteriorCollision::prepare(std::span<const PlaneF> planes,
                           std::span<const InteriorCollisionNode> nodes,
                           std::span<const InteriorCollisionLeaf> leaves,
                           std::span<const InteriorCollisionSurface> surfaces,
                           InteriorSurfacePolicy policy) {
    if (policy != InteriorSurfacePolicy::Strict && policy != InteriorSurfacePolicy::Native) {
        return {};
    }
    if (planes.empty() || nodes.empty() || leaves.empty() || planes.size() > 65536 ||
        nodes.size() > 65536 || leaves.size() > 65536 || surfaces.size() > 65536 ||
        std::ranges::any_of(planes, [](const auto& p) {
            return !finite(p) || !std::isfinite(p.d) || std::abs(p.d) > 1e12f ||
                   std::abs(m_dot(p, p) - 1) > .0001f;
        })) {
        return {};
    }
    const auto validChild = [&](Int32 child) {
        return child >= 0 ? std::size_t(child) < nodes.size()
                          : UInt64(-Int64(child) - 1) < leaves.size();
    };
    if (std::ranges::any_of(nodes, [&](const auto& n) {
            return n.plane >= planes.size() || !validChild(n.front) || !validChild(n.back);
        })) {
        return {};
    }
    // A bounded tree (shared leaves allowed), rejecting cycles, repeated nodes,
    // unreachable nodes and excessive recursion before a snapshot is published.
    std::vector<bool> seen(nodes.size());
    const std::function<bool(Int32, std::size_t)> visit = [&](Int32 index, std::size_t depth) {
        if (index < 0) {
            return true;
        }
        if (depth > 128 || seen[std::size_t(index)]) {
            return false;
        }
        seen[std::size_t(index)] = true;
        const auto& node = nodes[std::size_t(index)];
        return visit(node.front, depth + 1) && visit(node.back, depth + 1);
    };
    if (!visit(0, 1) || !std::ranges::all_of(seen, [](bool value) {
            return value;
        })) {
        return {};
    }
    std::size_t work = MaxWork;
    for (const auto& s : surfaces) {
        if (s.plane >= planes.size() || s.material < -1 || s.vertices.size() < 3 ||
            s.vertices.size() > 255 || s.vertices.size() * s.vertices.size() > work) {
            return {};
        }
        work -= s.vertices.size() * s.vertices.size();
        const auto normal = Point3F(planes[s.plane]) * (s.planeFront ? 1.f : -1.f);
        if (std::ranges::any_of(s.vertices, [](auto p) {
                return !finite(p);
            })) {
            return {};
        }
        // The original ITR collider consumes authored polygons as-is. Native
        // files include rounding, collinear edges and concave outlines; BSP
        // planes still define the solid volume. Never project or repair them.
        if (policy == InteriorSurfacePolicy::Native) {
            continue;
        }
        if (std::ranges::any_of(s.vertices, [&](auto p) {
                return std::abs(distance(planes[s.plane], p)) > .001f;
            })) {
            return {};
        }
        bool area = false;
        for (auto i : std::views::iota(std::size_t{0}, s.vertices.size())) {
            const auto a = s.vertices[i], b = s.vertices[(i + 1) % s.vertices.size()];
            Point3F edgeNormal;
            m_cross(b - a, normal, edgeNormal);
            if ((b - a).len() <= Precision || std::ranges::any_of(s.vertices, [&](auto p) {
                    return m_dot(p - a, edgeNormal) < -.001f;
                })) {
                return {};
            }
            area |= std::ranges::any_of(s.vertices, [&](auto p) {
                return m_dot(p - a, edgeNormal) > .00001f;
            });
        }
        if (!area) {
            return {};
        }
    }
    for (const auto& leaf : leaves) {
        if (leaf.surfaces.size() > work || std::ranges::any_of(leaf.surfaces, [&](auto s) {
                return s >= surfaces.size();
            })) {
            return {};
        }
        work -= leaf.surfaces.size();
    }
    InteriorCollision result;
    result.planes_.assign(planes.begin(), planes.end());
    result.nodes_.assign(nodes.begin(), nodes.end());
    result.leaves_.assign(leaves.begin(), leaves.end());
    result.surfaces_.assign(surfaces.begin(), surfaces.end());
    return result;
}

bool InteriorCollision::line(Point3F a, Point3F b, CollisionSurfaceList& output) const {
    check(a, b);
    CollisionSurfaceList result;
    std::size_t work = MaxWork;
    const std::function<bool(Int32, Int32, Point3F, Point3F, Flt32, Flt32)> visit =
        [&](Int32 index, Int32 entered, Point3F start, Point3F end, Flt32 lo, Flt32 hi) {
            spend(work);
            if (index >= 0) {
                const auto& node = nodes_[std::size_t(index)];
                const auto& plane = planes_[node.plane];
                const auto sa = side(plane, start), sb = side(plane, end);
                if (!sa && !sb) { // Original ignores coplanar leaf children.
                    return (node.back >= 0 && visit(node.back, entered, start, end, lo, hi)) ||
                           (node.front >= 0 && visit(node.front, entered, start, end, lo, hi));
                }
                if (sa >= 0 && sb >= 0) {
                    return visit(node.front, entered, start, end, lo, hi);
                }
                if (sa <= 0 && sb <= 0) {
                    return visit(node.back, entered, start, end, lo, hi);
                }
                const auto t =
                    distance(plane, start) / (distance(plane, start) - distance(plane, end));
                const auto p = start + (end - start) * t;
                const auto middle = lo + (hi - lo) * t;
                return visit(sa > 0 ? node.front : node.back, entered, start, p, lo, middle) ||
                       visit(
                           sa > 0 ? node.back : node.front, Int32(node.plane), p, end, middle, hi);
            }
            const auto leafIndex = std::size_t(-Int64(index) - 1);
            const auto& leaf = leaves_[leafIndex];
            if (!leaf.solid) {
                return false;
            }
            CollisionSurface hit;
            hit.time = lo;
            hit.position = start;
            hit.surface = entered;
            hit.part = Int32(leafIndex);
            hit.material = -1;
            if (entered < 0) {
                hit.normal = unit(end - start);
            } else {
                const auto& plane = planes_[std::size_t(entered)];
                hit.normal = plane;
                if (side(plane, end) != -1) {
                    hit.normal = -hit.normal;
                }
                (void)std::ranges::any_of(leaf.surfaces, [&](auto s) {
                    spend(work, surfaces_[s].vertices.size());
                    const auto& surface = surfaces_[s];
                    if (surface.plane != UInt32(entered) ||
                        !polygonSphere(surface.vertices,
                                       Point3F(plane) * (surface.planeFront ? 1.f : -1.f),
                                       start,
                                       0)) {
                        return false;
                    }
                    hit.material = surface.material;
                    return true;
                });
            }
            append(result, hit);
            return true;
        };
    visit(0, -1, a, b, 0, 1);
    return publish(output, std::move(result));
}

bool InteriorCollision::tube(Point3F a,
                             Point3F b,
                             Flt32 radius,
                             CollisionSurfaceList& output) const {
    check(a, b, radius);
    if (radius == 0) {
        return line(a, b, output);
    }
    const auto delta = b - a;
    Flt32 best = 2;
    Int32 bestPlane = -1;
    std::size_t work = MaxWork;
    const std::function<void(Int32, Int32, Flt32, Flt32)> visit =
        [&](Int32 index, Int32 entered, Flt32 lo, Flt32 hi) {
            spend(work);
            if (index < 0) {
                if (leaves_[std::size_t(-Int64(index) - 1)].solid && lo < best) {
                    best = lo;
                    bestPlane = entered;
                }
                return;
            }
            const auto& n = nodes_[std::size_t(index)];
            const auto& p = planes_[n.plane];
            const auto da = distance(p, a), dv = m_dot(p, delta), db = da + dv;
            const auto sa = da >= radius    ? 1
                            : da <= -radius ? -1
                                            : 0,
                       sb = db >= radius    ? 1
                            : db <= -radius ? -1
                                            : 0;
            const auto crossing = [&](Flt32 offset) {
                return std::clamp((da + offset) / -dv, lo, hi);
            };
            switch (sa * 3 + sb) {
                case -4:
                    visit(n.back, entered, lo, hi);
                    break;
                case -3:
                    visit(n.back, entered, lo, hi);
                    visit(n.front, Int32(n.plane), crossing(radius), hi);
                    break;
                case -1:
                    visit(n.front, Int32(n.plane), lo, lo);
                    visit(n.back, entered, lo, hi);
                    break;
                case 4:
                    visit(n.front, entered, lo, hi);
                    break;
                case 3:
                    visit(n.front, entered, lo, hi);
                    visit(n.back, Int32(n.plane), crossing(-radius), hi);
                    break;
                case 1:
                    visit(n.back, Int32(n.plane), lo, lo);
                    visit(n.front, entered, lo, hi);
                    break;
                case 0:
                    visit(n.back, Int32(n.plane), lo, hi);
                    visit(n.front, Int32(n.plane), lo, hi);
                    break;
                case -2:
                    visit(n.back, entered, lo, crossing(-2 * radius));
                    visit(n.front, Int32(n.plane), crossing(radius), hi);
                    break;
                case 2:
                    visit(n.front, entered, lo, crossing(2 * radius));
                    visit(n.back, Int32(n.plane), crossing(-radius), hi);
                    break;
            }
        };
    visit(0, -1, 0, 1);
    if (best > 1) {
        return false;
    }
    CollisionSurface hit;
    hit.time = best;
    hit.position = a + delta * best;
    hit.distance = radius;
    hit.material = -1;
    hit.normal = bestPlane < 0 ? unit(delta) : Point3F(planes_[std::size_t(bestPlane)]);
    if (bestPlane >= 0 && distance(planes_[std::size_t(bestPlane)], a) < 0) {
        hit.normal = -hit.normal;
    }
    CollisionSurfaceList result;
    append(result, hit);
    return publish(output, std::move(result));
}

bool InteriorCollision::sphere(Point3F center, Flt32 radius, CollisionSurfaceList& output) const {
    check(center, {}, radius);
    CollisionSurfaceList result;
    std::vector<UInt32> path;
    std::vector<bool> found(surfaces_.size());
    std::size_t work = MaxWork;
    const std::function<void(Int32)> visit = [&](Int32 index) {
        spend(work);
        if (index >= 0) {
            const auto& n = nodes_[std::size_t(index)];
            const auto d = distance(planes_[n.plane], center);
            const bool intersects = d < radius && d > -radius;
            if (intersects) {
                path.push_back(n.plane);
            }
            if (d > -radius) {
                visit(n.front);
            }
            if (d < radius) {
                visit(n.back);
            }
            if (intersects) {
                path.pop_back();
            }
            return;
        }
        const auto& leaf = leaves_[std::size_t(-Int64(index) - 1)];
        if (!leaf.solid) {
            return;
        }
        std::ranges::for_each(path, [&](auto planeIndex) {
            std::ranges::for_each(leaf.surfaces, [&](auto s) {
                spend(work);
                const auto& surface = surfaces_[s];
                if (found[s] || surface.plane != planeIndex) {
                    return;
                }
                const auto& plane = planes_[planeIndex];
                const auto d = distance(plane, center);
                if (std::abs(d) >= radius) {
                    return;
                }
                // Preserve the original signed projection policy, including d<0.
                const auto projected = center + Point3F(plane) * (d > 0 ? -d : d);
                const auto normal = Point3F(plane) * (surface.planeFront ? 1.f : -1.f);
                spend(work, surface.vertices.size() * 2);
                if (!polygonSphere(surface.vertices, normal, projected, radius * radius - d * d)) {
                    return;
                }
                CollisionSurface hit;
                hit.position = projected;
                hit.normal = normal;
                hit.distance = surface.planeFront ? d : -d;
                hit.material = surface.material;
                hit.surface = Int32(s);
                append(result, hit);
                found[s] = true;
            });
        });
    };
    visit(0);
    return publish(output, std::move(result));
}

bool InteriorCollision::box(const Box3F& box, const TMat3F& t, CollisionSurfaceList& output) const {
    if (!valid(box) || !rigid(t)) {
        throw std::invalid_argument("invalid interior box");
    }
    if (box.fMin.x == box.fMax.x || box.fMin.y == box.fMax.y || box.fMin.z == box.fMax.z) {
        return false;
    }
    const auto vertex = [&](std::size_t i) {
        return point({i & 1 ? box.fMax.x : box.fMin.x,
                      i & 2 ? box.fMax.y : box.fMin.y,
                      i & 4 ? box.fMax.z : box.fMin.z},
                     t);
    };
    constexpr std::array<std::array<std::size_t, 4>, 6> indices{{{{0, 2, 3, 1}},
                                                                 {{4, 5, 7, 6}},
                                                                 {{0, 1, 5, 4}},
                                                                 {{2, 6, 7, 3}},
                                                                 {{0, 4, 6, 2}},
                                                                 {{1, 3, 7, 5}}}};
    BspScratch memory;
    Polyhedron initial(&memory);
    std::ranges::for_each(indices, [&](const auto& face) {
        Face f(&memory);
        std::ranges::transform(face, std::back_inserter(f.vertices), vertex);
        initial.push_back(std::move(f));
    });
    Point3F up;
    m_mul(Point3F{0, 0, 1}, static_cast<const RMat3F&>(t), up);
    CollisionSurfaceList result;
    std::vector<bool> emitted(planes_.size());
    std::size_t work = MaxWork;
    bool solid = false;
    Flt32 step{};
    const std::function<void(Int32, const Polyhedron&)> visit = [&](Int32 index,
                                                                    const Polyhedron& poly) {
        spend(work);
        if (poly.empty()) {
            return;
        }
        if (index >= 0) {
            const BspScratch::Mark mark{memory};
            const auto& n = nodes_[std::size_t(index)];
            Polyhedron front(&memory), back(&memory);
            const auto f = clip(poly, planes_[n.plane], n.plane, true, work, front);
            visit(n.front, f == ClipResult::Unchanged ? poly : front);
            const auto b = clip(poly, planes_[n.plane], n.plane, false, work, back);
            visit(n.back, b == ClipResult::Unchanged ? poly : back);
            return;
        }
        const auto& leaf = leaves_[std::size_t(-Int64(index) - 1)];
        if (!leaf.solid) {
            return;
        }
        solid = true;
        std::ranges::for_each(poly, [&](const auto& face) {
            spend(work, face.vertices.size());
            std::ranges::for_each(face.vertices, [&](auto p) {
                step = std::max(step, m_dot(up, p - t.p));
            });
            if (face.plane < 0 || emitted[std::size_t(face.plane)]) {
                return;
            }
            const auto found = std::ranges::find_if(leaf.surfaces, [&](auto s) {
                spend(work);
                return surfaces_[s].plane == UInt32(face.plane);
            });
            if (found == leaf.surfaces.end()) {
                return;
            }
            CollisionSurface hit;
            hit.position = face.vertices.front();
            hit.normal = face.normal;
            hit.distance = 1;
            hit.surface = Int32(*found);
            hit.material = surfaces_[*found].material;
            append(result, hit);
            emitted[std::size_t(face.plane)] = true;
        });
    };
    visit(0, initial);
    result.contained = solid && result.surfaces.empty();
    if (!result.contained) {
        result.stepVector = {0, 0, step};
    }
    return publish(output, std::move(result));
}
} // namespace opent1::world
