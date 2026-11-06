#pragma once
#include <opent1/console/console.hpp>
#include <opent1/game/ban_list.hpp>
#include <opent1/game/data_block_manager.hpp>
#include <opent1/game/version.hpp>
#include <opent1/game/server_irc_controls.hpp>
#include <opent1/sim/sim_net.hpp>
#include <opent1/sim/sim_world_game.hpp>
#include <opent1/world/scene_objects.hpp>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace opent1::game {

inline constexpr sim::SimObjectId SimCSDelegateId = sim::CSDelegateId;
inline constexpr UInt16 kStandardServerPort = 28001;
inline constexpr UInt16 kQueryMasterPort = 28000;
inline constexpr UInt16 kDefaultPacketVersion = 1;
inline constexpr std::string_view kFearGameString = "Tribes";

class FearCSDelegate final : public sim::SimObject {
public:
    struct JoinIdentity {
        std::string name;
        std::string voice;
        bool male{};
    };

    struct ServerConfig {
        std::string hostName{"TRIBES Server"};
        Int32 maxPlayers{8};
        std::string info;
        std::string password;
        std::string joinPassword;
        bool dedicated{};
        Int32 timeLimit{};
        Int32 currentMaster{};
        Int32 numMasters{};
        bool allowRCon{};
        std::string rconPassword;
        bool hostPublicGame{true};
        bool heartbeats{true};
        Int32 port{kStandardServerPort};
    };

    struct GameInfoResponse {
        UInt8 packetType{};
        UInt8 key{};
        std::string gameName{"Tribes"};
        std::string version{FearVersion};
        std::string hostName;
        bool dedicated{};
        bool passwordRequired{};
        Int32 playerCount{};
        Int32 maxPlayers{};
        Int32 cpuSpeed{};
        std::string modName;
        std::string serverType;
        std::string missionName;
        std::string serverInfo;
        Int32 numTeams{};
    };

    explicit FearCSDelegate(bool isServer = true,
                            std::shared_ptr<BanList> banList = {},
                            std::shared_ptr<const sim::network::Registry> registry = {},
                            std::unique_ptr<net::Transport> transport = {});
    ~FearCSDelegate() override = default;

    bool onAdd() override;
    void onRemove() override;
    bool processEvent(const sim::SimEvent& event) override;
    void onDeleteNotify(sim::SimObject&) override;

    [[nodiscard]] bool isServer() const noexcept {
        return isServer_;
    }

    [[nodiscard]] BanList& banList() noexcept {
        return *banList_;
    }

    [[nodiscard]] const BanList& banList() const noexcept {
        return *banList_;
    }

    [[nodiscard]] sim::network::CSDelegate* delegate() noexcept {
        return registeredDelegate_ ? registeredDelegate_ : delegate_.get();
    }

    [[nodiscard]] const sim::network::CSDelegate* delegate() const noexcept {
        return registeredDelegate_ ? registeredDelegate_ : delegate_.get();
    }

    void bindConsole(Console& console);
    void syncFromConsole();

    [[nodiscard]] ServerConfig& config() noexcept {
        return config_;
    }

    [[nodiscard]] const ServerConfig& config() const noexcept {
        return config_;
    }

    // Encodes a client connection request BitStream matching legacy FearCSDelegate::simConnect.
    [[nodiscard]] std::vector<std::byte>
    buildConnectRequestPacket(std::string_view joinPassword = {},
                              std::string_view playerName = "TribesPlayer",
                              std::string_view gender = "Male",
                              std::string_view voice = "male1",
                              std::string_view skinBase = "base",
                              std::string_view version = FearVersion,
                              std::string_view gameString = kFearGameString,
                              UInt16 packetVersion = kDefaultPacketVersion) const;

    // Validates a binary BitStream connection request matching legacy FearCSDelegate::onConnectionRequested.
    [[nodiscard]] bool validateConnectRequestPacket(std::string_view transportAddress,
                                                    std::span<const std::byte> packetBytes,
                                                    std::string& errorOut,
                                                    Int32 activeClients = 0,
                                                    JoinIdentity* identity = nullptr) const;

    // Convenience overload accepting a colon/tab/newline-delimited string or binary bytes.
    [[nodiscard]] bool validateConnectRequest(std::string_view transportAddress,
                                              std::string_view packetData,
                                              std::string& errorOut,
                                              Int32 activeClients = 0) const;

    // Configures a newly accepted server or client PacketStream with EventManager,
    // GhostManager, DataBlockManager sync, and SimPathManager sync.
    bool attachStreamServices(sim::network::PacketStream& stream, sim::SimObjectId peerId);

    bool kick(sim::SimObjectId playerId,
              std::string_view reason = "You were kicked from the server.",
              UInt32 now = 0);

    [[nodiscard]] GameInfoResponse buildGameInfoResponse(UInt8 packetType,
                                                         UInt8 key,
                                                         Int32 activePlayers = 0,
                                                         Int32 cpuSpeed = 200) const;
    [[nodiscard]] std::vector<std::byte>
    encodePingResponse(UInt8 queryType, UInt8 flags, UInt16 key, UInt32 pingNumber) const;
    [[nodiscard]] std::optional<UInt32> decodePingRequest(std::span<const std::byte> raw) const;
    [[nodiscard]] std::vector<std::byte> encodeBrowserGameInfo() const;
    bool receiveBrowserGameInfo(const sim::network::ServerBrowser::GameInfo&);

    [[nodiscard]] std::span<const ServerListCtrl::ServerInfo> discoveredServers() const {
        return browserServers_;
    }

    void addBrowserServer(ServerListCtrl::ServerInfo);
    bool setBrowserPreference(std::string_view address, bool favorite, bool neverPing);
    bool queryBrowserServer(std::string_view address, bool detail = false);
    void refreshBrowser(std::span<const std::string> masters, bool lan);
    bool saveServerDatabase(const std::filesystem::path&) const;

    [[nodiscard]] const std::vector<sim::SimObjectId>& connectedPeers() const noexcept {
        return connectedPeers_;
    }

    [[nodiscard]] const std::string& lastErrorReason() const noexcept {
        return lastErrorReason_;
    }

    [[nodiscard]] const std::optional<JoinIdentity>& joiningIdentity() const noexcept {
        return joiningIdentity_;
    }

    std::function<bool(sim::network::PacketStream&)> configureGameStream;
    std::function<void(const net::Event&)> onConnection;

private:
    void syncHeartbeat(UInt32 now);
    bool isServer_{true};
    std::shared_ptr<BanList> banList_;
    std::shared_ptr<const sim::network::Registry> registry_;
    std::unique_ptr<sim::network::CSDelegate> delegate_;
    sim::network::CSDelegate* registeredDelegate_{};
    Console* console_{};
    ServerConfig config_{};
    std::vector<sim::SimObjectId> connectedPeers_;
    std::string lastErrorReason_;
    std::optional<JoinIdentity> joiningIdentity_;
    std::vector<ServerListCtrl::ServerInfo> browserServers_;
    std::string individualPing_;
    std::vector<std::string> browserMasters_;
    std::size_t browserMasterIndex_{};
    bool browserMasterPending_{}, browserMasterTimedOut_{};
};

} // namespace opent1::game
