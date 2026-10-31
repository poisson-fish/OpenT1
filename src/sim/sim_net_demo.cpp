#include "sim_net_demo_internal.hpp"
#include <opent1/core/mem_stream.hpp>
#include <opent1/sim/sim_world_game.hpp>
#include <algorithm>
#include <exception>

namespace opent1::sim::network {
namespace {
void word(std::vector<std::byte>& bytes, UInt32 value, unsigned count) {
    std::ranges::for_each(std::views::iota(0U, count), [&](auto i) {
        bytes.push_back(std::byte((value >> (i * 8)) & 255));
    });
}

UInt32 word(std::span<const std::byte> bytes) {
    UInt32 value{};
    std::ranges::for_each(std::views::iota(std::size_t{0}, bytes.size()), [&](auto i) {
        value |= std::to_integer<UInt32>(bytes[i]) << (i * 8);
    });
    return value;
}
}

PacketStream::DemoState PacketStream::demoState() const {
    return demo_ ? demo_->state : DemoState::Idle;
}

std::span<const std::byte> PacketStream::recording() const {
    return demo_ ? std::span<const std::byte>(demo_->bytes) : std::span<const std::byte>{};
}

bool PacketStream::startRecording(DemoTiming timing, UInt32 now) {
    if (!getManager() || isRemoved() || driving_ || receiving_ ||
        demoState() == DemoState::Recording || demoState() == DemoState::Playback ||
        (demo_ && demo_->busy)) {
        return false;
    }
    auto demo = std::make_unique<Demo>();
    demo->state = DemoState::Recording;
    demo->timing = timing;
    demo->offset = now & ~UInt32{31};
    word(demo->bytes, getManager()->getId(), 4);
    word(demo->bytes, getId(), 4);
    std::ranges::transform(objects_, std::back_inserter(demo->clients), [](auto* client) {
        return client->address();
    });
    demo_ = std::move(demo);
    if (timing == DemoTiming::Timer) {
        addToSet(TimerSetId);
    }
    return true;
}

bool PacketStream::appendDemo(std::span<const std::byte> data, UInt32 now, bool timestamp) {
    if (demoState() != DemoState::Recording) {
        return false;
    }
    const bool timed = timestamp && demo_->timing == DemoTiming::Timestamped;
    const auto time = now - demo_->offset;
    const auto available = Demo::MaxBytes - demo_->bytes.size();
    if ((timed && (time == 0xffffffff || time < demo_->lastTime)) ||
        available < (timed ? 4U : 0U) || data.size() > available - (timed ? 4U : 0U)) {
        demo_->state = DemoState::Failed;
        return false;
    }
    if (timed) {
        word(demo_->bytes, time, 4);
        demo_->lastTime = time;
    }
    demo_->bytes.insert(demo_->bytes.end(), data.begin(), data.end());
    return true;
}

bool PacketStream::recordClient(PacketStreamClient& client,
                                std::span<const std::byte> data,
                                UInt32 now) {
    if (demoState() != DemoState::Recording || client.owner() != this || data.empty() ||
        data.size() > Demo::MaxBytes) {
        return false;
    }
    const auto found = std::ranges::find_if(demo_->clients, [&](const auto& a) {
        return a.generation == client.address().generation;
    });
    if (found == demo_->clients.end()) {
        return false;
    }
    std::vector<std::byte> record{std::byte(found - demo_->clients.begin())};
    record.insert(record.end(), data.begin(), data.end());
    return appendDemo(record, now);
}

bool PacketStream::stopRecording() {
    if (demoState() != DemoState::Recording || demo_->busy || !getManager()) {
        return false;
    }
    bool result = false;
    getManager()->dispatch([&] {
        demo_->busy = true;
        struct Guard {
            bool& b;
            ~Guard() {
                b = false;
            }
        } guard{demo_->busy};
        // Final client records may be written by these callbacks, as in legacy.
        const auto clients = demo_->clients;
        auto* manager = getManager();
        std::exception_ptr error;
        std::ranges::for_each(clients, [&](const auto& address) {
            if (auto* client = dynamic_cast<PacketStreamClient*>(address.resolve(*manager));
                client && client->owner() == this) {
                try {
                    client->notifyRecordOver();
                } catch (...) {
                    if (!error) {
                        error = std::current_exception();
                    }
                }
            }
        });
        if (error) {
            demo_->state = DemoState::Failed;
            std::rethrow_exception(error);
        }
        const std::array timed{std::byte{255}, std::byte{255}, std::byte{255}, std::byte{255}};
        const std::array timer{std::byte{255}, std::byte{255}, std::byte{255}};
        result =
            appendDemo(demo_->timing == DemoTiming::Timestamped ? std::span<const std::byte>(timed)
                                                                : std::span<const std::byte>(timer),
                       0,
                       false);
        if (result) {
            demo_->state = DemoState::Finished;
            const auto finished = onRecordingFinished;
            if (finished) {
                finished(demo_->bytes);
            }
        }
        if (demo_->timing == DemoTiming::Timer) {
            if (auto* timers = dynamic_cast<SimSet*>(manager->findObject(TimerSetId))) {
                timers->removeObject(*this);
            }
        }
    });
    return result;
}

bool PacketStream::startPlayback(std::span<const std::byte> bytes, DemoTiming timing, UInt32 now) {
    if (!getManager() || isRemoved() || session_ || driving_ || receiving_ || !pending_.empty() ||
        demoState() == DemoState::Recording || demoState() == DemoState::Playback ||
        (demo_ && demo_->busy) || bytes.size() < 8 || bytes.size() > Demo::MaxBytes ||
        !error_.empty()) {
        return false;
    }
    const auto manager = word(bytes.first(4)), stream = word(bytes.subspan(4, 4));
    // Configure identities before playback instead of silently renumbering a live
    // world, which could leave existing event addresses pointing at other objects.
    if (manager != getManager()->getId() || stream != getId()) {
        return false;
    }
    auto demo = std::make_unique<Demo>();
    demo->timing = timing;
    demo->offset = now & ~UInt32{31};
    demo->state = DemoState::Playback;
    demo->bytes.assign(bytes.begin(), bytes.end());
    std::ranges::transform(objects_, std::back_inserter(demo->clients), [](auto* client) {
        return client->address();
    });
    demo_ = std::move(demo);
    if (timing == DemoTiming::Timer) {
        addToSet(TimerSetId);
    }
    return true;
}

void PacketStream::finishPlayback(bool success) {
    if (demoState() != DemoState::Playback) {
        return;
    }
    const auto previous = demo_->busy;
    demo_->busy = true;

    struct Guard {
        bool& b;
        bool previous;

        ~Guard() {
            b = previous;
        }
    } guard{demo_->busy, previous};

    demo_->state = success ? DemoState::Finished : DemoState::Failed;
    if (getManager() && demo_->timing == DemoTiming::Timer) {
        if (auto* timers = dynamic_cast<SimSet*>(getManager()->findObject(TimerSetId))) {
            timers->removeObject(*this);
        }
    }
    const auto clients = demo_->clients;
    std::exception_ptr error;
    std::ranges::for_each(clients, [&](const auto& address) {
        if (getManager()) {
            if (auto* client = dynamic_cast<PacketStreamClient*>(address.resolve(*getManager()));
                client && client->owner() == this) {
                try {
                    client->notifyPlaybackOver();
                } catch (...) {
                    if (!error) {
                        error = std::current_exception();
                    }
                }
            }
        }
    });
    if (error) {
        demo_->state = DemoState::Failed;
        std::rethrow_exception(error);
    }
}

void PacketStream::endPlayback() {
    if (!getManager() || (demo_ && demo_->busy)) {
        return;
    }
    getManager()->dispatch([&] {
        finishPlayback(true);
    });
}

bool PacketStream::replay(UInt32 now, bool timer, std::size_t budget) {
    if (demoState() != DemoState::Playback || demo_->busy || driving_ || receiving_ ||
        !getManager() || isRemoved() || !budget || budget > 65536) {
        return false;
    }
    bool result = true;
    getManager()->dispatch([&] {
        demo_->busy = true;
        struct Guard {
            bool& b;
            ~Guard() {
                b = false;
            }
        } guard{demo_->busy};
        try {
            std::size_t used{};
            while (demoState() == DemoState::Playback && !isRemoved() && used < budget) {
                auto remaining = std::span<const std::byte>(demo_->bytes).subspan(demo_->cursor);
                const auto fail = [&] {
                    result = false;
                    finishPlayback(false);
                };
                UInt32 eventTime = now;
                if (!timer) {
                    if (remaining.size() < 4) {
                        fail();
                        break;
                    }
                    const auto delta = word(remaining.first(4));
                    if (delta == 0xffffffff) {
                        demo_->cursor += 4;
                        if (remaining.size() != 4) {
                            fail();
                        } else {
                            finishPlayback(true);
                        }
                        break;
                    }
                    if (delta < demo_->lastTime) {
                        fail();
                        break;
                    }
                    if (now - demo_->offset < delta) {
                        break;
                    }
                    eventTime = demo_->offset + delta;
                    demo_->lastTime = delta;
                    demo_->cursor += 4;
                    remaining = remaining.subspan(4);
                }
                ++used;
                if (remaining.empty()) {
                    fail();
                    break;
                }
                const auto index = std::to_integer<UInt8>(remaining[0]);
                ++demo_->cursor;
                remaining = remaining.subspan(1);
                if (timer && index == 254) {
                    break;
                }
                if (index == 255) {
                    if (remaining.size() < 2) {
                        fail();
                        break;
                    }
                    const auto length = word(remaining.first(2));
                    demo_->cursor += 2;
                    if (timer && length == 65535) {
                        if (remaining.size() != 2) {
                            fail();
                        } else {
                            finishPlayback(true);
                        }
                        break;
                    }
                    if (!length || length > net::MaxPayload || remaining.size() - 2 < length) {
                        fail();
                        break;
                    }
                    demo_->cursor += length;
                    if (!readPacket(remaining.subspan(2, length), eventTime)) {
                        fail();
                        break;
                    }
                } else {
                    if (index >= demo_->clients.size()) {
                        fail();
                        break;
                    }
                    auto* client = dynamic_cast<PacketStreamClient*>(
                        demo_->clients[index].resolve(*getManager()));
                    MemRWStream stream(remaining);
                    if (!client || client->owner() != this ||
                        !client->processRecorderStream(stream, timer ? 0xffffffff : eventTime) ||
                        stream.getStatus() != STRM_OK || stream.getPosition() <= 0) {
                        fail();
                        break;
                    }
                    demo_->cursor += static_cast<std::size_t>(stream.getPosition());
                }
            }
        } catch (...) {
            try {
                finishPlayback(false);
            } catch (...) {
            }
            throw;
        }
    });
    return result;
}

bool PacketStream::processRecorder(UInt32 now, std::size_t budget) {
    return demo_ && demo_->timing == DemoTiming::Timestamped && replay(now, false, budget);
}

bool PacketStream::stepReplay(std::size_t budget) {
    if (demoState() == DemoState::Recording && demo_->timing == DemoTiming::Timer) {
        const std::array marker{std::byte{254}};
        return appendDemo(marker, 0, false);
    }
    return demo_ && demo_->timing == DemoTiming::Timer && replay(now_, true, budget);
}

bool PacketStream::setPacketFrame(std::optional<UInt32> frame) {
    if (driving_ || receiving_) {
        return false;
    }
    packetFrame_ = frame ? std::optional(std::clamp(*frame, 32U, 128U)) : std::nullopt;
    return true;
}
}
