#include "test_registry.hpp"
#include "test_support.hpp"
#include <opent1/game/game_app.hpp>
#include <opent1/game/ai.hpp>
#include <opent1/game/item.hpp>
#include <opent1/game/moveable.hpp>
#include <opent1/game/marker.hpp>
#include <opent1/game/player.hpp>
#include <opent1/game/player_manager.hpp>
#include <opent1/game/chat_inventory_huds.hpp>
#include <opent1/game/status_combat_huds.hpp>
#include <cstdlib>
#include <numbers>
#include <sstream>

using namespace opent1;
using namespace opent1::game;

namespace {
void itemWorldCollision() {
    const std::array<float, 4> heights{};
    const std::array<world::TerrainCollisionCell, 1> cells{};
    const auto terrain = world::TerrainCollision::prepare(1, 1, 16, heights, cells);
    CHECK(terrain);
    world::SimTerrainCollisionImage ground(
        std::make_shared<const world::TerrainCollision>(*terrain));
    ground.transform = TMat3F(EulerF{0, 0, .4f}, {100, 200, 300});
    ShapeCollisionImage shape;
    shape.sphere.center = {};
    shape.sphere.radius = 1;
    shape.bbox = {{-.5f, -.5f, -.5f}, {.5f, .5f, .5f}};
    Point3F position;
    m_mul(Point3F{4, 4, .25f}, ground.transform, position);
    shape.transform = TMat3F(EulerF{}, position);
    CollisionSurfaceList hits;
    CHECK(sim::SimCollisionImage::test(shape, ground, hits));
    CHECK(hits.surfaces.size() == 1);
    CHECK(std::abs(hits.surfaces.front().distance - .25f) < .001f);

    world::InteriorCollisionSurface wall;
    wall.plane = 0;
    wall.planeFront = false;
    wall.vertices = {{0, -2, -2}, {0, 2, -2}, {0, 2, 2}, {0, -2, 2}};
    const std::array<PlaneF, 1> planes{{{1, 0, 0, 0}}};
    const std::array<world::InteriorCollisionNode, 1> nodes{{{0, -2, -1}}};
    const std::array<world::InteriorCollisionLeaf, 2> leaves{{{false, {}}, {true, {0}}}};
    auto geometry = world::InteriorCollision::prepare(planes, nodes, leaves, std::span(&wall, 1));
    CHECK(geometry);
    world::SimInteriorCollisionImage interior(
        std::make_shared<const world::InteriorCollision>(*geometry));
    interior.transform = ground.transform;
    m_mul(Point3F{.25f, 0, 0}, interior.transform, position);
    shape.transform = TMat3F(EulerF{}, position);
    hits = {};
    CHECK(sim::SimCollisionImage::test(shape, interior, hits));
    CHECK(!hits.surfaces.empty());
}

void rosterSkinsAndZoom() {
    FearGameApp app;
    CHECK(app.init());
    CHECK(app.fearPlugin().createServerWorld({}, true));
    CHECK(app.console().execute("localConnect").value == "True");
    auto& globals = app.fearPlugin().serverGlobals();
    auto* players = globals.playerManager;
    const auto bot = players->aiAdded("Hidden enemy", "male2", "base", true);
    players->setClientSkin(bot, "robeblue");
    CHECK(players->getPlayerSkinBase(bot) == "robeblue");
    app.fearPlugin().syncLoopbackState(false);
    CHECK(app.fearPlugin().clientGlobals().playerManager->getPlayerSkinBase(bot) == "robeblue");
    players->setClientSkin(bot, "rpgleather");
    CHECK(app.fearPlugin().clientGlobals().playerManager->getPlayerSkinBase(bot) == "rpgleather");
    players->setClientScoreHeading("Player");
    ScoreListCtrl scores;
    scores.rebuildScores(*players);
    CHECK(std::ranges::none_of(scores.lines(), [bot](const auto& line) {
        return line.id == bot;
    }));
    CHECK(std::ranges::any_of(scores.lines(), [](const auto& line) {
        return line.id == 2049;
    }));

    players->setTeamSkin(0, "rpgbase");
    auto owned = std::make_unique<Item>();
    auto* item = dynamic_cast<Item*>(globals.manager->addObject(std::move(owned)));
    CHECK(item);
    item->setTeam(0);
    CHECK(item->getSkinBase() == "rpgbase");
    item->setOwnerClient(bot);
    CHECK(item->getSkinBase() == "rpgleather");

    CHECK(app.fearPlugin().focusServer());
    CHECK(app.console()
              .evaluate(R"(
        PlayerData VirtualDamageArmor { maxDamage = 1; };
        newObject(VirtualDamagePlayer, Player, VirtualDamageArmor);
        function VirtualDamageArmor::onDamage(%this, %type, %value) {
            GameBase::setDamageLevel(%this, %value);
        }
        Client::setOwnedObject(2049, VirtualDamagePlayer);
        GameBase::virtual(2049, onDamage, 0, 0.25);
    )")
              .ok());
    const auto* damaged = dynamic_cast<Player*>(globals.manager->findObject("VirtualDamagePlayer"));
    CHECK(damaged && damaged->getDamageLevel() == .25f);

    Player control;
    PlayerPSC psc;
    psc.setControlObject(&control);
    Crosshair crosshair;
    sim::EnvironmentCamera camera;
    const auto update = [&](UInt32 now) {
        crosshair.updateLive(
            *globals.manager, psc, nullptr, nullptr, camera, now, true, false, false);
    };
    const auto oldFov = PlayerPSC::sniperFov;
    update(0);
    CHECK(crosshair.reticleMetrics().enabled);
    CHECK(!crosshair.reticleMetrics().showZoomRectangle);
    PlayerPSC::sniperFov = 45;
    update(1000);
    CHECK(!crosshair.reticleMetrics().showZoomRectangle);
    psc.clientCollectInput(1000, 1000);
    sim::SimActionEvent action;
    action.action = 220114;
    action.fValue = 1;
    CHECK(psc.onSimActionEvent(action));
    update(1000);
    CHECK(crosshair.reticleMetrics().showZoomRectangle);
    update(3000);
    CHECK(!crosshair.reticleMetrics().showZoomRectangle);
    PlayerPSC::sniperFov = oldFov;
    psc.setControlObject(nullptr);
}

void ferryWaypoints() {
    FearGameApp app;
    CHECK(app.init());
    CHECK(app.fearPlugin().createServerWorld({}, true));
    auto& globals = app.fearPlugin().serverGlobals();
    CHECK(app.console()
              .evaluate(R"(
        MoveableData RegressionFerry { speed = 10; displace = false; blockForward = false; blockBackward = false; };
        function RegressionFerry::onNewPath(%this) { $FerryNewPaths++; }
        function RegressionFerry::onWaypoint(%this) { $FerryArrived = GameBase::getPosition(%this); }
    )")
              .ok());
    auto* group =
        dynamic_cast<sim::SimGroup*>(globals.manager->addObject(std::make_unique<sim::SimGroup>()));
    CHECK(group);
    world::PathState state;
    state.compressed = true;
    state.waypoints = {
        {.position = {10, 20, 30}}, {.position = {30, 20, 30}}, {.position = {50, 20, 30}}};
    auto path = std::make_unique<world::SimPath>();
    CHECK(path->setState(state));
    auto* addedPath =
        dynamic_cast<world::SimPath*>(globals.manager->addObject(std::move(path), group));
    CHECK(addedPath && addedPath->publish());
    auto owned = std::make_unique<Moveable>();
    owned->bindDataBlockManager(globals.dbm);
    owned->setDatFileName("RegressionFerry");
    auto* ferry = dynamic_cast<Moveable*>(globals.manager->addObject(std::move(owned), group));
    CHECK(ferry);
    CHECK(app.console().getVariable("FerryNewPaths") == "1");
    CHECK(ferry->moveToWaypoint(1));
    ferry->serverProcess(1000);
    CHECK(std::abs(ferry->getWaypointPosition() - .5f) < .001f);
    CHECK((ferry->getPos() - Point3F{20, 20, 30}).len() < .001f);
    ferry->serverProcess(2000);
    CHECK(ferry->currentWaypoint() == 1);
    CHECK((ferry->getPos() - Point3F{30, 20, 30}).len() < .001f);
    CHECK(app.console().getVariable("FerryArrived") == "30 20 30");
    CHECK(ferry->setWaypoint(0));
    CHECK((ferry->getPos() - Point3F{10, 20, 30}).len() < .001f);
    CHECK(ferry->moveToWaypoint(2));
    ferry->serverProcess(6000);
    CHECK((ferry->getPos() - Point3F{50, 20, 30}).len() < .001f);
    CHECK(ferry->setWaypoint(.5f));
    CHECK((ferry->getPos() - Point3F{10, 20, 30}).len() < .001f);
    CHECK(!ferry->setWaypoint(-1));
}

void installedRpgRegressions() {
    const auto* assets = std::getenv("OPENT1_TEST_ASSETS");
    if (!assets || !std::filesystem::exists(std::filesystem::path(assets) / "RPG/scripts.vol"))
        return;
    FearGameApp::Config config;
    config.assetRoot = assets;
    config.writeRoot = std::filesystem::temp_directory_path() /
                       std::format("opent1-rpg-regressions-{}",
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
    console.setVariable("PCFG::Name", "RegressionTest");
    console.setVariable("PCFG::Script", "");
    console.setVariable("PCFG::Info", "regression-password");
    CHECK(app.loadMission("rpgmap5"));
    CHECK(app.fearPlugin().focusServer());
    auto& globals = app.fearPlugin().serverGlobals();
    std::vector<std::pair<Item*, Point3F>> town;
    std::istringstream townIds(console.getVariable("TownBotList"));
    for (Int32 id; townIds >> id;) {
        auto* bot = dynamic_cast<Item*>(globals.manager->findObject(id));
        CHECK(bot);
        const auto name = bot->getFieldValue("name");
        CHECK(name);
        const auto* group = dynamic_cast<sim::SimGroup*>(
            globals.manager->findObject("MissionGroup/TownBots/" + std::string(*name)));
        CHECK(group);
        const Marker* marker = nullptr;
        for (const auto* object : group->objects()) {
            if (const auto* candidate = dynamic_cast<const Marker*>(object)) {
                marker = candidate;
            }
        }
        CHECK(marker && (bot->getPos() - marker->getLinearPosition()).len() < .001f);
        town.emplace_back(bot, bot->getPos());
        CHECK(bot->getSkinBase() == globals.playerManager->getTeamSkinBase(bot->getTeam()));
    }
    CHECK(town.size() >= 20);
    std::vector<std::pair<Moveable*, Point3F>> ferries;
    for (int i = 1; i <= 3; ++i) {
        auto* ferry = dynamic_cast<Moveable*>(globals.manager->findObject(
            std::format("MissionGroup/Ferry/System{0}/PlatformFerry{0}", i)));
        CHECK(ferry);
        CHECK(ferry->getWaypointCount() > 2);
        ferries.emplace_back(ferry, ferry->getPos());
    }
    for (int tick = 0; tick <= 96; ++tick)
        CHECK(app.stepFrame(tick * .032));
    CHECK(app.fearPlugin().focusServer());
    for (const auto& [ferry, start] : ferries) {
        CHECK((ferry->getPos() - start).len() > 1);
        const auto* ghost =
            dynamic_cast<Moveable*>(app.fearPlugin().findGhostForServerObject(ferry->getId()));
        CHECK(ghost && (ghost->getPos() - ferry->getPos()).len() < 2);
    }
    std::size_t skinMaps = 0;
    for (const auto& [bot, start] : town) {
        const auto distance = (bot->getPos() - start).len();
        if (distance > 2)
            std::printf("Town %d displacement %g\n", bot->getId(), distance);
        CHECK(distance < 2);
        auto* ghost = dynamic_cast<Item*>(app.fearPlugin().findGhostForServerObject(bot->getId()));
        CHECK(ghost && (ghost->getPos() - bot->getPos()).len() < .2f);
        world::WorldShapeQuery query;
        CHECK(ghost->processQuery(query) && query.image);
        CHECK(ghost->currentSkin() == bot->getSkinBase());
        const auto& authored = ghost->getTSShape()->materials->materials;
        const auto& rendered = query.image->resources.materials.list.materials;
        CHECK(authored.size() == rendered.size());
        for (std::size_t i = 0; i < authored.size(); ++i) {
            if (authored[i].mapFile.starts_with("base.")) {
                const auto map =
                    std::string(bot->getSkinBase()) + "." + authored[i].mapFile.substr(5);
                if (app.resources().contains(map)) {
                    CHECK(rendered[i].mapFile == map);
                    ++skinMaps;
                }
            }
        }
    }
    CHECK(skinMaps > 0);
    CHECK(console.evaluate("processMenupickgroup(2049, 3); processMenupickclass(2049, 1);").ok());
    auto* player = dynamic_cast<Player*>(globals.playerManager->findClient(2049)->ownedObject);
    CHECK(player);
    player->setPos({10000, 10000, 10000});
    player->setForceMask(0);
    const auto press = [&](std::string_view key) {
        CHECK(app.bindGameplayActions());
        auto* input = dynamic_cast<sim::SimInputManager*>(
            app.fearPlugin().clientGlobals().manager->findObject(sim::InputManagerId));
        CHECK(input);
        sim::SimInputEvent event;
        event.objInst = sim::inputKeyCode(key.front());
        event.fValue = 1;
        CHECK(input->submit(event));
        event.action = sim::InputBreak;
        event.fValue = 0;
        (void)input->submit(event);
    };
    press("i");
    CHECK(app.guiRuntime()->canvas.top()->getName() == "CmdInventoryGui");
    CHECK(app.gameplayKeyboardActive());
    CHECK(app.bindGameplayActions());
    press("i");
    CHECK(app.guiRuntime()->canvas.top()->getName() == "playGui");
    press("i");
    CHECK(app.escapeGameplay());
    CHECK(app.guiRuntime()->canvas.top()->getName() == "playGui");
    CHECK(globals.playerManager->findClient(2049)->curGuiMode == 1);

    CHECK(app.fearPlugin().focusServer());
    CHECK(console
              .evaluate(R"(
        TossLootbag(2049, "SmallRock 2", 5, "*", 0);
        $RegressionPackList = fetchData(2049, "lootbaglist");
    )")
              .ok());
    const auto packId = std::atoi(console.getVariable("RegressionPackList").c_str());
    auto* pack = dynamic_cast<Item*>(globals.manager->findObject(packId));
    CHECK(pack && pack->throwSource() == player);
    const auto packStart = pack->getPos();
    for (int tick = 97; tick <= 160; ++tick)
        CHECK(app.stepFrame(tick * .032));
    CHECK(globals.manager->findObject(packId));
    CHECK((pack->getPos() - packStart).len() > 1);
    CHECK(app.fearPlugin().focusServer());
    Int32 rockType = -1;
    CHECK(globals.dbm->lookupDataBlock("SmallRock", DataBlockManager::ItemDataType, &rockType));
    const auto rocks = player->getItemCount(rockType);
    player->setPos(pack->getPos());
    CHECK(player->stepPosition(player->getTransform(), .1f, .3f));
    globals.manager->advanceToTime(globals.manager->getCurrentTime());
    CHECK(!globals.manager->findObject(packId));
    CHECK(player->getItemCount(rockType) == rocks + 2);
    std::printf("Thrown RPG pack survived, moved, and was collected.\n");

    CHECK(console
              .evaluate(R"(
        SpawnAI(Raider990, DropEnemy, "10000 10020 10000", "TempSpawn 10000 10020 10000 1");
        $DropEnemy = AI::getId(Raider990);
        Player::kill($DropEnemy);
        $DeathPackList = fetchData($DropEnemy, "lootbaglist");
    )")
              .ok());
    const auto deathPackId = std::atoi(console.getVariable("DeathPackList").c_str());
    CHECK(globals.manager->findObject(deathPackId));
    for (int tick = 161; tick <= 200; ++tick)
        CHECK(app.stepFrame(tick * .032));
    CHECK(globals.manager->findObject(deathPackId));
    auto* deathPackGhost =
        dynamic_cast<Item*>(app.fearPlugin().findGhostForServerObject(deathPackId));
    CHECK(deathPackGhost && !deathPackGhost->isHidden());
    world::WorldShapeQuery deathPackQuery;
    CHECK(deathPackGhost->processQuery(deathPackQuery) && deathPackQuery.image);

    CHECK(app.fearPlugin().focusServer());
    CHECK(console
              .evaluate(R"(
        SpawnAI(Shaman991, CasterEnemy, "10000 10060 10000", "TempSpawn 10000 10060 10000 2");
        $RegressionCaster = AI::getId(Shaman991);
    )")
              .ok());
    auto* caster = globals.aiManager->findAI("Shaman991");
    CHECK(caster && caster->player());
    caster->player()->setForceMask(0);
    caster->player()->setRot({0, 0, std::numbers::pi_v<float>});
    player->setPos({10000, 10040, 10000});
    player->setLinearVelocity({});
    const auto damageBefore = player->getDamageLevel();
    bool cast = false;
    for (int tick = 201; tick <= 440 && player->getDamageLevel() == damageBefore; ++tick) {
        CHECK(app.stepFrame(tick * .032));
        CHECK(app.fearPlugin().focusServer());
        cast |=
            console.evaluate("return fetchData($RegressionCaster, SpellCastStep);").value == "1";
    }
    CHECK(cast);
    CHECK(player->getDamageLevel() > damageBefore);
    std::printf("RPG town markers and skins, ferries, inventory keys, death packs and caster "
                "damage verified.\n");
}
}

int opent1_test_rpg_roster_skins_zoom() {
    return opent1::test::run(rosterSkinsAndZoom);
}

int opent1_test_rpg_item_world_collision() {
    return opent1::test::run(itemWorldCollision);
}

int opent1_test_rpg_ferry_waypoints() {
    return opent1::test::run(ferryWaypoints);
}

int opent1_test_installed_rpg_regressions() {
    return opent1::test::run(installedRpgRegressions);
}

OPENT1_REGISTER_TEST("game", "rpg_roster_skins_zoom", opent1_test_rpg_roster_skins_zoom);
OPENT1_REGISTER_TEST("game", "rpg_item_world_collision", opent1_test_rpg_item_world_collision);
OPENT1_REGISTER_TEST("game", "rpg_ferry_waypoints", opent1_test_rpg_ferry_waypoints);
OPENT1_REGISTER_TEST("trpg", "installed_rpg_regressions", opent1_test_installed_rpg_regressions);
