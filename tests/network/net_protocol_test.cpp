#include "net_test_support.hpp"
#include <ranges>
#include <opent1/core/bit_stream.hpp>
using namespace net_test;

namespace {
Datagram header(unsigned seq, int type, unsigned connect = 1, unsigned parity = 1) {
    Datagram d;
    d.address = "attacker";
    BitStream b{std::span(d.bytes)};
    b.writeFlag(true);
    b.writeInt(static_cast<Int32>(parity), 1);
    b.writeInt(static_cast<Int32>(seq), 9);
    b.writeInt(0, 5);
    b.writeInt(0, 3);
    b.writeInt(type, 5);
    if ((type & ~24) >= 2 && (type & ~24) <= 5) {
        b.writeInt(static_cast<Int32>(connect), 32);
    }
    d.size = static_cast<std::size_t>(b.getPosition());
    return d;
}

void windowAndFailures() {
    Pair p;
    p.connect();
    p.a.sendResult = Result::WouldBlock;
    CHECK(p.client.send(p.clientId, bytes("failed"), 999, 0) == Result::WouldBlock);
    p.a.sendResult = Result::Ok;
    std::array<std::byte, MaxPayload + 1> big{};
    CHECK(p.client.send(p.clientId, big, 0, 0) == Result::TooLarge);
    unsigned sent{};
    while (p.client.send(p.clientId, bytes("x"), ++sent, 0) == Result::Ok) {
        CHECK(sent < 33);
    }
    CHECK(sent == 32);
    CHECK(p.client.info(p.clientId)->windowFull);
    CHECK(p.client.send(p.clientId, bytes("x"), 99, 0) == Result::WindowFull);
    p.pump();
    const auto notifications = drain(p.client);
    CHECK(!notifications.empty());
    CHECK(!p.client.info(p.clientId)->windowFull);
    CHECK(std::ranges::none_of(notifications, [](const auto& e) {
        return e.notifyKey == 999;
    }));
    Pair q;
    q.connect();
    CHECK(q.client.send(q.clientId, bytes("last"), 999, 0) == Result::Ok);
    // Local close reports the newest outstanding notify, fixing legacy omission.
    CHECK(q.client.disconnect(q.clientId, {}, 0) == Result::Ok);
    const auto closed = drain(q.client);
    CHECK(std::ranges::any_of(closed, [](const auto& e) {
        return e.type == EventType::Notify && e.notifyKey == 999 && !e.delivered;
    }));
    CHECK(!q.client.info(q.clientId));
    CHECK(q.client.send(q.clientId, {}, 0, 0) == Result::InvalidState);
}

void cancelHandshake() {
    for (const bool accepting : {false, true}) {
        Pair p;
        CHECK(p.client.connect("1", bytes("join"), 0, p.clientId) == Result::Ok);
        transfer(p.a, p.b);
        CHECK(p.server.process(0) == Result::Ok);
        const auto requested = drain(p.server);
        CHECK(requested.size() == 1 && requested.front().type == EventType::Requested);
        p.serverId = requested.front().connection;
        if (accepting) {
            CHECK(p.server.accept(p.serverId, bytes("ok"), 0) == Result::Ok);
        }
        // Neither the request nor the accept has completed on the client yet.
        CHECK(p.client.disconnect(p.clientId, bytes("cancel"), 1) == Result::Ok);
        const auto duplicate = p.a.outgoing.front();
        transfer(p.a, p.b);
        CHECK(p.server.process(1) == Result::Ok);
        const auto closed = drain(p.server);
        CHECK(closed.size() == 1 && closed.front().type == EventType::Disconnected);
        CHECK(text(closed.front().data()) == "cancel");
        CHECK(p.client.connectionCount() == 0 && p.server.connectionCount() == 0);
        p.b.incoming.push_back(duplicate);
        CHECK(p.server.process(2) == Result::Ok);
        CHECK(drain(p.server).empty() && p.server.connectionCount() == 0);
    }
    // A remote cancellation must also finish a locally Requesting connection.
    Pair p;
    CHECK(p.client.connect("1", {}, 0, p.clientId) == Result::Ok);
    transfer(p.a, p.b);
    CHECK(p.server.process(0) == Result::Ok);
    p.serverId = drain(p.server).front().connection;
    CHECK(p.server.disconnect(p.serverId, {}, 1) == Result::Ok);
    transfer(p.b, p.a);
    CHECK(p.client.process(1) == Result::Ok);
    CHECK(drain(p.client).front().type == EventType::Disconnected);
    CHECK(p.client.connectionCount() == 0);
}

void handshakeOrdering() {
    // Request and cancellation can both arrive before the server's next poll.
    Pair unpolled;
    CHECK(unpolled.client.connect("1", {}, 0, unpolled.clientId) == Result::Ok);
    CHECK(unpolled.client.disconnect(unpolled.clientId, {}, 1) == Result::Ok);
    transfer(unpolled.a, unpolled.b);
    CHECK(unpolled.server.process(1) == Result::Ok);
    const auto cancelled = drain(unpolled.server);
    CHECK(cancelled.size() == 2 && cancelled[0].type == EventType::Requested &&
          cancelled[1].type == EventType::Disconnected);
    CHECK(unpolled.server.connectionCount() == 0);

    Pair p;
    CHECK(p.client.connect("1", {}, 0, p.clientId) == Result::Ok);
    transfer(p.a, p.b);
    CHECK(p.server.process(0) == Result::Ok);
    p.serverId = drain(p.server).front().connection;
    CHECK(p.server.accept(p.serverId, bytes("accept"), 0) == Result::Ok);
    const auto accept = p.b.outgoing.front();
    CHECK(p.server.info(p.serverId)->state == State::Accepting);
    transfer(p.b, p.a);
    CHECK(p.client.process(1) == Result::Ok);
    const auto accepted = drain(p.client);
    CHECK(accepted.size() == 1 && accepted.front().type == EventType::Accepted);
    // A duplicate/resend acceptance must not deliver another accepted event.
    p.a.incoming.push_back(accept);
    CHECK(p.client.process(1) == Result::Ok);
    CHECK(drain(p.client).empty());
    CHECK(p.server.info(p.serverId)->state == State::Accepting);
    // Lose every standalone acceptance ACK. Disconnect itself acknowledges
    // that acceptance, promoting the server before dispatching termination,
    // just as VCProtocol::receive does in vanilla.
    p.a.outgoing.clear();
    CHECK(p.client.disconnect(p.clientId, bytes("after-accept"), 2) == Result::Ok);
    const auto disconnect = p.a.outgoing.front();
    transfer(p.a, p.b);
    p.b.incoming.push_back(disconnect);
    CHECK(p.server.process(2) == Result::Ok);
    const auto departed = drain(p.server);
    CHECK(departed.size() == 2 && departed[0].type == EventType::Connected &&
          departed[1].type == EventType::Disconnected);
    CHECK(text(departed[1].data()) == "after-accept");
    CHECK(p.server.connectionCount() == 0);

    // Reuse the endpoint twice so generation 3 has the same parity as 1.
    // Full control generations must be checked before ACK processing, not
    // merely the single parity bit or the control-packet dispatch branch.
    for (int reuse = 0; reuse < 2; ++reuse) {
        const UInt32 now = 3 + UInt32(reuse) * 3;
        p.a.outgoing.clear();
        p.b.outgoing.clear();
        CHECK(p.client.connect("1", {}, now, p.clientId) == Result::Ok);
        p.a.incoming.push_back(accept);
        for (const auto type : {2, 5}) {
            auto old = header(2, type);
            old.address = "1";
            p.a.incoming.push_back(old);
        }
        CHECK(p.client.process(now) == Result::Ok);
        CHECK(drain(p.client).empty());
        CHECK(p.client.info(p.clientId)->state == State::Requesting);
        transfer(p.a, p.b);
        CHECK(p.server.process(now) == Result::Ok);
        p.serverId = drain(p.server).front().connection;
        CHECK(p.server.accept(p.serverId, {}, now) == Result::Ok);
        p.b.incoming.push_back(disconnect);
        CHECK(p.server.process(now) == Result::Ok);
        CHECK(p.server.info(p.serverId)->state == State::Accepting);
        CHECK(drain(p.server).empty());
        p.pump(now + 1);
        CHECK(drain(p.client).front().type == EventType::Accepted);
        CHECK(drain(p.server).front().type == EventType::Connected);
        CHECK(p.client.disconnect(p.clientId, {}, now + 2) == Result::Ok);
        p.pump(now + 2);
        const auto closed = drain(p.server);
        CHECK(closed.size() == 1 && closed.front().type == EventType::Disconnected);
        CHECK(p.client.connectionCount() == 0);
    }
}

void staleHandshakeDoesNotRefreshTimeout() {
    Wire wire{"local"};
    SessionConfig config;
    config.connectSequence = 3;
    Session session(wire, config);
    ConnectionId id{};
    CHECK(session.connect("remote", {}, 0, id) == Result::Ok);
    for (UInt32 retry = 1; retry <= 16; ++retry) {
        for (const auto type : {2, 4, 5}) {
            auto stale = header(2, type, 1, 1);
            stale.address = "remote";
            wire.incoming.push_back(stale);
        }
        CHECK(session.process(retry * 3000 + 1) == Result::Ok);
        wire.outgoing.clear();
    }
    CHECK(!session.info(id));
    const auto events = drain(session);
    CHECK(events.size() == 1 && events.front().type == EventType::TimedOut);
}

void malformed() {
    Wire wire{"server"};
    Session session(wire);
    // Every truncated connect prefix; malformed ACK chains and unknown types.
    const auto good = header(1, 3);
    for (auto n : std::views::iota(std::size_t{1}) | std::views::take_while([&](std::size_t n) {
                      return n < good.size;
                  })) {
        auto d = good;
        d.size = n;
        wire.incoming.push_back(d);
    }
    wire.incoming.push_back(header(1, 6));
    wire.incoming.push_back(header(1, 3, 2, 1));
    wire.incoming.push_back(header(31, 3));
    wire.incoming.push_back(header(1, 0));
    Datagram bad;
    bad.address = "attacker";
    bad.bytes.fill(std::byte{255});
    bad.size = 100;
    wire.incoming.push_back(bad);
    CHECK(session.process(0) == Result::Ok);
    CHECK(session.connectionCount() == 0);
    CHECK(drain(session).empty());
    // Deterministic hostile corpus exercises every parser width without trusting a length field.
    UInt32 rng = 17;
    for ([[maybe_unused]] auto i : std::views::iota(unsigned{0}, unsigned{20000})) {
        rng = rng * 1664525 + 1013904223;
        bad.size = 1 + rng % MaxDatagram;
        std::ranges::generate(bad.bytes, [&] {
            rng = rng * 1664525 + 1013904223;
            return static_cast<std::byte>(rng >> 24);
        });
        bad.bytes[0] |= std::byte{1};
        wire.incoming.push_back(bad);
        CHECK(session.process(0) == Result::Ok);
        drain(session);
        wire.outgoing.clear();
    }
    CHECK(session.connectionCount() <= 64);
}

void timeoutAndWrap() {
    Wire w{"a"};
    Session s(w);
    ConnectionId id{};
    constexpr UInt32 start = 0xfffffff0;
    CHECK(s.connect("b", {}, start, id) == Result::Ok);
    w.outgoing.clear();
    CHECK(s.process(start + 3000u) == Result::Ok);
    CHECK(w.outgoing.empty());
    CHECK(s.process(start + 3001u) == Result::Ok);
    CHECK(w.outgoing.size() == 1);
    for (auto n : std::views::iota(UInt32{2}) | std::views::take_while([&](UInt32 n) {
                      return n <= 16;
                  })) {
        CHECK(s.process(start + n * 3000u + 1) == Result::Ok);
    }
    CHECK(!s.info(id));
    CHECK(drain(s).back().type == EventType::TimedOut);
}

void transactionalAndStale() {
    Pair p;
    p.connect();
    auto stale = header(2, 2, 3);
    stale.address = "0";
    p.b.incoming.push_back(stale);
    CHECK(p.server.process(100) == Result::Ok);
    CHECK(p.server.info(p.serverId)->state == State::Connected);
    CHECK(drain(p.server).empty());
    CHECK(p.client.send(p.clientId, bytes("retained"), 41, 0) == Result::Ok);
    p.a.outgoing.clear();
    p.a.sendResult = Result::WouldBlock;
    CHECK(p.client.send(p.clientId, bytes("failed"), 42, 0) == Result::WouldBlock);
    p.a.sendResult = Result::Ok;
    CHECK(p.client.process(3001) == Result::Ok);
    CHECK(p.a.outgoing.size() == 1);
    const auto& retry = p.a.outgoing.front();
    CHECK(text(retry.data().last(8)) == "retained");
    // Receiver needs an ACK, but the underlying queue is temporarily full.
    p.b.sendResult = Result::WouldBlock;
    transfer(p.a, p.b);
    CHECK(p.server.process(3001) == Result::WouldBlock);
    CHECK(drain(p.server).front().type == EventType::Data);
    p.b.sendResult = Result::Ok;
    CHECK(p.server.process(3001) == Result::Ok);
    transfer(p.b, p.a);
    CHECK(p.client.process(3001) == Result::Ok);
    const auto events = drain(p.client);
    CHECK(events.size() == 1 && events[0].type == EventType::Notify && events[0].notifyKey == 41 &&
          events[0].delivered);
}

void limitsAndBackpressure() {
    Wire wire{"server"};
    SessionConfig c;
    c.maxConnections = 1;
    c.eventCapacity = 64;
    c.packetsPerProcess = 1;
    Session s(wire, c);
    wire.incoming.push_back(header(1, 3));
    auto other = header(1, 3);
    other.address = "other";
    wire.incoming.push_back(other);
    CHECK(s.process(0) == Result::Ok);
    CHECK(wire.incoming.size() == 1);
    CHECK(s.connectionCount() == 1);
    CHECK(s.process(0) == Result::Ok);
    CHECK(s.connectionCount() == 1);
    drain(s);
    Datagram raw;
    raw.address = "peer";
    raw.size = 1;
    for ([[maybe_unused]] auto n : std::views::iota(unsigned{0}, unsigned{40})) {
        wire.incoming.push_back(raw);
    }
    for ([[maybe_unused]] auto n : std::views::iota(unsigned{0}, unsigned{30})) {
        CHECK(s.process(0) == Result::Ok);
    }
    CHECK(s.process(0) == Result::Capacity);
    CHECK(wire.incoming.size() == 9);
    CHECK(drain(s).size() == 31);
    CHECK(s.process(0) == Result::Ok);
    CHECK(s.sendRaw("IPX:obsolete", bytes("odd")) == Result::InvalidState);
}
}

int opent1_test_net_protocol() {
    return test::run([] {
        windowAndFailures();
        cancelHandshake();
        handshakeOrdering();
        staleHandshakeDoesNotRefreshTimeout();
        malformed();
        timeoutAndWrap();
        transactionalAndStale();
        limitsAndBackpressure();
    });
}

#include "test_registry.hpp"
OPENT1_REGISTER_TEST("network", "net_protocol", opent1_test_net_protocol);
