#include <opent1/gfx/render_limits.hpp>
#include <opent1/world/interior_links.hpp>
#include "collision_geometry.hpp"
#include "interior_pvs.hpp"
#include <cstdio>
#include <cstdlib>

namespace opent1::world {
namespace {
bool contains(const Box3F& outer, const Box3F& inner) {
    return std::ranges::all_of(collision_detail::axes(), [&](auto i) {
        return outer.fMin[i] <= inner.fMin[i] && outer.fMax[i] >= inner.fMax[i];
    });
}
}

std::optional<InteriorContainerGraph>
InteriorContainerGraph::prepare(std::span<const InteriorContainerNode> nodes,
                                std::span<const InteriorLinkInstance> interiors) {
    if (nodes.empty() || nodes.size() > gfx::renderLimits().containers || nodes[0].parent != -1 ||
        nodes[0].interior != -1 || !nodes[0].open) {
        return {};
    }
    auto links = InteriorLinkGraph::prepare(interiors);
    if (!links) {
        return {};
    }
    InteriorContainerGraph graph;
    graph.links_ = std::move(*links);
    graph.nodes_.assign(nodes.begin(), nodes.end());
    graph.children_.resize(nodes.size());
    graph.interiorNodes_.assign(interiors.size(), ~UInt32{0});
    for (std::size_t i = 0; i < nodes.size(); ++i) {
        const auto& node = nodes[i];
        if (!node.bounds.isValid() || node.interior < -1 || (node.pvs && node.interior < 0) ||
            !std::ranges::all_of(
                collision_detail::axes(),
                [&](auto axis) {
                    return std::isfinite(node.bounds.fMin[axis]) &&
                           std::isfinite(node.bounds.fMax[axis]);
                })) {
            return {};
        }
        if (i) {
            if (node.parent < 0 || std::size_t(node.parent) >= i) {
                return {};
            }
            const auto parent = std::size_t(node.parent);
            // parent<i guarantees an acyclic tree. Traversal uses an owned
            // heap stack, so valid native hierarchies need no depth cutoff.
            if (!nodes[parent].open || !contains(nodes[parent].bounds, node.bounds) ||
                !(nodes[parent].mask & node.type)) {
                return {};
            }
            graph.children_[parent].push_back(UInt32(i));
        }
        if (node.interior >= 0) {
            const auto index = std::size_t(node.interior);
            if (!node.open || index >= interiors.size() ||
                graph.interiorNodes_[index] != ~UInt32{0}) {
                return {};
            }
            graph.interiorNodes_[index] = UInt32(i);
        }
    }
    if (std::ranges::find(graph.interiorNodes_, ~UInt32{0}) != graph.interiorNodes_.end()) {
        return {};
    }
    return graph;
}

std::optional<InteriorContainerVisibility>
InteriorContainerGraph::visibility(Point3F camera, UInt32 cameraType) const {
    using namespace collision_detail;
    if (nodes_.empty() || !finite(camera) || !contains(nodes_[0].bounds, Box3F{camera, camera})) {
        return {};
    }
    InteriorContainerVisibility result;
    std::vector<bool> visited(nodes_.size());
    enum class Stage { Camera, CameraResult, SelectContents, Contents, Links, LinkResult };

    struct Visit {
        UInt32 node{};
        Stage stage{Stage::Camera};
        std::size_t next{};
        bool contained{}, forced{}, outside{}, localContained{};
        UInt32 raw{}, corrected{};
        std::vector<UInt32> contents;
    };

    std::vector<Visit> stack;
    stack.reserve(nodes_.size());
    bool returned = false, failed = false;
    const auto enter = [&](UInt32 index, bool contained, bool forced) {
        const auto& node = nodes_[index];
        if (visited[index] ||
            (node.interior >= 0 && links_.instances_[std::size_t(node.interior)].linked &&
             !links_.instances_[std::size_t(node.interior)].forceRender && !contained && !forced)) {
            returned = false;
            return;
        }
        visited[index] = true;
        Visit visit;
        visit.node = index;
        visit.contained = contained;
        visit.forced = forced;
        stack.push_back(std::move(visit));
    };
    const auto finish = [&](bool value) {
        returned = value;
        stack.pop_back();
    };
    enter(0, true, false);
    std::size_t work = gfx::renderLimits().operations;
    while (!stack.empty()) {
        if (!work--) {
            return {};
        }
        auto& visit = stack.back();
        const auto& node = nodes_[visit.node];
        const auto& children = children_[visit.node];
        if (visit.stage == Stage::Camera) {
            visit.stage = Stage::SelectContents;
            if (visit.contained && node.open) {
                const auto child = std::ranges::find_if(children, [&](auto i) {
                    return nodes_[i].open && (nodes_[i].mask & cameraType) &&
                           contains(nodes_[i].bounds, Box3F{camera, camera});
                });
                // The original tests the first matching child, then its tag.
                if (child != children.end() && !visited[*child]) {
                    visit.stage = Stage::CameraResult;
                    enter(*child, true, false);
                    continue;
                }
            }
            if (node.interior < 0) {
                result.containers.push_back(visit.node);
            }
        } else if (visit.stage == Stage::CameraResult) {
            if (!returned) {
                finish(false);
                continue;
            }
            visit.stage = Stage::SelectContents;
            if (node.interior < 0) {
                result.containers.push_back(visit.node);
            }
        } else if (visit.stage == Stage::SelectContents) {
            if (node.pvs) {
                try {
                    spend(work, children.size());
                    std::vector<Box3F> bounds;
                    bounds.reserve(children.size());
                    for (auto child : children) {
                        bounds.push_back(nodes_[child].bounds);
                    }
                    const auto index = std::size_t(node.interior);
                    const auto selected =
                        interior_detail::visibleObjects(*links_.instances_[index].geometry,
                                                        camera,
                                                        bounds,
                                                        links_.inverses_[index],
                                                        work);
                    if (!selected) {
                        if (std::getenv("OPENT1_SCENE_DIAGNOSTICS")) {
                            std::fprintf(
                                stderr,
                                "PVS object selection failed at node %u interior %d children %zu\n",
                                visit.node,
                                node.interior,
                                bounds.size());
                        }
                        return {};
                    }
                    for (auto child : *selected) {
                        visit.contents.push_back(children[child]);
                    }
                } catch (const std::length_error&) {
                    return {};
                }
            }
            visit.stage = Stage::Contents;
        } else if (visit.stage == Stage::Contents) {
            const auto& contents = node.pvs ? visit.contents : children;
            if (visit.next < contents.size()) {
                const auto child = contents[visit.next++];
                if (!nodes_[child].renderEligible) {
                    continue;
                }
                if (nodes_[child].open) {
                    enter(child, false, false);
                } else {
                    result.containers.push_back(child);
                }
                continue;
            }
            if (node.interior < 0) {
                finish(true);
                continue;
            }
            result.containers.push_back(visit.node);
            if (visit.forced) {
                result.linkedContainers.push_back(visit.node);
            }
            const auto index = std::size_t(node.interior);
            const auto& instance = links_.instances_[index];
            const auto local = point(camera, links_.inverses_[index]);
            const auto& detail =
                instance.detailGeometry ? instance.detailGeometry : instance.geometry;
            auto outside = detail->outsideVisibility(local);
            if (!outside) {
                if (std::getenv("OPENT1_SCENE_DIAGNOSTICS")) {
                    std::fprintf(stderr,
                                 "Outside visibility failed at node %u interior %d position %g %g "
                                 "%g leaves %zu\n",
                                 visit.node,
                                 node.interior,
                                 double(local.x),
                                 double(local.y),
                                 double(local.z),
                                 detail->leaves.size());
                }
                failed = true;
                break;
            }
            visit.raw = visit.corrected = outside->outsideBits;
            visit.localContained = instance.geometry->bounds.contains(local);
            if (visit.contained && !visit.localContained) {
                std::ranges::for_each(axes(), [&](auto i) {
                    if (local[i] < instance.geometry->bounds.fMin[i] &&
                        std::abs(local[i] - instance.geometry->bounds.fMin[i]) < 3) {
                        visit.corrected |= std::array{128U, 64U, 32U}[i];
                    }
                    if (local[i] > instance.geometry->bounds.fMax[i] &&
                        std::abs(local[i] - instance.geometry->bounds.fMax[i]) < 3) {
                        visit.corrected |= std::array{16U, 8U, 4U}[i];
                    }
                });
            }
            visit.next = 0;
            visit.stage = Stage::Links;
        } else if (visit.stage == Stage::LinkResult) {
            visit.outside |= returned;
            visit.stage = Stage::Links;
        } else {
            const auto edges = links_.links(UInt32(node.interior));
            if (links_.instances_[std::size_t(node.interior)].linked && !visit.contained &&
                !visit.forced) {
                visit.next = edges.size();
            }
            if (visit.next < edges.size()) {
                const auto edge = edges[visit.next++];
                const auto child = interiorNodes_[edge.instance];
                if ((visit.corrected & edge.face) && !visited[child]) {
                    visit.stage = Stage::LinkResult;
                    enter(child, false, true);
                }
                continue;
            }
            bool outside = visit.outside;
            if (!links_.instances_[std::size_t(node.interior)].linked) {
                outside = visit.localContained ? outside || visit.corrected != 0 : visit.raw != 0;
            }
            finish(outside);
        }
    }
    if (failed) {
        return {};
    }
    result.outsideVisible = returned;
    return result;
}
} // namespace opent1::world
