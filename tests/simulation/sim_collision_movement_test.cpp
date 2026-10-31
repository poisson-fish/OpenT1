#include "sim_test_support.hpp"
#include <opent1/sim/sim_world_game.hpp>
#include <algorithm>
#include <cmath>
#include <limits>
#include <ranges>
#include <thread>

namespace {
TMat3F at(float x, float y = 0, float z = 0) {
    TMat3F m;
    m.p = {x, y, z};
    m.flags |= TMat3F::Matrix_HasTranslation;
    return m;
}

bool near(float a, float b) {
    return std::abs(a - b) < 1e-4f;
}

template <class F> void throws(F&& f) {
    bool caught = false;
    try {
        f();
    } catch (const std::exception&) {
        caught = true;
    }
    CHECK(caught);
}

struct Body : SimMovement {
    std::shared_ptr<SimCollisionSphereImage> image = std::make_shared<SimCollisionSphereImage>();
    int notifications{};
    std::vector<SimAddress> others;

    bool processQuery(SimQuery& q) override {
        if (auto* query = dynamic_cast<SimCollisionImageQuery*>(&q)) {
            image->transform = getTransform();
            query->images = {image};
            return true;
        }
        return false;
    }

    bool processEvent(const SimEvent& e) override {
        if (const auto* collision = dynamic_cast<const SimMovementCollisionEvent*>(&e)) {
            ++notifications;
            others.push_back(collision->object);
            return true;
        }
        return SimMovement::processEvent(e);
    }

    using SimMovement::processSurface;
};

struct BlockedBody : Body {
    bool failWithException{};
    UInt32 observedMask{};

    bool processCollision(MovementInfo&) override {
        observedMask = getCollisionMask();
        if (failWithException) {
            throw std::runtime_error("blocked movement");
        }
        return true;
    }
};

struct DeletingBody : Body {
    bool processSurface(Point3F, const CollisionSurface&, UInt32) override {
        deleteObject();
        return false;
    }
};

struct EnterHook : SimDefaultOpenContainer {
    std::function<void(SimContainer&)> hook;

    void enterContainer(SimContainer& object) override {
        SimDefaultOpenContainer::enterContainer(object);
        if (hook) {
            hook(object);
        }
    }
};

struct Provider : SimContainer {
    std::vector<std::shared_ptr<SimCollisionImage>> values;
    std::function<void()> callback;
    int events{};

    bool processQuery(SimQuery& q) override {
        if (auto* query = dynamic_cast<SimCollisionImageQuery*>(&q)) {
            query->images = values;
            if (callback) {
                auto f = callback;
                f();
            }
            return true;
        }
        return false;
    }

    bool processEvent(const SimEvent& e) override {
        if (dynamic_cast<const SimMovementCollisionEvent*>(&e)) {
            ++events;
            return true;
        }
        return SimContainer::processEvent(e);
    }
};

Provider* floor(SimManager& manager, SimContainer& root, float z) {
    auto* provider = add<Provider>(manager);
    provider->setType(TerrainObjectType);
    CHECK(provider->setBoundingBox({{-100, -100, z - 1}, {100, 100, z + 1}}));
    auto callback = std::make_shared<const SimCollisionBackendImage::Collider>(
        [z](const SimCollisionImage& image, const TMat3F& mat, CollisionSurfaceList& list) {
            const auto* sphere = dynamic_cast<const SimCollisionSphereImage*>(&image);
            if (!sphere) {
                return false;
            }
            Point3F center;
            m_mul(sphere->center, mat, center);
            if (center.z - sphere->radius >= z) {
                return false;
            }
            CollisionSurface surface;
            surface.position = {center.x, center.y, z};
            surface.normal = {0, 0, 1};
            surface.distance = std::abs(center.z - z);
            list.surfaces.push_back(surface);
            return true;
        });
    provider->values.push_back(
        std::make_shared<SimCollisionBackendImage>(SimCollisionImage::Terrain, callback));
    CHECK(root.addObject(*provider));
    return provider;
}

void primitives() {
    SimCollisionSphereImage a, b;
    CollisionSurfaceList list;
    a.radius = b.radius = 1;
    CHECK(SimCollisionImage::test(a, b, list));
    CHECK(list.surfaces.front().normal == Point3F(0, 0, 1));
    b.transform = at(2);
    CHECK(SimCollisionImage::test(a, b, list));
    b.transform = at(2.001f);
    const auto old = list.surfaces.front().normal;
    CHECK(!SimCollisionImage::test(a, b, list));
    CHECK(list.surfaces.front().normal == old);
    b.radius = -1;
    throws([&] {
        SimCollisionImage::test(a, b, list);
    });
    b.radius = 1;
    b.transform.m[0][0] = 2;
    throws([&] {
        SimCollisionImage::test(a, b, list);
    });
    SimCollisionBoxImage box;
    box.box = {{3, 4, 5}, {5, 6, 7}};
    a.center = {3, 5, 6};
    a.radius = .5f;
    CHECK(SimCollisionImage::test(a, box, list));
    CHECK(list.surfaces.front().normal == Point3F(-1, 0, 0));
    a.center = {2.5f, 5, 6};
    CHECK(!SimCollisionImage::test(a, box, list)); // tangency excluded
    SimCollisionBoxImage box2;
    box2.box = box.box;
    CHECK(SimCollisionImage::test(box, box2, list));
    box2.transform = at(3);
    CHECK(!SimCollisionImage::test(box, box2, list));
    box2.transform = TMat3F(EulerF(.1f, .2f, .3f), {});
    box2.box = {{-.5f, -.5f, -.5f}, {.5f, .5f, .5f}};
    box.box = box2.box;
    CHECK(SimCollisionImage::test(box, box2, list));
    CHECK(near(list.surfaces.front().normal.len(), 1));
    SimCollisionLineImage line;
    line.start = {-3, 0, 0};
    line.end = {3, 0, 0};
    a.center = {};
    CHECK(SimCollisionImage::test(line, a, list));
    CHECK(near(list.surfaces.front().position.x, -.5f));
    line.end = {-2, 0, 0};
    CHECK(!SimCollisionImage::test(line, a, list)); // ray beyond endpoint is no longer a hit
    line.start = {1, 0, 0};
    line.end = {2, 0, 0};
    CHECK(!SimCollisionImage::test(line, a, list)); // behind origin
    line.start = {-3, 0, 0};
    line.end = {3, 0, 0};
    box.transform = at(1);
    CHECK(SimCollisionImage::test(line, box, list));
    CHECK(list.surfaces.front().normal == Point3F(-1, 0, 0));
    SimCollisionTubeImage tube;
    tube.start = {-3, 0, 0};
    tube.end = {3, 0, 0};
    tube.radius = .25f;
    CHECK(SimCollisionImage::test(tube, a, list));
    CHECK(near(list.surfaces.front().position.x, -.75f));
    CHECK(SimCollisionImage::test(tube, box, list));
    auto bad = std::make_shared<const SimCollisionBackendImage::Collider>(
        [](const auto&, const auto&, auto&) {
            return true;
        });
    SimCollisionBackendImage backend(SimCollisionImage::Terrain, bad);
    throws([&] {
        SimCollisionImage::test(a, backend, list);
    });
    bad = std::make_shared<const SimCollisionBackendImage::Collider>(
        [](const auto&, const auto&, auto& out) {
            out.surfaces.resize(4097);
            return true;
        });
    SimCollisionBackendImage oversized(SimCollisionImage::Interior, bad);
    throws([&] {
        SimCollisionImage::test(a, oversized, list);
    });
}

void databases() {
    std::ranges::for_each(std::views::iota(0, 3), [](int mode) {
        SimManager manager;
        auto* root = add<SimDefaultOpenContainer>(manager, RootContainerId);
        root->uninstallDatabase();
        if (mode == 0) {
            CHECK(root->installDatabase(std::make_unique<SimContainerDatabase>()));
        }
        if (mode == 1) {
            CHECK(root->installDatabase(std::make_unique<SimXDB>()));
        }
        if (mode == 2) {
            CHECK(root->installDatabase(std::make_unique<SimXYDB>()));
        }
        std::vector<SimContainer*> bodies;
        std::ranges::for_each(std::views::iota(0, 600), [&](int i) {
            auto* object = add<SimContainer>(manager);
            object->setType(UInt32{1} << unsigned(i % 9));
            const float x = float(i % 29 - 14), y = float((i * 7) % 37 - 18), z = float(i % 11 - 5);
            CHECK(object->setBoundingBox({{x, y, z}, {x + 1, y + 2, z + 3}}));
            CHECK(root->addObject(*object));
            bodies.push_back(object);
        });
        std::ranges::for_each(std::views::iota(0, 200), [&](int i) {
            auto* body = bodies[std::size_t(i)];
            auto box = body->getBoundingBox();
            box += Point3F{100, 0, 0};
            CHECK(body->setBoundingBox(box));
        });
        std::ranges::for_each(std::views::iota(0, 300), [&](int i) {
            SimContainerQuery q;
            q.id = bodies[std::size_t(i)]->getId();
            q.mask = i % 2 ? ~UInt32{0} : UInt32{1} << unsigned(i % 9);
            q.box = {{float(i % 31 - 15), -10, -4}, {float(i % 31 - 9), 10, 4}};
            SimContainerList found;
            root->findIntersections(q, found);
            std::vector<SimObjectId> expected, actual;
            std::ranges::for_each(bodies, [&](auto* b) {
                if (b->getId() != q.id && (b->getType() & q.mask) &&
                    b->getBoundingBox().overlaps(q.box)) {
                    expected.push_back(b->getId());
                }
            });
            std::ranges::transform(found, std::back_inserter(actual), [](auto* b) {
                return b->getId();
            });
            std::ranges::sort(actual);
            std::ranges::sort(expected);
            CHECK(actual == expected);
        });
        auto* nested = add<SimDefaultOpenContainer>(manager);
        CHECK(nested->setBoundingBox({{-4, -4, -4}, {4, 4, 4}}));
        CHECK(root->addObject(*nested));
        auto* leaf = add<SimContainer>(manager);
        CHECK(leaf->setBoundingBox({{-1, -1, -1}, {1, 1, 1}}));
        CHECK(root->addObject(*leaf));
        CHECK(leaf->getContainer() == nested);
        CHECK(!nested->addObject(*root));
        CHECK(leaf->setBoundingBox({{6, 6, 6}, {7, 7, 7}}));
        CHECK(leaf->getContainer() == root);
        CHECK(leaf->setBoundingBox({{-1, -1, -1}, {1, 1, 1}}));
        CHECK(leaf->getContainer() == nested);
        CHECK(nested->setBoundingBox({{-1, -1, -1}, {0, 0, 0}}));
        CHECK(leaf->getContainer() == root);
        CHECK(nested->setBoundingBox({{-4, -4, -4}, {4, 4, 4}}));
        CHECK(root->updateObject(*leaf));
        CHECK(leaf->getContainer() == nested);
        CHECK(nested->deleteObject());
        CHECK(leaf->getContainer() == root);
        auto* foreign = add<SimContainer>(manager);
        SimManager other;
        auto* unrelated = add<SimContainer>(other);
        Box3F invalid;
        invalid.fMin = {1, 1, 1};
        invalid.fMax = {0, 0, 0};
        CHECK(!root->addObject(*unrelated));
        CHECK(!root->setBoundingBox(invalid));
        CHECK(!root->removeObject(*foreign));
        root->uninstallDatabase();
        CHECK(leaf->getContainer() == nullptr);
    });
    // Spatial parents also work without a manager and clean up in either order.
    auto root = std::make_unique<SimDefaultOpenContainer>();
    auto child = std::make_unique<SimContainer>();
    CHECK(root->addObject(*child));
    root.reset();
    CHECK(child->getContainer() == nullptr);
}

void movement() {
    SimWorld world;
    auto& manager = world.manager();
    auto& root = dynamic_cast<SimContainer&>(*manager.findObject(RootContainerId));
    auto* ground = floor(manager, root, 0);
    auto* body = add<Body>(manager);
    body->setType(1U << 7);
    CHECK(body->setPosition(at(0, 0, 2), true));
    CHECK(root.addObject(*body));
    CHECK(body->setTimeSlice(1));
    body->setMovementFlags(0);
    CHECK(body->setLinearVelocity({0, 0, -2}));
    CHECK(body->stepPosition(at(0, 0, 0), .1f, .25f));
    CHECK(body->didContact());
    CHECK(body->getLinearPosition().z >= 1);
    manager.advanceToTime(0);
    CHECK(body->notifications == 1);
    CHECK(ground->events == 1);
    // Without an image, both 1.11 position entry points must still move.
    SimMovement noImage;
    CHECK(noImage.setPosition(at(1, 2, 3)));
    CHECK(noImage.stepPosition(at(4, 5, 6), .1f, 1));
    CHECK(noImage.getLinearPosition() == Point3F(4, 5, 6));
    CHECK(!body->stepPosition(at(0), 0, 1));
    CHECK(!body->stepPosition(at(0), 1, .5f));
    const auto previous = body->getTransform();
    auto nan = at(0);
    nan.p.x = std::numeric_limits<float>::quiet_NaN();
    CHECK(!body->setPosition(nan, true));
    CHECK(body->getLinearPosition() == previous.p);
    CHECK(!body->setMass(-1));
    CHECK(body->setMass(0));
    CHECK(!body->applyImpulse({1, 2, 3}));
    CHECK(body->setMass(1));
    CHECK(body->setLinearVelocity({0, 0, -2}));
    CHECK(body->collide(body->getLinearPosition(), {0, 0, 1}));
    CHECK(near(body->getLinearVelocity().z, 0));
    CHECK(body->setDisplacementVelocity({}));
    CHECK(!body->setDensity(0));
    CHECK(!body->setTimeSlice(-1));
    CHECK(!body->setContainerCurrent(nan.p, 1));
    CHECK(body->setContainerCurrent({}, 1));
    // Material IDs use an explicit value lookup rather than truncated pointers.
    body->setMaterialLookup(std::make_shared<const SimMovement::MaterialLookup>([](Int32 id) {
        CHECK(id == 123);
        return SimMovement::Material{.5f, .25f};
    }));
    CHECK(body->setLinearVelocity({1, 0, -1}));
    CHECK(body->setFriction(1));
    CHECK(body->setElasticity(1));
    CollisionSurface surface;
    surface.material = 123;
    CHECK(body->processSurface(
        {0, 0, 1}, surface, SimMovement::UseFriction | SimMovement::UseElasticity));
    CHECK(body->getLinearVelocity().x > .4f && body->getLinearVelocity().x < .6f);
    CHECK(body->getLinearVelocity().z > 0);
    // Displacement uses the 1.11 static-mask assignment, even with old mask=0.
    auto* platform = add<Body>(manager);
    platform->setType(1U << 9);
    CHECK(platform->setPosition(at(10, 0, 2), true));
    CHECK(root.addObject(*platform));
    auto* rider = add<Body>(manager);
    rider->setType(1U << 7);
    CHECK(rider->setPosition(at(10, 0, 4), true));
    CHECK(root.addObject(*rider));
    CHECK(platform->addContact(*rider));
    rider->setCollisionMask(0);
    auto old = platform->getTransform();
    CHECK(platform->setPosition(at(11, 0, 2), true));
    CHECK(platform->displaceObjects(old));
    CHECK(near(rider->getLinearPosition().x, 11));
    CHECK(rider->getCollisionMask() == 0);
    CHECK(!platform->addContact(*rider));
    CHECK(platform->removeContact(*rider));
    CHECK(platform->displaceObjects(platform->getTransform()));
    // More than the legacy fixed 30 contact slots, with no type mutations.
    std::vector<Body*> riders;
    std::ranges::for_each(std::views::iota(0, 48), [&](int i) {
        auto* r = add<Body>(manager);
        r->setType(1U << 7);
        CHECK(r->setPosition(at(20 + float(i) * 3, 0, 5), true));
        CHECK(root.addObject(*r));
        CHECK(platform->addContact(*r));
        riders.push_back(r);
    });
    old = platform->getTransform();
    CHECK(platform->setPosition(at(12, 0, 2), true));
    CHECK(platform->displaceObjects(old));
    std::ranges::for_each(std::views::iota(std::size_t{0}, riders.size()), [&](auto i) {
        CHECK(near(riders[i]->getLinearPosition().x, 21 + float(i) * 3));
        CHECK(riders[i]->getType() == (1U << 7));
    });
    platform->clearContacts();
    auto* blocked = add<BlockedBody>(manager);
    blocked->setType(1U << 7);
    CHECK(blocked->setPosition(at(0, 0, 2), true));
    CHECK(root.addObject(*blocked));
    blocked->setCollisionMask(0);
    CHECK(platform->addContact(*blocked));
    CHECK(platform->setPosition(at(12, 0, 0), true));
    CHECK(platform->setLinearVelocity({0, 0, -2}));
    const auto before = blocked->getTransform();
    old = at(12, 0, 2);
    CHECK(!platform->displaceObjects(old));
    CHECK(blocked->getLinearPosition() == before.p);
    CHECK(blocked->getCollisionMask() == 0);
    CHECK(blocked->observedMask == (TerrainObjectType | InteriorObjectType));
    blocked->failWithException = true;
    throws([&] {
        platform->displaceObjects(old);
    });
    CHECK(blocked->getLinearPosition() == before.p);
    CHECK(blocked->getCollisionMask() == 0);
}

void callbacksAndLos() {
    SimWorld world;
    auto& manager = world.manager();
    auto& root = dynamic_cast<SimContainer&>(*manager.findObject(RootContainerId));
    auto* provider = add<Provider>(manager);
    CHECK(provider->setBoundingBox({{1, -1, -1}, {3, 1, 1}}));
    auto sphere = std::make_shared<SimCollisionSphereImage>();
    sphere->transform = at(2);
    provider->values = {sphere};
    CHECK(root.addObject(*provider));
    SimContainerQuery q;
    q.box.fMin = {0, 0, 0};
    q.box.fMax = {5, 0, 0};
    SimCollisionInfo hit;
    CHECK(root.findLOS(q, hit));
    CHECK(hit.object.resolve(manager) == provider);
    sphere->trigger = true;
    CHECK(!root.findLOS(q, hit));
    CHECK(root.findLOS(q, hit, SimCollisionImageQuery::Editor));
    SimCollisionInfoList all;
    CHECK(root.findLOS(q, all));
    CHECK(all.size() == 1);
    sphere->trigger = false;
    const auto address = provider->address();
    provider->callback = [provider] {
        provider->deleteObject();
    };
    CHECK(!root.findLOS(q, hit));
    CHECK(!address.resolve(manager));
    CHECK(near(sphere->radius, 1));
    provider = add<Provider>(manager);
    CHECK(provider->setBoundingBox({{1, -1, -1}, {3, 1, 1}}));
    CHECK(root.addObject(*provider));
    provider->values.resize(21, sphere);
    throws([&] {
        root.findLOS(q, hit);
    });
    provider->values = {nullptr};
    throws([&] {
        root.findLOS(q, hit);
    });
    provider->values = {sphere};
    provider->callback = [] {
        throw std::runtime_error("query failure");
    };
    throws([&] {
        root.findLOS(q, hit);
    });
    provider->callback = {};
    CHECK(root.findLOS(q, hit));
    // Triggers notify once per move; the event cannot resolve recycled IDs.
    provider->values = {sphere};
    sphere->trigger = true;
    auto* body = add<Body>(manager);
    body->setType(1U << 7);
    CHECK(body->setPosition(at(0), true));
    CHECK(root.addObject(*body));
    CHECK(body->stepPosition(at(4), .1f, .25f));
    manager.advanceToTime(0);
    CHECK(provider->events == 1);
    SimMovementCollisionEvent event;
    event.object = body->address();
    auto id = body->getId();
    CHECK(body->deleteObject());
    auto* replacement = add<Body>(manager);
    CHECK(replacement->setId(id));
    CHECK(event.getObject(manager) == nullptr);
    provider->callback = {};
    CHECK(provider->deleteObject());
    floor(manager, root, 0);
    auto* deleting = add<DeletingBody>(manager);
    deleting->setType(1U << 7);
    CHECK(deleting->setPosition(at(0, 0, 2), true));
    CHECK(root.addObject(*deleting));
    const auto gone = deleting->address();
    CHECK(deleting->setLinearVelocity({0, 0, -1}));
    CHECK(!deleting->stepPosition(at(0, 0, 0), .1f, .25f));
    CHECK(!gone.resolve(manager));
    auto* hook = add<EnterHook>(manager);
    CHECK(hook->setBoundingBox({{20, 20, 20}, {30, 30, 30}}));
    CHECK(root.addObject(*hook));
    hook->hook = [](SimContainer& object) {
        object.deleteObject();
    };
    auto* victim = add<SimContainer>(manager);
    CHECK(victim->setBoundingBox({{21, 21, 21}, {22, 22, 22}}));
    const auto victimAddress = victim->address();
    CHECK(!root.addObject(*victim));
    CHECK(!victimAddress.resolve(manager));
}

struct LightReceiver : SimDefaultOpenContainer {
    std::unordered_map<UInt64, int> counts;
    std::function<void()> callback;
    int updates{}, decrements{};

    bool processEvent(const SimEvent& e) override {
        if (const auto* update = dynamic_cast<const SimDynamicLightUpdateEvent*>(&e)) {
            ++counts[update->light.generation];
            ++updates;
            if (callback) {
                auto f = callback;
                f();
            }
            return true;
        }
        if (const auto* dec = dynamic_cast<const SimDynamicLightDecRefCntEvent*>(&e)) {
            --counts[dec->light.generation];
            ++decrements;
            return true;
        }
        return SimDefaultOpenContainer::processEvent(e);
    }
};

void lights() {
    SimWorld world;
    auto& manager = world.manager();
    auto& root = dynamic_cast<SimContainer&>(*manager.findObject(RootContainerId));
    auto* receiver = add<LightReceiver>(manager);
    CHECK(receiver->setBoundingBox({{-2, -2, -2}, {2, 2, 2}}));
    CHECK(root.addObject(*receiver));
    auto* light = add<SimDynamicLight>(manager);
    const auto generation = light->generation();
    CHECK(receiver->counts[generation] == 1);
    CHECK(light->setRadius(2));
    CHECK(receiver->counts[generation] == 1);
    CHECK(light->getIntensity(0) == 255);
    CHECK(light->getIntensity(4) == 0);
    CHECK(!light->setRadius(0));
    CHECK(!light->setRadius(-1));
    CHECK(!light->setRadius(std::numeric_limits<float>::infinity()));
    throws([&] {
        (void)light->getIntensity(-1);
    });
    CHECK(light->setPosition({10, 0, 0}));
    CHECK(receiver->counts[generation] == 0);
    CHECK(light->setPosition({0, 0, 0}));
    CHECK(receiver->counts[generation] == 1);
    CHECK(light->setTolerances(1, 1));
    const auto updates = receiver->updates;
    CHECK(light->setPosition({.5f, 0, 0}));
    CHECK(receiver->updates == updates);
    receiver->callback = [&] {
        CHECK(!light->setRadius(3));
        light->deleteObject();
    };
    light->updateLight();
    CHECK(receiver->counts[generation] == 0);
    receiver->callback = {};
    light = add<SimDynamicLight>(manager);
    const auto other = light->generation();
    CHECK(receiver->counts[other] == 1);
    receiver->callback = [] {
        throw std::runtime_error("light callback");
    };
    throws([&] {
        light->updateLight();
    });
    CHECK(receiver->counts[other] == 1);
    receiver->callback = {};
    CHECK(light->deleteObject());
    CHECK(receiver->counts[other] == 0);
}
}

int opent1_test_sim_collision_movement() {
    globalAssert().setFlags(kAssertNoDialog | kAssertNoLog);
    globalAssert().enableExceptions();
    primitives();
    databases();
    movement();
    callbacksAndLos();
    lights();
    std::jthread a([] {
        databases();
    }),
        b([] {
            movement();
        });

    return 0;
}

#include "test_registry.hpp"
OPENT1_REGISTER_TEST("simulation", "sim_collision_movement", opent1_test_sim_collision_movement);
