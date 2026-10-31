#include <opent1/game/fear_cs_delegate.hpp>
#include <opent1/game/player_manager.hpp>
#include <opent1/core/file_stream.hpp>
#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <fstream>
#include <sstream>
#include <iomanip>

namespace opent1::game {
namespace {

constexpr UInt8 kPingVerifyByte = 0xF0U;

[[nodiscard]] Int32 parseIntAscii(std::string_view sv) noexcept {
    while (!sv.empty() && std::isspace(static_cast<unsigned char>(sv.front())) != 0) {
        sv.remove_prefix(1);
    }
    Int32 out = 0;
    (void)std::from_chars(sv.data(), sv.data() + sv.size(), out);
    return out;
}

} // namespace

FearCSDelegate::FearCSDelegate(bool isServer,
                               std::shared_ptr<BanList> banList,
                               std::shared_ptr<const sim::network::Registry> registry,
                               std::unique_ptr<net::Transport> transport)
    : isServer_(isServer), banList_(banList ? std::move(banList) : std::make_shared<BanList>()),
      registry_(std::move(registry)) {
    if (!registry_) {
        auto reg = std::make_shared<sim::network::Registry>();
        sim::network::registerConsoleEvent(*reg);
        sim::network::registerRemoteCreateEvent(*reg);
        sim::network::registerServiceGhosts(*reg);
        world::registerSceneObjectNetwork(*reg);
        registerDataBlockEvent(*reg);
        registry_ = std::move(reg);
    }
    if (transport) {
        delegate_ =
            std::make_unique<sim::network::CSDelegate>(std::move(transport), registry_, isServer_);
    }
}

bool FearCSDelegate::onAdd() {
    if (!sim::SimObject::onAdd()) {
        return false;
    }
    if (auto* mgr = getManager()) {
        if (delegate_) {
            delegate_->setId(sim::CSDelegateId);
            auto& browser = delegate_->browser();
            browser.getPingInfo = [this] {
                syncFromConsole();
                const auto* players = PlayerManager::get(getManager());
                return sim::network::ServerBrowser::PingInfo{
                    {},
                    config_.hostName,
                    0xf0,
                    static_cast<UInt8>(players ? players->getNumClients() : 0),
                    32,
                    0,
                    kDefaultPacketVersion};
            };
            browser.getGameInfo = [this] {
                syncFromConsole();
                return std::vector<std::vector<std::byte>>{encodeBrowserGameInfo()};
            };
            browser.shouldPing = [this](std::string_view address) {
                return std::ranges::none_of(browserServers_, [&](const auto& server) {
                    return server.transportAddress == address && server.neverPing;
                });
            };
            browser.resolvedAddress = [this](std::string_view original, std::string_view resolved) {
                for (auto& server : browserServers_) {
                    if (server.transportAddress == original) {
                        server.transportAddress = resolved;
                    }
                }
                if (individualPing_ == original) {
                    individualPing_ = resolved;
                }
                for (auto& master : browserMasters_) {
                    if (master == original) {
                        master = resolved;
                    }
                }
            };
            browser.gotPingInfo = [this](const auto& info) {
                if (info.gameType != 0xf0) {
                    return;
                }
                ServerListCtrl::ServerInfo server;
                server.transportAddress = info.address;
                server.name = info.name;
                server.pingTime = static_cast<Int32>(info.ping);
                server.numPlayers = info.players;
                server.maxPlayers = info.maxPlayers;
                server.status = ServerListCtrl::STAT_QUERYING;
                addBrowserServer(std::move(server));
                if (info.protocolVersion == kDefaultPacketVersion && delegate()) {
                    (void)delegate()->browser().query(info.address, info.ping);
                }
            };
            browser.gotGameInfo = [this](const auto& info) {
                (void)receiveBrowserGameInfo(info);
            };
            browser.onTimeout = [this](std::string_view address) {
                if (browserMasterPending_ &&
                    std::ranges::none_of(browserServers_, [&](const auto& server) {
                        return server.transportAddress == address;
                    })) {
                    ++browserMasterIndex_;
                    if (browserMasterIndex_ < browserMasters_.size() && delegate()) {
                        (void)delegate()->browser().ping(browserMasters_[browserMasterIndex_]);
                    } else {
                        browserMasterTimedOut_ = true;
                        browserMasterPending_ = false;
                    }
                }
                for (auto& server : browserServers_) {
                    if (server.transportAddress == address) {
                        server.status = ServerListCtrl::STAT_TIME_OUT;
                    }
                }
                std::erase_if(browserServers_, [&](const auto& server) {
                    return server.transportAddress == address && !server.favorite &&
                           !server.neverPing && address != individualPing_ &&
                           !browserMasterTimedOut_;
                });
            };
            browser.gotMaster = [this](const auto& info) {
                browserMasterPending_ = false;
                if (console_) {
                    console_->setVariable("pref::MSName", info.name);
                    console_->setVariable("pref::MSMOTD", info.motd);
                }
            };

            delegate_->clock = [this] {
                return getManager() ? UInt32(getManager()->getCurrentTime() * 1000.0f) : 0u;
            };
            delegate_->acceptAddressedRequest = [this](auto address, auto bytes) {
                syncFromConsole();
                joiningIdentity_.reset();
                JoinIdentity identity;
                if (!validateConnectRequestPacket(address,
                                                  bytes,
                                                  lastErrorReason_,
                                                  Int32(connectedPeers_.size()),
                                                  &identity)) {
                    return false;
                }
                joiningIdentity_ = std::move(identity);
                return true;
            };
            delegate_->configureStream = [this](auto& stream) {
                if (!attachStreamServices(stream, stream.getId())) {
                    throw std::runtime_error("Cannot attach game stream services");
                }
                if (configureGameStream && !configureGameStream(stream)) {
                    throw std::runtime_error("Cannot attach player stream services");
                }
            };
            delegate_->onConnection = [this](const auto& event) {
                if (!isServer_ && event.type == net::EventType::Accepted && getManager()) {
                    if (auto* dbm = dynamic_cast<DataBlockManager*>(
                            getManager()->findObject(DataBlockManagerId))) {
                        dbm->reset();
                    }
                    if (auto* players = PlayerManager::get(getManager())) {
                        players->reset();
                    }
                    auto* stream = delegate() ? delegate()->stream(2048) : nullptr;
                    const auto filename =
                        console_ ? console_->getVariable("recorderFileName") : std::string{};
                    if (stream && !filename.empty() &&
                        console_->getVariable("playingDemo") != "true") {
                        stream->onRecordingFinished = [filename](auto bytes) {
                            FileWStream output{std::filesystem::path(filename)};
                            if (output.getStatus() != STRM_OK || !output.writeBytes(bytes)) {
                                throw std::runtime_error("Cannot save game recording");
                            }
                        };
                        if (!stream->startRecording(
                                sim::network::PacketStream::DemoTiming::Timestamped,
                                static_cast<UInt32>(getManager()->getCurrentTime() * 1000.0f))) {
                            stream->fail("Cannot start game recording");
                        }
                    }
                    if (console_ && console_->isFunction("onConnection")) {
                        const std::array<std::string, 1> accepted{"Accepted"};
                        (void)console_->execute("onConnection", accepted);
                    }
                }
                if (event.type == net::EventType::Disconnected ||
                    event.type == net::EventType::TimedOut ||
                    event.type == net::EventType::Rejected) {
                    lastErrorReason_.assign(reinterpret_cast<const char*>(event.bytes.data()),
                                            event.size);
                    if (const auto end = lastErrorReason_.find('\0'); end != std::string::npos) {
                        lastErrorReason_.resize(end);
                    }
                    if (lastErrorReason_.empty()) {
                        lastErrorReason_ = event.type == net::EventType::TimedOut
                                               ? "Connection to server timed out."
                                           : event.type == net::EventType::Rejected
                                               ? "Connection to server rejected."
                                               : "Connection to server lost.";
                    }
                    if (console_ && !isServer_) {
                        console_->setVariable("OpenT1::LastNetworkError", lastErrorReason_);
                        console_->setVariable("errorString", lastErrorReason_);
                    }
                    std::erase_if(connectedPeers_, [this](auto peer) {
                        return !delegate() || !delegate()->stream(peer);
                    });
                }
                if (onConnection) {
                    onConnection(event);
                }
                if (!isServer_ && console_ && console_->isFunction("onConnection") &&
                    (event.type == net::EventType::Disconnected ||
                     event.type == net::EventType::TimedOut ||
                     event.type == net::EventType::Rejected)) {
                    const std::array<std::string, 1> message{
                        event.type == net::EventType::TimedOut   ? "TimedOut"
                        : event.type == net::EventType::Rejected ? "Rejected"
                                                                 : "Dropped"};
                    (void)console_->execute("onConnection", message);
                }
            };
            registeredDelegate_ =
                dynamic_cast<sim::network::CSDelegate*>(mgr->addObject(std::move(delegate_)));
            if (!registeredDelegate_) {
                return false;
            }
            deleteNotify(*registeredDelegate_);
        }
        if (mgr->console() && !console_) {
            bindConsole(*mgr->console());
        }
        if (!mgr->findObject(DataBlockManagerId)) {
            mgr->addObject(std::make_unique<DataBlockManager>(isServer_));
        }
        if (!mgr->findObject(world::PathManagerId)) {
            mgr->addObject(std::make_unique<world::SimPathManager>());
        }
    }
    return true;
}

void FearCSDelegate::onRemove() {
    if (registeredDelegate_) {
        auto* native = registeredDelegate_;
        registeredDelegate_ = nullptr;
        native->deleteObject();
    }
    connectedPeers_.clear();
    sim::SimObject::onRemove();
}

bool FearCSDelegate::processEvent(const sim::SimEvent& event) {
    if (auto* native = delegate()) {
        return native->processEvent(event);
    }
    return sim::SimObject::processEvent(event);
}

void FearCSDelegate::onDeleteNotify(sim::SimObject& object) {
    if (&object == registeredDelegate_) {
        registeredDelegate_ = nullptr;
    }
    sim::SimObject::onDeleteNotify(object);
}

[[nodiscard]] Int32
readConsoleInt(const Console& console, std::string_view name, Int32 fallback) noexcept {
    if (!console.hasVariable(name)) {
        return fallback;
    }
    const auto str = console.getVariable(name);
    if (str.empty()) {
        return fallback;
    }
    return parseIntAscii(str);
}

[[nodiscard]] bool
readConsoleBool(const Console& console, std::string_view name, bool fallback) noexcept {
    if (!console.hasVariable(name)) {
        return fallback;
    }
    const auto str = console.getVariable(name);
    if (str.empty()) {
        return fallback;
    }
    if (str == "true" || str == "True" || str == "TRUE") {
        return true;
    }
    if (str == "false" || str == "False" || str == "FALSE") {
        return false;
    }
    return parseIntAscii(str) != 0;
}

void FearCSDelegate::bindConsole(Console& console) {
    console_ = &console;
    const auto setDefault = [&console](std::string_view name, std::string_view value) {
        if (!console.hasVariable(name)) {
            console.setVariable(name, value);
        }
    };
    setDefault("Server::HostName", config_.hostName);
    setDefault("Server::MaxPlayers", std::to_string(config_.maxPlayers));
    setDefault("Server::Info", config_.info);
    setDefault("Server::Password", config_.password);
    setDefault("Server::JoinPassword", config_.joinPassword);
    setDefault("Server::Dedicated", config_.dedicated ? "true" : "false");
    setDefault("Server::TimeLimit", std::to_string(config_.timeLimit));
    setDefault("Server::CurrentMaster", std::to_string(config_.currentMaster));
    setDefault("Server::numMasters", std::to_string(config_.numMasters));
    setDefault("Server::AllowRCon", config_.allowRCon ? "true" : "false");
    setDefault("Server::RConPassword", config_.rconPassword);
    setDefault("Server::HostPublicGame", config_.hostPublicGame ? "true" : "false");
    setDefault("Server::Port", std::to_string(config_.port));
    syncFromConsole();
}

void FearCSDelegate::syncFromConsole() {
    if (!console_) {
        return;
    }
    config_.hostName = console_->getVariable("Server::HostName");
    config_.maxPlayers = readConsoleInt(*console_, "Server::MaxPlayers", config_.maxPlayers);
    config_.info = console_->getVariable("Server::Info");
    config_.password = console_->getVariable("Server::Password");
    config_.joinPassword = console_->getVariable("Server::JoinPassword");
    config_.dedicated = readConsoleBool(*console_, "Server::Dedicated", config_.dedicated);
    config_.timeLimit = readConsoleInt(*console_, "Server::TimeLimit", config_.timeLimit);
    config_.currentMaster =
        readConsoleInt(*console_, "Server::CurrentMaster", config_.currentMaster);
    config_.numMasters = readConsoleInt(*console_, "Server::numMasters", config_.numMasters);
    config_.allowRCon = readConsoleBool(*console_, "Server::AllowRCon", config_.allowRCon);
    config_.rconPassword = console_->getVariable("Server::RConPassword");
    config_.hostPublicGame =
        readConsoleBool(*console_, "Server::HostPublicGame", config_.hostPublicGame);
    config_.port = readConsoleInt(*console_, "Server::Port", config_.port);
}

std::vector<std::byte> FearCSDelegate::buildConnectRequestPacket(std::string_view joinPassword,
                                                                 std::string_view playerName,
                                                                 std::string_view gender,
                                                                 std::string_view voice,
                                                                 std::string_view skinBase,
                                                                 std::string_view version,
                                                                 std::string_view gameString,
                                                                 UInt16 packetVersion) const {
    std::array<std::byte, 1024> buf{};
    BitStream bs(std::span<std::byte>(buf.data(), buf.size()));
    const std::array<std::byte, 2> verBytes{
        static_cast<std::byte>(packetVersion & 0xFFU),
        static_cast<std::byte>((packetVersion >> 8) & 0xFFU),
    };
    bs.writeBytes(verBytes);
    bs.writeString(gameString);
    bs.writeString(version);
    bs.writeString(joinPassword);
    bs.writeString(playerName);
    bs.writeString(gender);
    bs.writeString(voice);
    bs.writeString(skinBase);

    const std::size_t byteLen = static_cast<std::size_t>(bs.getPosition());
    return std::vector<std::byte>(buf.begin(), buf.begin() + static_cast<std::ptrdiff_t>(byteLen));
}

bool FearCSDelegate::validateConnectRequestPacket(std::string_view transportAddress,
                                                  std::span<const std::byte> packetBytes,
                                                  std::string& errorOut,
                                                  Int32 activeClients,
                                                  JoinIdentity* identity) const {
    if (banList_ && banList_->isBanned(transportAddress)) {
        errorOut = "You are banned from this server.";
        return false;
    }
    if (!isServer_) {
        errorOut = "Cannot connect to a client.";
        return false;
    }
    BitStream bs(packetBytes);
    std::array<std::byte, 2> verBytes{};
    if (!bs.readBytes(verBytes) || !bs.isValid()) {
        errorOut = "Wrong packet version.";
        return false;
    }
    const UInt16 packetVersion =
        static_cast<UInt16>(std::to_integer<UInt8>(verBytes[0])) |
        static_cast<UInt16>(static_cast<UInt16>(std::to_integer<UInt8>(verBytes[1])) << 8);
    if (packetVersion != kDefaultPacketVersion) {
        errorOut = "Wrong packet version.";
        return false;
    }

    std::string gameStr;
    if (!bs.readString(gameStr) || gameStr != kFearGameString) {
        errorOut = "Wrong game type.";
        return false;
    }

    std::string verStr;
    if (!bs.readString(verStr)) {
        errorOut = "Wrong game version.";
        return false;
    }
    const auto dotPos = verStr.find('.');
    if (dotPos == std::string::npos ||
        verStr.substr(0, dotPos) != FearVersion.substr(0, FearVersion.find('.'))) {
        errorOut = "Wrong game version.";
        return false;
    }
    const std::string minorStr = verStr.substr(dotPos + 1, 2);
    const int minorVer = parseIntAscii(minorStr);
    if (minorVer < 11) {
        errorOut = "You must upgrade your executable to play on this server (requires version 1.11 "
                   "or higher).  Check www.tribesplayers.com for the most recent patches.";
        return false;
    }

    std::string pwd;
    if (!bs.readString(pwd)) {
        errorOut = "Wrong password.";
        return false;
    }
    const std::string expectedPwd =
        console_ ? std::string(console_->getVariable("Server::Password")) : config_.password;
    if (!expectedPwd.empty() && pwd != expectedPwd) {
        errorOut = "Wrong password.";
        return false;
    }

    Int32 maxClients = console_
                           ? readConsoleInt(*console_, "Server::MaxPlayers", config_.maxPlayers)
                           : config_.maxPlayers;
    if (maxClients > 128) {
        maxClients = 128;
    }
    if (activeClients >= maxClients) {
        errorOut = "Server is full.";
        return false;
    }
    JoinIdentity parsed;
    std::string gender;
    if (!bs.readString(parsed.name) || !bs.readString(gender) || !bs.readString(parsed.voice) ||
        !bs.isValid()) {
        errorOut = "Invalid player identity.";
        return false;
    }
    parsed.name.resize(std::min<std::size_t>(parsed.name.size(), 16));
    for (auto& c : parsed.name) {
        const auto byte = static_cast<unsigned char>(c);
        if (byte < 32 || byte > 127 || c == '\\' || c == '"' || c == '~') {
            c = '*';
        }
    }
    std::ranges::transform(gender, gender.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    parsed.male = gender == "male";
    if (identity) {
        *identity = std::move(parsed);
    }
    errorOut.clear();
    return true;
}

bool FearCSDelegate::validateConnectRequest(std::string_view transportAddress,
                                            std::string_view packetData,
                                            std::string& errorOut,
                                            Int32 activeClients) const {
    const auto bytes = std::as_bytes(std::span(packetData.data(), packetData.size()));
    return validateConnectRequestPacket(transportAddress, bytes, errorOut, activeClients);
}

bool FearCSDelegate::attachStreamServices(sim::network::PacketStream& stream,
                                          sim::SimObjectId peerId) {
    auto* mgr = getManager();
    if (!mgr) {
        return false;
    }
    if (!stream.eventManager()) {
        auto ev = std::make_unique<sim::network::EventManager>();
        if (auto* addedEv = mgr->addObject(std::move(ev))) {
            stream.addObject(*addedEv);
        }
    }
    if (!stream.ghostManager()) {
        auto gm = std::make_unique<sim::network::GhostManager>(true);
        if (auto* addedGm = mgr->addObject(std::move(gm))) {
            stream.addObject(*addedGm);
        }
    }
    if (isServer_) {
        if (auto* dbm = dynamic_cast<DataBlockManager*>(mgr->findObject(DataBlockManagerId))) {
            if (!dbm->sendDataToStream(stream)) {
                return false;
            }
        }
        if (auto* pm =
                dynamic_cast<world::SimPathManager*>(mgr->findObject(world::PathManagerId))) {
            if (!pm->sync(stream)) {
                return false;
            }
        }
    }
    if (std::ranges::find(connectedPeers_, peerId) == connectedPeers_.end()) {
        connectedPeers_.push_back(peerId);
    }
    return true;
}

bool FearCSDelegate::kick(sim::SimObjectId playerId, std::string_view reason, UInt32 now) {
    lastErrorReason_ = std::string(reason);
    const auto it = std::ranges::find(connectedPeers_, playerId);
    if (it != connectedPeers_.end()) {
        connectedPeers_.erase(it);
    }
    if (auto* native = delegate()) {
        const auto bytes = std::as_bytes(std::span(reason.data(), reason.size()));
        return native->disconnect(playerId, now, bytes);
    }
    if (auto* mgr = getManager()) {
        if (auto* ps = dynamic_cast<sim::network::PacketStream*>(mgr->findObject(playerId))) {
            const auto bytes = std::as_bytes(std::span(reason.data(), reason.size()));
            ps->disconnect(now, bytes);
            return true;
        }
    }
    return it != connectedPeers_.end();
}

FearCSDelegate::GameInfoResponse FearCSDelegate::buildGameInfoResponse(UInt8 packetType,
                                                                       UInt8 key,
                                                                       Int32 activePlayers,
                                                                       Int32 cpuSpeed) const {
    GameInfoResponse resp{};
    resp.packetType = packetType;
    resp.key = key;
    resp.gameName = std::string(kFearGameString);
    resp.version = std::string(FearVersion);
    resp.hostName = console_ ? console_->getVariable("Server::HostName") : config_.hostName;
    resp.dedicated = console_ ? readConsoleBool(*console_, "Server::Dedicated", config_.dedicated)
                              : config_.dedicated;
    const std::string pwd = console_ ? console_->getVariable("Server::Password") : config_.password;
    resp.passwordRequired = !pwd.empty();
    resp.playerCount = activePlayers;
    resp.maxPlayers = console_ ? readConsoleInt(*console_, "Server::MaxPlayers", config_.maxPlayers)
                               : config_.maxPlayers;
    resp.cpuSpeed =
        console_ ? readConsoleInt(*console_, "CPU::estimatedSpeed", cpuSpeed) : cpuSpeed;
    resp.modName = console_ ? std::string(console_->getVariable("modList")) : std::string{};
    resp.serverType =
        console_ ? std::string(console_->getVariable("Game::MissionType")) : std::string{};
    resp.missionName = console_ ? std::string(console_->getVariable("MissionName")) : std::string{};
    resp.serverInfo = console_ ? std::string(console_->getVariable("Server::Info")) : config_.info;
    return resp;
}

std::vector<std::byte> FearCSDelegate::encodePingResponse(UInt8 queryType,
                                                          UInt8 flags,
                                                          UInt16 key,
                                                          UInt32 pingNumber) const {
    return std::vector<std::byte>{
        static_cast<std::byte>(queryType),
        static_cast<std::byte>(flags),
        static_cast<std::byte>(key & 0xFFU),
        static_cast<std::byte>((key >> 8) & 0xFFU),
        static_cast<std::byte>(kPingVerifyByte),
        static_cast<std::byte>(pingNumber & 0xFFU),
        static_cast<std::byte>((pingNumber >> 8) & 0xFFU),
        static_cast<std::byte>((pingNumber >> 16) & 0xFFU),
        static_cast<std::byte>((pingNumber >> 24) & 0xFFU),
    };
}

std::optional<UInt32> FearCSDelegate::decodePingRequest(std::span<const std::byte> raw) const {
    if (raw.size() < 9 || std::to_integer<UInt8>(raw[4]) != kPingVerifyByte) {
        return std::nullopt;
    }
    const UInt32 num = static_cast<UInt32>(std::to_integer<UInt8>(raw[5])) |
                       (static_cast<UInt32>(std::to_integer<UInt8>(raw[6])) << 8) |
                       (static_cast<UInt32>(std::to_integer<UInt8>(raw[7])) << 16) |
                       (static_cast<UInt32>(std::to_integer<UInt8>(raw[8])) << 24);
    return num;
}

std::vector<std::byte> FearCSDelegate::encodeBrowserGameInfo() const {
    std::array<std::byte, 1492> data{};
    BitStream stream{std::span<std::byte>(data)};
    const auto* players = PlayerManager::get(getManager());
    const auto value = [this](std::string_view name) {
        return console_ ? console_->getVariable(name) : std::string{};
    };
    stream.write(UInt16(kDefaultPacketVersion));
    stream.writeString(kFearGameString);
    stream.writeString(FearVersion);
    stream.writeString(config_.hostName.empty() ? "N/A" : config_.hostName);
    stream.write(UInt8(players ? players->getNumClients() : 0));
    stream.write(UInt8(config_.maxPlayers));
    const auto mission = value("MissionName");
    stream.writeString(mission.empty() ? "N/A" : mission);
    stream.write(UInt8(config_.dedicated));
    stream.write(UInt8(!config_.password.empty()));
    const auto cpu = value("CPU::estimatedSpeed");
    stream.write(Int32(cpu.empty() ? -1 : parseIntAscii(cpu)));
    stream.writeString(value("modList"));
    stream.writeString(value("Game::MissionType"));
    stream.writeString(config_.info);
    stream.writeString(players ? players->teamScoreHeading() : std::string_view{});
    stream.writeString(players ? players->clientScoreHeading() : std::string_view{});
    if (!stream.isValid()) {
        return {};
    }
    const auto line = [&](std::string_view text) {
        if (stream.remainingWriteBits() < (text.size() + 2) * 8) {
            return false;
        }
        return bool(stream.writeString(text));
    };
    if (players) {
        const auto substitute = [](std::string_view text,
                                   const std::vector<std::pair<char, std::string>>& values) {
            std::string out;
            for (std::size_t i = 0; i < text.size() && out.size() < 253; ++i) {
                if (text[i] == '%' && i + 1 < text.size()) {
                    const auto v = std::ranges::find(
                        values, text[i + 1], &std::pair<char, std::string>::first);
                    if (v != values.end()) {
                        out += v->second;
                        ++i;
                        continue;
                    }
                }
                out += text[i];
            }
            return out.substr(0, 253);
        };
        if (!players->teamScoreHeading().empty()) {
            (void)line("00");
            (void)line("01" + std::string(players->teamScoreHeading()));
            std::vector<const PlayerManager::TeamRep*> teams;
            for (Int32 i = 0; i < players->getNumTeams(); ++i) {
                if (const auto* team = players->findTeam(i)) {
                    teams.push_back(team);
                }
            }
            std::stable_sort(teams.begin(), teams.end(), [](auto* a, auto* b) {
                return a->scoreSortValue > b->scoreSortValue;
            });
            for (const auto* team : teams) {
                if (!line("00" + substitute(team->scoreString, {{'t', team->name}}))) {
                    break;
                }
            }
        }
        (void)line("00");
        (void)line("11" + std::string(players->clientScoreHeading()));
        std::vector<const PlayerManager::ClientRep*> clients;
        for (auto* c = players->getClientList(); c;
             c = static_cast<PlayerManager::ClientRep*>(c->nextClient)) {
            clients.push_back(c);
        }
        std::stable_sort(clients.begin(), clients.end(), [](auto* a, auto* b) {
            return a->scoreSortValue > b->scoreSortValue;
        });
        for (const auto* c : clients) {
            const auto* team = players->findTeam(c->team);
            if (!line("10" + substitute(c->scoreString,
                                        {{'n', c->name},
                                         {'t', team ? team->name : ""},
                                         {'p', std::to_string(c->ping)},
                                         {'l', std::to_string(Int32(c->packetLoss * 100))}}))) {
                break;
            }
        }
    }
    return {data.begin(), data.begin() + stream.getPosition()};
}

bool FearCSDelegate::receiveBrowserGameInfo(const sim::network::ServerBrowser::GameInfo& info) {
    BitStream stream{std::span<const std::byte>(info.data)};
    UInt16 version{};
    std::string game;
    ServerListCtrl::ServerInfo server;
    UInt8 players{}, maximum{}, dedicated{}, password{};
    if (!stream.read(version) || version != kDefaultPacketVersion || !stream.readString(game) ||
        game != kFearGameString || !stream.readString(server.version) ||
        !stream.readString(server.name) || !stream.read(players) || !stream.read(maximum) ||
        !stream.readString(server.missionName) || !stream.read(dedicated) ||
        !stream.read(password) || !stream.read(server.cpuSpeed) ||
        !stream.readString(server.modType) || !stream.readString(server.missionType) ||
        !stream.readString(server.hostInfo) || !stream.readString(server.teamHeading) ||
        !stream.readString(server.playerHeading)) {
        return false;
    }
    server.transportAddress = info.address;
    server.pingTime = static_cast<Int32>(info.ping);
    server.numPlayers = players;
    server.maxPlayers = maximum;
    server.dedicated = dedicated != 0;
    server.password = password != 0;
    server.status = ServerListCtrl::STAT_RESPONDING;
    while (static_cast<std::size_t>(stream.getPosition()) < info.data.size()) {
        std::string line;
        if (!stream.readString(line) || line.size() < 2) {
            return false;
        }
        server.detailLines.push_back(std::move(line));
    }
    if (!stream.isValid()) {
        return false;
    }
    if (console_) {
        std::istringstream buddies(console_->getVariable("pref::buddyList"));
        std::string buddy;
        while (buddies >> std::quoted(buddy)) {
            if (buddy.empty()) {
                continue;
            }
            std::ranges::transform(buddy, buddy.begin(), [](unsigned char c) {
                return char(std::toupper(c));
            });
            for (auto line : server.detailLines) {
                std::ranges::transform(line, line.begin(), [](unsigned char c) {
                    return char(std::toupper(c));
                });
                if (line.find(buddy) != std::string::npos) {
                    server.hasBuddy = true;
                    break;
                }
            }
        }
    }
    addBrowserServer(std::move(server));
    return true;
}

void FearCSDelegate::addBrowserServer(ServerListCtrl::ServerInfo server) {
    if (delegate()) {
        const auto normalized =
            delegate()->translateAddress(server.transportAddress, kStandardServerPort);
        if (!normalized.empty()) {
            server.transportAddress = normalized;
        }
    }
    const auto existing = std::ranges::find(
        browserServers_, server.transportAddress, &ServerListCtrl::ServerInfo::transportAddress);
    if (existing != browserServers_.end()) {
        server.favorite = existing->favorite;
        server.neverPing = existing->neverPing;
        *existing = std::move(server);
    } else if (browserServers_.size() < 4096) {
        browserServers_.push_back(std::move(server));
    }
}

bool FearCSDelegate::setBrowserPreference(std::string_view address, bool favorite, bool neverPing) {
    const auto found =
        std::ranges::find(browserServers_, address, &ServerListCtrl::ServerInfo::transportAddress);
    if (found == browserServers_.end()) {
        return false;
    }
    found->favorite = favorite;
    found->neverPing = neverPing;
    return true;
}

bool FearCSDelegate::queryBrowserServer(std::string_view address, bool detail) {
    if (!delegate()) {
        return false;
    }
    const auto normalized = delegate()->translateAddress(address, kStandardServerPort);
    if (normalized.empty()) {
        return false;
    }
    individualPing_ = normalized;
    browserMasterPending_ = false;
    return detail ? delegate()->browser().query(normalized)
                  : delegate()->browser().ping(normalized);
}

void FearCSDelegate::refreshBrowser(std::span<const std::string> masters, bool lan) {
    if (!delegate()) {
        return;
    }
    auto& browser = delegate()->browser();
    browser.clear();
    individualPing_.clear();
    (void)browser.ping("IP:255.255.255.255:" + std::to_string(config_.port));
    (void)browser.ping("IPX:broadcast:" + std::to_string(config_.port));
    browserMasters_.assign(masters.begin(), masters.end());
    browserMasterIndex_ = 0;
    browserMasterTimedOut_ = false;
    browserMasterPending_ = !lan && !browserMasters_.empty();
    if (browserMasterPending_) {
        (void)browser.ping(browserMasters_.front());
    }
    for (auto& server : browserServers_) {
        if (!server.neverPing) {
            server.status = ServerListCtrl::STAT_QUERYING;
            (void)browser.ping(server.transportAddress);
        }
    }
}

bool FearCSDelegate::saveServerDatabase(const std::filesystem::path& path) const {
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    if (error) {
        return false;
    }
    std::ofstream file(path, std::ios::binary);
    if (!file) {
        return false;
    }
    const auto quote = [](std::string_view text) {
        std::string out = "\"";
        for (const char c : text) {
            if (c == '\\' || c == '\"') {
                out += '\\';
                out += c;
            } else if (c == '\n') {
                out += "\\n";
            } else if (c == '\r') {
                out += "\\r";
            } else {
                out += c;
            }
        }
        return out + '\"';
    };
    file << "// Database of GAME servers\n";
    for (const auto& server : browserServers_) {
        file << "addGameServer(" << quote(server.transportAddress) << ", " << quote(server.name)
             << ", " << quote(server.version) << ", " << quote(std::to_string(server.pingTime))
             << ", " << quote(server.favorite ? "TRUE" : "FALSE") << ", " << quote(server.modType)
             << ", " << quote(server.missionName) << ", " << quote(server.missionType) << ", "
             << quote(std::to_string(server.cpuSpeed)) << ", "
             << quote(server.neverPing ? "TRUE" : "FALSE") << ");\n";
    }
    return bool(file);
}

} // namespace opent1::game
