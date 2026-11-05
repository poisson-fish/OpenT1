#include "test_registry.hpp"
#include <ranges>
#include "test_support.hpp"
#include "net_test_support.hpp"
#include <opent1/game/game_app.hpp>
#include <opent1/game/shell_controls.hpp>
#include <opent1/core/vol_stream.hpp>
#include <opent1/game/player.hpp>
#include <opent1/game/player_manager.hpp>
#include <opent1/game/script_plugin.hpp>
#include <opent1/sim/sim_sdl_input.hpp>
#include <chrono>
#include <cstdlib>

namespace {
using namespace opent1;
using namespace opent1::game;

struct TemporaryDirectory {
    std::filesystem::path path =
        std::filesystem::temp_directory_path() /
        std::format("opent1-disconnect-{}",
                    std::chrono::steady_clock::now().time_since_epoch().count());

    ~TemporaryDirectory() {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }
};

void disconnectSendFailures() {
    for (const bool explicitDisconnect : {false, true}) {
        net_test::Pair peers;
        peers.connect();
        Console console;
        sim::SimWorld world(&console);
        auto owned = std::make_unique<sim::network::PacketStream>(
            std::make_shared<sim::network::Registry>(), false);
        auto* stream = owned.get();
        CHECK(world.manager().addObject(std::move(owned)));
        stream->attach(peers.client, peers.clientId);
        peers.a.sendResult = net::Result::IoError;
        if (explicitDisconnect) {
            stream->disconnect(32);
            stream->disconnect(64);
        }
        CHECK(stream->deleteObject());
        CHECK(console.log().size() == 1);
        CHECK(console.log().front() ==
              std::format("Failed to send disconnect to 1 (network result {}).",
                          int(net::Result::IoError)));
    }
}

void remoteDisconnect() {
    enum class Termination { Command, Shutdown, Kick, Timeout };
    for (const auto termination :
         {Termination::Command, Termination::Shutdown, Termination::Kick, Termination::Timeout}) {
        FearGameApp server, client;
        CHECK(server.init() && client.init());
        CHECK(server.fearPlugin().createServerWorld({}, true));
        auto& sg = server.fearPlugin().serverGlobals();
        CHECK(server.console()
                  .evaluate(R"(
            PlayerData DisconnectArmor { mass = 9; };
            function Server::onClientDisconnect(%id) {
                $DropCount++;
                $DroppedId = %id;
                $DroppedName = Client::getName(%id);
                $DroppedBody = Client::getOwnedObject(%id);
                $DroppedCamera = Client::getObserverCamera(%id);
                $DroppedBodyType = getObjectType($DroppedBody);
                Client::setGuiMode(%id, 1);
                Client::setControlObject(%id, -1);
                if ($DropMode == "delete")
                    deleteObject($DroppedBody);
                else if ($DropMode == "release")
                    Client::setOwnedObject(%id, -1);
            }
        )")
                  .ok());
        // The callback may delete or retain the body, as vanilla leaveGame does.
        server.console().setVariable("DropMode",
                                     termination == Termination::Shutdown ? "delete"
                                     : termination == Termination::Kick   ? "release"
                                                                          : "");
        const auto endpoint = net::parseIPv4Address(sg.csDelegate->delegate()->localAddress());
        CHECK(endpoint);
        const std::array<std::string, 1> address{"IP:127.0.0.1:" + std::to_string(endpoint->port)};
        client.console().setVariable("PCFG::Name", "DepartingPlayer");
        CHECK(client.console().execute("connect", address).value == "True");
        double time = 0;
        const auto advance = [&](int frames, bool advanceClient) {
            for (int frame = 0; frame < frames; ++frame) {
                time += .032;
                CHECK(server.stepFrame(time, false));
                if (advanceClient) {
                    CHECK(client.stepFrame(time, false));
                }
            }
        };
        advance(64, true);
        CHECK(client.fearPlugin().isConnected());
        auto* rep = sg.playerManager->getClientList();
        CHECK(rep && !rep->nextClient);
        const auto id = rep->id;
        const auto* state = server.scriptPlugin().findClient(id);
        CHECK(state);
        const auto camera = sg.manager->findObject(state->observerCameraId)->address();
        const auto bodyId =
            server.fearPlugin().spawnPlayer("DisconnectArmor", {10, 20, 30}, {}, id);
        CHECK(bodyId);
        const auto body = sg.manager->findObject(*bodyId)->address();
        // Keep the client world focused so callback object lookup must explicitly
        // select the server rather than accidentally using the active manager.
        CHECK(server.fearPlugin().createClientWorld());
        bool disconnected{}, timedOut{};
        sg.csDelegate->onConnection = [&](const net::Event& event) {
            disconnected |= event.type == net::EventType::Disconnected;
            timedOut |= event.type == net::EventType::TimedOut;
        };
        switch (termination) {
            case Termination::Command:
                CHECK(client.console().execute("disconnect").value == "True");
                break;
            case Termination::Shutdown:
                client.shutdown();
                break;
            case Termination::Kick:
                CHECK(server.console()
                          .evaluate(std::format("Net::kick({}, \"Disconnect regression\");", id))
                          .value == "True");
                break;
            case Termination::Timeout:
                // Stop polling the peer without closing its socket or sending a
                // disconnect. Only the server's native timeout can retire it.
                advance(1600, false);
                break;
        }
        advance(16, termination == Termination::Kick);
        CHECK(server.console().getVariable("DropCount") == "1");
        CHECK(server.console().getVariable("DroppedId") == std::to_string(id));
        CHECK(server.console().getVariable("DroppedName") == "DepartingPlayer");
        CHECK(server.console().getVariable("DroppedBody") == std::to_string(*bodyId));
        CHECK(server.console().getVariable("DroppedCamera") == std::to_string(camera.objectId));
        CHECK(server.console().getVariable("DroppedBodyType") == "Player");
        CHECK(!sg.playerManager->findBaseRep(id));
        CHECK(!server.scriptPlugin().findClient(id));
        CHECK(!camera.resolve(*sg.manager));
        if (termination == Termination::Kick) {
            const auto* retained = dynamic_cast<Player*>(body.resolve(*sg.manager));
            CHECK(retained && retained->getOwnerClient() == 0);
            CHECK(!client.fearPlugin().isConnected());
            CHECK(client.console().getVariable("errorString") == "Disconnect regression");
        } else {
            CHECK(!body.resolve(*sg.manager));
            CHECK(termination == Termination::Timeout ? timedOut : disconnected);
        }
        // Later frames must neither repeat the callback nor remove a retained body.
        advance(16, false);
        CHECK(server.console().getVariable("DropCount") == "1");
    }
}

void disconnectShellResources() {
    TemporaryDirectory directory;
    CHECK(std::filesystem::create_directories(directory.path));
    for (const auto* name : {"shell", "mission", "ghost"}) {
        VolumeRWStream volume;
        const std::string contents = name;
        CHECK(volume.put(contents + ".txt", std::as_bytes(std::span(contents))));
        CHECK(volume.save(directory.path / (contents + ".vol")));
    }
    FearGameApp app;
    CHECK(app.init());
    CHECK(app.resources().setSearchPaths(std::array{directory.path}));
    CHECK(app.fearPlugin().createClientWorld());
    CHECK(app.fearPlugin().focusClient());
    CHECK(app.console()
              .evaluate(R"(
        newObject(ShellVolume, SimVolume, "shell.vol");
        newObject(MainWindow, "SimGui::Canvas");
        function EndGame() {
            $ConnectedToServer = false;
            $DisconnectResult = disconnect();
        }
    )")
              .ok());
    auto* shell = app.fearPlugin().clientGlobals().manager->findObject("ShellVolume");
    auto* canvas = app.fearPlugin().clientGlobals().manager->findObject("MainWindow");
    CHECK(shell && canvas);
    auto button = std::make_unique<FGUniversalButton>();
    auto* click = button.get();
    auto fields = click->fields();
    fields.command = "EndGame();";
    CHECK(click->configure(fields));
    CHECK(app.guiRuntime()->show(gui::Graph::create(std::move(button))));
    for ([[maybe_unused]] auto attempt : std::views::iota(0, 2)) {
        CHECK(app.fearPlugin().createServerWorld());
        CHECK(app.console().execute("localConnect").value == "True");
        CHECK(app.fearPlugin().focusClient());
        CHECK(app.console()
                  .evaluate(R"(
            newObject(MissionGroup, SimGroup);
            newObject(MissionVolume, SimVolume, "mission.vol");
            addToSet(MissionGroup, MissionVolume);
        )")
                  .ok());
        auto& manager = *app.fearPlugin().clientGlobals().manager;
        auto ghost = std::make_unique<sim::SimVolume>();
        ghost->assignName("GhostVolume");
        CHECK(ghost->setFileName("ghost.vol"));
        CHECK(ghost->setNetFlags(ghost->netFlags() | sim::SimNetObject::IsGhost));
        CHECK(manager.addObject(std::move(ghost)));
        CHECK(app.resources().contains("shell.txt") && app.resources().contains("mission.txt") &&
              app.resources().contains("ghost.txt"));
        CHECK(app.guiRuntime()->canvas.performClick(*click));
        CHECK(app.console().getVariable("DisconnectResult") == "True");
        CHECK(!app.fearPlugin().isConnected() && !app.fearPlugin().serverGlobals().world);
        const auto* replacement = app.fearPlugin().clientGlobals().manager;
        CHECK(replacement && replacement->findObject("ShellVolume") == shell &&
              replacement->findObject("MainWindow") == canvas);
        CHECK(!replacement->findObject("MissionGroup") && !replacement->findObject("GhostVolume"));
        CHECK(app.resources().contains("shell.txt"));
        CHECK(!app.resources().contains("mission.txt") && !app.resources().contains("ghost.txt"));
    }
}

gui::Active* disconnectButton(gui::Control& root) {
    if (auto* button = dynamic_cast<gui::Active*>(&root);
        button && button->fields().command == "EndGame();") {
        return button;
    }
    for (auto* object : root.objects()) {
        if (auto* control = dynamic_cast<gui::Control*>(object)) {
            if (auto* button = disconnectButton(*control)) {
                return button;
            }
        }
    }
    return nullptr;
}

void installedRemoteDisconnect() {
    const auto* assets = std::getenv("OPENT1_TEST_ASSETS");
    if (!assets || !*assets) {
        return;
    }
    for (const bool rpg : {false, true}) {
        const auto mod = std::filesystem::path(assets) / "RPG";
        if (rpg && !std::filesystem::exists(mod / "scripts.vol") &&
            !std::filesystem::exists(mod / "Server.cs")) {
            continue;
        }
        TemporaryDirectory serverDirectory, clientDirectory;
        FearGameApp::Config config;
        config.assetRoot = assets;
        config.writeRoot = serverDirectory.path;
        FearGameApp server(config);
        config.writeRoot = clientDirectory.path;
        FearGameApp client(config);
        const std::array<std::string_view, 2> args{"-mod", "RPG"};
        CHECK(rpg ? server.init(args) : server.init());
        server.console().setVariable("Server::Port", "0");
        server.console().setVariable("Server::Heartbeats", "false");
        CHECK(server.loadMission(rpg ? "rpgmap5" : "Raindance"));
        CHECK(rpg ? client.init(args) : client.init());
        client.console().setVariable("PCFG::Name", "RemoteDrop");
        client.console().setVariable("PCFG::Script", "");
        client.console().setVariable("PCFG::Info", "disconnect-test-password");
        client.console().setVariable("pref::PlayGameMode", "JOIN");
        const auto endpoint = net::parseIPv4Address(
            server.fearPlugin().serverGlobals().csDelegate->delegate()->localAddress());
        CHECK(endpoint);
        const std::array<std::string, 1> address{"IP:127.0.0.1:" + std::to_string(endpoint->port)};
        CHECK(client.console().execute("connect", address).value == "True");
        for (int frame = 0; frame < 512; ++frame) {
            CHECK(server.stepFrame(frame * .032, false));
            CHECK(client.stepFrame(frame * .032, false));
        }
        CHECK(client.fearPlugin().isConnected());
        const auto& cg = client.fearPlugin().clientGlobals();
        CHECK(cg.playerPSC && cg.playerPSC->getControlObject());
        auto& sg = server.fearPlugin().serverGlobals();
        const auto* remote = sg.playerManager->getClientList();
        while (remote && remote->name != "RemoteDrop") {
            remote = remote->nextClient;
        }
        CHECK(remote);
        const auto remoteId = remote->id;
        auto surface = gfx::SoftwareSurface::create(1280, 720);
        CHECK(surface && client.renderFrame(*surface));
        CHECK(client.setGameplayGuiMode(1));
        CHECK(client.escapeGameplay());
        auto runtime = client.guiRuntime();
        auto* button = disconnectButton(*runtime->canvas.top());
        CHECK(button && button->active);
        CHECK(client.renderFrame(*surface));
        auto* service = dynamic_cast<sim::SimInputManager*>(
            client.fearPlugin().clientGlobals().manager->findObject(sim::InputManagerId));
        CHECK(service);
        sim::SdlSimInput input(*service, 1);
        std::printf("Clicking remote %s disconnect\n", rpg ? "RPG" : "base");
        std::fflush(stdout);
        gui::Event mouse;
        mouse.point = button->localToGlobal(
            {button->fields().extent.x / 2, button->fields().extent.y / 2});
        mouse.type = gui::EventType::Move;
        CHECK(runtime->canvas.dispatch(mouse));
        mouse.type = gui::EventType::Down;
        CHECK(runtime->canvas.dispatch(mouse));
        CHECK(client.renderFrame(*surface));
        mouse.type = gui::EventType::Up;
        CHECK(runtime->canvas.dispatch(mouse));
        CHECK(!client.fearPlugin().isConnected());
        // The desktop host updates capture before it next rebinds input.
        input.release();
        input.capture(false);
        for (int frame = 512; frame < 528; ++frame) {
            CHECK(server.stepFrame(frame * .032, false));
            CHECK(client.stepFrame(frame * .032, false));
            CHECK(client.renderFrame(*surface));
        }
        CHECK(!sg.playerManager->findBaseRep(remoteId));
        CHECK(runtime->canvas.top() && runtime->canvas.top()->getName() == "JoinGameGui");
    }
}

void installedDisconnect() {
    const auto* assets = std::getenv("OPENT1_TEST_ASSETS");
    if (!assets ||
        !std::filesystem::is_regular_file(std::filesystem::path(assets) / "RPG/scripts.vol")) {
        return;
    }
    FearGameApp::Config config;
    config.assetRoot = assets;
    TemporaryDirectory directory;
    config.writeRoot = directory.path;
    FearGameApp app(config);
    const std::array<std::string_view, 2> args{"-mod", "RPG"};
    CHECK(app.init(args));
    app.console().setVariable("PCFG::Name", "DisconnectTest");
    app.console().setVariable("PCFG::Info", "disconnect-test");
    CHECK(app.loadMission("rpgmap5"));
    for (auto frame : std::views::iota(0, 8)) {
        CHECK(app.stepFrame(frame * .032));
    }
    CHECK(app.fearPlugin().focusServer());
    CHECK(app.console()
              .evaluate("processMenupickgroup(2049, 3); processMenupickclass(2049, 1);")
              .ok());
    for (auto frame : std::views::iota(8, 16)) {
        CHECK(app.stepFrame(frame * .032));
    }
    CHECK(app.setGameplayGuiMode(1));
    CHECK(app.guiRuntime()->canvas.top()->getName() == "playGui");
    CHECK(app.escapeGameplay());
    CHECK(app.guiRuntime()->canvas.top()->getName() == "LobbyGui");
    auto* button = disconnectButton(*app.guiRuntime()->canvas.top());
    CHECK(button && button->active && app.fearPlugin().isConnected());
    app.console().setVariable("pref::PlayGameMode", "HOST");
    CHECK(app.resources().contains("gui/MainMenu.gui"));
    CHECK(app.guiRuntime()->canvas.performClick(*button));
    CHECK(!app.fearPlugin().isConnected());
    CHECK(!app.fearPlugin().serverGlobals().world);
    CHECK(app.resources().contains("gui/MainMenu.gui"));
    CHECK(app.guiRuntime()->canvas.top()->getName() == "CreateServerGui");
    CHECK(std::ranges::none_of(app.console().log(), [](const auto& line) {
        return line.starts_with("GUI resource missing:");
    }));
}
}

int opent1_test_disconnect_shell_resources() {
    return opent1::test::run(disconnectShellResources);
}

int opent1_test_installed_disconnect() {
    return opent1::test::run(installedDisconnect);
}

int opent1_test_installed_remote_disconnect() {
    return opent1::test::run(installedRemoteDisconnect);
}

int opent1_test_remote_disconnect() {
    return opent1::test::run(remoteDisconnect);
}

int opent1_test_disconnect_send_failures() {
    return opent1::test::run(disconnectSendFailures);
}

OPENT1_REGISTER_TEST("game", "disconnect_send_failures", opent1_test_disconnect_send_failures);
OPENT1_REGISTER_TEST("game", "remote_disconnect", opent1_test_remote_disconnect);
OPENT1_REGISTER_TEST("game", "disconnect_shell_resources", opent1_test_disconnect_shell_resources);
OPENT1_REGISTER_TEST("fixpack", "installed_disconnect", opent1_test_installed_disconnect);
OPENT1_REGISTER_TEST(
    "disconnect_assets", "installed_remote_disconnect", opent1_test_installed_remote_disconnect);
