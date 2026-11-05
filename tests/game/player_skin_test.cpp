#include "test_registry.hpp"
#include "ts_test_support.hpp"
#include <opent1/game/fear_cs_delegate.hpp>
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

void preSpawnClientTeam() {
    FearGameApp app;
    CHECK(app.init());
    auto& plugin = app.fearPlugin();
    CHECK(plugin.createServerWorld({}, true));
    auto& server = plugin.serverGlobals();
    server.playerManager->teamAdded("Citizen", "rpghuman0");
    server.playerManager->teamAdded("Elf", "rpgelf");
    CHECK(server.playerManager->clientAdded("twin", "male1", "base", true, 2049) == 2049);
    auto* representative = server.playerManager->findClient(2049);
    CHECK(representative && !representative->ownedObject && representative->team == -1);

    sim::SimManager client;
    auto ownedPlayers = std::make_unique<PlayerManager>(false);
    auto* players = ownedPlayers.get();
    players->setId(PlayerManagerId);
    CHECK(client.addObject(std::move(ownedPlayers)));
    players->teamAdded("Citizen", "rpghuman0");
    players->teamAdded("Elf", "rpgelf");
    CHECK(players->clientAdded("twin", "male1", "base", true, 2049) == 2049);
    std::vector<std::unique_ptr<sim::SimEvent>> pending;
    server.playerManager->onLocalEvent = [&](auto event) {
        CHECK(event && event->address.managerId == 2049);
        pending.push_back(std::move(event));
    };
    const auto deliver = [&] {
        auto events = std::move(pending);
        pending.clear();
        for (const auto& event : events) {
            CHECK(players->processEvent(*event));
        }
    };
    std::vector<std::string> messages;
    players->onChatMessage = [&](Int32, std::string_view message) {
        messages.emplace_back(message);
    };

    struct ResetDelivery {
        PlayerManager* server;
        PlayerManager* client;

        ~ResetDelivery() {
            server->onLocalEvent = {};
            client->onChatMessage = {};
        }
    } resetDelivery{server.playerManager, players};

    auto shape = test::ts::load(test::ts::shape());
    CHECK(shape && shape->materials);
    shape->materials->materials[0].mapFile = "base.body.bmp";
    CHECK(app.resources().add("min.dts", std::move(shape)));
    for (const auto name :
         {"base.body.bmp", "rpghuman0.body.bmp", "cphoenix.body.bmp", "rpgelf.body.bmp"}) {
        CHECK(app.resources().add(name, gfx::Bitmap::create(4, 4)));
    }
    auto* armor = dynamic_cast<PlayerData*>(
        server.dbm->createDataBlock("DeathKnightArmor", DataBlockManager::PlayerDataType));
    CHECK(armor);
    armor->fileName = "min.dts";

    // RPG's Game::playerSpawn calls UpdateTeam before assigning the owned body.
    // Vanilla c_setTeam falls back to PlayerManager::setTeam in this case.
    const auto beforeSpawn = app.console().evaluate("return GameBase::setTeam(2049, 0);");
    CHECK(beforeSpawn.ok());
    deliver();
    auto body = std::make_unique<Player>();
    auto* player = body.get();
    player->bindDataBlockManager(server.dbm);
    player->setDatFileName("DeathKnightArmor");
    CHECK(server.manager->addObject(std::move(body)));
    player->setTeam(representative->team);
    player->setOwnerClient(2049);
    auto ghost = std::make_unique<Player>();
    auto* rendered = ghost.get();
    CHECK(rendered->setNetFlags(rendered->netFlags() | sim::SimNetObject::IsGhost));
    rendered->bindDataBlockManager(server.dbm);
    rendered->setDatFileName("DeathKnightArmor");
    CHECK(client.addObject(std::move(ghost)));
    rendered->setOwnerClient(2049);
    rendered->setPlayerControl(false);
    CHECK(app.console().evaluate("Client::setSkin(2049, cphoenix);").ok());
    deliver();
    world::WorldShapeQuery initial;
    CHECK(rendered->processQuery(initial) && initial.image);
    CHECK(initial.image->resources.materials.list().materials[0].mapFile == "cphoenix.body.bmp");

    // The installed Weapon::onUse later enforces Citizen for human clients.
    // A correctly assigned spawn must make that operation a no-op, preserving
    // the custom skin instead of sending a late team event that replaces it.
    const auto messagesBeforeUse = messages.size();
    CHECK(app.console()
              .evaluate(R"(
        if (GameBase::getTeam(2049) != 0)
            GameBase::setTeam(2049, 0);
    )")
              .ok());
    deliver();
    world::WorldShapeQuery afterUse;
    CHECK(rendered->processQuery(afterUse) && afterUse.image);
    CHECK(afterUse.image->resources.materials.list().materials[0].mapFile == "cphoenix.body.bmp");
    CHECK(messages.size() == messagesBeforeUse);
    CHECK(messages == std::vector<std::string>{"twin joined team Citizen."});
    CHECK(representative->team == 0 && representative->skinBase == "cphoenix");
    CHECK(players->findClient(2049)->team == 0 &&
          players->findClient(2049)->skinBase == "cphoenix");
    CHECK(beforeSpawn.value == "-1"); // Vanilla's bodyless-client return value.

    // An actual team change must retain vanilla's default-skin reset.
    CHECK(app.console().evaluate("GameBase::setTeam(2049, 1);").ok());
    deliver();
    world::WorldShapeQuery changed;
    CHECK(rendered->processQuery(changed) && changed.image);
    CHECK(changed.image->resources.materials.list().materials[0].mapFile == "rpgelf.body.bmp");
    server.playerManager->onLocalEvent = {};
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
        app.console().setVariable("PCFG::Name", "SkinKnight");
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
        CHECK(std::filesystem::is_regular_file(config.writeRoot / "temp/SkinKnight.cs"));
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

    // Use the actual UDP path within one application's separate server/client
    // worlds, so the localConnect team's preassignment cannot mask the bug.
    FearGameApp app(config);
    CHECK(app.init(args));
    app.console().setVariable("PCFG::Name", "SkinKnight");
    app.console().setVariable("PCFG::Script", "");
    app.console().setVariable("PCFG::Info", "skin-test-password");
    auto reservation = net::makeUdpTransport({{127, 0, 0, 1}, 0});
    const auto reserved = net::parseIPv4Address(reservation->localAddress());
    CHECK(reserved);
    app.console().setVariable("Server::Port", std::to_string(reserved->port));
    reservation.reset();
    CHECK(app.fearPlugin().createServerWorld({}, true));
    CHECK(app.fearPlugin().focusServer());
    CHECK(app.loadMission("rpgmap5", true));
    std::puts("Checking the returning DeathKnight through native UDP");
    std::fflush(stdout);
    CHECK(app.fearPlugin().serverGlobals().playerManager->getNumClients() == 0);
    const auto endpoint = net::parseIPv4Address(
        app.fearPlugin().serverGlobals().csDelegate->delegate()->localAddress());
    CHECK(endpoint);
    const std::array<std::string, 1> address{"IP:127.0.0.1:" + std::to_string(endpoint->port)};
    CHECK(app.console().execute("connect", address).value == "True");
    std::size_t citizenJoins = 0;

    struct RestoreChat {
        FearPlugin& plugin;
        std::function<void(Int32, std::string_view)> previous;

        ~RestoreChat() {
            plugin.onChatMessage = std::move(previous);
        }
    } restoreChat{app.fearPlugin(), app.fearPlugin().onChatMessage};

    app.fearPlugin().onChatMessage = [&](Int32 type, std::string_view message) {
        if (message == "SkinKnight joined team Citizen.") {
            ++citizenJoins;
            std::printf("DeathKnight UDP event: %.*s\n", int(message.size()), message.data());
        }
        if (restoreChat.previous) {
            restoreChat.previous(type, message);
        }
    };
    int spawned = -1;
    std::size_t joinsAtSpawn = 0;
    bool verifiedAfterUse = false;
    for (int frame = 0; frame < 1024; ++frame) {
        CHECK(app.stepFrame(frame * .032, false));
        auto* psc = app.fearPlugin().clientGlobals().playerPSC;
        auto* player = psc ? dynamic_cast<Player*>(psc->getControlObject()) : nullptr;
        if (player) {
            checkDeathKnightSkin(app);
            if (spawned < 0) {
                spawned = frame;
                joinsAtSpawn = citizenJoins;
                const auto owner = player->getOwnerClient();
                CHECK(app.fearPlugin().focusServer());
                const auto* rep = app.fearPlugin().serverGlobals().playerManager->findClient(owner);
                CHECK(rep);
                std::printf("DeathKnight UDP first body: team=%d, skin=%s\n",
                            rep->team,
                            rep->skinBase.c_str());
                CHECK(app.console()
                          .evaluate(std::format(
                              "Weapon::onUse(Client::getOwnedObject({}), PickAxe);", owner))
                          .ok());
            }
            if (frame >= spawned + 32) {
                verifiedAfterUse = true;
                break;
            }
        }
    }
    CHECK(spawned >= 0 && verifiedAfterUse);
    CHECK(citizenJoins == 1 && citizenJoins == joinsAtSpawn);
    auto surface = gfx::SoftwareSurface::create(320, 240);
    CHECK(surface && app.renderFrame(*surface));
    CHECK(app.console().getVariable("OpenT1::LastRenderError").empty());
}
}

int opent1_test_player_skin_refresh() {
    return opent1::test::run(playerSkinRefresh);
}

int opent1_test_installed_deathknight_skin() {
    return opent1::test::run(installedDeathKnightSkin);
}

int opent1_test_pre_spawn_client_team() {
    return opent1::test::run(preSpawnClientTeam);
}

OPENT1_REGISTER_TEST("game", "player_skin_refresh", opent1_test_player_skin_refresh);
OPENT1_REGISTER_TEST("game", "pre_spawn_client_team", opent1_test_pre_spawn_client_team);
OPENT1_REGISTER_TEST("skins", "installed_deathknight_skin", opent1_test_installed_deathknight_skin);
