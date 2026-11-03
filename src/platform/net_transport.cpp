#include <opent1/platform/net_transport.hpp>
#include <ranges>
#include <opent1/core/bit_stream.hpp>
#include <algorithm>
#include <bit>
#include <deque>
#include <stdexcept>
#include <vector>

namespace opent1::net {
namespace {
constexpr int Data = 0, Disconnect = 2, Request = 3, Accept = 4, Reject = 5, Ping = 7, Resend = 8,
              Ack = 16;

struct AckRun {
    UInt32 start{}, count{};
};

struct Header {
    UInt32 sequence{}, highAck{}, connectSequence{};
    int parity{}, type{};
    std::array<AckRun, 16> runs{};
    std::size_t runCount{}, size{};
};

bool control(int type) {
    return type >= Disconnect && type <= Reject;
}

std::optional<Header> decode(std::span<const std::byte> data) {
    if (data.size() < 3 || data.size() > MaxDatagram) {
        return {};
    }
    BitStream bits(data.first(std::min(data.size(), MaxHeader)));
    if (!bits.readFlag()) {
        return {};
    }
    Header h;
    h.parity = bits.readInt(1);
    h.sequence = static_cast<UInt32>(bits.readInt(9));
    h.highAck = static_cast<UInt32>(bits.readInt(5));
    while (bits.isValid()) {
        const auto count = static_cast<UInt32>(bits.readInt(3));
        if (!bits.isValid()) {
            return {};
        }
        if (!count) {
            break;
        }
        if (h.runCount == h.runs.size()) {
            return {};
        }
        h.runs[h.runCount++] = {static_cast<UInt32>(bits.readInt(5)), count};
    }
    h.type = bits.readInt(5);
    const int base = h.type & ~(Ack | Resend);
    if (base != Data && base != Ping && !control(base)) {
        return {};
    }
    if (control(base)) {
        h.connectSequence = std::bit_cast<UInt32>(bits.readInt(32));
    }
    if (!bits.isValid()) {
        return {};
    }
    h.size = static_cast<std::size_t>(bits.getPosition());
    if (data.size() - h.size > MaxPayload) {
        return {};
    }
    return h;
}

struct Note {
    UInt32 sent{}, key{};
    int type{-1};
    bool acked{};
};

struct Protocol {
    State state{State::Unbound};
    UInt32 connectSequence{}, lastReceived{}, lastSent{}, highAck{}, lastCleared{}, lastRecvTime{};
    unsigned pingCount{};
    std::array<bool, 32> acks{};
    std::array<Note, 32> notes{};
    std::array<std::byte, MaxPayload> lastPayload{};
    std::size_t lastSize{};
    UInt32 averageRtt{100};
    Flt32 loss{};
    bool responsePending{};

    bool full() const {
        return ((lastSent + 1) & 31) == lastCleared;
    }

    Result
    build(int type, std::span<const std::byte> payload, UInt32 key, UInt32 now, Datagram& out) {
        if (payload.size() > MaxPayload) {
            return Result::TooLarge;
        }
        if (!(type & Resend) && type != Ping && type != Ack && full()) {
            return Result::WindowFull;
        }
        if (type & Resend) {
            if (notes[lastSent & 31].type < 0) {
                return Result::InvalidState;
            }
            notes[lastSent & 31].type |= Resend;
            type |= notes[lastSent & 31].type;
            payload = std::span(lastPayload).first(lastSize);
        } else if (type != Ping && type != Ack) {
            ++lastSent;
            notes[lastSent & 31] = {now, key, type, false};
            lastSize = payload.size();
            std::ranges::copy(payload, lastPayload.begin());
            if (type == Request) {
                state = State::Requesting;
            }
            if (type == Accept) {
                state = State::Accepting;
                type |= Resend;
            }
        }
        out.bytes.fill(std::byte{});
        BitStream bits{std::span(out.bytes)};
        bits.writeFlag(true);
        bits.writeInt(static_cast<Int32>(connectSequence & 1), 1);
        bits.writeInt(static_cast<Int32>(lastSent & 511), 9);
        bits.writeInt(static_cast<Int32>(highAck), 5);
        const UInt32 stop = (lastReceived + 1) & 31;
        UInt32 i = (stop + 1) & 31;
        while (i != stop) {
            if (!acks[i]) {
                i = (i + 1) & 31;
                continue;
            }
            const UInt32 start = i;
            UInt32 count = 0;
            while (i != stop && acks[i] && count < 7) {
                ++count;
                i = (i + 1) & 31;
            }
            bits.writeInt(static_cast<Int32>(count), 3);
            bits.writeInt(static_cast<Int32>(start), 5);
        }
        bits.writeInt(0, 3);
        bits.writeInt(type, 5);
        if (control(type & ~(Resend | Ack))) {
            bits.writeInt(std::bit_cast<Int32>(connectSequence), 32);
        }
        const auto headerSize = static_cast<std::size_t>(bits.getPosition());
        if (!bits.isValid() || headerSize > MaxHeader) {
            return Result::TooLarge;
        }
        std::ranges::copy(payload, out.bytes.begin() + static_cast<std::ptrdiff_t>(headerSize));
        out.size = headerSize + payload.size();
        return Result::Ok;
    }
};

struct Connection {
    ConnectionId id{};
    std::string address;
    Protocol p;
    bool dead{};
};

bool validAddress(std::string_view address) {
    return !address.empty() && address.size() <= MaxAddress && address.find('\0') == address.npos;
}
}

struct Session::Impl {
    Transport& transport;
    SessionConfig config;
    ConnectionId nextId{1};
    std::vector<Connection> connections;
    std::deque<Event> events;

    explicit Impl(Transport& t, SessionConfig c) : transport(t), config(c) {
        if (!c.maxConnections || c.maxConnections > 4096 || c.eventCapacity < 64 ||
            c.eventCapacity > 65536 || !c.packetsPerProcess || c.packetsPerProcess > 65536) {
            throw std::invalid_argument("Invalid network session limits");
        }
        connections.reserve(c.maxConnections);
    }

    bool room() const {
        return events.size() + 34 <= config.eventCapacity;
    }

    Connection* find(ConnectionId id) {
        const auto i = std::ranges::find(connections, id, &Connection::id);
        return i == connections.end() ? nullptr : &*i;
    }

    void emit(Connection& c,
              EventType type,
              std::span<const std::byte> data = {},
              UInt32 key = 0,
              bool delivered = false) {
        Event e;
        e.type = type;
        e.connection = c.id;
        e.address = c.address;
        e.notifyKey = key;
        e.delivered = delivered;
        e.size = data.size();
        std::ranges::copy(data, e.bytes.begin());
        events.push_back(std::move(e));
    }

    void flush(Connection& c) {
        // Include the latest outstanding packet (legacy flush accidentally skipped it).
        for (auto n : std::views::iota(UInt32{1}) | std::views::take_while([&](UInt32 n) {
                          return n <= 32;
                      })) {
            auto& note = c.p.notes[(c.p.lastSent + n) & 31];
            if (note.type >= 0 && note.key) {
                emit(c, EventType::Notify, {}, note.key, false);
            }
            note.type = -1;
        }
    }

    Result
    transmit(Connection& c, int type, std::span<const std::byte> data, UInt32 key, UInt32 now) {
        auto candidate = c.p;
        Datagram packet;
        const auto built = candidate.build(type, data, key, now, packet);
        if (built != Result::Ok) {
            return built;
        }
        const auto sent = transport.send(c.address, packet.data());
        if (sent == Result::Ok) {
            c.p = std::move(candidate);
        }
        return sent;
    }

    void eraseDead() {
        std::erase_if(connections, [](const auto& c) {
            return c.dead;
        });
    }

    void receive(Connection& c, Header h, std::span<const std::byte> data, UInt32 now) {
        auto& p = c.p;
        const int base = h.type & ~(Ack | Resend);
        if (base == Request && p.state != State::Unbound &&
            h.connectSequence != p.connectSequence) {
            flush(c);
            emit(c, EventType::TimedOut);
            c.dead = true;
            return;
        }
        // Reject stale control packets before acknowledgments or timeout state mutate.
        const UInt32 connect =
            base == Request && p.state == State::Unbound ? h.connectSequence : p.connectSequence;
        if (h.parity != static_cast<int>(connect & 1) ||
            (control(base) && h.connectSequence != connect)) {
            return;
        }
        UInt32 sequence = h.sequence | (p.lastReceived & ~UInt32{511});
        if (sequence - p.lastReceived > 30) {
            sequence = h.sequence | ((p.lastReceived + 30) & ~UInt32{511});
        }
        if (sequence - p.lastReceived > 30) {
            return;
        }
        p.connectSequence = connect;
        for (auto n : std::views::iota(UInt32{1}) | std::views::take_while([&](UInt32 n) {
                          return n < sequence - p.lastReceived;
                      })) {
            p.acks[(p.lastReceived + n) & 31] = false;
        }
        if (base != Ping) {
            p.acks[sequence & 31] = true;
        }
        for (UInt32 n = (sequence + 1) & 31; n != ((h.highAck + 1) & 31); n = (n + 1) & 31) {
            p.acks[n] = false;
        }
        bool ackBack = ((h.type == Ping || (h.type & Resend)) && !(h.type & Ack)) ||
                       base == Accept || (((sequence & 31) - h.highAck) & 31) > 11;
        if (h.runCount) {
            UInt32 end{};
            for (auto n :
                 std::views::iota(std::size_t{0}) | std::views::take_while([&](std::size_t n) {
                     return n < h.runCount;
                 })) {
                end = h.runs[n].start;
                for ([[maybe_unused]] auto j :
                     std::views::iota(UInt32{0}) | std::views::take_while([&](UInt32 j) {
                         return j < h.runs[n].count;
                     })) {
                    p.notes[end].acked = true;
                    end = (end + 1) & 31;
                }
            }
            p.lastCleared = h.runs[0].start;
            UInt32 n = (p.lastSent + 1) & 31;
            do {
                auto& note = p.notes[n];
                if (note.type >= 0) {
                    p.loss = static_cast<Flt32>((p.loss * 99 + (note.acked ? 0 : 1)) / 100.0);
                    if (note.key) {
                        emit(c, EventType::Notify, {}, note.key, note.acked);
                    }
                    if (note.acked) {
                        if ((note.type & ~Resend) == Accept && p.state == State::Accepting) {
                            p.state = State::Connected;
                            emit(c, EventType::Connected);
                        }
                        p.averageRtt = (now - note.sent + p.averageRtt * 31) >> 5;
                    }
                    note.type = -1;
                }
                n = (n + 1) & 31;
            } while (n != end);
            p.highAck = (end - 1) & 31;
        }
        p.lastRecvTime = now;
        p.pingCount = 0;
        const bool fresh = sequence != p.lastReceived;
        p.lastReceived = sequence;
        if (ackBack) {
            p.responsePending = true;
        }
        // Legacy emits the ack before delivering the packet to the session.
        if (p.responsePending) {
            const int type = p.notes[p.lastSent & 31].type >= 0 ? Ack | Resend : Ack;
            if (transmit(c, type, {}, 0, now) == Result::Ok) {
                p.responsePending = false;
            }
        }
        if (!fresh) {
            return;
        }
        const auto payload = data.subspan(h.size);
        if (base == Request && p.state == State::Unbound) {
            p.state = State::AwaitingDecision;
            emit(c, EventType::Requested, payload);
        } else if ((base == Accept || base == Reject) && p.state == State::Requesting) {
            if (base == Accept) {
                p.state = State::Connected;
                emit(c, EventType::Accepted, payload);
            } else {
                flush(c);
                emit(c, EventType::Rejected, payload);
                c.dead = true;
            }
        } else if (base == Disconnect && p.state == State::Connected) {
            flush(c);
            emit(c, EventType::Disconnected, payload);
            c.dead = true;
        } else if (base == Data && p.state == State::Connected) {
            emit(c, EventType::Data, payload);
        }
    }

    Result
    action(ConnectionId id, int type, std::span<const std::byte> data, UInt32 key, UInt32 now) {
        auto* c = find(id);
        if (!c) {
            return Result::InvalidState;
        }
        if (!room()) {
            return Result::Capacity;
        }
        if (type == Data && c->p.state != State::Connected) {
            return Result::InvalidState;
        }
        if ((type == Accept || type == Reject) && c->p.state != State::AwaitingDecision) {
            return Result::InvalidState;
        }
        const auto result = transmit(*c, type, data, key, now);
        if (result == Result::Ok && (type == Reject || type == Disconnect)) {
            flush(*c);
            c->dead = true;
            eraseDead();
        }
        return result;
    }
};

Session::Session(Transport& transport, SessionConfig config)
    : impl_(std::make_unique<Impl>(transport, config)) {}

Session::~Session() = default;

std::string Transport::canonicalAddress(std::string_view address) const {
    return validAddress(address) ? std::string(address) : std::string{};
}

Result Session::connect(std::string_view address,
                        std::span<const std::byte> data,
                        UInt32 now,
                        ConnectionId& id) {
    auto& s = *impl_;
    id = 0;
    if (!validAddress(address)) {
        return Result::InvalidAddress;
    }
    if (const auto a = parseIPv4Address(address); a && (std::ranges::all_of(a->host,
                                                                            [](UInt8 b) {
                                                                                return b == 255;
                                                                            }) ||
                                                        !a->port)) {
        return Result::InvalidAddress;
    }
    if (const auto a = parseIpxAddress(address);
        a && (!a->port || std::ranges::all_of(a->node, [](UInt8 b) {
            return b == 255;
        }))) {
        return Result::InvalidAddress;
    }
    // Normalize numeric aliases so one peer cannot occupy multiple connection slots.
    const auto canonical = s.transport.canonicalAddress(address);
    if (!validAddress(canonical)) {
        return Result::InvalidAddress;
    }
    if (std::ranges::any_of(s.connections, [&](const auto& c) {
            return c.address == canonical;
        })) {
        return Result::InvalidState;
    }
    if (s.connections.size() == s.config.maxConnections || !s.room()) {
        return Result::Capacity;
    }
    Connection c;
    c.id = s.nextId++;
    c.address = canonical;
    c.p.connectSequence = s.config.connectSequence++;
    c.p.lastRecvTime = now;
    const auto result = s.transmit(c, Request, data, 0, now);
    if (result == Result::Ok) {
        id = c.id;
        s.connections.push_back(std::move(c));
    }
    return result;
}

Result Session::accept(ConnectionId id, std::span<const std::byte> data, UInt32 now) {
    return impl_->action(id, Accept, data, 0, now);
}

Result Session::reject(ConnectionId id, std::span<const std::byte> data, UInt32 now) {
    return impl_->action(id, Reject, data, 0, now);
}

Result Session::disconnect(ConnectionId id, std::span<const std::byte> data, UInt32 now) {
    return impl_->action(id, Disconnect, data, 0, now);
}

Result Session::send(ConnectionId id, std::span<const std::byte> data, UInt32 key, UInt32 now) {
    return impl_->action(id, Data, data, key, now);
}

Result Session::sendRaw(std::string_view address, std::span<const std::byte> data) {
    if (!validAddress(address)) {
        return Result::InvalidAddress;
    }
    if (data.empty() || data.size() > MaxDatagram) {
        return Result::TooLarge;
    }
    if ((std::to_integer<unsigned>(data.front()) & 1) != 0) {
        return Result::InvalidState;
    }
    return impl_->transport.send(address, data);
}

Result Session::process(UInt32 now) {
    auto& s = *impl_;
    for ([[maybe_unused]] auto n :
         std::views::iota(std::size_t{0}) | std::views::take_while([&](std::size_t n) {
             return n < s.config.packetsPerProcess;
         })) {
        if (!s.room()) {
            return Result::Capacity;
        }
        Datagram packet;
        const auto result = s.transport.receive(packet);
        if (result == Result::WouldBlock) {
            break;
        }
        if (result == Result::TooLarge) {
            continue;
        }
        if (result != Result::Ok) {
            return result;
        }
        if (!validAddress(packet.address) || !packet.size || packet.size > MaxDatagram) {
            continue;
        }
        if (!(std::to_integer<unsigned>(packet.bytes[0]) & 1)) {
            Connection raw;
            raw.address = packet.address;
            s.emit(raw, EventType::Raw, packet.data());
            continue;
        }
        const auto header = decode(packet.data());
        if (!header) {
            continue;
        }
        auto i = std::ranges::find(s.connections, packet.address, &Connection::address);
        if (i == s.connections.end()) {
            if ((header->type & ~(Ack | Resend)) != Request ||
                header->parity != static_cast<int>(header->connectSequence & 1) ||
                header->sequence == 0 || header->sequence > 30 ||
                s.connections.size() == s.config.maxConnections) {
                continue;
            }
            Connection c;
            c.id = s.nextId++;
            c.address = packet.address;
            c.p.lastRecvTime = now;
            s.connections.push_back(std::move(c));
            i = s.connections.end() - 1;
        }
        s.receive(*i, *header, packet.data(), now);
        s.eraseDead();
    }
    for (auto& c : s.connections) {
        if (!s.room()) {
            return Result::Capacity;
        }
        auto& p = c.p;
        if (p.responsePending) {
            const auto result =
                s.transmit(c, p.notes[p.lastSent & 31].type >= 0 ? Ack | Resend : Ack, {}, 0, now);
            if (result != Result::Ok) {
                return result;
            }
            p.responsePending = false;
        }
        if (now - p.lastRecvTime > (p.pingCount + 1) * 3000u) {
            const auto result =
                s.transmit(c, p.notes[p.lastSent & 31].type >= 0 ? Resend : Ping, {}, 0, now);
            if (result != Result::Ok) {
                return result;
            }
            if (++p.pingCount > 15) {
                s.flush(c);
                s.emit(c, EventType::TimedOut);
                c.dead = true;
            }
        }
    }
    s.eraseDead();
    return Result::Ok;
}

bool Session::poll(Event& event) {
    auto& queue = impl_->events;
    if (queue.empty()) {
        return false;
    }
    event = std::move(queue.front());
    queue.pop_front();
    return true;
}

std::optional<ConnectionInfo> Session::info(ConnectionId id) const {
    const auto* c = impl_->find(id);
    if (!c) {
        return {};
    }
    return ConnectionInfo{c->p.state, c->p.averageRtt, c->p.loss, c->p.full()};
}

std::size_t Session::connectionCount() const {
    return impl_->connections.size();
}

std::string Session::address(ConnectionId id) const {
    const auto* connection = impl_->find(id);
    return connection ? connection->address : std::string{};
}
} // namespace opent1::net
