#include <opent1/game/fear_plugin.hpp>
#include <ranges>
#include <opent1/game/fear_player_psc.hpp>
#include <opent1/game/player_manager.hpp>
#include <opent1/game/player.hpp>
#include <algorithm>
#include <array>
#include <cctype>
#include <cstdlib>

namespace opent1::game {
namespace {
Int32 integer(const std::string& value) {
    return std::atoi(value.c_str());
}

bool boolean(std::string_view value) {
    if (value.starts_with('t') || value.starts_with('T')) {
        return true;
    }
    if (value.starts_with('f') || value.starts_with('F')) {
        return false;
    }
    return std::atof(std::string(value).c_str()) != 0;
}

struct MasterResolution {
    std::array<std::string, 10> addresses{}, results{};
    std::unique_ptr<net::AddressLookup> lookup;
    std::size_t count{}, next{};
    bool resolved{};

    void poll() {
        while (!resolved && next < count) {
            if (!lookup) {
                lookup = net::resolveIPv4AddressAsync(addresses[next]);
            }
            if (lookup && !lookup->poll(results[next])) {
                return;
            }
            lookup.reset();
            ++next;
        }
        resolved = count != 0 && next == count;
    }
};
}

void FearPlugin::installNetworkCommands(Console& console) {
    console.addCommand(
        0, "playVoice", [this](Console&, Int32, std::span<const std::string> args) -> std::string {
            if (!sg_.manager || !sg_.dbm || !sg_.playerManager || args.size() != 3) {
                return "False";
            }
            auto* speaker = sg_.playerManager->findClient(integer(args[1]));
            auto* profile = dynamic_cast<SoundProfileData*>(
                sg_.dbm->lookupDataBlock(0, DataBlockManager::SoundProfileDataType));
            if (!speaker || !speaker->ownedObject || !profile) {
                return "True";
            }
            for (auto* client = sg_.playerManager->getClientList(); client;
                 client = static_cast<PlayerManager::ClientRep*>(client->nextClient)) {
                auto* control = client->playerPSC ? client->playerPSC->getControlObject() : nullptr;
                if (!control ||
                    (control->getLinearPosition() - speaker->ownedObject->getLinearPosition())
                            .len() >= profile->profile.maxDistance) {
                    continue;
                }
                auto event = std::make_unique<VoiceEvent>(speaker->id, args[2]);
                event->flags = sim::SimEvent::Guaranteed;
                if (client->packetStream) {
                    (void)client->packetStream->sendRemoteEvent(std::move(event));
                } else if (client->id == localClientId_ && cg_.playerManager) {
                    (void)cg_.playerManager->processEvent(*event);
                }
            }
            return "True";
        });
    console.addCommand(
        0,
        "remotePlayAnim",
        [this](Console&, Int32, std::span<const std::string> args) -> std::optional<std::string> {
            auto* rep = args.size() == 3 && sg_.playerManager
                            ? dynamic_cast<PlayerManager::ClientRep*>(
                                  sg_.playerManager->findBaseRep(integer(args[1])))
                            : nullptr;
            auto* player = rep ? dynamic_cast<Player*>(rep->ownedObject) : nullptr;
            const Int32 wave = args.size() == 3 ? integer(args[2]) : -1;
            const auto* data =
                player ? dynamic_cast<const PlayerData*>(player->getDatPtr()) : nullptr;
            if (player && data && !player->isDead() && wave >= 0 && wave <= 12) {
                const auto animation = wave + 38;
                const auto current = player->serverAnimation();
                if (current != animation &&
                    (current < 0 || current >= Player::NUM_ANIMS ||
                     data->animData[static_cast<std::size_t>(current)].priority <
                         data->animData[static_cast<std::size_t>(animation)].priority)) {
                    player->setAnimation(static_cast<UInt32>(animation));
                }
            }
            return {};
        });
    console.addCommand(
        0,
        "listPlayers",
        [this](Console& con, Int32, std::span<const std::string>) -> std::optional<std::string> {
            const bool server = sg_.manager && sg_.playerManager;
            auto* players = server ? sg_.playerManager : cg_.playerManager;
            if (players) {
                con.print(std::string(server ? "Server: " : "Client: ") +
                          std::to_string(players->getNumClients()) + " clients");
                for (auto* rep = players->getClientList(); rep;
                     rep = static_cast<PlayerManager::ClientRep*>(rep->nextClient)) {
                    con.print(std::to_string(rep->id) +
                              (server
                                   ? " (" +
                                         (rep->packetStream ? rep->packetStream->transportAddress()
                                                            : std::string{}) +
                                         ")"
                                   : "") +
                              ": " + rep->name + " - " + rep->scoreString);
                }
            }
            return {};
        });
    console.addCommand(
        0,
        "remoteSelectCommander",
        [this](Console&, Int32, std::span<const std::string> args) -> std::optional<std::string> {
            if (args.size() == 3 && sg_.playerManager) {
                sg_.playerManager->selectCommander(integer(args[1]), integer(args[2]));
            }
            return {};
        });
    console.addCommand(0,
                       "Client::limitCommandBandwidth",
                       [this](Console&, Int32, std::span<const std::string> args) -> std::string {
                           auto* client = args.size() == 3 && sg_.playerManager
                                              ? sg_.playerManager->findClient(integer(args[1]))
                                              : nullptr;
                           if (!client || !client->playerPSC) {
                               return "false";
                           }
                           client->playerPSC->setCommandBandLimit(boolean(args[2]));
                           return "true";
                       });
    for (const bool voice : {false, true}) {
        console.addCommand(
            0,
            voice ? "Client::getVoiceBase" : "Client::getSkinBase",
            [this, voice](Console&, Int32, std::span<const std::string> args) -> std::string {
                auto* rep = args.size() == 2 && sg_.playerManager
                                ? sg_.playerManager->findBaseRep(integer(args[1]))
                                : nullptr;
                if (!rep) {
                    return {};
                }
                if (!voice) {
                    return rep->skinBase;
                }
                auto* client = dynamic_cast<PlayerManager::ClientRep*>(rep);
                return client ? client->voiceBase : std::string{};
            });
    }
    console.addCommand(0,
                       "Client::getTransportAddress",
                       [this](Console&, Int32, std::span<const std::string> args) -> std::string {
                           auto* client = args.size() == 2 && sg_.playerManager
                                              ? sg_.playerManager->findClient(integer(args[1]))
                                              : nullptr;
                           if (client && client->packetStream) {
                               return client->packetStream->transportAddress();
                           }
                           return client && client->id == localClientId_ &&
                                          connectAddress_ == "LOOPBACK:28001"
                                      ? connectAddress_
                                      : std::string{};
                       });
    console.addCommand(
        0,
        "Client::isItemBuyingOn",
        [this](Console&, Int32, std::span<const std::string> args) -> std::string {
            auto* client = args.size() == 3 && sg_.playerManager
                               ? sg_.playerManager->findClient(integer(args[1]))
                               : nullptr;
            if (!client || !client->playerPSC || !sg_.dbm) {
                return "False";
            }
            Int32 index = -1;
            if (!args[2].empty() && std::isdigit(static_cast<unsigned char>(args[2][0]))) {
                index = integer(args[2]);
            } else {
                (void)sg_.dbm->lookupDataBlock(args[2], DataBlockManager::ItemDataType, &index);
            }
            return index >= 0 && client->playerPSC->isItemBuyOn(index) ? "True" : "False";
        });
    console.addCommand(0,
                       "Client::setInitialTeam",
                       [this](Console&, Int32, std::span<const std::string> args) -> std::string {
                           auto* rep = args.size() == 3 && sg_.playerManager
                                           ? sg_.playerManager->findBaseRep(integer(args[1]))
                                           : nullptr;
                           const auto team = args.size() == 3 ? integer(args[2]) : -1;
                           if (!rep || team < 0 || team >= sg_.playerManager->getNumTeams()) {
                               return "False";
                           }
                           rep->team = team;
                           return "True";
                       });
    console.addCommand(0,
                       "Client::setSkin",
                       [this](Console&, Int32, std::span<const std::string> args) -> std::string {
                           if (args.size() != 3 || !sg_.playerManager) {
                               return "False";
                           }
                           sg_.playerManager->setClientSkin(integer(args[1]), args[2]);
                           return "True";
                       });
    console.addCommand(0,
                       "getClientByIndex",
                       [this](Console&, Int32, std::span<const std::string> args) -> std::string {
                           Int32 index = args.size() == 2 ? integer(args[1]) : -1;
                           if (!sg_.playerManager || index < 0 ||
                               index >= sg_.playerManager->getNumClients()) {
                               return "0";
                           }
                           for (auto* rep = sg_.playerManager->getClientList(); rep;
                                rep = static_cast<PlayerManager::ClientRep*>(rep->nextClient)) {
                               if (index-- == 0) {
                                   return std::to_string(rep->id);
                               }
                           }
                           return "0";
                       });
    console.addCommand(0,
                       "getClientByName",
                       [this](Console&, Int32, std::span<const std::string> args) -> std::string {
                           if (args.size() != 2 || !cg_.playerManager) {
                               return "0";
                           }
                           for (auto* rep = cg_.playerManager->getClientList(); rep;
                                rep = rep->nextClient) {
                               if (rep->name.size() == args[1].size() &&
                                   std::equal(rep->name.begin(),
                                              rep->name.end(),
                                              args[1].begin(),
                                              [](unsigned char a, unsigned char b) {
                                                  return std::tolower(a) == std::tolower(b);
                                              })) {
                                   return std::to_string(rep->id);
                               }
                           }
                           return "0";
                       });
    console.addCommand(0,
                       "resetPlayerManager",
                       [this](Console&, Int32, std::span<const std::string>) -> std::string {
                           if (sg_.playerManager) {
                               sg_.playerManager->missionReset();
                           }
                           return "True";
                       });
    console.addCommand(
        0,
        "resetGhostManagers",
        [this](Console&, Int32, std::span<const std::string>) -> std::string {
            if (auto* group = manager_ ? dynamic_cast<sim::SimSet*>(
                                             manager_->findObject(sim::GhostManagerSetId))
                                       : nullptr) {
                const std::vector<sim::SimObject*> objects(group->objects().begin(),
                                                           group->objects().end());
                for (auto* object : objects) {
                    if (auto* ghosts = dynamic_cast<sim::network::GhostManager*>(object)) {
                        ghosts->reset();
                    }
                }
            }
            return "True";
        });
    console.addCommand(0,
                       "Client::centerPrint",
                       [this](Console&, Int32, std::span<const std::string> args) -> std::string {
                           if (args.size() != 3) {
                               return "False";
                           }
                           centerPrintText_ = args[1];
                           centerPrintPosition_ = integer(args[2]);
                           return "True";
                       });
    auto masters = std::make_shared<MasterResolution>();
    console.addCommand(
        0,
        "DNet::resolveMasters",
        [masters](
            Console&, Int32, std::span<const std::string> args) -> std::optional<std::string> {
            *masters = {};
            if (args.size() < 2) {
                return {};
            }
            for (auto i :
                 std::views::iota(std::size_t{1}) | std::views::take_while([&](std::size_t i) {
                     return i < args.size() && i <= 10 && !args[i].empty();
                 })) {
                masters->addresses[masters->count++] = args[i].substr(0, 255);
            }
            masters->poll();
            return std::to_string(masters->count);
        });
    console.addCommand(
        0,
        "DNet::getResolvedMaster",
        [masters](Console&, Int32, std::span<const std::string> args) -> std::string {
            masters->poll();
            const auto index = args.size() == 2 ? integer(args[1]) : -1;
            return masters->resolved && index >= 0 && index < 10
                       ? masters->results[static_cast<std::size_t>(index)]
                       : std::string{};
        });
}
} // namespace opent1::game
