#pragma once
#include <opent1/sim/sim_base.hpp>
#include <ranges>
#include <opent1/math/point.hpp>
#include <opent1/platform/net_transport.hpp>
#include <opent1/sim/sim_net_browser.hpp>
#include <deque>
#include <map>
#include <typeindex>

namespace opent1::sim {
namespace network {
class GhostManager;
class PacketStream;
}

inline constexpr UInt16 InvalidGhostIndex = 1023;

class SimNetObject : public SimObject {
public:
    enum NetFlag : UInt32 {
        IsGhost = 2,
        PolledGhost = 32,
        ScopeAlways = 64,
        ScopeLocal = 128,
        Ghostable = 256,
        Locked = 512
    };

    struct CameraInfo {
        SimAddress camera;
        Point3F position{}, orientation{0, 1, 0};
        Flt32 fov{.785398163f}, sinFov{.5f}, cosFov{.5f}, visibleDistance{1};
    };

    ~SimNetObject() override;
    bool onAdd() override;
    void onRemove() override;

    [[nodiscard]] UInt32 netFlags() const {
        return netFlags_;
    }

    // Configure before registration; live changes require explicit rescoping.
    bool setNetFlags(UInt32);

    [[nodiscard]] bool isGhost() const {
        return (netFlags_ & IsGhost) != 0;
    }

    [[nodiscard]] UInt16 getNetIndex() const {
        return netIndex_;
    }

    void setMaskBits(UInt32);
    [[nodiscard]] UInt16 getGhostedIndex(const network::GhostManager&) const;

    [[nodiscard]] virtual UInt16 getGhostTag() const {
        return 0;
    }

    virtual UInt32 packUpdate(network::GhostManager&, UInt32, BitStream&) const {
        return 0;
    }

    virtual bool unpackUpdate(network::GhostManager&, BitStream&) {
        return true;
    }

    [[nodiscard]] virtual Flt32 getUpdatePriority(const CameraInfo&, UInt32, UInt32) const {
        return 0;
    }

    virtual void buildScopeAndCameraInfo(network::GhostManager&, CameraInfo&);
    Persistent::Error read(StreamIO&, Int32, Int32, Persistent::LoadContext&) override;
    Persistent::Error write(StreamIO&, Int32, Int32, Persistent::StoreContext&) const override;

private:
    friend class network::GhostManager;
    UInt32 netFlags_{};
    UInt16 netIndex_{InvalidGhostIndex};
    std::vector<network::GhostManager*> observers_;
};

namespace network {
enum class ReceivePolicy { Client, Server, Both };

struct EventContext {
    SimManager& manager;
    PacketStream& stream;
    SimObjectId source;
    bool server;
};

struct RemoteCreateEvent : SimEvent {
    RemoteCreateEvent() {
        type = EventTypeFirst + 33;
    }

    std::unique_ptr<SimNetObject> object;
};

// Complete registration before sharing this immutable registry with streams.
// Only registered event types/classes can be instantiated by the network.
class Registry {
public:
    struct EventCodec {
        std::type_index type{typeid(void)};
        ReceivePolicy policy{ReceivePolicy::Both};
        std::function<std::unique_ptr<SimEvent>()> create;
        std::function<bool(const SimEvent&, BitStream&, EventContext&)> pack;
        std::function<bool(SimEvent&, BitStream&, EventContext&)> unpack;
    };

    template <std::derived_from<SimEvent> T, class Pack, class Unpack>
    bool
    registerEvent(Int32 id, Pack pack, Unpack unpack, ReceivePolicy policy = ReceivePolicy::Both) {
        if (id < 1024 || id >= 1152) {
            return false;
        }
        return events_
            .emplace(id,
                     EventCodec{typeid(T),
                                policy,
                                [] {
                                    return std::make_unique<T>();
                                },
                                [pack](const SimEvent& e, BitStream& b, EventContext& c) {
                                    auto* p = dynamic_cast<const T*>(&e);
                                    return p && pack(*p, b, c);
                                },
                                [unpack](SimEvent& e, BitStream& b, EventContext& c) {
                                    auto* p = dynamic_cast<T*>(&e);
                                    return p && unpack(*p, b, c);
                                }})
            .second;
    }

    template <std::derived_from<SimNetObject> T> bool registerGhost(UInt16 tag) {
        if (!tag || tag >= 1024) {
            return false;
        }
        return ghosts_
            .emplace(tag,
                     [] {
                         return std::make_unique<T>();
                     })
            .second;
    }

    [[nodiscard]] const EventCodec* event(Int32) const;
    [[nodiscard]] std::unique_ptr<SimNetObject> createGhost(UInt16) const;

private:
    std::map<Int32, EventCodec> events_;
    std::map<UInt16, std::function<std::unique_ptr<SimNetObject>()>> ghosts_;
};

// Explicitly enable remote-prefixed console commands on this connection's registry.
bool registerConsoleEvent(Registry&, ReceivePolicy = ReceivePolicy::Both);
bool registerRemoteCreateEvent(Registry&); // original client-only event 1057
bool registerServiceGhosts(Registry&);     // SimVolume (1), SimPalette (2), SimVolumetric (134)

class PacketStreamClient : public SimObject {
public:
    [[nodiscard]] PacketStream* owner() const {
        return owner_;
    }

    virtual bool writePacket(BitStream&, UInt32& key) = 0;
    virtual bool readPacket(BitStream&, UInt32 now) = 0;
    virtual void packetReceived(UInt32) = 0;
    virtual void packetDropped(UInt32) = 0;

    // Client records are unframed in legacy demos. Consume exactly your own
    // fixed-width record; false terminates playback without dispatching a tail.
    virtual bool processRecorderStream(StreamIO&, UInt32) {
        return false;
    }

    virtual void notifyPlaybackOver() {}

    virtual void notifyRecordOver() {}

private:
    friend class PacketStream;
    PacketStream* owner_{};
};

struct PolledUpdateEvent : SimEvent {
    PolledUpdateEvent() {
        type = EventTypeFirst + 34;
    }
    class EventManager* events{};
};

class EventManager : public PacketStreamClient {
public:
    explicit EventManager(std::size_t capacity = 4096);
    ~EventManager() override;
    bool postRemoteEvent(std::unique_ptr<SimEvent>);

    struct Delivery {
        EventManager* target{};
        std::unique_ptr<SimEvent> event;
    };

    // Single-threaded two-phase fanout. Validate every target and allocate all
    // queue tails before swapping any queue; refusal leaves every queue intact.
    [[nodiscard]] static bool postRemoteBatch(std::vector<Delivery>);
    bool addPolledObject(SimObject&);
    bool writePacket(BitStream&, UInt32&) override;
    bool readPacket(BitStream&, UInt32) override;
    void packetReceived(UInt32) override;
    void packetDropped(UInt32) override;
    void onRemove() override;
    [[nodiscard]] std::size_t queued() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

class GhostManager : public PacketStreamClient {
public:
    enum class Mode { Inactive, ScopeAlways, Normal };
    explicit GhostManager(bool allowGhosts = true);
    ~GhostManager() override;
    bool onAdd() override;
    void onRemove() override;
    void onDeleteNotify(SimObject&) override;
    bool objectInScope(SimNetObject&);
    bool setScopeObject(SimNetObject*);
    [[nodiscard]] SimNetObject* scopeObject() const;
    void activate();
    void reset(); // leave replicated objects; stop scoping and retire outgoing ghosts
    [[nodiscard]] Mode mode() const;
    [[nodiscard]] SimNetObject* resolveGhost(UInt16) const;
    [[nodiscard]] SimNetObject* resolveGhostParent(UInt16) const;
    [[nodiscard]] UInt16 getGhostIndex(const SimNetObject&) const;
    bool writePacket(BitStream&, UInt32&) override;
    bool readPacket(BitStream&, UInt32) override;
    void packetReceived(UInt32) override;
    void packetDropped(UInt32) override;
    // Called by SimNetObject; source lifetimes never escape as pointer-sized keys.
    void setMaskBits(const SimNetObject&, UInt32);
    // In-process reliable delivery uses the same observer masks as packets.
    // Restore the mask with setMaskBits on failed serialization or application.
    [[nodiscard]] UInt32 takeUpdateMask(const SimNetObject&);
    // Complete reliable in-process creation after the receiver adds its ghost.
    // Source removal immediately releases these entries; the caller owns ghost
    // deletion, and the receiver clears its mapping through delete notification.
    bool registerDirectGhost(SimNetObject& source, GhostManager& receiver, SimNetObject& ghost);
    void removeSource(SimNetObject&);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

struct PacketStats {
    enum Direction : UInt8 { Send = 0, Receive = 1 };

    std::array<std::array<Int32, 2048>, 2> bitAccumulator{};
    Int32 totalSent{}, totalRecv{};

    void addBits(Direction dir, Int32 count, Int32 persistTag) noexcept {
        if (persistTag > 0 && persistTag < 2048) {
            bitAccumulator[static_cast<std::size_t>(dir)][static_cast<std::size_t>(persistTag)] +=
                count;
        }
    }

    void add(const PacketStats& other) noexcept {
        for (auto d : std::views::iota(std::size_t{0}, std::size_t{2})) {
            for (auto i : std::views::iota(std::size_t{0}, std::size_t{2048})) {
                bitAccumulator[d][i] += other.bitAccumulator[d][i];
            }
        }
        totalSent += other.totalSent;
        totalRecv += other.totalRecv;
    }

    void subtract(const PacketStats& other) noexcept {
        for (auto d : std::views::iota(std::size_t{0}, std::size_t{2})) {
            for (auto i : std::views::iota(std::size_t{0}, std::size_t{2048})) {
                bitAccumulator[d][i] -= other.bitAccumulator[d][i];
            }
        }
        totalSent -= other.totalSent;
        totalRecv -= other.totalRecv;
    }

    void reset() noexcept {
        bitAccumulator[0].fill(0);
        bitAccumulator[1].fill(0);
        totalSent = 0;
        totalRecv = 0;
    }
};

class PacketStream : public SimGroup {
public:
    enum class Mode { Normal, Reply, Server };

    struct Rate {
        UInt32 updateDelay{100};
        UInt16 packetSize{200};
        friend bool operator==(const Rate&, const Rate&) = default;
    };

    struct Packet {
        std::vector<std::byte> bytes;
        std::size_t bits{};
        UInt32 key{};
    };
    enum class DemoTiming { Timestamped, Timer };
    enum class DemoState { Idle, Recording, Playback, Finished, Failed };
    PacketStream(std::shared_ptr<const Registry>, bool server = false, Mode = Mode::Normal);
    ~PacketStream() override;
    bool onAdd() override;
    void onBeforeRemove() override;
    void onRemove() override;
    bool addObject(SimObject&) override;
    bool removeObject(SimObject&) override;
    bool processEvent(const SimEvent&) override;

    [[nodiscard]] const Registry& registry() const {
        return *registry_;
    }

    [[nodiscard]] bool isServer() const {
        return server_;
    }

    [[nodiscard]] EventManager* eventManager() const;
    [[nodiscard]] GhostManager* ghostManager() const;
    bool sendRemoteEvent(std::unique_ptr<SimEvent>, bool throughGhost = false);
    // Legacy peers use 450 bytes. Extended peers may opt in to the existing
    // ten-bit wire limit for larger authored datablock events.
    bool setMaxRate(UInt32 packetsPerSecond, UInt32 packetSize, UInt16 wireLimit = 450);

    [[nodiscard]] Rate rate() const {
        return currentRate_;
    }

    [[nodiscard]] Rate maxRate() const {
        return maxRate_;
    }

    // Nominal remaining packet budget for the client currently being packed.
    // A complete event may cross this boundary, but never the physical datagram limit.
    [[nodiscard]] std::size_t nominalWriteBits() const {
        return nominalWriteBits_;
    }

    [[nodiscard]] std::optional<UInt32> packetFrame() const {
        return packetFrame_;
    }

    void recordBits(PacketStats::Direction, Int32 count, Int32 persistTag) noexcept;

    [[nodiscard]] const PacketStats& stats() const noexcept {
        return stats_;
    }

    void clearStats() noexcept {
        stats_.reset();
    }

    [[nodiscard]] static PacketStats& globalStats() noexcept;
    // Packet codecs are also usable without a socket for captures/replay/tests.
    std::optional<Packet> writePacket();
    bool readPacket(std::span<const std::byte>, UInt32 now, std::optional<std::size_t> bits = {});
    bool handleNotify(UInt32 key, bool delivered);
    // All times are milliseconds modulo 2^32. Demos preserve the original
    // manager/stream header and 0xff packet / 0xfe timer / client-index records.
    bool startRecording(DemoTiming, UInt32 now);
    bool recordClient(PacketStreamClient&, std::span<const std::byte>, UInt32 now);
    bool stopRecording();
    [[nodiscard]] std::span<const std::byte> recording() const;
    bool startPlayback(std::span<const std::byte>, DemoTiming, UInt32 now);
    bool processRecorder(UInt32 now, std::size_t budget = 4096);
    bool stepReplay(std::size_t budget = 4096);
    void endPlayback();
    [[nodiscard]] DemoState demoState() const;
    // Opt in to the original Tribes frame pacing. Server mode uses negotiated
    // rate pacing. Frame values clamp to the original 32..128 ms interval.
    bool setPacketFrame(std::optional<UInt32>);
    void attach(net::Session&, net::ConnectionId);
    [[nodiscard]] std::string transportAddress() const;
    net::Result checkPacketSend(UInt32 now);
    void connectionDied();
    void disconnect(UInt32 now, std::span<const std::byte> reason = {});

    [[nodiscard]] std::string_view error() const {
        return error_;
    }

    void fail(std::string_view);

    [[nodiscard]] UInt32 currentTime() const {
        return now_;
    }

    [[nodiscard]] UInt32 lastReceivedTick() const {
        return lastReceivedTick_;
    }

    std::function<void()> onGhostAlwaysDone;
    std::function<void(std::span<const std::byte>)> onRecordingFinished;
    std::function<bool(SimNetObject&)> prepareGhost;

private:
    struct Note {
        SimAddress client;
        UInt32 key;
    };

    struct Pending {
        std::vector<Note> notes;
        bool rate{}, maximum{};
        UInt32 sequence{};
    };
    struct Demo;
    std::unique_ptr<Demo> demo_;
    bool appendDemo(std::span<const std::byte>, UInt32, bool timestamp = true);
    bool replay(UInt32, bool, std::size_t);
    void finishPlayback(bool);
    std::shared_ptr<const Registry> registry_;
    bool server_{}, rateChanged_{}, maxChanged_{}, needsReply_{}, driving_{}, receiving_{};
    Mode mode_;
    Rate currentRate_, maxRate_;
    UInt16 packetSizeLimit_{450};
    std::size_t nominalWriteBits_{net::MaxPayload * 8};
    UInt32 nextKey_{1}, now_{}, lastUpdate_{1}, lastReceivedTick_{};
    UInt32 packetSequence_{1}, lastPacketReceived_{};
    std::optional<UInt32> packetFrame_;
    std::map<UInt32, Pending> pending_;
    net::Session* session_{};
    net::ConnectionId connection_{};
    PacketStats stats_{}, stagedStats_{}, lastSentStats_{};
    std::string error_;
};

// Owns the transport/session and shares its immutable registry. All driving and
// callbacks use one thread; the host supplies monotonic milliseconds explicitly.
class CSDelegate : public SimObject {
public:
    CSDelegate(std::unique_ptr<net::Transport>, std::shared_ptr<const Registry>, bool server);
    ~CSDelegate() override;
    bool onAdd() override;
    void onRemove() override;
    bool processEvent(const SimEvent&) override;
    net::Result connect(std::string_view, UInt32 now, std::span<const std::byte> request = {});
    net::Result process(UInt32 now);
    bool disconnect(SimObjectId peer, UInt32 now, std::span<const std::byte> reason = {});
    [[nodiscard]] PacketStream* stream(SimObjectId peer) const;
    [[nodiscard]] std::string localAddress() const;
    [[nodiscard]] std::string translateAddress(std::string_view, UInt16 defaultPort) const;

    [[nodiscard]] std::shared_ptr<const Registry> registryShared() const {
        return registry_;
    }

    [[nodiscard]] ServerBrowser& browser() {
        return browser_;
    }

    // Called before accept; return false to reject. Request bytes are borrowed
    // for the duration of the callback. No game-global server/browser state.
    std::function<bool(std::span<const std::byte>)> acceptRequest;
    std::function<bool(std::string_view, std::span<const std::byte>)> acceptAddressedRequest;
    // Game-specific IDs can share a pool with non-connection representatives.
    // The returned slot is committed synchronously by configureStream.
    std::function<SimObjectId()> selectClientId;
    std::function<void(PacketStream&)> configureStream;
    std::function<void(const net::Event&)> onConnection;
    std::function<void(std::string_view, std::span<const std::byte>)> onRawPacket;
    // Refresh game-owned preferences before browser/session I/O on each tick.
    std::function<void(UInt32)> beforeProcess;
    std::function<UInt32()> clock;

private:
    PacketStream* createStream(net::ConnectionId, SimObjectId);
    void removeStream(net::ConnectionId);
    std::unique_ptr<net::Transport> transport_;
    net::Session session_;
    ServerBrowser browser_;
    std::shared_ptr<const Registry> registry_;
    bool server_{}, driving_{};
    std::unique_ptr<net::AddressLookup> pendingLookup_;
    std::vector<std::byte> pendingRequest_;
    std::string pendingAddress_;
    UInt32 now_{};
    std::map<net::ConnectionId, SimAddress> streams_;
};
} // namespace network
} // namespace opent1::sim
