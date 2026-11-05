#include "test_registry.hpp"
#include "test_support.hpp"
#include <opent1/world/terrain_visibility.hpp>
#include <cmath>

namespace {
using namespace opent1;

void rotatingFootprintCoverage() {
    struct Pose {
        Point3F camera;
        EulerF rotation;
    };

    // These views used to lose both intersections at a nearly coplanar corner
    // of the footprint. A small rotation then restored the missing foreground.
    const std::array poses{
        Pose{{0x1.3b072p+9f, 0x1.6ab2dep+11f, 0x1.ce0802p+7f}, {-0x1.075ecp-1f, 0, 0x1.4fcf7ap+1f}},
        Pose{{0x1.50648cp+12f, 0x1.e1bae8p+11f, 0x1.1de8dap+6f},
             {-0x1.4a618p-3f, 0, 0x1.1be2d2p+1f}}};
    unsigned checked{};
    for (const auto& pose : poses) {
        for (int frame = -8; frame <= 8; ++frame) {
            world::TerrainVisibilityView view;
            view.nearDistance = .5f;
            view.halfExtent = {.5f, .3f};
            auto rotation = pose.rotation;
            rotation.z += Flt32(frame) * .00001f;
            view.cameraToGrid.set(rotation);
            const auto footprint =
                world::terrainVisibility(3, 6, pose.camera, {-10.0775f, 701}, view);
            CHECK(footprint);
            // Independently intersect interior screen rays with several ground
            // heights. Every hit safely inside the far sphere must be admitted
            // at every level of the selection hierarchy.
            for (int y = -5; y <= 5; ++y) {
                for (int x = -9; x <= 9; ++x) {
                    Point3F ray;
                    m_mul(Point3F{Flt32(x) * .1f, 1, Flt32(y) * .1f}, view.cameraToGrid, ray);
                    for (Flt32 height : {-10.0775f, 65.f, 400.f, 701.f}) {
                        const auto t = (height - pose.camera.z) / ray.z;
                        const auto hit = pose.camera + ray * t;
                        if (!std::isfinite(t) || t <= view.nearDistance ||
                            (hit - pose.camera).len() > 1000)
                            continue;
                        for (UInt32 level = 0; level <= footprint->scanLevel; ++level) {
                            const auto size = Flt32(8U << level);
                            CHECK(footprint->tables[level].visible(
                                Int32(std::floor(hit.x / size)), Int32(std::floor(hit.y / size))));
                            ++checked;
                        }
                    }
                }
            }
        }
    }
    CHECK(checked > 10000);
}
}

int terrainVisibilityCoverage() {
    return opent1::test::run(rotatingFootprintCoverage);
}

OPENT1_REGISTER_TEST("world", "terrain_visibility_coverage", terrainVisibilityCoverage);
