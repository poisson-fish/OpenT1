#pragma once
#include <opent1/core/types.hpp>
#include <array>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace opent1::net {
inline constexpr std::size_t MaxPayload = 1499; // Legacy VC asserts length < 1500.
inline constexpr std::size_t MaxHeader = 24;
inline constexpr std::size_t MaxDatagram = 1500 + MaxHeader;
inline constexpr std::size_t MaxAddress = 127;
enum class Result {
    Ok,
    WouldBlock,
    Closed,
    InvalidAddress,
    TooLarge,
    IoError,
    WindowFull,
    InvalidState,
    Capacity,
    UnsupportedTransport
};

struct Datagram {
    std::string address;
    std::array<std::byte, MaxDatagram> bytes{};
    std::size_t size{};

    [[nodiscard]] std::span<const std::byte> data() const {
        return std::span(bytes).first(size);
    }
};

// Datagram boundary for native UDP and future WebRTC/WebSocket adapters. No DNS,
// OS handles, sleeps or connection protocol in this interface. Single owner thread.
class Transport {
public:
    virtual ~Transport() = default;
    virtual Result send(std::string_view address, std::span<const std::byte> data) = 0;
    virtual Result receive(Datagram& packet) = 0; // WouldBlock means no packet now.
    // Must equal the address reported by receive for that peer. Empty rejects it.
    [[nodiscard]] virtual std::string canonicalAddress(std::string_view address) const;
    [[nodiscard]] virtual std::string localAddress() const = 0;
    virtual void close() = 0;
};

struct IPv4Address {
    std::array<UInt8, 4> host{};
    UInt16 port{};
    friend bool operator==(const IPv4Address&, const IPv4Address&) = default;
};

[[nodiscard]] std::optional<IPv4Address> parseIPv4Address(std::string_view address);
[[nodiscard]] std::string formatIPv4Address(const IPv4Address& address);
// Normalize an IPv4 or DNS endpoint without resolving it or blocking the caller.
[[nodiscard]] std::string canonicalIPv4Endpoint(std::string_view address);

class AddressLookup {
public:
    virtual ~AddressLookup() = default;
    // False while pending; an empty result on completion means resolution failed.
    virtual bool poll(std::string& result) const = 0;
};

// At most eight native DNS lookups may run concurrently. Destroying a lookup
// cancels its consumer without retaining a transport, world or callback.
[[nodiscard]] std::unique_ptr<AddressLookup> resolveIPv4AddressAsync(std::string_view address);
// Throws std::runtime_error on bind/setup failure. Port zero selects an ephemeral port.
[[nodiscard]] std::unique_ptr<Transport> makeUdpTransport(IPv4Address bindAddress = {});

struct IpxAddress {
    std::array<UInt8, 4> network{};
    std::array<UInt8, 6> node{};
    UInt16 port{};
    friend bool operator==(const IpxAddress&, const IpxAddress&) = default;
};

[[nodiscard]] std::optional<IpxAddress> parseIpxAddress(std::string_view);
[[nodiscard]] std::string formatIpxAddress(const IpxAddress&);
// Uses an actual OS IPX provider. Throws with the native setup error when absent.
[[nodiscard]] std::unique_ptr<Transport> makeIpxTransport(UInt16 port = 0);
// Native game session routing: UDP plus IPX when the OS provides it. Unsupported
// IPX sends return UnsupportedTransport; they are never tunneled as UDP packets.
[[nodiscard]] std::unique_ptr<Transport> makeGameTransport(IPv4Address bindAddress = {});
// Independent pair, copied FIFO datagrams, finite queue; no global active server.
[[nodiscard]] std::pair<std::unique_ptr<Transport>, std::unique_ptr<Transport>>
makeLoopTransportPair(std::size_t queueCapacity = 256);

using ConnectionId = UInt64;
enum class State { Unbound, Requesting, AwaitingDecision, Accepting, Connected };
enum class EventType {
    Requested,
    Accepted,
    Rejected,
    Connected,
    TimedOut,
    Disconnected,
    Data,
    Notify,
    Raw
};

struct Event {
    EventType type{};
    ConnectionId connection{};
    std::string address;
    UInt32 notifyKey{};
    bool delivered{};
    std::array<std::byte, MaxDatagram> bytes{};
    std::size_t size{};

    [[nodiscard]] std::span<const std::byte> data() const {
        return std::span(bytes).first(size);
    }
};

struct ConnectionInfo {
    State state{};
    UInt32 averageRtt{100};
    Flt32 packetLoss{};
    bool windowFull{};
};

struct SessionConfig {
    std::size_t maxConnections{64};     // 1..4096
    std::size_t eventCapacity{256};     // 64..65536; poll events to release backpressure.
    std::size_t packetsPerProcess{256}; // finite receive work per game tick
    UInt32 connectSequence{1};          // host should seed with its monotonic clock
};

// Borrows transport; it must outlive Session. Time is caller-supplied monotonic
// milliseconds modulo 2^32. Events own payloads; IDs remain safe after teardown.
class Session {
public:
    explicit Session(Transport& transport, SessionConfig config = {});
    ~Session();
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;
    Result connect(std::string_view address,
                   std::span<const std::byte> data,
                   UInt32 now,
                   ConnectionId& id);
    Result accept(ConnectionId id, std::span<const std::byte> data, UInt32 now);
    Result reject(ConnectionId id, std::span<const std::byte> data, UInt32 now);
    Result disconnect(ConnectionId id, std::span<const std::byte> data, UInt32 now);
    Result send(ConnectionId id, std::span<const std::byte> data, UInt32 notifyKey, UInt32 now);
    Result sendRaw(std::string_view address, std::span<const std::byte> data);
    Result process(UInt32 now);
    bool poll(Event& event);
    [[nodiscard]] std::optional<ConnectionInfo> info(ConnectionId id) const;
    [[nodiscard]] std::string address(ConnectionId id) const;
    [[nodiscard]] std::size_t connectionCount() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace opent1::net
