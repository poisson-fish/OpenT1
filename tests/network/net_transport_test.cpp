#include "net_test_support.hpp"
#include <chrono>
#include <thread>
#include <charconv>
using namespace net_test;

namespace {
template <class Fn> void until(Fn&& fn) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!fn()) {
        CHECK(std::chrono::steady_clock::now() < deadline);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

void integration(Transport& clientTransport, Transport& serverTransport) {
    Session client(clientTransport), server(serverTransport);
    ConnectionId cid{}, sid{};
    Event e;
    CHECK(client.connect(serverTransport.localAddress(), bytes("request"), 0, cid) == Result::Ok);
    until([&] {
        CHECK(server.process(0) == Result::Ok);
        return server.poll(e);
    });
    CHECK(e.type == EventType::Requested && text(e.data()) == "request");
    sid = e.connection;
    CHECK(server.accept(sid, bytes("welcome"), 0) == Result::Ok);
    until([&] {
        CHECK(client.process(0) == Result::Ok);
        return client.poll(e);
    });
    CHECK(e.type == EventType::Accepted && text(e.data()) == "welcome");
    until([&] {
        CHECK(server.process(0) == Result::Ok);
        return server.poll(e);
    });
    CHECK(e.type == EventType::Connected);
    std::array<std::byte, MaxPayload> maximum{};
    std::ranges::generate(maximum, [n = 0u]() mutable {
        return static_cast<std::byte>(++n);
    });
    CHECK(client.send(cid, maximum, 77, 0) == Result::Ok);
    until([&] {
        CHECK(server.process(0) == Result::Ok);
        return server.poll(e);
    });
    CHECK(e.type == EventType::Data);
    CHECK(std::ranges::equal(e.data(), maximum));
    CHECK(server.send(sid, bytes("reply"), 0, 0) == Result::Ok);
    std::vector<Event> events;
    until([&] {
        CHECK(client.process(0) == Result::Ok);
        while (client.poll(e)) {
            events.push_back(e);
        }
        return events.size() >= 2;
    });
    CHECK(client.info(cid)->averageRtt != 100);
    CHECK(events.size() == 2);
    CHECK(events[0].type == EventType::Notify && events[0].notifyKey == 77 && events[0].delivered);
    CHECK(events[1].type == EventType::Data && text(events[1].data()) == "reply");
    std::array<std::byte, MaxDatagram> raw{};
    raw.back() = std::byte{42};
    CHECK(client.sendRaw(serverTransport.localAddress(), raw) == Result::Ok);
    until([&] {
        CHECK(server.process(0) == Result::Ok);
        return server.poll(e);
    });
    CHECK(e.type == EventType::Raw && std::ranges::equal(e.data(), raw) && e.connection == 0);
    CHECK(client.disconnect(cid, bytes("bye"), 0) == Result::Ok);
    until([&] {
        CHECK(server.process(0) == Result::Ok);
        return server.poll(e);
    });
    CHECK(e.type == EventType::Disconnected && text(e.data()) == "bye");
    CHECK(client.connectionCount() == 0 && server.connectionCount() == 0);
}

void loop() {
    auto [a, b] = makeLoopTransportPair(1);
    Datagram d;
    CHECK(a->receive(d) == Result::WouldBlock);
    std::string data = "\x02hello";
    CHECK(a->send("LOOPBACK", bytes(data)) == Result::Ok);
    data[1] = 'X';
    CHECK(a->send(b->localAddress(), bytes("next")) == Result::WouldBlock);
    CHECK(b->receive(d) == Result::Ok);
    CHECK(text(d.data()) == "\x02hello");
    CHECK(d.address == a->localAddress());
    CHECK(a->send("IPX:retired", bytes("x")) == Result::InvalidAddress);
    CHECK(a->send("LOOPBACK:wrong", bytes("x")) == Result::InvalidAddress);
    CHECK(a->send("LOOPBACK", {}) == Result::TooLarge);
    std::array<std::byte, MaxDatagram + 1> big{};
    CHECK(a->send("LOOPBACK", big) == Result::TooLarge);
    integration(*a, *b);
    CHECK(a->send("LOOPBACK", bytes("queued")) == Result::Ok);
    a->close();
    CHECK(b->receive(d) == Result::Ok);
    CHECK(b->receive(d) == Result::Closed);
    CHECK(b->send("LOOPBACK", bytes("x")) == Result::Closed);
    auto [c, e] = makeLoopTransportPair();
    integration(*c, *e); // no global loop server
    a.reset();
    b.reset();
    integration(*c, *e);
}

void addresses() {
    CHECK(parseIPv4Address("iP:127.0.0.1:28001") == IPv4Address{{127, 0, 0, 1}, 28001});
    CHECK(formatIPv4Address(*parseIPv4Address("broadcast:28001")) == "IP:Broadcast:28001");
    for (const auto text : {"IPX:1:2",
                            "LOOPBACK",
                            "localhost:123",
                            "127.1:80",
                            "1.2.3.4:-1",
                            "1.2.3.4:65536",
                            "1.2.3.4:5junk",
                            "256.2.3.4:1",
                            "1.2.3.4:",
                            "1.2.3:4",
                            "1.2.3.4.5:9",
                            "1.2.3.4: 5"}) {
        CHECK(!parseIPv4Address(text));
    }
    CHECK(!parseIPv4Address(std::string_view("1.2.3.4:5\0ignored", 18)));
    CHECK(canonicalIPv4Endpoint("iP:LocalHost:28001") == "IP:localhost:28001");
    for (const auto text : {"host",
                            "host:0",
                            "host:-1",
                            "host:65536",
                            "host..name:12",
                            "-host:12",
                            "host-:12",
                            "host/name:12",
                            "256.2.3.4:1"}) {
        CHECK(canonicalIPv4Endpoint(text).empty());
        CHECK(!resolveIPv4AddressAsync(text));
    }
    auto lookup = resolveIPv4AddressAsync("localhost:28001");
    CHECK(lookup);
    std::string resolved;
    until([&] {
        return lookup->poll(resolved);
    });
    CHECK(parseIPv4Address(resolved) == IPv4Address{{127, 0, 0, 1}, 28001});
    // An unresolved name completes as failure without blocking the caller.
    auto failed = resolveIPv4AddressAsync("opent1-host-that-does-not-exist.invalid:28001");
    CHECK(failed);
    until([&] {
        return failed->poll(resolved);
    });
    CHECK(resolved.empty());
}

void ipx() {
    const auto address = parseIpxAddress("ipx:1234abcd:00112233aabb:28001");
    CHECK(address && address->network == std::array<UInt8, 4>{0x12, 0x34, 0xab, 0xcd} &&
          address->node == std::array<UInt8, 6>{0, 0x11, 0x22, 0x33, 0xaa, 0xbb});
    CHECK(formatIpxAddress(*address) == "IPX:1234ABCD:00112233AABB:28001");
    CHECK(formatIpxAddress(*parseIpxAddress("IPX:Broadcast:28001")) == "IPX:broadcast:28001");
    for (const auto invalid : {"IPX:1234567:001122334455:1",
                               "IPX:00000000:00112233445g:1",
                               "IPX:broadcast:65536",
                               "IPX:00000000:001122334455:-1"}) {
        CHECK(!parseIpxAddress(invalid));
    }
    auto sessionTransport = makeGameTransport({{127, 0, 0, 1}, 0});
    CHECK(sessionTransport->canonicalAddress("ipx:1234abcd:00112233aabb:28001") ==
          "IPX:1234ABCD:00112233AABB:28001");
    std::unique_ptr<Transport> native;
    try {
        native = makeIpxTransport();
    } catch (const std::runtime_error& error) {
        CHECK(std::string_view(error.what()).find("IPX") != std::string_view::npos);
        CHECK(sessionTransport->send(formatIpxAddress(*address), bytes("probe")) ==
              Result::UnsupportedTransport);
        std::printf("Native IPX unavailable: %s\n", error.what());
    }
    if (native) {
        CHECK(parseIpxAddress(native->localAddress()));
        native->close();
        Datagram packet;
        CHECK(native->receive(packet) == Result::Closed);
    }
    Session session(*sessionTransport);
    ConnectionId id{};
    CHECK(session.connect("IPX:broadcast:28001", {}, 0, id) == Result::InvalidAddress && id == 0);
}

void udp() {
    auto a = makeUdpTransport({{127, 0, 0, 1}, 0}), b = makeUdpTransport({{127, 0, 0, 1}, 0});
    CHECK(parseIPv4Address(a->localAddress())->port != 0);
    CHECK(a->localAddress() != b->localAddress());
    Datagram d;
    CHECK(a->receive(d) == Result::WouldBlock);
    CHECK(a->send("IPX:obsolete", bytes("x")) == Result::InvalidAddress);
    CHECK(a->send("127.0.0.1:0", bytes("x")) == Result::InvalidAddress);
    bool bindFailed{};
    try {
        auto duplicate = makeUdpTransport(*parseIPv4Address(a->localAddress()));
    } catch (const std::runtime_error&) {
        bindFailed = true;
    }
    CHECK(bindFailed);
    integration(*a, *b);
    a->close();
    a->close();
    CHECK(a->receive(d) == Result::Closed);
    CHECK(a->send(b->localAddress(), bytes("x")) == Result::Closed);
    // One socket's teardown cannot uninitialize another socket's Winsock reference.
    auto c = makeUdpTransport({{127, 0, 0, 1}, 0});
    integration(*c, *b);
}

void legacyPeer(std::string_view port) {
    auto transport = makeUdpTransport({{127, 0, 0, 1}, 0});
    Session session(*transport);
    ConnectionId id{};
    Event e;
    CHECK(session.connect("IP:127.0.0.1:" + std::string(port), bytes("request"), 0, id) ==
          Result::Ok);
    until([&] {
        CHECK(session.process(0) == Result::Ok);
        return session.poll(e);
    });
    CHECK(e.type == EventType::Accepted && text(e.data()) == "legacy");
    CHECK(session.send(id, bytes("modern"), 77, 0) == Result::Ok);
    until([&] {
        CHECK(session.process(0) == Result::Ok);
        return session.poll(e);
    });
    CHECK(e.type == EventType::Notify && e.notifyKey == 77 && e.delivered);
    until([&] {
        CHECK(session.process(0) == Result::Ok);
        return session.poll(e);
    });
    CHECK(e.type == EventType::Data && text(e.data()) == "modern");
    CHECK(session.disconnect(id, bytes("bye"), 0) == Result::Ok);
}

void legacyClient(std::string_view portText) {
    unsigned port{};
    const auto parsed = std::from_chars(portText.data(), portText.data() + portText.size(), port);
    CHECK(parsed.ec == std::errc{} && port > 0 && port <= 65535);
    auto transport = makeUdpTransport({{127, 0, 0, 1}, static_cast<UInt16>(port)});
    Session session(*transport);
    std::puts("ready");
    std::fflush(stdout);
    Event e;
    until([&] {
        CHECK(session.process(0) == Result::Ok);
        return session.poll(e);
    });
    CHECK(e.type == EventType::Requested && text(e.data()) == "request");
    const auto id = e.connection;
    CHECK(session.accept(id, bytes("modern"), 0) == Result::Ok);
    until([&] {
        CHECK(session.process(0) == Result::Ok);
        return session.poll(e);
    });
    CHECK(e.type == EventType::Connected);
    until([&] {
        CHECK(session.process(0) == Result::Ok);
        return session.poll(e);
    });
    CHECK(e.type == EventType::Data && text(e.data()) == "legacy");
    CHECK(session.send(id, e.data(), 0, 0) == Result::Ok);
    until([&] {
        CHECK(session.process(0) == Result::Ok);
        return session.poll(e);
    });
    CHECK(e.type == EventType::Disconnected);
}
}

int opent1_test_net_transport(int argc, char** argv) {
    return test::run([&] {
        if (argc == 2) {
            legacyPeer(argv[1]);
            return;
        }
        if (argc == 3 && std::string_view(argv[1]) == "--server") {
            legacyClient(argv[2]);
            return;
        }
        addresses();
        loop();
        udp();
        ipx();
    });
}

namespace {
int opent1_test_net_transport_default() {
    char program[] = "opent1_tests";
    char* args[] = {program, nullptr};
    return opent1_test_net_transport(1, args);
}
}

#include "test_registry.hpp"
OPENT1_REGISTER_TEST("network", "net_transport", opent1_test_net_transport_default);
