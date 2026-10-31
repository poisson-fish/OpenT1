#include <opent1/game/fear_plugin.hpp>
#include <opent1/game/fear_player_psc.hpp>
#include <opent1/game/game_base.hpp>
#include <opent1/game/marker.hpp>
#include <opent1/game/moveable.hpp>
#include <opent1/game/player.hpp>
#include <opent1/game/player_manager.hpp>
#include <opent1/game/script_plugin.hpp>
#include <opent1/game/observer_camera.hpp>
#include <charconv>
#include <algorithm>
#include <cctype>

namespace opent1::game {
namespace {
Int32 integer(std::string_view text) {
    Int32 value{};
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    return result.ec == std::errc{} && result.ptr == text.data() + text.size() ? value : -1;
}

bool sendCommand(sim::network::PacketStream& stream, std::vector<std::string> args) {
    auto event = std::make_unique<sim::SimConsoleEvent>();
    event->flags = sim::SimEvent::Guaranteed | sim::SimEvent::Ordered;
    event->arguments = std::move(args);
    return stream.sendRemoteEvent(std::move(event));
}
}

bool FearPlugin::configureRemoteStream(sim::network::PacketStream& stream) {
    auto* manager = stream.getManager();
    if (!manager || !stream.ghostManager()) {
        return false;
    }
    stream.prepareGhost = [manager](sim::SimNetObject& object) {
        if (auto* base = dynamic_cast<GameBase*>(&object)) {
            auto* blocks = dynamic_cast<DataBlockManager*>(manager->findObject(DataBlockManagerId));
            if (!blocks) {
                return false;
            }
            base->bindDataBlockManager(blocks);
            base->bindConsole(manager->console());
        }
        if (auto* moveable = dynamic_cast<MoveableBase*>(&object)) {
            moveable->bindPathManager(
                dynamic_cast<world::SimPathManager*>(manager->findObject(world::PathManagerId)));
        }
        return true;
    };
    if (!stream.setMaxRate(30, 450) || !stream.setPacketFrame(32)) {
        return false;
    }
    if (stream.isServer()) {
        if (!sg_.playerManager) {
            return false;
        }
        auto psc = std::make_unique<PlayerPSC>(true);
        auto* control = dynamic_cast<PlayerPSC*>(manager->addObject(std::move(psc)));
        if (!control || !stream.addObject(*control)) {
            return false;
        }
        if (!stream.reOrder(*control, stream.ghostManager())) {
            return false;
        }
        const auto id = stream.getId();
        if (!sg_.playerManager->findClient(id)) {
            const auto& identity = sg_.csDelegate->joiningIdentity();
            if (!identity) {
                return false;
            }
            std::string name = identity->name;
            const auto sameName = [&](std::string_view candidate) {
                for (auto* rep = sg_.playerManager->getBaseRepList(); rep; rep = rep->nextClient) {
                    if (rep->name.size() == candidate.size() &&
                        std::equal(rep->name.begin(),
                                   rep->name.end(),
                                   candidate.begin(),
                                   [](unsigned char a, unsigned char b) {
                                       return std::tolower(a) == std::tolower(b);
                                   })) {
                        return true;
                    }
                }
                return false;
            };
            for (Int32 suffix = 1; sameName(name); ++suffix) {
                name = identity->name + "." + std::to_string(suffix);
            }
            if (!sg_.playerManager->clientAdded(
                    name, identity->voice, "base", identity->male, id)) {
                return false;
            }
        }
        auto* client = sg_.playerManager->findClient(id);
        if (!client) {
            return false;
        }
        client->playerPSC = control;
        client->packetStream = &stream;
        client->ghostManager = stream.ghostManager();
        client->eventManager = stream.eventManager();
        control->bindConsole(&game_.console());
        if (scriptPlugin_) {
            auto& representative = scriptPlugin_->ensureClient(static_cast<Int32>(id));
            // Native PlayerManager::clientAdded creates every client's observer
            // before Server::onClientConnect and the initial mission drop.
            if (!dynamic_cast<ObserverCamera*>(
                    manager->findObject(representative.observerCameraId))) {
                auto camera = std::make_unique<ObserverCamera>(static_cast<Int32>(id));
                auto* added = manager->addObject(std::move(camera));
                if (!added) {
                    return false;
                }
                representative.observerCameraId = added->getId();
            }
        }
        const auto publish = [](PlayerManager::ClientRep& source,
                                sim::network::PacketStream& destination) {
            auto event = std::make_unique<PlayerAddEvent>();
            event->flags = sim::SimEvent::Guaranteed | sim::SimEvent::Ordered;
            event->id = source.id;
            event->name = source.name;
            event->skinBase = source.skinBase;
            event->voiceBase = source.voiceBase;
            event->gender = source.gender == "MALE";
            event->team = source.team;
            event->commanderId = source.commanderId;
            return destination.sendRemoteEvent(std::move(event));
        };
        for (auto* rep = sg_.playerManager->getBaseRepList(); rep; rep = rep->nextClient) {
            auto* other = sg_.playerManager->findClient(rep->id);
            if (!other) {
                continue;
            }
            if (!publish(*other, stream)) {
                return false;
            }
            if (other != client && other->packetStream && !publish(*client, *other->packetStream)) {
                return false;
            }
        }
        const std::array<std::string, 1> args{std::to_string(id)};
        game_.withWorld(sim::WorldType::Server, [&] {
            if (game_.console().isFunction("Server::onClientConnect")) {
                (void)game_.console().execute("Server::onClientConnect", args);
            }
        });
    } else {
        if (!cg_.playerPSC || !stream.addObject(*cg_.playerPSC) || !cg_.dbm) {
            return false;
        }
        if (!stream.reOrder(*cg_.playerPSC, stream.ghostManager())) {
            return false;
        }
        cg_.ghostManager = stream.ghostManager();
        cg_.playerPSC->bindConsole(&game_.console());
        cg_.playerPSC->onGuiState =
            [this, lastMode = UInt32(PlayerPSC::InitialGuiMode)](UInt32 mode, bool scores) mutable {
                game_.console().setVariable("scoresVisible", scores ? "true" : "false");
                if (mode != lastMode && mode != PlayerPSC::InitialGuiMode && onGuiMode) {
                    onGuiMode(mode);
                }
                lastMode = mode;
                // Vanilla servers send menu visibility in the PSC header. A score
                // toggle must update the dialog without reloading the play GUI.
                if (cg_.playerPSC->getControlObject() && onScoresVisible) {
                    onScoresVisible(scores);
                }
            };
        cg_.playerPSC->onControlObject = [this](GameBase* controlled) {
            if (controlled) {
                if (onPlayerControl) {
                    onPlayerControl();
                }
                if (onScoresVisible) {
                    onScoresVisible(cg_.playerPSC->scoresVisible());
                }
            }
        };
        stream.onGhostAlwaysDone = [this, address = stream.address()] {
            if (!cg_.manager) {
                return;
            }
            game_.withWorld(sim::WorldType::Client, [&] {
                if (game_.console().isFunction("onClientGhostAlwaysDone")) {
                    (void)game_.console().execute("onClientGhostAlwaysDone");
                } else if (auto* connection = dynamic_cast<sim::network::PacketStream*>(
                               address.resolve(*cg_.manager))) {
                    (void)sendCommand(*connection, {"CGADone"});
                }
            });
        };
        const auto address = stream.address();
        cg_.dbm->onDataFinished = [this, address] {
            if (!cg_.manager) {
                return;
            }
            dataFinishedFlag_ = true;
            downloadedBlocks_ = 0;
            for (Int32 group = 0; group < DataBlockManager::NumDataTypes; ++group) {
                downloadedBlocks_ += cg_.dbm->getDataGroupSize(group);
            }
            game_.console().setVariable("DataBlock::Finished", "true");
            if (auto* connection =
                    dynamic_cast<sim::network::PacketStream*>(address.resolve(*cg_.manager))) {
                if (!sendCommand(*connection, {"dataFinished"})) {
                    connection->fail("Cannot request mission ghosts");
                }
            }
        };
    }
    return true;
}

void FearPlugin::installRemoteCommands(Console& console) {
    console.addCommand(0,
                       "startGhosting",
                       [this](Console&, Int32, std::span<const std::string> args) -> std::string {
                           auto* stream = args.size() == 2 && manager_
                                              ? dynamic_cast<sim::network::PacketStream*>(
                                                    manager_->findObject(integer(args[1])))
                                              : nullptr;
                           if (!stream || !stream->ghostManager()) {
                               return "False";
                           }
                           stream->ghostManager()->activate();
                           return "True";
                       });
    console.addCommand(0,
                       "Client::setDataFinished",
                       [this](Console&, Int32, std::span<const std::string> args) -> std::string {
                           auto* client = args.size() == 2 && sg_.playerManager
                                              ? sg_.playerManager->findClient(integer(args[1]))
                                              : nullptr;
                           if (!client) {
                               return "False";
                           }
                           client->dataFinished[0] = "true";
                           return "True";
                       });
    // These defaults are replaced by server.cs when a mod supplies the normal
    // script handshake. Neither path requires an OpenT1-only remote function.
    console.addCommand(0,
                       "remoteDataFinished",
                       [this](Console&, Int32, std::span<const std::string> args) -> std::string {
                           auto* client = args.size() == 2 && sg_.playerManager
                                              ? sg_.playerManager->findClient(integer(args[1]))
                                              : nullptr;
                           if (!client || !client->ghostManager) {
                               return "False";
                           }
                           if (client->dataFinished[0].empty()) {
                               client->dataFinished[0] = "true";
                               client->ghostManager->activate();
                           }
                           return "True";
                       });
    console.addCommand(
        0,
        "remoteCGADone",
        [this](Console& con, Int32, std::span<const std::string> args) -> std::string {
            auto* client = args.size() == 2 && sg_.playerManager
                               ? sg_.playerManager->findClient(integer(args[1]))
                               : nullptr;
            if (!client || client->dataFinished[0].empty()) {
                return "False";
            }
            const std::array<std::string, 1> callback{args[1]};
            if (!con.isFunction("Game::initialMissionDrop")) {
                return "False";
            }
            bool success{};
            game_.withWorld(sim::WorldType::Server, [&] {
                success = con.execute("Game::initialMissionDrop", callback).ok();
            });
            return success ? "True" : "False";
        });
}

void FearPlugin::updateRemoteControls() {
    if (sg_.playerManager && sg_.manager) {
        std::vector<Int32> disconnected;
        for (auto* rep = sg_.playerManager->getBaseRepList(); rep; rep = rep->nextClient) {
            if (auto* client = sg_.playerManager->findClient(rep->id);
                client && client->packetStream &&
                sg_.manager->findObject(client->id) != client->packetStream) {
                disconnected.push_back(client->id);
            }
        }
        for (const auto peer : disconnected) {
            auto* client = sg_.playerManager->findClient(peer);
            const auto retire = [&](auto&& self, sim::SimGroup& group) -> bool {
                const std::vector<sim::SimObject*> objects(group.objects().begin(),
                                                           group.objects().end());
                for (auto* object : objects) {
                    if (object == client->ownedObject) {
                        object->deleteObject();
                        return true;
                    }
                    if (auto* child = dynamic_cast<sim::SimGroup*>(object);
                        child && self(self, *child)) {
                        return true;
                    }
                }
                return false;
            };
            if (client->ownedObject) {
                (void)retire(retire, *sg_.manager);
            }
            if (scriptPlugin_) {
                if (const auto* state = scriptPlugin_->findClient(peer)) {
                    if (auto* observer = dynamic_cast<ObserverCamera*>(
                            sg_.manager->findObject(state->observerCameraId))) {
                        observer->deleteObject();
                    }
                }
                scriptPlugin_->removeClient(peer);
            }
            sg_.playerManager->clientDropped(peer);
            pendingRemoteControls_.erase(peer);
        }
    }
}

} // namespace opent1::game
