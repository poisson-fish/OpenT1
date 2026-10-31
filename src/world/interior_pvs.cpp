#include "interior_pvs.hpp"
#include "collision_geometry.hpp"
#include <cstdio>
#include <cstdlib>

namespace opent1::world {
std::optional<std::vector<UInt32>> interior_detail::visibleObjects(const InteriorGeometry& geometry,
                                                                   Point3F camera,
                                                                   std::span<const Box3F> bounds,
                                                                   const TMat3F& inverse,
                                                                   std::size_t& work) {
    using namespace collision_detail;
    if (!rigid(inverse) || !finite(camera) || bounds.size() > 65536) {
        return {};
    }
    spend(work);
    const auto cameraLeaf = leafAt(geometry, point(camera, inverse), work);
    if (!cameraLeaf) {
        if (std::getenv("OPENT1_SCENE_DIAGNOSTICS")) {
            std::fprintf(stderr, "PVS camera leaf invalid\n");
        }
        return {};
    }
    // The native database returns before visiting any objects for a solid camera.
    const auto& leaf = geometry.leaves[*cameraLeaf];
    if (leaf.solid) {
        return std::vector<UInt32>{};
    }
    spend(work, leaf.pvsCount);
    const auto pvs =
        geometry.bitIndices(leaf.pvsIndex, leaf.pvsCount, UInt32(geometry.leaves.size()));
    if (!pvs) {
        if (std::getenv("OPENT1_SCENE_DIAGNOSTICS")) {
            std::fprintf(stderr,
                         "PVS bytes invalid leaf %u index %u count %u leaves %zu\n",
                         *cameraLeaf,
                         leaf.pvsIndex,
                         leaf.pvsCount,
                         geometry.leaves.size());
        }
        return {};
    }
    spend(work, pvs->size() + geometry.leaves.size() + bounds.size());
    // Native insertion prepends each object to one center-point leaf bucket.
    // Bucket zero holds all solid leaves; the others are leaf ID plus one.
    constexpr auto end = ~UInt32{0};
    std::vector<UInt32> heads(geometry.leaves.size() + 1, end), next(bounds.size());
    for (std::size_t i = 0; i < bounds.size(); ++i) {
        if (!valid(bounds[i])) {
            if (std::getenv("OPENT1_SCENE_DIAGNOSTICS")) {
                std::fprintf(stderr,
                             "PVS child bounds invalid at %zu: %g %g %g to %g %g %g\n",
                             i,
                             double(bounds[i].fMin.x),
                             double(bounds[i].fMin.y),
                             double(bounds[i].fMin.z),
                             double(bounds[i].fMax.x),
                             double(bounds[i].fMax.y),
                             double(bounds[i].fMax.z));
            }
            return {};
        }
        const auto center = (bounds[i].fMin + bounds[i].fMax) * .5f;
        const auto index = leafAt(geometry, point(center, inverse), work);
        if (!index) {
            if (std::getenv("OPENT1_SCENE_DIAGNOSTICS")) {
                std::fprintf(stderr, "PVS child leaf invalid at %zu\n", i);
            }
            return {};
        }
        auto& head = heads[geometry.leaves[*index].solid ? 0 : *index + 1];
        next[i] = head;
        head = UInt32(i);
    }
    std::vector<UInt32> result;
    const auto append = [&](std::size_t bucket) {
        for (auto i = heads[bucket]; i != end; i = next[i]) {
            spend(work);
            result.push_back(i);
        }
    };
    for (auto index : *pvs) {
        if (index >= 43) {
            append(std::size_t(index) + 1);
        }
    }
    append(0);
    const auto outside =
        leaf.pvsCount ? std::to_integer<UInt8>(geometry.bits[leaf.pvsIndex]) & 0xfcU : 0U;
    if (outside) {
        for (std::size_t i = 0; i < std::min(std::size_t{43}, geometry.leaves.size()); ++i) {
            append(i + 1);
        }
    }
    return result;
}

std::optional<std::vector<UInt32>> InteriorGeometry::visibleObjects(
    Point3F camera, std::span<const Box3F> worldBounds, const TMat3F& inverse) const {
    try {
        std::size_t work = collision_detail::MaxWork;
        return interior_detail::visibleObjects(*this, camera, worldBounds, inverse, work);
    } catch (const std::length_error&) {
        return {};
    }
}
} // namespace opent1::world
