#pragma once
#include <opent1/video/smacker.hpp>

namespace opent1::video {
enum class PlayState { Empty, Playing, Paused, Ended };
enum class PlaybackError { None, Input, Clock, Decode, Backend };

struct PlayerOptions {
    bool loop{};
    UInt8 tracks{1};
};

struct Presentation {
    std::shared_ptr<const Frame> frame;
    std::array<Pcm, 7> audio; // at most one second per selected track
    UInt64 timeUs{}, audioOriginUs{}, loops{};
    PlayState state{PlayState::Empty};
    bool refreshAudio{};
};

// publish must stage fallible work before replacing current output. Returning
// false preserves its committed output; caller snapshots can outlive the sink.
class Output {
public:
    virtual ~Output() = default;
    virtual bool publish(const Presentation&) = 0;
    virtual void clear() noexcept = 0;
};

// Externally synchronized. Host supplies monotonic microseconds (steady_clock
// in production, deterministic clock in tests). Every operation is transactional.
class Player {
public:
    explicit Player(std::shared_ptr<Output>);

    ~Player() {
        close();
    }

    Player(const Player&) = delete;
    Player& operator=(const Player&) = delete;
    bool load(std::shared_ptr<const Movie>, UInt64 nowUs, PlayerOptions = {});
    bool load(Resource<Movie>, UInt64 nowUs, PlayerOptions = {});
    bool tick(UInt64 nowUs);
    bool pause(UInt64 nowUs);
    bool stop(UInt64 nowUs); // pause and rewind in one publication
    bool play(UInt64 nowUs);
    bool seek(UInt64 mediaUs, UInt64 nowUs);
    void close() noexcept;

    [[nodiscard]] const Presentation& snapshot() const {
        return current_;
    }

    [[nodiscard]] PlaybackError error() const {
        return error_;
    }

private:
    bool publish(Decoder, UInt64, UInt64, PlayState, PlayerOptions, UInt64, bool);
    bool clock(UInt64, UInt64&, UInt64&) const;
    std::shared_ptr<Output> output_;
    std::optional<Decoder> decoder_;
    Presentation current_;
    PlayerOptions options_;
    UInt64 lastClock_{};
    PlaybackError error_{};
};
} // namespace opent1::video
