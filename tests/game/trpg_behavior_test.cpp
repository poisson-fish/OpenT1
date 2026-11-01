#include "test_registry.hpp"
#include "test_support.hpp"
#include <opent1/game/game_app.hpp>
#include <opent1/game/ai.hpp>
#include <opent1/game/player.hpp>
#include <opent1/game/player_manager.hpp>
#include <opent1/game/chat_inventory_huds.hpp>
#include <opent1/game/item.hpp>
#include <opent1/world/world_collision_sim.hpp>
#include <cstdlib>
#include <cmath>
#include <numbers>

using namespace opent1;
using namespace opent1::game;

namespace {
void botRoster() {
    FearGameApp app;
    CHECK(app.init());
    auto& plugin = app.fearPlugin();
    CHECK(plugin.createServerWorld({}, true));
    CHECK(app.console().execute("localConnect").value == "True");
    CHECK(app.console().evaluate("function onClientJoin(%id) { $BotTestJoins++; }").ok());
    auto* server = plugin.serverGlobals().playerManager;
    auto* client = plugin.clientGlobals().playerManager;
    const auto humans = client->getNumClients();
    const auto bot = server->aiAdded("Enemy zone bot", "male2", "base", true);
    CHECK(bot > 0);
    plugin.syncLoopbackState(false);
    CHECK(client->findBaseRep(bot));
    CHECK(client->findBaseRep(bot)->loginStatus == 0);
    CHECK(client->getNumClients() == humans);
    CHECK(app.console().getVariable("BotTestJoins").empty());
}

void botTeams() {
    FearGameApp app;
    CHECK(app.init());
    CHECK(app.fearPlugin().createServerWorld({}, true));
    auto& globals = app.fearPlugin().serverGlobals();
    auto& console = app.console();
    CHECK(console
              .evaluate(R"(
        newObject(MissionCleanup, SimGroup);
        PlayerData BotTestArmor { mass = 9; boxWidth = 0.5; boxDepth = 0.5; boxNormalHeight = 2.3; };
        AI::spawn(TeamBot, BotTestArmor, "0 2 100");
    )")
              .ok());
    auto* bot = globals.aiManager->findAI("TeamBot");
    CHECK(bot && bot->player());
    globals.playerManager->teamAdded("Players", "base");
    globals.playerManager->teamAdded("Enemies", "base");
    const auto human = globals.playerManager->clientAdded("Human", "male1", "base", true);
    auto player = std::make_unique<Player>();
    player->bindDataBlockManager(globals.dbm);
    player->setDatFileName("BotTestArmor");
    auto* target = dynamic_cast<Player*>(globals.manager->addObject(std::move(player)));
    CHECK(target);
    target->setPos({0, 0, 100});
    target->setOwnerClient(human);
    target->setTeam(0);
    bot->player()->setTeam(1);
    CHECK(globals.playerManager->findBaseRep(bot->id)->team == 1);
    CHECK(console.evaluate(std::format("return Player::isAiControlled({});", bot->id)).value ==
          "True");
    CHECK(console.evaluate(std::format("return Player::isAiControlled({});", human)).value ==
          "False");
    bot->setAutomaticTargets(true);
    bot->setVar("iq", "100");
    bot->setVar("triggerPct", "1");
    bot->player()->setRot({0, 0, std::numbers::pi_v<float>});
    bool fired = false;
    for (int tick = 1; tick <= 96; ++tick) {
        globals.aiManager->doServerProcesses(tick * .032f);
        bot->player()->serverProcess(tick * 32);
        fired |= bot->lastGeneratedMove().trigger;
    }
    CHECK(fired);
}

void aimedMelee() {
    FearGameApp app;
    CHECK(app.init());
    CHECK(app.fearPlugin().createServerWorld({}, true));
    auto& globals = app.fearPlugin().serverGlobals();
    CHECK(app.console()
              .evaluate(R"(
        PlayerData MeleeTestArmor { mass = 9; boxWidth = 0.5; boxDepth = 0.5; boxNormalHeight = 2.3; };
        newObject(MeleeAttacker, Player, MeleeTestArmor);
        newObject(MeleeTarget, Player, MeleeTestArmor);
        function MeleeTestArmor::onDamage(%obj, %type, %value) { GameBase::setDamageLevel(%obj, %value); }
    )")
              .ok());
    auto* source = dynamic_cast<Player*>(globals.manager->findObject("MeleeAttacker"));
    auto* target = dynamic_cast<Player*>(globals.manager->findObject("MeleeTarget"));
    CHECK(source && target);
    source->setPos({0, 0, 100});
    // Vanilla Player::getLOSTransform uses the eye, including view pitch.
    // A body-centered horizontal ray misses this target above the attacker.
    source->setViewPitch(.65f);
    const auto eye = source->getEyeTransform();
    Point3F point;
    m_mul(Point3F{0, 3, 0}, eye, point);
    target->setPos(point - Point3F{0, 0, 1.15f});
    CHECK(app.console()
              .evaluate(std::format(R"(
        if (GameBase::getLOSInfo({}, 4))
            GameBase::virtual($los::object, onDamage, 0, 0.25);
    )",
                                    source->getId()))
              .ok());
    CHECK(app.console().getVariable("los::object") == target->scriptThis());
    CHECK(target->getDamageLevel() == .25f);
}

void scriptedWeaponFire() {
    FearGameApp app;
    CHECK(app.init());
    CHECK(app.fearPlugin().createServerWorld({}, true));
    auto& globals = app.fearPlugin().serverGlobals();
    auto& console = app.console();
    CHECK(console
              .evaluate(R"(
        PlayerData ScriptWeaponArmor { mass = 9; maxEnergy = 100; };
        ItemImageData ScriptWeaponImage { weaponType = 0; minEnergy = 0; fireTime = 1; };
        ItemData ScriptWeapon { imageType = ScriptWeaponImage; className = ScriptWeaponClass; };
        ItemImageData FallbackWeaponImage { weaponType = 0; minEnergy = 0; fireTime = 1; };
        ItemData FallbackWeapon { imageType = FallbackWeaponImage; className = ScriptWeaponClass; };
        ItemImageData DefaultWeaponImage { weaponType = 0; minEnergy = 0; fireTime = 1; };
        ItemData DefaultWeapon { imageType = DefaultWeaponImage; };
        function ScriptWeaponImage::onFire(%player, %slot) { $ImageFire = "image"; $FirePlayer = %player; $FireSlot = %slot; }
        function ScriptWeaponClass::onFire(%player, %slot) { $ImageFire = "class"; }
        function ItemImage::onFire(%player, %slot) { $ImageFire = "default"; }
        newObject(ScriptWeaponPlayer, Player, ScriptWeaponArmor);
    )")
              .ok());
    auto* player = dynamic_cast<Player*>(globals.manager->findObject("ScriptWeaponPlayer"));
    CHECK(player);
    player->setEnergy(50);
    for (const auto& [name, callback] :
         std::array<std::pair<const char*, const char*>, 3>{{{"ScriptWeapon", "image"},
                                                             {"FallbackWeapon", "class"},
                                                             {"DefaultWeapon", "default"}}}) {
        Int32 type = -1;
        CHECK(globals.dbm->lookupDataBlock(name, DataBlockManager::ItemDataType, &type));
        CHECK(player->mountItem(type, 0));
        player->updateImageState(0, 1);
        player->setImageTriggerDown(0);
        CHECK(console.getVariable("ImageFire") == callback);
        // Script weapons manage their own costs; the projectile path must not
        // deduct energy or ammunition before entering the script callback.
        CHECK(player->getEnergy() == 50);
        player->setImageTriggerUp(0);
        CHECK(player->unmountItem(0));
    }
    CHECK(console.getVariable("FirePlayer") == player->scriptThis());
    CHECK(console.getVariable("FireSlot") == "0");
}

void movementBounds() {
    PlayerData armor;
    armor.mass = 9;
    armor.boxWidth = armor.boxDepth = .5f;
    armor.boxNormalHeight = 2.3f;
    armor.boxCrouchHeight = 1.6f;
    Player player;
    CHECK(player.initResources(&armor));
    player.setCrouching(true);
    // Player::setBoundingBox in 1.11 only adjusts the ray hit box. The
    // terrain sphere and interior movement box retain their standing size.
    CHECK(player.collisionImage().bbox.fMax.z == armor.boxNormalHeight);
    CHECK(player.collisionImage().crouchBox.fMax.z == armor.boxCrouchHeight);
    CHECK(player.collisionImage().sphere.radius == armor.boxNormalHeight * .5f);
    // Vanilla accepts additional movement forces as well as gravity and jets.
    CHECK(player.addForce(1u << 30, {0, 0, 9}, true));
    PlayerMove idle;
    player.updateMove(&idle, true);
    CHECK(std::abs(player.getLinearVelocity().z - .288f) < .0001f);
}

void movementTrajectory() {
    PlayerData armor;
    armor.mass = 9;
    armor.groundForce = 675;
    armor.groundTraction = 3;
    armor.jumpImpulse = 75;
    armor.maxForwardSpeed = 10;
    armor.boxWidth = armor.boxDepth = .5f;
    armor.boxNormalHeight = 2.3f;
    armor.boxCrouchHeight = 1.6f;
    Console console;
    sim::SimManager manager;
    auto root = std::make_unique<sim::SimDefaultOpenContainer>();
    root->setId(sim::RootContainerId);
    CHECK(manager.addObject(std::move(root)));
    const std::array<float, 4> heights{0, 0, 64, 64};
    const std::array<world::TerrainCollisionCell, 1> cells{};
    auto terrain = world::TerrainCollision::prepare(1, 1, 128, heights, cells);
    CHECK(terrain);
    auto geometry = std::make_shared<const world::TerrainCollision>(std::move(*terrain));
    CHECK(manager.addObject(
        std::make_unique<world::SimWorldCollider>(world::SimTerrainCollisionImage(geometry))));
    auto owned = std::make_unique<Player>();
    auto& player = *owned;
    CHECK(player.initResources(&armor));
    CHECK(manager.addObject(std::move(owned)));
    player.setPos({16, 16, 8.1f});
    player.addForce(sim::MovementGravityForce, {0, 0, -20}, true);
    PlayerMove idle;
    for (int tick = 0; tick < 8; ++tick)
        player.updateMove(&idle, true);
    CHECK(player.didContact());
    player.setLinearVelocity({});
    player.setTraction(1);
    PlayerMove run;
    run.forwardAction = 1;
    run.turnRot = std::numbers::pi_v<float> * .5f;
    player.updateMove(&run, true);
    // Independent 1.11 ground acceleration and surface projection, for a
    // slope z=y/2: +2.4 forward, -.64 gravity, then remove inward velocity.
    Point3F expected{0, 2.4f, -.64f};
    Point3F normal{0, -1.f / std::sqrt(5.f), 2.f / std::sqrt(5.f)};
    expected += normal * (-m_dot(expected, normal) + .0001f);
    CHECK((player.getLinearVelocity() - expected).len() < .001f);
    CHECK(std::abs(player.getRot().z - run.turnRot) < .0001f);
    // The new heading affects acceleration on the following tick. Turning
    // back now still accelerates left before committing the original heading.
    run.turnRot = -run.turnRot;
    player.updateMove(&run, true);
    CHECK(player.getLinearVelocity().x < -.1f);
    run.turnRot = 0;
    const auto start = player.getPos();
    for (int tick = 0; tick < 24; ++tick)
        player.updateMove(&run, true);
    CHECK(player.getPos().y > start.y + 4 && player.getPos().z > start.z + 2);

    // Airborne takeoff and arc use the native 32 ms trapezoidal integrator.
    player.setPos({200, 200, 100});
    player.setLinearVelocity({});
    player.setJumpSurfaceLastContact(0);
    PlayerMove jump;
    jump.jumpAction = true;
    float z = 100, velocity = 75.f / 9.f;
    for (int tick = 0; tick < 28; ++tick) {
        z += (velocity + velocity - .64f) * .016f;
        velocity -= .64f;
        player.updateMove(tick == 0 ? &jump : &idle, true);
        CHECK(std::abs(player.getPos().z - z) < .001f);
        CHECK(std::abs(player.getLinearVelocity().z - velocity) < .001f);
    }

    // Smoothing a received grounded update must retain its predicted contact.
    player.bindConsole(&console);
    console.setVariable("net::interpolateTime", "64");
    console.setVariable("net::predictForwardTime", "0");
    player.ghostSetMove(idle, {16, 16, 8.1f}, {}, true, 0, 0, 0, false, 32);
    CHECK(player.didContact());
}

void installedRpgCombat() {
    const auto* assets = std::getenv("OPENT1_TEST_ASSETS");
    if (!assets || !std::filesystem::exists(std::filesystem::path(assets) / "RPG/scripts.vol"))
        return;
    FearGameApp::Config config;
    config.assetRoot = assets;
    config.writeRoot = std::filesystem::temp_directory_path() /
                       std::format("opent1-rpg-combat-{}",
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
    console.setVariable("PCFG::Name", "CombatTest");
    console.setVariable("PCFG::Script", "");
    console.setVariable("PCFG::Info", "combat-test-password");
    CHECK(app.loadMission("rpgmap5"));
    for (int tick = 0; tick < 8; ++tick)
        CHECK(app.stepFrame(tick * .032));
    CHECK(app.fearPlugin().focusServer());
    CHECK(console.evaluate("processMenupickgroup(2049, 3); processMenupickclass(2049, 1);").ok());
    auto& globals = app.fearPlugin().serverGlobals();
    auto* player = dynamic_cast<Player*>(globals.playerManager->findClient(2049)->ownedObject);
    CHECK(player);
    player->setPos({10000, 10000, 10000});
    player->setRot({});
    player->setViewPitch(0);
    CHECK(console
              .evaluate(R"(
        SpawnAI(Raider900, CombatEnemy, "10000 10002 10000", "TempSpawn 10000 10002 10000 1");
        Player::mountItem(2049, PickAxe, 0);
    )")
              .ok());
    auto* bot = globals.aiManager->findAI("Raider900");
    CHECK(bot && bot->player());
    CHECK(globals.playerManager->findBaseRep(bot->id)->team == 1);
    CHECK(bot->player()->getItemImageEntry(0).imageId >= 0);
    CHECK(player->getItemImageEntry(0).imageId >= 0);
    player->setForceMask(0);
    bot->player()->setForceMask(0);
    // Both actors remain in melee range while the original scripts handle
    // normal RPG hit rolls, weapon delays and HP conversion.
    CHECK(console.evaluate("$PlayerSkill[2049, $SkillPiercing] = 100; 2049.lastFireTime = -100;")
              .ok());
    const auto damage = bot->player()->getDamageLevel();
    player->setImageTriggerDown(0);
    for (int tick = 1; tick <= 640 && bot->player()->getDamageLevel() == damage; ++tick) {
        globals.currentTime = 10000 + tick * 32;
        player->updateImageState(0, .032f);
    }
    player->setImageTriggerUp(0);
    CHECK(bot->player()->getDamageLevel() > damage);
    CHECK(!bot->player()->isDead());
    bot->player()->setRot({0, 0, std::numbers::pi_v<float>});
    bool fired = false;
    const auto playerDamage = player->getDamageLevel();
    for (int tick = 1; tick <= 640 && player->getDamageLevel() == playerDamage; ++tick) {
        globals.currentTime = 40000 + tick * 32;
        globals.aiManager->doServerProcesses(globals.currentTime * .001f);
        bot->player()->serverProcess(globals.currentTime);
        fired |= bot->lastGeneratedMove().trigger;
    }
    CHECK(fired);
    CHECK(player->getDamageLevel() > playerDamage);
    std::printf("RPG pickaxe damaged bot; enemy AI acquired, fired at and damaged player.\n");

    // Exercise the original RPG sling script, whose projectiles are Items.
    CHECK(console.evaluate(std::format(R"(
        storeData({0}, "LoadedProjectile Sling", SmallRock);
        Player::incItemCount({0}, SmallRock, 2);
        {0}.lastFireTime = -100;
        ProjectileAttack({0}, Sling, 30);
    )", bot->id)).ok());
    auto* cleanupGroup = dynamic_cast<sim::SimGroup*>(globals.manager->findObject("MissionCleanup"));
    CHECK(cleanupGroup);
    Item* rock = nullptr;
    for (auto* object : cleanupGroup->objects()) {
        auto* candidate = dynamic_cast<Item*>(object);
        if (candidate && candidate->getDatFileName() == "SmallRock" &&
            candidate->throwSource() == bot->player()) {
            rock = candidate;
        }
    }
    CHECK(rock);
    const auto start = rock->getPos();
    rock->serverProcess(rock->getLastProcessTime() + 32);
    CHECK((rock->getPos() - start).len() > .1f);

    // A real chest/player overlap must invoke Item::onCollision and grant loot.
    Int32 rockType = -1;
    CHECK(globals.dbm->lookupDataBlock("SmallRock", DataBlockManager::ItemDataType, &rockType));
    const auto inventory = player->getItemCount(rockType);
    CHECK(console.evaluate(R"(
        newObject(PathLoot, Item, Lootbag, 1, false);
        GameBase::setPosition(PathLoot, "10000 10010 10000");
        $loot[nameToId(PathLoot)] = "* * SmallRock 2";
    )").ok());
    auto* chest = globals.manager->findObject("PathLoot");
    CHECK(chest);
    const auto chestId = chest->getId();
    player->setPos({10000, 10010, 10000});
    CHECK(player->stepPosition(player->getTransform(), .1f, .3f));
    globals.manager->advanceToTime(globals.manager->getCurrentTime());
    CHECK(!globals.manager->findObject(chestId));
    CHECK(player->getItemCount(rockType) == inventory + 2);

    // Rechecking an unchanged enemy zone must announce the entry only once.
    std::vector<std::string> messages;
    auto* localPlayers = app.fearPlugin().clientGlobals().playerManager;
    auto previousChat = localPlayers->onChatMessage;
    localPlayers->onChatMessage = [&](Int32, std::string_view message) {
        messages.emplace_back(message);
    };
    CHECK(console.evaluate(R"(
        for (%z = 1; %z <= $numZones; %z++) {
            if ($Zone::Type[%z] == "DUNGEON") {
                GameBase::setPosition(2049, $Zone::Marker[%z]);
                break;
            }
        }
        DoZoneCheck(2, 0);
        DoZoneCheck(2, 0);
        DoZoneCheck(2, 0);
    )").ok());
    const auto entries = std::ranges::count_if(messages, [](const auto& message) {
        return message.starts_with("You have entered ");
    });
    CHECK(entries == 1);
    localPlayers->onChatMessage = std::move(previousChat);

    // Check the loaded play.gui editor across rendering and repeated submission.
    app.fearPlugin().syncLoopbackState(false);
    CHECK(app.setGameplayGuiMode(1));
    FearGuiChat* chat = nullptr;
    const auto findChat = [&](auto&& self, gui::Control& control) -> void {
        if (auto* candidate = dynamic_cast<FearGuiChat*>(&control)) {
            chat = candidate;
        }
        for (auto* child : control.objects()) {
            self(self, *static_cast<gui::Control*>(child));
        }
    };
    findChat(findChat, *app.guiRuntime()->canvas.top());
    CHECK(chat);
    auto surface = gfx::SoftwareSurface::create(640, 480);
    CHECK(surface && app.renderFrame(*surface));
    for (int i = 0; i < 2; ++i) {
        chat->activate();
        CHECK(app.renderFrame(*surface));
        gui::Event input;
        input.type = gui::EventType::Text;
        input.text = "RPG chat test";
        CHECK(app.guiRuntime()->canvas.dispatch(input));
        CHECK(chat->text == input.text);
        input.type = gui::EventType::KeyDown;
        input.key = gui::Key::Return;
        CHECK(app.guiRuntime()->canvas.dispatch(input));
        CHECK(!chat->visible());
    }
    std::printf("RPG bot sling, chest pickup, stable zone entry and reopened chat verified.\n");
}
}

int opent1_test_bot_roster() {
    return opent1::test::run(botRoster);
}

int opent1_test_bot_teams() {
    return opent1::test::run(botTeams);
}

int opent1_test_aimed_melee() {
    return opent1::test::run(aimedMelee);
}

int opent1_test_installed_rpg_combat() {
    return opent1::test::run(installedRpgCombat);
}

int opent1_test_movement_bounds() {
    return opent1::test::run(movementBounds);
}

int opent1_test_movement_trajectory() {
    return opent1::test::run(movementTrajectory);
}

int opent1_test_scripted_weapon_fire() {
    return opent1::test::run(scriptedWeaponFire);
}

OPENT1_REGISTER_TEST("game", "bot_roster", opent1_test_bot_roster);
OPENT1_REGISTER_TEST("game", "bot_teams", opent1_test_bot_teams);
OPENT1_REGISTER_TEST("game", "aimed_melee", opent1_test_aimed_melee);
OPENT1_REGISTER_TEST("game", "movement_bounds", opent1_test_movement_bounds);
OPENT1_REGISTER_TEST("game", "movement_trajectory", opent1_test_movement_trajectory);
OPENT1_REGISTER_TEST("game", "scripted_weapon_fire", opent1_test_scripted_weapon_fire);
OPENT1_REGISTER_TEST("trpg", "installed_rpg_combat", opent1_test_installed_rpg_combat);
