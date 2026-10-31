#pragma once
#include <opent1/world/interior_shape.hpp>
#include <opent1/math/random.hpp>

namespace opent1::world {
struct InteriorLightPlayback {
    UInt32 state{}, loops{}; // zero loops means infinite playback
    Flt32 time{}, timeScale{1};
    bool playing{};
};

// Owned lighting snapshot and per-instance RNG. Copies resume independently from
// the same state. Advance explicitly from simulation time, never from rendering.
class InteriorLightPlayer {
public:
    [[nodiscard]] static std::optional<InteriorLightPlayer>
    create(const InteriorLighting&, UInt32 seed = Random::defaultSeed);

    [[nodiscard]] const InteriorLighting& lighting() const {
        return *lighting_;
    }

    [[nodiscard]] std::span<const InteriorLightSlot> slots() const {
        return slots_;
    }

    // Frozen playback projected onto another LOD's maps. Tracks must have the
    // same ordinal timing, flags and colors with disjoint state spans;
    // IDs/map/slot layouts may differ.
    // Replays retained installations, including states that leave slots alone.
    [[nodiscard]] std::optional<std::vector<InteriorLightSlot>>
    slotsFor(const InteriorLighting&) const;
    // Native detail switch: retain instance clocks by ordinal, reset state,
    // and evaluate each light against the newly selected detail's timeline.
    [[nodiscard]] std::optional<std::vector<InteriorLightSlot>>
    slotsForDetail(const InteriorLighting&) const;
    [[nodiscard]] std::optional<InteriorLightPlayback> playback(Int32 id) const;
    bool setTime(Int32 id, Flt32);
    bool setState(Int32 id, UInt32);
    bool setLoopCount(Int32 id, UInt32);
    bool setTimeScale(Int32 id, Flt32); // finite, nonnegative; zero pauses the clock
    bool setPlaying(Int32 id, bool);
    bool advance(Flt32 seconds); // atomic; bounded work, finite nonnegative delta
private:
    struct Thread {
        InteriorLightPlayback playback;
        std::array<UInt8, 4> color{};
    };

    struct Installation {
        UInt64 order{};
        std::array<UInt8, 4> color{};
    };

    explicit InteriorLightPlayer(UInt32 seed) : random_(seed) {}

    [[nodiscard]] std::optional<std::size_t> find(Int32) const;
    bool update(std::size_t, std::size_t& work);
    void install(std::size_t);
    std::shared_ptr<const InteriorLighting> lighting_;
    std::vector<Thread> threads_;
    std::vector<InteriorLightSlot> slots_;
    std::vector<Installation> installed_; // last installation per authored state
    UInt64 order_{};
    Random random_;
};
} // namespace opent1::world
