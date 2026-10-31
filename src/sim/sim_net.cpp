#include <opent1/sim/sim_net.hpp>
#include <opent1/sim/sim_world_game.hpp>
#include <opent1/sim/common_irc_shadow.hpp>
#include <opent1/sim/sim_palette.hpp>
#include <algorithm>
#include <cmath>
#include <limits>
#include <set>
#include <stdexcept>

namespace opent1::sim {
namespace {
struct DriveGuard {
    bool& value;

    explicit DriveGuard(bool& v) : value(v) {
        if (value) {
            throw std::logic_error("recursive network drive");
        }
        value = true;
    }

    ~DriveGuard() {
        value = false;
    }
};

struct Restore {
    std::function<void()> action;

    ~Restore() {
        action();
    }
};

template <class F> void protectedCall(SimObject& object, F&& fn) {
    if (auto* manager = object.getManager()) {
        manager->dispatch(std::forward<F>(fn));
    }
}

bool live(const SimObject& object) {
    return object.getManager() && !object.isRemoved();
}

UInt32 newKey(UInt32& next) {
    if (!next) {
        throw std::overflow_error("network notification key exhausted");
    }
    return next++;
}
}

SimNetObject::~SimNetObject() {
    onRemove();
}

bool SimNetObject::setNetFlags(UInt32 flags) {
    if (getManager()) {
        return false;
    }
    netFlags_ = flags;
    setLocked((flags & Locked) != 0);
    return true;
}

bool SimNetObject::onAdd() {
    if ((netFlags_ & (Ghostable | ScopeAlways | IsGhost)) == (Ghostable | ScopeAlways)) {
        addToSet(GhostAlwaysSetId);
        if (auto* set = dynamic_cast<SimSet*>(getManager()->findObject(GhostManagerSetId))) {
            const auto snapshot =
                std::vector<SimObject*>(set->objects().begin(), set->objects().end());
            std::ranges::for_each(snapshot, [&](auto* object) {
                if (auto* gm = dynamic_cast<network::GhostManager*>(object); gm && live(*gm)) {
                    gm->objectInScope(*this);
                }
            });
        }
    }
    return true;
}

void SimNetObject::onRemove() {
    const auto snapshot = observers_;
    std::ranges::for_each(snapshot, [&](auto* gm) {
        gm->removeSource(*this);
    });
    observers_.clear();
}

void SimNetObject::setMaskBits(UInt32 mask) {
    if (isDeleted()) {
        return;
    }
    std::ranges::for_each(observers_, [&](auto* gm) {
        gm->setMaskBits(*this, mask);
    });
}

UInt16 SimNetObject::getGhostedIndex(const network::GhostManager& gm) const {
    return gm.getGhostIndex(*this);
}

void SimNetObject::buildScopeAndCameraInfo(network::GhostManager& gm, CameraInfo&) {
    if (!getManager()) {
        return;
    }
    std::vector<SimAddress> work{getManager()->address()};
    std::unordered_set<UInt64> visited;
    while (!work.empty() && live(gm)) {
        auto address = work.back();
        work.pop_back();
        auto* object = address.resolve(*getManager());
        if (!object || !visited.insert(object->generation()).second) {
            continue;
        }
        if (auto* net = dynamic_cast<SimNetObject*>(object);
            net && (net->netFlags() & Ghostable) && !net->isGhost()) {
            gm.objectInScope(*net);
        }
        if (auto* set = dynamic_cast<SimSet*>(object)) {
            std::ranges::for_each(set->objects(), [&](auto* child) {
                work.push_back(child->address());
            });
        }
        if (visited.size() > 65536 || work.size() > 65536) {
            throw std::length_error("network scope traversal limit");
        }
    }
}

Persistent::Error SimNetObject::write(StreamIO& s, Int32, Int32, Persistent::StoreContext&) const {
    const auto flags = (netFlags_ & ~UInt32{Locked}) | (isLocked() ? UInt32{Locked} : 0);
    return s.write(isDynamicId() ? 0U : getId()) && s.write(flags) && s.write(UInt32{0}) &&
                   s.write(UInt32{0xffffffff}) && s.write(UInt32{0})
               ? Persistent::Error::Ok
               : Persistent::Error::WriteError;
}

Persistent::Error SimNetObject::read(StreamIO& s, Int32, Int32, Persistent::LoadContext&) {
    UInt32 id{}, flags{}, pointer{}, object{}, manager{};
    if (getManager() || !s.read(id) || !s.read(flags) || !s.read(pointer) || !s.read(object) ||
        !s.read(manager) || pointer) {
        return Persistent::Error::ReadError;
    }
    setId(id);
    netFlags_ |= flags;
    setLocked((netFlags_ & Locked) != 0);
    return Persistent::Error::Ok;
}

namespace network {
const Registry::EventCodec* Registry::event(Int32 id) const {
    const auto it = events_.find(id);
    return it == events_.end() ? nullptr : &it->second;
}

std::unique_ptr<SimNetObject> Registry::createGhost(UInt16 tag) const {
    const auto it = ghosts_.find(tag);
    return it == ghosts_.end() ? nullptr : it->second();
}

bool registerConsoleEvent(Registry& registry, ReceivePolicy policy) {
    return registry.registerEvent<SimConsoleEvent>(
        EventTypeFirst + 8,
        [](const auto& e, auto& b, auto&) {
            return e.packCommand(b);
        },
        [](auto& e, auto& b, auto& c) {
            return e.unpackCommand(b, c.source);
        },
        policy);
}

bool registerServiceGhosts(Registry& registry) {
    return registry.registerGhost<SimVolume>(1) && registry.registerGhost<SimPalette>(3) &&
           registry.registerGhost<SimVolumetric>(134);
}

bool registerRemoteCreateEvent(Registry& registry) {
    return registry.registerEvent<RemoteCreateEvent>(
        EventTypeFirst + 33,
        [](const RemoteCreateEvent& e, BitStream& b, EventContext& c) {
            auto* gm = c.stream.ghostManager();
            const auto tag = e.object ? e.object->getGhostTag() : 0;
            if (!gm || !tag || tag >= 1024) {
                return false;
            }
            b.writeInt(tag, 10);
            return e.object->packUpdate(*gm, ~UInt32{0}, b) == 0 && b.isValid();
        },
        [](RemoteCreateEvent& e, BitStream& b, EventContext& c) {
            auto* gm = c.stream.ghostManager();
            const auto startPos = static_cast<Int32>(b.getCurPos());
            const auto tag = static_cast<UInt16>(b.readInt(10));
            auto object = c.stream.registry().createGhost(tag);
            if (c.server || !gm || !object || !b.isValid() ||
                (c.stream.prepareGhost && !c.stream.prepareGhost(*object)) ||
                !object->unpackUpdate(*gm, b) || !b.isValid()) {
                return false;
            }
            const auto posDelta = static_cast<Int32>(b.getCurPos()) - startPos;
            c.stream.recordBits(PacketStats::Receive, posDelta, tag);
            c.stream.recordBits(PacketStats::Receive, -posDelta, EventTypeFirst + 33);
            e.object = std::move(object);
            return true;
        },
        ReceivePolicy::Client);
}

struct EventManager::Impl {
    struct Entry {
        std::shared_ptr<SimEvent> event;
        std::optional<UInt64> sequence;
    };

    explicit Impl(std::size_t c) : capacity(c) {}

    std::deque<Entry> queue;
    std::map<UInt32, std::vector<Entry>> pending;
    std::array<std::unique_ptr<SimEvent>, 128> waiting;
    std::set<UInt64> outstanding;
    std::vector<SimAddress> polled;
    std::size_t capacity, count{};
    UInt64 nextSend{}, highSent{};
    UInt32 nextReceive{}, nextKey{1};
    bool driving{};
};

EventManager::EventManager(std::size_t capacity) : impl_(std::make_unique<Impl>(capacity)) {
    if (!capacity || capacity > 65536) {
        throw std::invalid_argument("invalid remote event capacity");
    }
}

EventManager::~EventManager() = default;

std::size_t EventManager::queued() const {
    return impl_->count;
}

void EventManager::onRemove() {
    impl_->queue.clear();
    impl_->pending.clear();
    impl_->outstanding.clear();
    std::ranges::for_each(impl_->waiting, [](auto& e) {
        e.reset();
    });
    impl_->count = 0;
    impl_->polled.clear();
}

bool EventManager::postRemoteEvent(std::unique_ptr<SimEvent> event) {
    if (!event || !live(*this) || !owner() || !owner()->error().empty() ||
        impl_->count >= impl_->capacity) {
        return false;
    }
    const auto* codec = owner()->registry().event(event->type);
    if (!codec || codec->type != std::type_index(typeid(*event)) ||
        ((event->flags & SimEvent::Ordered) && !(event->flags & SimEvent::Guaranteed))) {
        return false;
    }
    std::optional<UInt64> seq;
    if (event->flags & SimEvent::Ordered) {
        seq = impl_->nextSend++;
    }
    impl_->queue.push_back({std::move(event), seq});
    ++impl_->count;
    return true;
}

bool EventManager::postRemoteBatch(std::vector<Delivery> deliveries) {
    struct Prepared {
        std::deque<Impl::Entry> queue;
        std::size_t count{};
        UInt64 sequence{};
    };

    std::map<EventManager*, Prepared> prepared;
    try {
        for (auto& delivery : deliveries) {
            auto* target = delivery.target;
            const auto& event = delivery.event;
            if (!target || !event || !live(*target) || !target->owner() ||
                !target->owner()->error().empty()) {
                return false;
            }
            const auto* codec = target->owner()->registry().event(event->type);
            if (!codec || codec->type != std::type_index(typeid(*event)) ||
                ((event->flags & SimEvent::Ordered) && !(event->flags & SimEvent::Guaranteed))) {
                return false;
            }
            auto [entry, inserted] = prepared.try_emplace(target);
            auto& staged = entry->second;
            const auto& current = *target->impl_;
            if (inserted) {
                staged.queue = current.queue;
                staged.count = current.count;
                staged.sequence = current.nextSend;
            }
            if (staged.count >= current.capacity) {
                return false;
            }
            std::optional<UInt64> sequence;
            if (event->flags & SimEvent::Ordered) {
                if (staged.sequence == std::numeric_limits<UInt64>::max()) {
                    return false;
                }
                sequence = staged.sequence++;
            }
            staged.queue.push_back({std::move(delivery.event), sequence});
            ++staged.count;
        }
    } catch (...) {
        return false;
    }
    // No callbacks or allocations occur during commit.
    for (auto& [target, staged] : prepared) {
        target->impl_->queue.swap(staged.queue);
        target->impl_->count = staged.count;
        target->impl_->nextSend = staged.sequence;
    }
    return true;
}

bool EventManager::addPolledObject(SimObject& object) {
    if (!live(*this) || object.getManager() != getManager() || object.isRemoved() ||
        impl_->polled.size() >= 4096) {
        return false;
    }
    const auto address = object.address();
    if (std::ranges::any_of(impl_->polled, [&](const auto& a) {
            return a.objectId == address.objectId && a.generation == address.generation;
        })) {
        return true;
    }
    impl_->polled.push_back(address);
    return true;
}

bool EventManager::writePacket(BitStream& output, UInt32& key) {
    key = 0;
    if (!live(*this) || !owner()) {
        return false;
    }
    if (impl_->driving) {
        return false;
    }
    bool used = false;
    protectedCall(*this, [&] {
        DriveGuard drive(impl_->driving);
        const auto polled = impl_->polled;
        PolledUpdateEvent poll;
        poll.events = this;
        std::ranges::for_each(polled, [&](auto address) {
            if (live(*this)) {
                getManager()->sendEvent(address, poll);
            }
        });
        if (!live(*this) || !owner()) {
            return;
        }
        std::erase_if(impl_->polled, [&](auto a) {
            return !a.resolve(*getManager());
        });
        if (impl_->queue.empty() || output.remainingWriteBits() < 1) {
            return;
        }
        std::vector<Impl::Entry> notes;
        bool committed = false;
        Restore restore{[&] {
            if (!committed && live(*this)) {
                std::ranges::for_each(notes | std::views::reverse, [&](auto& e) {
                    impl_->queue.push_front(std::move(e));
                });
            }
        }};
        Int64 previous = -4;
        while (!impl_->queue.empty() && live(*this)) {
            if (std::size_t(output.getCurPos()) >= owner()->nominalWriteBits()) {
                break;
            }
            auto& front = impl_->queue.front();
            const auto oldest =
                impl_->outstanding.empty() ? impl_->highSent : *impl_->outstanding.begin();
            if (front.sequence && *front.sequence > oldest + 126) {
                break;
            }
            std::array<std::byte, net::MaxPayload> bytes{};
            BitStream trial{std::span(bytes)};
            trial.writeFlag(true);
            const bool guaranteed = (front.event->flags & SimEvent::Guaranteed) != 0;
            trial.writeFlag(guaranteed);
            const auto seq = front.sequence ? static_cast<Int64>(*front.sequence) : Int64{-2};
            if (guaranteed && !trial.writeFlag(seq == previous + 1)) {
                if (trial.writeFlag(seq != -2)) {
                    trial.writeInt(static_cast<Int32>(seq & 127), 7);
                }
            }
            trial.writeInt(front.event->type - 1024, 7);
            const auto packStart = static_cast<Int32>(trial.getCurPos());
            EventContext context{*getManager(), *owner(), owner()->getId(), owner()->isServer()};
            const auto* codec = owner()->registry().event(front.event->type);
            const auto encoded = codec && codec->pack(*front.event, trial, context);
            if (!live(*this) || !owner()) {
                break;
            }
            if (!encoded || !trial.isValid()) {
                owner()->fail("Remote event encoding failed");
                break;
            }
            const auto bits = static_cast<std::size_t>(trial.getCurPos());
            if (bits + 1 > output.remainingWriteBits()) {
                // An event that cannot fit even an otherwise empty physical
                // packet must fail explicitly instead of wedging the queue.
                const auto framing = 43 + owner()->objects().size();
                if (bits + 1 + framing > net::MaxPayload * 8) {
                    owner()->fail("Remote event exceeds physical packet capacity");
                }
                break;
            }
            output.writeBits(bits, bytes);
            used = true;
            owner()->recordBits(PacketStats::Send, static_cast<Int32>(bits) - 1, front.event->type);
            if (const auto* create = dynamic_cast<const RemoteCreateEvent*>(front.event.get());
                create && create->object) {
                const auto posDelta = static_cast<Int32>(bits) - packStart;
                owner()->recordBits(PacketStats::Send, posDelta, create->object->getGhostTag());
                owner()->recordBits(PacketStats::Send, -posDelta, front.event->type);
            }
            if (guaranteed) {
                previous = seq;
                if (front.sequence) {
                    impl_->outstanding.insert(*front.sequence);
                    impl_->highSent = std::max(impl_->highSent, *front.sequence);
                }
                notes.push_back(std::move(front));
            } else {
                --impl_->count;
            }
            impl_->queue.pop_front();
        }
        output.writeFlag(false);
        if (!notes.empty()) {
            key = newKey(impl_->nextKey);
            impl_->pending.emplace(key, std::move(notes));
        }
        committed = true;
    });
    return used;
}

void EventManager::packetReceived(UInt32 key) {
    auto found = impl_->pending.find(key);
    if (found == impl_->pending.end()) {
        return;
    }
    auto entries = std::move(found->second);
    impl_->pending.erase(found);
    std::ranges::for_each(entries, [&](auto& entry) {
        if (entry.sequence) {
            impl_->outstanding.erase(*entry.sequence);
        }
        --impl_->count;
    });
}

void EventManager::packetDropped(UInt32 key) {
    auto found = impl_->pending.find(key);
    if (found == impl_->pending.end()) {
        return;
    }
    auto entries = std::move(found->second);
    impl_->pending.erase(found);
    std::ranges::for_each(entries | std::views::reverse, [&](auto& entry) {
        impl_->queue.push_front(std::move(entry));
    });
}

bool EventManager::readPacket(BitStream& input, UInt32) {
    if (!live(*this) || !owner()) {
        return false;
    }
    if (impl_->driving) {
        return false;
    }
    bool ok = true;
    protectedCall(*this, [&] {
        DriveGuard drive(impl_->driving);
        struct Decoded {
            std::unique_ptr<SimEvent> event;
            Int32 seq;
        };
        std::vector<Decoded> decoded;
        Int32 previous = -4;
        while (input.readFlag()) {
            if (decoded.size() >= impl_->capacity) {
                ok = false;
                break;
            }
            const auto startPos = static_cast<Int32>(input.getCurPos());
            const auto guaranteed = input.readFlag();
            Int32 sequence = -2;
            if (guaranteed) {
                sequence = input.readFlag()   ? ((previous + 1) & 127)
                           : input.readFlag() ? input.readInt(7)
                                              : -2;
                previous = sequence;
            }
            const auto type = input.readInt(7) + 1024;
            const auto* codec = owner()->registry().event(type);
            if (!input.isValid() || !codec ||
                (owner()->isServer() ? codec->policy == ReceivePolicy::Client
                                     : codec->policy == ReceivePolicy::Server)) {
                owner()->fail("Invalid remote event type " + std::to_string(type) + " at bit " +
                              std::to_string(startPos));
                ok = false;
                break;
            }
            auto event = codec->create();
            event->type = type;
            event->sourceManagerId = owner()->getId();
            EventContext context{*getManager(), *owner(), owner()->getId(), owner()->isServer()};
            if (!codec->unpack(*event, input, context) || !input.isValid() || !live(*this)) {
                if (live(*this) && owner()) {
                    owner()->fail("Remote event " + std::to_string(type) +
                                  " decoding failed at bit " + std::to_string(input.getCurPos()));
                }
                ok = false;
                break;
            }
            owner()->recordBits(
                PacketStats::Receive, static_cast<Int32>(input.getCurPos()) - startPos, type);
            event->flags |= SimEvent::Remote | (guaranteed ? SimEvent::Guaranteed : 0U) |
                            (sequence != -2 ? SimEvent::Ordered : 0U);
            // Resolve ghost addresses now, before a waiting ordered event outlives
            // its ghost slot. Generations reject a later occupant of that slot.
            if (event->flags & (SimEvent::ToGhost | SimEvent::ToGhostParent)) {
                auto* gm = owner()->ghostManager();
                SimNetObject* target = nullptr;
                if (gm && event->address.objectId < InvalidGhostIndex) {
                    target =
                        (event->flags & SimEvent::ToGhost)
                            ? gm->resolveGhost(static_cast<UInt16>(event->address.objectId))
                            : gm->resolveGhostParent(static_cast<UInt16>(event->address.objectId));
                }
                event->address = target ? target->address() : SimAddress{~UInt32{0}, 0, 0};
            }
            if (event->address.managerId) {
                owner()->fail("Invalid remote address for event " + std::to_string(type) +
                              ": manager " + std::to_string(event->address.managerId));
                ok = false;
                break;
            }
            decoded.push_back({std::move(event), sequence});
        }
        if (!ok || !input.isValid()) {
            ok = false;
            if (owner()) {
                owner()->fail("Invalid remote event packet");
            }
            return;
        }
        const auto deliver = [&](SimEvent& e) {
            if (!live(*this)) {
                return;
            }
            if (auto* create = dynamic_cast<RemoteCreateEvent*>(&e)) {
                if (!create->object || !getManager()->addObject(std::move(create->object))) {
                    ok = false;
                    if (owner()) {
                        owner()->fail("Remote object registration failed");
                    }
                }
            } else {
                getManager()->sendEvent(e.address, e);
            }
        };
        std::ranges::for_each(decoded, [&](auto& entry) {
            if (!live(*this) || !ok) {
                return;
            }
            if (entry.seq == -2) {
                deliver(*entry.event);
                return;
            }
            const auto seq = static_cast<UInt32>(entry.seq);
            const auto distance = (seq - impl_->nextReceive) & 127;
            if (distance == 127 || impl_->waiting[seq]) {
                return;
            }
            impl_->waiting[seq] = std::move(entry.event);
            while (live(*this) && ok && impl_->waiting[impl_->nextReceive]) {
                auto event = std::move(impl_->waiting[impl_->nextReceive]);
                impl_->nextReceive = (impl_->nextReceive + 1) & 127;
                deliver(*event);
            }
        });
    });
    return ok;
}
} // namespace network
} // namespace opent1::sim
