#include "test_registry.hpp"
#include "test_support.hpp"
#include "ts_collision_test_support.hpp"
#include <opent1/game/game_app.hpp>
#include <opent1/game/item.hpp>
#include <opent1/world/world_collision_sim.hpp>
#include <cmath>

namespace {
using namespace opent1;
using namespace opent1::game;

void addRoom(sim::SimManager& manager, const TMat3F& placement) {
    // A thin, solid wall and a separate floor exercise both impact and settling.
    const std::array<PlaneF, 2> planes{{{1, 0, 0, -2}, {1, 0, 0, -2.02f}}};
    const std::array<world::InteriorCollisionNode, 2> nodes{{{0, 1, -1}, {1, -1, -2}}};
    const std::array<world::InteriorCollisionLeaf, 2> leaves{{{false, {}}, {true, {0, 1}}}};
    const std::array<world::InteriorCollisionSurface, 2> surfaces{{
        {0, false, -1, {{2, -8, -4}, {2, 8, -4}, {2, 8, 12}, {2, -8, 12}}},
        {1, true, -1, {{2.02f, -8, -4}, {2.02f, -8, 12}, {2.02f, 8, 12}, {2.02f, 8, -4}}},
    }};
    auto wall = world::InteriorCollision::prepare(planes, nodes, leaves, surfaces);
    CHECK(wall);
    CHECK(manager.addObject(std::make_unique<world::SimWorldCollider>(
        world::SimInteriorCollisionImage(std::make_shared<const world::InteriorCollision>(*wall)),
        Box3F{{2, -8, -4}, {2.02f, 8, 12}},
        placement)));

    const std::array<PlaneF, 1> floorPlanes{{{0, 0, 1, 0}}};
    const std::array<world::InteriorCollisionNode, 1> floorNodes{{{0, -1, -2}}};
    const std::array<world::InteriorCollisionLeaf, 2> floorLeaves{{{false, {}}, {true, {0}}}};
    const std::array<world::InteriorCollisionSurface, 1> floorSurfaces{{
        {0, true, -1, {{-8, -8, 0}, {-8, 8, 0}, {8, 8, 0}, {8, -8, 0}}},
    }};
    auto floor =
        world::InteriorCollision::prepare(floorPlanes, floorNodes, floorLeaves, floorSurfaces);
    CHECK(floor);
    CHECK(manager.addObject(std::make_unique<world::SimWorldCollider>(
        world::SimInteriorCollisionImage(std::make_shared<const world::InteriorCollision>(*floor)),
        Box3F{{-8, -8, -4}, {8, 8, 0}},
        placement)));
}

void thrownPacksHitWallsAndFall() {
    for (const bool ghost : {false, true}) {
        for (const Flt32 yaw : {0.f, .65f}) {
            FearGameApp app;
            CHECK(app.init());
            CHECK(app.fearPlugin().createServerWorld({}, true));
            auto& globals = app.fearPlugin().serverGlobals();
            auto shape = test::ts_collision::cube();
            // Pack-sized bounds, with the origin at its bottom, like Lootbag.
            shape->bounds = {{-.2f, -.32f, 0}, {.2f, .32f, .46f}};
            shape->center = {0, 0, .23f};
            shape->radius = .442f;
            CHECK(app.resources().add<world::TsShape>("wall-pack.dts", std::move(shape)));
            auto* data = dynamic_cast<ItemData*>(
                globals.dbm->createDataBlock("WallPack", DataBlockManager::ItemDataType));
            CHECK(data);
            data->shape = app.resources().load<world::TsShape>("wall-pack.dts");
            data->mass = 1;
            data->friction = data->elasticity = data->drag = 0;
            const TMat3F placement(EulerF{0, 0, yaw}, {100, 200, 300});
            addRoom(*globals.manager, placement);
            TMat3F inverse = placement;
            inverse.inverse();

            for (const Flt32 speed : {20.f, 150.f}) {
                auto owned = std::make_unique<Item>();
                owned->bindDataBlockManager(globals.dbm);
                owned->setDatFileName("WallPack");
                if (ghost)
                    CHECK(owned->setNetFlags(owned->netFlags() | sim::SimNetObject::IsGhost));
                auto* pack = owned.get();
                CHECK(globals.manager->addObject(std::move(owned)));
                CHECK(!pack->isCollideable()); // Pickup triggers still hit walls themselves.
                Point3F start, velocity;
                m_mul(Point3F{1.5f, 0, 4}, placement, start);
                m_mul(Point3F{speed, 0, 0}, static_cast<const RMat3F&>(placement), velocity);
                pack->setPos(start);
                pack->setLinearVelocity(velocity);
                CHECK(pack->addForce(sim::MovementGravityForce, {0, 0, -20}, true));
                const auto process = [&](UInt32 time) {
                    if (ghost)
                        pack->clientProcess(time);
                    else
                        pack->serverProcess(time);
                };
                const auto localPosition = [&] {
                    Point3F local;
                    m_mul(pack->getPos(), inverse, local);
                    return local;
                };
                process(32);
                CHECK(localPosition().x < 2);
                sim::SimMovement::MovementInfo afterImpact;
                CHECK(pack->testPosition(pack->getTransform(), afterImpact));
                CHECK(std::abs(m_dot(pack->getLinearVelocity(), velocity) / speed) < .01f);
                CHECK(!pack->isAtRest());
                for (UInt32 time = 64; time <= 1504; time += 32) {
                    process(time);
                    CHECK(localPosition().x < 2);
                    CHECK(localPosition().z > -.001f);
                }
                CHECK(localPosition().z < .1f);
                CHECK(pack->isAtRest());
                sim::SimMovement::MovementInfo settled;
                CHECK(pack->testPosition(pack->getTransform(), settled));
                pack->deleteObject();
            }
        }
    }
}
} // namespace

int opent1_test_thrown_pack_wall_collision() {
    return opent1::test::run(thrownPacksHitWallsAndFall);
}

OPENT1_REGISTER_TEST("game", "thrown_pack_wall_collision", opent1_test_thrown_pack_wall_collision);
