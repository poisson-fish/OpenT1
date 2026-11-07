#include <opent1/sim/sim_net.hpp>
#include <opent1/sim/sim_world_game.hpp>
#include "sim_net_demo_internal.hpp"
#include <algorithm>
#include <exception>
#include <stdexcept>

namespace opent1::sim::network {
PacketStream::PacketStream(std::shared_ptr<const Registry> registry, bool server, Mode mode)
    : registry_(std::move(registry)), server_(server), mode_(mode) {
    if (!registry_) {
        throw std::invalid_argument("network registry required");
    }
    setStable(true);
}

PacketStream::~PacketStream() = default;

bool PacketStream::onAdd() {
    addToSet(PacketStreamSetId);
    return true;
}

void PacketStream::onBeforeRemove() {
    if (demoState() == DemoState::Recording && !demo_->busy) {
        (void)stopRecording();
    }
    if (demoState() == DemoState::Playback) {
        finishPlayback(false);
    }
}

void PacketStream::onRemove() {
    disconnect(now_);
    pending_.clear();
    std::ranges::for_each(objects_, [](auto* object) {
        static_cast<PacketStreamClient*>(object)->owner_ = nullptr;
    });
}

bool PacketStream::addObject(SimObject& object) {
    auto* client = dynamic_cast<PacketStreamClient*>(&object);
    if (!client || driving_ || receiving_ || demoState() == DemoState::Recording ||
        demoState() == DemoState::Playback || objects_.size() >= 32 ||
        (client->owner_ && client->owner_ != this) ||
        (dynamic_cast<EventManager*>(client) && eventManager()) ||
        (dynamic_cast<GhostManager*>(client) && ghostManager())) {
        return false;
    }
    if (!SimGroup::addObject(object)) {
        return false;
    }
    client->owner_ = this;
    if (auto* ghosts = dynamic_cast<GhostManager*>(client); ghosts && getManager()) {
        if (auto* set = dynamic_cast<SimSet*>(getManager()->findObject(GhostAlwaysSetId))) {
            std::ranges::for_each(set->objects(), [&](auto* member) {
                if (auto* net = dynamic_cast<SimNetObject*>(member)) {
                    ghosts->objectInScope(*net);
                }
            });
        }
    }
    return true;
}

bool PacketStream::removeObject(SimObject& object) {
    const auto address = object.address();
    if (!SimGroup::removeObject(object)) {
        return false;
    }
    if (!isRemoved()) {
        fail("Packet client removed");
    }
    if (auto* client = dynamic_cast<PacketStreamClient*>(&object)) {
        client->owner_ = nullptr;
    }
    std::ranges::for_each(pending_, [&](auto& pending) {
        std::erase_if(pending.second.notes, [&](const auto& n) {
            return n.client.generation == address.generation;
        });
    });
    return true;
}

EventManager* PacketStream::eventManager() const {
    const auto it = std::ranges::find_if(objects_, [](auto* p) {
        return dynamic_cast<EventManager*>(p) != nullptr;
    });
    return it == objects_.end() ? nullptr : static_cast<EventManager*>(*it);
}

GhostManager* PacketStream::ghostManager() const {
    const auto it = std::ranges::find_if(objects_, [](auto* p) {
        return dynamic_cast<GhostManager*>(p) != nullptr;
    });
    return it == objects_.end() ? nullptr : static_cast<GhostManager*>(*it);
}

bool PacketStream::sendRemoteEvent(std::unique_ptr<SimEvent> event, bool throughGhost) {
    if (!event || !getManager() || isRemoved()) {
        return false;
    }
    if (throughGhost) {
        auto* object = dynamic_cast<SimNetObject*>(event->address.resolve(*getManager()));
        auto* gm = ghostManager();
        if (!object || !gm) {
            return false;
        }
        const auto index = object->isGhost() ? object->getNetIndex() : gm->getGhostIndex(*object);
        if (index == InvalidGhostIndex) {
            return false;
        }
        event->flags |= object->isGhost() ? SimEvent::ToGhostParent : SimEvent::ToGhost;
        event->address = {index, getId(), 0};
    }
    auto* events = eventManager();
    return events && events->postRemoteEvent(std::move(event));
}

bool PacketStream::setMaxRate(UInt32 rate, UInt32 size, UInt16 wireLimit) {
    if (driving_ || receiving_ || wireLimit < 450 || wireLimit > 1023) {
        return false;
    }
    packetSizeLimit_ = wireLimit;
    rate = std::clamp(rate, 1U, 30U);
    size = std::clamp(size, 100U, UInt32(wireLimit));
    Rate maximum{1000 / rate, static_cast<UInt16>(size)};
    if (maximum != maxRate_) {
        maxRate_ = maximum;
        maxChanged_ = true;
    }
    return true;
}

void PacketStream::recordBits(PacketStats::Direction dir, Int32 count, Int32 persistTag) noexcept {
    if (driving_ || receiving_) {
        stagedStats_.addBits(dir, count, persistTag);
    } else {
        stats_.addBits(dir, count, persistTag);
        globalStats().addBits(dir, count, persistTag);
    }
}

PacketStats& PacketStream::globalStats() noexcept {
    static PacketStats pstats{};
    return pstats;
}

void PacketStream::fail(std::string_view error) {
    if (!error_.empty()) {
        return;
    }
    error_ = error.substr(0, 1023);
    if (getManager() && getManager()->console()) {
        getManager()->console()->print("Network stream " + std::to_string(getId()) + ": " + error_);
    }
}

std::optional<PacketStream::Packet> PacketStream::writePacket() {
    if (!getManager() || isRemoved() || driving_ || receiving_ ||
        demoState() == DemoState::Playback || !error_.empty() || pending_.size() >= 32) {
        return {};
    }
    std::optional<Packet> result;
    getManager()->dispatch([&] {
        driving_ = true;
        struct Guard {
            bool& b;
            ~Guard() {
                b = false;
            }
        } guard{driving_};
        stagedStats_.reset();
        // Legacy packetSize is a scheduling budget, not the physical write limit.
        // Reserve datagram headroom so a whole reliable event can cross it.
        Packet packet;
        packet.bytes.resize(net::MaxPayload);
        BitStream output{std::span(packet.bytes)};
        Pending pending;
        pending.rate = rateChanged_;
        pending.maximum = maxChanged_;
        pending.sequence = packetSequence_;
        auto used = rateChanged_ || maxChanged_;
        output.writeFlag(rateChanged_);
        if (rateChanged_) {
            output.writeInt(static_cast<Int32>(currentRate_.updateDelay), 10);
            output.writeInt(currentRate_.packetSize, 10);
        }
        output.writeFlag(maxChanged_);
        if (maxChanged_) {
            output.writeInt(static_cast<Int32>(maxRate_.updateDelay), 10);
            output.writeInt(maxRate_.packetSize, 10);
        }
        const auto snapshot = objects_;
        const auto rollback = [&] {
            stagedStats_.reset();
            std::ranges::for_each(pending.notes, [&](const auto& n) {
                if (getManager()) {
                    if (auto* cl =
                            dynamic_cast<PacketStreamClient*>(n.client.resolve(*getManager()))) {
                        cl->packetDropped(n.key);
                    }
                }
            });
        };
        try {
            std::size_t remaining = snapshot.size();
            std::ranges::for_each(snapshot, [&](auto* object) {
                --remaining;
                if (!getManager() || isRemoved() || !error_.empty()) {
                    return;
                }
                auto* client = dynamic_cast<PacketStreamClient*>(object);
                // Client layout is a negotiated positional contract. Removal is
                // terminal for the stream, never silently shifts wire fields.
                if (!client || client->isRemoved() || client->owner() != this) {
                    fail("Packet client layout changed");
                    return;
                }
                std::array<std::byte, net::MaxPayload> scratch{};
                BitStream payload{std::span(scratch)};
                if (output.remainingWriteBits() <= remaining) {
                    fail("Packet header overflow");
                    return;
                }
                const auto budget = output.remainingWriteBits() - remaining - 1;
                const auto nominal = std::size_t(currentRate_.packetSize) * 8;
                const auto consumed = std::size_t(output.getCurPos()) + remaining + 1;
                nominalWriteBits_ = nominal > consumed ? nominal - consumed : 0;
                payload.setLimits(scratch.size() * 8, budget);
                if (dynamic_cast<GhostManager*>(client)) {
                    payload.setLimits(scratch.size() * 8, std::min(budget, nominalWriteBits_));
                }
                UInt32 key{};
                bool present = false;
                try {
                    present = client->writePacket(payload, key);
                } catch (...) {
                    if (key && !client->isRemoved()) {
                        client->packetDropped(key);
                    }
                    throw;
                }
                if (key && !client->isRemoved()) {
                    pending.notes.push_back({client->address(), key});
                }
                if (!getManager() || isRemoved()) {
                    return;
                }
                if (!payload.isValid() || client->isRemoved() || client->owner() != this) {
                    fail("Packet client encoding failed");
                    return;
                }
                output.writeFlag(present);
                if (present) {
                    output.writeBits(static_cast<std::size_t>(payload.getCurPos()), scratch);
                    used = true;
                }
            });
        } catch (...) {
            rollback();
            throw;
        }
        if (!getManager() || isRemoved() || !error_.empty() || !output.isValid() || !used) {
            rollback();
            return;
        }
        if (!nextKey_) {
            rollback();
            throw std::overflow_error("packet notification key exhausted");
        }
        packet.key = nextKey_++;
        packet.bits = static_cast<std::size_t>(output.getCurPos());
        packet.bytes.resize((packet.bits + 7) / 8);
        pending_.emplace(packet.key, std::move(pending));
        rateChanged_ = maxChanged_ = false;
        result = std::move(packet);
        ++packetSequence_;
        stagedStats_.totalSent = 1;
        lastSentStats_ = stagedStats_;
        stats_.add(stagedStats_);
        globalStats().add(stagedStats_);
        stagedStats_.reset();
    });
    return result;
}

bool PacketStream::readPacket(std::span<const std::byte> bytes,
                              UInt32 now,
                              std::optional<std::size_t> bits) {
    if (!getManager() || isRemoved() || driving_ || receiving_ || !error_.empty() ||
        bytes.empty() || bytes.size() > net::MaxPayload || (bits && *bits > bytes.size() * 8)) {
        return false;
    }
    bool result = false;
    getManager()->dispatch([&] {
        receiving_ = true;
        struct Guard {
            bool& driving;
            std::string& error;
            int exceptions{std::uncaught_exceptions()};
            ~Guard() {
                driving = false;
                if (std::uncaught_exceptions() > exceptions && error.empty()) {
                    error = "Simulation packet callback failed";
                }
            }
        } guard{receiving_, error_};
        now_ = now;
        stagedStats_.reset();
        if (demoState() == DemoState::Recording) {
            std::vector<std::byte> record{
                std::byte{255}, std::byte(bytes.size() & 255), std::byte(bytes.size() >> 8)};
            record.insert(record.end(), bytes.begin(), bytes.end());
            if (!appendDemo(record, now)) {
                fail("Demo recording limit or time order");
                return;
            }
        }
        BitStream input(bytes);
        if (bits) {
            input.setLimits(*bits, 0);
        }
        auto current = currentRate_;
        bool changed = rateChanged_;
        const auto readRate = [&](Rate& r) {
            r.updateDelay = static_cast<UInt32>(input.readInt(10));
            r.packetSize = static_cast<UInt16>(input.readInt(10));
            return input.isValid() && r.updateDelay >= 1 && r.updateDelay <= 1000 &&
                   r.packetSize >= 100 && r.packetSize <= packetSizeLimit_;
        };
        if (input.readFlag() && !readRate(current)) {
            fail("Invalid current packet rate");
            return;
        }
        if (input.readFlag()) {
            Rate maximum;
            if (!readRate(maximum)) {
                fail("Invalid maximum packet rate");
                return;
            }
            Rate negotiated{std::max(maximum.updateDelay, maxRate_.updateDelay),
                            std::min(maximum.packetSize, maxRate_.packetSize)};
            changed = changed || negotiated != current;
            current = negotiated;
        }
        if (!input.isValid()) {
            fail("Truncated packet rate");
            return;
        }
        const auto snapshot = objects_;
        std::ranges::for_each(snapshot, [&](auto* object) {
            if (!getManager() || isRemoved() || !error_.empty()) {
                return;
            }
            auto* client = dynamic_cast<PacketStreamClient*>(object);
            if (!client || client->isRemoved() || client->owner() != this) {
                fail("Packet client layout changed");
                return;
            }
            const auto present = input.readFlag();
            if (!input.isValid() || (present && !client->readPacket(input, now))) {
                fail("Invalid packet client payload");
            }
        });
        if (!getManager() || isRemoved()) {
            return;
        }
        if (!input.isValid()) {
            fail("Truncated simulation packet");
        }
        if (!error_.empty()) {
            return;
        }
        // Only the final byte's padding may remain. Legacy padding is undefined.
        // Exact bit limits are
        // useful for exhaustive truncation tests and recording adapters.
        if (input.remainingReadBits() > 7 || (bits && input.remainingReadBits() != 0)) {
            fail("Unexpected simulation packet tail: " + std::to_string(input.remainingReadBits()) +
                 " bits at " + std::to_string(input.getCurPos()));
            return;
        }
        currentRate_ = current;
        rateChanged_ = changed;
        needsReply_ = mode_ == Mode::Reply;
        lastReceivedTick_ = now;
        result = true;
        stagedStats_.totalRecv = 1;
        stats_.add(stagedStats_);
        globalStats().add(stagedStats_);
        stagedStats_.reset();
    });
    return result;
}

bool PacketStream::handleNotify(UInt32 key, bool delivered) {
    if (!getManager() || isRemoved()) {
        return false;
    }
    bool result = false;
    getManager()->dispatch([&] {
        auto found = pending_.find(key);
        if (found == pending_.end()) {
            return;
        }
        auto pending = std::move(found->second);
        pending_.erase(found);
        result = true;
        if (!delivered) {
            rateChanged_ |= pending.rate;
            maxChanged_ |= pending.maximum;
        } else if (pending.sequence - lastPacketReceived_ < 0x80000000U) {
            lastPacketReceived_ = pending.sequence;
        }
        std::ranges::for_each(pending.notes | std::views::reverse, [&](const auto& note) {
            if (!getManager() || isRemoved()) {
                return;
            }
            if (auto* client =
                    dynamic_cast<PacketStreamClient*>(note.client.resolve(*getManager()));
                client && client->owner() == this) {
                if (delivered) {
                    client->packetReceived(note.key);
                } else {
                    client->packetDropped(note.key);
                }
            }
        });
    });
    return result;
}

void PacketStream::attach(net::Session& session, net::ConnectionId connection) {
    session_ = &session;
    connection_ = connection;
}

std::string PacketStream::transportAddress() const {
    return session_ ? session_->address(connection_) : std::string{};
}

void PacketStream::connectionDied() {
    session_ = nullptr;
    connection_ = 0;
}

void PacketStream::disconnect(UInt32 now, std::span<const std::byte> reason) {
    if (session_) {
        const auto address = session_->address(connection_);
        const auto result = session_->disconnect(connection_, reason, now);
        // A successful send retires the transport slot, but already queued
        // events still belong to this stream. Cancel them on local disposal
        // so a callback-driven reconnect cannot receive the old departure.
        session_->forget(connection_);
        if (result != net::Result::Ok && getManager() && getManager()->console()) {
            getManager()->console()->print(std::format(
                "Failed to send disconnect to {} (network result {}).", address, int(result)));
        }
    }
    connectionDied();
}

net::Result PacketStream::checkPacketSend(UInt32 now) {
    if (!session_ || !getManager() || isRemoved() || demoState() == DemoState::Playback ||
        !error_.empty()) {
        return net::Result::InvalidState;
    }
    auto info = session_->info(connection_);
    if (!info || info->state != net::State::Connected) {
        return net::Result::InvalidState;
    }
    if (info->windowFull) {
        return net::Result::WindowFull;
    }
    auto delay = currentRate_.updateDelay;
    if (mode_ == Mode::Reply) {
        delay += delay >> 1;
    }
    const auto frame = mode_ != Mode::Server ? packetFrame_ : std::nullopt;
    const auto interval = frame.value_or(delay);
    if (packetSequence_ - lastPacketReceived_ > std::max(1U, 1000 / interval) ||
        ((frame || !needsReply_) && now - lastUpdate_ < interval)) {
        return net::Result::WouldBlock;
    }
    auto* manager = getManager();
    net::Result result = net::Result::WouldBlock;
    manager->dispatch([&] {
        now_ = now;
        auto packet = writePacket();
        if (!packet || isRemoved() || !session_) {
            return;
        }
        result = session_->send(connection_, packet->bytes, packet->key, now);
        if (result != net::Result::Ok) {
            --packetSequence_;
            stats_.subtract(lastSentStats_);
            globalStats().subtract(lastSentStats_);
            lastSentStats_.reset();
            handleNotify(packet->key, false);
        } else {
            lastUpdate_ = now;
            needsReply_ = false;
        }
    });
    return result;
}

bool PacketStream::processEvent(const SimEvent& event) {
    if (dynamic_cast<const SimTimerEvent*>(&event) && demo_ && demo_->timing == DemoTiming::Timer) {
        (void)stepReplay();
        return true;
    }
    if (const auto* message = dynamic_cast<const SimMessageEvent*>(&event);
        message && message->message == 0) {
        auto* delegate = getManager()
                             ? dynamic_cast<CSDelegate*>(getManager()->findObject(CSDelegateId))
                             : nullptr;
        const auto clock = delegate ? delegate->clock : std::function<UInt32()>{};
        const auto now = clock ? clock() : now_;
        if (demoState() == DemoState::Playback && demo_->timing == DemoTiming::Timestamped) {
            (void)processRecorder(now);
        } else {
            (void)checkPacketSend(now);
        }
        return true;
    }
    return SimGroup::processEvent(event);
}
} // namespace opent1::sim::network
