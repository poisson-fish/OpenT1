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
        malformed();
        timeoutAndWrap();
        transactionalAndStale();
        limitsAndBackpressure();
    });
}

#include "test_registry.hpp"
OPENT1_REGISTER_TEST("network", "net_protocol", opent1_test_net_protocol);
