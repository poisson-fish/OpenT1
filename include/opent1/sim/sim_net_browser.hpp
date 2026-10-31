#pragma once
#include <opent1/platform/net_transport.hpp>
#include <functional>
#include <vector>

namespace opent1::sim::network {
// Original master/game-server discovery protocol. Payloads stay application
// owned; no game globals, DNS lookup, implicit public hosting or background I/O.
class ServerBrowser {
public:
    enum class Type : UInt8 {
        PingQuery = 3,
        PingResponse = 4,
        Heartbeat = 5,
        MasterList = 6,
        GameQuery = 7,
        GameResponse = 8
    };

    struct Packet {
        Type type{};
        UInt8 number{255}, total{};
        UInt16 key{}, id{};
        std::vector<std::byte> data;
        [[nodiscard]] std::vector<std::byte> encode() const;
        [[nodiscard]] static std::optional<Packet> decode(std::span<const std::byte>);
    };

    struct PingInfo {
        std::string address, name;
        UInt8 gameType{254}, players{5}, maxPlayers{10};
        UInt32 ping{65535};
        UInt16 protocolVersion{};
    };

    struct GameInfo {
        std::string address;
        UInt16 key{};
        UInt32 ping{65535};
        std::vector<std::byte> data;
    };

    struct MasterInfo {
        std::string address, name, motd;
        bool first{};
    };

    struct Config {
        std::size_t capacity{4096}, concurrentPings{20}, concurrentRequests{10};
        UInt32 pingTimeout{1500}, requestTimeout{2000}, pingRetries{2}, requestRetries{2};
    };

    explicit ServerBrowser(net::Transport&, bool server);
    ServerBrowser(net::Transport&, bool server, Config);
    bool ping(std::string_view address);
    bool query(std::string_view address, UInt32 ping = 65535);
    void clear();
    void process(UInt32 now);
    // Returns true for recognized discovery packets, including malformed packets
    // which are discarded. Unknown protocols remain available to the raw handler.
    bool receive(std::string_view address, std::span<const std::byte>, UInt32 now);
    bool setHeartbeat(std::span<const std::string> masters, bool publicHost, UInt32 now);
    [[nodiscard]] std::size_t pending() const;
    std::function<PingInfo()> getPingInfo;
    // At most 64 response chunks, each <=1492 bytes. Legacy responses share a key.
    std::function<std::vector<std::vector<std::byte>>()> getGameInfo;
    std::function<void(const PingInfo&)> gotPingInfo;
    std::function<void(const GameInfo&)> gotGameInfo;
    std::function<void(const MasterInfo&)> gotMaster;
    std::function<void(std::string_view)> onTimeout;
    std::function<bool(std::string_view)> shouldPing;
    std::function<void(std::string_view, std::string_view)> resolvedAddress;

private:
    struct Request {
        std::string address;
        UInt8 number{};
        UInt16 key{};
        UInt32 sent{}, tries{}, ping{65535};
    };

    struct Response {
        std::string address;
        UInt16 key{};
        UInt32 sent{}, ping{};
    };

    struct Lookup {
        std::string address;
        std::unique_ptr<net::AddressLookup> lookup;
        bool query{};
        UInt32 ping{};
    };

    bool enqueue(std::vector<Request>&, std::string_view, UInt8, UInt32);
    net::Transport& transport_;
    bool server_{}, busy_{}, publicHost_{}, heartbeatSent_{};
    Config config_;
    UInt16 nextKey_{};
    UInt32 heartbeatTime_{};
    std::vector<Request> pings_, queries_, broadcasts_;
    std::vector<Response> responses_;
    std::vector<Lookup> lookups_;
    std::vector<std::string> completed_, masters_;
};
}
