#include <opent1/game/fear_plugin.hpp>
#include <ranges>
#include <opent1/game/fear_player_psc.hpp>
#include <opent1/game/game_base.hpp>
#include <opent1/game/marker.hpp>
#include <opent1/game/moveable.hpp>
#include <opent1/game/player.hpp>
#include <opent1/game/player_manager.hpp>
#include <opent1/game/script_plugin.hpp>
#include <opent1/game/sensor_manager.hpp>
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

void FearPlugin::bindClientControlCallbacks() {
    if (!cg_.playerPSC) {
        return;
    }
    cg_.playerPSC->onGuiState =
        [this, lastMode = UInt32(PlayerPSC::InitialGuiMode)](UInt32 mode, bool scores) mutable {
            game_.console().setVariable("scoresVisible", scores ? "true" : "false");
            if (mode != lastMode && mode != PlayerPSC::InitialGuiMode && onGuiMode) {
                onGuiMode(mode);
            }
            lastMode = mode;
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
}

void FearPlugin::processPendingLocalDisconnect() {
    if (!pendingLocalKick_) {
        return;
    }
    const auto reason = std::move(*pendingLocalKick_);
    pendingLocalKick_.reset();
    if (connectAddress_ != "LOOPBACK:28001") {
        return;
    }
    (void)game_.console().execute("disconnect");
    game_.console().setVariable("ConnectedToServer", "false");
    game_.console().setVariable("errorString", reason);
    game_.withWorld(sim::WorldType::Client, [&] {
        if (game_.console().isFunction("onConnection")) {
            const std::array<std::string, 1> args{"Dropped"};
            (void)game_.console().execute("onConnection", args);
        }
    });
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
                for (auto* rep = sg_.playerManager->getClientList(); rep; rep = rep->nextClient) {
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
            for (auto suffix : std::views::iota(Int32{1}) |
                                   std::views::take_while([&]([[maybe_unused]] Int32 suffix) {
                                       return sameName(name);
                                   })) {
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
        // Native clients need the team skins before roster entries and ghosts:
        // static NPCs use the team directly, and AI team changes copy its skin.
        for (Int32 teamId = -1; teamId < sg_.playerManager->getNumTeams(); ++teamId) {
            const auto* team = sg_.playerManager->findTeam(teamId);
            if (!team) {
                return false;
            }
            auto event = std::make_unique<TeamAddEvent>(*team);
            event->flags = sim::SimEvent::Guaranteed | sim::SimEvent::Ordered;
            if (!stream.sendRemoteEvent(std::move(event))) {
                return false;
            }
        }
        const auto publish = [](PlayerManager::ClientRep& source,
                                sim::network::PacketStream& destination) {
            auto event = std::make_unique<PlayerAddEvent>();
            event->flags = sim::SimEvent::Guaranteed | sim::SimEvent::Ordered;
            event->id = source.id;
            event->isBaseRepType = source.isBaseRepType;
            event->name = source.name;
            event->skinBase = source.skinBase;
            event->voiceBase = source.voiceBase;
            event->gender = source.gender == "MALE";
            event->team = source.team;
            event->commanderId = source.commanderId;
            return destination.sendRemoteEvent(std::move(event));
        };
        for (auto* rep = sg_.playerManager->getBaseRepList(); rep; rep = rep->nextClient) {
            auto* other = static_cast<PlayerManager::ClientRep*>(rep);
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
        bindClientControlCallbacks();
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
            for (auto group : std::views::iota(Int32{0}) | std::views::take_while([&](Int32 group) {
                                  return group < DataBlockManager::NumDataTypes;
                              })) {
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
    console.addCommand(
        0,
        "startGhosting",
        [this](Console&, Int32, std::span<const std::string> args) -> std::string {
            if (args.size() == 2 && integer(args[1]) == localClientId_ &&
                connectAddress_ == "LOOPBACK:28001" && sg_.manager) {
                // The hosted client has no PacketStream to send CGADone.
                // Complete the same mission-drop handshake after the script
                // finishes publishing the new mission.
                if (auto* scheduler = sg_.manager->findObject(sim::ConsoleSchedulerId)) {
                    return sim::SimConsoleEvent::schedule(
                               *scheduler, std::format("remoteCGADone({});", localClientId_), 0.f)
                               ? "True"
                               : "False";
                }
                return "False";
            }
            auto* stream = args.size() == 2 && manager_
                               ? dynamic_cast<sim::network::PacketStream*>(
                                     manager_->findObject(integer(args[1])))
                               : nullptr;
            if (!stream || !stream->ghostManager()) {
                return "False";
            }
            if (sg_.sensorManager) {
                sg_.sensorManager->newClient(integer(args[1]));
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
                // PacketStream owns these objects and has already removed them.
                // Scripts may change GUI/control state or broadcast during the
                // disconnect hook, including to another peer lost this frame.
                client->packetStream = nullptr;
                client->playerPSC = nullptr;
                client->ghostManager = nullptr;
                client->eventManager = nullptr;
            }
        }
        if (disconnected.empty()) {
            return;
        }
        for (const auto peer : disconnected) {
            retireServerClient(peer);
        }
    }
}

void FearPlugin::retireServerClient(Int32 peer) {
    if (!sg_.manager || !sg_.playerManager || !sg_.playerManager->findClient(peer)) {
        return;
    }
    game_.withWorld(sim::WorldType::Server, [&] {
        auto* client = sg_.playerManager->findClient(peer);
        // Vanilla runs the hook while the representative, owned body and
        // observer still exist. A leaveGame hook may release/delete the body.
        if (game_.console().isFunction("Server::onClientDisconnect")) {
            const std::array<std::string, 1> args{std::to_string(peer)};
            (void)game_.console().execute("Server::onClientDisconnect", args);
        }
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
        if (auto* placeholder = sg_.manager->findObject(peer)) {
            placeholder->deleteObject();
        }
        pendingRemoteControls_.erase(peer);
    });
}

} // namespace opent1::game
