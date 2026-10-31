#pragma once
#include <opent1/core/base.hpp>
#include <chrono>

namespace opent1 {
class Timer {
public:
    using Clock = std::chrono::steady_clock;
    Timer() noexcept;
    void reset() noexcept;
    [[nodiscard]] double getElapsed() const noexcept;
    [[nodiscard]] UInt64 getElapsedMicroseconds() const noexcept;

private:
    Clock::time_point start_{};
};

// Legacy accumulates at most one trip per update, even for large dt.
// update returns false for non-repeating timers; bool conversion consumes a trip
// in either mode. Pending trips occupy a bounded unsigned 32-bit shift register.
class DeltaTimer {
public:
    void set(float period, bool repeat, float startScale = 0.0f);

    constexpr void reset() noexcept {
        elapsed_ = 0;
        tripped_ = 0;
    }

    bool update(float dt);

    [[nodiscard]] constexpr float getElapsed() const noexcept {
        return elapsed_;
    }

    [[nodiscard]] explicit constexpr operator bool() noexcept {
        const UInt32 tmp = tripped_;
        tripped_ >>= 1U;
        return (tmp & 1U) != 0;
    }

private:
    float elapsed_{0}, period_{0};
    bool repeat_{false};
    UInt32 tripped_{0};
};
} // namespace opent1
