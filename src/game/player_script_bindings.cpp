#include <opent1/game/fear_plugin.hpp>
#include <opent1/game/player.hpp>
#include <opent1/game/player_manager.hpp>
#include <opent1/game/data_block_manager.hpp>
#include <charconv>
#include <format>

namespace opent1::game {
namespace {
Int32 integer(std::string_view text) {
    Int32 value{};
    std::from_chars(text.data(), text.data() + text.size(), value);
    return value;
}

Flt32 real(std::string_view text) {
    Flt32 value{};
    std::from_chars(text.data(), text.data() + text.size(), value);
    return value;
}

Player* resolve(sim::SimManager* manager, PlayerManager* clients, std::string_view name) {
    if (!manager)
        return nullptr;
    if (auto* object = dynamic_cast<Player*>(manager->findObject(name)))
        return object;
    if (auto* client = clients ? clients->findBaseRep(integer(name)) : nullptr)
        return dynamic_cast<Player*>(client->ownedObject);
    return nullptr;
}
}

void FearPlugin::installPlayerCommands(Console& console) {
    console.addCommand(0,
                       "Player::blowUp",
                       [this](Console&, Int32, std::span<const std::string> args) -> std::string {
                           if (auto* object = args.size() == 2
                                                  ? resolve(sg_.manager, sg_.playerManager, args[1])
                                                  : nullptr)
                               object->setBlownUp(true);
                           return "True"; // original command succeeds even when no player resolves
                       });
    console.addCommand(0,
                       "Player::isJetting",
                       [this](Console&, Int32, std::span<const std::string> args) -> std::string {
                           const auto* object =
                               args.size() == 2 ? resolve(sg_.manager, sg_.playerManager, args[1])
                                                : nullptr;
                           return object && object->isJetting() ? "True" : "False";
                       });
    console.addCommand(
        0,
        "Player::getNextMountedItem",
        [this](Console&, Int32, std::span<const std::string> args) -> std::string {
            const auto* object =
                args.size() == 3 ? resolve(sg_.manager, sg_.playerManager, args[1]) : nullptr;
            const auto slot = args.size() == 3 ? integer(args[2]) : -1;
            const auto index = object && slot >= 0 && slot < Player::MaxImageSlots
                                   ? object->getItemImageEntry(slot).nextId
                                   : -1;
            const auto* data = index >= 0 && sg_.dbm
                                   ? sg_.dbm->lookupDataBlock(index, DataBlockManager::ItemDataType)
                                   : nullptr;
            return data ? data->dbmName : "-1";
        });
    console.addCommand(0,
                       "Player::setSensorSupression",
                       [this](Console&, Int32, std::span<const std::string> args) -> std::string {
                           auto* object = args.size() == 3
                                              ? resolve(sg_.manager, sg_.playerManager, args[1])
                                              : nullptr;
                           if (!object)
                               return "False";
                           object->setSupressionField(real(args[2]));
                           return "True";
                       });
    console.addCommand(
        0,
        "Player::getSensorSupression",
        [this](Console&, Int32, std::span<const std::string> args) -> std::string {
            const auto* object =
                args.size() == 2 ? resolve(sg_.manager, sg_.playerManager, args[1]) : nullptr;
            return object ? std::format("{:.6f}", object->getSupressionField()) : "0";
        });
    console.addCommand(0,
                       "Player::setDetectParameters",
                       [this](Console&, Int32, std::span<const std::string> args) -> std::string {
                           auto* object = args.size() == 4
                                              ? resolve(sg_.manager, sg_.playerManager, args[1])
                                              : nullptr;
                           if (!object)
                               return "False";
                           object->setDetectParams(real(args[2]), real(args[3]));
                           return "True";
                       });
}
}
