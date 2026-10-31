#include <opent1/sim/sim_net.hpp>
#include <opent1/sim/sim_world_game.hpp>
#include <algorithm>
#include <bit>
#include <cmath>
#include <numeric>
#include <stdexcept>

namespace opent1::sim::network {
namespace {
struct Rollback {
    std::function<void()> action;

    ~Rollback() {
        action();
    }
};
}

struct GhostManager::Impl {
    struct Entry {
        SimAddress source;
        SimNetObject* sourceObject{}; // paired with SimNetObject::observers_
        UInt32 mask{~UInt32{0}}, skips{};
        bool used{}, inScope{}, always{}, fresh{}, creating{}, kill{}, killing{}, direct{};

        struct Update {
            UInt32 key, mask;
        };

        std::vector<Update> chain;
    };

    struct Local {
        SimAddress address;
        SimNetObject* object{};
        std::unique_ptr<SimNetObject> staged;
        UInt32 number{};
    };

    struct Note {
        UInt16 index;
        UInt32 mask;
        bool create{}, kill{};
    };

    std::array<Entry, InvalidGhostIndex> entries;
    std::array<Local, InvalidGhostIndex> locals;
    std::deque<UInt16> free;
    std::vector<UInt16> active; // newest first, preserving legacy traversal
    std::map<UInt32, std::vector<Note>> pending;
    SimAddress scope;
    UInt32 nextKey{1};
    Int32 idBits{3};
    Mode mode{Mode::Inactive};
    bool allow{}, driving{};

    explicit Impl(bool allowed) : allow(allowed) {
        std::ranges::for_each(std::views::iota(UInt16{0}, InvalidGhostIndex), [&](auto i) {
            free.push_back(i);
        });
    }
};

GhostManager::GhostManager(bool allow) : impl_(std::make_unique<Impl>(allow)) {}

GhostManager::~GhostManager() = default;

GhostManager::Mode GhostManager::mode() const {
    return impl_->mode;
}

void GhostManager::activate() {
    if (impl_->driving) {
        return;
    }
    impl_->mode = Mode::ScopeAlways;
    // A connection may spend many packets downloading datablocks while
    // ghosting is inactive. Rebuild the service scope at activation time.
    if (getManager()) {
        if (auto* set = dynamic_cast<SimSet*>(getManager()->findObject(GhostAlwaysSetId))) {
            for (auto* object : set->objects()) {
                if (auto* net = dynamic_cast<SimNetObject*>(object)) {
                    objectInScope(*net);
                }
            }
        }
    }
}

void GhostManager::reset() {
    if (!impl_->driving) {
        impl_->mode = Mode::Inactive;
    }
}

SimNetObject* GhostManager::scopeObject() const {
    return getManager() ? dynamic_cast<SimNetObject*>(impl_->scope.resolve(*getManager()))
                        : nullptr;
}

bool GhostManager::setScopeObject(SimNetObject* object) {
    if (impl_->driving || (object && (object->getManager() != getManager() || object->isRemoved() ||
                                      object->isGhost()))) {
        return false;
    }
    impl_->scope = object ? object->address() : SimAddress{};
    return true;
}

bool GhostManager::onAdd() {
    addToSet(GhostManagerSetId);
    if (auto* set = dynamic_cast<SimSet*>(getManager()->findObject(GhostAlwaysSetId))) {
        std::ranges::for_each(set->objects(), [&](auto* object) {
            if (auto* net = dynamic_cast<SimNetObject*>(object)) {
                objectInScope(*net);
            }
        });
    }
    return true;
}

void GhostManager::onRemove() {
    if (!getManager()) {
        return;
    }
    std::ranges::for_each(impl_->active, [&](auto index) {
        if (auto* source = impl_->entries[index].sourceObject) {
            std::erase(source->observers_, this);
        }
    });
    impl_->active.clear();
    impl_->pending.clear();
    std::ranges::for_each(impl_->locals, [&](auto& local) {
        auto* object = local.object;
        local.address = {};
        local.object = nullptr;
        local.staged.reset();
        if (object && object->getManager() == getManager() && !object->isRemoved()) {
            object->deleteObject();
        }
    });
}

void GhostManager::onDeleteNotify(SimObject& object) {
    std::ranges::for_each(impl_->locals, [&](auto& local) {
        if (local.object == &object) {
            local.object = nullptr;
            local.address = {};
        }
    });
    SimObject::onDeleteNotify(object);
}

bool GhostManager::objectInScope(SimNetObject& object) {
    if (!getManager() || object.getManager() != getManager() || object.isRemoved() ||
        object.isGhost() || !(object.netFlags() & SimNetObject::Ghostable) ||
        ((object.netFlags() & SimNetObject::ScopeLocal) &&
         (!owner() || owner()->getId() != 2049))) {
        return false;
    }
    const auto address = object.address();
    const auto found = std::ranges::find_if(impl_->active, [&](auto i) {
        const auto& e = impl_->entries[i];
        return e.sourceObject == &object && !e.kill && !e.killing;
    });
    if (found != impl_->active.end()) {
        auto& e = impl_->entries[*found];
        e.inScope = true;
        e.source = address;
        return true;
    }
    if (impl_->free.empty()) {
        return false;
    }
    const auto index = impl_->free.front();
    impl_->free.pop_front();
    auto& entry = impl_->entries[index];
    entry = Impl::Entry{};
    entry.source = address;
    entry.sourceObject = &object;
    entry.used = entry.inScope = entry.fresh = true;
    entry.always = (object.netFlags() & SimNetObject::ScopeAlways) != 0;
    impl_->active.insert(impl_->active.begin(), index);
    object.observers_.push_back(this);
    impl_->idBits = std::max(impl_->idBits, static_cast<Int32>(std::bit_width(UInt32(index))));
    return true;
}

void GhostManager::setMaskBits(const SimNetObject& object, UInt32 mask) {
    std::ranges::for_each(impl_->active, [&](auto i) {
        auto& e = impl_->entries[i];
        if (e.sourceObject == &object) {
            e.mask |= mask;
            e.source = object.address();
        }
    });
}

UInt32 GhostManager::takeUpdateMask(const SimNetObject& object) {
    for (const auto index : impl_->active) {
        auto& entry = impl_->entries[index];
        if (entry.sourceObject == &object && !entry.kill) {
            // Direct delivery has no packet acknowledgement to retire a dead
            // source, including one whose client creation never succeeded.
            if (entry.chain.empty() && !entry.creating && !entry.killing) {
                entry.direct = true;
            }
            const auto mask = entry.mask;
            entry.mask = 0;
            return mask;
        }
    }
    return 0;
}

bool GhostManager::registerDirectGhost(SimNetObject& source,
                                       GhostManager& receiver,
                                       SimNetObject& ghost) {
    if (impl_->driving || receiver.impl_->driving || !getManager() || !receiver.getManager() ||
        source.getManager() != getManager() || source.isGhost() || source.isRemoved() ||
        source.isDeleted() || ghost.getManager() != receiver.getManager() || !ghost.isGhost() ||
        ghost.isRemoved() || ghost.isDeleted() || !receiver.impl_->allow ||
        source.getGhostTag() != ghost.getGhostTag()) {
        return false;
    }
    const auto found = std::ranges::find_if(impl_->active, [&](auto index) {
        const auto& entry = impl_->entries[index];
        return entry.sourceObject == &source && !entry.kill && !entry.killing && !entry.creating &&
               entry.chain.empty();
    });
    if (found == impl_->active.end()) {
        return false;
    }
    const UInt16 index = *found;
    auto& local = receiver.impl_->locals[index];
    if (local.staged || (local.object && local.object != &ghost) ||
        (ghost.netIndex_ != InvalidGhostIndex && ghost.netIndex_ != index)) {
        return false;
    }
    receiver.deleteNotify(ghost);
    local.address = ghost.address();
    local.object = &ghost;
    ghost.netIndex_ = index;
    auto& entry = impl_->entries[index];
    entry.direct = true;
    entry.fresh = false;
    if ((ghost.netFlags_ & SimNetObject::PolledGhost) && receiver.owner()) {
        if (auto* events = receiver.owner()->eventManager()) {
            events->addPolledObject(ghost);
        }
    }
    return true;
}

void GhostManager::removeSource(SimNetObject& object) {
    std::erase_if(impl_->active, [&](auto i) {
        auto& e = impl_->entries[i];
        if (e.sourceObject != &object) {
            return false;
        }
        if (e.direct && e.chain.empty()) {
            e = Impl::Entry{};
            impl_->free.push_front(i);
            return true;
        }
        e.kill = true;
        e.source = {};
        e.sourceObject = nullptr;
        return false;
    });
    std::erase(object.observers_, this);
}

SimNetObject* GhostManager::resolveGhost(UInt16 index) const {
    if (index >= InvalidGhostIndex || !getManager()) {
        return nullptr;
    }
    const auto& local = impl_->locals[index];
    return local.staged ? local.staged.get()
           : local.object && local.object->getManager() == getManager() &&
                   !local.object->isRemoved()
               ? local.object
               : nullptr;
}

SimNetObject* GhostManager::resolveGhostParent(UInt16 index) const {
    if (index >= InvalidGhostIndex || !getManager() || !impl_->entries[index].used ||
        !impl_->entries[index].source.generation) {
        return nullptr;
    }
    auto* object = impl_->entries[index].sourceObject;
    return object && object->getManager() == getManager() && !object->isRemoved() ? object
                                                                                  : nullptr;
}

UInt16 GhostManager::getGhostIndex(const SimNetObject& object) const {
    const auto found = std::ranges::find_if(impl_->active, [&](auto i) {
        const auto& e = impl_->entries[i];
        return e.sourceObject == &object && !e.fresh && !e.creating && !e.kill && !e.killing;
    });
    return found == impl_->active.end() ? InvalidGhostIndex : *found;
}

bool GhostManager::writePacket(BitStream& output, UInt32& key) {
    key = 0;
    if (!getManager() || isRemoved() || !owner() || impl_->driving ||
        (impl_->mode == Mode::Inactive && impl_->active.empty())) {
        return false;
    }
    bool result = false;
    getManager()->dispatch([&] {
        impl_->driving = true;
        struct Guard {
            bool& b;
            ~Guard() {
                b = false;
            }
        } guard{impl_->driving};
        const auto active = [&] {
            return getManager() && !isRemoved() && owner();
        };
        const auto release = [&](UInt16 i) {
            auto& e = impl_->entries[i];
            if (auto* source = resolveGhostParent(i)) {
                std::erase(source->observers_, this);
            }
            e = Impl::Entry{};
            std::erase(impl_->active, i);
            impl_->free.push_front(i);
        };
        const auto always = impl_->mode == Mode::ScopeAlways;
        SimNetObject::CameraInfo camera;
        if (!always) {
            std::ranges::for_each(impl_->active, [&](auto i) {
                auto& e = impl_->entries[i];
                e.inScope = e.always && impl_->mode != Mode::Inactive;
            });
            if (impl_->mode == Mode::Normal && impl_->scope.generation) {
                if (auto* scope =
                        dynamic_cast<SimNetObject*>(impl_->scope.resolve(*getManager()))) {
                    scope->buildScopeAndCameraInfo(*this, camera);
                }
            }
            if (!active()) {
                return;
            }
            const auto snapshot = impl_->active;
            std::ranges::for_each(snapshot, [&](auto i) {
                auto& e = impl_->entries[i];
                if (!e.inScope) {
                    if (auto* source = resolveGhostParent(i)) {
                        removeSource(*source);
                    } else {
                        e.kill = true;
                    }
                }
                if (e.kill && e.fresh && e.chain.empty()) {
                    release(i);
                }
            });
        }
        struct Priority {
            UInt16 index;
            Flt32 priority;
            UInt64 number;
        };
        std::vector<Priority> updates;
        std::vector<UInt16> deletes;
        const auto snapshot = impl_->active;
        std::ranges::for_each(snapshot, [&](auto i) {
            if (!active()) {
                return;
            }
            auto& e = impl_->entries[i];
            if (!e.used || e.creating || e.killing) {
                return;
            }
            if (e.kill) {
                deletes.push_back(i);
                return;
            }
            auto* source = resolveGhostParent(i);
            if (!source) {
                e.kill = true;
                return;
            }
            if (always && e.fresh) {
                updates.push_back({i, 0, source->getNumber()});
            } else if (!always && e.mask) {
                const auto priority = impl_->scope.generation
                                          ? source->getUpdatePriority(camera, e.mask, e.skips)
                                          : 1.f;
                if (!std::isfinite(priority)) {
                    owner()->fail("Nonfinite ghost priority");
                    return;
                }
                updates.push_back({i, priority, source->getNumber()});
            }
        });
        if (!active() || !owner()->error().empty()) {
            return;
        }
        std::ranges::stable_sort(updates, [&](const auto& a, const auto& b) {
            return always ? a.number < b.number : a.priority > b.priority;
        });
        std::vector<UInt16> order = std::move(deletes);
        std::ranges::for_each(updates, [&](const auto& u) {
            order.push_back(u.index);
        });
        if (output.remainingWriteBits() < std::size_t(always ? 6 : 5)) {
            return;
        }
        output.writeFlag(always);
        output.writeInt(impl_->idBits - 3, 3);
        // Allocate the numeric token before callbacks; token zero never denotes a pointer.
        if (!impl_->nextKey) {
            throw std::overflow_error("ghost notification key exhausted");
        }
        const auto token = impl_->nextKey++;
        auto& notes = impl_->pending.emplace(token, std::vector<Impl::Note>{}).first->second;
        notes.reserve(order.size());
        bool committed = false;
        Rollback rollback{[&] {
            if (!committed && active()) {
                packetDropped(token);
                key = 0;
            }
        }};
        std::ranges::for_each(order, [&](auto i) {
            if (!active() || !owner()->error().empty()) {
                return;
            }
            auto& e = impl_->entries[i];
            if (!e.used || e.creating || e.killing) {
                return;
            }
            e.chain.reserve(e.chain.size() + 1);
            std::array<std::byte, net::MaxPayload> bytes{};
            BitStream trial{std::span(bytes)};
            trial.writeFlag(true);
            trial.writeInt(i, impl_->idBits);
            trial.writeFlag(e.kill);
            const auto creation = e.fresh;
            const auto killing = e.kill;
            const auto mask = e.mask;
            UInt32 remaining = mask;
            UInt16 ghostTag{};
            if (!killing) {
                auto* source = resolveGhostParent(i);
                if (!source) {
                    return;
                }
                ghostTag = source->getGhostTag();
                if (creation) {
                    if (!ghostTag || ghostTag >= 1024) {
                        owner()->fail("Invalid ghost class tag");
                        return;
                    }
                    if (always) {
                        trial.writeInt(
                            std::bit_cast<Int32>(static_cast<UInt32>(source->getNumber())), 32);
                    }
                    trial.writeInt(ghostTag, 10);
                }
                // Clear before calling the encoder so mask bits set reentrantly
                // are retained independently of the mask being transmitted.
                e.mask = 0;
                try {
                    remaining = source->packUpdate(*this, mask, trial);
                } catch (...) {
                    e.mask |= mask;
                    throw;
                }
                if (!active()) {
                    return;
                }
                if (!trial.isValid() || (remaining & ~mask)) {
                    e.mask |= mask;
                    owner()->fail("Ghost encoding failed");
                    return;
                }
            }
            const auto bits = static_cast<std::size_t>(trial.getCurPos());
            if (bits + std::size_t(always ? 2 : 1) > output.remainingWriteBits()) {
                e.mask |= mask;
                ++e.skips;
                return;
            }
            output.writeBits(bits, bytes);
            e.mask |= remaining;
            e.skips = 0;
            if (!killing && ghostTag) {
                owner()->recordBits(PacketStats::Send, static_cast<Int32>(bits), ghostTag);
            }
            if (killing) {
                e.kill = false;
                e.killing = true;
            } else if (creation) {
                e.fresh = false;
                e.creating = true;
            }
            const auto sent = mask & ~remaining;
            notes.push_back({i, sent, creation && !killing, killing});
            e.chain.push_back({token, sent});
        });
        if (!active()) {
            return;
        }
        output.writeFlag(false);
        if (!notes.empty()) {
            key = token;
        } else {
            impl_->pending.erase(token);
        }
        result = output.isValid();
        if (always) {
            const auto done = std::ranges::none_of(impl_->active, [&](auto i) {
                const auto& e = impl_->entries[i];
                return e.fresh || e.creating;
            });
            output.writeFlag(done);
            if (done) {
                impl_->mode = Mode::Normal;
                const auto callback = owner()->onGhostAlwaysDone;
                if (callback) {
                    callback();
                }
            }
        }
        committed = true;
    });
    return result;
}

void GhostManager::packetReceived(UInt32 key) {
    auto found = impl_->pending.find(key);
    if (found == impl_->pending.end()) {
        return;
    }
    auto notes = std::move(found->second);
    impl_->pending.erase(found);
    std::ranges::for_each(notes, [&](const auto& note) {
        auto& e = impl_->entries[note.index];
        std::erase_if(e.chain, [&](auto u) {
            return u.key == key;
        });
        if (note.create) {
            e.creating = false;
        }
        if (note.kill) {
            e.killing = false;
            if (e.chain.empty()) {
                e = Impl::Entry{};
                std::erase(impl_->active, note.index);
                impl_->free.push_front(note.index);
            }
        }
    });
}

void GhostManager::packetDropped(UInt32 key) {
    auto found = impl_->pending.find(key);
    if (found == impl_->pending.end()) {
        return;
    }
    auto notes = std::move(found->second);
    impl_->pending.erase(found);
    std::ranges::for_each(notes, [&](const auto& note) {
        auto& e = impl_->entries[note.index];
        UInt32 later{};
        bool after = false;
        std::ranges::for_each(e.chain, [&](auto u) {
            if (after) {
                later |= u.mask;
            }
            if (u.key == key) {
                after = true;
            }
        });
        e.mask |= note.mask & ~later;
        std::erase_if(e.chain, [&](auto u) {
            return u.key == key;
        });
        if (note.create) {
            e.creating = false;
            e.fresh = true;
        }
        if (note.kill) {
            e.killing = false;
            e.kill = true;
        }
    });
}

bool GhostManager::readPacket(BitStream& input, UInt32) {
    if (!getManager() || isRemoved() || !owner() || impl_->driving) {
        return false;
    }
    bool ok = true;
    getManager()->dispatch([&] {
        impl_->driving = true;
        struct Guard {
            bool& b;
            ~Guard() {
                b = false;
            }
        } guard{impl_->driving};
        const auto active = [&] {
            return getManager() && !isRemoved() && owner();
        };
        const auto flush = [&] {
            std::vector<UInt16> staged;
            std::ranges::for_each(std::views::iota(UInt16{0}, InvalidGhostIndex), [&](auto i) {
                if (impl_->locals[i].staged) {
                    staged.push_back(i);
                }
            });
            std::ranges::stable_sort(staged, [&](auto a, auto b) {
                return impl_->locals[a].number < impl_->locals[b].number;
            });
            std::ranges::for_each(staged, [&](auto i) {
                if (!active() || !ok) {
                    return;
                }
                const auto tag = impl_->locals[i].staged->getGhostTag();
                if (auto* existing =
                        impl_->locals[i].staged->getId()
                            ? getManager()->findObject(impl_->locals[i].staged->getId())
                            : nullptr) {
                    auto* net = dynamic_cast<SimNetObject*>(existing);
                    // Native dictionaries allow repeated service IDs (notably
                    // MissionCenterPos); the newest ghost owns their lookup.
                    // Retain older ghosts under dynamic IDs so both network
                    // indices remain usable without aliasing manager objects.
                    if (!net || !net->isGhost() || net->getGhostTag() != tag ||
                        !getManager()->assignId(*net, 0)) {
                        owner()->fail("Ghost ID conflict for tag " + std::to_string(tag));
                        ok = false;
                        return;
                    }
                }
                auto* object = getManager()->addObject(std::move(impl_->locals[i].staged));
                if (!active()) {
                    return;
                }
                if (!object) {
                    owner()->fail("Ghost initialization failed for tag " + std::to_string(tag));
                    ok = false;
                    return;
                }
                impl_->locals[i].address = object->address();
                impl_->locals[i].object = static_cast<SimNetObject*>(object);
                deleteNotify(*object);
                if (auto* net = dynamic_cast<SimNetObject*>(object);
                    net && (net->netFlags() & SimNetObject::PolledGhost)) {
                    if (auto* events = owner()->eventManager()) {
                        events->addPolledObject(*net);
                    }
                }
            });
            if (active() && ok) {
                const auto callback = owner()->onGhostAlwaysDone;
                if (callback) {
                    callback();
                }
            }
        };
        const auto always = input.readFlag();
        const auto idBits = input.readInt(3) + 3;
        if (!input.isValid()) {
            ok = false;
            return;
        }
        if (!always && std::ranges::any_of(impl_->locals, [](const auto& l) {
                return bool(l.staged);
            })) {
            flush();
        }
        std::size_t records{};
        while (active() && ok && input.readFlag()) {
            if (!impl_->allow || ++records > InvalidGhostIndex) {
                ok = false;
                break;
            }
            const auto startPos = static_cast<Int32>(input.getCurPos());
            const auto index = static_cast<UInt16>(input.readInt(idBits));
            const auto killing = input.readFlag();
            if (!input.isValid() || index >= InvalidGhostIndex) {
                ok = false;
                break;
            }
            auto& local = impl_->locals[index];
            if (killing) {
                auto* object = local.object;
                local.address = {};
                local.object = nullptr;
                local.staged.reset();
                if (object && object->getManager() == getManager() && !object->isRemoved()) {
                    object->deleteObject();
                }
            } else if (auto* object = resolveGhost(index)) {
                const auto tag = object->getGhostTag();
                ok = object->unpackUpdate(*this, input);
                if (!ok && active()) {
                    owner()->fail("Ghost update decoding failed for tag " + std::to_string(tag) +
                                  " at bit " + std::to_string(startPos));
                }
                if (ok && input.isValid() && active() && owner() && tag) {
                    owner()->recordBits(PacketStats::Receive,
                                        static_cast<Int32>(input.getCurPos()) - startPos,
                                        tag);
                }
            } else {
                const auto number = always ? std::bit_cast<UInt32>(input.readInt(32)) : 0U;
                const auto tag = static_cast<UInt16>(input.readInt(10));
                auto created = owner()->registry().createGhost(tag);
                if (!created || !input.isValid()) {
                    owner()->fail("Unknown ghost class " + std::to_string(tag));
                    ok = false;
                    break;
                }
                created->netFlags_ |= SimNetObject::IsGhost;
                created->netIndex_ = index;
                if (owner()->prepareGhost && !owner()->prepareGhost(*created)) {
                    ok = false;
                    break;
                }
                if (!created->unpackUpdate(*this, input) || !input.isValid()) {
                    if (active()) {
                        owner()->fail("Initial ghost update decoding failed for tag " +
                                      std::to_string(tag) + " at bit " + std::to_string(startPos));
                    }
                    ok = false;
                    break;
                }
                if (!active()) {
                    break;
                }
                if (owner() && tag) {
                    owner()->recordBits(PacketStats::Receive,
                                        static_cast<Int32>(input.getCurPos()) - startPos,
                                        tag);
                }
                local.number = number;
                if (always) {
                    local.staged = std::move(created);
                } else {
                    auto* added = getManager()->addObject(std::move(created));
                    if (!active()) {
                        break;
                    }
                    if (!added) {
                        owner()->fail("Ghost initialization failed for tag " + std::to_string(tag));
                        ok = false;
                        break;
                    }
                    local.address = added->address();
                    local.object = static_cast<SimNetObject*>(added);
                    deleteNotify(*added);
                    if ((static_cast<SimNetObject*>(added)->netFlags() &
                         SimNetObject::PolledGhost) &&
                        owner()->eventManager()) {
                        owner()->eventManager()->addPolledObject(*added);
                    }
                }
            }
            if (!input.isValid()) {
                ok = false;
            }
        }
        if (active() && ok && always) {
            const auto done = input.readFlag();
            if (input.isValid() && done) {
                flush();
            }
        }
        ok = ok && input.isValid();
        if (active() && !ok) {
            owner()->fail("Invalid ghost packet");
        }
    });
    return ok;
}
} // namespace opent1::sim::network
