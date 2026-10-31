#include <opent1/game/fear_plugin.hpp>
#include <opent1/game/fear_player_psc.hpp>
#include <opent1/game/explosion.hpp>
#include <opent1/game/player.hpp>
#include <opent1/game/player_manager.hpp>
#include <opent1/game/static_base.hpp>
#include <opent1/game/moveable.hpp>
#include <opent1/game/projectile_factory.hpp>
#include <opent1/game/interior_shape.hpp>
#include <charconv>
#include <cmath>
#include <format>
#include <sstream>

namespace opent1::game {
namespace {
Int32 integer(std::string_view text, Int32 fallback = -1) {
    Int32 value{};
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    return result.ec == std::errc{} && result.ptr == text.data() + text.size() ? value : fallback;
}

Player* player(sim::SimManager* manager, PlayerManager* clients, std::string_view text) {
    if (!manager) {
        return nullptr;
    }
    if (auto* object = dynamic_cast<Player*>(manager->findObject(text))) {
        return object;
    }
    if (clients) {
        if (auto* client = clients->findClient(integer(text)); client && client->ownedObject) {
            return dynamic_cast<Player*>(client->ownedObject);
        }
    }
    return nullptr;
}

GameBase* gameObject(sim::SimManager* manager, PlayerManager* clients, std::string_view text) {
    if (!manager) {
        return nullptr;
    }
    if (auto* object = dynamic_cast<GameBase*>(manager->findObject(text))) {
        return object;
    }
    const auto id = integer(text);
    if (clients && id >= 2049 && id <= 2176) {
        if (auto* client = clients->findClient(id)) {
            return client->ownedObject;
        }
    }
    return nullptr;
}

bool vector(std::string_view text, Point3F& value) {
    std::istringstream input{std::string(text)};
    return bool(input >> value.x >> value.y >> value.z) && std::isfinite(value.x) &&
           std::isfinite(value.y) && std::isfinite(value.z);
}

float scalar(std::string_view text) {
    float value{};
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    return parsed.ec == std::errc{} && std::isfinite(value) ? value : 0.0f;
}

std::string vectorText(const Point3F& value) {
    return std::format("{:g} {:g} {:g}", value.x, value.y, value.z);
}

Int32 item(DataBlockManager* blocks, std::string_view name) {
    if (!blocks) {
        return -1;
    }
    Int32 index = integer(name);
    if (index < 0) {
        (void)blocks->lookupDataBlock(name, DataBlockManager::ItemDataType, &index);
    }
    return index;
}
}

void FearPlugin::installGameplayCommands(Console& console) {
    installLegacyUtilityCommands(console);
    for (const auto command : {"Interior::setLightTime",
                               "Interior::toggleLight",
                               "Interior::switchOnLight",
                               "Interior::switchOffLight"}) {
        console.addCommand(
            0,
            command,
            [this, command](Console&, Int32, std::span<const std::string> args) -> std::string {
                const std::string_view name(command);
                const bool setTime = name == "Interior::setLightTime";
                auto* interior = args.size() == (setTime ? 4U : 3U)
                                     ? dynamic_cast<InteriorShape*>(
                                           gameObject(sg_.manager, sg_.playerManager, args[1]))
                                     : nullptr;
                auto* lights = interior ? interior->lightPlayer() : nullptr;
                if (!lights) {
                    return "False";
                }
                const auto& data = lights->lighting();
                for (const auto& light : data.lights) {
                    if (light.name < 0 ||
                        static_cast<std::size_t>(light.name) >= data.names.size()) {
                        continue;
                    }
                    const auto names =
                        std::span(data.names).subspan(static_cast<std::size_t>(light.name));
                    const auto end = std::ranges::find(names, std::byte{0});
                    if (end == names.end()) {
                        continue;
                    }
                    const std::string_view lightName(reinterpret_cast<const char*>(names.data()),
                                                     static_cast<std::size_t>(end - names.begin()));
                    if (lightName != args[2]) {
                        continue;
                    }
                    bool changed = false;
                    if (setTime) {
                        changed = lights->setTime(light.id, scalar(args[3]));
                    } else {
                        const auto current = lights->playback(light.id);
                        const UInt32 state = name == "Interior::switchOnLight"    ? 1U
                                             : name == "Interior::switchOffLight" ? 0U
                                             : current && current->state == 0     ? 1U
                                                                                  : 0U;
                        changed = lights->setState(light.id, state);
                    }
                    if (changed) {
                        interior->invalidateLightMaps();
                    }
                    break;
                }
                // The native commands return False even when they alter a light.
                return "False";
            });
    }
    console.addCommand(0,
                       "Player::deployItem",
                       [this](Console&, Int32, std::span<const std::string> args) -> std::string {
                           auto* object = args.size() == 3
                                              ? player(sg_.manager, sg_.playerManager, args[1])
                                              : nullptr;
                           return object && object->deployItem(item(sg_.dbm, args[2])) ? "True"
                                                                                       : "False";
                       });
    console.addCommand(0,
                       "Player::lastJetTime",
                       [this](Console&, Int32, std::span<const std::string> args) -> std::string {
                           const auto* object =
                               args.size() == 2 ? player(sg_.manager, sg_.playerManager, args[1])
                                                : nullptr;
                           return object ? std::to_string(object->getLastJetTime()) : "0";
                       });
    console.addCommand(0,
                       "GameBase::throw",
                       [this](Console&, Int32, std::span<const std::string> args) -> std::string {
                           auto* thrown = args.size() == 5
                                              ? gameObject(sg_.manager, sg_.playerManager, args[1])
                                              : nullptr;
                           auto* source = args.size() == 5
                                              ? gameObject(sg_.manager, sg_.playerManager, args[2])
                                              : nullptr;
                           if (!thrown || !source) {
                               return "False";
                           }
                           thrown->throwObject(source, scalar(args[3]), script::toBool(args[4]));
                           return "True";
                       });
    for (const auto command : {"Moveable::moveToWaypoint",
                               "Moveable::moveForward",
                               "Moveable::moveBackward",
                               "Moveable::stop",
                               "Moveable::getState",
                               "Moveable::getWaypointCount",
                               "Moveable::getPosition",
                               "Moveable::setWaypoint"}) {
        console.addCommand(
            0,
            command,
            [this, command](Console&, Int32, std::span<const std::string> args) -> std::string {
                const std::string_view name(command);
                const bool hasWaypoint =
                    name == "Moveable::moveToWaypoint" || name == "Moveable::setWaypoint";
                auto* object = args.size() == (hasWaypoint ? 3U : 2U)
                                   ? dynamic_cast<Moveable*>(
                                         gameObject(sg_.manager, sg_.playerManager, args[1]))
                                   : nullptr;
                if (!object) {
                    return name == "Moveable::getPosition" || name == "Moveable::getWaypointCount"
                               ? "0"
                               : "False";
                }
                if (name == "Moveable::getState") {
                    return std::string(object->getMoveStateName());
                }
                if (name == "Moveable::getWaypointCount") {
                    return std::to_string(object->getWaypointCount());
                }
                if (name == "Moveable::getPosition") {
                    return script::formatNumber(object->getWaypointPosition());
                }
                if (name == "Moveable::moveToWaypoint") {
                    return object->moveToWaypoint(integer(args[2])) ? "True" : "False";
                }
                if (name == "Moveable::setWaypoint") {
                    return object->setWaypoint(static_cast<Flt32>(integer(args[2]))) ? "True"
                                                                                     : "False";
                }
                if (name == "Moveable::moveForward") {
                    object->moveForward();
                } else if (name == "Moveable::moveBackward") {
                    object->moveBackward();
                } else {
                    object->stop();
                }
                return "True";
            });
    }
    console.addCommand(
        0,
        "Projectile::spawnProjectile",
        [this](Console& con, Int32, std::span<const std::string> args) -> std::string {
            if (!sg_.manager || !sg_.dbm || (args.size() != 5 && args.size() != 6)) {
                return "-1";
            }
            ProjectileDataType type;
            bool found = false;
            for (const auto group : {DataBlockManager::BulletDataType,
                                     DataBlockManager::GrenadeDataType,
                                     DataBlockManager::RocketDataType,
                                     DataBlockManager::LaserDataType,
                                     DataBlockManager::TargetLaserDataType,
                                     DataBlockManager::SeekingMissileDataType,
                                     DataBlockManager::LightningDataType,
                                     DataBlockManager::RepairEffectDataType}) {
                if (sg_.dbm->lookupDataBlock(args[1], group, &type.dataType)) {
                    type.type = group;
                    found = true;
                    break;
                }
            }
            if (!found) {
                return "-1";
            }
            TMat3F transform;
            transform.identity();
            std::istringstream input(args[2]);
            for (auto& row : transform.m) {
                for (auto& value : row) {
                    if (!(input >> value) || !std::isfinite(value)) {
                        return "-1";
                    }
                }
            }
            if (!(input >> transform.p.x >> transform.p.y >> transform.p.z) ||
                !std::isfinite(transform.p.x) || !std::isfinite(transform.p.y) ||
                !std::isfinite(transform.p.z)) {
                return "-1";
            }
            transform.flags = TMat3F::Matrix_HasRotation | TMat3F::Matrix_HasTranslation;
            Point3F velocity;
            if (!vector(args[4], velocity)) {
                return "-1";
            }
            auto projectile = createProjectile(type);
            if (!projectile) {
                return "-1";
            }
            projectile->bindConsole(&con);
            projectile->bindDataBlockManager(sg_.dbm);
            projectile->initProjectile(
                transform, velocity, static_cast<sim::SimObjectId>(integer(args[3], 0)));
            if (args.size() == 6 && projectile->isTargetable()) {
                projectile->setTarget(
                    dynamic_cast<ShapeBase*>(gameObject(sg_.manager, sg_.playerManager, args[5])));
            }
            auto* cleanup = dynamic_cast<sim::SimGroup*>(sg_.manager->findObject("MissionCleanup"));
            auto* result = sg_.manager->addObject(std::move(projectile), cleanup);
            return result ? std::to_string(result->getId()) : "-1";
        });
    // The shipped onDamage callbacks own armor, friendly-fire and shield rules.
    // Keep their native helpers available so damage follows the legacy scripts.
    console.addCommand(0,
                       "Vector::normalize",
                       [](Console&, Int32, std::span<const std::string> args) -> std::string {
                           Point3F value;
                           if (args.size() != 2 || !vector(args[1], value)) {
                               return "false";
                           }
                           // The static shield script also normalizes center - hitPosition for
                           // an unused intermediate. A center hit must not abort the callback.
                           if (value.len() == 0) {
                               return "0 0 0";
                           }
                           value.normalize();
                           return vectorText(value);
                       });
    console.addCommand(0,
                       "GameBase::getControlClient",
                       [this](Console&, Int32, std::span<const std::string> args) -> std::string {
                           const auto* object =
                               args.size() == 2 && sg_.playerManager
                                   ? gameObject(sg_.manager, sg_.playerManager, args[1])
                                   : nullptr;
                           return object && object->getControlClient()
                                      ? std::to_string(object->getControlClient())
                                      : "-1";
                       });
    console.addCommand(0,
                       "GameBase::getOwnerClient",
                       [this](Console&, Int32, std::span<const std::string> args) -> std::string {
                           const auto* object =
                               args.size() == 2 && sg_.playerManager
                                   ? gameObject(sg_.manager, sg_.playerManager, args[1])
                                   : nullptr;
                           return object && object->getOwnerClient()
                                      ? std::to_string(object->getOwnerClient())
                                      : "-1";
                       });
    console.addCommand(0,
                       "Client::setOwnedObject",
                       [this](Console&, Int32, std::span<const std::string> args) -> std::string {
                           auto* client = args.size() == 3 && sg_.playerManager
                                              ? sg_.playerManager->findClient(integer(args[1]))
                                              : nullptr;
                           if (!client) {
                               return "false";
                           }
                           auto* object = gameObject(sg_.manager, sg_.playerManager, args[2]);
                           if (client->ownedObject && client->ownedObject != object) {
                               client->ownedObject->setOwnerClient(0);
                           }
                           if (object) {
                               if (auto* previous =
                                       sg_.playerManager->findClient(object->getOwnerClient());
                                   previous && previous->ownedObject == object) {
                                   previous->ownedObject = nullptr;
                               }
                               object->setOwnerClient(client->id);
                           }
                           client->ownedObject = object;
                           return "true";
                       });
    console.addCommand(0,
                       "Player::setMountObject",
                       [this](Console&, Int32, std::span<const std::string> args) -> std::string {
                           if (args.size() != 4) {
                               return "False";
                           }
                           if (auto* object = player(sg_.manager, sg_.playerManager, args[1])) {
                               object->setMountObject(
                                   gameObject(sg_.manager, sg_.playerManager, args[2]),
                                   integer(args[3], 0));
                           }
                           return "True";
                       });
    console.addCommand(0,
                       "Player::getMountObject",
                       [this](Console&, Int32, std::span<const std::string> args) -> std::string {
                           if (args.size() != 2) {
                               return "False";
                           }
                           const auto* object = player(sg_.manager, sg_.playerManager, args[1]);
                           return object && object->getMountObject()
                                      ? std::to_string(object->getMountObject()->getId())
                                      : "-1";
                       });
    console.addCommand(0,
                       "Player::isExposed",
                       [this](Console&, Int32, std::span<const std::string> args) -> std::string {
                           const auto* object =
                               args.size() == 2 ? player(sg_.manager, sg_.playerManager, args[1])
                                                : nullptr;
                           return object && object->isExposed() ? "True" : "False";
                       });
    console.addCommand(0,
                       "Player::isTriggered",
                       [this](Console&, Int32, std::span<const std::string> args) -> std::string {
                           const auto* object =
                               args.size() == 3 ? player(sg_.manager, sg_.playerManager, args[1])
                                                : nullptr;
                           const auto slot = args.size() == 3 ? integer(args[2]) : -1;
                           return object && slot >= 0 && slot < Player::MaxImageSlots &&
                                          object->isImageTriggerDown(slot)
                                      ? "True"
                                      : "False";
                       });
    console.addCommand(0,
                       "Player::dropItem",
                       [this](Console&, Int32, std::span<const std::string> args) -> std::string {
                           auto* object = args.size() == 3
                                              ? player(sg_.manager, sg_.playerManager, args[1])
                                              : nullptr;
                           return object && object->dropItem(item(sg_.dbm, args[2])) ? "True"
                                                                                     : "False";
                       });
    console.addCommand(0,
                       "Player::applyImpulse",
                       [this](Console&, Int32, std::span<const std::string> args) -> std::string {
                           auto* object = args.size() == 3
                                              ? player(sg_.manager, sg_.playerManager, args[1])
                                              : nullptr;
                           Point3F impulse;
                           if (object && vector(args[2], impulse)) {
                               (void)object->applyImpulse(impulse);
                           }
                           // The original binding returns False even when the impulse succeeds.
                           return "False";
                       });
    console.addCommand(0,
                       "Player::getDamageFlash",
                       [this](Console&, Int32, std::span<const std::string> args) -> std::string {
                           const auto* object =
                               args.size() == 2 ? player(sg_.manager, sg_.playerManager, args[1])
                                                : nullptr;
                           return object ? std::format("{:g}", object->getDamageFlash()) : "0";
                       });
    console.addCommand(0,
                       "Player::setDamageFlash",
                       [this](Console&, Int32, std::span<const std::string> args) -> std::string {
                           auto* object = args.size() == 3
                                              ? player(sg_.manager, sg_.playerManager, args[1])
                                              : nullptr;
                           if (!object) {
                               return "False";
                           }
                           object->setDamageFlash(scalar(args[2]));
                           return "True";
                       });
    console.addCommand(0,
                       "Player::getLastContactCount",
                       [this](Console&, Int32, std::span<const std::string> args) -> std::string {
                           const auto* object =
                               args.size() == 2 ? player(sg_.manager, sg_.playerManager, args[1])
                                                : nullptr;
                           return object ? std::to_string(object->lastContactCount()) : "-1";
                       });
    console.addCommand(0,
                       "Player::isCrouching",
                       [this](Console&, Int32, std::span<const std::string> args) -> std::string {
                           const auto* object =
                               args.size() == 2 ? player(sg_.manager, sg_.playerManager, args[1])
                                                : nullptr;
                           return object ? (object->isCrouching() ? "True" : "False") : "-1";
                       });
    console.addCommand(0,
                       "Player::setAnimation",
                       [this](Console&, Int32, std::span<const std::string> args) -> std::string {
                           if (args.size() != 3) {
                               return "False";
                           }
                           if (auto* object = player(sg_.manager, sg_.playerManager, args[1])) {
                               object->setAnimation(static_cast<UInt32>(integer(args[2], 0)));
                           }
                           return "True";
                       });
    console.addCommand(
        0,
        "GameBase::activateShield",
        [this](Console&, Int32, std::span<const std::string> args) -> std::string {
            auto* object = (args.size() == 3 || args.size() == 4) && sg_.manager
                               ? dynamic_cast<ShapeBase*>(sg_.manager->findObject(args[1]))
                               : nullptr;
            Point3F direction;
            if (!object || !vector(args[2], direction)) {
                return "False";
            }
            object->setShieldVec(direction, args.size() == 4 ? scalar(args[3]) : 0.0f);
            return "True";
        });
    console.addCommand(
        0,
        "GameBase::getDisabledDamage",
        [this](Console&, Int32, std::span<const std::string> args) -> std::string {
            const auto* object = args.size() == 2 && sg_.manager
                                     ? dynamic_cast<StaticBase*>(sg_.manager->findObject(args[1]))
                                     : nullptr;
            const auto* data =
                object ? dynamic_cast<StaticBaseData*>(object->getDatPtr()) : nullptr;
            return object ? std::format("{:g}",
                                        data ? data->maxDamage * data->damageLevel.disabled : 0.0f)
                          : "false";
        });
    console.addCommand(0,
                       "GameBase::getRadius",
                       [this](Console&, Int32, std::span<const std::string> args) -> std::string {
                           const auto* object =
                               args.size() == 2 && sg_.manager
                                   ? dynamic_cast<ShapeBase*>(sg_.manager->findObject(args[1]))
                                   : nullptr;
                           return object ? std::format("{:g}", object->getRadius()) : "false";
                       });
    console.addCommand(0,
                       "getBoxCenter",
                       [this](Console&, Int32, std::span<const std::string> args) -> std::string {
                           const auto* object =
                               args.size() == 2
                                   ? gameObject(sg_.manager, sg_.playerManager, args[1])
                                   : nullptr;
                           return object ? vectorText(object->getBoxCenter()) : "0 0 0";
                       });
    console.addCommand(0,
                       "findPointOnSphere",
                       [this](Console&, Int32, std::span<const std::string> args) -> std::string {
                           const auto* object =
                               args.size() == 5 && sg_.manager
                                   ? dynamic_cast<ShapeBase*>(sg_.manager->findObject(args[4]))
                                   : nullptr;
                           Point3F eye, center, direction;
                           if (!object || !vector(args[1], eye) || !vector(args[2], center) ||
                               !vector(args[3], direction)) {
                               return "0 0 0";
                           }
                           const auto length = direction.len();
                           if (length < 0.01f) {
                               return "0 0 0";
                           }
                           direction /= length;
                           const auto radius = object->getRadius() - 0.5f;
                           const Point3F offset = center - eye;
                           const auto projection = m_dot(offset, direction);
                           const auto discriminant =
                               radius * radius - (m_dot(offset, offset) - projection * projection);
                           // The legacy helper uses sqrt(abs(discriminant)) even for a missed sphere.
                           const Point3F point =
                               eye + direction * (projection - std::sqrt(std::abs(discriminant)));
                           Point3F result = center - point;
                           const auto resultLength = result.len();
                           if (resultLength < 0.01f) {
                               return "0 0 0";
                           }
                           result /= resultLength;
                           return vectorText(result);
                       });
    console.addCommand(0,
                       "GameBase::applyRadiusDamage",
                       [this](Console&, Int32, std::span<const std::string> args) -> std::string {
                           Point3F position;
                           if (args.size() != 7 || !sg_.manager || !vector(args[2], position)) {
                               return "false";
                           }
                           Explosion::applyRadiusDamage(sg_.manager,
                                                        integer(args[1], 0),
                                                        position,
                                                        Point3F{0, 0, 0},
                                                        scalar(args[3]),
                                                        scalar(args[4]),
                                                        scalar(args[5]),
                                                        integer(args[6], 0));
                           return "true";
                       });
    console.addCommand(0,
                       "GameBase::repairDamage",
                       [this](Console&, Int32, std::span<const std::string> args) -> std::string {
                           auto* object =
                               args.size() == 3 && sg_.manager
                                   ? dynamic_cast<ShapeBase*>(sg_.manager->findObject(args[1]))
                                   : nullptr;
                           if (!object) {
                               return "False";
                           }
                           (void)object->repairDamage(scalar(args[2]));
                           return "True";
                       });
    console.addCommand(0,
                       "GameBase::setAutoRepairRate",
                       [this](Console&, Int32, std::span<const std::string> args) -> std::string {
                           auto* object =
                               args.size() == 3 && sg_.manager
                                   ? dynamic_cast<ShapeBase*>(sg_.manager->findObject(args[1]))
                                   : nullptr;
                           if (!object) {
                               return "False";
                           }
                           object->setAutoRepairRate(scalar(args[2]));
                           return "True";
                       });
    console.addCommand(0,
                       "GameBase::getAutoRepairRate",
                       [this](Console&, Int32, std::span<const std::string> args) -> std::string {
                           const auto* object =
                               args.size() == 2 && sg_.manager
                                   ? dynamic_cast<ShapeBase*>(sg_.manager->findObject(args[1]))
                                   : nullptr;
                           return object ? std::format("{:g}", object->getAutoRepairRate()) : "0";
                       });
    console.addCommand(0,
                       "GameBase::startFadeOut",
                       [this](Console&, Int32, std::span<const std::string> args) -> std::string {
                           auto* object = args.size() == 2
                                              ? dynamic_cast<ShapeBase*>(gameObject(
                                                    sg_.manager, sg_.playerManager, args[1]))
                                              : nullptr;
                           if (!object) {
                               return "False";
                           }
                           object->startFadeOut();
                           return "True";
                       });
    console.addCommand(0,
                       "getIntegerTime",
                       [this](Console&, Int32, std::span<const std::string> args) -> std::string {
                           if (args.size() != 2) {
                               return {};
                           }
                           const bool server =
                               args[1] == "true" || args[1] == "True" || integer(args[1], 0) != 0;
                           const auto time = server ? sg_.currentTime : cg_.currentTime;
                           return std::to_string(time >> 5) + "." + std::to_string(time & 31);
                       });
    console.addCommand(
        0,
        "Player::getItemState",
        [this](Console&, Int32, std::span<const std::string> args) -> std::string {
            const auto* object =
                args.size() == 3 ? player(sg_.manager, sg_.playerManager, args[1]) : nullptr;
            constexpr std::array states{"Reset",
                                        "Fire",
                                        "NoAmmo",
                                        "Idle",
                                        "Ready",
                                        "Reload",
                                        "SpinUp",
                                        "SpinDown",
                                        "Activate",
                                        "Deactivate"};
            const auto state = object ? object->getImageState(integer(args[2])) : -1;
            return state >= 0 && std::size_t(state) < states.size() ? states[state] : "";
        });
    console.addCommand(0,
                       "Player::unMountItem",
                       [this](Console&, Int32, std::span<const std::string> args) -> std::string {
                           auto* object = args.size() == 3
                                              ? player(sg_.manager, sg_.playerManager, args[1])
                                              : nullptr;
                           return object && object->unmountItem(integer(args[2])) ? "True"
                                                                                  : "False";
                       });
    console.addCommand(
        0,
        "getObjectType",
        [this](Console&, Int32, std::span<const std::string> args) -> std::string {
            const auto* object = args.size() == 2 ? game_.findObject(args[1]) : nullptr;
            const auto* descriptor =
                object ? game_.classes().find(std::type_index(typeid(*object))) : nullptr;
            return descriptor ? descriptor->name : std::string{};
        });
    console.addCommand(0,
                       "Client::setMenuScoreVis",
                       [this](Console&, Int32, std::span<const std::string> args) -> std::string {
                           auto* client = args.size() == 3 && sg_.playerManager
                                              ? sg_.playerManager->findClient(integer(args[1]))
                                              : nullptr;
                           if (!client) {
                               return "False";
                           }
                           const bool visible =
                               args[2] == "true" || args[2] == "True" || integer(args[2], 0) != 0;
                           sg_.playerManager->setDisplayScores(client->id, visible);
                           if (!client->packetStream) {
                               game_.withWorld(sim::WorldType::Client, [&] {
                                   if (onScoresVisible) {
                                       onScoresVisible(visible);
                                   }
                               });
                           }
                           return "True";
                       });
    console.addCommand(0,
                       "remoteOpenT1ScoresVisible",
                       [this](Console&, Int32, std::span<const std::string> args) -> std::string {
                           if (args.size() != 3 || integer(args[1]) != 2048) {
                               return "False";
                           }
                           if (onScoresVisible) {
                               onScoresVisible(integer(args[2], 0) != 0);
                           }
                           return "True";
                       });
    for (const auto command : {"GameBase::generatePower",
                               "GameBase::isPowered",
                               "GameBase::setActive",
                               "GameBase::isActive",
                               "GameBase::playSequence",
                               "GameBase::stopSequence",
                               "GameBase::pauseSequence",
                               "GameBase::setSequenceDirection",
                               "GameBase::getDamageState"}) {
        console.addCommand(
            0,
            command,
            [this, command](Console&, Int32, std::span<const std::string> args) -> std::string {
                auto* base = args.size() >= 2 && sg_.manager
                                 ? dynamic_cast<GameBase*>(sg_.manager->findObject(args[1]))
                                 : nullptr;
                if (!base) {
                    return "False";
                }
                const std::string_view op(command);
                const bool enabled = args.size() > 2 && (args[2] == "true" || args[2] == "True" ||
                                                         integer(args[2], 0) != 0);
                if (op == "GameBase::generatePower" && args.size() == 3) {
                    base->generatePower(enabled);
                    return "True";
                }
                if (op == "GameBase::isPowered") {
                    return base->isPowered() ? "True" : "False";
                }
                if (auto* object = dynamic_cast<StaticBase*>(base)) {
                    if (op == "GameBase::setActive" && args.size() == 3) {
                        return object->setActive(enabled) ? "True" : "False";
                    }
                    if (op == "GameBase::isActive") {
                        return object->isActive() ? "True" : "False";
                    }
                }
                auto* shape = dynamic_cast<ShapeBase*>(base);
                if (op == "GameBase::getDamageState") {
                    return shape ? std::string(shape->getDamageState()) : "False";
                }
                if (!shape || args.size() < 3) {
                    return "False";
                }
                const Int32 slot = integer(args[2]);
                if (slot < 0 || slot >= ShapeBase::MaxScriptThreads) {
                    return "False";
                }
                if (op == "GameBase::stopSequence") {
                    return shape->stopSequence(slot) ? "True" : "False";
                }
                if (op == "GameBase::pauseSequence") {
                    return shape->pauseSequence(slot) ? "True" : "False";
                }
                if (op == "GameBase::setSequenceDirection" && args.size() == 4) {
                    return shape->setSequenceDir(slot, integer(args[3], 0) != 0) ? "True" : "False";
                }
                if (op == "GameBase::playSequence") {
                    if (args.size() > 3) {
                        const auto* data = shape->getTSShape();
                        if (!data) {
                            return "False";
                        }
                        Int32 sequence = -1;
                        for (std::size_t i = 0; i < data->sequences.size(); ++i) {
                            const auto name = data->sequences[i].name;
                            if (name >= 0 && std::size_t(name) < data->names.size() &&
                                data->names[name] == args[3]) {
                                sequence = static_cast<Int32>(i);
                                break;
                            }
                        }
                        if (sequence < 0 || !shape->setSequence(slot, sequence)) {
                            return "False";
                        }
                    }
                    return shape->playSequence(slot) ? "True" : "False";
                }
                return "False";
            });
    }
    console.addCommand(
        0,
        "GameBase::getLOSInfo",
        [this](Console& con, Int32, std::span<const std::string> args) -> std::string {
            auto* object = args.size() >= 3 && sg_.manager
                               ? dynamic_cast<ShapeBase*>(sg_.manager->findObject(args[1]))
                               : nullptr;
            if (!object || args.size() > 4) {
                return "False";
            }
            float range{};
            const auto parsed =
                std::from_chars(args[2].data(), args[2].data() + args[2].size(), range);
            if (parsed.ec != std::errc{} || !std::isfinite(range) || range < 0 || range > 100000) {
                return "False";
            }
            auto transform = object->getLOSTransform();
            if (args.size() == 4) {
                EulerF rotation{};
                std::istringstream input(args[3]);
                if (!(input >> rotation.x >> rotation.y >> rotation.z)) {
                    return "False";
                }
                const TMat3F local(rotation, {0, 0, 0});
                TMat3F result;
                m_mul(local, transform, result);
                transform = result;
            }
            Point3F position, normal;
            auto* target = object->findLOSObject(
                transform,
                range,
                SimTerrainObjectType | SimInteriorObjectType | StaticObjectType |
                    SimPlayerObjectType | VehicleObjectType | MoveableObjectType | ItemObjectType,
                &position,
                &normal);
            if (!target) {
                return "False";
            }
            const auto vectorText = [](const Point3F& v) {
                return std::to_string(v.x) + " " + std::to_string(v.y) + " " + std::to_string(v.z);
            };
            con.setVariable("los::object", std::to_string(target->getId()));
            con.setVariable("los::position", vectorText(position));
            con.setVariable("los::normal", vectorText(normal));
            return "True";
        });
    console.addCommand(0,
                       "Player::isAiControlled",
                       [this](Console&, Int32, std::span<const std::string> args) -> std::string {
                           const auto* object =
                               args.size() == 2 ? player(sg_.manager, sg_.playerManager, args[1])
                                                : nullptr;
                           return object && object->getOwnerClient() <= 0 ? "True" : "False";
                       });
    console.addCommand(0,
                       "Client::getGuiMode",
                       [this](Console&, Int32, std::span<const std::string> args) -> std::string {
                           const auto* client =
                               args.size() == 2 && sg_.playerManager
                                   ? sg_.playerManager->findClient(integer(args[1]))
                                   : nullptr;
                           return client ? std::to_string(client->curGuiMode) : "1";
                       });
    console.addCommand(0,
                       "Client::setGuiMode",
                       [this](Console&, Int32, std::span<const std::string> args) -> std::string {
                           auto* client = args.size() == 3 && sg_.playerManager
                                              ? sg_.playerManager->findClient(integer(args[1]))
                                              : nullptr;
                           const auto mode = args.size() == 3 ? integer(args[2]) : -1;
                           if (!client || mode < 0 || mode > 6) {
                               return "False";
                           }
                           const auto oldMode = client->playerPSC ? client->playerPSC->guiMode()
                                                                  : client->curGuiMode;
                           // Stations repeatedly request inventory mode. Native c_setClientGuiMode
                           // only publishes a transition; reopening the same graph loses selection.
                           if (UInt32(mode) == oldMode) {
                               return "True";
                           }
                           if (client->playerPSC) {
                               client->playerPSC->setGuiMode(UInt32(mode));
                           }
                           sg_.playerManager->clientGuiModeChanged(client->id, mode, oldMode);
                           if (auto* controlled = dynamic_cast<Player*>(client->ownedObject)) {
                               controlled->setCurGuiMode(mode);
                           }
                           if (client->packetStream) {
                               return "True";
                           }
                           game_.withWorld(sim::WorldType::Client, [&] {
                               if (onGuiMode) {
                                   onGuiMode(mode);
                               }
                           });
                           return "True";
                       });
    for (const auto command : {"Client::setItemShopping",
                               "Client::setItemBuying",
                               "Client::clearItemShopping",
                               "Client::clearItemBuying"}) {
        console.addCommand(
            0,
            command,
            [this, command](Console&, Int32, std::span<const std::string> args) -> std::string {
                const auto* client = args.size() >= 2 && sg_.playerManager
                                         ? sg_.playerManager->findClient(integer(args[1]))
                                         : nullptr;
                auto* psc = client ? client->playerPSC : nullptr;
                if (!psc) {
                    return "False";
                }
                const std::string_view op(command);
                if (op == "Client::clearItemShopping") {
                    psc->clearShoppingList();
                } else if (op == "Client::clearItemBuying") {
                    psc->clearBuyList();
                } else if (args.size() >= 3) {
                    const auto index = item(sg_.dbm, args[2]);
                    if (index < 0) {
                        return "False";
                    }
                    const bool enabled = args.size() < 4 || args[3] == "True" ||
                                         args[3] == "true" || integer(args[3], 0) != 0;
                    if (op == "Client::setItemShopping") {
                        if (enabled) {
                            psc->setInShoppingList(index);
                        } else {
                            psc->clearFromShoppingList(index);
                        }
                    } else {
                        if (enabled) {
                            psc->setInBuyList(index);
                        } else {
                            psc->clearFromBuyList(index);
                        }
                    }
                }
                return "True";
            });
    }
    console.addCommand(
        0,
        "Client::sendMessage",
        [this](Console&, Int32, std::span<const std::string> args) -> std::string {
            const auto* client = (args.size() == 4 || args.size() == 5) && sg_.playerManager
                                     ? sg_.playerManager->findClient(integer(args[1]))
                                     : nullptr;
            if (!client) {
                return "False";
            }
            auto event = std::make_unique<PlayerSayEvent>();
            event->flags = sim::SimEvent::Guaranteed | sim::SimEvent::Ordered;
            event->address.objectId = PlayerManagerId;
            event->sender = args.size() == 5 ? std::max(0, integer(args[4], 0)) : 0;
            event->msgType = static_cast<UInt32>(integer(args[2], 0));
            event->message = args[3].substr(0, PlayerManager::MaxMsgLength);
            if (client->packetStream) {
                return client->packetStream->sendRemoteEvent(std::move(event)) ? "True" : "False";
            }
            if (cg_.playerManager) {
                game_.withWorld(sim::WorldType::Client, [&] {
                    (void)cg_.playerManager->processEvent(*event);
                });
            }
            return "True";
        });
    console.addCommand(0,
                       "remoteOpenT1Chat",
                       [this](Console&, Int32, std::span<const std::string> args) -> std::string {
                           if (args.size() != 4 || integer(args[1]) != 2048) {
                               return "False";
                           }
                           if (onChatMessage) {
                               onChatMessage(integer(args[2], 0), args[3]);
                           }
                           return "True";
                       });
    // The local host uses in-process replication, so it has no PacketStream.
    // Route its console events with the same source ID and world context.
    console.addCommand(
        0,
        "remoteEval",
        [this](Console& con, Int32, std::span<const std::string> args) -> std::string {
            if (args.size() < 3 || args.size() > 33) {
                return "False";
            }
            const auto peer = integer(args[1]);
            if (connectAddress_ == "LOOPBACK:28001" && (peer == 2048 || peer == 2049)) {
                std::vector<std::string> parameters{peer == 2048 ? "2049" : "2048"};
                parameters.insert(parameters.end(), args.begin() + 3, args.end());
                bool success = false;
                game_.withWorld(peer == 2048 ? sim::WorldType::Server : sim::WorldType::Client,
                                [&] {
                                    success = con.execute("remote" + args[2], parameters).ok();
                                });
                return success ? "True" : "False";
            }
            auto* manager = game_.manager();
            auto* stream =
                manager && peer > 0
                    ? dynamic_cast<sim::network::PacketStream*>(manager->findObject(peer))
                    : nullptr;
            if (!stream) {
                return "False";
            }
            auto event = std::make_unique<sim::SimConsoleEvent>();
            event->flags = sim::SimEvent::Guaranteed | sim::SimEvent::Ordered;
            event->arguments.assign(args.begin() + 2, args.end());
            return event->valid() && stream->sendRemoteEvent(std::move(event)) ? "True" : "False";
        });
    console.addCommand(0, "getNumItems", [this](Console&, Int32, auto) {
        return std::to_string(sg_.dbm ? sg_.dbm->getDataGroupSize(DataBlockManager::ItemDataType)
                                      : 0);
    });
    console.addCommand(0,
                       "getItemData",
                       [this](Console&, Int32, std::span<const std::string> args) -> std::string {
                           const auto* data =
                               args.size() == 2 && sg_.dbm
                                   ? sg_.dbm->lookupDataBlock(integer(args[1]),
                                                              DataBlockManager::ItemDataType)
                                   : nullptr;
                           return data ? data->dbmName : "-1";
                       });
    console.addCommand(0,
                       "Player::getClient",
                       [this](Console&, Int32, std::span<const std::string> args) -> std::string {
                           const auto* object =
                               args.size() == 2 ? player(sg_.manager, sg_.playerManager, args[1])
                                                : nullptr;
                           return object ? std::to_string(object->getOwnerClient()) : "-1";
                       });
    console.addCommand(
        0,
        "Player::getItemClassCount",
        [this](Console&, Int32, std::span<const std::string> args) -> std::string {
            const auto* object =
                args.size() == 3 ? player(sg_.manager, sg_.playerManager, args[1]) : nullptr;
            Int32 count = 0;
            if (object && sg_.dbm) {
                for (Int32 index = 0;
                     index < sg_.dbm->getDataGroupSize(DataBlockManager::ItemDataType);
                     ++index) {
                    const auto* data =
                        sg_.dbm->lookupDataBlock(index, DataBlockManager::ItemDataType);
                    if (data && data->className == args[2] && object->getItemCount(index) > 0) {
                        ++count;
                    }
                }
            }
            return std::to_string(count);
        });
    console.addCommand(0,
                       "Player::getArmor",
                       [this](Console&, Int32, std::span<const std::string> args) -> std::string {
                           const auto* object =
                               args.size() == 2 ? player(sg_.manager, sg_.playerManager, args[1])
                                                : nullptr;
                           return object ? std::string(object->getDatFileName()) : "-1";
                       });
    console.addCommand(
        0,
        "Player::setArmor",
        [this](Console&, Int32, std::span<const std::string> args) -> std::string {
            auto* object =
                args.size() == 3 ? player(sg_.manager, sg_.playerManager, args[1]) : nullptr;
            if (!object || !sg_.dbm ||
                !sg_.dbm->lookupDataBlock(args[2], DataBlockManager::PlayerDataType)) {
                return "False";
            }
            object->setDatFileName(args[2]);
            return object->getDatFileName() == args[2] ? "True" : "False";
        });
    for (const auto command : {"Player::incItemCount", "Player::decItemCount"}) {
        console.addCommand(
            0,
            command,
            [this, command](Console&, Int32, std::span<const std::string> args) -> std::string {
                auto* object =
                    args.size() >= 3 ? player(sg_.manager, sg_.playerManager, args[1]) : nullptr;
                const auto index = args.size() >= 3 ? item(sg_.dbm, args[2]) : -1;
                if (!object || index < 0) {
                    return "0";
                }
                const auto amount = args.size() > 3 ? integer(args[3], 1) : 1;
                const auto count = std::max(
                    0,
                    object->getItemCount(index) +
                        (std::string_view(command) == "Player::incItemCount" ? amount : -amount));
                return object->setItemCount(index, count) ? std::to_string(count) : "0";
            });
    }
    console.addCommand(
        0,
        "Player::getMountedItem",
        [this](Console&, Int32, std::span<const std::string> args) -> std::string {
            const auto* object =
                args.size() == 3 ? player(sg_.manager, sg_.playerManager, args[1]) : nullptr;
            const auto index = object ? object->getMountedItem(integer(args[2])) : -1;
            const auto* data = index >= 0 && sg_.dbm
                                   ? sg_.dbm->lookupDataBlock(index, DataBlockManager::ItemDataType)
                                   : nullptr;
            return data ? data->dbmName : "-1";
        });
    console.addCommand(
        0,
        "GameBase::setRechargeRate",
        [this](Console&, Int32, std::span<const std::string> args) -> std::string {
            auto* object = args.size() == 3 && sg_.manager
                               ? dynamic_cast<ShapeBase*>(sg_.manager->findObject(args[1]))
                               : nullptr;
            if (!object) {
                return "False";
            }
            float rate{};
            const auto result =
                std::from_chars(args[2].data(), args[2].data() + args[2].size(), rate);
            if (result.ec != std::errc{} || result.ptr != args[2].data() + args[2].size() ||
                !std::isfinite(rate) || rate < 0) {
                return "False";
            }
            object->setRechargeRate(rate);
            return "True";
        });
    console.addCommand(0,
                       "Client::getGender",
                       [this](Console&, Int32, std::span<const std::string> args) -> std::string {
                           const auto* client =
                               args.size() == 2 && sg_.playerManager
                                   ? sg_.playerManager->findClient(integer(args[1]))
                                   : nullptr;
                           return client ? client->gender : "";
                       });
    console.addCommand(
        0,
        "Client::isItemShoppingOn",
        [this](Console&, Int32, std::span<const std::string> args) -> std::string {
            const auto* client = args.size() == 3 && sg_.playerManager
                                     ? sg_.playerManager->findClient(integer(args[1]))
                                     : nullptr;
            return client && client->playerPSC &&
                           client->playerPSC->isItemShoppingOn(item(sg_.dbm, args[2]))
                       ? "True"
                       : "False";
        });
}

} // namespace opent1::game
