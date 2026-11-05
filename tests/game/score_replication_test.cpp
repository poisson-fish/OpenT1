#include "sim_net_test_support.hpp"
#include "test_registry.hpp"
#include <opent1/game/chat_inventory_huds.hpp>
#include <opent1/game/fear_player_psc.hpp>
#include <opent1/game/game_app.hpp>
#include <opent1/game/player_manager.hpp>

using namespace opent1::game;
using namespace sim_net_test;

namespace {
unsigned drain(Endpoint& from, Endpoint& to) {
    unsigned count = 0;
    while (auto packet = from.stream->writePacket()) {
        CHECK(++count < 64);
        CHECK(to.stream->readPacket(packet->bytes, 0, packet->bits));
        CHECK(from.stream->handleNotify(packet->key, true));
    }
    return count;
}

void scoreDelivery(bool usePacketStream) {
    auto protocol = std::make_shared<Registry>();
    CHECK(registerPlayerManagerEvents(*protocol));
    Endpoint from(protocol, true), to(protocol);
    auto* server = add<PlayerManager>(from.manager(), true);
    auto* client = add<PlayerManager>(to.manager(), false);
    // Headings and rows exist before this client joins. Only production setters
    // on the server may supply the receiving manager's score state.
    server->setClientScoreHeading("Player\tScore");
    server->setTeamScoreHeading("Team\tScore");
    for (auto* players : {server, client}) {
        CHECK(players->clientAdded("Viewer", "male1", "base", true, 2049) == 2049);
        CHECK(players->clientAdded("Peer", "male1", "base", true, 2050) == 2050);
        CHECK(players->aiAdded("Bot", "male1", "base", true, 2051) == 2051);
    }
    const auto attach = [&](Endpoint& endpoint) {
        auto* viewer = server->findClient(2049);
        viewer->packetStream = usePacketStream ? endpoint.stream : nullptr;
        viewer->eventManager = endpoint.events;
    };
    attach(from);
    unsigned hiddenScoreRows = 0;
    server->onLocalEvent = [&](std::unique_ptr<SimEvent> event) {
        CHECK(event->address.managerId == 2050);
        if (auto* score = dynamic_cast<DeltaScoreEvent*>(event.get())) {
            if ((score->isTeam && score->id >= 0) ||
                (!score->isTeam && score->id != PlayerManager::BaseClientId)) {
                ++hiddenScoreRows;
            }
        }
    };
    server->teamAdded("TeamOne");
    CHECK(drain(from, to) > 0);
    server->setTeamScore(0, "%t\t9", 9);
    server->setClientScore(2049, "%n\t7", 7);
    server->setClientScore(2050, "%n\t3", 3);
    CHECK(drain(from, to) == 0);
    CHECK(client->clientScoreHeading().empty());
    CHECK(client->findClient(2049)->scoreString.empty());

    server->setDisplayScores(2049, true);
    CHECK(drain(from, to) > 0);
    CHECK(client->clientScoreHeading() == "Player\tScore");
    CHECK(client->teamScoreHeading() == "Team\tScore");
    CHECK(client->findClient(2049)->scoreString == "%n\t7");
    CHECK(client->findClient(2049)->scoreSortValue == 7);
    CHECK(client->findTeam(0)->scoreString == "%t\t9");
    CHECK(client->findTeam(0)->scoreSortValue == 9);
    ScoreListCtrl scores;
    scores.rebuildScores(*client);
    CHECK(scores.lines().size() == 5);
    CHECK(scores.getCellText({0, 1}) == "TeamOne\t9");
    CHECK(scores.getCellText({0, 3}) == "Viewer\t7");
    CHECK(scores.getCellText({0, 4}) == "Peer\t3");

    // A sort-only change must arrive and reorder the GUI's player rows.
    server->setClientScore(2050, "%n\t3", 20);
    server->setTeamScore(0, "%t\t10", -10);
    CHECK(drain(from, to) > 0);
    CHECK(client->findTeam(0)->scoreSortValue == -10);
    scores.rebuildScores(*client);
    CHECK(scores.getCellText({0, 3}) == "Peer\t3");
    server->setClientScore(2050, "%n\t3", 20);
    server->setTeamScore(0, "%t\t10", -10);
    server->setClientScore(2051, "Bot score", 100);
    server->setClientScore(9999, "Invalid", 100);
    server->setTeamScore(99, "Invalid", 100);
    // Team -1 is the wire's heading sentinel, not a playable score row.
    server->setTeamScore(-1, "Neutral score", 100);
    CHECK(drain(from, to) == 0);
    CHECK(client->teamScoreHeading() == "Team\tScore");

    server->setDisplayScores(2049, false);
    server->setClientScore(2049, "%n\t11", 11);
    server->setTeamScore(0, "%t\t12", 12);
    server->setClientScoreHeading("Name\tPoints");
    server->setTeamScoreHeading("Side\tPoints");
    CHECK(drain(from, to) > 0);
    // Headings go to hidden viewers; row updates wait for the next show.
    CHECK(client->clientScoreHeading() == "Name\tPoints");
    CHECK(client->teamScoreHeading() == "Side\tPoints");
    CHECK(client->findClient(2049)->scoreString == "%n\t7");
    CHECK(client->findTeam(0)->scoreString == "%t\t10");
    server->setDisplayScores(2049, true);
    CHECK(drain(from, to) > 0);
    CHECK(client->findClient(2049)->scoreString == "%n\t11");
    CHECK(client->findTeam(0)->scoreString == "%t\t12");

    // Mission reset must clear old rows and sort values on both transports,
    // then allow new team/score events without a reconnect.
    server->missionReset();
    server->teamAdded("TeamTwo");
    CHECK(drain(from, to) > 0);
    CHECK(client->getNumTeams() == 1);
    CHECK(client->findTeam(0)->name == "TeamTwo");
    CHECK(client->findTeam(0)->scoreString.empty());
    CHECK(client->findClient(2049)->scoreString.empty());
    CHECK(client->findClient(2049)->scoreSortValue == 0);
    server->setClientScore(2049, "%n\t1", 1);
    CHECK(drain(from, to) > 0);
    CHECK(client->findClient(2049)->scoreString == "%n\t1");
    CHECK(hiddenScoreRows == 0);

    // A fresh connection reusing an ID must receive its own catch-up snapshot.
    server->clientDropped(2049);
    client->reset();
    Endpoint reconnectedFrom(protocol, true), reconnectedTo(protocol);
    auto* reconnected = add<PlayerManager>(reconnectedTo.manager(), false);
    CHECK(server->clientAdded("Rejoined", "male1", "base", true, 2049) == 2049);
    CHECK(server->findClient(2049)->sendScores == 0);
    CHECK(reconnected->clientAdded("Rejoined", "male1", "base", true, 2049) == 2049);
    CHECK(reconnected->clientAdded("Peer", "male1", "base", true, 2050) == 2050);
    attach(reconnectedFrom);
    // Teams normally precede scores in the connection bootstrap.
    reconnected->teamAdded("TeamTwo");
    server->setClientScore(2049, std::string(300, 'x'), -123);
    server->setDisplayScores(2049, true);
    CHECK(drain(reconnectedFrom, reconnectedTo) > 0);
    CHECK(reconnected->clientScoreHeading() == "Name\tPoints");
    CHECK(reconnected->teamScoreHeading() == "Side\tPoints");
    CHECK(reconnected->findClient(2049)->scoreString == std::string(255, 'x'));
    CHECK(reconnected->findClient(2049)->scoreSortValue == -123);
    CHECK(reconnected->findClient(2050)->scoreString.empty());
    server->setClientScore(2049, {}, 0);
    CHECK(drain(reconnectedFrom, reconnectedTo) > 0);
    CHECK(reconnected->findClient(2049)->scoreString.empty());
    server->findClient(2049)->packetStream = nullptr;
    server->findClient(2049)->eventManager = nullptr;
    server->onLocalEvent = {};
}

void scoreGuiModes() {
    auto protocol = std::make_shared<Registry>();
    CHECK(registerPlayerManagerEvents(*protocol));
    Endpoint from(protocol, true), to(protocol);
    auto* server = add<PlayerManager>(from.manager(), true);
    auto* client = add<PlayerManager>(to.manager(), false);
    for (auto* players : {server, client}) {
        CHECK(players->clientAdded("Viewer", "male1", "base", true, 2049) == 2049);
    }
    server->findClient(2049)->packetStream = from.stream;
    server->teamAdded("Team");
    server->serverClientTeamChange(2049, 0);
    server->setClientScoreHeading("Player");
    server->setTeamScoreHeading("Team");
    server->setClientScore(2049, "%n 1", 1);
    CHECK(drain(from, to) > 0);

    // Objective mode leaves score visibility alone. Victory takes a final
    // snapshot even if scores are hidden, then freezes further row updates.
    server->clientGuiModeChanged(2049, 3, PlayerPSC::InitialGuiMode);
    (void)drain(from, to);
    CHECK(client->findClient(2049)->scoreString.empty());
    server->clientGuiModeChanged(2049, 5, 3);
    CHECK(drain(from, to) > 0);
    CHECK(server->findClient(2049)->sendScores == 0);
    CHECK(client->findClient(2049)->scoreString == "%n 1");
    server->setClientScore(2049, "%n 2", 2);
    server->setDisplayScores(2049, true);
    CHECK(drain(from, to) == 0);
    CHECK(client->findClient(2049)->scoreString == "%n 1");
    server->clientGuiModeChanged(2049, PlayerPSC::PlayGuiMode, 5);
    CHECK(drain(from, to) > 0);
    CHECK(client->findClient(2049)->scoreString == "%n 2");
    server->clientGuiModeChanged(2049, 5, PlayerPSC::PlayGuiMode);
    server->setTeamScore(0, "%t 3", 3);
    CHECK(drain(from, to) == 0);
    server->clientGuiModeChanged(2049, 3, 5);
    CHECK(drain(from, to) > 0);
    CHECK(client->findTeam(0)->scoreString == "%t 3");

    server->clientGuiModeChanged(2049, 5, 3);
    server->missionReset();
    (void)drain(from, to);
    CHECK(client->findClient(2049)->scoreString == "%n 2");
    server->clientGuiModeChanged(2049, PlayerPSC::PlayGuiMode, 5);
    CHECK(drain(from, to) > 0);
    CHECK(client->findClient(2049)->scoreString.empty());

    DeltaScoreEvent untrusted;
    untrusted.id = 2049;
    untrusted.score = "Untrusted";
    CHECK(!server->processEvent(untrusted));
    CHECK(server->findClient(2049)->scoreString.empty());
    CHECK(drain(from, to) == 0);
}

void localScoreDelivery() {
    FearGameApp app;
    CHECK(app.init());
    auto& plugin = app.fearPlugin();
    CHECK(plugin.createServerWorld({}, true));
    CHECK(app.console().execute("localConnect").value == "True");
    (void)plugin.syncLoopbackState();
    auto* server = plugin.serverGlobals().playerManager;
    auto* client = plugin.clientGlobals().playerManager;
    const auto id = server->getClientList()->id;
    server->setClientScoreHeading("Player");
    server->setClientScore(id, "%n 5", 5);
    const std::array<std::string, 2> args{std::to_string(id), "true"};
    CHECK(app.console().execute("Client::setMenuScoreVis", args).value == "True");
    CHECK(client->clientScoreHeading() == "Player");
    CHECK(client->findClient(id)->scoreString == "%n 5");
    server->clientGuiModeChanged(id, 5, server->findClient(id)->curGuiMode);
    server->setClientScore(id, "%n 6", 6);
    (void)plugin.syncLoopbackState();
    CHECK(client->findClient(id)->scoreString == "%n 5");
    server->clientGuiModeChanged(id, PlayerPSC::PlayGuiMode, 5);
    CHECK(client->findClient(id)->scoreString == "%n 6");
    ScoreListCtrl scores;
    scores.rebuildScores(*client);
    CHECK(scores.lines().size() == 2);
}
}

int opent1_test_score_replication() {
    scoreDelivery(true);
    scoreDelivery(false);
    scoreGuiModes();
    localScoreDelivery();
    return 0;
}

OPENT1_REGISTER_TEST("game", "native_score_replication", opent1_test_score_replication)
