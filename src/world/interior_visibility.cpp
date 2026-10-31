#include <opent1/world/interior_shape.hpp>
#include "collision_geometry.hpp"
#include "interior_pvs.hpp"
#include <ranges>

namespace opent1::world {
namespace {
bool inside(const PlaneF& plane, Point3F camera) {
    // TPlaneF::whichSide, including its subtract-before-compare rounding.
    return m_dot(plane, camera) - .00001f > -plane.d;
}

bool validBox(const Box3F& box) {
    return collision_detail::finite(box.fMin) && collision_detail::finite(box.fMax) &&
           box.fMin.x <= box.fMax.x && box.fMin.y <= box.fMax.y && box.fMin.z <= box.fMax.z;
}

bool sphereVisible(const Box3F& box, const InteriorViewCone& view) {
    Point3F center;
    m_mul((box.fMax + box.fMin) * .5f, view.objectToCamera, center);
    const auto radius = (box.fMax - box.fMin).lenf();
    if (!collision_detail::finite(center) || !std::isfinite(radius)) {
        throw std::invalid_argument("interior leaf sphere overflow");
    }
    const auto right = std::sqrt(1 + view.right * view.right),
               left = std::sqrt(1 + view.left * view.left),
               top = std::sqrt(1 + view.top * view.top),
               bottom = std::sqrt(1 + view.bottom * view.bottom);
    return !((center.x - right * radius) > center.y * view.right ||
             (center.x + left * radius) < center.y * view.left ||
             (center.z - top * radius) > center.y * view.top ||
             (center.z + bottom * radius) < center.y * view.bottom ||
             center.y + radius < view.nearDistance || center.y - radius > view.farDistance);
}
}

std::optional<UInt32>
interior_detail::leafAt(const InteriorGeometry& geometry, Point3F camera, std::size_t& work) {
    using namespace collision_detail;
    const auto& bounds = geometry.bounds;
    const auto& nodes = geometry.nodes;
    const auto& leaves = geometry.leaves;
    const auto& planes = geometry.planes;
    if (!finite(camera) || !validBox(bounds) || leaves.empty() || leaves.size() > 65536 ||
        nodes.size() > 65536 || planes.size() > 65536) {
        return {};
    }
    UInt32 leaf = (camera.x < bounds.fMin.x   ? 1U
                   : camera.x > bounds.fMax.x ? 2U
                                              : 0U) |
                  (camera.y < bounds.fMin.y   ? 4U
                   : camera.y > bounds.fMax.y ? 8U
                                              : 0U) |
                  (camera.z < bounds.fMin.z   ? 16U
                   : camera.z > bounds.fMax.z ? 32U
                                              : 0U);
    if (!leaf) {
        Int32 index{};
        UInt32 depth{};
        while (index >= 0) {
            spend(work);
            if (std::size_t(index) >= nodes.size() || ++depth > 128) {
                return {};
            }
            const auto& node = nodes[std::size_t(index)];
            if (node.plane >= planes.size() || !finite(planes[node.plane]) ||
                !std::isfinite(planes[node.plane].d)) {
                return {};
            }
            index = inside(planes[node.plane], camera) ? node.front : node.back;
        }
        const auto leafIndex = UInt64(-Int64(index) - 1);
        if (leafIndex >= leaves.size()) {
            return {};
        }
        leaf = UInt32(leafIndex);
    }
    if (leaf >= leaves.size()) {
        return {};
    }
    return leaf;
}

std::optional<UInt32> InteriorGeometry::leafAt(Point3F point) const {
    std::size_t work = collision_detail::MaxWork;
    return interior_detail::leafAt(*this, point, work);
}

std::optional<InteriorOutsideVisibility> InteriorGeometry::outsideVisibility(Point3F camera) const {
    const auto index = leafAt(camera);
    if (!index) {
        return {};
    }
    InteriorOutsideVisibility result{*index, 0};
    const auto& leaf = leaves[result.leaf];
    if (!leaf.solid && leaf.pvsCount) {
        if (leaf.pvsIndex > bits.size() || leaf.pvsCount > bits.size() - leaf.pvsIndex) {
            return {};
        }
        result.outsideBits = std::to_integer<UInt8>(bits[leaf.pvsIndex]) & 0xfcU;
    }
    return result;
}

std::optional<InteriorVisibility> InteriorGeometry::visibility(Point3F camera,
                                                               const InteriorViewCone* cone) const {
    using namespace collision_detail;
    const auto outside = outsideVisibility(camera);
    if (!outside || surfaces.size() > 65536) {
        return {};
    }
    if (cone && (!rigid(cone->objectToCamera) ||
                 !std::ranges::all_of(std::array{cone->left,
                                                 cone->right,
                                                 cone->bottom,
                                                 cone->top,
                                                 cone->nearDistance,
                                                 cone->farDistance},
                                      [](auto v) {
                                          return std::isfinite(v) && std::abs(v) <= 1e10f;
                                      }) ||
                 cone->left >= cone->right || cone->bottom >= cone->top ||
                 cone->nearDistance <= 0 || cone->nearDistance >= cone->farDistance)) {
        return {};
    }
    try {
        std::size_t work = MaxWork;
        InteriorVisibility result{outside->leaf, outside->outsideBits, {}};
        const auto& leaf = leaves[result.leaf];
        std::vector<bool> surfaceSet(surfaces.size()), planeSet(planes.size());
        const auto visit = [&](const InteriorLeaf& source) {
            spend(work, std::size_t(source.surfaceCount) + source.planeCount + 1);
            auto ss = bitIndices(source.surfaceIndex, source.surfaceCount, UInt32(surfaces.size()));
            auto ps = bitIndices(source.planeIndex, source.planeCount, UInt32(planes.size()));
            if (!ss || !ps) {
                return false;
            }
            spend(work, ss->size() + ps->size());
            std::ranges::for_each(*ss, [&](auto s) {
                surfaceSet[s] = true;
            });
            std::ranges::for_each(*ps, [&](auto p) {
                planeSet[p] = true;
            });
            return true;
        };
        if (!visit(leaf)) {
            return {};
        } // solid leaves still contribute their own authored surfaces
        if (!leaf.solid) {
            spend(work, leaf.pvsCount);
            auto pvs = bitIndices(leaf.pvsIndex, leaf.pvsCount, UInt32(leaves.size()));
            if (!pvs) {
                return {};
            }
            for (auto index : *pvs) {
                spend(work);
                if (index < 6) {
                    continue;
                } // reserved outside-face entries are always empty
                const auto& visible = leaves[index];
                if (visible.solid || !validBox(visible.bounds)) {
                    return {};
                }
                if ((!cone || sphereVisible(visible.bounds, *cone)) && !visit(visible)) {
                    return {};
                }
            }
        }
        for (std::size_t i = 0; i < planeSet.size(); ++i) {
            if (!planeSet[i]) {
                continue;
            }
            spend(work);
            if (!finite(planes[i]) || !std::isfinite(planes[i].d)) {
                return {};
            }
            planeSet[i] = inside(planes[i], camera);
        }
        for (std::size_t i = 0; i < surfaceSet.size(); ++i) {
            if (!surfaceSet[i]) {
                continue;
            }
            spend(work);
            const auto& surface = surfaces[i];
            if (surface.plane >= planes.size()) {
                return {};
            }
            if (surface.planeFront() == planeSet[surface.plane]) {
                result.surfaces.push_back(UInt32(i));
            }
        }
        return result;
    } catch (const std::length_error&) {
        return {};
    } catch (const std::invalid_argument&) {
        return {};
    }
}
} // namespace opent1::world
