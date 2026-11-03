#include "test_registry.hpp"
#include "test_support.hpp"
#include <opent1/game/ai.hpp>
#include <opent1/game/game_app.hpp>
#include <algorithm>
#include <cmath>
#include <cstdlib>

using namespace opent1;
using namespace opent1::game;

namespace {
struct Wall : sim::SimContainer {
    std::shared_ptr<sim::SimCollisionBoxImage> image =
        std::make_shared<sim::SimCollisionBoxImage>();

    bool processQuery(sim::SimQuery& query) override {
        if (auto* collision = dynamic_cast<sim::SimCollisionImageQuery*>(&query)) {
            collision->images = {image};
            return true;
        }
        return false;
    }
};

void acquisitionAndFollow() {
    FearGameApp app;
    CHECK(app.init());
    CHECK(app.fearPlugin().createServerWorld({}, true));
    auto& globals = app.fearPlugin().serverGlobals();
    auto& console = app.console();
    globals.playerManager->teamAdded("Players", "base");
    globals.playerManager->teamAdded("Enemies", "base");
    CHECK(console
              .evaluate(R"(
        newObject(MissionCleanup, SimGroup);
        PlayerData HuntArmor { mass = 9; boxWidth = 0.5; boxDepth = 0.5; boxNormalHeight = 2.3; };
        AI::spawn(Hunter, HuntArmor, "0 0 100");
        function AI::onTargetLOSAcquired(%name, %target) {
            $Acquired++; $AcquiredId = %target;
            AI::directiveFollow(%name, %target, 0, 99);
        }
        function AI::onTargetLOSLost(%name, %target) {
            $Lost++; AI::directiveRemove(%name, 99);
        }
        function AI::onTargetLOSRegained(%name, %target) {
            $Regained++; AI::directiveFollow(%name, %target, 0, 99);
        }
        function AI::onTargetDied(%name, %target) {
            $Died++; AI::directiveRemove(%name, 99);
        }
        function CustomAcquired(%name, %target) { $CustomAcquired++; }
    )")
              .ok());
    auto* bot = globals.aiManager->findAI("Hunter");
    CHECK(bot && bot->player());
    bot->player()->setTeam(1);
    bot->setVar("iq", "100");
    bot->setVar("spotDist", "50");
    bot->setVar("attackMode", "0");
    bot->setSeekOff({});
    bot->setAutomaticTargets(true);
    const auto client = globals.playerManager->clientAdded("Runner", "male1", "base", true);
    auto owned = std::make_unique<Player>();
    owned->bindDataBlockManager(globals.dbm);
    owned->setDatFileName("HuntArmor");
    auto* target = dynamic_cast<Player*>(globals.manager->addObject(std::move(owned)));
    CHECK(target);
    target->setOwnerClient(client);
    target->setTeam(0);
    target->setPos({0, 80, 100});
    int tick = 0;
    const auto advance = [&] {
        for (int i = 0; i < 12; ++i)
            globals.aiManager->doServerProcesses(++tick * .032f);
    };
    advance();
    CHECK(bot->currentTarget().id == -1);
    CHECK(console.getVariable("Acquired").empty());
    target->setPos({0, 30, 100});
    advance();
    CHECK(console.getVariable("Acquired") == "1");
    CHECK(console.getVariable("AcquiredId") == std::to_string(client));
    CHECK(bot->currentTarget().obj == target);
    CHECK(bot->curDest() == target->getBoxCenter());
    CHECK(bot->lastGeneratedMove().forwardAction > .9f);
    target->setPos({30, 0, 100});
    advance();
    CHECK(bot->curDest() == target->getBoxCenter());
    CHECK(bot->lastGeneratedMove().rightAction > .9f);
    // aiObj.cpp::moveTowardLoc applies the turn generated while aiming before
    // expressing movement in the player's local axes.
    const auto& moving = bot->lastGeneratedMove();
    CHECK(std::abs(moving.rightAction - std::cos(moving.turnRot)) < .0001f);
    CHECK(std::abs(moving.forwardAction - moving.backwardAction + std::sin(moving.turnRot)) <
          .0001f);
    bot->setAttackMode(2);
    advance();
    const auto& standing = bot->lastGeneratedMove();
    CHECK(standing.forwardAction + standing.backwardAction + standing.leftAction +
              standing.rightAction ==
          0);
    bot->setAttackMode(1);
    target->setPos({0, 10, 100});
    advance();
    CHECK(bot->lastGeneratedMove().backwardAction > .9f); // Vanilla's 17-unit standoff.
    bot->setAttackMode(0);
    target->setPos({30, 0, 100});
    advance();

    auto wall = std::make_unique<Wall>();
    wall->setType(sim::InteriorObjectType);
    wall->image->box = {{10, -5, 90}, {20, 5, 110}};
    CHECK(wall->setBoundingBox(wall->image->box));
    auto* blocker = wall.get();
    CHECK(globals.manager->addObject(std::move(wall)));
    auto* root =
        dynamic_cast<sim::SimContainer*>(globals.manager->findObject(sim::RootContainerId));
    CHECK(root && root->addObject(*blocker));
    advance();
    CHECK(bot->currentTarget().id == -1);
    CHECK(console.getVariable("Lost") == "1");
    blocker->deleteObject();
    advance();
    CHECK(console.getVariable("Regained") == "1");
    CHECK(bot->curDest() == target->getBoxCenter());

    // Vanilla forgets previous LOS outside spotDist and acquires afresh on return.
    target->setPos({80, 0, 100});
    advance();
    target->setPos({30, 0, 100});
    advance();
    CHECK(console.getVariable("Acquired") == "2");
    target->setDead(true);
    advance();
    advance();
    CHECK(console.getVariable("Died") == "1");
    CHECK(bot->currentTarget().id == -1);

    target->setDead(false);
    target->setTeam(1);
    advance();
    CHECK(bot->listDirectives(AIObj::TargetDirective).empty());
    target->setTeam(0);
    advance();
    CHECK(console.getVariable("Acquired") == "3");
    CHECK(bot->currentTarget().obj == target);
    CHECK(bot->installDirectiveCallback(1, "CustomAcquired", {}, AIObj::TargetDirective));
    target->setPos({80, 0, 100});
    advance();
    target->setPos({30, 0, 100});
    advance();
    CHECK(console.getVariable("CustomAcquired") == "1");
    CHECK(console.getVariable("Acquired") == "3");

    // Switching to scripted targeting preserves authored targets and removes auto targets.
    CHECK(console.evaluate(std::format("AI::directiveTarget(Hunter, {}, 200);", client)).ok());
    bot->setAutomaticTargets(false);
    const auto targets = bot->listDirectives(AIObj::TargetDirective);
    CHECK(targets.size() == 1 && targets.front().order == 200);
}

void installedRpgHunt() {
    const auto* assets = std::getenv("OPENT1_TEST_ASSETS");
    if (!assets || !std::filesystem::exists(std::filesystem::path(assets) / "RPG/scripts.vol"))
        return;
    FearGameApp::Config config;
    config.assetRoot = assets;
    config.writeRoot = std::filesystem::temp_directory_path() /
                       std::format("opent1-rpg-hunt-{}",
                                   std::chrono::steady_clock::now().time_since_epoch().count());

    struct Cleanup {
        std::filesystem::path path;

        ~Cleanup() {
            std::error_code ec;
            std::filesystem::remove_all(path, ec);
        }
    } cleanup{config.writeRoot};

    FearGameApp app(config);
    const std::array<std::string_view, 2> args{"-mod", "RPG"};
    CHECK(app.init(args));
    auto& console = app.console();
    console.setVariable("PCFG::Name", "HuntTest");
    console.setVariable("PCFG::Script", "");
    console.setVariable("PCFG::Info", "hunt-test-password");
    CHECK(app.loadMission("rpgmap1"));
    for (int tick = 0; tick < 8; ++tick)
        CHECK(app.stepFrame(tick * .032));
    CHECK(app.fearPlugin().focusServer());
    const auto localId = Int32(app.fearPlugin().clientGlobals().manager->getId());
    CHECK(console
              .evaluate(std::format("processMenupickgroup({0}, 3); processMenupickclass({0}, 1);",
                                    localId))
              .ok());
    auto& globals = app.fearPlugin().serverGlobals();
    const auto* local = globals.playerManager->findClient(localId);
    CHECK(local);
    auto* player = dynamic_cast<Player*>(local->ownedObject);
    CHECK(player);
    // Use the crypt zone on rpgmap1 and original RPG scripts, with a controlled enemy.
    player->setPos({2248, 765, 200});
    CHECK(console
              .evaluate(R"(
        SpawnAI(Raider998, HuntEnemy, "2248 785 200", "TempSpawn 2248 785 200 1");
        $AIsmartFOVbots = false;
    )")
              .ok());
    auto* bot = globals.aiManager->findAI("Raider998");
    CHECK(bot && bot->player());
    std::vector<std::string> others;
    for (const auto& ai : globals.aiManager->aiList()) {
        if (ai.get() != bot)
            others.push_back(ai->name);
    }
    for (const auto& name : others)
        CHECK(globals.aiManager->remove(name, true));
    CHECK(console.evaluate(std::format("return fetchData({}, SpawnBotInfo);", bot->id)).value !=
          "");
    // Periodic hunting must install a moving follow target beyond melee spotDist.
    bot->setVar("spotDist", "3");
    for (int tick = 1; tick <= 170; ++tick)
        globals.aiManager->doServerProcesses((1000 + tick * 32) * .001f);
    const auto follows = bot->listDirectives(AIObj::FollowDirective);
    CHECK(follows.size() == 1 && follows.front().order == 99 &&
          follows.front().playerId == localId);
    CHECK((bot->curDest() - (player->getBoxCenter() + bot->seekOff())).len() < .001f);
    player->setPos({2250, 765, 200});
    for (int tick = 171; tick <= 182; ++tick)
        globals.aiManager->doServerProcesses((1000 + tick * 32) * .001f);
    CHECK((bot->curDest() - (player->getBoxCenter() + bot->seekOff())).len() < .001f);
    std::printf("Original RPG periodic hunt follows a moving player beyond melee range.\n");
}
}

int opent1_test_ai_acquisition_and_follow() {
    return opent1::test::run(acquisitionAndFollow);
}

int opent1_test_installed_rpg_hunt() {
    return opent1::test::run(installedRpgHunt);
}

OPENT1_REGISTER_TEST("game", "ai_acquisition_and_follow", opent1_test_ai_acquisition_and_follow);
OPENT1_REGISTER_TEST("trpg", "installed_rpg_hunt", opent1_test_installed_rpg_hunt);
