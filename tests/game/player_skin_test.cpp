#include "test_registry.hpp"
#include "ts_test_support.hpp"
#include <opent1/game/fear_player_psc.hpp>
#include <opent1/game/game_app.hpp>
#include <opent1/game/player.hpp>
#include <opent1/game/player_manager.hpp>
#include <chrono>
#include <cstdlib>

namespace {
using namespace opent1;
using namespace opent1::game;

void playerSkinRefresh() {
    auto resources = std::make_shared<ResourceManager>();
    auto shape = test::ts::load(test::ts::shape());
    CHECK(shape && shape->materials);
    shape->materials->materials[0].mapFile = "base.body.bmp";
    CHECK(resources->add("min.dts", std::move(shape)));
    CHECK(resources->add("base.body.bmp", gfx::Bitmap::create(4, 4)));
    DataBlockManager blocks(false, resources);
    auto* armor = dynamic_cast<PlayerData*>(
        blocks.createDataBlock("DeathKnightArmor", DataBlockManager::PlayerDataType));
    CHECK(armor);
    armor->fileName = "min.dts";

    sim::SimManager manager;
    auto ownedPlayers = std::make_unique<PlayerManager>(false);
    auto* players = ownedPlayers.get();
    players->setId(PlayerManagerId);
    CHECK(manager.addObject(std::move(ownedPlayers)));
    CHECK(players->clientAdded("DeathKnight", "male1", "cphoenix", true, 2049) == 2049);
    auto ownedPlayer = std::make_unique<Player>();
    auto* player = ownedPlayer.get();
    CHECK(player->setNetFlags(player->netFlags() | sim::SimNetObject::IsGhost));
    player->bindDataBlockManager(&blocks);
    player->setDatFileName("DeathKnightArmor");
    CHECK(manager.addObject(std::move(ownedPlayer)));
    player->setOwnerClient(2049);
    player->setPlayerControl(false);

    world::WorldShapeQuery fallback;
    CHECK(player->processQuery(fallback) && fallback.image);
    CHECK(fallback.image->resources.materials.list().materials[0].mapFile == "base.body.bmp");
    CHECK(player->getSkinBase() == "cphoenix");

    // Resource arrival and a same-name skin event must refresh the actual
    // materials, including when the event reaches the body through ShapeBase.
    auto skin = gfx::Bitmap::create(4, 4);
    skin->row(0)[0] = std::byte{42};
    CHECK(resources->add("cphoenix.body.bmp", std::move(skin)));
    CHECK(players->processEvent(PlayerSkinEvent(2049, "cphoenix")));
    world::WorldShapeQuery refreshed;
    CHECK(player->processQuery(refreshed) && refreshed.image);
    const auto& materials = refreshed.image->resources.materials;
    CHECK(materials.list().materials[0].mapFile == "cphoenix.body.bmp");
    CHECK(materials.textures()[0]->row(0)[0] == std::byte{42});
    // Rendering an unchanged player retains the immutable material table.
    world::WorldShapeQuery repeated;
    CHECK(player->processQuery(repeated) && repeated.image);
    CHECK(&repeated.image->resources.materials.data() == &materials.data());
    // Previously published frames and shared shape data must stay unchanged.
    CHECK(fallback.image->resources.materials.list().materials[0].mapFile == "base.body.bmp");
    CHECK(player->getTSShape()->materials->materials[0].mapFile == "base.body.bmp");
}

void checkDeathKnightSkin(FearGameApp& app) {
    auto& globals = app.fearPlugin().clientGlobals();
    CHECK(globals.playerPSC);
    auto* player = dynamic_cast<Player*>(globals.playerPSC->getControlObject());
    const auto* data = player ? dynamic_cast<const PlayerData*>(player->getDatPtr()) : nullptr;
    CHECK(data && data->fileName == "min");
    globals.playerPSC->setFirstPerson(false);
    world::WorldShapeQuery query;
    CHECK(player->processQuery(query) && query.image && query.image->drawShape);
    CHECK(player->getSkinBase() == "cphoenix");
    const auto& authored = player->getTSShape()->materials->materials;
    const auto& materials = query.image->resources.materials;
    std::size_t replacements = 0;
    for (std::size_t i = 0; i < authored.size(); ++i) {
        if (authored[i].mapFile.starts_with("base.")) {
            const auto name = "cphoenix." + authored[i].mapFile.substr(5);
            if (app.resources().contains(name)) {
                CHECK(materials.list().materials[i].mapFile == name);
                CHECK(materials.textures()[i].get() ==
                      app.resources().load<gfx::Bitmap>(name).get());
                ++replacements;
            }
        }
    }
    CHECK(replacements > 0);
}

void installedDeathKnightSkin() {
    const auto* assets = std::getenv("OPENT1_TEST_ASSETS");
    if (!assets ||
        !std::filesystem::exists(std::filesystem::path(assets) / "RPG/SpecialArmors.cs")) {
        return;
    }
    FearGameApp::Config config;
    config.assetRoot = assets;
    config.writeRoot = std::filesystem::temp_directory_path() /
                       std::format("opent1-deathknight-skin-{}",
                                   std::chrono::steady_clock::now().time_since_epoch().count());

    struct Cleanup {
        std::filesystem::path path;

        ~Cleanup() {
            std::error_code ec;
            std::filesystem::remove_all(path, ec);
        }
    } cleanup{config.writeRoot};

    const std::array<std::string_view, 2> args{"-mod", "RPG"};
    const auto init = [&](FearGameApp& app) {
        CHECK(app.init(args));
        app.console().setVariable("PCFG::Name", "DeathKnightSkinTest");
        app.console().setVariable("PCFG::Script", "");
        app.console().setVariable("PCFG::Info", "skin-test-password");
        CHECK(app.loadMission("rpgmap5"));
    };
    {
        FearGameApp app(config);
        init(app);
        for (int frame = 0; frame < 8; ++frame) {
            CHECK(app.stepFrame(frame * .032));
        }
        CHECK(app.fearPlugin().focusServer());
        CHECK(app.console()
                  .evaluate(R"(
            processMenupickgroup(2049, 3);
            processMenupickclass(2049, 1);
            storeData(2049, "RACE", "DeathKnight");
            UpdateAppearance(2049);
        )")
                  .ok());
        app.fearPlugin().syncLoopbackState(false);
        checkDeathKnightSkin(app);
        CHECK(app.fearPlugin().focusServer());
        CHECK(app.console().evaluate("return SaveCharacter(2049);").value == "true");
        CHECK(std::filesystem::is_regular_file(config.writeRoot / "temp/DeathKnightSkinTest.cs"));
        app.shutdown();
    }
    // Recreate the application to exercise a cold resource cache and returning
    // character login. Check the first controllable body, without waiting for
    // periodic appearance updates to repair it.
    for (int join = 0; join < 2; ++join) {
        FearGameApp app(config);
        init(app);
        bool spawned = false;
        for (int frame = 0; frame < 8; ++frame) {
            CHECK(app.stepFrame(frame * .032));
            auto* control = app.fearPlugin().clientGlobals().playerPSC->getControlObject();
            if (dynamic_cast<Player*>(control)) {
                checkDeathKnightSkin(app);
                spawned = true;
                break;
            }
        }
        CHECK(spawned);
        auto surface = gfx::SoftwareSurface::create(320, 240);
        CHECK(surface && app.renderFrame(*surface));
        CHECK(app.console().getVariable("OpenT1::LastRenderError").empty());
        app.shutdown();
    }
}
}

int opent1_test_player_skin_refresh() {
    return opent1::test::run(playerSkinRefresh);
}

int opent1_test_installed_deathknight_skin() {
    return opent1::test::run(installedDeathKnightSkin);
}

OPENT1_REGISTER_TEST("game", "player_skin_refresh", opent1_test_player_skin_refresh);
OPENT1_REGISTER_TEST("skins", "installed_deathknight_skin", opent1_test_installed_deathknight_skin);
