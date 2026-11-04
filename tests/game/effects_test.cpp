#include "test_registry.hpp"
#include "ts_test_support.hpp"
#include <opent1/game/game_app.hpp>
#include <opent1/game/ai.hpp>
#include <opent1/game/explosion.hpp>
#include <opent1/game/mine.hpp>
#include <opent1/game/player.hpp>
#include <opent1/game/player_manager.hpp>
#include <opent1/game/proj_grenade.hpp>
#include <opent1/game/proj_laser.hpp>
#include <opent1/game/smoke_puff.hpp>
#include <opent1/world/world_collision_sim.hpp>
#include <opent1/world/world_scene.hpp>
#include <chrono>
#include <cstdlib>

namespace {
using namespace opent1;
using namespace opent1::game;

std::vector<Explosion*> explosions(sim::SimManager& manager) {
    std::vector<Explosion*> result;
    auto* render = dynamic_cast<sim::SimSet*>(manager.findObject(sim::RenderSetId));
    CHECK(render);
    for (auto* object : render->objects())
        if (auto* effect = dynamic_cast<Explosion*>(object))
            result.push_back(effect);
    return result;
}

void checkGeometry(const world::WorldShapeImage& image,
                   Point3F camera,
                   const gfx::Palette* activePalette = nullptr) {
    CHECK(image.resources.shape && image.pose.shape().get() == image.resources.shape.get());
    TMat3F worldToCamera;
    worldToCamera.p = -camera;
    worldToCamera.flags |= RMat3F::Matrix_HasTranslation;
    world::TsStaticView view;
    m_mul(image.shapeToWorld, worldToCamera, view.shapeToCamera);
    view.automaticDetail = true;
    const auto frame = world::TsStaticShapeFrame::prepare(image.resources, view, image.pose);
    CHECK(frame && frame->polygonCount() > 0);
    gfx::Palette fallback;
    const auto* palette = activePalette ? activePalette : &fallback;
    const auto software = frame->softwarePolygons(palette);
    const auto hardware = frame->hardware(palette);
    CHECK(software && !software->empty() && hardware &&
          (!hardware->draws().empty() || !hardware->meshes().empty()));
    const auto retained =
        world::TsStaticShapeFrame::prepare(image.resources, view, image.pose, true);
    CHECK(retained && retained->polygonCount() > 0);
    const auto batch = retained->hardware(palette);
    CHECK(batch && (!batch->draws().empty() || !batch->meshes().empty()));
    if (activePalette) {
        auto surface = gfx::SoftwareSurface::create(view.width, view.height);
        CHECK(surface && surface->setPalette(*activePalette));
        surface->clear(0);
        CHECK(frame->draw(*surface));
        CHECK(std::ranges::any_of(surface->pixels(), [](auto pixel) {
            return pixel != std::byte{};
        }));
    }
}

void effects() {
    FearGameApp app;
    CHECK(app.init() && app.fearPlugin().createClientWorld());
    auto& globals = app.fearPlugin().clientGlobals();
    auto& manager = *globals.manager;
    auto& dbm = *globals.dbm;
    auto shape = test::ts::load(test::ts::shape());
    CHECK(shape);
    shape->transforms[0].translation = {1, 0, 0};
    shape->names.push_back("ambient");
    shape->sequences = {{Int32(shape->names.size() - 1), 0, 1.f, 0}};
    CHECK(app.resources().add("effect.dts", std::move(shape)));
    CHECK(app.resources().add("test.bmp", gfx::Bitmap::create(4, 4)));
    const auto model = app.resources().load<world::TsShape>("effect.dts");
    auto* explosion = dynamic_cast<ExplosionData*>(
        dbm.createDataBlock("EffectExplosion", DataBlockManager::ExplosionDataType));
    auto* mineData =
        dynamic_cast<MineData*>(dbm.createDataBlock("EffectBomb", DataBlockManager::MineDataType));
    CHECK(explosion && mineData);
    explosion->shape = model;
    explosion->timeScale = 1;
    explosion->faceCamera = true;
    mineData->shape = model;
    mineData->maxDamage = 1;
    mineData->explosionId = explosion->dbmIndex;

    auto mine = std::make_unique<Mine>();
    mine->bindDataBlockManager(&dbm);
    mine->setDatFileName("EffectBomb");
    CHECK(mine->setNetFlags(mine->netFlags() | sim::SimNetObject::IsGhost));
    auto* bomb = mine.get();
    CHECK(manager.addObject(std::move(mine)));
    bomb->setPos({0, 10, 0});
    CHECK(explosions(manager).empty());
    bomb->setState(StaticBase::Enabled);
    bomb->setState(StaticBase::Destroyed);
    auto spawned = explosions(manager);
    CHECK(spawned.size() == 1);
    CHECK((spawned.front()->position() - bomb->getBoxCenter()).len() < .001f);
    CHECK((spawned.front()->axis() - Point3F{0, 0, 1}).len() < .001f);
    world::WorldShapeQuery query;
    query.cameraPosition = Point3F{};
    CHECK(spawned.front()->processQuery(query) && query.image);
    checkGeometry(*query.image, {0, 0, 0});
    bomb->setState(StaticBase::Destroyed);
    CHECK(explosions(manager).size() == 1);
    CHECK(bomb->initResources(mineData)); // resource reinitialization must not replay destruction
    CHECK(explosions(manager).size() == 1);
    auto initial = std::make_unique<Mine>();
    initial->bindDataBlockManager(&dbm);
    initial->setDatFileName("EffectBomb");
    CHECK(initial->setNetFlags(initial->netFlags() | sim::SimNetObject::IsGhost));
    initial->setState(StaticBase::Destroyed);
    CHECK(manager.addObject(std::move(initial)));
    CHECK(explosions(manager).size() == 1);
    Mine arguments;
    arguments.bindDataBlockManager(&dbm);
    CHECK(arguments.processArguments(std::array<std::string, 1>{"EffectBomb"}));
    CHECK(arguments.getDatFileId() == mineData->dbmIndex);
    CHECK(!arguments.processArguments({}));
    CHECK(!arguments.processArguments(std::array<std::string, 3>{"EffectBomb", "1", "false"}));

    auto* smoke = SmokeManager::get(&manager);
    CHECK(smoke);
    auto* render = dynamic_cast<sim::SimSet*>(manager.findObject(sim::RenderSetId));
    CHECK(std::ranges::find(render->objects(), smoke) != render->objects().end());
    smoke->addPuff(model, 1, {0, 10, 0}, {0, 0, 1});
    smoke->addPuff("effect.dts", 1, {1, 10, 0}, {});
    query = {};
    CHECK(smoke->processQuery(query) && query.image && query.attachments.size() == 1);
    CHECK(query.image->shapeToWorld.p.x == model->transforms[0].translation.x);
    checkGeometry(*query.image, {});
    checkGeometry(query.attachments[0], {});
    sim::EnvironmentCamera camera;
    camera.viewport = {0, 0, 640, 480};
    camera.focal = {320, 320};
    camera.center = {320, 240};
    const auto scene = world::WorldSceneFrame::capture(*globals.world, camera, {}, false);
    CHECK(scene && scene->shapes().size() >= 3);
    const auto frozen = *query.image;
    query = {};
    query.cameraPosition = Point3F{-3, 0, 2};
    CHECK(smoke->processQuery(query) && query.image && query.attachments.size() == 1);
    Point3F actual;
    query.image->shapeToWorld.getRow(1, actual);
    auto expected = smoke->puffs()[0].position - *query.cameraPosition;
    expected.normalize();
    CHECK((actual - expected).len() < .001f);
    checkGeometry(*query.image, *query.cameraPosition);
    CHECK(app.stepFrame(0, false));
    CHECK(app.stepFrame(.256, false));
    CHECK(smoke->puffs()[0].position.z > 0);
    for (unsigned tick = 2; tick <= 6; ++tick)
        CHECK(app.stepFrame(tick * .256, false));
    CHECK(smoke->activePuffCount() == 0);
    CHECK(explosions(manager).empty());
    checkGeometry(frozen, {}); // captured resources survive puff expiry/reuse
}

void mineLifecycle() {
    FearGameApp app;
    CHECK(app.init() && app.fearPlugin().createServerWorld({}, true));
    auto& globals = app.fearPlugin().serverGlobals();
    auto& manager = *globals.manager;
    auto& console = app.console();
    CHECK(app.resources().add("lifecycle.dts", test::ts::load(test::ts::shape())));
    auto* data = dynamic_cast<MineData*>(
        globals.dbm->createDataBlock("LifecycleBomb", DataBlockManager::MineDataType));
    CHECK(data);
    data->shape = app.resources().load<world::TsShape>("lifecycle.dts");
    data->maxDamage = 1;
    data->explosionRadius = 10;
    data->damageValue = .5f;
    CHECK(console
              .evaluate(R"(
        $MineContacts = 0;
        $MineDamageEvents = 0;
        $MineDestroyed = 0;
        function LifecycleBomb::onCollision(%this, %object) { $MineContacts++; }
        function LifecycleBomb::onDestroyed(%this) { $MineDestroyed++; }
        PlayerData LifecycleArmor { maxDamage = 1; };
        function LifecycleArmor::onDamage(%this, %type, %value) {
            $MineDamageEvents++;
            $MineBlastDamage = %value;
            $DamageAfterDestroyed = $MineDestroyed;
        }
        newObject(LifecycleTarget, Player, LifecycleArmor);
        GameBase::setPosition(LifecycleTarget, "0 3 0");
    )")
              .ok());

    auto owned = std::make_unique<Mine>();
    owned->bindDataBlockManager(globals.dbm);
    owned->setDatFileName("LifecycleBomb");
    auto* bomb = owned.get();
    CHECK(manager.addObject(std::move(owned)));
    const auto id = bomb->getId();
    sim::SimCollisionImageQuery collision;
    CHECK(bomb->processQuery(collision) && !collision.images.empty());
    GameBase liveContact, destroyedContact;
    liveContact.setType(SimPlayerObjectType);
    destroyedContact.setType(SimPlayerObjectType);
    manager.advanceToTime(.125f);
    bomb->onCollisionNotify(&liveContact);
    CHECK(console.getVariable("MineContacts") == "1");
    manager.advanceToTime(.25f);
    bomb->setDamageLevel(data->maxDamage);
    CHECK(bomb->getState() == StaticBase::Destroyed);
    CHECK(console.getVariable("MineDestroyed") == "1");
    CHECK(console.getVariable("MineDamageEvents") == "1");
    CHECK(std::stof(console.getVariable("MineBlastDamage")) > 0);
    CHECK(console.getVariable("DamageAfterDestroyed") == "1");
    collision = {};
    CHECK(!bomb->processQuery(collision) && collision.images.empty());
    bomb->setDamageLevel(data->maxDamage);
    bomb->serverProcess(250);
    CHECK(console.getVariable("MineDamageEvents") == "1");
    // Use a new contact so the collision throttle cannot hide a pickup callback.
    bomb->onCollisionNotify(&destroyedContact);
    CHECK(console.getVariable("MineContacts") == "1");
    CHECK(bomb->isCollideable());
    manager.advanceToTime(.749f);
    CHECK(manager.findObject(id) == bomb); // allow the destroyed state to replicate
    manager.advanceToTime(.75f);
    CHECK(!manager.findObject(id));
}

void mineScriptCallbacks() {
    FearGameApp app;
    CHECK(app.init() && app.fearPlugin().createServerWorld({}, true));
    auto& globals = app.fearPlugin().serverGlobals();
    auto& console = app.console();
    CHECK(app.resources().add("callbacks.dts", test::ts::load(test::ts::shape())));
    auto* data = dynamic_cast<MineData*>(
        globals.dbm->createDataBlock("CallbackBomb", DataBlockManager::MineDataType));
    CHECK(data);
    data->shape = app.resources().load<world::TsShape>("callbacks.dts");
    data->className = "Handgrenade";
    data->maxDamage = 1;
    CHECK(console.evaluate(R"(
        $ItemContacts = 0;
        $MineContacts = 0;
        $MineDamage = 0;
        function Item::onCollision(%this, %object) { $ItemContacts++; }
        function Mine::onDamage(%this) { $MineDamage++; }
    )").ok());
    auto owned = std::make_unique<Mine>();
    owned->bindDataBlockManager(globals.dbm);
    owned->setDatFileName("CallbackBomb");
    auto* bomb = owned.get();
    CHECK(globals.manager->addObject(std::move(owned)));
    std::array<GameBase, 5> contacts;
    for (auto& contact : contacts)
        contact.setType(SimPlayerObjectType);
    globals.manager->advanceToTime(.125f);
    bomb->onCollisionNotify(&contacts[0]);
    // A live spell bomb has no pickup callback, even though Mine derives from Item.
    CHECK(console.getVariable("ItemContacts") == "0");
    bomb->applyDamage(0, .1f, {}, {}, {}, 0);
    CHECK(console.getVariable("MineDamage") == "1");

    // Ordinary pickups retain their Item fallback.
    auto* pickup = dynamic_cast<Item*>(globals.manager->addObject(std::make_unique<Item>()));
    CHECK(pickup);
    pickup->scriptOnCollision(&contacts[0], .125f);
    CHECK(console.getVariable("ItemContacts") == "1");

    CHECK(console.evaluate(R"(
        function Mine::onCollision(%this, %object) { $MineContacts++; }
    )").ok());
    bomb->onCollisionNotify(&contacts[1]);
    CHECK(console.getVariable("MineContacts") == "1");
    CHECK(console.evaluate(R"(
        function Handgrenade::onCollision(%this, %object) { $MineContacts += 10; }
    )").ok());
    bomb->onCollisionNotify(&contacts[2]);
    CHECK(console.getVariable("MineContacts") == "11");
    CHECK(console.evaluate(R"(
        function CallbackBomb::onCollision(%this, %object) { $MineContacts += 100; }
    )").ok());
    bomb->onCollisionNotify(&contacts[3]);
    CHECK(console.getVariable("MineContacts") == "111");
    bomb->assignName("NamedCallbackBomb");
    CHECK(console.evaluate(R"(
        function NamedCallbackBomb::onCollision(%this, %object) { $MineContacts += 1000; }
    )").ok());
    CHECK(bomb->scriptName("onCollision") == "NamedCallbackBomb::onCollision");
    bomb->setState(StaticBase::Destroyed);
    bomb->onCollisionNotify(&contacts[4]);
    CHECK(console.getVariable("MineContacts") == "111");
}

// A new registered Item subclass must get its own script namespace without an override.
class CallbackItemSubclass final : public Item {};

void runtimeScriptCallbacks() {
    FearGameApp app;
    CHECK(app.init() && app.fearPlugin().createServerWorld({}, true));
    auto& globals = app.fearPlugin().serverGlobals();
    auto& console = app.console();
    auto& registry = app.game().classes();
    CHECK(registry.registerClass<CallbackItemSubclass>("CallbackEffect"));
    CHECK(app.resources().add("runtime-callback.dts", test::ts::load(test::ts::shape())));
    const auto model = app.resources().load<world::TsShape>("runtime-callback.dts");
    for (const auto name :
         {"GameBase",     "ShapeBase",       "StaticBase",  "StaticShape",    "Moveable",
          "MoveableBase", "Player",          "Item",        "Mine",           "Sensor",
          "Turret",       "Vehicle",         "Flier",       "Bullet",         "Grenade",
          "RocketDumb",   "LaserProjectile", "TargetLaser", "SeekingMissile", "Lightning",
          "RepairEffect", "Marker",          "Trigger",     "CallbackEffect"}) {
        const auto* descriptor = registry.find(name);
        CHECK(descriptor);
        auto object = descriptor->create();
        auto* base = dynamic_cast<GameBase*>(object.get());
        CHECK(base);
        if (const auto group = base->getDatGroup(); group >= 0) {
            const auto dataName = std::string(name) + "CallbackData";
            auto* data = globals.dbm->createDataBlock(dataName, group);
            CHECK(data);
            if (auto* shape = dynamic_cast<ShapeBaseData*>(data))
                shape->shape = model;
            if (auto* projectile = dynamic_cast<ProjectileData*>(data))
                projectile->shape = model;
            base->bindDataBlockManager(globals.dbm);
            base->setDatFileName(dataName);
        }
        auto owned = std::unique_ptr<sim::SimObject>(base);
        (void)object.release();
        CHECK(globals.manager->addObject(std::move(owned)));
        CHECK(console
                  .evaluate(std::format(
                      "function {}::onRuntimeProbe(%this) {{ return \"{}\"; }}", name, name))
                  .ok());
        CHECK(base->getScriptDefaultClass() == name);
        CHECK(base->scriptName("onRuntimeProbe") == std::string(name) + "::onRuntimeProbe");
        CHECK(console
                  .execute(base->scriptName("onRuntimeProbe"),
                           std::array<std::string, 1>{base->scriptThis()})
                  .value == name);
        base->deleteObject();
    }

    // A parent callback is not a fallback in vanilla; only the global is.
    CHECK(console
              .evaluate(R"(
        function Item::onParentOnly(%this) { return "pickup"; }
        function ShapeBase::onParentOnly(%this) { return "shape"; }
    )")
              .ok());
    auto* effect = dynamic_cast<GameBase*>(
        globals.manager->addObject(std::make_unique<CallbackItemSubclass>()));
    CHECK(effect && effect->scriptName("onParentOnly").empty());
    CHECK(console
              .evaluate(R"(
        function onParentOnly(%this) { return "global"; }
    )")
              .ok());
    CHECK(effect->scriptName("onParentOnly") == "onParentOnly");

    // Registration belongs to the host, including ghosts and independent games.
    CHECK(app.fearPlugin().createClientWorld());
    auto ghost = std::make_unique<CallbackItemSubclass>();
    CHECK(ghost->setNetFlags(sim::SimNetObject::IsGhost));
    auto* ghostBase = ghost.get();
    CHECK(app.fearPlugin().clientGlobals().manager->addObject(std::move(ghost)));
    CHECK(ghostBase->getScriptDefaultClass() == "CallbackEffect");
    FearGameApp other;
    CHECK(other.init() && other.fearPlugin().createServerWorld({}, true));
    CHECK(other.game().classes().registerClass<CallbackItemSubclass>("OtherCallbackEffect"));
    auto* otherBase = dynamic_cast<GameBase*>(other.fearPlugin().serverGlobals().manager->addObject(
        std::make_unique<CallbackItemSubclass>()));
    CHECK(otherBase && otherBase->getScriptDefaultClass() == "OtherCallbackEffect");
    CHECK(ghostBase->getScriptDefaultClass() == "CallbackEffect");
}

class GrenadeProbe final : public Grenade {
public:
    using Grenade::Grenade;
    unsigned blasts{};

protected:
    void applyRadiusDamage(const Point3F& position) override {
        ++blasts;
        Grenade::applyRadiusDamage(position);
    }
};

void grenadeLifecycle() {
    for (const int direction : {-1, 1, 0}) {
        const bool timeout = direction == 0;
        FearGameApp app;
        CHECK(app.init() && app.fearPlugin().createClientWorld() &&
              app.fearPlugin().createServerWorld({}, true));
        auto& globals = app.fearPlugin().serverGlobals();
        auto& server = *globals.manager;
        auto& client = *app.fearPlugin().clientGlobals().manager;
        CHECK(app.resources().add("grenade-lifecycle.dts", test::ts::load(test::ts::shape())));
        auto* effect = dynamic_cast<ExplosionData*>(
            globals.dbm->createDataBlock("GrenadeBlast", DataBlockManager::ExplosionDataType));
        auto* data = dynamic_cast<GrenadeData*>(
            globals.dbm->createDataBlock("LifecycleShell", DataBlockManager::GrenadeDataType));
        CHECK(effect && data);
        effect->shape = app.resources().load<world::TsShape>("grenade-lifecycle.dts");
        data->bulletShapeName = "grenade-lifecycle.dts";
        data->explosionTag = effect->dbmIndex;
        data->explosionRadius = 10;
        data->elasticity = .45f;
        data->liveTime = 1;
        data->totalTime = 2;
        data->pSmokeName.clear();

        const std::array<PlaneF, 1> planes{{{0, 0, 1, 0}}};
        const std::array<world::InteriorCollisionNode, 1> nodes{{{0, -1, -2}}};
        const std::array<world::InteriorCollisionLeaf, 2> leaves{{{false, {}}, {true, {0}}}};
        const std::array<world::InteriorCollisionSurface, 1> surfaces{{
            {0, true, 0, {{-100, -100, 0}, {-100, 100, 0}, {100, 100, 0}, {100, -100, 0}}},
        }};
        auto floor = world::InteriorCollision::prepare(planes, nodes, leaves, surfaces);
        CHECK(floor);
        const std::array<sim::SimCollisionMaterial, 1> materials{{{0, .5f, 3}}};
        for (auto* manager : {&server, &client})
            CHECK(manager->addObject(std::make_unique<world::SimWorldCollider>(
                world::SimInteriorCollisionImage(
                    std::make_shared<const world::InteriorCollision>(*floor), materials),
                Box3F{{-100, -100, -1}, {100, 100, 0}})));

        auto owned = std::make_unique<GrenadeProbe>(data->dbmIndex);
        owned->bindDataBlockManager(globals.dbm);
        owned->initProjectile(TMat3F(EulerF{}, {0, 0, timeout ? 1000.f : 1.1f}), {}, 0);
        auto* shell = owned.get();
        CHECK(server.addObject(std::move(owned)));
        const auto id = shell->getId();
        shell->setForceMask(timeout ? sim::MovementGravityForce : 0);
        CHECK(shell->addForce(sim::MovementGravityForce, {0, 0, -20}, true));
        shell->setLinearVelocity(timeout ? Point3F{} : Point3F{4.f * direction, 0, -10});
        auto ghostOwned = std::make_unique<Grenade>(data->dbmIndex);
        ghostOwned->bindDataBlockManager(globals.dbm);
        CHECK(ghostOwned->setNetFlags(ghostOwned->netFlags() | sim::SimNetObject::IsGhost));
        auto* ghost = ghostOwned.get();
        CHECK(client.addObject(std::move(ghostOwned)));
        sim::network::GhostManager gm;
        const auto replicate = [&](UInt32 mask) {
            std::array<std::byte, 1024> bytes{};
            BitStream writer{std::span<std::byte>{bytes}};
            shell->packUpdate(gm, mask, writer);
            CHECK(writer.isValid());
            BitStream reader{std::span<const std::byte>{bytes}};
            CHECK(ghost->unpackUpdate(gm, reader));
        };
        replicate(Projectile::InitialUpdate);
        ghost->setForceMask(timeout ? sim::MovementGravityForce : 0);
        CHECK(ghost->addForce(sim::MovementGravityForce, {0, 0, -20}, true));
        UInt32 now = 0;
        const auto process = [&](UInt32 time) {
            server.advanceToTime(static_cast<float>(time) * .001f);
            shell->serverProcess(time);
        };
        bool bounceChecked = false;
        for (now = 32; now <= 1024; now += 32) {
            process(now);
            ghost->clientProcess(now);
            CHECK(!shell->hasExploded() && shell->blasts == 0);
            CHECK(!ghost->hasExploded());
            if (!timeout && shell->hasBounced() && !bounceChecked) {
                CHECK(std::abs(shell->getLinearVelocity().x - .9f * direction) < .001f);
                CHECK(std::abs(shell->getLinearVelocity().z - 2.2545f) < .001f);
                CHECK(shell->lastSurfaceType() == 3);
                bounceChecked = true;
            }
        }
        if (timeout) {
            CHECK(shell->getLinearPosition().z < 995); // gravity acts throughout the airborne fuse
            process(2000);
            CHECK(!shell->hasExploded());
            now = 2001;
        } else {
            CHECK(bounceChecked);
            CHECK(shell->setPosition(TMat3F(EulerF{}, {0, 0, 1.1f}), true));
            shell->setLinearVelocity({0, 0, -10});
            CHECK(ghost->setPosition(shell->getTransform(), true));
            ghost->setLinearVelocity(shell->getLinearVelocity());
            now = 1200;
        }
        process(now);
        CHECK(shell->hasExploded() && shell->isWaitingForDelete() && shell->blasts == 1);
        auto* smoke = SmokeManager::get(&client);
        CHECK(smoke);
        smoke->addPuff("grenade-lifecycle.dts", 1, shell->getLinearPosition(), {});
        smoke->addPuff(
            "grenade-lifecycle.dts", 1, shell->getLinearPosition() + Point3F{100, 0, 0}, {});
        CHECK(smoke->activePuffCount() == 2);
        ghost->clientProcess(now);
        CHECK(ghost->hasExploded() ==
              !timeout); // contacts predict; airborne timeout is authoritative
        replicate(Projectile::FPExplosionMask | Projectile::FPPositionMask |
                  Projectile::FPVelocityMask);
        CHECK(ghost->hasExploded() && explosions(client).size() == 1);
        CHECK((explosions(client).front()->position() - shell->getLinearPosition()).len() < .001f);
        CHECK(smoke->activePuffCount() == 1);
        replicate(Projectile::FPExplosionMask | Projectile::FPPositionMask);
        CHECK(explosions(client).size() == 1);
        world::WorldShapeQuery query;
        CHECK(!ghost->processQuery(query));
        process(now + 499);
        CHECK(server.findObject(id) == shell && shell->blasts == 1);
        process(now + 500);
        CHECK(!server.findObject(id));
    }
}

class BeamProbe final : public LaserProjectile {
public:
    using LaserProjectile::LaserProjectile;

    void setShooter(ShapeBase* shooter) {
        m_pShooter_ = shooter;
    }
};

void unarmedBeams() {
    FearGameApp app;
    CHECK(app.init() && app.fearPlugin().createClientWorld());
    auto& globals = app.fearPlugin().clientGlobals();
    auto& manager = *globals.manager;
    auto& dbm = *globals.dbm;
    CHECK(app.resources().add("beam-armor.dts", test::ts::load(test::ts::shape())));
    CHECK(app.resources().add("beam.bmp", gfx::Bitmap::create(4, 4)));
    auto* armor = dynamic_cast<PlayerData*>(
        dbm.createDataBlock("BeamArmor", DataBlockManager::PlayerDataType));
    auto* data =
        dynamic_cast<LaserData*>(dbm.createDataBlock("SpellBeam", DataBlockManager::LaserDataType));
    CHECK(armor && data);
    armor->shape = app.resources().load<world::TsShape>("beam-armor.dts");
    data->pBitmapName = "beam.bmp";
    data->liveTime = 1.5f;
    auto ownedPlayer = std::make_unique<Player>();
    ownedPlayer->bindDataBlockManager(&dbm);
    ownedPlayer->setDatFileName("BeamArmor");
    CHECK(ownedPlayer->setNetFlags(sim::SimNetObject::IsGhost));
    auto* player = ownedPlayer.get();
    CHECK(manager.addObject(std::move(ownedPlayer)));
    player->setPos({10, 20, 30});
    player->setViewPitch(-.3f);
    TMat3F muzzle;
    CHECK(!player->getMuzzleTransform(0, &muzzle));

    auto ownedBeam = std::make_unique<BeamProbe>(data->dbmIndex);
    ownedBeam->bindDataBlockManager(&dbm);
    CHECK(ownedBeam->setNetFlags(sim::SimNetObject::IsGhost));
    ownedBeam->setShooter(player);
    const Point3F impact{10, 40, 25};
    ownedBeam->setBeamEndpoints({}, impact, true);
    auto* beam = ownedBeam.get();
    CHECK(manager.addObject(std::move(ownedBeam)));
    CHECK((beam->startPoint() - player->getBoxCenter()).len() < .001f);
    sim::EnvironmentCamera camera;
    camera.viewport = {0, 0, 640, 480};
    camera.focal = {320, 320};
    camera.center = {320, 240};
    camera.worldToCamera.p = {-8, -18, -31};
    camera.worldToCamera.flags |= RMat3F::Matrix_HasTranslation;
    world::WorldSurfaceQuery query;
    query.camera = camera;
    CHECK(beam->processQuery(query) && query.image && !query.image->mesh->triangles.empty());
    CHECK(beam->endPoint() == impact); // remote shooters keep the replicated contact
    player->setPlayerControl(true);
    player->setPos({11, 20, 30});
    beam->clientProcess(32);
    CHECK((beam->startPoint() - player->getBoxCenter()).len() < .001f);
    Point3F direction;
    player->getEyeTransform().getRow(1, direction);
    auto actual = beam->endPoint() - player->getEyeTransform().p;
    actual.normalize();
    CHECK((actual - direction).len() < .001f);
    beam->clientProcess(1501);
    query = {};
    query.camera = camera;
    CHECK(!beam->processQuery(query));
}

void installedEffects() {
    const auto* assets = std::getenv("OPENT1_TEST_ASSETS");
    if (!assets || !std::filesystem::exists(std::filesystem::path(assets) / "RPG/scripts.vol"))
        return;
    FearGameApp::Config config;
    config.assetRoot = assets;
    config.writeRoot =
        std::filesystem::temp_directory_path() /
        ("opent1-effects-" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));

    struct Cleanup {
        std::filesystem::path path;

        ~Cleanup() {
            std::error_code ec;
            std::filesystem::remove_all(path, ec);
        }
    } cleanup{config.writeRoot};

    FearGameApp app(config);
    CHECK(app.init(std::array<std::string_view, 2>{"-mod", "RPG"}));
    app.console().setVariable("PCFG::Name", "EffectTest");
    app.console().setVariable("PCFG::Script", "");
    app.console().setVariable("PCFG::Info", "effects-test-password");
    CHECK(app.loadMission("rpgmap5"));
    for (unsigned tick = 0; tick < 8; ++tick)
        CHECK(app.stepFrame(tick * .032, false));
    CHECK(app.fearPlugin().focusServer());
    CHECK(app.console()
              .evaluate("processMenupickgroup(2049, 3); processMenupickclass(2049, 1);")
              .ok());
    CHECK(app.fearPlugin().focusServer());
    CHECK(app.console().isFunction("CreateAndDetBomb"));
    // Exercise the unmodified scripts used by fireball and forced-remort sparks.
    CHECK(app.console()
              .evaluate(R"(
        CreateAndDetBomb(2049, "Bomb9", "10000 10000 10000", False, 14);
        CreateAndDetBomb(2049, "Bomb7", "10010 10000 10000", False, 19);
    )")
              .ok());
    auto& server = *app.fearPlugin().serverGlobals().manager;
    auto* projectiles = dynamic_cast<sim::SimSet*>(server.findObject(PlayerSetId));
    CHECK(projectiles);
    std::vector<sim::SimObjectId> initialBombs;
    for (auto* object : projectiles->objects())
        if (auto* bomb = dynamic_cast<Mine*>(object); bomb && bomb->getPos().x > 9990)
            initialBombs.push_back(bomb->getId());
    CHECK(initialBombs.size() == 2);
    auto& client = *app.fearPlugin().clientGlobals().manager;
    std::vector<Int32> seen;
    for (unsigned tick = 8; tick < 40; ++tick) {
        CHECK(app.stepFrame(tick * .032, false));
        for (auto* effect : explosions(client)) {
            if (effect->position().x < 9990 ||
                std::ranges::find(seen, effect->getDatFileId()) != seen.end())
                continue;
            const auto camera = effect->position() - Point3F{0, 8, 0};
            world::WorldShapeQuery query;
            query.cameraPosition = camera;
            CHECK(effect->processQuery(query) && query.image);
            checkGeometry(*query.image, camera, app.defaultPalette().get());
            seen.push_back(effect->getDatFileId());
        }
    }
    CHECK(seen.size() == 2);
    for (const auto id : initialBombs) {
        CHECK(!server.findObject(id));
        CHECK(!app.fearPlugin().findGhostForServerObject(id));
    }
    CHECK(app.fearPlugin().focusServer());
    CHECK(app.console().evaluate("Player::mountItem(2049, OrbOfLuminance0, 2);").ok());
    CHECK(app.stepFrame(40 * .032, false));
    auto* player = dynamic_cast<Player*>(
        app.fearPlugin().serverGlobals().playerManager->findClient(2049)->ownedObject);
    CHECK(player);
    auto* ghost = dynamic_cast<Player*>(app.fearPlugin().findGhostForServerObject(player->getId()));
    CHECK(ghost && ghost->getItemImageEntry(2).imageId >= 0);
    world::WorldShapeQuery mounted;
    CHECK(ghost->processQuery(mounted));
    const auto& slot = ghost->getItemImageEntry(2);
    const auto image = std::ranges::find_if(mounted.attachments, [&](const auto& candidate) {
        return slot.resources && candidate.resources.shape.get() == slot.resources->shape.get();
    });
    CHECK(image != mounted.attachments.end());
    checkGeometry(*image, image->shapeToWorld.p - Point3F{0, 4, 0}, app.defaultPalette().get());
    auto* smoke = SmokeManager::get(&client);
    CHECK(smoke);
    smoke->addPuff("smoke.dts", 1, {10000, 10000, 10000}, {0, 0, 1});
    smoke->addPuff("smoke.dts", 1, {10001, 10000, 10000}, {0, 0, 1});
    for (unsigned tick = 41; tick <= 46; ++tick)
        CHECK(app.stepFrame(tick * .032, false));
    world::WorldShapeQuery puffs;
    puffs.cameraPosition = Point3F{10000, 9992, 10000};
    CHECK(smoke->processQuery(puffs) && puffs.image && puffs.attachments.size() >= 1);
    checkGeometry(*puffs.image, *puffs.cameraPosition, app.defaultPalette().get());
    checkGeometry(puffs.attachments[0], *puffs.cameraPosition, app.defaultPalette().get());

    CHECK(app.fearPlugin().focusServer());
    player->setPos(player->getPos() + Point3F{0, 0, 2});
    player->setRot({});
    player->setViewPitch(-.8f);
    player->setForceMask(0);
    player->setLinearVelocity({});
    (void)player->unmountItem(0);
    CHECK(app.console()
              .evaluate(R"(
        $PlayerSkill[2049, $SkillOffensiveCasting] = 1000;
        $PlayerSkill[2049, $SkillEnergy] = 1000;
        setMANA(2049);
        $SpellCast = BeginCastSpell(2049, "beam");
    )")
              .ok());
    CHECK(script::toBool(app.console().getVariable("SpellCast")));
    server.advanceToTime(server.getCurrentTime());
    LaserProjectile* beam = nullptr;
    for (auto* object : projectiles->objects())
        if (auto* candidate = dynamic_cast<LaserProjectile*>(object))
            beam = candidate;
    CHECK(beam);
    const auto beamId = beam->getId();
    player->setLinearVelocity({}); // hold the camera still after the spell's recoil
    for (unsigned tick = 47; tick <= 54; ++tick)
        CHECK(app.stepFrame(tick * .032, false));
    auto* beamGhost =
        dynamic_cast<LaserProjectile*>(app.fearPlugin().findGhostForServerObject(beamId));
    CHECK(beamGhost);
    sim::EnvironmentCamera beamCamera;
    beamCamera.viewport = {0, 0, 640, 480};
    beamCamera.focal = {320, 320};
    beamCamera.center = {320, 240};
    beamCamera.worldToCamera = ghost->getEyeTransform();
    beamCamera.worldToCamera.inverse();
    world::WorldSurfaceQuery beamQuery;
    beamQuery.camera = beamCamera;
    CHECK(beamGhost->processQuery(beamQuery) && beamQuery.image);
    CHECK((beamGhost->startPoint() - ghost->getEyeTransform().p).len() > .1f);
    world::TsStaticView beamView;
    beamView.shapeToCamera = beamCamera.worldToCamera;
    const auto beamFrame = beamQuery.image->mesh->prepare(beamView);
    CHECK(beamFrame && beamFrame->polygonCount() > 0);
    auto beamPixels = gfx::SoftwareSurface::create(640, 480);
    CHECK(beamPixels && beamPixels->setPalette(*app.defaultPalette()));
    beamPixels->clear(0);
    CHECK(beamFrame->draw(*beamPixels));
    CHECK(std::ranges::any_of(beamPixels->pixels(), [](auto pixel) {
        return pixel != std::byte{};
    }));
    const auto beamHardware = beamFrame->hardware(app.defaultPalette().get());
    CHECK(beamHardware && !beamHardware->draws().empty());
    const auto beamScene = world::WorldSceneFrame::captureContainers(
        *app.fearPlugin().clientGlobals().world, beamCamera, {}, false);
    CHECK(beamScene);
    const auto beamBitmap = app.resources().load<gfx::Bitmap>("forcefield.bmp");
    CHECK(std::ranges::any_of(beamScene->surfaces(), [&](const auto& surface) {
        return !surface.mesh->materials.textures().empty() &&
               surface.mesh->materials.textures()[0].get() == beamBitmap.get();
    }));

    CHECK(app.fearPlugin().focusServer());
    auto& console = app.console();
    unsigned tick = 55;
    for (const auto& [keyword, explosionName] :
         {std::pair{"fireball", "plasmaExp"}, std::pair{"icespike", "energyExp"}}) {
        CHECK(app.fearPlugin().focusServer());
        CHECK(console
                  .evaluate(R"(
            $SpellLOS = GameBase::getLOSInfo(Client::getOwnedObject(2049), 80);
        )")
                  .ok());
        CHECK(script::toBool(console.getVariable("SpellLOS")));
        CHECK(console.evaluate(std::format("$SpellCast = BeginCastSpell(2049, \"{}\");", keyword))
                  .ok());
        CHECK(script::toBool(console.getVariable("SpellCast")));
        Int32 explosionId = -1;
        CHECK(app.fearPlugin().serverGlobals().dbm->lookupDataBlock(
            explosionName, DataBlockManager::ExplosionDataType, &explosionId));
        bool spellEffect = false;
        const unsigned end = tick + 64;
        for (; tick < end; ++tick) {
            CHECK(app.stepFrame(tick * .032, false));
            if (spellEffect)
                continue;
            for (auto* effect : explosions(client)) {
                if (effect->getDatFileId() != explosionId)
                    continue;
                spellEffect = true;
                world::WorldShapeQuery shape;
                shape.cameraPosition = effect->position() - Point3F{0, 8, 0};
                CHECK(effect->processQuery(shape) && shape.image);
                checkGeometry(*shape.image, *shape.cameraPosition, app.defaultPalette().get());
                auto* render = dynamic_cast<sim::SimSet*>(client.findObject(sim::RenderSetId));
                CHECK(render);
                const auto scene = world::WorldSceneFrame::captureContainers(
                    *app.fearPlugin().clientGlobals().world, beamCamera, {}, false);
                CHECK(scene);
                CHECK(render->removeObject(*effect));
                const auto without = world::WorldSceneFrame::captureContainers(
                    *app.fearPlugin().clientGlobals().world, beamCamera, {}, false);
                CHECK(render->addObject(*effect));
                CHECK(without && scene->shapes().size() == without->shapes().size() + 1);
                auto before = gfx::SoftwareSurface::create(640, 480);
                auto after = gfx::SoftwareSurface::create(640, 480);
                CHECK(before && after && before->setPalette(*app.defaultPalette()) &&
                      after->setPalette(*app.defaultPalette()));
                before->clear(0);
                after->clear(0);
                CHECK(without->draw(*before) && scene->draw(*after));
                CHECK(!std::ranges::equal(before->pixels(), after->pixels()));
                break;
            }
        }
        CHECK(spellEffect);
    }

    CHECK(app.fearPlugin().focusServer());
    CHECK(console
              .evaluate(R"(
        $HellstormCast = DoCastSpell(2049, 18, GameBase::getPosition(2049),
                                    "10000 10000 10000", 0, "");
    )")
              .ok());
    CHECK(script::toBool(console.getVariable("HellstormCast")));
    std::vector<sim::SimAddress> hellstormBombs, hellstormGhosts;
    unsigned destroyed = 0;
    bool hellstormEffect = false;
    const auto end = tick + 100;
    for (; tick < end; ++tick) {
        CHECK(app.stepFrame(tick * .032, false));
        for (auto* object : projectiles->objects()) {
            auto* bomb = dynamic_cast<Mine*>(object);
            if (!bomb || bomb->getPos().x < 9990)
                continue;
            const auto address = bomb->address();
            if (std::ranges::none_of(hellstormBombs, [&](const auto& seenBomb) {
                    return seenBomb.objectId == address.objectId &&
                           seenBomb.generation == address.generation;
                })) {
                // Deleted object IDs are reused during this multi-stage spell.
                hellstormBombs.push_back(address);
            }
            if (auto* bombGhost = app.fearPlugin().findGhostForServerObject(bomb->getId()))
                hellstormGhosts.push_back(bombGhost->address());
            if (bomb->getState() == StaticBase::Destroyed)
                ++destroyed;
        }
        for (auto* effect : explosions(client))
            if (effect->position().x > 9990)
                hellstormEffect = true;
    }
    CHECK(hellstormBombs.size() == 26); // 25 falling plasma effects and one final blast
    CHECK(destroyed > 0 && hellstormEffect);
    for (const auto& address : hellstormBombs)
        CHECK(!address.resolve(server));
    CHECK(!hellstormGhosts.empty());
    for (const auto& address : hellstormGhosts)
        CHECK(!address.resolve(client));

    CHECK(app.fearPlugin().focusServer());
    CHECK(console
              .evaluate(R"(
        newObject(EffectHandGrenade, Mine, Handgrenade);
        GameBase::throw(EffectHandGrenade, Client::getOwnedObject(2049), 9, False);
        GameBase::setPosition(EffectHandGrenade, "10000 10000 10000");
    )")
              .ok());
    auto* hand = dynamic_cast<Mine*>(server.findObject("EffectHandGrenade"));
    CHECK(hand && hand->throwSource() == player && hand->getLinearVelocity().len() > 0);
    const auto handAddress = hand->address();
    const auto handExplosion = hand->mineData()->explosionId;
    const auto handSpawnTime = server.getCurrentTime();
    bool handDestroyed = false, handEffect = false;
    const auto handEnd = tick + 96;
    for (; tick < handEnd; ++tick) {
        CHECK(app.stepFrame(tick * .032, false));
        hand = dynamic_cast<Mine*>(handAddress.resolve(server));
        if (server.getCurrentTime() < handSpawnTime + 1.99f)
            CHECK(hand && hand->getState() != StaticBase::Destroyed);
        if (hand && hand->getState() == StaticBase::Destroyed)
            handDestroyed = true;
        for (auto* explosion : explosions(client))
            if (explosion->getDatFileId() == handExplosion && explosion->position().x > 9900)
                handEffect = true;
    }
    CHECK(handDestroyed && handEffect && !handAddress.resolve(server));
    CHECK(!app.fearPlugin().findGhostForServerObject(handAddress.objectId));

    // A hostile caster's live effect can touch the player before its short fuse.
    // Self casts and effects far from players hide the incorrect Item fallback.
    CHECK(app.fearPlugin().focusServer());
    CHECK(console.evaluate(R"(
        $BotInfo[EffectGoblin992, RACE] = "Goblin";
        SpawnAI(EffectGoblin992, SpellGoblin, "20000 20020 20000",
                "TempSpawn 20000 20020 20000 2");
        $EffectCaster = AI::getId(EffectGoblin992);
        $EnemyCast = DoCastSpell($EffectCaster, 14, GameBase::getPosition($EffectCaster),
                                "20000 20000 20000", 0, "");
    )").ok());
    CHECK(script::toBool(console.getVariable("EnemyCast")));
    auto* caster = app.fearPlugin().serverGlobals().aiManager->findAI("EffectGoblin992");
    CHECK(caster && caster->player());
    caster->player()->setForceMask(0);
    Mine* enemyBomb = nullptr;
    for (auto* object : projectiles->objects())
        if (auto* bomb = dynamic_cast<Mine*>(object); bomb && bomb->getPos().x > 19990)
            enemyBomb = bomb;
    CHECK(enemyBomb && enemyBomb->throwSource() == caster->player());
    CHECK(enemyBomb->getState() != StaticBase::Destroyed);
    const auto enemyAddress = enemyBomb->address();
    const auto enemyExplosion = enemyBomb->mineData()->explosionId;
    auto messages = std::make_shared<std::vector<std::string>>();
    app.fearPlugin().clientGlobals().playerManager->onChatMessage =
        [messages](Int32, std::string_view message) { messages->emplace_back(message); };
    player->setPos(enemyBomb->getPos());
    player->setLinearVelocity({});
    (void)player->stepPosition(player->getTransform(), .1f, .3f);
    server.advanceToTime(server.getCurrentTime());
    CHECK(enemyAddress.resolve(server) == enemyBomb);
    bool enemyDestroyed = false, enemyEffect = false;
    const auto enemyEnd = tick + 32;
    for (; tick < enemyEnd; ++tick) {
        CHECK(app.stepFrame(tick * .032, false));
        if (auto* bomb = dynamic_cast<Mine*>(enemyAddress.resolve(server)))
            enemyDestroyed |= bomb->getState() == StaticBase::Destroyed;
        for (auto* explosion : explosions(client))
            if (explosion->getDatFileId() == enemyExplosion && explosion->position().x > 19990)
                enemyEffect = true;
    }
    for (const auto& message : *messages)
        CHECK(message.find("Handgrenade") == std::string::npos);
    CHECK(enemyDestroyed && enemyEffect && !enemyAddress.resolve(server));

    // Discover the mod's complete mine group instead of enumerating spells.
    CHECK(app.fearPlugin().focusServer());
    auto& dbm = *app.fearPlugin().serverGlobals().dbm;
    unsigned checkedMines = 0, liveContacts = 0;
    for (Int32 index = 0; index < dbm.getDataGroupSize(DataBlockManager::MineDataType); ++index) {
        const auto name = dbm.lookupBlockName(index, DataBlockManager::MineDataType);
        CHECK(!name.empty());
        CHECK(console.evaluate(std::format("$CallbackMine = newObject(\"\", Mine, \"{}\");", name))
                  .ok());
        auto* mine = dynamic_cast<Mine*>(
            server.findObject(script::toInteger(console.getVariable("CallbackMine"))));
        CHECK(mine && mine->getScriptDefaultClass() == "Mine");
        CHECK(mine->scriptName("onCollision") != "Item::onCollision");
        if (mine->scriptName("onCollision").empty()) {
            mine->onCollisionNotify(player);
            ++liveContacts;
        }
        mine->deleteObject();
        ++checkedMines;
    }
    for (const auto tickEnd = tick + 4; tick < tickEnd; ++tick)
        CHECK(app.stepFrame(tick * .032, false));
    for (const auto& message : *messages)
        CHECK(message.find("You received ") == std::string::npos);
    CHECK(checkedMines > 0 && liveContacts > 0);
    std::printf("Checked all %u installed mine datablocks, including %u live contacts.\n",
                checkedMines,
                liveContacts);
}
}

int opent1_test_effects() {
    return opent1::test::run(effects);
}

int opent1_test_installed_effects() {
    return opent1::test::run(installedEffects);
}

OPENT1_REGISTER_TEST("game", "effects", opent1_test_effects);

int opent1_test_effects_focused() {
    return opent1::test::run(effects);
}

OPENT1_REGISTER_TEST("effects", "effects", opent1_test_effects_focused);
OPENT1_REGISTER_TEST("effects", "installed_effects", opent1_test_installed_effects);

int opent1_test_mine_lifecycle() {
    return opent1::test::run(mineLifecycle);
}

OPENT1_REGISTER_TEST("game", "mine_lifecycle", opent1_test_mine_lifecycle);

int opent1_test_mine_lifecycle_focused() {
    return opent1::test::run(mineLifecycle);
}

OPENT1_REGISTER_TEST("effects", "mine_lifecycle", opent1_test_mine_lifecycle_focused);

int opent1_test_mine_script_callbacks() {
    return opent1::test::run(mineScriptCallbacks);
}

OPENT1_REGISTER_TEST("game", "mine_script_callbacks", opent1_test_mine_script_callbacks);

int opent1_test_mine_script_callbacks_focused() {
    return opent1::test::run(mineScriptCallbacks);
}

OPENT1_REGISTER_TEST("effects", "mine_script_callbacks", opent1_test_mine_script_callbacks_focused);

int opent1_test_runtime_script_callbacks() {
    return opent1::test::run(runtimeScriptCallbacks);
}

OPENT1_REGISTER_TEST("game", "runtime_script_callbacks", opent1_test_runtime_script_callbacks);

int opent1_test_runtime_script_callbacks_focused() {
    return opent1::test::run(runtimeScriptCallbacks);
}

OPENT1_REGISTER_TEST("effects",
                     "runtime_script_callbacks",
                     opent1_test_runtime_script_callbacks_focused);

int opent1_test_grenade_lifecycle() {
    return opent1::test::run(grenadeLifecycle);
}

OPENT1_REGISTER_TEST("game", "grenade_lifecycle", opent1_test_grenade_lifecycle);

int opent1_test_grenade_lifecycle_focused() {
    return opent1::test::run(grenadeLifecycle);
}

OPENT1_REGISTER_TEST("effects", "grenade_lifecycle", opent1_test_grenade_lifecycle_focused);

int opent1_test_unarmed_beams() {
    return opent1::test::run(unarmedBeams);
}

OPENT1_REGISTER_TEST("game", "unarmed_beams", opent1_test_unarmed_beams);

int opent1_test_unarmed_beams_focused() {
    return opent1::test::run(unarmedBeams);
}

OPENT1_REGISTER_TEST("effects", "unarmed_beams", opent1_test_unarmed_beams_focused);
