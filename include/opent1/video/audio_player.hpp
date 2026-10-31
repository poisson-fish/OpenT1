#pragma once
#include <opent1/video/smacker.hpp>
#include <opent1/audio/audio.hpp>

namespace opent1::video {
// Movie sound follows the device cursor. Each track retains one second of PCM;
// update recycles completed quarter-second buffers without restarting a source.
// The host owns the device and stops this player before destroying it.
class AudioPlayer {
public:
    explicit AudioPlayer(audio::Device& device) : device_(device) {}

    ~AudioPlayer() {
        stop();
    }

    bool start(std::shared_ptr<const Movie>, bool loop);
    bool update();
    void stop() noexcept;
    [[nodiscard]] std::array<std::shared_ptr<audio::QueuedSource>, 7> queues() const;

private:
    struct Track {
        std::shared_ptr<audio::QueuedSource> queue;
        UInt64 nextSample{};
    };

    std::shared_ptr<const audio::Buffer> chunk(const Movie&, UInt32, UInt64&, bool);
    audio::Device& device_;
    std::shared_ptr<const Movie> movie_;
    std::array<Track, 7> tracks_;
    bool loop_{};
};
} // namespace opent1::video
