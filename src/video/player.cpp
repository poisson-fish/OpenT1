#include <opent1/video/player.hpp>
#include <ranges>
#include <algorithm>
#include <limits>
#include <stdexcept>

namespace opent1::video {
Player::Player(std::shared_ptr<Output> output) : output_(std::move(output)) {
    if (!output_) {
        throw std::invalid_argument("missing video output");
    }
}

bool Player::load(Resource<Movie> resource, UInt64 now, PlayerOptions options) {
    return load(resource ? std::shared_ptr<const Movie>(resource.get(),
                                                        [resource](const Movie*) mutable {
                                                            resource.reset();
                                                        })
                         : nullptr,
                now,
                options);
}

bool Player::load(std::shared_ptr<const Movie> movie, UInt64 now, PlayerOptions options) {
    if (!movie || options.tracks > 127) {
        error_ = PlaybackError::Input;
        return false;
    }
    return publish(Decoder(std::move(movie)), 0, now, PlayState::Playing, options, 0, true);
}

bool Player::publish(Decoder decoder,
                     UInt64 time,
                     UInt64 now,
                     PlayState state,
                     PlayerOptions options,
                     UInt64 loops,
                     bool reset) {
    try {
        const auto movie = decoder.movie();
        const auto index =
            UInt32(std::min(time / movie->frameTimeUs(), UInt64(movie->frameCount() - 1)));
        if (!decoder.seek(index)) {
            error_ = PlaybackError::Decode;
            return false;
        }
        Presentation next = current_;
        next.frame = decoder.frame();
        next.timeUs = time;
        next.loops = loops;
        next.state = state;
        next.refreshAudio = reset || state != current_.state || time < current_.audioOriginUs ||
                            time - current_.audioOriginUs >= 500000;
        if (next.refreshAudio) {
            next.audio = {};
            next.audioOriginUs = time;
            for (auto track : std::views::iota(UInt32{0}, UInt32{7})) {
                const auto format = movie->audioFormat(track);
                if (!(options.tracks & (1U << track)) || !format.rate ||
                    state != PlayState::Playing) {
                    continue;
                }
                auto& out = next.audio[track];
                out.format = format;
                out.firstSample = time * format.rate / 1000000;
                const auto block = UInt32(format.channels * (format.bits / 8));
                const auto length = movie->durationUs() * format.rate / 1000000;
                if (!length) {
                    continue;
                }
                UInt64 sample = out.firstSample;
                UInt32 remaining = format.rate;
                while (remaining) {
                    if (sample >= length) {
                        if (!options.loop) {
                            break;
                        }
                        sample %= length;
                    }
                    const auto count = UInt32(std::min(UInt64(remaining), length - sample));
                    auto part = movie->audio(track, sample, count);
                    const auto old = out.bytes.size();
                    out.bytes.resize(old + std::size_t(count) * block,
                                     format.bits == 8 ? std::byte{128} : std::byte{});
                    std::copy(part.bytes.begin(),
                              part.bytes.end(),
                              out.bytes.begin() + std::ptrdiff_t(old));
                    remaining -= count;
                    sample += count;
                }
            }
        }
        bool accepted{};
        try {
            accepted = output_->publish(next);
        } catch (const std::exception&) {
            accepted = false;
        }
        if (!accepted) {
            error_ = PlaybackError::Backend;
            return false;
        }
        decoder_ = std::move(decoder);
        current_ = std::move(next);
        lastClock_ = now;
        options_ = options;
        error_ = PlaybackError::None;
        return true;
    } catch (const std::exception&) {
        error_ = PlaybackError::Decode;
        return false;
    }
}

bool Player::clock(UInt64 now, UInt64& time, UInt64& loops) const {
    if (!decoder_ || now < lastClock_) {
        return false;
    }
    time = current_.timeUs;
    loops = current_.loops;
    if (current_.state != PlayState::Playing) {
        return true;
    }
    const auto delta = now - lastClock_, duration = decoder_->movie()->durationUs();
    if (options_.loop) {
        const auto cycles = delta / duration;
        const auto tail = delta % duration;
        const auto carry = (time + tail) / duration;
        if (cycles > std::numeric_limits<UInt64>::max() - loops ||
            carry > std::numeric_limits<UInt64>::max() - loops - cycles) {
            return false;
        }
        loops += cycles + carry;
        time = (time + tail) % duration;
    } else {
        time += std::min(delta, duration - time);
    }
    return true;
}

bool Player::tick(UInt64 now) {
    UInt64 time{}, loops{};
    if (!clock(now, time, loops)) {
        error_ = PlaybackError::Clock;
        return false;
    }
    const auto state = time == decoder_->movie()->durationUs() ? PlayState::Ended : current_.state;
    return publish(*decoder_, time, now, state, options_, loops, loops != current_.loops);
}

bool Player::pause(UInt64 now) {
    UInt64 time{}, loops{};
    if (!clock(now, time, loops)) {
        error_ = PlaybackError::Clock;
        return false;
    }
    return publish(*decoder_,
                   time,
                   now,
                   time == decoder_->movie()->durationUs() ? PlayState::Ended : PlayState::Paused,
                   options_,
                   loops,
                   true);
}

bool Player::play(UInt64 now) {
    if (!decoder_ || now < lastClock_) {
        error_ = PlaybackError::Clock;
        return false;
    }
    if (current_.state == PlayState::Playing) {
        return tick(now);
    }
    const auto time = current_.state == PlayState::Ended ? 0 : current_.timeUs;
    return publish(*decoder_, time, now, PlayState::Playing, options_, current_.loops, true);
}

bool Player::stop(UInt64 now) {
    if (!decoder_ || now < lastClock_) {
        error_ = PlaybackError::Clock;
        return false;
    }
    return publish(*decoder_, 0, now, PlayState::Paused, options_, 0, true);
}

bool Player::seek(UInt64 time, UInt64 now) {
    if (!decoder_ || time > decoder_->movie()->durationUs()) {
        error_ = PlaybackError::Input;
        return false;
    }
    if (now < lastClock_) {
        error_ = PlaybackError::Clock;
        return false;
    }
    const auto state =
        time == decoder_->movie()->durationUs()
            ? PlayState::Ended
            : (current_.state == PlayState::Playing ? PlayState::Playing : PlayState::Paused);
    return publish(*decoder_, time, now, state, options_, current_.loops, true);
}

void Player::close() noexcept {
    output_->clear();
    decoder_.reset();
    current_ = {};
    options_ = {};
    lastClock_ = 0;
    error_ = PlaybackError::None;
}
} // namespace opent1::video
