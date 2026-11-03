#include <opent1/gfx/render_limits.hpp>
#include <ranges>
#include <opent1/world/interior_links.hpp>
#include "collision_geometry.hpp"

namespace opent1::world {
namespace {
constexpr std::array<UInt32, 3> MinFaces{128, 64, 32}, MaxFaces{16, 8, 4};

bool overlap(Flt32 a, Flt32 b, Flt32 c, Flt32 d) {
    // Original testFaces: equal intervals, >one-unit partial overlap, or strict containment.
    return (std::abs(a - c) <= .001f && std::abs(b - d) <= .001f) ||
           (a <= c && b <= d && b > c + 1) || (b >= d && a >= c && a < d - 1) || (a > c && b < d) ||
           (a < c && b > d);
}

UInt32 abutting(const Box3F& a, const Box3F& b) {
    for (const auto axis : collision_detail::axes()) {
        const auto j = (axis + 1) % 3, k = (axis + 2) % 3;
        if (!overlap(a.fMin[j], a.fMax[j], b.fMin[j], b.fMax[j]) ||
            !overlap(a.fMin[k], a.fMax[k], b.fMin[k], b.fMax[k])) {
            continue;
        }
        if (std::abs(a.fMin[axis] - b.fMax[axis]) < .001f) {
            return MinFaces[axis];
        }
        if (std::abs(a.fMax[axis] - b.fMin[axis]) < .001f) {
            return MaxFaces[axis];
        }
    }
    return 0;
}

bool aligned(const TMat3F& a, const TMat3F& inverse) {
    TMat3F relative;
    m_mul(a, inverse, relative);
    return std::ranges::all_of(collision_detail::axes(), [&](auto i) {
        UInt32 ones{};
        return std::ranges::all_of(collision_detail::axes(),
                                   [&](auto j) {
                                       const auto v = std::abs(relative.m[i][j]);
                                       if (std::abs(v - 1) < 1e-5f) {
                                           ++ones;
                                           return true;
                                       }
                                       return v < 1e-5f;
                                   }) &&
               ones == 1;
    });
}

Box3F relativeBox(const InteriorLinkInstance& source, const TMat3F& inverse) {
    using collision_detail::point;
    const auto a = point(point(source.geometry->bounds.fMin, source.placement), inverse),
               b = point(point(source.geometry->bounds.fMax, source.placement), inverse);
    Box3F box;
    std::ranges::for_each(collision_detail::axes(), [&](auto i) {
        box.fMin[i] = std::min(a[i], b[i]);
        box.fMax[i] = std::max(a[i], b[i]);
    });
    return box;
}
}

std::optional<InteriorLinkGraph>
InteriorLinkGraph::prepare(std::span<const InteriorLinkInstance> instances) {
    using namespace collision_detail;
    if (instances.size() > gfx::renderLimits().images) {
        return {};
    }
    InteriorLinkGraph graph;
    graph.instances_.assign(instances.begin(), instances.end());
    graph.links_.resize(instances.size());
    for (auto& instance : graph.instances_) {
        if (!instance.geometry || !valid(instance.geometry->bounds) ||
            (instance.detailGeometry && !valid(instance.detailGeometry->bounds)) ||
            !rigid(instance.placement) || (instance.linkableFaces & ~255U)) {
            return {};
        }
        // Legacy DIS files can set the two unused low bits in this byte mask.
        // Only the six face bits participate in linking and visibility.
        instance.linkableFaces &= UInt32(InteriorFace::All);
        auto inverse = instance.placement;
        inverse.inverse();
        graph.inverses_.push_back(inverse);
    }
    instances = graph.instances_;
    std::size_t count{};
    for (auto i : std::views::iota(std::size_t{0}) | std::views::take_while([&](std::size_t i) {
                      return i < instances.size();
                  })) {
        for (auto j : std::views::iota(static_cast<std::size_t>(i + 1)) |
                          std::views::take_while([&](std::size_t j) {
                              return j < instances.size();
                          })) {
            if (!instances[i].linkableFaces || !instances[j].linkableFaces ||
                !aligned(instances[j].placement, graph.inverses_[i])) {
                continue;
            }
            const auto a = relativeBox(instances[j], graph.inverses_[i]),
                       b = relativeBox(instances[i], graph.inverses_[j]);
            if (!valid(a) || !valid(b)) {
                return {};
            }
            const auto fi = abutting(instances[i].geometry->bounds, a),
                       fj = abutting(instances[j].geometry->bounds, b);
            if (!(fi & instances[i].linkableFaces) || !(fj & instances[j].linkableFaces)) {
                continue;
            }
            if (gfx::renderLimits().links - count < 2) {
                return {};
            }
            count += 2;
            graph.links_[i].push_back({UInt32(j), fi});
            graph.links_[j].push_back({UInt32(i), fj});
        }
    }
    return graph;
}

std::span<const InteriorLink> InteriorLinkGraph::links(UInt32 index) const {
    return index < links_.size() ? std::span<const InteriorLink>(links_[index])
                                 : std::span<const InteriorLink>{};
}

std::optional<InteriorLinkVisibility>
InteriorLinkGraph::visibility(Point3F camera, std::span<const InteriorLinkRoot> roots) const {
    using namespace collision_detail;
    if (!finite(camera) || roots.size() > gfx::renderLimits().images ||
        !std::ranges::all_of(roots, [&](auto r) {
            return r.instance < instances_.size();
        })) {
        return {};
    }
    InteriorLinkVisibility result;
    std::vector<bool> visited(instances_.size());

    struct Visit {
        UInt32 index{}, raw{}, corrected{};
        std::size_t next{};
        bool contained{}, outside{};
    };

    std::vector<Visit> stack;
    stack.reserve(instances_.size());
    const auto enter = [&](UInt32 index, bool contained) {
        const auto& instance = instances_[index];
        const auto local = point(camera, inverses_[index]);
        const auto& box = instance.geometry->bounds;
        const auto& detail = instance.detailGeometry ? instance.detailGeometry : instance.geometry;
        const auto visible = detail->outsideVisibility(local);
        if (!visible) {
            return false;
        }
        auto corrected = visible->outsideBits;
        if (contained && !box.contains(local)) {
            std::ranges::for_each(axes(), [&](auto i) {
                if (local[i] < box.fMin[i] && std::abs(local[i] - box.fMin[i]) < 3) {
                    corrected |= MinFaces[i];
                }
                if (local[i] > box.fMax[i] && std::abs(local[i] - box.fMax[i]) < 3) {
                    corrected |= MaxFaces[i];
                }
            });
        }
        stack.push_back({index, visible->outsideBits, corrected, 0, box.contains(local), false});
        visited[index] = true;
        result.instances.push_back(index);
        return true;
    };
    for (const auto root : roots) {
        if (visited[root.instance] ||
            (instances_[root.instance].linked && !instances_[root.instance].forceRender &&
             !root.cameraContained)) {
            continue;
        }
        if (!enter(root.instance, root.cameraContained)) {
            return {};
        }
        if (instances_[root.instance].linked && !root.cameraContained) {
            stack.back().next = links_[root.instance].size();
        }
        while (!stack.empty()) {
            auto& visit = stack.back();
            const auto& edges = links_[visit.index];
            if (visit.next < edges.size()) {
                const auto edge = edges[visit.next++];
                if ((visit.corrected & edge.face) && !visited[edge.instance] &&
                    !enter(edge.instance, false)) {
                    return {};
                }
                continue;
            }
            auto outside = visit.outside;
            if (!instances_[visit.index].linked) {
                outside = visit.contained ? outside || visit.corrected != 0 : visit.raw != 0;
            }
            stack.pop_back();
            if (stack.empty()) {
                result.outsideVisible |= outside;
            } else {
                stack.back().outside |= outside;
            }
        }
    }
    return result;
}
} // namespace opent1::world
