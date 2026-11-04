#include "test_registry.hpp"
#include "ts_test_support.hpp"
#include <opent1/game/game_app.hpp>
#include <opent1/game/explosion.hpp>
#include <opent1/game/mine.hpp>
#include <opent1/game/player.hpp>
#include <opent1/game/smoke_puff.hpp>
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
