#include <opent1/video/audio_player.hpp>
#include <algorithm>
#include <stdexcept>

namespace opent1::video {
std::shared_ptr<const audio::Buffer>
AudioPlayer::chunk(const Movie& movie, UInt32 track, UInt64& sample, bool loop) {
    const auto format = movie.audioFormat(track);
    const auto block = UInt32(format.channels * (format.bits / 8));
    const auto length = movie.durationUs() * format.rate / 1000000;
    if (!block || !format.rate || !length) {
        throw std::invalid_argument("invalid movie audio format");
    }
    const auto count = std::max(1U, format.rate / 4);
    audio::Wave wave;
    wave.rate = format.rate;
    wave.channels = format.channels;
    wave.bits = format.bits;
    wave.samples.resize(std::size_t(count) * block,
                        format.bits == 8 ? std::byte{128} : std::byte{});
    UInt32 remaining = count, offset{};
    while (remaining) {
        if (sample >= length) {
            if (!loop) {
                break;
            }
            sample %= length;
        }
        const auto take = UInt32(std::min(UInt64(remaining), length - sample));
        const auto pcm = movie.audio(track, sample, take);
        if (pcm.bytes.size() > std::size_t(take) * block) {
            throw std::runtime_error("invalid movie audio window");
        }
        std::copy(
            pcm.bytes.begin(), pcm.bytes.end(), wave.samples.begin() + std::size_t(offset) * block);
        remaining -= take;
        offset += take;
        sample += take;
    }
    return device_.upload(wave);
}

bool AudioPlayer::start(std::shared_ptr<const Movie> movie, bool loop) {
    if (!movie) {
        return false;
    }
    try {
        std::array<Track, 7> staged;
        for (UInt32 t = 0; t < staged.size(); ++t) {
            if (!movie->audioFormat(t).rate || !movie->audioSamples(t)) {
                continue;
            }
            std::array<std::shared_ptr<const audio::Buffer>, 4> buffers;
            for (auto& buffer : buffers) {
                buffer = chunk(*movie, t, staged[t].nextSample, loop);
            }
            staged[t].queue = device_.queue(buffers);
        }
        for (auto& track : staged) {
            if (track.queue) {
                track.queue->prepare();
            }
        }
        stop();
        movie_ = std::move(movie);
        loop_ = loop;
        tracks_ = std::move(staged);
        for (auto& track : tracks_) {
            if (track.queue) {
                track.queue->activate();
            }
        }
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

bool AudioPlayer::update() {
    if (!movie_) {
        return true;
    }
    try {
        for (UInt32 t = 0; t < tracks_.size(); ++t) {
            auto& track = tracks_[t];
            if (!track.queue) {
                continue;
            }
            const auto processed = track.queue->processed();
            if (!processed) {
                continue;
            }
            auto next = track.nextSample;
            std::vector<std::shared_ptr<const audio::Buffer>> buffers;
            buffers.reserve(processed);
            for (UInt32 i = 0; i < processed; ++i) {
                buffers.push_back(chunk(*movie_, t, next, loop_));
            }
            track.queue->recycle(buffers);
            track.nextSample = next;
            if (!track.queue->playing()) {
                track.queue->play();
            }
        }
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

void AudioPlayer::stop() noexcept {
    for (auto& track : tracks_) {
        if (track.queue) {
            track.queue->stop();
        }
        track = {};
    }
    movie_.reset();
    loop_ = false;
}

std::array<std::shared_ptr<audio::QueuedSource>, 7> AudioPlayer::queues() const {
    std::array<std::shared_ptr<audio::QueuedSource>, 7> result;
    for (std::size_t t = 0; t < tracks_.size(); ++t) {
        result[t] = tracks_[t].queue;
    }
    return result;
}
} // namespace opent1::video
