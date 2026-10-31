#pragma once
#include "test_support.hpp"
#include <opent1/platform/net_transport.hpp>
#include <algorithm>
#include <deque>
#include <vector>

namespace net_test {
using namespace opent1;
using namespace opent1::net;

inline std::span<const std::byte> bytes(std::string_view text) {
    return std::as_bytes(std::span(text));
}

inline std::string text(std::span<const std::byte> data) {
    return {reinterpret_cast<const char*>(data.data()), data.size()};
}

struct Wire final : Transport {
    std::string address;
    std::deque<Datagram> incoming, outgoing;
    Result sendResult{Result::Ok};
    std::vector<std::string>* trace{};

    explicit Wire(std::string name) : address(std::move(name)) {}

    Result send(std::string_view, std::span<const std::byte> data) override {
        if (sendResult != Result::Ok) {
            return sendResult;
        }
        Datagram d;
        d.address = address;
        d.size = data.size();
        std::ranges::copy(data, d.bytes.begin());
        outgoing.push_back(d);
        if (trace) {
            constexpr std::string_view digits = "0123456789abcdef";
            std::string hex;
            for (auto b : data) {
                const auto v = std::to_integer<unsigned>(b);
                hex += digits[v >> 4];
                hex += digits[v & 15];
            }
            trace->push_back(address + ":wire:" + hex);
        }
        return Result::Ok;
    }

    Result receive(Datagram& d) override {
        if (incoming.empty()) {
            return Result::WouldBlock;
        }
        d = incoming.front();
        incoming.pop_front();
        return Result::Ok;
    }

    std::string localAddress() const override {
        return address;
    }

    void close() override {
        sendResult = Result::Closed;
        incoming.clear();
    }
};

inline void transfer(Wire& a, Wire& b) {
    while (!a.outgoing.empty()) {
        b.incoming.push_back(a.outgoing.front());
        a.outgoing.pop_front();
    }
}

inline std::vector<Event> drain(Session& s) {
    std::vector<Event> result;
    Event event;
    while (s.poll(event)) {
        result.push_back(event);
    }
    return result;
}

struct Pair {
    Wire a{"0"}, b{"1"};
    Session client{a}, server{b};
    ConnectionId clientId{}, serverId{};

    void pump(UInt32 now = 0) {
        for (unsigned i = 0; i < 10; ++i) {
            transfer(a, b);
            CHECK(server.process(now) == Result::Ok);
            transfer(b, a);
            CHECK(client.process(now) == Result::Ok);
            if (a.outgoing.empty() && b.outgoing.empty()) {
                return;
            }
        }
        CHECK(false);
    }

    void connect(UInt32 now = 0) {
        CHECK(client.connect("1", bytes("hello"), now, clientId) == Result::Ok);
        pump(now);
        auto events = drain(server);
        CHECK(events.size() == 1 && events[0].type == EventType::Requested);
        serverId = events[0].connection;
        CHECK(server.accept(serverId, bytes("ok"), now) == Result::Ok);
        pump(now);
        CHECK(drain(client).front().type == EventType::Accepted);
        CHECK(drain(server).front().type == EventType::Connected);
    }
};
}
