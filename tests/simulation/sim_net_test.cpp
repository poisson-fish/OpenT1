#include "sim_net_test_support.hpp"
#include <opent1/sim/common_irc_shadow.hpp>
#include <opent1/core/mem_stream.hpp>
#include <random>
using namespace sim_net_test;

namespace {
void wholeEventHeadroom() {
    struct LargeEvent : AddressEvent {
        LargeEvent() {
            type = 1104;
        }
    };

    auto r = registry();
    CHECK(r->registerEvent<LargeEvent>(
        1104,
        [](const auto& e, auto& b, auto&) {
            std::array<std::byte, 600> padding{};
            return e.packAddress(b) && b.write(e.value) && b.writeBytes(padding);
        },
        [](auto& e, auto& b, auto&) {
            std::array<std::byte, 600> padding{};
            return e.unpackAddress(b) && b.read(e.value) && b.readBytes(padding);
        }));
    Endpoint server(r, true, false), client(r, false, false);
    auto* target = add<ProbeGhost>(client.manager());
    CHECK(target->setId(99));
    auto large = std::make_unique<LargeEvent>();
    large->address.objectId = 99;
    large->value = 71;
    large->flags = SimEvent::Guaranteed | SimEvent::Ordered;
    CHECK(server.stream->sendRemoteEvent(std::move(large)));
    CHECK(server.stream->sendRemoteEvent(addressed(99, 72)));
    auto first = packet(*server.stream);
    CHECK(first.bytes.size() > server.stream->rate().packetSize &&
          first.bytes.size() <= net::MaxPayload);
    CHECK(server.stream->handleNotify(first.key, false));
    deliver(*server.stream, *client.stream);
    deliver(*server.stream, *client.stream);
    CHECK(target->received == std::vector<UInt32>({71, 72}));
    CHECK(server.events->queued() == 0);
}

void events() {
    Endpoint sender(registry(), true, false), receiver(registry(), false, false);
    auto* target = add<ProbeGhost>(receiver.manager());
    CHECK(target->setId(99));
    // Drop the first ordered event, deliver its successor, then retransmit.
    CHECK(sender.stream->sendRemoteEvent(addressed(99, 1)));
    auto first = packet(*sender.stream);
    CHECK(sender.stream->sendRemoteEvent(addressed(99, 2)));
    auto second = packet(*sender.stream);
    CHECK(receiver.stream->readPacket(second.bytes, 0, second.bits));
    CHECK(target->received.empty());
    CHECK(sender.stream->handleNotify(first.key, false));
    CHECK(sender.stream->handleNotify(second.key, true));
    deliver(*sender.stream, *receiver.stream);
    CHECK(target->received == std::vector<UInt32>({1, 2}));
    CHECK(!sender.stream->handleNotify(first.key, true));
    CHECK(sender.events->queued() == 0);
    // Exercise sequence wrap many times with independent packet framing.
    std::ranges::for_each(std::views::iota(0U, 600U), [&](auto i) {
        CHECK(sender.stream->sendRemoteEvent(addressed(99, i)));
        deliver(*sender.stream, *receiver.stream);
    });
    CHECK(target->received.size() == 602);
    // Missing and recycled local destinations do not redirect ghost-resolved events.
    CHECK(sender.stream->sendRemoteEvent(addressed(999, 3)));
    deliver(*sender.stream, *receiver.stream);
    auto bad = addressed(99, 4, SimEvent::Ordered);
    CHECK(!sender.stream->sendRemoteEvent(std::move(bad)));
    auto unknown = std::make_unique<SimEvent>();
    unknown->type = 1152;
    CHECK(!sender.stream->sendRemoteEvent(std::move(unknown)));
    // Receiver role allowlists are enforced before any event delivery.
    Endpoint client(registry(), false, false), server(registry(ReceivePolicy::Client), true, false);
    CHECK(client.stream->sendRemoteEvent(addressed(0, 1)));
    const auto denied = packet(*client.stream);
    CHECK(!server.stream->readPacket(denied.bytes, 0, denied.bits));
    CHECK(!server.stream->error().empty());
    // The 127-event window stops a missing ordered event from being overwritten.
    Endpoint window(registry(), false, false);
    std::ranges::for_each(std::views::iota(0U, 200U), [&](auto i) {
        CHECK(window.stream->sendRemoteEvent(addressed(0, i)));
    });
    std::vector<PacketStream::Packet> pending;
    while (auto p = window.stream->writePacket()) {
        pending.push_back(std::move(*p));
    }
    CHECK(pending.size() < 32 && window.events->queued() == 200);
    std::ranges::for_each(pending, [&](const auto& p) {
        CHECK(window.stream->handleNotify(p.key, true));
    });
    CHECK(window.events->queued() == 73);
    auto tail = packet(*window.stream);
    CHECK(window.stream->handleNotify(tail.key, true));
}

void ghosting() {
    Endpoint server(registry(), true), client;
    auto* a = add<ProbeGhost>(server.manager());
    a->x = 11;
    a->y = 22;
    auto* b = add<ProbeGhost>(server.manager());
    b->x = 33;
    b->y = 44;
    server.ghosts->activate();
    int serverDone{}, clientDone{};
    server.stream->onGhostAlwaysDone = [&] {
        ++serverDone;
    };
    client.stream->onGhostAlwaysDone = [&] {
        ++clientDone;
    };
    auto initial = packet(*server.stream);
    CHECK(client.stream->readPacket(initial.bytes, 0, initial.bits));
    CHECK(client.ghosts->resolveGhost(0) && !client.ghosts->resolveGhost(0)->getManager());
    CHECK(server.ghosts->getGhostIndex(*a) == InvalidGhostIndex);
    CHECK(server.stream->handleNotify(initial.key, true));
    deliver(*server.stream, *client.stream);
    CHECK(serverDone == 1 && clientDone == 1 &&
          server.ghosts->mode() == GhostManager::Mode::Normal);
    auto* remote = dynamic_cast<ProbeGhost*>(client.ghosts->resolveGhost(0));
    CHECK(remote && remote->x == 11 && remote->y == 22 && remote->isGhost());
    CHECK(remote->getNumber() < client.ghosts->resolveGhost(1)->getNumber());
    CHECK(server.ghosts->getGhostIndex(*a) == 0);
    // A newer update covers the older lost mask; notify order need not be a pointer chain.
    a->x = 101;
    a->setMaskBits(1);
    auto older = packet(*server.stream);
    a->x = 102;
    a->setMaskBits(1);
    auto newer = packet(*server.stream);
    CHECK(server.stream->handleNotify(older.key, false));
    CHECK(client.stream->readPacket(newer.bytes, 0, newer.bits));
    CHECK(server.stream->handleNotify(newer.key, true));
    CHECK(remote->x == 102);
    auto idle = packet(*server.stream);
    CHECK(idle.bits == 9);
    CHECK(server.stream->handleNotify(idle.key, true));
    // Remote events address each direction through the established ghost slot.
    CHECK(server.stream->sendRemoteEvent(addressed(a->getId(), 77), true));
    deliver(*server.stream, *client.stream);
    CHECK(remote->received == std::vector<UInt32>{77});
    CHECK(client.stream->sendRemoteEvent(addressed(remote->getId(), 88), true));
    deliver(*client.stream, *server.stream);
    CHECK(a->received == std::vector<UInt32>{88});
    // Deletion is reliable and the slot can be reused only after acknowledgement.
    const auto oldAddress = remote->address();
    CHECK(a->deleteObject());
    auto kill = packet(*server.stream);
    CHECK(server.stream->handleNotify(kill.key, false));
    deliver(*server.stream, *client.stream);
    CHECK(!oldAddress.resolve(client.manager()));
    CHECK(!client.ghosts->resolveGhost(0));
    auto* c = add<ProbeGhost>(server.manager());
    c->x = 999;
    deliver(*server.stream, *client.stream);
    CHECK(server.ghosts->getGhostIndex(*c) == 0 &&
          dynamic_cast<ProbeGhost*>(client.ghosts->resolveGhost(0))->x == 999);
    CHECK(server.ghosts->resolveGhost(InvalidGhostIndex) == nullptr);
    CHECK(!server.ghosts->resolveGhostParent(InvalidGhostIndex));
    const auto remoteAddress = client.ghosts->resolveGhost(0)->address();
    CHECK(client.stream->deleteObject());
    CHECK(!remoteAddress.resolve(client.manager()));
}

void serviceGhosts() {
    auto r = registry();
    CHECK(registerServiceGhosts(*r));
    Endpoint server(r, true), client(r);
    auto* volume = add<SimVolume>(server.manager()); // empty names need no external assets
    auto* water = add<SimVolumetric>(server.manager());
    auto state = water->state();
    state.position = {3, 4, 5};
    state.density = 2;
    CHECK(water->setState(state));
    server.ghosts->activate();
    deliver(*server.stream, *client.stream);
    deliver(*server.stream, *client.stream);
    const auto vi = server.ghosts->getGhostIndex(*volume),
               wi = server.ghosts->getGhostIndex(*water);
    CHECK(dynamic_cast<SimVolume*>(client.ghosts->resolveGhost(vi)));
    auto* remote = dynamic_cast<SimVolumetric*>(client.ghosts->resolveGhost(wi));
    CHECK(remote && remote->state().position == state.position && remote->state().density == 2);
    state.position = {9, 8, 7};
    CHECK(water->setState(state));
    deliver(*server.stream, *client.stream);
    CHECK(remote->state().position == state.position);
}

void remoteCreationAndPolling() {
    auto r = registry();
    CHECK(registerRemoteCreateEvent(*r));
    Endpoint server(r, true), client(r);
    const auto count = client.manager().objectCount();
    auto event = std::make_unique<RemoteCreateEvent>();
    auto created = std::make_unique<ProbeGhost>();
    created->x = 177;
    created->y = 188;
    event->object = std::move(created);
    event->flags = SimEvent::Guaranteed | SimEvent::Ordered;
    CHECK(server.stream->sendRemoteEvent(std::move(event)));
    deliver(*server.stream, *client.stream);
    CHECK(client.manager().objectCount() == count + 1);
    CHECK(std::ranges::any_of(client.manager().objects(), [](auto* o) {
        auto* p = dynamic_cast<ProbeGhost*>(o);
        return p && p->x == 177 && p->y == 188 && !p->isGhost();
    }));
    auto denied = std::make_unique<RemoteCreateEvent>();
    denied->object = std::make_unique<ProbeGhost>();
    CHECK(client.stream->sendRemoteEvent(std::move(denied)));
    const auto bad = packet(*client.stream);
    CHECK(!server.stream->readPacket(bad.bytes, 0, bad.bits));

    struct Poller : SimObject {
        bool processEvent(const SimEvent& e) override {
            if (auto* poll = dynamic_cast<const PolledUpdateEvent*>(&e)) {
                CHECK(poll->events->postRemoteEvent(addressed(99, 55)));
                CHECK(deleteObject());
                return true;
            }
            return false;
        }
    };

    Endpoint sender(registry(), true, false), receiver(registry(), false, false);
    auto* target = add<ProbeGhost>(receiver.manager());
    CHECK(target->setId(99));
    auto* poller = add<Poller>(sender.manager());
    CHECK(sender.events->addPolledObject(*poller));
    deliver(*sender.stream, *receiver.stream);
    CHECK(target->received == std::vector<UInt32>{55});
    CHECK(!sender.stream->writePacket());
    SimWorld bounded;
    auto* stream = add<PacketStream>(bounded.manager(), r);
    auto* events = add<EventManager>(bounded.manager(), 1);
    CHECK(stream->addObject(*events));
    CHECK(events->postRemoteEvent(addressed(0, 1)));
    CHECK(!events->postRemoteEvent(addressed(0, 2)));
    auto p = packet(*stream);
    CHECK(events->queued() == 1);
    CHECK(!events->postRemoteEvent(addressed(0, 2)));
    CHECK(stream->handleNotify(p.key, true));
    CHECK(events->postRemoteEvent(addressed(0, 2)));
}

void malformed() {
    Endpoint sender(registry(), true);
    auto* source = add<ProbeGhost>(sender.manager());
    source->x = 999;
    CHECK(sender.stream->sendRemoteEvent(addressed(0, 17)));
    sender.ghosts->activate();
    const auto valid = packet(*sender.stream);
    // Every bit prefix of a real mixed event/ghost packet is bounded and terminal.
    std::ranges::for_each(std::views::iota(std::size_t{0}, valid.bits), [&](auto n) {
        Endpoint receiver;
        CHECK(!receiver.stream->readPacket(valid.bytes, 0, n));
        CHECK(!receiver.stream->error().empty());
    });
    Endpoint receiver;
    CHECK(receiver.stream->readPacket(valid.bytes, 0, valid.bits));
    // Malformed rates, unknown classes, invalid slots, random packets and nonzero tails.
    std::mt19937 random(419);
    std::ranges::for_each(std::views::iota(0, 1500), [&](int) {
        Endpoint endpoint;
        std::array<std::byte, 128> bytes{};
        std::ranges::generate(bytes, [&] {
            return std::byte(random() & 255);
        });
        (void)endpoint.stream->readPacket(std::span(bytes).first(1 + random() % bytes.size()), 0);
    });
    auto corrupt = valid.bytes;
    corrupt.push_back(std::byte{1});
    Endpoint tail;
    CHECK(!tail.stream->readPacket(corrupt, 0));
    // Small budgets defer updates without losing a guaranteed event or mask.
    Endpoint bounded(registry(), true, false);
    CHECK(bounded.events->postRemoteEvent(addressed(0, 42)));
    std::array<std::byte, 1> tiny{};
    BitStream output{std::span(tiny)};
    UInt32 key{};
    CHECK(!bounded.events->writePacket(output, key));
    CHECK(!key && bounded.events->queued() == 1);
    auto p = packet(*bounded.stream);
    CHECK(bounded.stream->handleNotify(p.key, true));
    CHECK(bounded.events->queued() == 0);
    Persistent::ClassRegistry classes;
    Persistent::LoadContext load(classes);
    Persistent::StoreContext store(classes);
    std::array<std::byte, 20> body{};
    MemRWStream initial{std::span(body)};
    SimNetObject value;
    value.setId(77);
    CHECK(value.write(initial, 0, 0, store) == Persistent::Error::Ok);
    std::ranges::for_each(std::views::iota(std::size_t{0}, body.size()), [&](auto n) {
        MemRWStream in{std::span<const std::byte>(body).first(n)};
        SimNetObject object;
        object.setId(5);
        CHECK(object.read(in, 0, 0, load) != Persistent::Error::Ok);
        CHECK(object.getId() == 5);
    });
}

void callbackLifetime() {
    Endpoint endpoint(registry(), true);
    auto* source = add<ProbeGhost>(endpoint.manager());
    endpoint.ghosts->activate();
    const auto streamAddress = endpoint.stream->address();
    source->onPack = [&] {
        CHECK(endpoint.stream->deleteObject());
    };
    CHECK(!endpoint.stream->writePacket());
    CHECK(!streamAddress.resolve(endpoint.manager()));
    Endpoint events(registry(), false, false);
    auto* target = add<ProbeGhost>(events.manager());
    auto map = registry();
    CHECK(map->registerEvent<SimEvent>(
        1102,
        [](const auto&, auto&, auto& c) -> bool {
            CHECK(c.stream.deleteObject());
            return true;
        },
        [](auto&, auto&, auto&) {
            return true;
        }));
    Endpoint deleted(map, false, false);
    auto event = std::make_unique<SimEvent>();
    event->type = 1102;
    event->flags = SimEvent::Guaranteed;
    CHECK(deleted.events->postRemoteEvent(std::move(event)));
    CHECK(!deleted.stream->writePacket());
    CHECK(target->deleteObject());
    // Callback exceptions restore the reliable event queue before propagating.
    auto throws = registry();
    bool throwing = true;
    CHECK(throws->registerEvent<SimEvent>(
        1103,
        [&](const auto&, auto&, auto&) -> bool {
            if (throwing) {
                throw std::runtime_error("pack");
            }
            return true;
        },
        [](auto&, auto&, auto&) {
            return true;
        }));
    Endpoint exception(throws, false, false);
    CHECK(exception.stream->sendRemoteEvent(addressed(0, 1)));
    auto e = std::make_unique<SimEvent>();
    e->type = 1103;
    e->flags = SimEvent::Guaranteed;
    CHECK(exception.stream->sendRemoteEvent(std::move(e)));
    try {
        (void)exception.stream->writePacket();
        CHECK(false);
    } catch (const std::runtime_error&) {
    }
    CHECK(exception.events->queued() == 2);
    throwing = false;
    auto p = packet(*exception.stream);
    CHECK(exception.stream->handleNotify(p.key, true));
    CHECK(exception.events->queued() == 0);
    // A later failing ghost encoder restores earlier creation notifications.
    Endpoint ghostException(registry(), true);
    auto* first = add<ProbeGhost>(ghostException.manager());
    auto* second = add<ProbeGhost>(ghostException.manager());
    ghostException.ghosts->activate();
    second->onPack = [] {
        throw std::runtime_error("ghost pack");
    };
    try {
        (void)ghostException.stream->writePacket();
        CHECK(false);
    } catch (const std::runtime_error&) {
    }
    second->onPack = {};
    auto retry = packet(*ghostException.stream);
    CHECK(ghostException.stream->handleNotify(retry.key, true));
    CHECK(ghostException.ghosts->getGhostIndex(*first) != InvalidGhostIndex &&
          ghostException.ghosts->getGhostIndex(*second) != InvalidGhostIndex);
}

void scopeAndIdentity() {
    Endpoint server(registry(), true), client;
    CHECK(server.manager().setNetworkId(2048));
    CHECK(client.manager().setNetworkId(2049));
    CHECK(server.manager().address().resolve(server.manager()) == &server.manager());
    server.ghosts->activate();
    deliver(*server.stream, *client.stream);
    auto source = std::make_unique<ProbeGhost>();
    CHECK(source->setNetFlags(SimNetObject::Ghostable));
    auto* object = source.get();
    CHECK(server.manager().addObject(std::move(source)));
    auto* camera = add<SimNetObject>(server.manager());
    CHECK(server.ghosts->setScopeObject(camera));
    deliver(*server.stream, *client.stream);
    CHECK(server.ghosts->getGhostIndex(*object) == 0);
    auto* remote = client.ghosts->resolveGhost(0);
    CHECK(remote);
    CHECK(object->setId(900));
    CHECK(remote->setId(901));
    object->setMaskBits(1);
    deliver(*server.stream, *client.stream);
    CHECK(server.ghosts->getGhostIndex(*object) == 0 && client.ghosts->resolveGhost(0) == remote);
    CHECK(server.ghosts->setScopeObject(nullptr));
    deliver(*server.stream, *client.stream);
    CHECK(!client.ghosts->resolveGhost(0));
    CHECK(server.stream->deleteObject());
    CHECK(object->deleteObject()); // source observer cleared even after ID changes
    // ScopeLocal objects existing before stream attachment still bootstrap.
    SimWorld world;
    auto local = std::make_unique<ProbeGhost>();
    CHECK(local->setNetFlags(SimNetObject::Ghostable | SimNetObject::ScopeAlways |
                             SimNetObject::ScopeLocal));
    auto* localPointer = local.get();
    CHECK(world.manager().addObject(std::move(local)));
    auto* stream = add<PacketStream>(world.manager(), registry(), true);
    CHECK(stream->setId(2049));
    auto* gm = add<GhostManager>(world.manager(), false);
    CHECK(stream->addObject(*gm));
    gm->activate();
    auto p = packet(*stream);
    CHECK(stream->handleNotify(p.key, true));
    CHECK(gm->getGhostIndex(*localPointer) == 0);
    auto* other = add<PacketStream>(world.manager(), registry(), true);
    CHECK(other->setId(2050));
    auto* otherGm = add<GhostManager>(world.manager(), false);
    CHECK(other->addObject(*otherGm));
    CHECK(!otherGm->objectInScope(*localPointer));
    // Exactly 1023 usable slots; slot 1023 is the protocol sentinel.
    Endpoint limit(registry(), true);
    std::ranges::for_each(std::views::iota(0, 1023), [&](int) {
        add<ProbeGhost>(limit.manager());
    });
    auto* overflow = add<ProbeGhost>(limit.manager());
    CHECK(!limit.ghosts->objectInScope(*overflow));
}
}

int opent1_test_sim_net() {
    wholeEventHeadroom();
    events();
    ghosting();
    serviceGhosts();
    remoteCreationAndPolling();
    malformed();
    callbackLifetime();
    scopeAndIdentity();
    return 0;
}

#include "test_registry.hpp"
OPENT1_REGISTER_TEST("simulation", "sim_net", opent1_test_sim_net);
