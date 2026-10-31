#include <opent1/math/random.hpp>
#include <ranges>

namespace opent1 {
void Random::setSeed(UInt32 seed, bool resetNormal) {
    index_ = 0;
    seed_ = seed;
    // Reseeding invalidates the cached deviate. The legacy omission reused a
    // deviate from the previous seed. No process-global srand side effect remains.
    if (resetNormal) {
        normalReady_ = false;
    }
    UInt32 quick = seed;
    const auto next = [&] {
        quick = 1664525U * quick + 1013904223U;
        return quick;
    };
    std::ranges::generate(buffer_, next);
    std::ranges::for_each(buffer_, [&](auto& value) {
        if (next() > 0x40000000U) {
            value |= 0x80000000U;
        }
    });
    UInt32 msb = 0x80000000U, mask = 0xffffffffU;
    std::ranges::for_each(std::views::iota(0U, 32U), [&](auto j) {
        auto& value = buffer_[7 * j + 3];
        value = (value & mask) | msb;
        mask >>= 1;
        msb >>= 1;
    });
}

UInt32 Random::getInt() noexcept {
    const auto j = index_ >= 147 ? index_ - 147 : index_ + 103;
    const auto value = buffer_[index_] ^ buffer_[j];
    buffer_[index_] = value;
    index_ = index_ == 249 ? 0 : index_ + 1;
    return value;
}

UInt32 Random::getInt(UInt32 max) noexcept {
    return static_cast<UInt32>((static_cast<UInt64>(getInt()) * max) / 0xffffffffU);
}

Int32 Random::getInt(Int32 min, Int32 max) {
    OPENT1_ASSERT_FATAL(min <= max, "Random integer bounds reversed");
    const auto range = static_cast<UInt32>(static_cast<Int64>(max) - min);
    return static_cast<Int32>(static_cast<Int64>(getInt(range)) + min);
}

RealF Random::getFloat() noexcept {
    // MAX_UINT rounds to 2^32 in single precision, as in the original wrapper.
    return static_cast<RealF>(getInt()) / static_cast<RealF>(0xffffffffU);
}

RealF Random::getNormal() {
    if (normalReady_) {
        normalReady_ = false;
        return normalStore_;
    }
    RealF a{}, b{}, radius{};
    do { // Rejection sampling has no equivalent bounded ranges algorithm.
        a = 2.0f * getFloat() - 1.0f;
        b = 2.0f * getFloat() - 1.0f;
        radius = a * a + b * b;
    } while (radius >= 1.0f || radius == 0.0f);
    // Preserve Darkstar's log base 2, including its float log(2) denominator.
    const auto scale = static_cast<RealF>(
        std::sqrt(-2.0 * (std::log(static_cast<RealD>(radius)) / std::log(2.0f)) / radius));
    normalStore_ = b * scale;
    normalReady_ = true;
    return a * scale;
}
} // namespace opent1
