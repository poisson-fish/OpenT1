#include <opent1/game/fear_plugin.hpp>
#include <opent1/game/commander_huds.hpp>
#include <opent1/game/fear_player_psc.hpp>
#include <opent1/game/item.hpp>
#include <opent1/game/player_manager.hpp>
#include <opent1/game/sensor_manager.hpp>
#include <opent1/game/sound.hpp>
#include <opent1/game/version.hpp>
#include <opent1/math/random.hpp>
#include <opent1/sim/sim_editor_events.hpp>
#include <opent1/console/eval.hpp>
#include <charconv>
#include <cctype>
#include <cmath>
#include <format>
#include <sstream>

namespace opent1::game {
namespace {
Int32 number(std::string_view text) {
    Int32 value{};
    std::from_chars(text.data(), text.data() + text.size(), value);
    return value;
}

float real(std::string_view text) {
    float value{};
    std::from_chars(text.data(), text.data() + text.size(), value);
    return std::isfinite(value) ? value : 0;
}

Point3F point(std::string_view text) {
    Point3F value{};
    std::istringstream input{std::string(text)};
    input >> value.x >> value.y >> value.z;
    return value;
}

std::string text(const Point3F& value) {
    return std::format("{:g} {:g} {:g}", value.x, value.y, value.z);
}

GameBase* object(WorldGlobals* world, std::string_view name) {
    if (!world || !world->manager) {
        return nullptr;
    }
    if (auto* found = dynamic_cast<GameBase*>(world->manager->findObject(name))) {
        return found;
    }
    const auto* client =
        world->playerManager ? world->playerManager->findClient(number(name)) : nullptr;
    return client ? client->ownedObject : nullptr;
}

bool same(std::string_view a, std::string_view b) {
    return a.size() == b.size() &&
           std::equal(a.begin(), a.end(), b.begin(), [](unsigned char x, unsigned char y) {
               return std::tolower(x) == std::tolower(y);
           });
}
}

void FearPlugin::installLegacyUtilityCommands(Console& console) {
    console.addCommand(
        0, "version", [](Console&, Int32, std::span<const std::string>) -> std::string {
            return std::string(FearVersion);
        });
    console.addCommand(
        0, "timeStamp", [](Console&, Int32, std::span<const std::string>) -> std::string {
            return std::string(BuildTimeStamp) + " " + std::string(BuildDateStamp);
        });
    console.addCommand(
        0, "sprintf", [](Console&, Int32, std::span<const std::string> args) -> std::string {
            if (args.size() < 2 || args.size() > 10) {
                return "false";
            }
            std::string result;
            for (std::size_t i = 0; i < args[1].size(); ++i) {
                if (args[1][i] != '%') {
                    result += args[1][i];
                    continue;
                }
                if (i + 1 == args[1].size() || args[1][i + 1] < '1' || args[1][i + 1] > '9') {
                    return "false";
                }
                const auto index = std::size_t(args[1][++i] - '0' + 1);
                if (index >= args.size()) {
                    return "false";
                }
                result += args[index];
            }
            return result;
        });
    console.addCommand(
        0, "escapeString", [](Console&, Int32, std::span<const std::string> args) -> std::string {
            if (args.size() != 2) {
                return {};
            }
            std::string out;
            constexpr char hex[] = "0123456789ABCDEF";
            for (const unsigned char c : args[1]) {
                if (!c) {
                    break;
                }
                switch (c) {
                    case '"':
                        out += "\\\"";
                        break;
                    case '\\':
                        out += "\\\\";
                        break;
                    case '\r':
                        out += "\\r";
                        break;
                    case '\n':
                        out += "\\n";
                        break;
                    case '\t':
                        out += "\\t";
                        break;
                    default:
                        if (c < 32 || c > 127) {
                            out += "\\x";
                            out += hex[c >> 4];
                            out += hex[c & 15];
                        } else {
                            out += char(c);
                        }
                }
            }
            return out;
        });
    for (const auto name : {"Vector::dot",
                            "Vector::add",
                            "Vector::sub",
                            "Vector::neg",
                            "Vector::getDistance",
                            "Vector::getFromRot",
                            "Vector::getRotation"}) {
        console.addCommand(
            0, name, [name](Console&, Int32, std::span<const std::string> args) -> std::string {
                const std::string_view op = name;
                const bool unary = op == "Vector::neg" || op == "Vector::getRotation",
                           rotation = op == "Vector::getFromRot";
                const bool scalar = op == "Vector::dot" || op == "Vector::getDistance";
                if ((rotation && (args.size() < 2 || args.size() > 4)) ||
                    (!rotation && args.size() != (unary ? 2U : 3U))) {
                    return scalar ? "0" : "0 0 0";
                }
                const auto a = point(args[1]);
                const auto b = args.size() > 2 ? point(args[2]) : Point3F{};
                if (op == "Vector::dot") {
                    return std::format("{:g}", m_dot(a, b));
                }
                if (op == "Vector::getDistance") {
                    return std::format("{:g}", (a - b).len());
                }
                if (op == "Vector::add") {
                    return text(a + b);
                }
                if (op == "Vector::sub") {
                    return text(a - b);
                }
                if (op == "Vector::neg") {
                    return text(-a);
                }
                if (rotation) {
                    RMat3F matrix;
                    matrix.set(EulerF(a.x, a.y, a.z));
                    Point3F result;
                    m_mul(Point3F(0,
                                  args.size() > 2 ? real(args[2]) : 1,
                                  args.size() > 3 ? real(args[3]) : 0),
                          matrix,
                          result);
                    return text(result);
                }
                Point3F rotationValue{};
                if (a.y + a.x == 0) {
                    rotationValue.x = a.z >= 0 ? 0.f : kRealFPi;
                } else {
                    rotationValue.z = float(-std::atan2(double(a.x), double(a.y)));
                    rotationValue.x = float(
                        -std::atan2(std::sqrt(double(a.x) * a.x + double(a.y) * a.y), double(a.z)));
                }
                return text(rotationValue);
            });
    }
    console.addCommand(
        0, "pow", [](Console&, Int32, std::span<const std::string> args) -> std::string {
            return args.size() == 3
                       ? std::format("{:g}", float(std::pow(real(args[1]), real(args[2]))))
                       : "-1";
        });
    console.addCommand(0,
                       "getRandom",
                       [random = std::make_shared<Random>()](
                           Console&, Int32, std::span<const std::string>) -> std::string {
                           return std::format("{:g}", random->getFloat(0, 1));
                       });
    console.addCommand(0,
                       "WaypointToWorld",
                       [this](Console&, Int32, std::span<const std::string> args) -> std::string {
                           if (args.size() != 2 || !sg_.manager) {
                               return "0 0 0";
                           }
                           const auto* area = dynamic_cast<MissionCenterPos*>(
                               sg_.manager->findObject(MissionCenterPosId));
                           if (!area) {
                               return "0 0 0";
                           }
                           const auto p = point(args[1]);
                           return text({area->pos.x + area->extent.x * p.x / 1024,
                                        area->pos.y + area->extent.y * p.y / 1024,
                                        0});
                       });
    console.addCommand(0,
                       "removeFromSet",
                       [this](Console&, Int32, std::span<const std::string> args) -> std::string {
                           auto* manager = game_.manager();
                           if (args.size() != 3 || !manager) {
                               return "True";
                           }
                           auto* set = dynamic_cast<sim::SimSet*>(manager->findObject(args[1]));
                           if (!set) {
                               return "False";
                           }
                           if (auto* child = manager->findObject(args[2])) {
                               (void)set->removeObject(*child);
                           }
                           return "True";
                       });
    console.addCommand(0,
                       "activateGroup",
                       [this](Console&, Int32, std::span<const std::string> args) -> std::string {
                           auto* manager = game_.manager();
                           auto* selected = manager && (args.size() == 2 || args.size() == 3)
                                                ? manager->findObject(args[1])
                                                : nullptr;
                           if (!selected) {
                               return "False";
                           }
                           auto* group = dynamic_cast<sim::SimGroup*>(selected);
                           if (!group) {
                               group = selected->getGroup();
                           }
                           if (!group) {
                               return "False";
                           }
                           sim::SimTriggerEvent event;
                           event.activate = true;
                           event.value = args.size() == 3 ? real(args[2]) : 0;
                           std::vector<sim::SimAddress> children;
                           for (auto* child : group->objects()) {
                               if (child != selected) {
                                   children.push_back(child->address());
                               }
                           }
                           for (const auto& address : children) {
                               if (auto* child = address.resolve(*manager)) {
                                   (void)child->processEvent(event);
                               }
                           }
                           return "True";
                       });
    for (const auto name : {"GameBase::isPowerGenerator",
                            "GameBase::getPowerCount",
                            "GameBase::getRechargeRate",
                            "GameBase::isAtRest",
                            "GameBase::getTransform",
                            "GameBase::testPosition",
                            "GameBase::setMapName",
                            "GameBase::startFadeIn",
                            "GameBase::setIsTarget",
                            "GameBase::getMuzzleTransform"}) {
        console.addCommand(
            0,
            name,
            [this, name](Console& con, Int32, std::span<const std::string> args) -> std::string {
                const std::string_view op = name;
                const bool parameter = op == "GameBase::testPosition" ||
                                       op == "GameBase::setMapName" ||
                                       op == "GameBase::setIsTarget";
                auto* base = args.size() == (parameter ? 3U : 2U) ? object(wg_, args[1]) : nullptr;
                auto* shape = dynamic_cast<ShapeBase*>(base);
                if (op == "GameBase::getPowerCount") {
                    return std::to_string(base ? base->powerCount() : 0);
                }
                if (op == "GameBase::getRechargeRate") {
                    return std::format("{:g}", shape ? shape->getRechargeRate() : 0);
                }
                if (op == "GameBase::getTransform") {
                    if (!base) {
                        return "0 0 0 0 0 0";
                    }
                    EulerF rotation;
                    base->getTransform().angles(rotation);
                    return text(base->getTransform().p) + " " +
                           text({rotation.x, rotation.y, rotation.z});
                }
                if (op == "GameBase::getMuzzleTransform") {
                    if (!shape) {
                        return "1 0 0 0 1 0 0 0 1 0 0 0";
                    }
                    TMat3F muzzle, total;
                    if (shape->getMuzzleTransform(0, &muzzle)) {
                        m_mul(muzzle, shape->getTransform(), total);
                    } else {
                        total = shape->getEyeTransform();
                    }
                    return std::format(
                        "{:f} {:f} {:f} {:f} {:f} {:f} {:f} {:f} {:f} {:f} {:f} {:f}",
                        total.m[0][0],
                        total.m[0][1],
                        total.m[0][2],
                        total.m[1][0],
                        total.m[1][1],
                        total.m[1][2],
                        total.m[2][0],
                        total.m[2][1],
                        total.m[2][2],
                        total.p.x,
                        total.p.y,
                        total.p.z);
                }
                if (!base) {
                    return "False";
                }
                if (op == "GameBase::isPowerGenerator") {
                    return base->isGenerator() ? "True" : "False";
                }
                if (op == "GameBase::isAtRest") {
                    return base->isAtRest() ? "True" : "False";
                }
                if (op == "GameBase::testPosition") {
                    auto transform = base->getTransform();
                    transform.p = point(args[2]);
                    sim::SimMovement::MovementInfo info;
                    return base->testPosition(transform, info) ? "True" : "False";
                }
                if (op == "GameBase::setMapName" && shape) {
                    shape->setName(args[2], &con);
                    return "True";
                }
                if (op == "GameBase::startFadeIn" && shape) {
                    shape->startFadeIn();
                    return "True";
                }
                if (op == "GameBase::setIsTarget") {
                    if (auto* target = dynamic_cast<StaticBase*>(base)) {
                        const auto enabled = script::toBool(args[2]);
                        target->setIsTarget(enabled);
                        return enabled ? "True" : "False";
                    }
                }
                return "False";
            });
    }
    for (const auto name : {"Item::getItemData",
                            "Item::setVelocity",
                            "Item::getVelocity",
                            "Item::getCount",
                            "Item::isRotating",
                            "Item::hide"}) {
        console.addCommand(
            0,
            name,
            [this, name](Console&, Int32, std::span<const std::string> args) -> std::string {
                const std::string_view op = name;
                const bool parameter = op == "Item::setVelocity" || op == "Item::hide";
                auto* base = args.size() == (parameter ? 3U : 2U) ? object(wg_, args[1]) : nullptr;
                auto* item = dynamic_cast<Item*>(base);
                if (op == "Item::getVelocity") {
                    return base ? text(base->getLinearVelocity()) : "0 0 0";
                }
                if (op == "Item::getItemData") {
                    return item ? std::string(item->getDatFileName()) : "-1";
                }
                if (op == "Item::getCount") {
                    return args.size() != 2 ? "-1" : std::to_string(item ? item->getCount() : 0);
                }
                if (op == "Item::setVelocity") {
                    if (args.size() != 3) {
                        return "false";
                    }
                    if (!base) {
                        return "False";
                    }
                    base->setLinearVelocity(point(args[2]));
                    return "True";
                }
                if (op == "Item::isRotating") {
                    return item && item->isRotating() ? "True" : "False";
                }
                if (op == "Item::hide" && item) {
                    item->hideItem(script::toBool(args[2]));
                    return "True";
                }
                return "False";
            });
    }
    for (const auto name : {"getItemType", "getItemCount", "getMountedItem"}) {
        console.addCommand(
            0,
            name,
            [this, name](Console&, Int32, std::span<const std::string> args) -> std::string {
                const std::string_view op = name;
                if (op == "getMountedItem") {
                    const auto* player =
                        cg_.playerPSC ? dynamic_cast<Player*>(cg_.playerPSC->getControlObject())
                                      : nullptr;
                    return std::to_string(
                        player && args.size() == 2 ? player->getMountedItem(number(args[1])) : -1);
                }
                Int32 index = -1;
                if (args.size() == 2 && cg_.dbm) {
                    for (Int32 i = 0; i < cg_.dbm->getDataGroupSize(DataBlockManager::ItemDataType);
                         ++i) {
                        const auto* data = dynamic_cast<ItemData*>(
                            cg_.dbm->lookupDataBlock(i, DataBlockManager::ItemDataType));
                        if (data && same(data->description, args[1])) {
                            index = i;
                            break;
                        }
                    }
                }
                if (op == "getItemType") {
                    return args.size() != 2 ? "False" : std::to_string(index);
                }
                return std::to_string(cg_.playerPSC && index >= 0 ? cg_.playerPSC->itemCount(index)
                                                                  : 0);
            });
    }
    console.addCommand(0,
                       "getObjectByTargetIndex",
                       [this](Console&, Int32, std::span<const std::string> args) -> std::string {
                           const auto index = args.size() == 2 ? number(args[1]) : -1;
                           const auto* target =
                               sg_.sensorManager && index >= 0 && index < SensorManager::MaxTargets
                                   ? sg_.sensorManager->target(index).obj
                                   : nullptr;
                           return target ? std::to_string(target->getId()) : "-1";
                       });
    console.addCommand(0,
                       "Team::clearObjectives",
                       [this](Console&, Int32, std::span<const std::string> args) -> std::string {
                           if (sg_.playerManager && args.size() == 2) {
                               sg_.playerManager->clearTeamObjectives(number(args[1]));
                           }
                           return "0";
                       });
    for (const auto name : {"playSound", "GameBase::playSound"}) {
        console.addCommand(
            0,
            name,
            [this, name](Console&, Int32, std::span<const std::string> args) -> std::string {
                const bool attached = std::string_view(name) == "GameBase::playSound";
                if (!sg_.manager || !sg_.dbm || args.size() != (attached ? 4U : 3U)) {
                    return "False";
                }
                Int32 index = -1;
                if (!sg_.dbm->lookupDataBlock(
                        args[attached ? 2 : 1], DataBlockManager::SoundDataType, &index)) {
                    return "False";
                }
                if (attached) {
                    auto* target = object(wg_, args[1]);
                    if (!target) {
                        return "False";
                    }
                    TSFX::serverPlaySound(UInt32(index), target, number(args[3]));
                } else {
                    TSFX::serverPlaySound(UInt32(index), point(args[2]), sg_.manager);
                }
                return "True";
            });
    }
    console.addCommand(
        0, "which", [this](Console& con, Int32, std::span<const std::string> args) -> std::string {
            if (args.size() != 2) {
                return "False";
            }
            const auto path = resources_ ? resources_->getFullPath(args[1]) : std::nullopt;
            con.print(path ? "path: " + path->string() : "Couldn't find file: " + args[1]);
            return {};
        });
    console.addCommand(
        0, "resetSimTime", [this](Console&, Int32, std::span<const std::string>) -> std::string {
            if (sg_.manager) {
                game_.resetScriptTime(sg_.manager->getCurrentTime());
            } else if (cg_.manager) {
                game_.resetScriptTime(cg_.manager->getCurrentTime());
            }
            return "true";
        });
    console.addCommand(
        0,
        "containerBoxFillSet",
        [this](Console&, Int32, std::span<const std::string> args) -> std::string {
            if (!sg_.manager || (args.size() != 7 && args.size() != 8)) {
                return "False";
            }
            auto* set = dynamic_cast<sim::SimSet*>(sg_.manager->findObject(args[1]));
            auto* root =
                dynamic_cast<sim::SimContainer*>(sg_.manager->findObject(SimRootContainerId));
            if (!set || !root) {
                return "False";
            }
            const auto p = point(args[3]),
                       half = Point3F(real(args[4]) / 2, real(args[5]) / 2, real(args[6]) / 2);
            auto low = p - half;
            if (args.size() == 8 && number(args[7]) == 1) {
                low.z = p.z;
            }
            sim::SimContainerQuery query;
            query.box = {low, p + half};
            query.mask = UInt32(number(args[2]));
            sim::SimContainerList found;
            root->findIntersections(query, found);
            for (auto* value : found) {
                (void)set->addObject(*value);
            }
            return std::to_string(found.size());
        });
}
} // namespace opent1::game
