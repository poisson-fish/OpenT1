#include "test_registry.hpp"
#include <ranges>
#include "test_support.hpp"
#include "ts_collision_test_support.hpp"
#include <opent1/game/game_app.hpp>
#include <opent1/game/flier.hpp>
#include <opent1/game/proj_bullet.hpp>
#include <opent1/game/proj_rocket.hpp>
#include <opent1/game/sensor_manager.hpp>
#include <opent1/game/sound.hpp>
#include <opent1/game/status_combat_huds.hpp>
#include <opent1/game/turret.hpp>
#include <cstdlib>

using namespace opent1;
using namespace opent1::game;

namespace {
bool near(Flt32 a, Flt32 b) {
    return std::abs(a - b) < .001f;
}

template <class T> T& block(DataBlockManager& dbm, Int32 group, std::string_view name) {
    auto* result = dynamic_cast<T*>(dbm.createDataBlock(name, group));
    CHECK(result);
    return *result;
}

template <class T>
T* add(sim::SimManager& manager, DataBlockManager& dbm, std::string_view data, bool ghost = false) {
    auto object = std::make_unique<T>();
    object->bindDataBlockManager(&dbm);
    object->setDatFileName(data);
    if (ghost)
        CHECK(object->setNetFlags(object->netFlags() | sim::SimNetObject::IsGhost));
    auto* result = object.get();
    CHECK(manager.addObject(std::move(object)));
    return result;
}

class ImpactProbe : public Bullet {
public:
    void staticImpact(sim::SimCollisionInfo& hit) {
        Projectile::spawnImpactEffect(hit);
    }
};

void gameplayRegressions() {
    FearGameApp app;
    CHECK(app.init());
    CHECK(app.fearPlugin().createServerWorld("GameplayRegression"));
    auto& globals = app.fearPlugin().serverGlobals();
    auto& manager = *globals.manager;
    auto& dbm = *globals.dbm;
    auto& console = app.console();
    auto shape = opent1::test::ts_collision::cube();
    shape->names.insert(shape->names.end(), {"root", "die"});
    shape->sequences = {{2, 1, 1.f, 0}, {3, 0, 1.f, 0}};
    CHECK(app.resources().add<world::TsShape>("regression.dts", std::move(shape)));
    const auto model = app.resources().load<world::TsShape>("regression.dts");
    CHECK(console
              .evaluate(R"(
        SoundProfileData RegressionLoop { flags = {SFX_IS_HARDWARE_3D, SFX_IS_LOOPING}; };
        SoundProfileData RegressionStream { flags = {SFX_IS_SOFTWARE_3D, SFX_IS_STREAMING, SFX_IS_PITCHING}; };
        SoundData RegressionSound { wavFileName = "synthetic.wav"; profile = RegressionLoop; };
        function RegressionTurret::verifyTarget(%turret, %target) {
            $CheckedTurret = %turret; $CheckedTarget = %target; return $TargetHot;
        }
    )")
              .ok());
    const auto* profile = dynamic_cast<SoundProfileData*>(
        dbm.lookupDataBlock("RegressionLoop", DataBlockManager::SoundProfileDataType));
    const auto* stream = dynamic_cast<SoundProfileData*>(
        dbm.lookupDataBlock("RegressionStream", DataBlockManager::SoundProfileDataType));
    CHECK(profile && profile->profile.flags == 34);
    CHECK(stream && stream->profile.flags == 28);
    auto* sound = dbm.lookupDataBlock("RegressionSound", DataBlockManager::SoundDataType);
    CHECK(sound);
    std::vector<UInt32> played;
    std::vector<SoundHandle> stopped;
    auto playback = std::make_unique<SoundPlayback>([&](UInt32 id, const TMat3F&, const Point3F&) {
        played.push_back(id);
        return SoundHandle(played.size());
    });
    playback->stop = [&](SoundHandle handle) {
        stopped.push_back(handle);
    };
    CHECK(manager.addObject(std::move(playback)));

    auto& armor = block<PlayerData>(dbm, DataBlockManager::PlayerDataType, "RegressionArmor");
    armor.shape = model;
    armor.maxDamage = 1.5f;
    armor.animData[Player::ANIM_STAND] = {"root", -1, 1, 7};
    armor.animData[Player::ANIM_DIE_TORSO_FRONT] = {"die", sound->dbmIndex, 1, 7};
    auto* body = add<Player>(manager, dbm, "RegressionArmor", true);
    const auto* visible = dynamic_cast<sim::SimSet*>(manager.findObject(SensorVisibleSetId));
    CHECK(visible && std::ranges::find(visible->objects(), body) != visible->objects().end());
    body->setDamageLevel(.75f);
    const auto metrics =
        Crosshair::computeReticleMetrics({640, 480}, 90, 90, 22.5f, false, 0, body, "Player");
    CHECK(near(metrics.targetDamage, .5f));
    body->setDamageLevel(1.5f);
    body->updateAnimation(.032f);
    CHECK(played.size() == 1 && played.back() == UInt32(sound->dbmIndex));
    body->updateAnimation(.2f);
    CHECK(played.size() == 1);

    auto& image = block<ItemImageData>(dbm, DataBlockManager::ItemImageDataType, "RegressionImage");
    image.weaponType = Player::Spinning;
    image.sfxFireTag = sound->dbmIndex;
    image.fireTime = .1f;
    auto& mounted = body->getItemImageEntry(0);
    mounted.imageId = image.dbmIndex;
    mounted.ammo = mounted.triggerDown = true;
    body->setImageState(0, Player::ItemImageEntry::Fire);
    const auto loopHandle = mounted.animSound;
    CHECK(loopHandle && mounted.animLoopingSound);
    const auto playCount = played.size();
    for ([[maybe_unused]] auto i : std::views::iota(0, 40))
        body->updateImageState(0, .1f);
    CHECK(mounted.animSound == loopHandle && played.size() == playCount && stopped.empty());
    mounted.triggerDown = false;
    body->updateImageState(0, .1f);
    CHECK(stopped.size() == 1 && stopped.back() == loopHandle);

    auto& turretData = block<TurretData>(dbm, DataBlockManager::TurretDataType, "RegressionTurret");
    turretData.shape = model;
    turretData.iRange = 200;
    turretData.FOV = -1;
    auto* turret = add<Turret>(manager, dbm, "RegressionTurret");
    turret->setTeam(0);
    turret->setActive(true);
    turret->setElevationLimits(-3.14f, 3.14f);
    auto* enemy = add<Player>(manager, dbm, "RegressionArmor");
    enemy->setTeam(1);
    enemy->setPos({0, 20, 0});
    console.setVariable("TargetHot", "False");
    CHECK(!turret->isTargetable(enemy));
    CHECK(console.getVariable("CheckedTurret") == turret->scriptThis());
    CHECK(console.getVariable("CheckedTarget") == enemy->scriptThis());
    console.setVariable("TargetHot", "True");
    CHECK(turret->isTargetable(enemy));
    console.setVariable("TargetHot", "False");
    CHECK(!turret->isTargetable(enemy));

    auto& staticData =
        block<StaticShapeData>(dbm, DataBlockManager::StaticShapeDataType, "RegressionShape");
    staticData.shape = model;
    staticData.visibleToSensor = true;
    auto* first = add<StaticShape>(manager, dbm, "RegressionShape");
    auto* second = add<StaticShape>(manager, dbm, "RegressionShape");
    first->setName("West antenna");
    second->setName("East antenna");
    CHECK(first->sensorKey >= 0 && second->sensorKey >= 0 && first->sensorKey != second->sensorKey);
    CHECK(globals.sensorManager->target(first->sensorKey).name == "West antenna");
    const auto removedKey = first->sensorKey;

    auto& bullet = block<BulletData>(dbm, DataBlockManager::BulletDataType, "RegressionBullet");
    bullet.totalTime = bullet.liveTime = 3;
    auto& effect =
        block<ExplosionData>(dbm, DataBlockManager::ExplosionDataType, "RegressionImpact");
    effect.shape = model;
    effect.timeScale = 1;
    bullet.explosionTag = effect.dbmIndex;
    auto* probe = add<ImpactProbe>(manager, dbm, "RegressionBullet");
    sim::SimCollisionInfo hit;
    hit.object = first->address();
    CollisionSurface contact;
    contact.position = {1, 2, 3};
    contact.normal = {0, -1, 0};
    hit.surfaces.surfaces.push_back(contact);
    probe->staticImpact(hit);
    CHECK(probe->hasExploded() && near(probe->explosionPosition().y, 2));
    // A decoded effect can precede object adoption. It must survive until onAdd.
    probe->setSpawnState({}, {0, 100, 0});
    probe->setExplosionState({0, 10, 0}, {0, -1, 0}, effect.dbmIndex);
    sim::network::GhostManager wire;
    std::array<std::byte, 2048> bytes{};
    BitStream output{std::span<std::byte>(bytes)};
    probe->packUpdate(wire, Projectile::FPExplosionMask, output);
    auto pending = std::make_unique<Bullet>();
    pending->setNetFlags(sim::SimNetObject::IsGhost);
    pending->bindDataBlockManager(&dbm);
    pending->setDatFileName("RegressionBullet");
    BitStream input{std::span<const std::byte>(bytes)};
    CHECK(pending->unpackUpdate(wire, input));
    CHECK(!pending->hasExploded());
    auto* adopted = pending.get();
    CHECK(manager.addObject(std::move(pending)));
    CHECK(adopted->hasExploded());
    int effectCount = 0;
    for (auto* object : manager.objects())
        if (auto* explosion = dynamic_cast<Explosion*>(object)) {
            ++effectCount;
            world::WorldShapeQuery query;
            CHECK(explosion->processQuery(query) && query.image);
        }
    CHECK(effectCount == 1);
    first->deleteObject();
    CHECK(globals.sensorManager->target(removedKey).obj == nullptr);

    auto& rocket = block<RocketData>(dbm, DataBlockManager::RocketDataType, "RegressionRocket");
    rocket.muzzleVelocity = rocket.terminalVelocity = 100;
    rocket.inheritedVelocityScale = .5f;
    rocket.totalTime = rocket.liveTime = 3;
    TMat3F muzzle{EulerF{0, 0, 1.57079632679f}, {100, 200, 300}};
    auto moving = std::make_unique<RocketDumb>(rocket.dbmIndex);
    moving->bindDataBlockManager(&dbm);
    moving->initProjectile(muzzle, {20, -40, 60}, 0);
    auto* launched = moving.get();
    CHECK(manager.addObject(std::move(moving)));
    const auto velocity = launched->getLinearVelocity();
    CHECK(near(velocity.x, -90));
    CHECK(near(velocity.y, -20));
    CHECK(near(velocity.z, 30));
    CHECK(near(launched->getLinearPosition().x, 100));

    // Exercise the weapon-to-projectile path as well as direct initialization.
    image.projectile = {.type = DataBlockManager::RocketDataType, .dataType = rocket.dbmIndex};
    enemy->getItemImageEntry(0).imageId = image.dbmIndex;
    enemy->setRot({0, 0, 1.57079632679f});
    enemy->setLinearVelocity({20, -40, 60});
    enemy->fireImage(0);
    bool movingShot = false;
    for (auto* object : manager.objects()) {
        if (auto* shot = dynamic_cast<RocketDumb*>(object);
            shot && shot->getOwnerId() == enemy->getId()) {
            const auto shotVelocity = shot->getLinearVelocity();
            CHECK(near(shotVelocity.x, -90));
            CHECK(near(shotVelocity.y, -20));
            CHECK(near(shotVelocity.z, 30));
            movingShot = true;
        }
    }
    CHECK(movingShot);
    app.shutdown();
}

void scheduledMissionClock() {
    FearGameApp app;
    CHECK(app.init());
    CHECK(app.fearPlugin().createServerWorld("ClockRegression"));
    CHECK(app.stepFrame(0));
    CHECK(app.stepFrame(5)); // stepFrame bounds pauses to a quarter second
    CHECK(
        app.console()
            .evaluate(
                R"(schedule("resetSimTime(); $ResetClock = getSimTime(); schedule(\"$ClockDone=1;\",0.064);",0.032);)")
            .ok());
    CHECK(app.stepFrame(5.032));
    CHECK(app.stepFrame(5.064));
    CHECK(!app.console().getVariable("ResetClock").empty());
    CHECK(near(std::stof(app.console().getVariable("ResetClock")), 0));
    CHECK(app.stepFrame(5.192));
    CHECK(app.console().getVariable("ClockDone") == "1");
    CHECK(app.fearPlugin().serverGlobals().manager->getCurrentTime() > .3f);
    app.shutdown();
}

void installedGameplay() {
    const auto* assets = std::getenv("OPENT1_TEST_ASSETS");
    if (!assets)
        return;
    FearGameApp::Config config;
    config.assetRoot = assets;
    config.writeRoot = std::filesystem::temp_directory_path() / "opent1-gameplay-acceptance";
    config.mountDiscoveredAssets = true;
    FearGameApp app(config);
    CHECK(app.init());
    app.console().setVariable("Server::warmupTime", "0");
    CHECK(app.loadMission("Raindance"));
    CHECK(app.fearPlugin().focusServer());
    auto& globals = app.fearPlugin().serverGlobals();
    Int32 bulletId = -1;
    CHECK(globals.dbm->lookupDataBlock(
        "ChaingunBullet", DataBlockManager::BulletDataType, &bulletId));
    for (const auto name :
         {"SmallAntenna", "MediumAntenna", "LargeAntenna", "ArrayAntenna", "RodAntenna", "Scout"}) {
        std::unique_ptr<StaticBase> object =
            std::string_view(name) == "Scout"
                ? std::unique_ptr<StaticBase>(std::make_unique<Flier>())
                : std::make_unique<StaticShape>();
        object->bindDataBlockManager(globals.dbm);
        object->bindConsole(&app.console());
        object->setDatFileName(name);
        auto* target = object.get();
        CHECK(globals.manager->addObject(std::move(object)));
        target->setPos({10000, 10000, 10000});
        target->setEnergy(0);
        sim::SimCollisionImageQuery query;
        CHECK(target->processQuery(query) && !query.images.empty());
        std::optional<Point3F> shotStart;
        const auto box = target->getBoundingBox();
        for (auto x : std::views::iota(1, 10))
            for (auto z : std::views::iota(1, 10)) {
                sim::SimCollisionLineImage line;
                line.start = {box.fMin.x + box.len_x() * float(x) / 10,
                              box.fMin.y - 5,
                              box.fMin.z + box.len_z() * float(z) / 10};
                line.end = {line.start.x, box.fMax.y + 5, line.start.z};
                CollisionSurfaceList surfaces;
                if (query.images.front()->collide(line, surfaces) &&
                    (!shotStart || std::abs(line.start.x - target->getBoxCenter().x) <
                                       std::abs(shotStart->x - target->getBoxCenter().x)))
                    shotStart = line.start;
            }
        CHECK(shotStart);
        auto shot = std::make_unique<Bullet>(bulletId);
        auto* bullet = shot.get();
        shot->bindDataBlockManager(globals.dbm);
        TMat3F muzzle;
        muzzle.identity();
        muzzle.p = *shotStart;
        shot->initProjectile(muzzle, {}, 0);
        CHECK(globals.manager->addObject(std::move(shot)));
        const auto shotAddress = bullet->address();
        bool effectSeen = false;
        for (UInt32 time = 32; time < 600; time += 32) {
            auto* live = dynamic_cast<Bullet*>(shotAddress.resolve(*globals.manager));
            if (!live)
                break;
            live->serverProcess(time);
            app.fearPlugin().syncLoopbackState(false);
            auto* renderSet = dynamic_cast<sim::SimSet*>(
                app.fearPlugin().clientGlobals().manager->findObject(sim::RenderSetId));
            std::vector<sim::SimObject*> consumed;
            for (auto* item : renderSet->objects())
                if (auto* explosion = dynamic_cast<Explosion*>(item)) {
                    world::WorldShapeQuery image;
                    CHECK(explosion->processQuery(image) && image.image);
                    CHECK(std::ranges::any_of(image.image->pose.objects(), [](const auto& state) {
                        return state.visible;
                    }));
                    effectSeen = true;
                    consumed.push_back(explosion);
                }
            for (auto* item : consumed)
                item->deleteObject();
        }
        CHECK(target->getDamageLevel() > 0);
        CHECK(effectSeen);
        target->deleteObject();
    }
    auto surface = gfx::SoftwareSurface::create(640, 480);
    CHECK(app.console().evaluate("schedule(\"Server::loadMission(Broadside);\",0.032);").ok());
    for (auto i : std::views::iota(1) | std::views::take_while([&](int i) {
                      return i <= 650;
                  })) {
        CHECK(app.stepFrame(i * .032));
        if (i % 60 == 0)
            CHECK(app.renderFrame(*surface));
    }
    CHECK(app.console().getVariable("loadingMission") == "false");
    CHECK(app.console().getVariable("MissionName") == "Broadside");
    CHECK(app.console().getVariable("OpenT1::LastNetworkError").empty());
    CHECK(globals.manager->findObject("MissionGroup"));
    CHECK(globals.playerPSC->getControlObject());
    CHECK(app.fearPlugin().clientGlobals().playerPSC->getControlObject());
    CHECK(app.guiRuntime()->canvas.top()->getName() == "playGui");
    app.shutdown();
}
}

int opent1_test_gameplay_regressions() {
    return opent1::test::run(gameplayRegressions);
}

int opent1_test_scheduled_mission_clock() {
    return opent1::test::run(scheduledMissionClock);
}

int opent1_test_installed_gameplay() {
    return opent1::test::run(installedGameplay);
}

OPENT1_REGISTER_TEST("game", "gameplay_regressions", opent1_test_gameplay_regressions);
OPENT1_REGISTER_TEST("game", "scheduled_mission_clock", opent1_test_scheduled_mission_clock);
OPENT1_REGISTER_TEST("fixpack", "installed_gameplay", opent1_test_installed_gameplay);
