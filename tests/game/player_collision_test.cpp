#include "test_registry.hpp"
#include "test_support.hpp"
#include <opent1/game/player_collision.hpp>
#include <cmath>
#include <limits>

namespace {
using namespace opent1;

void overlappingPlayers() {
    game::PlayerCollisionImage first, second;
    first.sphere.center = second.sphere.center = {0, 0, 1};
    first.sphere.radius = second.sphere.radius = 1;
    first.transform = second.transform = TMat3F(EulerF{}, {100, 200, 300});

    const auto contact = [](const auto& moving, const auto& other) {
        CollisionSurfaceList surfaces;
        CHECK(sim::SimCollisionImage::test(moving, other, surfaces));
        CHECK(surfaces.surfaces.size() == 1);
        const auto surface = surfaces.surfaces.front();
        CHECK(std::isfinite(surface.normal.x) && std::isfinite(surface.normal.y) &&
              std::isfinite(surface.normal.z));
        CHECK(std::abs(surface.normal.len() - 1) < 1e-6f);
        return surface;
    };

    // Multiple goblins can spawn at the same position. Both query orders must
    // report a valid contact without trying to normalize a zero vector.
    CHECK(contact(first, second).distance < .001f);
    CHECK(contact(second, first).distance < .001f);

    first.transform.identity();
    for (const auto separation : {1e-4f, 1e-20f, std::numeric_limits<float>::denorm_min()}) {
        second.transform = TMat3F(EulerF{}, {separation, 0, 0});
        (void)contact(first, second);
        (void)contact(second, first);
    }

    // Ordinary overlapping players retain the direction between their centers.
    second.transform = TMat3F(EulerF{}, {1, 0, 0});
    CHECK(contact(first, second).normal == Point3F(-1, 0, 0));
    CHECK(contact(second, first).normal == Point3F(1, 0, 0));
    second.transform = TMat3F(EulerF{}, {3, 0, 0});
    CollisionSurfaceList surfaces;
    CHECK(!sim::SimCollisionImage::test(first, second, surfaces));
}
} // namespace

int opent1_test_overlapping_player_collision() {
    return opent1::test::run(overlappingPlayers);
}

OPENT1_REGISTER_TEST("game", "overlapping_player_collision", opent1_test_overlapping_player_collision);
