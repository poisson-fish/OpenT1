#include "sim_net_test_support.hpp"
#include <opent1/core/mem_stream.hpp>
#include <opent1/game/fear_cs_delegate.hpp>
#include <opent1/game/fear_player_psc.hpp>
#include <opent1/game/player_manager.hpp>
#include <opent1/game/chat_inventory_huds.hpp>
#include <opent1/game/game_base.hpp>
#include <opent1/game/string_table.hpp>
#include <opent1/game/game_app.hpp>
#include <opent1/game/status_combat_huds.hpp>
#include <opent1/game/observer_camera.hpp>
#include <opent1/game/item.hpp>
#include <opent1/game/marker.hpp>
#include <opent1/world/scene_objects.hpp>
#include <opent1/game/sound.hpp>
#include <opent1/game/sensor_manager.hpp>
#include <opent1/sim/sim_resource_volume.hpp>
#include <opent1/game/server_irc_controls.hpp>
#include <chrono>
#include <cstring>
#include <thread>
#include <fstream>
#include <iterator>
#include <limits>
#include <ranges>

using namespace opent1::game;
using namespace sim_net_test;

namespace {
void dedicatedStartupPort() {
    const auto root = std::filesystem::temp_directory_path() /
                      std::format("opent1-dedicated-{}",
                                  std::chrono::steady_clock::now().time_since_epoch().count());
    struct Cleanup {
        std::filesystem::path root;
        ~Cleanup() {
            std::error_code ec;
            std::filesystem::remove_all(root, ec);
        }
    } cleanup{root};
    std::filesystem::create_directories(root / "base");
    std::filesystem::create_directories(root / "config");
    auto reservation = net::makeUdpTransport({{0, 0, 0, 0}, 0});
    const auto port = net::parseIPv4Address(reservation->localAddress())->port;
    {
        std::ofstream bootstrap(root / "console.cs");
        bootstrap << R"(
            $dedicated = true;
            newServer(); focusServer();
            newObject(StartupObject, SimObject);
            $ConsoleWorld::DefaultSearchPath = "config;base";
            exec("ServerPrefs.cs");
            $MissionName = "StartupMission";
        )";
        std::ofstream preferences(root / "config/ServerPrefs.cs");
        preferences << "$Server::Port = " << port << ";"
                    << "$Server::HostName = \"Dedicated startup test\";";
        CHECK(bool(bootstrap) && bool(preferences));
    }
    FearGameApp::Config config;
    config.assetRoot = root;
    {
        // A port conflict must fail startup instead of leaving an ephemeral
        // listener running behind a successful dedicated-server message.
        FearGameApp occupied(config);
        CHECK(!occupied.init());
        CHECK(occupied.console().getVariable("OpenT1::StartupError").find(std::to_string(port)) !=
              std::string::npos);
    }
    reservation.reset();
    FearGameApp app(config);
    CHECK(app.init());
    auto& globals = app.fearPlugin().serverGlobals();
    CHECK(globals.manager && globals.manager->findObject("StartupObject"));
    CHECK(!app.fearPlugin().clientGlobals().manager);
    CHECK(app.console().getVariable("Server::Dedicated") == "true");
    CHECK(app.console().getVariable("MissionName") == "StartupMission");
    CHECK(net::parseIPv4Address(globals.csDelegate->delegate()->localAddress())->port == port);
    auto client = net::makeUdpTransport({{127, 0, 0, 1}, 0});
    ServerBrowser browser(*client, false);
    bool replied{};
    browser.gotPingInfo = [&](const auto& info) {
        replied = info.name == "Dedicated startup test" && info.players == 0;
    };
    CHECK(browser.ping("IP:127.0.0.1:" + std::to_string(port)));
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    for (UInt32 frame = 0; std::chrono::steady_clock::now() < deadline; ++frame) {
        CHECK(app.stepFrame(frame * .032, false));
        browser.process(frame * 32);
        net::Datagram packet;
        while (client->receive(packet) == net::Result::Ok) {
            CHECK(browser.receive(packet.address, packet.data(), frame * 32));
        }
        if (replied)
            break;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    CHECK(replied);
    // Later hosted worlds also honor the selected installation's port even
    // though console.cs is outside the active resource search directories.
    CHECK(app.fearPlugin().createServerWorld({}, true));
    CHECK(net::parseIPv4Address(globals.csDelegate->delegate()->localAddress())->port == port);
}

void localProfileDelivery() {
    FearGameApp app;
    CHECK(app.init());
    auto& plugin = app.fearPlugin();
    auto& console = app.console();
    CHECK(plugin.createServerWorld({}, false));
    auto* players = plugin.serverGlobals().playerManager;
    CHECK(!players->findClient(2049));
    CHECK(players->getNumClients() == 0 && players->getNumBaseReps() == 0);
    const auto bot = players->aiAdded("Mission startup bot", "male1", "base", true);
    CHECK(bot == 2049);
    CHECK(!players->findClient(bot));
    CHECK(players->findBaseRep(bot) && !players->findBaseRep(bot)->isClientRep());
    console.setVariable("PCFG::Name", "LocalProfile");
    console.setVariable("PCFG::Info", "dummy RPG password \"quoted\" \\ value");
    console.setVariable("PCFG::RealName", "Profile Name");
    CHECK(console
              .evaluate(R"(
        function Server::onClientConnect(%client) {
            $profileConnectCount++;
            $profileConnectWorld = getManagerId();
            $profileAddress = Client::getTransportAddress(%client);
            $Client::info[%client, 5] = "";
        }
        function onConnection(%message) {
            $profileAcceptedCount++;
            $profileAcceptedWorld = getManagerId();
            $profileAcceptedMessage = %message;
            remoteEval(2048, "SetCLInfo", $PCFG::SkinBase, $PCFG::RealName,
                       $PCFG::EMail, $PCFG::Tribe, $PCFG::URL, $PCFG::Info,
                       $pref::autoWaypoint, $pref::noEnterInvStation, $pref::messageMask);
        }
        function remoteSetCLInfo(%client, %skin, %name, %email, %tribe, %url,
                                 %info, %autowp, %enterInv, %msgMask) {
            $profileSender = %client;
            $profileName = %name;
            $Client::info[%client, 5] = %info;
        }
        function dataFinished() {
            $profileAtDataFinished = $Client::info[$profileSender, 5];
        }
    )")
              .ok());
    // connect(LOOPBACK) must run the same script handshake as a remote join,
    // before datablock completion can trigger RPG's initial mission drop.
    CHECK(console.evaluate("connect(\"LOOPBACK:28001\");").value == "True");
    const auto localId = Int32(plugin.clientGlobals().manager->getId());
    CHECK(localId == 2050);
    CHECK(players->findClient(localId)->name == "LocalProfile");
    CHECK(players->findClient(localId)->isClientRep());
    CHECK(players->findClient(localId)->playerPSC == plugin.serverGlobals().playerPSC);
    CHECK(players->findBaseRep(bot)->name == "Mission startup bot");
    CHECK(!players->findBaseRep(bot)->isClientRep());
    CHECK(players->getNumClients() == 1 && players->getNumBaseReps() == 2);
    CHECK(console.getVariable("profileConnectCount") == "1");
    CHECK(console.getVariable("profileAcceptedCount") == "1");
    CHECK(console.getVariable("profileAcceptedMessage") == "Accepted");
    CHECK(console.getVariable("profileConnectWorld") ==
          std::to_string(plugin.serverGlobals().manager->getId()));
    CHECK(console.getVariable("profileAcceptedWorld") == std::to_string(localId));
    CHECK(console.getVariable("profileAddress") == "LOOPBACK:28001");
    CHECK(console.getVariable("profileSender") == std::to_string(localId));
    CHECK(console.getVariable("profileName") == "Profile Name");
    CHECK(console.getVariable("profileAtDataFinished") == console.getVariable("PCFG::Info"));
    CHECK(plugin.focusServer());
    CHECK(console.evaluate("PlayerData LocalProfileArmor { mass = 9; };").ok());
    CHECK(plugin.spawnPlayer("LocalProfileArmor", {0, 0, 100}, {}, localId));
    CHECK(players->findClient(localId)->ownedObject);
    CHECK(players->findClient(localId)->ownedObject->getOwnerClient() == localId);
    CHECK(!plugin.spawnPlayer("LocalProfileArmor", {}, {}, bot));
    CHECK(console.execute("localConnect").value == "True");
    CHECK(console.getVariable("profileConnectCount") == "1");
    CHECK(console.getVariable("profileAcceptedCount") == "1");
    // Rehosting creates a new server-side client even when the client world
    // and loopback address survive. It must send the current profile again.
    CHECK(plugin.createServerWorld({}, false));
    console.setVariable("PCFG::Info", "");
    CHECK(console.execute("localConnect").value == "True");
    CHECK(console.getVariable("profileConnectCount") == "2");
    CHECK(console.getVariable("profileAcceptedCount") == "2");
    CHECK(console.getVariable("profileAtDataFinished").empty());
    CHECK(plugin.clientGlobals().manager->getId() == 2049);
    CHECK(plugin.clientGlobals().playerManager->findClient(2049));
    CHECK(!plugin.clientGlobals().playerManager->findBaseRep(bot + 1));
}

void localObserverMenuState() {
    FearGameApp app;
    CHECK(app.init());
    auto& plugin = app.fearPlugin();
    CHECK(plugin.createServerWorld({}, true));
    CHECK(app.console().execute("localConnect").value == "True");
    auto* server = plugin.serverGlobals().manager;
    auto camera = std::make_unique<ObserverCamera>(2049);
    auto* observer = server->addObject(std::move(camera));
    CHECK(observer);
    const std::array<std::string, 2> control{"2049", std::to_string(observer->getId())};
    CHECK(app.console().execute("Client::setControlObject", control).value == "True");
    CHECK(dynamic_cast<GameBase*>(observer)->getControlClient() == 2049);
    CHECK(app.console()
              .evaluate("Client::setGuiMode(2049, 1); Client::setMenuScoreVis(2049, true);")
              .ok());
    std::vector<std::string> events;
    plugin.onPlayerControl = [&] {
        events.emplace_back("control");
    };
    plugin.onGuiMode = [&](UInt32 mode) {
        events.emplace_back("mode" + std::to_string(mode));
    };
    plugin.onScoresVisible = [&](bool visible) {
        events.emplace_back(visible ? "show" : "hide");
    };
    CHECK(plugin.syncLoopbackState() > 0);
    CHECK(dynamic_cast<ObserverCamera*>(plugin.clientGlobals().playerPSC->getControlObject()));
    CHECK(plugin.clientGlobals().playerPSC->scoresVisible());
    CHECK(!events.empty() && events.back() == "show");
    CHECK(std::find(events.begin(), events.end(), "control") != events.end());
    events.clear();
    CHECK(plugin.syncLoopbackState() > 0);
    CHECK(events.empty());
    CHECK(app.console().evaluate("Client::setMenuScoreVis(2049, false);").ok());
    (void)plugin.syncLoopbackState();
    CHECK(!plugin.clientGlobals().playerPSC->scoresVisible());
    const std::array<std::string, 2> clear{"2049", "-1"};
    CHECK(app.console().execute("Client::setControlObject", clear).value == "True");
    CHECK(dynamic_cast<GameBase*>(observer)->getControlClient() == 0);
    CHECK(!plugin.serverGlobals().playerPSC->getControlObject());
}

void scriptObserverTransforms() {
    FearGameApp app;
    CHECK(app.init());
    auto& plugin = app.fearPlugin();
    CHECK(plugin.createServerWorld({}, true));
    CHECK(app.console().execute("localConnect").value == "True");
    CHECK(plugin.focusServer());
    auto marker = std::make_unique<Marker>();
    marker->assignName("ObserverMarker");
    marker->setLinearPosition({-2500, -260, 120});
    marker->setAngPosition({.15f, -.2f, -.94f});
    auto* source = dynamic_cast<Marker*>(plugin.serverGlobals().manager->addObject(std::move(marker)));
    CHECK(source);
    CHECK(app.console().evaluate(R"(
        $observerCamera = Client::getObserverCamera(2049);
        Client::setControlObject(2049, $observerCamera);
        Observer::setFlyMode(2049, GameBase::getPosition(ObserverMarker),
                            GameBase::getRotation(ObserverMarker), false, true);
    )").ok());
    auto* camera = dynamic_cast<ObserverCamera*>(plugin.serverGlobals().playerPSC->getControlObject());
    CHECK(camera && camera->mode() == ObserverCamera::FreeRotateMode);
    const auto matches = [&](const TMat3F& actual) {
        CHECK((actual.p - source->getLinearPosition()).len() < .001f);
        for (auto row : std::views::iota(0, 3)) {
            for (auto col : std::views::iota(0, 3)) {
                CHECK(std::abs(actual.m[row][col] - source->getTransform().m[row][col]) < .00001f);
            }
        }
    };
    matches(camera->getEyeTransform());
    CHECK(plugin.syncLoopbackState() > 0);
    auto* client = dynamic_cast<ObserverCamera*>(plugin.clientGlobals().playerPSC->getControlObject());
    CHECK(client && client->mode() == ObserverCamera::FreeRotateMode);
    matches(client->getEyeTransform());
    // Disabled translation must preserve the scripted position while looking around.
    PlayerMove move;
    move.forwardAction = 1;
    move.turnRot = .1f;
    client->update(0, 32, &move);
    CHECK((client->getLinearPosition() - source->getLinearPosition()).len() < .001f);
    CHECK(app.console()
              .evaluate("Observer::setOrbitPoint(2049, \"10 20 30\", 5, 2, 8, \"0 0 .3\");")
              .value == "True");
    CHECK(camera->mode() == ObserverCamera::OrbitPointMode);
    CHECK(camera->curOrbitPoint() == Point3F(10, 20, 30));
    CHECK(camera->orbitDist() == 5 && camera->minOrbitDist() == 2 && camera->maxOrbitDist() == 8);
    CHECK(plugin.syncLoopbackState() > 0);
    CHECK(client->mode() == ObserverCamera::OrbitPointMode && client->curOrbitPoint() == Point3F(10, 20, 30));
    CHECK(app.console()
              .evaluate("Observer::setFlyMode(2049, \"1 2 3\", \"0 0 0\", false, false);")
              .value == "True");
    CHECK(camera->mode() == ObserverCamera::StationaryMode && camera->getLinearPosition() == Point3F(1, 2, 3));
}

void scriptCreatedItems() {
    FearGameApp app;
    CHECK(app.init());
    auto& plugin = app.fearPlugin();
    CHECK(plugin.createServerWorld({}, true));
    CHECK(plugin.focusServer());
    CHECK(app.console()
              .evaluate(R"(
        ItemData TestTownBot { description = "Town bot"; mass = 2; };
        $createdItem = newObject("ScriptTownBot", Item, TestTownBot, 3, false, true, false);
    )")
              .ok());
    auto* item = dynamic_cast<Item*>(plugin.serverGlobals().manager->findObject("ScriptTownBot"));
    CHECK(item && item->getDatPtr() && item->getDatFileId() >= 0);
    CHECK(item->getDatFileName() == "TestTownBot");
    CHECK(item->getCount() == 3 && item->isRotating() && !item->isCollideable());
    CHECK(app.console()
              .evaluate("return newObject(\"BadTownBot\", Item, MissingData, 1, false);")
              .value == "False");
    CHECK(!plugin.serverGlobals().manager->findObject("BadTownBot"));
}

void localScriptKick() {
    FearGameApp app;
    CHECK(app.init());
    auto& plugin = app.fearPlugin();
    CHECK(plugin.createServerWorld({}, true));
    CHECK(plugin.serverGlobals().playerManager->aiAdded("Startup NPC", "male1", "base", true) ==
          2049);
    CHECK(app.console().execute("localConnect").value == "True");
    CHECK(plugin.clientGlobals().manager->getId() == 2050);
    CHECK(app.console()
              .evaluate(R"(
        function Server::onClientDisconnect(%id) { $droppedClient = %id; }
        function onConnection(%state) { $connectionState = %state; }
        schedule("Net::kick(2050, \"Dummy profile rejection\");", 0);
    )")
              .ok());
    CHECK(app.stepFrame(0));
    CHECK(!plugin.isConnected() && plugin.serverGlobals().manager &&
          plugin.clientGlobals().manager);
    CHECK(!plugin.serverGlobals().playerManager->findClient(2050));
    CHECK(plugin.serverGlobals().playerManager->findBaseRep(2049));
    CHECK(app.console().getVariable("ConnectedToServer") == "false");
    CHECK(app.console().getVariable("droppedClient") == "2050");
    CHECK(app.console().getVariable("connectionState") == "Dropped");
    CHECK(app.console().getVariable("errorString") == "Dummy profile rejection");
}

void nativeSkinDelivery() {
    FearGameApp app;
    CHECK(app.init());
    auto& plugin = app.fearPlugin();
    CHECK(plugin.createServerWorld({}, true));
    CHECK(app.console().execute("localConnect").value == "True");
    auto* server = plugin.serverGlobals().playerManager;
    auto* host = plugin.clientGlobals().playerManager;
    auto protocol = std::make_shared<Registry>();
    CHECK(registerPlayerManagerEvents(*protocol));
    Endpoint from(protocol, true), to(protocol);
    auto* remote = add<PlayerManager>(to.manager(), false);
    CHECK(remote->clientAdded("Host", "male1", "base", true, 2049) == 2049);
    for (auto* players : {server, host, remote}) {
        CHECK(players->clientAdded("Peer", "female1", "base", false, 2050) == 2050);
    }
    auto* peer = server->findClient(2050);
    peer->packetStream = from.stream;
    for (const auto id : {"2049", "2050"}) {
        for (const auto skin : {"storm", "base", "beagle"}) {
            const std::array<std::string, 2> args{id, skin};
            CHECK(app.console().execute("Client::setSkin", args).value == "True");
            deliver(*from.stream, *to.stream);
            const auto clientId = std::string_view(id) == "2049" ? 2049 : 2050;
            CHECK(server->findClient(clientId)->skinBase == skin);
            CHECK(host->findClient(clientId)->skinBase == skin);
            CHECK(remote->findClient(clientId)->skinBase == skin);
        }
    }
    // The existing event-manager transport must receive the same native event.
    peer->packetStream = nullptr;
    peer->eventManager = from.events;
    server->setClientSkin(2049, "storm");
    deliver(*from.stream, *to.stream);
    CHECK(host->findClient(2049)->skinBase == "storm");
    CHECK(remote->findClient(2049)->skinBase == "storm");
    peer->eventManager = nullptr;
}

void nativeChatDelivery() {
    auto protocol = std::make_shared<Registry>();
    CHECK(registerPlayerManagerEvents(*protocol));
    Endpoint from(protocol, true), to(protocol);
    Console console;
    auto* client = add<PlayerManager>(to.manager(), false);
    client->bindConsole(&console);
    std::vector<std::pair<Int32, std::string>> messages;
    client->onChatMessage = [&](Int32 type, std::string_view message) {
        messages.emplace_back(type, message);
    };
    const auto send = [&](auto event) {
        event->flags = SimEvent::Guaranteed | SimEvent::Ordered;
        CHECK(from.stream->sendRemoteEvent(std::move(event)));
        deliver(*from.stream, *to.stream);
    };
    auto joined = std::make_unique<PlayerAddEvent>();
    joined->id = 2049;
    joined->name = "OpenT1 Player";
    joined->skinBase = "base";
    joined->voiceBase = "male1";
    send(std::move(joined));
    CHECK(client->findClient(2049)->name == "OpenT1 Player");
    CHECK(messages.back().first == FearGuiChatDisplay::MSG_System &&
          messages.back().second == "OpenT1 Player connected to the game.");
    client->teamAdded("Blood Eagle");
    client->teamAdded("Diamond Sword");
    send(std::make_unique<PlayerTeamChangeEvent>(2049, 1));
    CHECK(messages.back().second == "OpenT1 Player joined team Diamond Sword.");
    CHECK(console
              .evaluate("function onClientMessage(%client, %message) { $chatSender = %client; "
                        "$chatBody = %message; return %message != \"hidden\"; }")
              .ok());
    const auto say = [&](Int32 sender, UInt32 type, std::string_view message) {
        auto event = std::make_unique<PlayerSayEvent>();
        event->sender = sender;
        event->msgType = type;
        event->message = message;
        send(std::move(event));
    };
    say(2049, FearGuiChatDisplay::MSG_Chat, "Hello \"Tribes\"");
    CHECK(messages.back().second == "OpenT1 Player: Hello \"Tribes\"");
    CHECK(console.getVariable("chatSender") == "2049" &&
          console.getVariable("chatBody") == "Hello \"Tribes\"");
    say(2049, FearGuiChatDisplay::MSG_TeamChat, "Defend~wdef");
    CHECK(messages.back().first == FearGuiChatDisplay::MSG_TeamChat &&
          messages.back().second == "OpenT1 Player: Defend");
    say(0, FearGuiChatDisplay::MSG_Game, "Match started.");
    CHECK(messages.back().first == FearGuiChatDisplay::MSG_Game &&
          messages.back().second == "Match started.");
    CHECK(console.getVariable("chatSender") == "0");
    const auto count = messages.size();
    say(2049, FearGuiChatDisplay::MSG_Chat, "hidden");
    CHECK(messages.size() == count);

    // comchat.cs supplies the optional sender to the same native event on
    // both the local host and remote clients.
    FearGameApp app;
    CHECK(app.init());
    auto& plugin = app.fearPlugin();
    CHECK(plugin.createServerWorld({}, true));
    CHECK(app.console().execute("localConnect").value == "True");
    auto* serverRep = plugin.serverGlobals().playerManager->findClient(2049);
    CHECK(serverRep);
    auto* localPlayers = plugin.clientGlobals().playerManager;
    localPlayers->onChatMessage = [&](Int32 type, std::string_view message) {
        messages.emplace_back(type, message);
    };
    const std::array<std::string, 4> arguments{"2049", "2", "From OpenT1", "2049"};
    CHECK(app.console().execute("Client::sendMessage", arguments).value == "True");
    CHECK(messages.back().second == localPlayers->findClient(2049)->name + ": From OpenT1");
    serverRep->packetStream = from.stream;
    CHECK(app.console().execute("Client::sendMessage", arguments).value == "True");
    deliver(*from.stream, *to.stream);
    CHECK(messages.back().second == "OpenT1 Player: From OpenT1");
    CHECK(app.console().execute("Client::sendMessage", std::span(arguments).first(3)).value ==
          "True");
    deliver(*from.stream, *to.stream);
    CHECK(messages.back().second == "From OpenT1");
    serverRep->packetStream = nullptr;

    send(std::make_unique<PlayerTeamChangeEvent>(2049, -1));
    CHECK(messages.back().second == "OpenT1 Player is no longer on a team.");
    send(std::make_unique<PlayerRemoveEvent>(2049));
    CHECK(messages.back().second == "OpenT1 Player dropped.");
    CHECK(!client->findClient(2049));
    CHECK(!plugin.serverGlobals().playerManager->processEvent(PlayerSayEvent{}));
}

void nativeTeamDelivery() {
    Console console;
    auto protocol = std::make_shared<Registry>();
    CHECK(registerPlayerManagerEvents(*protocol));
    Endpoint from(protocol, true), to(protocol);
    auto* server = add<PlayerManager>(from.manager(), true);
    auto* client = add<PlayerManager>(to.manager(), false);
    server->bindConsole(&console);
    CHECK(server->clientAdded("Peer", "male1", "base", true, 2049) == 2049);
    server->findClient(2049)->packetStream = from.stream;
    console.setVariable("Server::teamSkin/", "neutral");
    console.setVariable("Server::teamName0", "Citizen");
    console.setVariable("Server::teamSkin0", "rpghuman0");
    server->teamAdded(0);
    deliver(*from.stream, *to.stream);
    CHECK(client->getNumTeams() == 1);
    CHECK(client->getTeamSkinBase(-1) == "neutral");
    CHECK(client->findTeam(0)->name == "Citizen");
    CHECK(client->getTeamSkinBase(0) == "rpghuman0");

    // The named overload must publish through the event-manager path as well.
    server->findClient(2049)->packetStream = nullptr;
    server->findClient(2049)->eventManager = from.events;
    server->teamAdded("Elf", "rpgelf");
    deliver(*from.stream, *to.stream);
    CHECK(client->getNumTeams() == 2);
    CHECK(client->findTeam(1)->name == "Elf");
    CHECK(client->getTeamSkinBase(1) == "rpgelf");
    server->findClient(2049)->eventManager = nullptr;
    server->findClient(2049)->packetStream = from.stream;

    // A mission reset must precede the new neutral and playable team records.
    console.setVariable("Server::teamSkin/", "newneutral");
    console.setVariable("Server::teamSkin0", "rpgscalemail");
    server->missionReset();
    server->teamAdded(0);
    deliver(*from.stream, *to.stream);
    CHECK(client->getNumTeams() == 1);
    CHECK(client->getTeamSkinBase(-1) == "newneutral");
    CHECK(client->getTeamSkinBase(0) == "rpgscalemail");
    CHECK(!client->findTeam(1));
}

void nativeLightGhosts() {
    Endpoint receiver;
    const auto decode = [&](Int32 type, Flt32 range, Point3F direction) {
        // Original SimLight sends all fields on creation, including fields
        // its constructor never initializes for this particular light type.
        std::array<std::byte, 128> bytes{};
        BitStream output{std::span(bytes)};
        output.writeFlag(true);
        output.writeFlag(false);
        output.writeFlag(true);
        output.writeInt(type, 3);
        output.writeFlag(true);
        output.write(range);
        for (const auto value :
             {Point3F{.5f, .5f, .5f}, Point3F{.1f, .1f, .1f}, Point3F{1, 2, 3}, direction}) {
            output.writeFlag(true);
            output.write(value.x);
            output.write(value.y);
            output.write(value.z);
        }
        BitStream input{std::span<const std::byte>(bytes)};
        input.setLimits(output.getCurPos(), 0);
        auto light = std::make_unique<world::SimNativeLight>();
        CHECK(light->unpackUpdate(*receiver.ghosts, input));
        CHECK(input.remainingReadBits() == 0);
        CHECK(std::isfinite(light->state().range) && std::isfinite(light->state().direction.len()));
        sim::SimLightQuery query;
        CHECK(light->processQuery(query));
        CHECK(query.lights.empty() == (type == 2 && range == 0));
        CHECK(receiver.manager().addObject(std::move(light)));
    };
    decode(1, 0, {0, 0, -1});
    decode(5, std::numeric_limits<Flt32>::quiet_NaN(), {0, 0, -1});
    decode(2, 20, {});
    decode(2, 20, {std::numeric_limits<Flt32>::quiet_NaN(), 0, 0});
    decode(2, 0, {});
    world::LightState invalid;
    invalid.range = -1;
    CHECK(!invalid.valid());
    invalid.range = 1;
    invalid.position.x = std::numeric_limits<Flt32>::quiet_NaN();
    CHECK(!invalid.valid());
    invalid.position = {};
    invalid.intensity.red = std::numeric_limits<Flt32>::quiet_NaN();
    CHECK(!invalid.valid());
}

class SoundShape final : public ShapeBase {
public:
    SoundShape() {
        CHECK(setNetFlags(Ghostable | ScopeAlways));
    }

    bool onAdd() override {
        return sim::SimNetObject::onAdd();
    }

    UInt16 getGhostTag() const override {
        return 200;
    }

    UInt32 packUpdate(GhostManager&, UInt32, BitStream& output) const override {
        output.write(getPos().x);
        output.write(getPos().y);
        output.write(getPos().z);
        return 0;
    }

    bool unpackUpdate(GhostManager&, BitStream& input) override {
        Point3F position;
        if (!input.read(position.x) || !input.read(position.y) || !input.read(position.z)) {
            return false;
        }
        setPos(position);
        return true;
    }
};

void delayedControlReplay() {
    auto protocol = std::make_shared<Registry>();
    CHECK(protocol->registerGhost<SoundShape>(200));
    Endpoint from(protocol, true), to(protocol);
    auto* source = add<SoundShape>(from.manager());
    CHECK(from.ghosts->setScopeObject(source));
    from.ghosts->activate();
    deliver(*from.stream, *to.stream);
    deliver(*from.stream, *to.stream);
    const auto index = from.ghosts->getGhostIndex(*source);
    auto* target = dynamic_cast<GameBase*>(to.ghosts->resolveGhost(index));
    CHECK(target);
    auto* server = add<PlayerPSC>(from.manager(), true);
    auto* client = add<PlayerPSC>(to.manager());
    server->setLocalGhostManager(from.ghosts);
    client->setLocalGhostManager(to.ghosts);
    server->setControlObject(source);
    client->setControlObject(target);
    UInt32 previous = 0;
    for (auto i : std::views::iota(UInt32{0}) | std::views::take_while([&](UInt32 i) {
                      return i < PlayerPSC::MaxMoves;
                  })) {
        client->clientCollectInput(previous, i * 32 + 8, .032f);
        previous = i * 32 + 8;
    }
    client->clientCollectInput(previous, 1608, .032f);
    server->setLastServerMove(21);
    std::array<std::byte, 512> bytes{};
    BitStream output{std::span(bytes)};
    UInt32 key{};
    CHECK(server->writePacket(output, key));
    BitStream input{std::span<const std::byte>(bytes)};
    CHECK(client->readPacket(input, 1608));
    CHECK(target->getLastProcessTime() == 640);
    CHECK(client->moves().size() == 10 && client->firstMoveSeq() == 20);
}

void soundEventDelivery() {
    auto protocol = std::make_shared<Registry>();
    CHECK(registerPlayerManagerEvents(*protocol));
    CHECK(registerSoundEvents(*protocol));
    CHECK(protocol->registerGhost<SoundShape>(200));
    Console console;
    Endpoint from(protocol, true), to(protocol);
    to.manager().setConsole(&console);
    for (auto* endpoint : {&from, &to}) {
        auto* blocks = add<DataBlockManager>(endpoint->manager(), endpoint == &from);
        auto* profile = dynamic_cast<SoundProfileData*>(
            blocks->createDataBlock("VoiceProfile", DataBlockManager::SoundProfileDataType));
        CHECK(profile);
        profile->dist = 100;
        profile->profile.maxDistance = 100;
        for (auto i : std::views::iota(Int32{0}, Int32{3})) {
            auto* sound = dynamic_cast<SoundData*>(blocks->createDataBlock(
                "Effect" + std::to_string(i), DataBlockManager::SoundDataType));
            CHECK(sound);
            sound->wavFileName = "effect.wav";
            sound->profileIndex = 0;
        }
    }
    auto* source = add<SoundShape>(from.manager());
    source->setPos({10, 20, 30});
    CHECK(from.ghosts->setScopeObject(source));
    from.ghosts->activate();
    deliver(*from.stream, *to.stream);
    // Complete the native ghost-always staging barrier before gameplay events
    // target the created object; creation acknowledgment alone is insufficient.
    deliver(*from.stream, *to.stream);
    const auto index = from.ghosts->getGhostIndex(*source);
    CHECK(index != sim::InvalidGhostIndex);
    auto* target = dynamic_cast<SoundShape*>(to.ghosts->resolveGhost(index));
    CHECK(target && target->getManager() == &to.manager());
    UInt32 plays{}, relativePlays{}, voices{}, stopped{};
    auto* playback =
        add<SoundPlayback>(to.manager(), [&](UInt32 id, const TMat3F& transform, const Point3F&) {
            CHECK(id == 2 && transform.p == Point3F(10, 20, 30));
            return ++plays;
        });
    playback->stop = [&](SoundHandle handle) {
        CHECK(handle == 1);
        ++stopped;
    };
    playback->playRelative = [&](UInt32 id, const Point3F& relative) {
        CHECK(id == 2 && relative.len() < .001f);
        ++relativePlays;
        return 5;
    };
    TSFX::serverPlaySound(2, source, 1);
    deliver(*from.stream, *to.stream);
    CHECK(plays == 1);
    TSFX::serverPlaySound(2, source, 1);
    deliver(*from.stream, *to.stream);
    CHECK(plays == 2 && stopped == 1);
    TSFX::serverPlaySound(2, source->getPos(), &from.manager());
    deliver(*from.stream, *to.stream);
    CHECK(relativePlays == 1);
    // Native SoundEvent uses the DBM codec (eight bits, blockId + 1).
    auto* blocks = dynamic_cast<DataBlockManager*>(from.manager().findObject(DataBlockManagerId));
    CHECK(blocks);
    SoundEvent sound;
    sound.ghostIndex = index;
    sound.channel = 1;
    sound.soundId = 2;
    std::array<std::byte, 64> bytes{};
    BitStream out{std::span(bytes)};
    EventContext context{from.manager(), *from.stream, 2049, true};
    CHECK(protocol->event(SoundEventType)->pack(sound, out, context));
    CHECK(out.getCurPos() == 20);
    BitStream encoded{std::span<const std::byte>(bytes)};
    CHECK(encoded.readInt(10) == index && encoded.readInt(2) == 1 && encoded.readInt(8) == 3);

    auto* players = add<PlayerManager>(to.manager(), false);
    players->bindConsole(&console);
    CHECK(players->clientAdded("Speaker", "female2", "base", false, 2049) == 2049);
    players->findClient(2049)->ownedObject = target;
    auto* resources = SimResource::find(to.manager());
    CHECK(resources);
    CHECK(resources->get().add("female2.wattack.wav", std::make_unique<Int32>(1)));
    CHECK(resources->get().add("wfallback.wav", std::make_unique<Int32>(1)));
    std::string lastVoice;
    playback->playVoice = [&](Int32 speaker,
                              std::string_view filename,
                              const SoundProfileData& profile,
                              const TMat3F& transform,
                              const Point3F&) {
        CHECK(speaker == 2049 && profile.profile.maxDistance == 100 &&
              transform.p == Point3F(10, 20, 30));
        lastVoice = filename;
        ++voices;
        return 6;
    };
    const auto speak = [&](std::string_view name) {
        auto event = std::make_unique<VoiceEvent>(2049, name);
        event->flags = SimEvent::Guaranteed;
        CHECK(from.stream->sendRemoteEvent(std::move(event)));
        deliver(*from.stream, *to.stream);
    };
    speak("attack");
    CHECK(voices == 1 && lastVoice == "female2.wattack.wav" &&
          console.getVariable("pref::playVoices") == "TRUE");
    speak("fallback");
    CHECK(voices == 2 && lastVoice == "wfallback.wav");
    speak("missing");
    CHECK(voices == 2);
    console.setVariable("pref::playVoices", "false");
    speak("attack");
    CHECK(voices == 2);
}

void networkCommands() {
    FearGameApp app;
    CHECK(app.init());
    auto& plugin = app.fearPlugin();
    auto& console = app.console();
    for (const auto* name : {"Client::limitCommandBandwidth",
                             "Client::getSkinBase",
                             "Client::getVoiceBase",
                             "Client::getTransportAddress",
                             "Client::isItemBuyingOn",
                             "Client::setInitialTeam",
                             "Client::setSkin",
                             "getClientByIndex",
                             "getClientByName",
                             "DNet::resolveMasters",
                             "DNet::getResolvedMaster",
                             "resetGhostManagers",
                             "resetPlayerManager",
                             "startGhosting",
                             "Client::centerPrint",
                             "listPlayers",
                             "remoteSelectCommander",
                             "remotePlayAnim",
                             "playVoice"}) {
        CHECK(console.isFunction(name));
    }
    CHECK(plugin.createServerWorld({}, true));
    CHECK(plugin.createClientWorld());
    CHECK(plugin.focusServer());
    auto& server = plugin.serverGlobals();
    auto& client = plugin.clientGlobals();
    const auto id =
        server.playerManager->clientAdded("Network Tester", "female2", "base", false, 2051);
    CHECK(id == 2051);
    CHECK(client.playerManager->clientAdded("Network Tester", "female2", "base", false, 2051) ==
          id);
    auto* rep = server.playerManager->findClient(id);
    rep->playerPSC = server.playerPSC;
    const auto call = [&](std::string_view name, std::initializer_list<std::string> args) {
        return console.execute(name, std::vector<std::string>(args)).value;
    };
    CHECK(call("Client::getVoiceBase", {"2051"}) == "female2");
    CHECK(call("Client::getSkinBase", {"2051"}) == "base");
    CHECK(call("Client::getGender", {"2051"}) == "Female");
    CHECK(call("Client::getGender", {"999"}).empty());
    CHECK(call("Client::getVoiceBase", {"999"}).empty());
    CHECK(call("Client::getTransportAddress", {"2051"}).empty());
    CHECK(call("getClientByIndex", {"0"}) == "2051");
    CHECK(call("getClientByIndex", {"-1"}) == "0");
    CHECK(call("getClientByName", {"nEtWoRk TeStEr"}) == "2051");
    CHECK(call("getClientByName", {"missing"}) == "0");
    CHECK(server.playerManager->aiAdded("AI lookup exclusion", "male1", "base", true) != 0);
    CHECK(client.playerManager->aiAdded("AI lookup exclusion", "male1", "base", true) != 0);
    CHECK(call("getClientByIndex", {"0"}) == "2051");
    CHECK(call("getClientByIndex", {"1"}) == "0");
    CHECK(call("getClientByName", {"AI lookup exclusion"}) == "0");
    CHECK(call("Client::getFirst", {}) == "2051");
    CHECK(call("Client::getNext", {"2051"}) == "-1");
    CHECK(call("Client::getFirst", {"extra"}) == "-1");
    CHECK(call("Client::getNext", {}) == "-1");
    CHECK(call("Client::setMenuScoreVis", {"2051", "true"}) == "True" && rep->sendScores == 1);
    CHECK(call("Client::setMenuScoreVis", {"2051", "false"}) == "True" && rep->sendScores == 0);
    CHECK(call("Client::setMenuScoreVis", {"999", "true"}) == "False");
    CHECK(!server.playerPSC->commandBandLimited());
    CHECK(call("Client::limitCommandBandwidth", {"2051", "True"}) == "true");
    CHECK(server.playerPSC->commandBandLimited());
    CHECK(call("Client::limitCommandBandwidth", {"999", "true"}) == "false");
    CHECK(call("Client::limitCommandBandwidth", {"2051", "false"}) == "true");
    CHECK(!server.playerPSC->commandBandLimited());
    CHECK(call("Client::setSkin", {"2051", "beagle"}) == "True");
    CHECK(rep->skinBase == "beagle");
    CHECK(call("Client::setSkin", {"2051", "../bad"}) == "True");
    CHECK(rep->skinBase == "beagle");
    CHECK(call("Client::setSkin", {"999", "base"}) == "True");
    server.playerManager->teamAdded(0);
    server.playerManager->teamAdded(1);
    CHECK(call("Client::setInitialTeam", {"2051", "1"}) == "True" && rep->team == 1);
    CHECK(call("Client::setInitialTeam", {"2051", "-1"}) == "False");
    server.playerPSC->setItemBuyList(3);
    CHECK(call("Client::isItemBuyingOn", {"2051", "3"}) == "True");
    CHECK(call("Client::isItemBuyingOn", {"2051", "4"}) == "False");
    rep->curCommand = 7;
    rep->scoreString = "score";
    CHECK(call("resetPlayerManager", {}) == "True");
    CHECK(server.playerManager->findClient(id) == rep);
    CHECK(rep->curCommand == -1 && rep->scoreString.empty() &&
          server.playerManager->getNumTeams() == 0);
    server.ghostManager->activate();
    CHECK(call("resetGhostManagers", {}) == "True");
    CHECK(server.ghostManager->mode() == GhostManager::Mode::Inactive);
    CHECK(call("startGhosting", {"999"}) == "False");
    CHECK(call("Client::centerPrint", {"<jc>Hello", "2"}) == "True");
    CHECK(plugin.centerPrintText() == "<jc>Hello" && plugin.centerPrintPosition() == 2);
    CHECK(call("Client::centerPrint", {"bad"}) == "False");
    CHECK(call("DNet::resolveMasters",
               {"IP:127.0.0.1:28000", "IP:localhost:28001", "bad/name:1"}) == "3");
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (call("DNet::getResolvedMaster", {"0"}).empty()) {
        CHECK(std::chrono::steady_clock::now() < deadline);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    CHECK(call("DNet::getResolvedMaster", {"1"}) == "IP:127.0.0.1:28001");
    CHECK(call("DNet::getResolvedMaster", {"2"}).empty());
    CHECK(call("DNet::getResolvedMaster", {"10"}).empty());
    CHECK(call("DNet::resolveMasters", {"IP:127.0.0.2:1", "", "IP:127.0.0.3:2"}) == "1");
    CHECK(call("DNet::getResolvedMaster", {"0"}) == "IP:127.0.0.2:1");
    CHECK(call("DNet::getResolvedMaster", {"1"}).empty());

    auto protocol = std::make_shared<Registry>();
    CHECK(registerPlayerManagerEvents(*protocol));
    Endpoint from(protocol, true), to(protocol);
    auto* source = add<PlayerManager>(from.manager(), true);
    auto* destination = add<PlayerManager>(to.manager(), false);
    CHECK(source->clientAdded("Peer", "male1", "base", true, 2049) == 2049);
    CHECK(destination->clientAdded("Peer", "male1", "base", true, 2049) == 2049);
    source->findClient(2049)->packetStream = from.stream;
    source->setClientSkin(2049, "storm");
    deliver(*from.stream, *to.stream);
    CHECK(destination->findClient(2049)->skinBase == "storm");
    destination->findClient(2049)->curCommand = 4;
    destination->findClient(2049)->scoreString = "keep";
    source->missionReset();
    deliver(*from.stream, *to.stream);
    CHECK(destination->getNumClients() == 1 && destination->findClient(2049)->curCommand == -1 &&
          destination->findClient(2049)->scoreString == "keep");

    CHECK(registerSensorManagerEvents(*protocol));
    auto* sensors = add<SensorManager>(from.manager());
    auto* receivedSensors = add<SensorManager>(to.manager());
    auto* target = add<GameBase>(from.manager());
    const auto key = sensors->addTarget(target, "West generator");
    CHECK(key >= 0);
    deliver(*from.stream, *to.stream);
    CHECK(receivedSensors->target(key).name == "West generator");
    sensors->changeTargetName(key, "East generator");
    deliver(*from.stream, *to.stream);
    CHECK(receivedSensors->target(key).name == "East generator");
    sensors->newClient(2049);
    deliver(*from.stream, *to.stream);
    CHECK(receivedSensors->target(key).name == "East generator");
    target->deleteObject();
    deliver(*from.stream, *to.stream);
    CHECK(receivedSensors->target(key).name.empty());
}

struct IrcPipe {
    std::string input, output;
    bool closed{};
};

class IrcSocket final : public net::TcpStream {
public:
    explicit IrcSocket(std::shared_ptr<IrcPipe> pipe) : pipe_(std::move(pipe)) {}

    ~IrcSocket() override {
        close();
    }

    net::Result connected() override {
        return pipe_->closed ? net::Result::Closed : net::Result::Ok;
    }

    net::StreamResult read(std::span<std::byte> bytes) override {
        if (pipe_->closed) {
            return {net::Result::Closed};
        }
        if (pipe_->input.empty()) {
            return {net::Result::WouldBlock};
        }
        const auto count = std::min<std::size_t>({bytes.size(), pipe_->input.size(), 3});
        std::memcpy(bytes.data(), pipe_->input.data(), count);
        pipe_->input.erase(0, count);
        return {net::Result::Ok, count};
    }

    net::StreamResult write(std::span<const std::byte> bytes) override {
        if (pipe_->closed) {
            return {net::Result::Closed};
        }
        const auto count = std::min<std::size_t>(bytes.size(), 4);
        pipe_->output.append(reinterpret_cast<const char*>(bytes.data()), count);
        return {net::Result::Ok, count};
    }

    void close() override {
        pipe_->closed = true;
    }

private:
    std::shared_ptr<IrcPipe> pipe_;
};

void liveIrcControls() {
    opent1::game::IRCClient client;
    auto pipe = std::make_shared<IrcPipe>();
    CHECK(client.attachTransport(std::make_unique<IrcSocket>(pipe), 0, "me", "Me"));
    pipe->input =
        ":server 422 me :No MOTD\r\n:me!u@h JOIN :#tribes\r\n:s 353 me = #tribes :@me +other\r\n:s "
        "332 me #tribes :Live topic\r\n:other!u@h PRIVMSG #tribes :Hello\r\nPING :token\r\n";
    client.process(1);
    client.process(2);
    CHECK(client.state() == opent1::game::IRCClient::IDIRC_CONNECTED);
    CHECK(pipe->output.find("USER me ignore ignore :Me\r\nNICK me\r\n") != std::string::npos);
    CHECK(pipe->output.find("PONG token\r\n") != std::string::npos);
    const auto* channel = client.currentChannel();
    CHECK(channel && channel->name == "#tribes" && channel->members.size() == 2);
    auto resolver = [&] {
        return &client;
    };
    FGIRCNicks nicks;
    nicks.resolveClient = resolver;
    nicks.tick(0);
    CHECK(nicks.nicks().size() == 2);
    FGIRCTopic topic;
    topic.resolveClient = resolver;
    topic.tick(0);
    CHECK(topic.text == "Live topic");
    FGIRCActiveTextFormat messages;
    messages.resolveClient = resolver;
    messages.tick(0);
    CHECK(messages.lineCount() == 1);
    FGIRCChannelCtrl tabs;
    tabs.resolveClient = resolver;
    tabs.tick(0);
    CHECK(tabs.tabs().size() == 2);
    tabs.selectChannel(0);
    CHECK(client.currentChannel()->name == "Status");
    tabs.selectChannel(1);
    FGIRCTextEdit edit;
    edit.resolveClient = resolver;
    edit.setText("From UI");
    gui::Event event;
    event.type = gui::EventType::KeyDown;
    event.key = gui::Key::Return;
    CHECK(edit.event(event) && edit.text.empty());
    client.process(3);
    CHECK(pipe->output.find("PRIVMSG #tribes :From UI\r\n") != std::string::npos);
    event.key = gui::Key::Up;
    CHECK(edit.event(event) && edit.text == "From UI");
    CHECK(client.sendInput("bad\r\nOPER root password").empty());
    FGIRCChannelList listing;
    listing.resolveClient = resolver;
    listing.wake();
    listing.setShowAll(true);
    client.process(4);
    CHECK(pipe->output.find("LIST\r\n") != std::string::npos);
    pipe->input = ":s 322 me #room 8 :A room\r\n:s 323 me :Done\r\n";
    client.process(5);
    listing.tick(0);
    CHECK(listing.entries().size() == 1 && listing.entries()[0].name == "#room" &&
          listing.entries()[0].userCount == 8);
    listing.setFilter("ROOM");
    CHECK(listing.entries().size() == 1);
    listing.setFilter("nomatch");
    CHECK(listing.entries().empty());
    FGIRCChannelPropsCtrl properties;
    properties.resolveClient = resolver;
    gui::TextEdit topicEdit;
    topicEdit.tag(141132);
    CHECK(properties.addObject(topicEdit));
    FGUniversalButton moderated;
    moderated.tag(141137);
    CHECK(properties.addObject(moderated));
    FGUniversalButton anyTopic;
    anyTopic.tag(141138);
    CHECK(properties.addObject(anyTopic));
    properties.wake();
    CHECK(topicEdit.text == "Live topic" && topicEdit.active && moderated.active);
    topicEdit.setText("Changed topic");
    moderated.setState(true);
    (void)properties.message(moderated, 110001);
    client.process(6);
    CHECK(pipe->output.find("TOPIC #tribes :Changed topic\r\n") != std::string::npos);
    CHECK(pipe->output.find("MODE #tribes +m\r\n") != std::string::npos);
    FGIRCBanCtrl bans;
    bans.resolveClient = resolver;
    gui::TextEdit mask;
    mask.tag(141112);
    CHECK(bans.addObject(mask));
    FGTextList masks;
    masks.tag(141113);
    CHECK(bans.addObject(masks));
    FGUniversalButton banButton;
    banButton.tag(141110);
    CHECK(bans.addObject(banButton));
    bans.wake();
    CHECK(!banButton.active);
    client.process(7);
    CHECK(pipe->output.find("MODE #tribes +b\r\n") != std::string::npos);
    pipe->input = ":s 367 me #tribes *!*@old\r\n:s 368 me #tribes :Done\r\n";
    client.process(8);
    bans.tick(0);
    CHECK(bans.banMasks().size() == 1 && masks.entries.size() == 1 && banButton.active);
    mask.setText("*!*@new");
    (void)bans.message(banButton, 141110);
    client.process(9);
    CHECK(pipe->output.find("MODE #tribes +b *!*@new\r\n") != std::string::npos);
    (void)bans.message(banButton, 141111);
    client.process(10);
    CHECK(bans.banMasks().size() == 1);
    CHECK(pipe->output.find("MODE #tribes -b *!*@new\r\n") != std::string::npos);
    pipe->closed = true;
    client.process(11);
    CHECK(client.state() == opent1::game::IRCClient::IDIRC_DISCONNECTED);
    topic.tick(0);
    nicks.tick(0);
    CHECK(nicks.nicks().empty());
}

void originalPscCapture() {
    std::ifstream file(OPENT1_GAME_PSC_FIXTURE, std::ios::binary);
    CHECK(file);
    const std::vector<char> chars{std::istreambuf_iterator<char>(file), {}};
    MemRWStream input{std::as_bytes(std::span(chars))};
    std::array<std::byte, 4> magic{};
    UInt32 version{};
    CHECK(input.readBytes(magic) && input.read(version) && version == 1);
    CHECK(magic == std::array{std::byte{'T'}, std::byte{'1'}, std::byte{'P'}, std::byte{'C'}});
    Endpoint endpoint(registry(), true, false);
    auto* players = add<PlayerManager>(endpoint.manager(), true);
    CHECK(players->clientAdded("Capture", "male1", "base", true, 2049) == 2049);
    auto* psc = add<PlayerPSC>(endpoint.manager(), true);
    CHECK(endpoint.stream->addObject(*psc));
    for (auto mode : std::views::iota(UInt32{0}, UInt32{6})) {
        auto* client = players->findClient(2049);
        client->curGuiMode = mode;
        client->sendScores = mode & 1;
        psc->setLastServerMove(static_cast<Int32>(mode * 3));
        UInt32 bitCount{}, byteCount{};
        CHECK(input.read(bitCount) && input.read(byteCount));
        std::vector<std::byte> expected(byteCount);
        CHECK(input.readBytes(expected));
        std::array<std::byte, 100> buffer{};
        BitStream output{std::span(buffer)};
        UInt32 key{};
        CHECK(psc->writePacket(output, key));
        CHECK(static_cast<UInt32>(output.getCurPos()) == bitCount);
        CHECK(std::equal(expected.begin(), expected.end(), buffer.begin()));
    }
    UInt32 size{};
    CHECK(input.read(size) && size == 33);
    std::vector<std::byte> record(size);
    CHECK(input.readBytes(record));
    PlayerPSC playback;
    MemRWStream move{std::span<const std::byte>(record)};
    CHECK(playback.processRecorderStream(move, 32));
    const auto& decoded = playback.moves().front();
    CHECK(decoded.forwardAction == .25f && decoded.backwardAction == .5f &&
          decoded.leftAction == .75f && decoded.rightAction == 1.f);
    CHECK(decoded.jetting && decoded.crouching && !decoded.jumpAction && decoded.trigger);
    CHECK(decoded.useItem == 17 && decoded.turnRot == -.25f && decoded.pitch == .125f);
    CHECK(static_cast<std::size_t>(input.getPosition()) == chars.size());
}

void nativeServerFraming() {
    // Native 1.11 PSC begins with mode(3), scores(1), lastMove(32),
    // control-present(1). Inventory must not precede this header.
    PlayerPSC server(true), client(false);
    server.setGuiMode(PlayerPSC::PlayGuiMode);
    server.setLastServerMove(3);
    client.pushMove({});
    client.pushMove({});
    client.pushMove({});
    std::array<std::byte, 1499> bytes{};
    BitStream output{std::span(bytes)};
    UInt32 key{};
    CHECK(server.writePacket(output, key));
    CHECK(output.getCurPos() == 37);
    CHECK(key == 0);
    CHECK(bytes[0] == std::byte{0x21});
    CHECK(bytes[1] == std::byte{0});
    CHECK(bytes[4] == std::byte{0});
    BitStream input{std::span<const std::byte>(bytes)};
    input.setLimits(37, 0);
    CHECK(client.readPacket(input, 96));
    CHECK(client.firstMoveSeq() == 2);
    CHECK(client.moves().size() == 1);

    // A transport acknowledgement alone does not discard unprocessed input.
    BitStream moves{std::span(bytes)};
    CHECK(client.writePacket(moves, key));
    client.packetReceived(key);
    CHECK(client.moves().size() == 1);

    GameBase controlled;
    server.setControlObject(&controlled);
    server.setGuiMode(PlayerPSC::InventoryGuiMode);
    server.setItemShoppingList(33);
    server.setItemBuyList(3);
    server.setItemCount(4, 9);
    output.clear();
    CHECK(server.writePacket(output, key));
    const auto count = output.getCurPos();
    const auto original = bytes;
    // Decode independently of PlayerPSC::readPacket, including indexed XOR words.
    BitStream wire{std::span<const std::byte>(bytes)};
    wire.setLimits(count, 0);
    CHECK(wire.readInt(3) == 4);
    CHECK(!wire.readFlag());
    CHECK(wire.readInt(32) == 2);
    CHECK(wire.readFlag());
    CHECK(wire.readInt(8) == 0);
    CHECK(wire.readInt(10) == 1023);
    CHECK(wire.readFlag());
    CHECK(wire.readInt(4) == 0);
    CHECK(wire.readFlag());
    CHECK(wire.readInt(32) == 8);
    CHECK(wire.readFlag());
    CHECK(wire.readInt(32) == 8);
    CHECK(wire.readFlag());
    CHECK(wire.readInt(4) == 1);
    CHECK(wire.readFlag());
    CHECK(wire.readInt(32) == 2);
    CHECK(!wire.readFlag());
    CHECK(!wire.readFlag());
    CHECK(!wire.readFlag());
    CHECK(wire.readFlag());
    CHECK(wire.readInt(8) == 4);
    CHECK(wire.readSignedInt(10) == 9);
    CHECK(!wire.readFlag());
    CHECK(wire.remainingReadBits() == 0);
    server.packetDropped(key);
    output.clear();
    CHECK(server.writePacket(output, key));
    CHECK(count == output.getCurPos());
    CHECK(original == bytes);
    BitStream inventory{std::span<const std::byte>(bytes)};
    inventory.setLimits(count, 0);
    CHECK(client.readPacket(inventory, 128));
    CHECK(client.isItemShoppingOn(33));
    CHECK(client.isItemBuyOn(3));
    CHECK(client.itemCount(4) == 9);
    server.packetReceived(key);
    // Initial/control-absent packets and indexed inventory both reject truncation.
    for (auto bits : std::views::iota(Int32{0}) | std::views::take_while([&](Int32 bits) {
                         return bits < count;
                     })) {
        PlayerPSC truncated;
        truncated.setFirstMoveSeq(2);
        BitStream packet{std::span<const std::byte>(bytes)};
        packet.setLimits(bits, 0);
        CHECK(!truncated.readPacket(packet, 128));
        CHECK(truncated.itemCount(4) == 0 && !truncated.isItemShoppingOn(33));
    }
}

void recorderLayout() {
    PlayerPSC psc;
    // Bytes represent the original x86 PlayerMove field order, which differs
    // from the modern struct order (crouch and jump in particular).
    std::array<std::byte, 33> bytes{};
    MemRWStream output{std::span(bytes)};
    CHECK(output.write(UInt8{1}));
    CHECK(output.write(0.25f) && output.write(0.5f) && output.write(0.75f) && output.write(1.0f));
    CHECK(output.write(UInt8{1}) && output.write(UInt8{1}) && output.write(UInt8{0}) &&
          output.write(UInt8{1}));
    CHECK(output.write(Int32{17}) && output.write(-0.25f) && output.write(0.125f));
    CHECK(output.getPosition() == 33);
    for (auto n : std::views::iota(std::size_t{0}, bytes.size())) {
        MemRWStream shortInput{std::span<const std::byte>(bytes).first(n)};
        CHECK(!psc.processRecorderStream(shortInput, 0));
        CHECK(psc.moves().empty());
    }
    MemRWStream input{std::span<const std::byte>(bytes)};
    CHECK(psc.processRecorderStream(input, 32));
    CHECK(psc.moves().size() == 1);
    const auto& move = psc.moves().front();
    CHECK(move.jetting && move.crouching && !move.jumpAction && move.trigger && move.useItem == 17);
    CHECK(move.forwardAction == 0.25f && move.turnRot == -0.25f && move.pitch == 0.125f);
    std::array<std::byte, 9> action{};
    MemRWStream record{std::span(action)};
    CHECK(record.write(UInt8{0}) && record.write(1.0f) && record.write(IDACTION_VIEW));
    MemRWStream replay{std::span<const std::byte>(action)};
    CHECK(psc.processRecorderStream(replay, 64));
    CHECK(!psc.isFirstPerson());
}

void ownershipBacklinks() {
    SimWorld world;
    auto& manager = world.manager();
    auto* players = add<PlayerManager>(manager, false);
    CHECK(players->clientAdded("One", "male1", "base", true, 2049) == 2049);
    CHECK(players->clientAdded("Two", "female1", "base", false, 2050) == 2050);
    CHECK(players->findClient(2049)->gender == "MALE" &&
          players->findClient(2050)->gender == "FEMALE");
    auto* object = add<GameBase>(manager);
    const auto transfer = [&](Int32 control, Int32 ownerId) {
        GameBase source;
        source.setControlClient(control);
        source.setOwnerClient(ownerId);
        std::array<std::byte, 16> bytes{};
        BitStream output{std::span(bytes)};
        CHECK(source.packGameBaseUpdate(GameBase::BaseStateMask, output));
        BitStream input{std::span<const std::byte>(bytes)};
        CHECK(object->unpackGameBaseUpdate(input));
    };
    transfer(2049, 2049);
    CHECK(players->findClient(2049)->controlObject == object);
    CHECK(players->findClient(2049)->ownedObject == object);
    transfer(2050, 2050);
    CHECK(!players->findClient(2049)->controlObject && !players->findClient(2049)->ownedObject);
    CHECK(players->findClient(2050)->controlObject == object &&
          players->findClient(2050)->ownedObject == object);
    CHECK(object->deleteObject());
    CHECK(!players->findClient(2050)->controlObject && !players->findClient(2050)->ownedObject);
}

void remoteJoinWithStartupBots() {
    FearGameApp serverApp, clientApp;
    CHECK(serverApp.init() && clientApp.init());
    auto& serverPlugin = serverApp.fearPlugin();
    auto& clientPlugin = clientApp.fearPlugin();
    CHECK(serverPlugin.createServerWorld({}, true));
    CHECK(serverApp.console()
              .evaluate(R"(
        function Server::onClientConnect(%id) { $HumanConnections++; $HumanId = %id; }
        PlayerData NetworkBotArmor { mass = 9; boxWidth = 0.5; boxDepth = 0.5; boxNormalHeight = 2.3; };
        AI::spawn(StartupBot, NetworkBotArmor, "0 2 100", "0 0 0", RemoteHuman);
    )")
              .ok());
    auto& sg = serverPlugin.serverGlobals();
    const auto* startupBot = sg.aiManager->findAI("StartupBot");
    sg.playerManager->teamAdded("Players", "rpghuman0");
    sg.playerManager->teamAdded("Enemies", "rpgscalemail");
    sg.playerManager->setTeamSkin(-1, "neutral");
    CHECK(startupBot && startupBot->id == 2049);
    CHECK(sg.playerManager->getNumClients() == 0);
    CHECK(serverApp.console().getVariable("HumanConnections").empty());
    CHECK(clientApp.console()
              .evaluate(R"(
        function onClientJoin(%id) { $HumanJoins++; }
        function onClientDrop(%id) { $HumanDrops++; }
    )")
              .ok());
    clientApp.console().setVariable("PCFG::Name", "RemoteHuman");
    const auto endpoint = net::parseIPv4Address(sg.csDelegate->delegate()->localAddress());
    CHECK(endpoint);
    const std::array<std::string, 1> address{"IP:127.0.0.1:" + std::to_string(endpoint->port)};
    CHECK(clientApp.console().execute("connect", address).value == "True");
    int frame = 0;
    const auto advanceFrames = [&](int count, FearGameApp* replacement = nullptr) {
        for (const auto currentFrame : std::views::iota(frame, frame + count)) {
            const auto time = currentFrame * .032;
            CHECK(serverApp.stepFrame(time, false));
            CHECK(clientApp.stepFrame(time, false));
            if (replacement) {
                CHECK(replacement->stepFrame(time, false));
            }
        }
        frame += count;
    };
    advanceFrames(64);
    auto& cg = clientPlugin.clientGlobals();
    CHECK(clientPlugin.isConnected() && cg.manager->getId() == 2050);
    CHECK(serverApp.console().getVariable("HumanConnections") == "1");
    CHECK(serverApp.console().getVariable("HumanId") == "2050");
    CHECK(sg.playerManager->findClient(2050)->name == "RemoteHuman");
    CHECK(!sg.playerManager->findClient(2049) && !cg.playerManager->findClient(2049));
    CHECK(cg.playerManager->findBaseRep(2049));
    CHECK(cg.playerManager->getNumClients() == 1 && cg.playerManager->getNumBaseReps() == 2);
    CHECK(cg.playerManager->getNumTeams() == 2);
    CHECK(cg.playerManager->getTeamSkinBase(-1) == "neutral");
    CHECK(cg.playerManager->findTeam(0)->name == "Players");
    CHECK(cg.playerManager->getTeamSkinBase(0) == "rpghuman0");
    CHECK(cg.playerManager->getTeamSkinBase(1) == "rpgscalemail");
    // Static town NPCs select their skin from the received team table.
    auto* townNpc = add<Item>(*cg.manager);
    townNpc->setTeam(0);
    CHECK(townNpc->getSkinBase() == "rpghuman0");
    CHECK(clientApp.console().getVariable("HumanJoins") == "1");
    CHECK(serverApp.console()
              .evaluate(R"(
        AI::spawn(LateBot, NetworkBotArmor, "0 4 100");
    )")
              .ok());
    auto* late = sg.aiManager->findAI("LateBot");
    CHECK(late);
    const auto lateId = late->id;
    sg.playerManager->serverClientTeamChange(lateId, 1);
    advanceFrames(16);
    CHECK(cg.playerManager->findBaseRep(lateId));
    CHECK(cg.playerManager->findBaseRep(lateId)->team == 1);
    CHECK(cg.playerManager->getPlayerSkinBase(lateId) == "rpgscalemail");
    CHECK(!cg.playerManager->findClient(lateId));
    CHECK(clientApp.console().getVariable("HumanJoins") == "1");
    CHECK(sg.aiManager->remove("LateBot", true));
    advanceFrames(16);
    CHECK(!cg.playerManager->findBaseRep(lateId));
    CHECK(clientApp.console().getVariable("HumanDrops").empty());
    CHECK(sg.aiManager->remove("StartupBot", true));
    advanceFrames(16);
    CHECK(!cg.playerManager->findBaseRep(2049));
    CHECK(cg.playerManager->findClient(2050) && sg.playerManager->findClient(2050));
    CHECK(clientApp.console().getVariable("HumanDrops").empty());

    // Reuse the former AI slot for a connection, then for an AI again while
    // the first client stays connected and receives the ordered roster events.
    FearGameApp replacementApp;
    CHECK(replacementApp.init());
    replacementApp.console().setVariable("PCFG::Name", "ReplacementHuman");
    CHECK(replacementApp.console().execute("connect", address).value == "True");
    advanceFrames(32, &replacementApp);
    CHECK(replacementApp.fearPlugin().clientGlobals().manager->getId() == 2049);
    CHECK(cg.playerManager->findClient(2049));
    CHECK(cg.playerManager->findClient(2049)->name == "ReplacementHuman");
    CHECK(cg.playerManager->getNumClients() == 2);
    CHECK(clientApp.console().getVariable("HumanJoins") == "2");
    CHECK(sg.playerManager->findClient(2049)->playerPSC);
    CHECK(replacementApp.console().execute("disconnect").value == "True");
    advanceFrames(16);
    CHECK(!cg.playerManager->findBaseRep(2049));
    CHECK(clientApp.console().getVariable("HumanDrops") == "1");
    CHECK(serverApp.console().evaluate(
        "AI::spawn(ReusedStartupBot, NetworkBotArmor, \"0 2 100\");").ok());
    CHECK(sg.aiManager->findAI("ReusedStartupBot")->id == 2049);
    advanceFrames(16);
    CHECK(cg.playerManager->findBaseRep(2049) && !cg.playerManager->findClient(2049));
    CHECK(cg.playerManager->getNumClients() == 1 && cg.playerManager->getNumBaseReps() == 2);
    CHECK(clientApp.console().getVariable("HumanJoins") == "2");
    CHECK(clientApp.console().getVariable("HumanDrops") == "1");
}

void addressedJoin() {
    auto transports = net::makeLoopTransportPair();
    const auto clientAddress = transports.second->localAddress();
    const auto serverAddress = transports.first->localAddress();
    SimWorld serverWorld, clientWorld;
    auto bans = std::make_shared<BanList>();
    auto* server = add<FearCSDelegate>(serverWorld.manager(),
                                       true,
                                       bans,
                                       std::shared_ptr<const Registry>{},
                                       std::move(transports.first));
    auto* client = add<FearCSDelegate>(clientWorld.manager(),
                                       false,
                                       std::shared_ptr<BanList>{},
                                       std::shared_ptr<const Registry>{},
                                       std::move(transports.second));
    const auto request =
        client->buildConnectRequestPacket("", "A~\\\"VeryLongPlayerName", "fEmAlE", "female3");
    FearCSDelegate::JoinIdentity identity;
    std::string error;
    CHECK(server->validateConnectRequestPacket(clientAddress, request, error, 0, &identity));
    CHECK(identity.name == "A***VeryLongPlay" && !identity.male && identity.voice == "female3");
    bool addressSeen{};
    bans->addBan(clientAddress, BanList::kPermanentBan);
    const auto original = server->delegate()->acceptAddressedRequest;
    server->delegate()->acceptAddressedRequest = [&](auto address, auto bytes) {
        addressSeen = address == clientAddress;
        return original(address, bytes);
    };
    CHECK(client->delegate()->connect(serverAddress, 0, request) == net::Result::Ok);
    for (UInt32 now = 1; now < 1000; now += 10) {
        (void)server->delegate()->process(now);
        (void)client->delegate()->process(now);
    }
    CHECK(addressSeen);
    CHECK(server->connectedPeers().empty());
    CHECK(server->lastErrorReason() == "You are banned from this server.");
}

void liveServerBrowser() {
    auto transports = net::makeLoopTransportPair();
    const auto address = transports.first->localAddress();
    Console serverConsole, clientConsole;
    SimWorld serverWorld(&serverConsole), clientWorld(&clientConsole);
    auto* players = add<PlayerManager>(serverWorld.manager(), true);
    CHECK(players->clientAdded("Buddy One", "male1", "base", true, 2049) == 2049);
    players->setClientScoreHeading("Player\tScore\tPing");
    players->setClientScore(2049, "%n\t7\t%p", 7);
    CHECK(players->aiAdded("Hidden browser NPC", "male1", "base", true) > 0);
    auto* server = add<FearCSDelegate>(serverWorld.manager(),
                                       true,
                                       std::shared_ptr<BanList>{},
                                       std::shared_ptr<const Registry>{},
                                       std::move(transports.first));
    auto* client = add<FearCSDelegate>(clientWorld.manager(),
                                       false,
                                       std::shared_ptr<BanList>{},
                                       std::shared_ptr<const Registry>{},
                                       std::move(transports.second));
    serverConsole.setVariable("Server::Hostname", "Native browser");
    serverConsole.setVariable("Server::MaxPlayers", "16");
    serverConsole.setVariable("MissionName", "Raindance");
    serverConsole.setVariable("Game::MissionType", "CTF");
    serverConsole.setVariable("Server::Info", "A server");
    serverConsole.setVariable("modList", "base");
    clientConsole.setVariable("pref::buddyList", "\"Buddy One\" Other");
    CHECK(client->queryBrowserServer(address));
    for (auto now : std::views::iota(UInt32{1}, UInt32{100})) {
        (void)client->delegate()->process(now);
        (void)server->delegate()->process(now);
    }
    CHECK(client->discoveredServers().size() == 1);
    const auto& found = client->discoveredServers().front();
    CHECK(found.name == "Native browser" && found.missionName == "Raindance" &&
          found.numPlayers == 1 && found.maxPlayers == 16);
    CHECK(found.status == ServerListCtrl::STAT_RESPONDING && found.hasBuddy &&
          found.detailLines.size() == 3);
    ServerDetailCtrl details;
    details.setServer(found);
    CHECK(details.getCellText({0, 2}) == "Buddy One" && details.getCellText({1, 2}) == "7");
    ServerListCtrl list;
    list.discoverServers = [&] {
        return std::vector<ServerListCtrl::ServerInfo>(client->discoveredServers().begin(),
                                                       client->discoveredServers().end());
    };
    list.preferenceChanged = [&](const auto& value) {
        CHECK(
            client->setBrowserPreference(value.transportAddress, value.favorite, value.neverPing));
    };
    list.tick(0);
    CHECK(list.servers().size() == 1 && list.cells.x == 1);
    CHECK(list.setServerPreference(address, true, false));
    serverConsole.setVariable("Server::Hostname", "Updated");
    CHECK(client->queryBrowserServer(address, true));
    for (auto now : std::views::iota(UInt32{100}, UInt32{150})) {
        (void)client->delegate()->process(now);
        (void)server->delegate()->process(now);
    }
    CHECK(client->discoveredServers().front().favorite &&
          client->discoveredServers().front().name == "Updated");
    CHECK(list.setServerPreference(address, false, true));
    CHECK(!client->queryBrowserServer(address));
    const auto database =
        std::filesystem::temp_directory_path() / "opent1-browser-parity" / "GameServerList.cs";
    CHECK(client->saveServerDatabase(database));
    std::ifstream saved(database);
    CHECK(saved);
    const std::string script{std::istreambuf_iterator<char>(saved), {}};
    CHECK(script.find("addGameServer(\"" + address + "\", \"Updated\"") != std::string::npos);
    CHECK(script.find(", \"TRUE\");") != std::string::npos);
    saved.close();
    std::error_code cleanup;
    std::filesystem::remove(database, cleanup);
    std::filesystem::remove(database.parent_path(), cleanup);
    CHECK(client->setBrowserPreference(address, true, false));
    client->delegate()->browser().onTimeout(address);
    CHECK(client->discoveredServers().front().status == ServerListCtrl::STAT_TIME_OUT);
    const auto encoded = server->encodeBrowserGameInfo();
    for (auto size :
         std::views::iota(std::size_t{0}) | std::views::take_while([&](std::size_t size) {
             return size < encoded.size() / 2;
         })) {
        CHECK(!client->receiveBrowserGameInfo(
            {address,
             1,
             10,
             {encoded.begin(), encoded.begin() + static_cast<std::ptrdiff_t>(size)}}));
    }
}

void hostnameConnection() {
    SimWorld serverWorld, clientWorld;
    auto r = registry();
    auto* server =
        add<CSDelegate>(serverWorld.manager(), net::makeUdpTransport({{127, 0, 0, 1}, 0}), r, true);
    auto* client = add<CSDelegate>(
        clientWorld.manager(), net::makeUdpTransport({{127, 0, 0, 1}, 0}), r, false);
    const auto port = net::parseIPv4Address(server->localAddress())->port;
    const auto endpoint = client->translateAddress("localhost", port);
    CHECK(endpoint == "IP:localhost:" + std::to_string(port));
    CHECK(client->connect(endpoint, 0) == net::Result::Ok);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    for (auto now :
         std::views::iota(UInt32{1}) | std::views::take_while([&]([[maybe_unused]] UInt32 now) {
             return !client->stream(2048);
         })) {
        CHECK(std::chrono::steady_clock::now() < deadline);
        (void)server->process(now);
        (void)client->process(now);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    CHECK(server->stream(2049));
    CHECK(clientWorld.manager().getId() == 2049);
    CHECK(client->stream(2048)->transportAddress() == server->localAddress());
    CHECK(server->stream(2049)->transportAddress() == client->localAddress());
    CHECK(client->disconnect(2048, 1000));
    bool browserReply{};
    server->browser().getPingInfo = [] {
        return ServerBrowser::PingInfo{{}, "Native DNS server", 0xf0, 1, 32, 0, 1};
    };
    client->browser().gotPingInfo = [&](const auto& info) {
        browserReply = info.name == "Native DNS server" && info.protocolVersion == 1;
    };
    CHECK(client->browser().ping("IP:localhost:" + std::to_string(port)));
    const auto browserDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    UInt32 browserTime = 1001;
    while (!browserReply) {
        CHECK(std::chrono::steady_clock::now() < browserDeadline);
        (void)server->process(browserTime);
        (void)client->process(browserTime++);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    CHECK(client->connect("bad/name:123", 1000) == net::Result::InvalidAddress);
    bool rejected{};
    client->onConnection = [&](const auto& event) {
        if (event.type == net::EventType::Rejected) {
            rejected = true;
        }
    };
    CHECK(client->connect("opent1-host-that-does-not-exist.invalid:123", 1000) == net::Result::Ok);
    const auto failureDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    UInt32 failureTime = 1001;
    while (!rejected) {
        CHECK(std::chrono::steady_clock::now() < failureDeadline);
        (void)client->process(failureTime++);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}
}

int opent1_test_game_network_parity() {
    dedicatedStartupPort();
    localProfileDelivery();
    localObserverMenuState();
    scriptObserverTransforms();
    scriptCreatedItems();
    localScriptKick();
    nativeSkinDelivery();
    nativeChatDelivery();
    nativeLightGhosts();
    delayedControlReplay();
    soundEventDelivery();
    networkCommands();
    liveServerBrowser();
    liveIrcControls();
    originalPscCapture();
    nativeServerFraming();
    recorderLayout();
    ownershipBacklinks();
    remoteJoinWithStartupBots();
    nativeTeamDelivery();
    addressedJoin();
    hostnameConnection();
    return 0;
}

#include "test_registry.hpp"
OPENT1_REGISTER_TEST("game", "game_network_parity", opent1_test_game_network_parity);
