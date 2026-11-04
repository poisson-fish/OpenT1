// Expectations are derived from legacy/program/code, not an original binary.
// Synthetic checks run in CTest; an asset root adds an installed hunt trace.
// --mayor observes normal map5 spawning; optional 'patrol' loads its installed
// patrol script into this isolated server without editing the installation.
#include <opent1/game/ai.hpp>
#include <opent1/game/game_app.hpp>
#include <opent1/game/proj_target_laser.hpp>
#include <opent1/world/world_collision_sim.hpp>
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

using namespace opent1;
using namespace opent1::game;

namespace {
void require(bool condition, std::string_view message) {
    if (!condition)
        throw std::runtime_error(std::string(message));
}

int differences{}, comparisons{};

void compare(std::string_view name, double observed, double expected, double tolerance = 0) {
    const bool equal = std::abs(observed - expected) <= tolerance;
    ++comparisons;
    differences += !equal;
    std::printf("%s %.*s observed=%.6f legacy_source=%.6f\n",
                equal ? "MATCH" : "DIFF",
                int(name.size()),
                name.data(),
                observed,
                expected);
}

struct Fixture {
    FearGameApp app;
    AIObj* bot{};
    Player* target{};
    Int32 targetId{};
    int tick{};

    Fixture() {
        require(app.init(), "init");
        require(app.fearPlugin().createServerWorld({}, true), "server");
        auto& g = app.fearPlugin().serverGlobals();
        g.playerManager->teamAdded("Players", "base");
        g.playerManager->teamAdded("Enemies", "base");
        eval(R"(
            newObject(MissionCleanup, SimGroup);
            PlayerData AuditArmor {
                mass=9; boxWidth=0.5; boxDepth=0.5; boxNormalHeight=2.3;
                maxForwardSpeed=0; maxBackwardSpeed=0; maxSideSpeed=0;
            };
            AI::spawn(Auditor, AuditArmor, "0 0 100");
            function AI::onTargetLOSAcquired(%name, %target) { $Acquired++; }
        )");
        bot = g.aiManager->findAI("Auditor");
        require(bot && bot->player(), "bot");
        bot->player()->setTeam(1);
        bot->player()->setForceMask(0);
        bot->setIq(100);
        bot->setAttackMode(2);
        targetId = g.playerManager->clientAdded("Target", "male1", "base", true);
        auto owned = std::make_unique<Player>();
        owned->bindDataBlockManager(g.dbm);
        owned->setDatFileName("AuditArmor");
        target = dynamic_cast<Player*>(g.manager->addObject(std::move(owned)));
        require(target != nullptr, "target");
        target->setOwnerClient(targetId);
        target->setTeam(0);
        target->setPos({0, 20, 100});
        target->setForceMask(0);
    }

    void eval(std::string_view script) {
        require(app.console().evaluate(script).ok(), script);
    }

    void targetDirective(int order) {
        eval(std::format("AI::directiveTarget(Auditor, {}, {});", targetId, order));
    }

    void advance(int ticks) {
        auto& g = app.fearPlugin().serverGlobals();
        for (int i = 0; i < ticks; ++i) {
            g.currentTime = ++tick * 32;
            g.aiManager->doServerProcesses(tick * .032f);
            // AI-only loops exhaust Player's move debt after six submissions.
            if (auto* current = g.aiManager->findAI("Auditor"); current && current->player())
                current->player()->serverProcess(g.currentTime);
        }
    }

    double acquired() {
        const auto value = app.console().getVariable("Acquired");
        return value.empty() ? 0 : std::stod(value);
    }

    void walkingFloor() {
        auto& g = app.fearPlugin().serverGlobals();
        auto* armor = dynamic_cast<PlayerData*>(
            g.dbm->lookupDataBlock("AuditArmor", DataBlockManager::PlayerDataType));
        require(armor != nullptr, "walking armor");
        armor->maxForwardSpeed = 7;
        armor->maxBackwardSpeed = 5.6f;
        armor->maxSideSpeed = 5.25f;
        armor->groundForce = 675;
        armor->groundTraction = 3;
        const std::array<Flt32, 4> heights{};
        const std::array<world::TerrainCollisionCell, 1> cells{};
        auto floor = world::TerrainCollision::prepare(1, 1, 256.f, heights, cells);
        require(floor.has_value(), "waypoint floor");
        require(g.manager->addObject(std::make_unique<world::SimWorldCollider>(
                    world::SimTerrainCollisionImage(
                        std::make_shared<const world::TerrainCollision>(std::move(*floor))),
                    TMat3F(EulerF{}, Point3F{-100, -100, 100}))) != nullptr,
                "waypoint floor collider");
        bot->player()->setPos({0, 0, 100});
        bot->player()->setForceMask(~UInt32{0});
        bot->player()->addForce(sim::MovementGravityForce, {0, 0, -20});
    }
};

void waypointSynthetic() {
    // Exercise physical travel in every quadrant, including unequal armor axis
    // speeds, rather than asserting only the generated input direction.
    const std::array<Point3F, 8> destinations{{{0, 20, 100},
                                               {20, 20, 100},
                                               {20, 0, 100},
                                               {20, -20, 100},
                                               {0, -20, 100},
                                               {-20, -20, 100},
                                               {-20, 0, 100},
                                               {-20, 20, 100}}};
    for (std::size_t i = 0; i < destinations.size(); ++i) {
        Fixture f;
        f.walkingFloor();
        const auto goal = destinations[i];
        f.eval(std::format(
            "AI::directiveWaypoint(Auditor, \"{} {} {}\", 99);", goal.x, goal.y, goal.z));
        Flt32 nearest = 100, corridor = 0;
        const auto length = std::hypot(goal.x, goal.y);
        for (int tick = 0; tick < 250; ++tick) {
            f.advance(1);
            const auto p = f.bot->player()->getPos();
            nearest = std::min(nearest, std::hypot(p.x - goal.x, p.y - goal.y));
            corridor = std::max(corridor, std::abs(p.x * goal.y - p.y * goal.x) / length);
        }
        compare(std::format("waypoint_bearing{}_arrives", i), nearest < 1.2f, 1);
        compare(std::format("waypoint_bearing{}_corridor", i), corridor < 2.f, 1);
    }
    for (int type = 0; type < 3; ++type) {
        Fixture f;
        f.bot->setPathType(type);
        f.eval(R"(
            AI::directiveWaypoint(Auditor, "0 20 100", 100);
            AI::directiveWaypoint(Auditor, "20 20 100", 200);
            AI::directiveWaypoint(Auditor, "20 0 100", 300);
        )");
        f.advance(1);
        compare(std::format("path{}_first", type), f.bot->curDest().y, 20);
        f.bot->player()->setPos({0, 20, 100});
        f.advance(1);
        compare(std::format("path{}_second", type), f.bot->curDest().x, 20);
        f.bot->player()->setPos({20, 20, 100});
        f.advance(1);
        compare(std::format("path{}_third", type), f.bot->curDest().y, 0);
        f.bot->player()->setPos({20, 0, 100});
        f.advance(1);
        if (type == AIObj::OneWay) {
            const auto& move = f.bot->lastGeneratedMove();
            compare("one_way_stops",
                    move.forwardAction + move.backwardAction + move.leftAction + move.rightAction,
                    0);
        } else {
            compare(std::format("path{}_endpoint_next_x", type),
                    f.bot->curDest().x,
                    type == AIObj::TwoWay ? 20 : 0);
            compare(std::format("path{}_endpoint_next_y", type), f.bot->curDest().y, 20);
        }
    }
    {
        Fixture f;
        f.eval("AI::directiveWaypoint(Auditor, \"0 20 100\", 99);");
        f.advance(2);
        f.eval(
            "AI::directiveRemove(Auditor, 99); AI::directiveWaypoint(Auditor, \"20 0 100\", 99);");
        f.advance(1);
        compare("replace_active_order99_destination", f.bot->curDest().x, 20);
        f.bot->player()->setPos({20, 0, 100});
        f.advance(1);
        f.eval(
            "AI::directiveRemove(Auditor, 99); AI::directiveWaypoint(Auditor, \"0 -20 100\", 99);");
        f.advance(1);
        compare("replace_completed_order99_destination", f.bot->curDest().y, -20);
    }
    {
        Fixture f;
        f.eval("AI::spawn(AliasBot, AuditArmor, \"0 0 100\", \"0 0 0\", PublicAlias);");
        auto* alias = f.app.fearPlugin().serverGlobals().aiManager->findAI("AliasBot");
        require(alias != nullptr, "alias bot");
        f.eval("AI::directiveWaypoint(PublicAlias, \"20 20 100\", 99);");
        compare("display_name_waypoint_matches", double(alias->directives().size()), 1);
        f.eval("AI::setVar(\"Public*\", attackMode, 2);");
        compare("display_name_wildcard_variable_matches", alias->attackMode(), 2);
        std::vector<AIObj*> matches;
        f.app.fearPlugin().serverGlobals().aiManager->buildExpressionList(matches, "*");
        compare("display_and_internal_match_once", double(matches.size()), 2);
    }
    {
        Fixture f;
        f.targetDirective(100);
        f.advance(10);
        f.bot->attackPlayer(99999);
        f.advance(1);
        compare("legacy_attack_suppresses_stale_directive_target", f.bot->attackedTargetId(), -1);
        compare("legacy_attack_clears_directive_selection", f.bot->currentTarget().id, -1);
    }
    {
        Fixture f;
        f.eval("AI::directiveTargetPoint(Auditor, \"0 20 100\", 100);");
        f.advance(3);
        f.bot->attackPlayer(99999);
        f.advance(1);
        compare("legacy_attack_suppresses_point_target", f.bot->lastGeneratedMove().trigger, 0);
    }
    {
        Fixture f;
        f.eval(R"(
            function AI::onTargetLOSAcquired(%name, %target) { AI::Attack(%name, 99999); }
        )");
        f.targetDirective(100);
        f.advance(5);
        compare("callback_attack_preserves_current_setup_selection",
                f.bot->currentTarget().id,
                f.targetId);
        f.advance(1);
        compare("callback_attack_suppresses_next_setup_selection", f.bot->currentTarget().id, -1);
    }
}

void synthetic() {
    {
        Fixture f;
        f.bot->setVar("iq", "0");
        compare("console_iq_change_defers_recomputation", f.bot->iqVars().maxSpeed, 1, .000001);
        f.advance(1);
        compare("manager_pulse_recomputes_iq", f.bot->iqVars().maxSpeed, .4, .000001);
    }

    {
        Fixture f;
        auto& g = f.app.fearPlugin().serverGlobals();
        auto* armor = dynamic_cast<PlayerData*>(
            g.dbm->lookupDataBlock("AuditArmor", DataBlockManager::PlayerDataType));
        require(armor != nullptr, "walking armor");
        armor->maxForwardSpeed = armor->maxBackwardSpeed = armor->maxSideSpeed = 7;
        armor->groundForce = 675;
        armor->groundTraction = 3;
        const std::array<Flt32, 4> heights{};
        const std::array<world::TerrainCollisionCell, 1> cells{};
        auto floor = world::TerrainCollision::prepare(1, 1, 256.f, heights, cells);
        require(floor.has_value(), "walking floor");
        require(g.manager->addObject(std::make_unique<world::SimWorldCollider>(
                    world::SimTerrainCollisionImage(
                        std::make_shared<const world::TerrainCollision>(std::move(*floor))),
                    TMat3F(EulerF{}, Point3F{-100, -100, 100}))) != nullptr,
                "floor collider");
        f.bot->player()->setPos({0, 0, 99.99f});
        f.bot->player()->setLinearVelocity({0, 0, -1});
        f.bot->player()->setRotation(static_cast<const RMat3F&>(f.bot->player()->getTransform()),
                                     true);
        compare("forced_rotation_rechecks_ground_contact", f.bot->player()->didContact(), 1);
        f.bot->player()->setPos({0, 0, 100});
        f.bot->player()->setLinearVelocity({});
        f.bot->player()->setForceMask(~UInt32{0});
        f.bot->player()->addForce(sim::MovementGravityForce, {0, 0, -20});
        f.bot->setAutomaticTargets(true);
        f.bot->setAttackMode(0);
        f.bot->setSeekOff({});
        f.bot->setVar("spotDist", "3");
        f.eval(std::format("AI::directiveFollow(Auditor, {}, 0, 99);", f.targetId));
        bool fired = false;
        Flt32 nearest = 20;
        for (int tick = 0; tick < 240; ++tick) {
            f.advance(1);
            fired |= f.bot->lastGeneratedMove().trigger;
            nearest = std::min(
                nearest, (f.bot->player()->getLeadCenter() - f.target->getLeadCenter()).len());
        }
        compare("grounded_follow_closes_to_melee_range", nearest < 3, 1);
        compare("grounded_follow_acquires_and_fires", fired, 1);
    }

    {
        Fixture f;
        f.eval(R"(
            function AI::onTargetLOSAcquired(%name, %target) {
                $Acquired++; AI::directiveRemove(%name, "*");
            }
        )");
        f.targetDirective(100);
        f.advance(5);
        compare("callback_removes_current_target", f.bot->currentTarget().id, -1);
        compare("callback_removes_directives", double(f.bot->directives().size()), 0);
    }
    {
        Fixture f;
        f.eval(R"(
            function AI::onTargetLOSAcquired(%name, %target) {
                deleteObject(AI::getObject(%name));
            }
        )");
        f.targetDirective(100);
        f.advance(5);
        compare("callback_deletes_own_ai",
                f.app.fearPlugin().serverGlobals().aiManager->findAI("Auditor") != nullptr,
                0);
    }
    {
        Fixture f;
        f.eval(R"(
            AI::directiveWaypoint(Auditor, "0 0 100", 100);
            AI::directiveWaypoint(Auditor, "0 40 100", 200);
            function Reached(%name) { AI::directiveRemove(%name, 200); }
            AI::directiveCallback1(Auditor, Reached, 100, 1);
        )");
        f.advance(1);
        compare("waypoint_callback_removes_next_before_selection", f.bot->curDest().y, 0);
    }
    {
        Fixture f;
        f.targetDirective(300);
        f.advance(5);
        f.app.console().setVariable("AuditTargetRep", std::to_string(f.targetId));
        f.eval(R"(
            function ReplaceAtWaypoint(%name) {
                deleteObject(Client::getOwnedObject($AuditTargetRep));
                newObject(UnownedReplacement, Player, AuditArmor);
                newObject(OwnedReplacement, Player, AuditArmor);
                GameBase::setPosition(OwnedReplacement, "0 20 100");
                Client::setOwnedObject($AuditTargetRep, OwnedReplacement);
            }
            AI::directiveWaypoint(Auditor, "0 0 100", 100);
            AI::directiveCallback1(Auditor, ReplaceAtWaypoint, 100, 1);
        )");
        f.advance(1);
        auto* replacement =
            f.app.fearPlugin().serverGlobals().manager->findObject("OwnedReplacement");
        require(replacement != nullptr, "replacement body");
        compare("waypoint_callback_refreshes_owned_target",
                f.bot->currentTarget().obj == replacement,
                1);
    }
    {
        Fixture f;
        f.eval(R"(
            BulletData AuditBullet { muzzleVelocity=100; totalTime=2; liveTime=2; };
            ItemImageData AuditGunImage { weaponType=0; projectileType=AuditBullet; fireTime=0.1; };
            ItemData AuditGun { imageType=AuditGunImage; };
        )");
        f.eval(std::format(
            "Player::setItemCount({0}, AuditGun, 1); Player::mountItem({0}, AuditGun, 0);",
            f.bot->id));
        require(f.bot->getProjectileSpeed() > 0, "laser fixture weapon");
        compare("mounted_weapon_projectile_speed", f.bot->getProjectileSpeed(), 100);
        f.bot->setAttackMode(0);
        f.eval(std::format(
            "AI::directiveFollow(Auditor, {}, 0, 99); AI::directiveTargetLaser(Auditor, {}, 200);",
            f.targetId,
            f.targetId));
        auto& g = f.app.fearPlugin().serverGlobals();
        auto laser = std::make_unique<TargetLaser>();
        auto* laserData = dynamic_cast<TargetLaserData*>(
            g.dbm->createDataBlock("AuditBeam", DataBlockManager::TargetLaserDataType));
        require(laserData && laser->initResources(laserData), "laser data");
        laser->initProjectile(f.target->getTransform(), {}, f.target->getId());
        auto* beam = dynamic_cast<TargetLaser*>(g.manager->addObject(std::move(laser)));
        require(beam != nullptr, "target laser");
        beam->setBeamState(f.target->getLeadCenter(), {0, 40, 101}, true, 0);
        f.advance(10);
        const auto& aiming = f.bot->lastGeneratedMove();
        compare("laser_aiming_stops_route_movement",
                aiming.forwardAction + aiming.backwardAction + aiming.leftAction +
                    aiming.rightAction,
                0);
        beam->setBeamState({}, {}, false, 0);
        f.advance(1);
        const auto& walking = f.bot->lastGeneratedMove();
        compare("missing_laser_resumes_route",
                walking.forwardAction + walking.backwardAction + walking.leftAction +
                        walking.rightAction >
                    0,
                1);
        compare("missing_laser_turns_to_route", std::abs(walking.turnRot) > 0, 1);
    }
    {
        Fixture f;
        f.bot->setAttackMode(0);
        f.bot->setSeekOff({});
        f.eval(std::format("AI::directiveFollow(Auditor, {}, 0, 99);", f.targetId));
        f.targetDirective(100);
        f.advance(10);
        // Native aiming dope can add a small turn to the movement compensation.
        compare("attack_mode0_follows_route", f.bot->lastGeneratedMove().forwardAction, 1, .001);
        f.bot->setAttackMode(2);
        f.advance(1);
        const auto still = f.bot->lastGeneratedMove();
        compare("attack_mode2_stands_still",
                still.forwardAction + still.backwardAction + still.leftAction + still.rightAction,
                0);
        f.bot->setAttackMode(1);
        f.target->setPos({0, 10, 100});
        f.advance(1);
        // Aiming dope can add a small turn. Do not depend on shared RNG state
        // to produce an exactly axial retreat.
        compare("attack_mode1_keeps_17_units", f.bot->lastGeneratedMove().backwardAction, 1, .001);
    }
    {
        Fixture f;
        f.targetDirective(100);
        f.advance(5);
        compare("initial_acquisition", f.acquired(), 1);
        f.eval("$Acquired=0; AI::directiveRemove(Auditor, 100);");
        f.targetDirective(100);
        f.advance(5);
        compare("remove_readd_acquisition", f.acquired(), 1);
    }
    {
        Fixture f;
        f.targetDirective(100);
        f.advance(5);
        f.eval("$Acquired=0;");
        f.targetDirective(100);
        f.advance(5);
        compare("replace_directive_acquisition", f.acquired(), 1);
    }
    {
        Fixture f;
        f.targetDirective(100);
        for (int tick = 0; tick < 20; ++tick) {
            f.bot->setVar("spotDist", "50");
            f.advance(1);
        }
        compare("repeated_spot_distance_allows_acquisition", f.acquired(), 1);
    }
    {
        Fixture f;
        f.targetDirective(100);
        f.targetDirective(200);
        f.advance(5);
        compare("duplicate_callbacks_tick5", f.acquired(), 1);
        f.advance(5);
        compare("duplicate_callbacks_tick10", f.acquired(), 2);
    }
    {
        Fixture f;
        f.eval("AI::directiveTargetPoint(Auditor, \"0 20 100\", 100);");
        f.targetDirective(200);
        f.advance(5);
        compare("point_consumes_first_los_slice", f.acquired(), 0);
    }
    {
        Fixture f;
        f.targetDirective(100);
        f.eval(std::format("AI::directiveTargetLaser(Auditor, {}, 200);", f.targetId));
        f.advance(5);
        compare("player_los_callback_excludes_laser_directive", f.acquired(), 1);
    }
    {
        Fixture f;
        f.eval("AI::spawn(Second, AuditArmor, \"30 0 100\");");
        auto* second = f.app.fearPlugin().serverGlobals().aiManager->findAI("Second");
        require(second != nullptr, "second bot");
        second->player()->setTeam(1);
        f.bot->setAutomaticTargets(true);
        second->setAutomaticTargets(true);
        f.advance(1);
        const int enrolled = int(!f.bot->listDirectives(AIObj::TargetDirective).empty()) +
                             int(!second->listDirectives(AIObj::TargetDirective).empty());
        compare("automatic_enrollment_bots_per_tick", enrolled, 1);
    }
    {
        Fixture f;
        PlayerMove move;
        move.reset();
        const auto point = f.bot->player()->getBoxCenter() + Point3F{0, 20, 5};
        f.bot->fireAtPointTarget(&move, point);
        compare("pitch_step_iq100", move.pitch, .084, .000001);
        f.bot->setIq(0);
        f.bot->fireAtPointTarget(&move, point);
        compare("pitch_step_iq0", move.pitch, .084, .000001);
    }
    {
        Fixture f;
        PlayerMove move;
        move.reset();
        move.turnRot = .25f;
        f.bot->player()->serverUpdateMove(std::array<PlayerMove, 1>{move});
        compare("first_ai_move_yaw", f.bot->player()->getRot().z, 0, .000001);
    }
    {
        Fixture f;
        f.bot->setAttackMode(0);
        f.eval(R"(
            function AI::onTargetLOSAcquired(%name, %target) {
                $Acquired++; AI::directiveFollow(%name, %target, 0, 99);
            }
        )");
        f.targetDirective(100);
        f.advance(5);
        compare("acquisition_follow_destination_same_tick",
                f.bot->curDest().y,
                f.target->getBoxCenter().y + f.bot->seekOff().y);
    }
    {
        Fixture f;
        f.bot->setAttackMode(0);
        f.eval(std::format(R"(
            function AI::onTargetLOSAcquired(%name, %target) {{
                $Acquired++; AI::directiveFollow(%name, %target, 0, 99);
            }}
            AI::directiveWaypoint(Auditor, "20 0 100", 200);
            AI::directiveTarget(Auditor, {}, 300);
        )",
                           f.targetId));
        f.advance(5);
        // A lower order follow doesn't preempt the active native waypoint.
        compare("follow_does_not_preempt_waypoint", f.bot->curDest().x, 20);
    }
    {
        Fixture f;
        f.targetDirective(100);
        f.advance(5);
        f.target->setPos({0, 200, 100});
        f.advance(5);
        f.target->setPos({0, 20, 100});
        f.advance(5);
        compare("out_of_range_forgets_los", f.acquired(), 2);
    }
    {
        Fixture f;
        f.bot->setAttackMode(0);
        auto& g = f.app.fearPlugin().serverGlobals();
        auto* mount = dynamic_cast<GameBase*>(g.manager->addObject(std::make_unique<GameBase>()));
        require(mount != nullptr, "mount");
        mount->setPosition(TMat3F(EulerF{}, Point3F{50, 50, 100}), true);
        mount->setLinearVelocity({10, 0, 0});
        f.target->setMountObject(mount, 0);
        f.eval(std::format("AI::directiveFollow(Auditor, {}, 0, 99);", f.targetId));
        f.advance(1);
        compare("mounted_target_velocity_x", f.target->getLeadVelocity().x, 10);
        compare("mounted_follow_center_error",
                (f.bot->curDest() - mount->getLinearPosition() - f.bot->seekOff()).len(),
                0,
                .000001);
    }
}

void mayorTrace(const std::filesystem::path& assets, int ticks, bool loadPatrol) {
    FearGameApp::Config config;
    config.assetRoot = assets;
    config.writeRoot = std::filesystem::temp_directory_path() /
                       std::format("opent1-mayor-audit-{}",
                                   std::chrono::steady_clock::now().time_since_epoch().count());

    struct Cleanup {
        std::filesystem::path path;

        ~Cleanup() {
            std::error_code error;
            std::filesystem::remove_all(path, error);
        }
    } cleanup{config.writeRoot};

    FearGameApp app(config);
    const std::array<std::string_view, 2> args{"-mod", "RPG"};
    require(app.init(args), "mayor init");
    app.console().setVariable("PCFG::Name", "MayorAudit");
    app.console().setVariable("PCFG::Script", "");
    app.console().setVariable("PCFG::Info", "mayor-audit-password");
    require(app.loadMission("rpgmap5"), "mayor map");
    for (int tick = 0; tick < 8; ++tick)
        require(app.stepFrame(tick * .032, false), "mayor initial frame");
    require(app.fearPlugin().focusServer(), "mayor server focus");
    const auto localId = Int32(app.fearPlugin().clientGlobals().manager->getId());
    require(app.console()
                .evaluate(std::format("processMenupickgroup({0}, 3); processMenupickclass({0}, 1);",
                                      localId))
                .ok(),
            "mayor observer character");
    auto& g = app.fearPlugin().serverGlobals();
    auto* observer = dynamic_cast<Player*>(g.playerManager->findClient(localId)->ownedObject);
    require(observer != nullptr, "mayor observer");
    observer->setPos({-2340, -280, 65});
    std::printf("MAYOR trace started bots=%zu mayorFunction=%d running=%s\n",
                g.aiManager->aiList().size(),
                int(app.console().isFunction("Taurik::Mayor")),
                app.console().getVariable("TaurikMayorRunning").c_str());
    if (loadPatrol)
        require(app.console().evaluate("exec(Taurik_Mayor);").ok(), "load installed mayor patrol");
    Int32 mayorId = -1;
    Flt32 furthest = 0, lowest = std::numeric_limits<Flt32>::max();
    for (int tick = 0; tick <= ticks; ++tick) {
        require(app.stepFrame((tick + 8) * .032, false), "mayor frame");
        if (tick % 256 == 0)
            std::printf("MAYOR progress tick=%d time=%u bots=%zu\n",
                        tick,
                        g.currentTime,
                        g.aiManager->aiList().size());
        for (const auto& ai : g.aiManager->aiList()) {
            if (ai->name.find("Mayor") != std::string::npos ||
                ai->name.find("mayor") != std::string::npos) {
                mayorId = ai->id;
                if (ai->player()) {
                    const auto p = ai->player()->getPos();
                    furthest = std::max(furthest, std::hypot(p.x + 2343.f, p.y + 308.f));
                    lowest = std::min(lowest, p.z);
                }
                if (tick % 32 == 0 && ai->player()) {
                    const auto p = ai->player()->getPos();
                    const auto d = ai->curDest();
                    const auto v = ai->player()->getLinearVelocity();
                    const auto scriptDestination =
                        app.console()
                            .evaluate(std::format("return fetchData({}, tmpbotdata);", ai->id))
                            .value;
                    const auto scriptMode =
                        app.console()
                            .evaluate(std::format("return fetchData({}, botAttackMode);", ai->id))
                            .value;
                    std::printf("MAYOR tick=%d name=%s id=%d pos=(%.3f,%.3f,%.3f) "
                                "dest=(%.3f,%.3f,%.3f) vel=(%.3f,%.3f,%.3f) "
                                "yaw=%.3f contact=%d waypoints=%zu attackMode=%d botMode=%s "
                                "step=%s tmp=%s\n",
                                tick,
                                ai->name.c_str(),
                                ai->id,
                                p.x,
                                p.y,
                                p.z,
                                d.x,
                                d.y,
                                d.z,
                                v.x,
                                v.y,
                                v.z,
                                ai->player()->getRot().z,
                                int(ai->player()->didContact()),
                                ai->listDirectives(AIObj::WaypointDirective).size(),
                                ai->attackMode(),
                                scriptMode.c_str(),
                                app.console().getVariable("Mayor_PathStep").c_str(),
                                scriptDestination.c_str());
                }
            }
        }
    }
    require(mayorId >= 0, "No Mayor spawned");
    std::printf("MAYOR result patrolLoaded=%d running=%s furthest=%.3f lowestZ=%.3f\n",
                int(app.console().isFunction("Taurik::Mayor")),
                app.console().getVariable("TaurikMayorRunning").c_str(),
                furthest,
                lowest);
}

void installed(const std::filesystem::path& assets, int ticks) {
    FearGameApp::Config config;
    config.assetRoot = assets;
    config.writeRoot = std::filesystem::temp_directory_path() /
                       std::format("opent1-ai-audit-{}",
                                   std::chrono::steady_clock::now().time_since_epoch().count());

    struct Cleanup {
        std::filesystem::path path;

        ~Cleanup() {
            std::error_code error;
            std::filesystem::remove_all(path, error);
        }
    } cleanup{config.writeRoot};

    FearGameApp app(config);
    const std::array<std::string_view, 2> args{"-mod", "RPG"};
    require(app.init(args), "installed init");
    auto& console = app.console();
    console.setVariable("PCFG::Name", "AuditPlayer");
    console.setVariable("PCFG::Script", "");
    console.setVariable("PCFG::Info", "audit-local-password");
    require(app.loadMission("rpgmap1"), "rpgmap1");
    // The installed Wasteland cage has map5-only bounds. Drain its scheduled
    // pass before spawning a player in this map1 crypt hunt fixture.
    console.setVariable("Server::bPlayerCageEnabled", "false");
    for (int tick = 0; tick < 8; ++tick)
        require(app.stepFrame(tick * .032), "initial frame");
    require(app.fearPlugin().focusServer(), "server focus");
    const auto localId = Int32(app.fearPlugin().clientGlobals().manager->getId());
    require(console
                .evaluate(std::format("processMenupickgroup({0}, 3); processMenupickclass({0}, 1);",
                                      localId))
                .ok(),
            "character");
    auto& g = app.fearPlugin().serverGlobals();
    auto* player = dynamic_cast<Player*>(g.playerManager->findClient(localId)->ownedObject);
    require(player != nullptr, "installed player");
    // Original scripts choose weapon, attack mode, spotting and hunting behavior.
    player->setPos({2248, 755, 200});
    require(console
                .evaluate(R"(
        SpawnAI(Raider998, AuditEnemy, "2248 775 200", "TempSpawn 2248 775 200 1");
        $AIsmartFOVbots = false;
    )")
                .ok(),
            "installed spawn");
    auto* bot = g.aiManager->findAI("Raider998");
    require(bot && bot->player(), "installed bot");
    const auto botId = bot->id;
    std::vector<std::string> others;
    for (const auto& ai : g.aiManager->aiList())
        if (ai.get() != bot)
            others.push_back(ai->name);
    for (const auto& name : others)
        g.aiManager->remove(name, true);
    Flt32 nearest = 20;
    bool fired = false;
    const auto initialDamage = player->getDamageLevel();
    for (int tick = 1; tick <= ticks; ++tick) {
        require(app.stepFrame((7 + tick) * .032, false), "installed frame");
        bot = g.aiManager->findByPlayerId(botId);
        if (!bot || !bot->player()) {
            std::printf("RPG tick=%d bot_removed\n", tick);
            break;
        }
        nearest =
            std::min(nearest, (bot->player()->getLeadCenter() - player->getLeadCenter()).len());
        fired |= bot->lastGeneratedMove().trigger;
        if (tick % 32 == 0 || tick == 1) {
            const auto pos = bot->player()->getPos(), targetPos = player->getPos();
            const auto& m = bot->lastGeneratedMove();
            std::printf("RPG tick=%d distance=%.3f bot=(%.3f,%.3f,%.3f) player=(%.3f,%.3f,%.3f) "
                        "target=%d trigger=%d movement=%.3f spot=%s mode=%d follows=%zu "
                        "damage=%.6f dead=%d\n",
                        tick,
                        (pos - targetPos).len(),
                        pos.x,
                        pos.y,
                        pos.z,
                        targetPos.x,
                        targetPos.y,
                        targetPos.z,
                        bot->attackedTargetId(),
                        int(m.trigger),
                        m.forwardAction + m.backwardAction + m.leftAction + m.rightAction,
                        bot->getVar("spotDist").c_str(),
                        bot->attackMode(),
                        bot->listDirectives(AIObj::FollowDirective).size(),
                        player->getDamageLevel(),
                        int(player->isDead()));
            if (tick == 192 || tick == 640) {
                const auto v = bot->player()->getLinearVelocity();
                const auto* data = dynamic_cast<const PlayerData*>(bot->player()->getDatPtr());
                require(data != nullptr, "installed armor");
                const auto* group = bot->player()->getGroup();
                std::printf(
                    "PROCESS serverTime=%u playerTime=%u aiControlled=%d group=%s debt=%d "
                    "gravity=%.6f\n",
                    g.currentTime,
                    bot->player()->getLastProcessTime(),
                    int(bot->player()->isAiControlled()),
                    group ? std::string(group->getName()).c_str() : "<none>",
                    bot->player()->updateDebt(),
                    bot->player()->getForce(sim::MovementGravityForce).value_or(Point3F{}).z);
                std::printf("PHYSICS velocity=(%.6f,%.6f,%.6f) contact=%d contactAge=%d "
                            "groundForce=%.3f traction=%.3f maxForward=%.3f mass=%.3f\n",
                            v.x,
                            v.y,
                            v.z,
                            int(bot->player()->didContact()),
                            bot->player()->lastContactCount(),
                            data->groundForce,
                            data->groundTraction,
                            data->maxForwardSpeed,
                            bot->player()->getMass());
                sim::SimMovement::MovementInfo collision;
                auto proposed = bot->player()->getTransform();
                proposed.p.y -= .1f;
                (void)bot->player()->testPosition(proposed, collision);
                for (const auto& hit : collision.collisionList) {
                    auto* object = hit.object.resolve(*g.manager);
                    std::printf("COLLISION object=%d name=%s type=%u contained=%d surfaces=%zu\n",
                                object ? object->getId() : 0,
                                object ? std::string(object->getName()).c_str() : "",
                                object ? object->getType() : 0,
                                int(hit.surfaces.contained),
                                hit.surfaces.surfaces.size());
                    for (std::size_t i = 0;
                         i < std::min(std::size_t{4}, hit.surfaces.surfaces.size());
                         ++i) {
                        const auto& surface = hit.surfaces.surfaces[i];
                        Point3F normal;
                        m_mul(surface.normal,
                              static_cast<const RMat3F&>(hit.surfaces.tWorld),
                              normal);
                        std::printf("SURFACE normal=(%.6f,%.6f,%.6f) distance=%.6f\n",
                                    normal.x,
                                    normal.y,
                                    normal.z,
                                    surface.distance);
                    }
                }
            }
        }
        if (player->isDead())
            break;
    }
    if (ticks >= 1000) {
        compare("installed_hunt_enters_attack_range", nearest < 5, 1);
        compare("installed_hunt_fires", fired, 1);
        compare("installed_hunt_damages_player",
                player->getDamageLevel() > initialDamage || player->isDead(),
                1);
    }
}
}

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    try {
        if (argc >= 3 && std::string_view(argv[1]) == "--mayor") {
            if (argc > 5 || (argc == 5 && std::string_view(argv[4]) != "patrol"))
                throw std::runtime_error(
                    "Usage: opent1_ai_behavior_audit --mayor assets [ticks [patrol]]");
            mayorTrace(argv[2],
                       argc >= 4 ? std::stoi(argv[3]) : 4000,
                       argc >= 5 && std::string_view(argv[4]) == "patrol");
            return 0;
        }
        waypointSynthetic();
        synthetic();
        if (argc == 2 || argc == 3)
            installed(argv[1], argc == 3 ? std::stoi(argv[2]) : 1250);
        else if (argc != 1)
            throw std::runtime_error("Usage: opent1_ai_behavior_audit [PCRPG asset root [ticks]]");
        std::printf("%d comparisons, %d differences (diagnostic, not a parity certification)\n",
                    comparisons,
                    differences);
        return differences ? 1 : 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "Audit setup failed: %s\n", error.what());
        return 2;
    }
}
