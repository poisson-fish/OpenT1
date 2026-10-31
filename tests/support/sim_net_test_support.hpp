#pragma once
#include "sim_test_support.hpp"
#include <opent1/sim/sim_net.hpp>
#include <opent1/sim/sim_world_game.hpp>
#include <algorithm>
#include <array>

namespace sim_net_test {
using namespace opent1::sim::network;

struct ProbeEvent : SimEvent {
    UInt32 value{};

    ProbeEvent() {
        type = 1100;
    }
};

struct AddressEvent : ProbeEvent {
    AddressEvent() {
        type = 1101;
    }
};

struct ProbeGhost : SimNetObject {
    UInt32 x{1}, y{2};
    std::vector<UInt32> received;
    std::function<void()> onPack, onUnpack, onAdded;

    ProbeGhost() {
        CHECK(setNetFlags(Ghostable | ScopeAlways));
    }

    bool onAdd() override {
        if (onAdded) {
            onAdded();
        }
        return SimNetObject::onAdd();
    }

    UInt16 getGhostTag() const override {
        return 200;
    }

    UInt32 packUpdate(GhostManager&, UInt32 mask, BitStream& b) const override {
        mask &= 3;
        b.writeInt(static_cast<Int32>(mask), 2);
        if (mask & 1) {
            b.writeInt(std::bit_cast<Int32>(x), 32);
        }
        if (mask & 2) {
            b.writeInt(std::bit_cast<Int32>(y), 32);
        }
        if (onPack) {
            onPack();
        }
        return 0;
    }

    bool unpackUpdate(GhostManager&, BitStream& b) override {
        const auto mask = b.readInt(2);
        auto a = x, c = y;
        if (mask & 1) {
            a = std::bit_cast<UInt32>(b.readInt(32));
        }
        if (mask & 2) {
            c = std::bit_cast<UInt32>(b.readInt(32));
        }
        if (!b.isValid()) {
            return false;
        }
        x = a;
        y = c;
        if (onUnpack) {
            onUnpack();
        }
        return true;
    }

    bool processEvent(const SimEvent& event) override {
        if (auto* p = dynamic_cast<const ProbeEvent*>(&event)) {
            received.push_back(p->value);
            return true;
        }
        return SimObject::processEvent(event);
    }
};

inline std::shared_ptr<Registry> registry(ReceivePolicy policy = ReceivePolicy::Both) {
    auto r = std::make_shared<Registry>();
    CHECK(r->registerEvent<ProbeEvent>(
        1100,
        [](const auto& e, auto& b, auto&) {
            b.writeInt(std::bit_cast<Int32>(e.value), 32);
            return b.isValid();
        },
        [](auto& e, auto& b, auto&) {
            e.value = std::bit_cast<UInt32>(b.readInt(32));
            return b.isValid();
        },
        policy));
    CHECK(r->registerEvent<AddressEvent>(
        1101,
        [](const auto& e, auto& b, auto&) {
            return e.packAddress(b) && b.write(e.value);
        },
        [](auto& e, auto& b, auto&) {
            return e.unpackAddress(b) && b.read(e.value);
        },
        policy));
    CHECK(r->registerGhost<ProbeGhost>(200));
    return r;
}

struct Endpoint {
    SimWorld world;
    PacketStream* stream;
    EventManager* events;
    GhostManager* ghosts{};

    explicit Endpoint(std::shared_ptr<Registry> r = registry(),
                      bool server = false,
                      bool withGhosts = true) {
        stream = add<PacketStream>(manager(), std::move(r), server);
        CHECK(stream->setId(server ? 2049 : 2048));
        events = add<EventManager>(manager());
        CHECK(stream->addObject(*events));
        if (withGhosts) {
            ghosts = add<GhostManager>(manager(), !server);
            CHECK(stream->addObject(*ghosts));
        }
    }

    SimManager& manager() {
        return world.manager();
    }
};

inline std::unique_ptr<AddressEvent>
addressed(SimObjectId id, UInt32 value, UInt32 flags = SimEvent::Guaranteed | SimEvent::Ordered) {
    auto e = std::make_unique<AddressEvent>();
    e->address.objectId = id;
    e->value = value;
    e->flags = flags;
    return e;
}

inline PacketStream::Packet packet(PacketStream& stream) {
    auto p = stream.writePacket();
    CHECK(p);
    return std::move(*p);
}

inline void deliver(PacketStream& from, PacketStream& to) {
    const auto p = packet(from);
    CHECK(to.readPacket(p.bytes, 0, p.bits));
    CHECK(from.handleNotify(p.key, true));
}
} // namespace sim_net_test
