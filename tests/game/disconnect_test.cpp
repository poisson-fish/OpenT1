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
#include <thread>

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

struct ShellIdentity {
    sim::SimWorld* world;
    sim::SimManager* manager;
    UInt64 revision;
    std::vector<std::pair<sim::SimObjectId, UInt64>> services;

    explicit ShellIdentity(FearGameApp& app)
        : world(app.fearPlugin().clientGlobals().world),
          manager(app.fearPlugin().clientGlobals().manager),
          revision(app.game().worldRevision(sim::WorldType::Client)) {
        CHECK(world && manager);
        for (const auto id : {sim::InputManagerId, sim::ConsoleSchedulerId}) {
            auto* service = manager->findObject(id);
            CHECK(service);
            services.emplace_back(id, service->address().generation);
        }
        for (const auto* name : {"IRCClient", "MainWindow", "ShellVolume"}) {
            if (auto* service = manager->findObject(name)) {
                services.emplace_back(service->getId(), service->address().generation);
            }
        }
    }

    void check(FearGameApp& app) const {
        CHECK(app.fearPlugin().clientGlobals().world == world);
        CHECK(app.fearPlugin().clientGlobals().manager == manager);
        CHECK(app.game().worldRevision(sim::WorldType::Client) == revision);
        for (const auto& [id, generation] : services) {
            auto* service = manager->findObject(id);
            CHECK(service && service->address().generation == generation);
        }
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
        CHECK(peers.client.connectionCount() == 0);
        peers.a.sendResult = net::Result::Ok;
        CHECK(peers.client.connect("1", {}, 65, peers.clientId) == net::Result::Ok);
        CHECK(console.log().size() == 1);
        CHECK(console.log().front() ==
              std::format("Failed to send disconnect to 1 (network result {}).",
                          int(net::Result::IoError)));
    }
}

void disconnectQueuedEvents() {
    for (const auto result : {net::Result::Ok, net::Result::IoError}) {
        net_test::Pair peers;
        peers.connect();
        Console console;
        sim::SimWorld world(&console);
        auto owned = std::make_unique<sim::network::PacketStream>(
            std::make_shared<sim::network::Registry>(), false);
        auto* stream = owned.get();
        CHECK(world.manager().addObject(std::move(owned)));
        stream->attach(peers.client, peers.clientId);
        // Session::process can enqueue several events before a script in the
        // first packet replaces the connection. Even a successful close must
        // discard the remaining old events and pending send notifications.
        CHECK(peers.client.send(peers.clientId, net_test::bytes("outgoing"), 42, 1) ==
              net::Result::Ok);
        CHECK(peers.server.send(peers.serverId, net_test::bytes("queued"), 0, 1) ==
              net::Result::Ok);
        net_test::transfer(peers.b, peers.a);
        CHECK(peers.client.process(2) == net::Result::Ok);
        peers.a.sendResult = result;
        stream->disconnect(3);
        net::Event event;
        CHECK(!peers.client.poll(event));
        CHECK(peers.client.connectionCount() == 0);
        peers.a.sendResult = net::Result::Ok;
        CHECK(peers.client.connect("1", {}, 4, peers.clientId) == net::Result::Ok);
    }
}

void pendingDelegateDisconnect() {
    {
        auto transport = std::make_unique<net_test::Wire>("client");
        auto* wire = transport.get();
        sim::SimWorld world;
        auto client = std::make_unique<sim::network::CSDelegate>(
            std::move(transport), std::make_shared<sim::network::Registry>(), false);
        auto* connector = client.get();
        CHECK(world.manager().addObject(std::move(client)));
        CHECK(connector->connect("server", 0) == net::Result::Ok);
        wire->sendResult = net::Result::IoError;
        CHECK(connector->disconnect(2048, 1));
        wire->sendResult = net::Result::Ok;
        CHECK(connector->connect("server", 2) == net::Result::Ok);
    }
    for (const bool removeDelegate : {false, true}) {
        auto transports = net::makeLoopTransportPair();
        auto registry = std::make_shared<sim::network::Registry>();
        sim::SimWorld serverWorld, clientWorld;
        auto server =
            std::make_unique<sim::network::CSDelegate>(std::move(transports.first), registry, true);
        auto client = std::make_unique<sim::network::CSDelegate>(
            std::move(transports.second), registry, false);
        auto* listener = server.get();
        auto* connector = client.get();
        CHECK(serverWorld.manager().addObject(std::move(server)));
        CHECK(clientWorld.manager().addObject(std::move(client)));
        CHECK(connector->connect(listener->localAddress(), 0) == net::Result::Ok);
        CHECK(listener->process(0) == net::Result::Ok);
        CHECK(listener->stream(2049) && !connector->stream(2048));
        unsigned drops{};
        listener->onConnection = [&](const net::Event& event) {
            drops += event.type == net::EventType::Disconnected;
        };
        if (removeDelegate) {
            CHECK(connector->deleteObject());
        } else {
            CHECK(connector->disconnect(2048, 1));
            CHECK(!connector->disconnect(2048, 1));
            CHECK(connector->connect(listener->localAddress(), 1) == net::Result::Ok);
        }
        CHECK(listener->process(1) == (removeDelegate ? net::Result::Closed : net::Result::Ok));
        CHECK(drops == 1);
        CHECK(removeDelegate ? !listener->stream(2049) : listener->stream(2049) != nullptr);
        if (!removeDelegate) {
            // An old acceptance was already queued when we cancelled. It must
            // not complete or reject the replacement connection.
            CHECK(connector->process(2) == net::Result::Ok);
            CHECK(connector->stream(2048));
            CHECK(listener->process(2) == net::Result::Ok);
        }
    }
}

void remoteDisconnect() {
    enum class Termination { Command, Shutdown, Kick, Timeout };
    for (const auto termination :
         {Termination::Command, Termination::Shutdown, Termination::Kick, Termination::Timeout}) {
        std::printf("Disconnect termination mode: %d\n", static_cast<int>(termination));
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
        CHECK(client.fearPlugin().createClientWorld());
        const ShellIdentity shell(client);
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
                shell.check(client);
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
        // Real UDP delivery can lag a tight optimized simulation loop on macOS.
        // Allow kernel progress, as the other native-network tests do, without
        // accepting a missing callback or a different termination event below.
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (server.console().getVariable("DropCount").empty() &&
               std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            advance(1, termination == Termination::Kick);
        }
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
            shell.check(client);
        } else {
            CHECK(!body.resolve(*sg.manager));
            CHECK(termination == Termination::Timeout ? timedOut : disconnected);
        }
        // Later frames must neither repeat the callback nor remove a retained body.
        advance(16, false);
        CHECK(server.console().getVariable("DropCount") == "1");
    }
}

void cancelAndReconnect() {
    FearGameApp server, client;
    CHECK(server.init() && client.init());
    CHECK(server.fearPlugin().createServerWorld({}, true));
    CHECK(server.console()
              .evaluate(R"(
        function Server::onClientConnect(%id) { $JoinCount++; }
        function Server::onClientDisconnect(%id) { $DropCount++; }
    )")
              .ok());
    auto& sg = server.fearPlugin().serverGlobals();
    const auto endpoint = net::parseIPv4Address(sg.csDelegate->delegate()->localAddress());
    CHECK(endpoint);
    const std::array<std::string, 1> address{"IP:127.0.0.1:" + std::to_string(endpoint->port)};
    client.console().setVariable("PCFG::Name", "RejoiningPlayer");
    CHECK(client.fearPlugin().createClientWorld());
    const ShellIdentity shell(client);
    double time{};
    const auto advance = [&](bool pollClient) {
        time += .032;
        CHECK(server.stepFrame(time, false));
        if (pollClient)
            CHECK(client.stepFrame(time, false));
    };
    const std::array<std::string, 1> hostname{"IP:localhost:" + std::to_string(endpoint->port)};
    CHECK(client.console().execute("connect", hostname).value == "True");
    // The server has allocated a player, but the client has not polled the
    // acceptance yet. Cancelling ConnectingGui must still notify that server.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!sg.playerManager->getClientList()) {
        CHECK(std::chrono::steady_clock::now() < deadline);
        // Poll DNS before the server, stopping as soon as it accepts the
        // request so the client cannot consume that acceptance in this loop.
        time += .032;
        CHECK(client.stepFrame(time, false));
        CHECK(server.stepFrame(time, false));
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    CHECK(!client.fearPlugin().isConnected());
    for (int attempt = 1; attempt <= 3; ++attempt) {
        CHECK(client.console().execute("disconnect").value == "True");
        CHECK(client.console().execute("disconnect").value == "True");
        CHECK(!client.fearPlugin().isConnected());
        shell.check(client);
        // Rejoin before the server's next frame, without waiting for a timeout.
        CHECK(client.console().execute("connect", address).value == "True");
        for (int frame = 0; frame < 64; ++frame)
            advance(true);
        CHECK(client.fearPlugin().isConnected());
        CHECK(sg.playerManager->getNumClients() == 1);
        CHECK(sg.playerManager->getClientList()->name == "RejoiningPlayer");
        CHECK(server.console().getVariable("JoinCount") == std::to_string(attempt + 1));
        CHECK(server.console().getVariable("DropCount") == std::to_string(attempt));
    }
    CHECK(client.console().execute("disconnect").value == "True");
    for (int frame = 0; frame < 16; ++frame)
        advance(false);
    CHECK(sg.playerManager->getNumClients() == 0);
    CHECK(server.console().getVariable("DropCount") == "4");
    server.console().setVariable("Server::Password", "required");
    CHECK(client.console().execute("connect", address).value == "True");
    for (int frame = 0; frame < 64; ++frame)
        advance(true);
    CHECK(!client.fearPlugin().isConnected());
    shell.check(client);
    CHECK(client.console().getVariable("errorString") == "Connection to server rejected.");
}

void portalTransfers(bool installed, bool immediate = false) {
    const auto* assets = std::getenv("OPENT1_TEST_ASSETS");
    if (installed && (!assets || !std::filesystem::is_regular_file(std::filesystem::path(assets) /
                                                                   "config/SinConnect.cs"))) {
        return;
    }
    TemporaryDirectory firstDirectory, secondDirectory, clientDirectory;
    FearGameApp::Config config;
    if (installed) {
        config.assetRoot = assets;
    }
    config.writeRoot = firstDirectory.path;
    FearGameApp first(config);
    config.writeRoot = secondDirectory.path;
    FearGameApp second(config);
    config.writeRoot = clientDirectory.path;
    FearGameApp client(config);
    const std::array<std::string_view, 2> mod{"-mod", "RPG"};
    std::array<FearGameApp*, 2> servers{&first, &second};
    std::array<std::string, 2> endpoints;
    std::array<Int32, 2> hosted{};
    for (std::size_t i = 0; i < servers.size(); ++i) {
        auto& server = *servers[i];
        CHECK(installed ? server.init(mod) : server.init());
        server.console().setVariable("Server::Port", "0");
        server.console().setVariable("Server::Heartbeats", "false");
        const bool loaded = installed ? server.loadMission("rpgmap5")
                                      : server.fearPlugin().createServerWorld({}, true);
        if (!loaded) {
            for (const auto& line : server.console().log()) {
                if (line.find("fail") != std::string::npos ||
                    line.find("missing") != std::string::npos) {
                    std::fprintf(stderr, "%s\n", line.c_str());
                }
            }
        }
        CHECK(loaded);
        auto& sg = server.fearPlugin().serverGlobals();
        hosted[i] = sg.playerManager->getNumClients();
        const auto endpoint = net::parseIPv4Address(sg.csDelegate->delegate()->localAddress());
        CHECK(endpoint);
        endpoints[i] = "IP:127.0.0.1:" + std::to_string(endpoint->port);
    }
    CHECK(installed ? client.init(mod) : client.init());
    if (!client.fearPlugin().clientGlobals().world) {
        CHECK(client.fearPlugin().createClientWorld());
    }
    const ShellIdentity shell(client);
    client.console().setVariable("PCFG::Name", "PortalRegression");
    client.console().setVariable("PCFG::Info", "portal-regression-password");
    client.console().setVariable("PCFG::Script", "");
    client.console().setVariable("PortalImmediate", immediate ? "true" : "false");
    if (!installed) {
        // The portal delivers a remote event, then disconnects and joins in a
        // scheduler callback. Calling these commands outside dispatch misses
        // the original failure, even when using a real UDP transport.
        CHECK(client.console()
                  .evaluate(R"(
            function remoteSinBot(%server, %address) {
                if (%server != 2048) return;
                $PortalAddress = %address;
                if ($PortalImmediate) PortalTransfer();
                else schedule("PortalTransfer();", 0.3);
            }
            function PortalTransfer() {
                $PortalDisconnect = disconnect();
                $PortalConnect = connect($PortalAddress);
            }
        )")
                  .ok());
    } else {
        CHECK(client.console().isFunction("remoteSinBot"));
        CHECK(client.console().isFunction("Wasteworld::Transfer"));
    }
    unsigned accepted{};
    auto* delegate = client.fearPlugin().clientGlobals().csDelegate;
    const auto callback = delegate->onConnection;
    delegate->onConnection = [&, callback](const net::Event& event) {
        accepted += event.type == net::EventType::Accepted;
        callback(event);
    };
    double time{};
    const auto advance = [&] {
        time += .032;
        CHECK(first.stepFrame(time, false));
        CHECK(second.stepFrame(time, false));
        CHECK(client.stepFrame(time, false));
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    };
    const auto joined = [&](std::size_t destination, unsigned count) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
        for (;;) {
            auto& cg = client.fearPlugin().clientGlobals();
            const auto* stream = cg.csDelegate->delegate()->stream(2048);
            const auto ready = accepted == count && client.fearPlugin().isConnected() && stream &&
                               stream->transportAddress() == endpoints[destination] &&
                               (!installed || (cg.playerPSC && cg.playerPSC->getControlObject()));
            if (ready) {
                break;
            }
            CHECK(std::chrono::steady_clock::now() < deadline);
            advance();
        }
        shell.check(client);
        CHECK(client.console().getVariable("OpenT1::LastNetworkError").empty());
        for (std::size_t i = 0; i < servers.size(); ++i) {
            auto* players = servers[i]->fearPlugin().serverGlobals().playerManager;
            CHECK(players->getNumClients() == hosted[i] + (i == destination ? 1 : 0));
            Int32 matches{};
            for (auto* rep = players->getClientList(); rep; rep = rep->nextClient) {
                matches += rep->name == "PortalRegression";
            }
            CHECK(matches == (i == destination ? 1 : 0));
        }
    };
    CHECK(client.console().execute("connect", std::array{endpoints[0]}).value == "True");
    joined(0, 1);
    std::size_t source{};
    // Repeated cross-port transfers and a same-port portal both need teardown
    // before the next request, without recreating the shell or renaming players.
    for (const auto destination :
         {std::size_t{1}, std::size_t{0}, std::size_t{0}, std::size_t{1}}) {
        auto& server = *servers[source];
        CHECK(server.fearPlugin().focusServer());
        const auto id = client.fearPlugin().clientGlobals().manager->getId();
        const std::array<std::string, 3> args{std::to_string(id), "SinBot", endpoints[destination]};
        CHECK(server.console().execute("remoteEval", args).value == "True");
        joined(destination, accepted + 1);
        if (!installed) {
            CHECK(client.console().getVariable("PortalDisconnect") == "True");
            CHECK(client.console().getVariable("PortalConnect") == "True");
        }
        source = destination;
    }
    if (!installed) {
        // A destination rejecting the transfer must leave a reusable client,
        // including when the next attempt returns to the original port.
        first.console().setVariable("Server::Password", "required");
        auto& server = *servers[source];
        CHECK(server.fearPlugin().focusServer());
        const auto id = client.fearPlugin().clientGlobals().manager->getId();
        const std::array<std::string, 3> args{std::to_string(id), "SinBot", endpoints[0]};
        CHECK(server.console().execute("remoteEval", args).value == "True");
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (client.console().getVariable("OpenT1::LastNetworkError").empty()) {
            CHECK(std::chrono::steady_clock::now() < deadline);
            advance();
        }
        CHECK(!client.fearPlugin().isConnected());
        CHECK(client.console().getVariable("errorString") == "Connection to server rejected.");
        shell.check(client);
        first.console().setVariable("Server::Password", "");
    }
    // The ordinary menu/console recovery must still work after the portals.
    CHECK(client.console().execute("disconnect").value == "True");
    CHECK(client.console().execute("connect", std::array{endpoints[0]}).value == "True");
    joined(0, accepted + 1);
    CHECK(client.console().execute("disconnect").value == "True");
    for (int frame = 0; frame < 16; ++frame) {
        advance();
    }
    shell.check(client);
    for (std::size_t i = 0; i < servers.size(); ++i) {
        CHECK(servers[i]->fearPlugin().serverGlobals().playerManager->getNumClients() == hosted[i]);
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
            deleteServer();
        }
    )")
              .ok());
    auto* shell = app.fearPlugin().clientGlobals().manager->findObject("ShellVolume");
    auto* canvas = app.fearPlugin().clientGlobals().manager->findObject("MainWindow");
    CHECK(shell && canvas);
    const ShellIdentity identity(app);
    auto button = std::make_unique<FGUniversalButton>();
    auto* click = button.get();
    auto fields = click->fields();
    fields.command = "EndGame();";
    CHECK(click->configure(fields));
    CHECK(app.guiRuntime()->show(gui::Graph::create(std::move(button))));
    CHECK(app.stepFrame(0, false));
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
        CHECK(sim::SimConsoleEvent::schedule(
            *manager.findObject("MissionGroup"), "$StaleMissionCallback = 1;", .032f));
        CHECK(sim::SimConsoleEvent::schedule(
            *manager.findObject(sim::ConsoleSchedulerId), "$ShellCallback++;", .032f));
        auto ghost = std::make_unique<sim::SimVolume>();
        ghost->assignName("GhostVolume");
        CHECK(ghost->setFileName("ghost.vol"));
        CHECK(ghost->setNetFlags(ghost->netFlags() | sim::SimNetObject::IsGhost));
        CHECK(manager.addObject(std::move(ghost)));
        CHECK(app.resources().contains("shell.txt") && app.resources().contains("mission.txt") &&
              app.resources().contains("ghost.txt"));
        CHECK(app.guiRuntime()->canvas.performClick(*click));
        CHECK(app.console().getVariable("DisconnectResult") == "True");
        identity.check(app);
        CHECK(!app.fearPlugin().isConnected() && !app.fearPlugin().serverGlobals().world);
        const auto* replacement = app.fearPlugin().clientGlobals().manager;
        CHECK(replacement && replacement->findObject("ShellVolume") == shell &&
              replacement->findObject("MainWindow") == canvas);
        CHECK(!replacement->findObject("MissionGroup") && !replacement->findObject("GhostVolume"));
        CHECK(app.resources().contains("shell.txt"));
        CHECK(!app.resources().contains("mission.txt") && !app.resources().contains("ghost.txt"));
        CHECK(app.stepFrame((attempt + 1) * .064, false));
        CHECK(app.console().getVariable("StaleMissionCallback").empty());
        CHECK(app.console().getVariable("ShellCallback") == std::to_string(attempt + 1));
    }
}

void persistentLocalWorld() {
    FearGameApp app;
    CHECK(app.init());
    CHECK(app.fearPlugin().createServerWorld({}, true));
    CHECK(app.fearPlugin().createClientWorld());
    const ShellIdentity shell(app);
    auto& sg = app.fearPlugin().serverGlobals();
    auto* server = sg.world;
    CHECK(app.fearPlugin().focusServer());
    CHECK(app.console()
              .evaluate(R"(
        PlayerData PersistentArmor { mass = 9; };
        function Server::onClientDisconnect(%id) { $DropCount++; }
    )")
              .ok());
    for (int attempt = 1; attempt <= 3; ++attempt) {
        CHECK(app.console().execute("localConnect").value == "True");
        auto* rep = sg.playerManager->getClientList();
        CHECK(rep && !rep->nextClient);
        const auto id = rep->id;
        const auto body = app.fearPlugin().spawnPlayer("PersistentArmor", {}, {}, id);
        CHECK(body);
        const auto bodyAddress = sg.manager->findObject(*body)->address();
        CHECK(app.fearPlugin().syncLoopbackState() > 0);
        CHECK(app.console().execute("disconnect").value == "True");
        CHECK(app.console().execute("disconnect").value == "True");
        shell.check(app);
        CHECK(sg.world == server && sg.playerManager->getNumClients() == 0);
        CHECK(!sg.manager->findObject(id) && !bodyAddress.resolve(*sg.manager));
        CHECK(app.console().getVariable("DropCount") == std::to_string(attempt));
        CHECK(app.fearPlugin().clientGlobals().manager->getId() == 0);
        CHECK(!app.fearPlugin().clientGlobals().playerPSC->getControlObject());
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
        const auto hostedClients =
            server.fearPlugin().serverGlobals().playerManager->getNumClients();
        CHECK(rpg ? client.init(args) : client.init());
        const ShellIdentity shell(client);
        client.console().setVariable("PCFG::Name", "RemoteDrop");
        client.console().setVariable("PCFG::Script", "");
        client.console().setVariable("PCFG::Info", "disconnect-test-password");
        client.console().setVariable("pref::PlayGameMode", "JOIN");
        const auto endpoint = net::parseIPv4Address(
            server.fearPlugin().serverGlobals().csDelegate->delegate()->localAddress());
        CHECK(endpoint);
        const std::array<std::string, 1> address{"IP:127.0.0.1:" + std::to_string(endpoint->port)};
        for (int attempt = 0; attempt < 2; ++attempt) {
            client.console().setVariable("Server::Address", address.front());
            CHECK(client.console().execute("JoinGame").ok());
            for (int frame = 0; frame < 512; ++frame) {
                CHECK(server.stepFrame((attempt * 528 + frame) * .032, false));
                CHECK(client.stepFrame((attempt * 528 + frame) * .032, false));
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
            CHECK(sg.playerManager->getNumClients() == hostedClients + 1);
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
            shell.check(client);
            // The desktop host updates capture before it next rebinds input.
            input.release();
            input.capture(false);
            for (int frame = 512; frame < 528; ++frame) {
                CHECK(server.stepFrame((attempt * 528 + frame) * .032, false));
                CHECK(client.stepFrame((attempt * 528 + frame) * .032, false));
            }
            CHECK(client.renderFrame(*surface));
            CHECK(sg.playerManager->getNumClients() == hostedClients);
            CHECK(!sg.playerManager->findBaseRep(remoteId));
            CHECK(runtime->canvas.top() && runtime->canvas.top()->getName() == "JoinGameGui");
        }
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
    return opent1::test::run([] {
        disconnectShellResources();
        persistentLocalWorld();
    });
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
    return opent1::test::run([] {
        disconnectSendFailures();
        disconnectQueuedEvents();
    });
}

int opent1_test_cancel_and_reconnect() {
    return opent1::test::run([] {
        pendingDelegateDisconnect();
        cancelAndReconnect();
    });
}

int opent1_test_portal_transfers() {
    return opent1::test::run([] {
        portalTransfers(false);
        portalTransfers(false, true);
    });
}

int opent1_test_installed_portal_transfers() {
    return opent1::test::run([] {
        portalTransfers(true);
    });
}

OPENT1_REGISTER_TEST("game", "portal_transfers", opent1_test_portal_transfers);
OPENT1_REGISTER_TEST("disconnect_assets",
                     "installed_portal_transfers",
                     opent1_test_installed_portal_transfers);
OPENT1_REGISTER_TEST("game", "cancel_and_reconnect", opent1_test_cancel_and_reconnect);
OPENT1_REGISTER_TEST("game", "disconnect_send_failures", opent1_test_disconnect_send_failures);
OPENT1_REGISTER_TEST("game", "remote_disconnect", opent1_test_remote_disconnect);
OPENT1_REGISTER_TEST("game", "disconnect_shell_resources", opent1_test_disconnect_shell_resources);
OPENT1_REGISTER_TEST("fixpack", "installed_disconnect", opent1_test_installed_disconnect);
OPENT1_REGISTER_TEST("disconnect_assets",
                     "installed_remote_disconnect",
                     opent1_test_installed_remote_disconnect);
