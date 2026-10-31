#pragma once
#include <opent1/math/math_Fix16_and_int.hpp>
#include <array>

namespace opent1 {
// Instance-owned Darkstar R250 generator. The seed is an identifier, not a
// serialized snapshot. Copies retain the full sequence and cached normal.
class Random {
public:
    static constexpr UInt32 defaultSeed = 33655;

    explicit Random(UInt32 seed = defaultSeed) {
        setSeed(seed);
    }

    // Landscape's native reseed retains a pending normal deviate. Other callers
    // reset that cache by default, preserving the modern generator contract.
    void setSeed(UInt32 seed, bool resetNormal = true);

    [[nodiscard]] UInt32 getSeed() const noexcept {
        return seed_;
    }

    [[nodiscard]] UInt32 getInt() noexcept;
    [[nodiscard]] UInt32 getInt(UInt32 max) noexcept;
    [[nodiscard]] Int32 getInt(Int32 min, Int32 max);
    [[nodiscard]] RealF getFloat() noexcept;

    [[nodiscard]] RealF getFloat(RealF max) noexcept {
        return getFloat() * max;
    }

    [[nodiscard]] RealF getFloat(RealF min, RealF max) noexcept {
        return getFloat() * (max - min) + min;
    }

    [[nodiscard]] RealF getNormal();

    [[nodiscard]] RealF getNormal(RealF mean, RealF deviation) {
        return getNormal() * deviation + mean;
    }

    // Legacy remapping; min/max are scale anchors, not clamping bounds.
    [[nodiscard]] RealF getNormal(RealF mean, RealF deviation, RealF min, RealF max) {
        const auto value = getNormal(mean, deviation);
        return (value < 0 ? mean - min : max - mean) * value + mean;
    }

private:
    std::array<UInt32, 250> buffer_{};
    std::size_t index_{};
    UInt32 seed_{};
    RealF normalStore_{};
    bool normalReady_{};
};
} // namespace opent1
