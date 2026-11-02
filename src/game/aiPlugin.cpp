#include <opent1/game/ai.hpp>
#include <opent1/game/string_table.hpp>

#include <charconv>
#include <format>
#include <sstream>

namespace opent1::game {

namespace {

enum AICommandId : Int32 {
    CallWithId = 102,
    SetScriptedTargets = 103,
    AttackPlayer = 104,
    GetId = 101,
    SpawnAtMarker = 100,
    SpawnAI = 1,
    DeleteAI = 2,
    DirectiveWaypoint = 3,
    DirectiveFollow = 4,
    DirectiveGuard = 5,
    DirectiveTarget = 6,
    DirectiveTargetLaser = 7,
    DirectiveTargetPoint = 8,
    DirectiveRemove = 9,
    DirectiveList = 10,
    CallbackPeriodic = 11,
    CallbackDied = 12,
    CallbackBored = 13,
    SetVar = 14,
    GetVar = 15,
    ListAI = 16,
    SetAutoTargets = 17,
    SelectMovement = 18,
    Action = 19,
    GetTarget = 20,
    GetCount = 21,
    GetObject = 22,
    BuildGraph = 23,
    GraphAddNode,
    GraphNodeCount,
    GraphLoadNode,
    GraphPrintNode,
    DirectiveCallback1,
    DirectiveCallback2,
    DirectiveCallback3,
    DirectiveCallback4,
};

[[nodiscard]] Int32 parseIntArg(std::string_view sv) noexcept {
    Int32 val = 0;
    std::from_chars(sv.data(), sv.data() + sv.size(), val);
    return val;
}

[[nodiscard]] Flt32 parseFloatArg(std::string_view sv) noexcept {
    Flt32 val = 0.0f;
    std::from_chars(sv.data(), sv.data() + sv.size(), val);
    return val;
}

[[nodiscard]] Point3F parsePoint3FArg(std::string_view sv) noexcept {
    Point3F out{0.0f, 0.0f, 0.0f};
    std::size_t idx = 0;
    for (std::size_t comp = 0; comp < 3 && idx < sv.size(); ++comp) {
        while (idx < sv.size() && (sv[idx] == ' ' || sv[idx] == '\t')) {
            ++idx;
        }
        if (idx >= sv.size()) {
            break;
        }
        std::size_t end = idx;
        while (end < sv.size() && sv[end] != ' ' && sv[end] != '\t') {
            ++end;
        }
        out[comp] = parseFloatArg(sv.substr(idx, end - idx));
        idx = end;
    }
    return out;
}

void parsePointAndRadius(AIObj::Directive& directive, std::string_view value) {
    std::istringstream input{std::string(value)};
    input >> directive.pos.x >> directive.pos.y >> directive.pos.z >> directive.rad;
}

} // namespace

void AIPlugin::init(Console& console, sim::SimManager* manager) {
    console_ = &console;
    manager_ = manager;

    auto bindCmd = [this, &console](Int32 id, std::string_view name) {
        console.addCommand(
            id, name, [this](Console& c, Int32 cmdId, std::span<const std::string> argv) {
                return handleCommand(c, cmdId, argv);
            });
    };

    bindCmd(SpawnAI, "AI::Spawn");
    bindCmd(SpawnAtMarker, "AI::SpawnAtMarker");
    bindCmd(DeleteAI, "AI::Delete");
    bindCmd(DirectiveWaypoint, "AI::DirectiveWaypoint");
    bindCmd(DirectiveFollow, "AI::DirectiveFollow");
    bindCmd(DirectiveGuard, "AI::DirectiveGuard");
    bindCmd(DirectiveTarget, "AI::DirectiveTarget");
    bindCmd(DirectiveTargetLaser, "AI::DirectiveTargetLaser");
    bindCmd(DirectiveTargetPoint, "AI::DirectiveTargetPoint");
    bindCmd(DirectiveRemove, "AI::DirectiveRemove");
    bindCmd(DirectiveList, "AI::DirectiveList");
    bindCmd(CallbackPeriodic, "AI::CallbackPeriodic");
    bindCmd(CallbackDied, "AI::CallbackDied");
    bindCmd(CallbackBored, "AI::CallbackBored");
    bindCmd(SetVar, "AI::SetVar");
    bindCmd(GetVar, "AI::GetVar");
    bindCmd(ListAI, "AI::List");
    bindCmd(SetAutoTargets, "AI::SetAutomaticTargets");
    bindCmd(SelectMovement, "AI::SelectMovement");
    bindCmd(Action, "AI::Action");
    bindCmd(AttackPlayer, "AI::Attack");
    bindCmd(GetTarget, "AI::GetTarget");
    bindCmd(GetCount, "AI::GetCount");
    bindCmd(GetObject, "AI::GetObject");
    bindCmd(GetId, "AI::GetId");
    bindCmd(CallWithId, "AI::CallWithId");
    bindCmd(SetScriptedTargets, "AI::SetScriptedTargets");
    bindCmd(BuildGraph, "AI::BuildGraph");
    bindCmd(GraphAddNode, "Graph::AddNode");
    bindCmd(GraphNodeCount, "Graph::NodeCount");
    bindCmd(GraphLoadNode, "Graph::LoadNode");
    bindCmd(GraphPrintNode, "Graph::PrintNode");
    bindCmd(DirectiveCallback1, "AI::DirectiveCallback1");
    bindCmd(DirectiveCallback2, "AI::DirectiveCallback2");
    bindCmd(DirectiveCallback3, "AI::DirectiveCallback3");
    bindCmd(DirectiveCallback4, "AI::DirectiveCallback4");
}

void AIPlugin::endFrame() {}

AIManager* AIPlugin::resolveAIManager() const noexcept {
    if (!manager_) {
        return nullptr;
    }
    return dynamic_cast<AIManager*>(manager_->findObject(AIManagerId));
}

std::optional<std::string>
AIPlugin::handleCommand(Console& console, Int32 id, std::span<const std::string> argv) {
    AIManager* aiMgr = resolveAIManager();
    if (!aiMgr && manager_) {
        auto created = std::make_unique<AIManager>();
        aiMgr = created.get();
        if (!manager_->addObject(std::move(created))) {
            return "false";
        }
    }
    if (!aiMgr) {
        return "false";
    }

    switch (id) {
        case DirectiveCallback1:
        case DirectiveCallback2:
        case DirectiveCallback3:
        case DirectiveCallback4: {
            if (argv.size() < 3 || argv.size() > 5) {
                return "false";
            }
            std::vector<AIObj*> list;
            aiMgr->buildExpressionList(list, argv[1]);
            const UInt32 mask = argv.size() == 5 ? UInt32(parseIntArg(argv[4])) : ~UInt32{0};
            for (auto* ai : list) {
                ai->installDirectiveCallback(id - DirectiveCallback1 + 1,
                                             argv[2],
                                             argv.size() >= 4 ? std::string_view(argv[3])
                                                              : std::string_view{},
                                             mask);
            }
            return list.empty() ? "false" : "true";
        }
        case SpawnAI: {
            if (argv.size() < 4)
                return "false";
            return aiMgr->spawn(
                       console,
                       argv[1],
                       argv[2],
                       parsePoint3FArg(argv[3]),
                       argv.size() > 4 ? parsePoint3FArg(argv[4]) : Point3F{},
                       argv.size() > 5 ? std::string_view(argv[5]) : std::string_view(argv[1]),
                       argv.size() > 6 ? std::string_view(argv[6]) : std::string_view("male1"))
                       ? "true"
                       : "false";
        }
        case SpawnAtMarker: {
            if (argv.size() < 2) {
                return "false";
            }
            const std::string_view aiName = argv[1];
            const std::string_view markerName =
                (argv.size() >= 3) ? std::string_view(argv[2]) : std::string_view{};
            const Int32 teamId = (argv.size() >= 4) ? parseIntArg(argv[3]) : 0;
            const std::string_view skinBase =
                (argv.size() >= 5) ? std::string_view(argv[4]) : std::string_view("base");
            return aiMgr->add(aiName, markerName, teamId, skinBase) ? "true" : "false";
        }

        case DeleteAI: {
            if (argv.size() < 2) {
                return "false";
            }
            std::vector<AIObj*> matches;
            aiMgr->buildExpressionList(matches, argv[1]);
            for (auto* ai : matches) {
                if (auto* player = ai->player()) {
                    player->deleteObject();
                    ai->setPlayer(nullptr);
                }
            }
            return matches.empty() ? "false" : "true";
        }

        case DirectiveWaypoint: {
            if (argv.size() < 3) {
                return "false";
            }
            std::vector<AIObj*> list{};
            aiMgr->buildExpressionList(list, argv[1]);
            AIObj::Directive dir{};
            dir.orderMask = AIObj::WaypointDirective;
            parsePointAndRadius(dir, argv[2]);
            dir.pts.push_back(dir.pos);
            dir.order = (argv.size() >= 4) ? parseIntArg(argv[3]) : -1;

            for (AIObj* ai : list) {
                ai->addDirective(dir, true);
            }
            return list.empty() ? "false" : "true";
        }

        case DirectiveFollow: {
            if (argv.size() < 3) {
                return "false";
            }
            std::vector<AIObj*> list{};
            aiMgr->buildExpressionList(list, argv[1]);
            AIObj::Directive dir{};
            dir.orderMask = AIObj::FollowDirective;
            dir.playerId = parseIntArg(argv[2]);
            dir.followTarget.id = dir.playerId;
            if (argv.size() >= 4) {
                parsePointAndRadius(dir, argv[3]);
            }
            dir.order = (argv.size() >= 5) ? parseIntArg(argv[4]) : -1;
            if (argv.size() >= 5 && dir.order < 0) {
                return "false";
            }
            const auto* players = PlayerManager::get(manager_);
            const auto* rep = players ? players->findBaseRep(dir.playerId) : nullptr;
            if (!rep || !dynamic_cast<Player*>(rep->ownedObject)) {
                return "false";
            }
            for (AIObj* ai : list) {
                ai->addDirective(dir, true);
            }
            return list.empty() ? "false" : "true";
        }

        case DirectiveGuard:
            return "false"; // native command is explicitly unimplemented

        case DirectiveTarget: {
            if (argv.size() < 3) {
                return "false";
            }
            std::vector<AIObj*> list{};
            aiMgr->buildExpressionList(list, argv[1]);
            AIObj::Directive dir{};
            dir.orderMask = AIObj::TargetDirective;
            dir.targetMode = AIObj::PlayerTarget;
            dir.targetPlayerId = parseIntArg(argv[2]);
            dir.activeTarget.id = dir.targetPlayerId;
            dir.order = (argv.size() >= 4) ? parseIntArg(argv[3]) : -1;
            bool added = false;
            for (AIObj* ai : list) {
                if (dir.targetPlayerId != ai->id) {
                    ai->addDirective(dir, true);
                    added = true;
                }
            }
            return added ? "true" : "false";
        }

        case DirectiveTargetLaser: {
            if (argv.size() < 3) {
                return "false";
            }
            std::vector<AIObj*> list{};
            aiMgr->buildExpressionList(list, argv[1]);
            AIObj::Directive dir{};
            dir.orderMask = AIObj::TargetDirective;
            dir.targetMode = AIObj::LaserTarget;
            if (argv.size() >= 5) {
                parsePointAndRadius(dir, argv[4]);
            }
            dir.targetPlayerId = parseIntArg(argv[2]);
            dir.order = (argv.size() >= 4) ? parseIntArg(argv[3]) : -1;
            bool added = false;
            for (AIObj* ai : list) {
                if (dir.targetPlayerId != ai->id) {
                    ai->addDirective(dir, true);
                    added = true;
                }
            }
            return added ? "true" : "false";
        }

        case DirectiveTargetPoint: {
            if (argv.size() < 3) {
                return "false";
            }
            std::vector<AIObj*> list{};
            aiMgr->buildExpressionList(list, argv[1]);
            AIObj::Directive dir{};
            dir.orderMask = AIObj::TargetDirective;
            dir.targetMode = AIObj::PointTarget;
            parsePointAndRadius(dir, argv[2]);
            dir.order = (argv.size() >= 4) ? parseIntArg(argv[3]) : -1;
            for (AIObj* ai : list) {
                ai->addDirective(dir, true);
            }
            return list.empty() ? "false" : "true";
        }

        case DirectiveRemove: {
            if (argv.size() < 3) {
                return "false";
            }
            std::vector<AIObj*> list{};
            aiMgr->buildExpressionList(list, argv[1]);
            Int32 total = 0;
            for (AIObj* ai : list) {
                total += ai->removeDirectives(argv[2]);
            }
            return (total > 0) ? "true" : "false";
        }

        case DirectiveList: {
            if (argv.size() < 2) {
                return "0";
            }
            std::vector<AIObj*> list{};
            aiMgr->buildExpressionList(list, argv[1]);
            const UInt32 mask =
                (argv.size() >= 3) ? static_cast<UInt32>(parseIntArg(argv[2])) : ~0U;
            std::size_t count = 0;
            for (const AIObj* ai : list) {
                count += ai->listDirectives(mask).size();
            }
            return std::format("{}", count);
        }

        case CallbackPeriodic: {
            if (argv.size() < 3) {
                return "false";
            }
            std::vector<AIObj*> list{};
            aiMgr->buildExpressionList(list, argv[1]);
            const Flt32 period = parseFloatArg(argv[2]);
            for (AIObj* ai : list) {
                if (argv.size() > 3)
                    ai->setCallback(AIObj::Periodic, argv[3], period);
                else
                    ai->setPeriodicDelay(period);
            }
            return list.empty() ? "false" : "true";
        }

        case CallbackDied: {
            if (argv.size() < 3) {
                return "false";
            }
            std::vector<AIObj*> list{};
            aiMgr->buildExpressionList(list, argv[1]);
            for (AIObj* ai : list) {
                ai->setCallback(AIObj::AIKilled, argv[2]);
            }
            return list.empty() ? "false" : "true";
        }

        case CallbackBored: {
            if (argv.size() < 3) {
                return "false";
            }
            std::vector<AIObj*> list{};
            aiMgr->buildExpressionList(list, argv[1]);
            for (AIObj* ai : list) {
                ai->setCallback(AIObj::Bored, argv[2]);
            }
            return list.empty() ? "false" : "true";
        }

        case SetVar: {
            if (argv.size() < 3)
                return "false";
            std::vector<AIObj*> list;
            aiMgr->buildExpressionList(list, argv[1]);
            for (auto* ai : list) {
                const auto variable = "AI::" + ai->name + "::" + argv[2];
                if (argv.size() > 3) {
                    if (ai->scriptObjectId)
                        console.setVariable(variable, argv[3]);
                    else if (!ai->setVar(argv[2], argv[3]))
                        console.setVariable(variable, argv[3]);
                } else
                    console.print("Value of " + argv[2] + " for " + ai->name + " = " +
                                  (console.hasVariable(variable) ? console.getVariable(variable)
                                                                 : ai->getVar(argv[2])));
            }
            return list.empty() ? "false" : "true";
        }
        case CallWithId: {
            if (argv.size() < 3 || argv.size() > 102)
                return "false";
            std::vector<AIObj*> matched;
            aiMgr->buildExpressionList(matched, argv[1]);
            std::vector<Int32> ids;
            for (const auto* ai : matched)
                ids.push_back(ai->id);
            for (const auto clientId : ids) {
                std::vector<std::string> args{std::to_string(clientId)};
                args.insert(args.end(), argv.begin() + 3, argv.end());
                (void)console.execute(argv[2], args);
            }
            return ids.empty() ? "false" : "true";
        }

        case GetVar: {
            if (argv.size() < 3) {
                return "";
            }
            std::vector<AIObj*> list{};
            aiMgr->buildExpressionList(list, argv[1]);
            if (list.empty()) {
                return "";
            }
            return list.front()->getVar(argv[2]);
        }

        case ListAI: {
            std::string out{};
            for (const auto& ai : aiMgr->aiList()) {
                if (!out.empty()) {
                    out.push_back(' ');
                }
                out.append(ai->name);
            }
            return out;
        }

        case SetAutoTargets:
        case SetScriptedTargets: {
            if (argv.size() < 2) {
                return "false";
            }
            std::vector<AIObj*> list{};
            aiMgr->buildExpressionList(list, argv[1]);
            for (AIObj* ai : list) {
                ai->setAutomaticTargets(id == SetAutoTargets);
            }
            return list.empty() ? "false" : "true";
        }

        case AttackPlayer: {
            if (argv.size() != 3) {
                return "false";
            }
            std::vector<AIObj*> matches;
            aiMgr->buildExpressionList(matches, argv[1]);
            for (auto* ai : matches) {
                ai->attackPlayer(parseIntArg(argv[2]));
            }
            return matches.empty() ? "false" : "true";
        }
        case SelectMovement:
        case Action:
            return "false";

        case GetTarget: {
            if (argv.size() < 2) {
                return "-1";
            }
            std::vector<AIObj*> list{};
            aiMgr->buildExpressionList(list, argv[1]);
            if (list.empty()) {
                return "-1";
            }
            return std::format("{}", list.front()->attackedTargetId());
        }

        case GetCount:
            return std::format("{}", aiMgr->aiList().size());

        case GetId: {
            const auto* ai = argv.size() == 2 ? aiMgr->findAI(argv[1]) : nullptr;
            return ai ? std::to_string(ai->id) : "-1";
        }
        case GetObject: {
            if (argv.size() < 2) {
                return "-1";
            }
            std::vector<AIObj*> list{};
            aiMgr->buildExpressionList(list, argv[1]);
            if (list.empty()) {
                return "-1";
            }
            return list.front()->scriptObjectId ? std::to_string(list.front()->scriptObjectId)
                                                : "-1";
        }

        case GraphAddNode:
            return argv.size() >= 2
                       ? std::to_string(aiMgr->graph().addNode(
                             parsePoint3FArg(argv[1]), argv.size() > 2 ? parseIntArg(argv[2]) : 4))
                       : "-1";
        case GraphNodeCount:
            return std::to_string(aiMgr->graph().nodes().size());
        case GraphLoadNode:
            return argv.size() == 2 ? std::to_string(aiMgr->graph().loadNode(argv[1])) : "-1";
        case GraphPrintNode:
            if (argv.size() == 2) {
                const auto text = aiMgr->graph().printNode(parseIntArg(argv[1]));
                if (!text.empty()) {
                    console.print(text);
                    return "true";
                }
            }
            return "false";
        case BuildGraph:
            return "true";

        default:
            return "false";
    }
}

} // namespace opent1::game
