#include "test_registry.hpp"
#include "test_support.hpp"
#include <opent1/game/ai.hpp>
#include <opent1/game/fear_cs_delegate.hpp>
#include <opent1/game/game_app.hpp>
#include <opent1/game/sensor_manager.hpp>
#include <opent1/game/status_combat_huds.hpp>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>

namespace {
using namespace opent1;
using namespace opent1::game;

class PopupWall final : public sim::SimContainer {
public:
    explicit PopupWall(Flt32 height) {
        setType(StaticObjectType);
        image_ = std::make_shared<sim::SimCollisionBoxImage>();
        image_->box = {{-2, 9, 100}, {2, 11, 100 + height}};
        CHECK(setBoundingBox(image_->box));
    }

    bool onAdd() override {
        return sim::SimContainer::onAdd() &&
               dynamic_cast<sim::SimContainer*>(getManager()->findObject(sim::RootContainerId))
                   ->addObject(*this);
    }

    bool processQuery(sim::SimQuery& query) override {
        if (auto* collision = dynamic_cast<sim::SimCollisionImageQuery*>(&query)) {
            collision->images = {image_};
            return true;
        }
        return sim::SimContainer::processQuery(query);
    }

private:
    std::shared_ptr<sim::SimCollisionBoxImage> image_;
};

std::shared_ptr<gfx::BitmapFont> popupFont() {
    auto font = std::make_shared<gfx::BitmapFont>();
    font->info.flags = gfx::FontFixed | gfx::FontBitmap | gfx::FontTransparent;
    font->info.width = 1;
    font->info.height = 8;
    auto sheet = gfx::Bitmap::create(256, 8);
    CHECK(sheet);
    sheet->attributes |= gfx::BitmapTransparent;
    for (Int32 c = 0; c < 256; ++c) {
        font->lookup.push_back(Int16(c));
        font->characters.push_back({0, UInt8(c), 0, 1, 8, 0, {}});
        for (Int32 y = 0; y < 8; ++y) {
            sheet->row(y)[c] = std::byte(UInt8(128 + c % 64));
        }
    }
    font->sheets.bitmaps.push_back(std::move(*sheet));
    CHECK(font->valid());
    return font;
}

void liveEnemyPopup() {
    for (const bool bot : {false, true}) {
        FearGameApp app;
        CHECK(app.init());
        auto& plugin = app.fearPlugin();
        CHECK(plugin.createServerWorld({}, true));
        CHECK(app.console()
                  .evaluate(R"(
            PlayerData PopupArmor {
                mass = 9; maxDamage = 1.5;
                boxWidth = 0.5; boxDepth = 0.5; boxNormalHeight = 2;
            };
        )")
                  .ok());
        const auto remoteEnemyId = bot ? 0
                                       : plugin.serverGlobals().playerManager->clientAdded(
                                             "Enemy player", "male1", "base", true);
        std::unique_ptr<FearGameApp> remote;
        unsigned frame = 0;
        const auto advance = [&] {
            for (unsigned i = 0; i < 64; ++i, ++frame) {
                CHECK(app.stepFrame(frame * .032, false));
                if (remote) {
                    CHECK(remote->stepFrame(frame * .032, false));
                }
            }
        };
        if (bot) {
            CHECK(app.console().execute("localConnect").value == "True");
        } else {
            remote = std::make_unique<FearGameApp>();
            CHECK(remote->init());
            const auto address = net::parseIPv4Address(
                plugin.serverGlobals().csDelegate->delegate()->localAddress());
            CHECK(address);
            CHECK(
                remote->console()
                    .execute("connect", std::array{"IP:127.0.0.1:" + std::to_string(address->port)})
                    .value == "True");
            advance();
        }
        auto& sg = plugin.serverGlobals();
        auto& cg = (remote ? *remote : app).fearPlugin().clientGlobals();
        sg.playerManager->teamAdded(0);
        sg.playerManager->teamAdded(1);
        const auto id = Int32(cg.manager->getId());
        const auto viewerId = plugin.spawnPlayer("PopupArmor", {0, 0, 100}, {}, id, 0);
        CHECK(viewerId);
        auto* viewer = dynamic_cast<Player*>(sg.manager->findObject(*viewerId));
        CHECK(viewer);
        viewer->setViewPitch(std::atan2(.65f - 1.7f, 20.f));
        viewer->setForceMask(0);
        sg.playerManager->findClient(id)->curGuiMode = PlayerPSC::PlayGuiMode;
        const auto enemyId =
            bot ? sg.playerManager->aiAdded("Enemy bot", "male1", "base", true) : remoteEnemyId;
        CHECK(enemyId > 0);
        sg.playerManager->serverClientTeamChange(enemyId, 1);
        auto object = std::make_unique<Player>();
        object->bindDataBlockManager(sg.dbm);
        object->setDatFileName("PopupArmor");
        auto* enemy = object.get();
        CHECK(sg.manager->addObject(std::move(object)));
        enemy->setOwnerClient(enemyId);
        enemy->setTeam(1);
        enemy->setPos({0, 20, 100});
        enemy->setForceMask(0);
        enemy->setDamageLevel(.75f);
        CHECK(viewer->detectItem(enemy));
        sg.sensorManager->testObject(enemy);
        CHECK(enemy->getVisibleToTeam(0));
        const auto replicate = [&] {
            if (remote) {
                advance();
            } else {
                plugin.syncLoopbackState(false);
            }
        };
        replicate();
        const auto slot = enemyId - PlayerManager::BaseClientId;
        CHECK(cg.playerPSC->visibleList()[slot / 32] & (1u << (slot % 32)));
        Crosshair crosshair;
        crosshair.hudFont = crosshair.lowResFont = popupFont();
        auto marker = gfx::Bitmap::create(3, 3);
        CHECK(marker);
        std::ranges::fill(marker->pixels, std::byte{WHITE});
        crosshair.sprites["H_Foe.bmp"] = crosshair.sprites["lr_H_Foe.bmp"] =
            std::make_shared<gfx::Bitmap>(std::move(*marker));
        sim::EnvironmentCamera camera;
        const auto update = [&](Point2I extent, bool sensors = true) {
            auto* ghost = dynamic_cast<Player*>(cg.playerPSC->getControlObject());
            CHECK(ghost);
            ghost->setViewPitch(std::atan2(.65f - 1.7f, 20.f));
            camera.worldToCamera = ghost->getEyeTransform();
            camera.worldToCamera.inverse();
            CHECK(crosshair.resize({}, extent));
            camera.center = {extent.x * .5f, extent.y * .5f};
            camera.focal = {extent.x * .5f, extent.x * .5f};
            crosshair.updateLive(*cg.manager,
                                 *cg.playerPSC,
                                 cg.playerManager,
                                 cg.dbm,
                                 camera,
                                 1000,
                                 false,
                                 sensors,
                                 false);
        };
        for (const auto extent : {Point2I{640, 480}, Point2I{320, 240}}) {
            update(extent);
            const auto& metrics = crosshair.reticleMetrics();
            CHECK(metrics.enabled && metrics.hasTarget && !metrics.targetFriendly);
            CHECK(metrics.targetName == (bot ? "Enemy bot" : "Enemy player"));
            CHECK(std::abs(metrics.targetDamage - .5f) < .01f);
            CHECK(metrics.markers.size() == 1);
            auto surface = gfx::SoftwareSurface::create(extent.x, extent.y);
            CHECK(surface && crosshair.draw(*surface, {}, {0, 0, extent.x - 1, extent.y - 1}));
            const auto text = extent / 2 + (metrics.lowRes ? Point2I{10, 10} : Point2I{16, 16});
            CHECK(surface->bitmap().row(text.y)[text.x] == std::byte(128 + 'E' % 64));
            const auto barY = text.y + 8 + (metrics.lowRes ? 3 : 4);
            CHECK(surface->bitmap().row(barY)[text.x] == std::byte{BLUE_HILITE});
            CHECK(surface->bitmap().row(barY + 1)[text.x + 1] == std::byte{GREEN});
            CHECK(surface->bitmap().row(barY + 1)[text.x + (metrics.lowRes ? 15 : 20)] ==
                  std::byte{0});
            const auto point = metrics.markers.front().point;
            CHECK(surface->bitmap().row(point.y - 2)[point.x] == std::byte{WHITE});
        }
        update({640, 480}, false);
        CHECK(!crosshair.reticleMetrics().hasTarget && crosshair.reticleMetrics().markers.empty());
        // Preserve the server visibility bit while checking the client's
        // vanilla-height marker ray independently of sensor detection.
        auto wall = std::make_unique<PopupWall>(1.5f);
        auto* lowWall = wall.get();
        CHECK(cg.manager->addObject(std::move(wall)));
        update({640, 480});
        CHECK(crosshair.reticleMetrics().hasTarget);
        lowWall->deleteObject();
        auto highWall = std::make_unique<PopupWall>(3.f);
        auto* occluder = highWall.get();
        CHECK(cg.manager->addObject(std::move(highWall)));
        update({640, 480});
        CHECK(!crosshair.reticleMetrics().hasTarget && crosshair.reticleMetrics().markers.empty());
        occluder->deleteObject();
        enemy->setNeverVisibleToTeam(0, true);
        sg.sensorManager->testObject(enemy);
        replicate();
        update({640, 480});
        CHECK(!crosshair.reticleMetrics().hasTarget && crosshair.reticleMetrics().markers.empty());
        enemy->setNeverVisibleToTeam(0, false);
        sg.playerManager->serverClientTeamChange(enemyId, 0);
        enemy->setTeam(0);
        sg.sensorManager->testObject(enemy);
        replicate();
        update({640, 480});
        CHECK(crosshair.reticleMetrics().hasTarget && crosshair.reticleMetrics().targetFriendly);
        enemy->setDamageLevel(1.5f);
        replicate();
        update({640, 480});
        CHECK(!crosshair.reticleMetrics().hasTarget && crosshair.reticleMetrics().markers.empty());
    }
}

void installedEnemyPopup() {
    const auto* assets = std::getenv("OPENT1_TEST_ASSETS");
    if (!assets || !*assets) {
        return;
    }
    const auto mod = std::filesystem::path(assets) / "RPG";
    if (!std::filesystem::exists(mod / "scripts.vol") &&
        !std::filesystem::exists(mod / "Server.cs")) {
        return;
    }
    FearGameApp::Config config;
    config.assetRoot = assets;
    config.writeRoot = std::filesystem::temp_directory_path() /
                       std::format("opent1-target-popup-{}",
                                   std::chrono::steady_clock::now().time_since_epoch().count());

    struct Cleanup {
        std::filesystem::path path;

        ~Cleanup() {
            std::error_code ec;
            std::filesystem::remove_all(path, ec);
        }
    } cleanup{config.writeRoot};

    FearGameApp app(config);
    CHECK(app.init(std::array<std::string_view, 2>{"-mod", "RPG"}));
    auto& console = app.console();
    console.setVariable("PCFG::Name", "TargetTest");
    console.setVariable("PCFG::Script", "");
    console.setVariable("PCFG::Info", "target-test-password");
    console.setVariable("Server::Port", "0");
    console.setVariable("Server::Heartbeats", "false");
    CHECK(app.loadMission("rpgmap5"));
    for (int tick = 0; tick < 8; ++tick) {
        CHECK(app.stepFrame(tick * .032));
    }
    CHECK(app.fearPlugin().focusServer());
    CHECK(console.evaluate("processMenupickgroup(2049, 3); processMenupickclass(2049, 1);").ok());
    auto& sg = app.fearPlugin().serverGlobals();
    auto* player = dynamic_cast<Player*>(sg.playerManager->findClient(2049)->ownedObject);
    CHECK(player);
    player->setPos({10000, 10000, 10000});
    player->setRot({});
    player->setViewPitch(0);
    CHECK(console
              .evaluate(R"(
        SpawnAI(Raider901, PopupEnemy, "10000 10020 10000", "TempSpawn 10000 10020 10000 1");
    )")
              .ok());
    auto* bot = sg.aiManager->findAI("Raider901");
    CHECK(bot && bot->player());
    bot->player()->setPos({10000, 10020, 10000});
    const auto* armor = dynamic_cast<const PlayerData*>(bot->player()->getDatPtr());
    CHECK(armor);
    bot->player()->setDamageLevel(armor->maxDamage * .5f);
    CHECK(player->detectItem(bot->player()));
    sg.sensorManager->testObject(bot->player());
    CHECK(bot->player()->getVisibleToTeam(player->getTeam()));
    app.fearPlugin().syncLoopbackState(false);
    CHECK(app.setGameplayGuiMode(PlayerPSC::PlayGuiMode));
    auto surface = gfx::SoftwareSurface::create(640, 480);
    CHECK(surface && app.renderFrame(*surface));
    Crosshair* crosshair = nullptr;
    const auto find = [&](auto&& self, gui::Control& control) -> void {
        if (auto* candidate = dynamic_cast<Crosshair*>(&control)) {
            crosshair = candidate;
        }
        for (auto* child : control.objects()) {
            self(self, *static_cast<gui::Control*>(child));
        }
    };
    find(find, *app.guiRuntime()->canvas.top());
    CHECK(crosshair);
    CHECK(crosshair->hudFont && crosshair->lowResFont);
    const auto& cg = app.fearPlugin().clientGlobals();
    const auto slot = bot->id - PlayerManager::BaseClientId;
    CHECK(cg.playerPSC->visibleList()[slot / 32] & (1u << (slot % 32)));
    for (const auto pixels : {Point2I{640, 480}, Point2I{320, 240}, Point2I{1280, 960}}) {
        const bool scaled = pixels.x == 1280;
        console.setVariable("pref::UI::scale", scaled ? "2" : "1");
        surface = gfx::SoftwareSurface::create(pixels.x, pixels.y);
        CHECK(surface);
        CHECK(app.guiPresentation(pixels).scale == (scaled ? 2.f : 1.f));
        CHECK(app.renderFrame(*surface));
        const auto& metrics = crosshair->reticleMetrics();
        CHECK(metrics.enabled && metrics.hasTarget && !metrics.targetFriendly);
        CHECK(metrics.lowRes == (pixels.x == 320));
        CHECK(metrics.targetName == sg.playerManager->findBaseRep(bot->id)->name);
        CHECK(std::abs(metrics.targetDamage - .5f) < .02f);
        CHECK(metrics.markers.size() == 1);
        auto hud = gfx::SoftwareSurface::create(crosshair->fields().extent.x,
                                                crosshair->fields().extent.y);
        CHECK(hud);
        const auto extent = crosshair->fields().extent;
        CHECK(crosshair->draw(*hud, {}, {0, 0, extent.x - 1, extent.y - 1}));
        const auto text = extent / 2 + (metrics.lowRes ? Point2I{10, 10} : Point2I{16, 16});
        const auto& font = *(metrics.lowRes ? crosshair->lowResFont : crosshair->hudFont);
        const auto barY = text.y + font.info.height + (metrics.lowRes ? 3 : 4);
        CHECK(hud->bitmap().row(barY)[text.x] == std::byte{BLUE_HILITE});
        CHECK(hud->bitmap().row(barY + 1)[text.x + 1] == std::byte{GREEN});
        bool nameDrawn = false;
        for (auto y = text.y; y < text.y + font.info.height; ++y) {
            const auto row = hud->bitmap().row(y).subspan(
                text.x, std::size_t(font.textWidth(metrics.targetName)));
            nameDrawn |= std::ranges::any_of(row, [](std::byte pixel) {
                return pixel != std::byte{0};
            });
        }
        CHECK(nameDrawn);
    }
}
} // namespace

int opent1_test_installed_enemy_popup() {
    return opent1::test::run(installedEnemyPopup);
}

int opent1_test_live_enemy_popup() {
    return opent1::test::run(liveEnemyPopup);
}

OPENT1_REGISTER_TEST("game", "live_enemy_popup", opent1_test_live_enemy_popup);
OPENT1_REGISTER_TEST("targetpopup", "installed_enemy_popup", opent1_test_installed_enemy_popup);
