#include <opent1/core/timer.hpp>
#include <cmath>

namespace opent1 {
Timer::Timer() noexcept {
    reset();
}

void Timer::reset() noexcept {
    start_ = Clock::now();
}

double Timer::getElapsed() const noexcept {
    return std::chrono::duration<double>(Clock::now() - start_).count();
}

UInt64 Timer::getElapsedMicroseconds() const noexcept {
    const auto count =
        std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - start_).count();
    return static_cast<UInt64>(count >= 0 ? count : 0);
}

void DeltaTimer::set(float period, bool repeat, float startScale) {
    OPENT1_ASSERT_FATAL(std::isfinite(period) && period > 0 && std::isfinite(startScale) &&
                            startScale >= 0 && std::isfinite(period * startScale),
                        "Invalid delta timer configuration");
    period_ = period;
    repeat_ = repeat;
    tripped_ = 0;
    elapsed_ = period * startScale;
}

bool DeltaTimer::update(float dt) {
    OPENT1_ASSERT_FATAL(period_ > 0 && std::isfinite(dt) && dt >= 0 && std::isfinite(elapsed_ + dt),
                        "Invalid delta timer step");
    elapsed_ += dt;
    if (elapsed_ < period_) {
        return false;
    }
    tripped_ = (tripped_ << 1U) | 1U;
    elapsed_ -= period_;
    return repeat_;
}
} // namespace opent1
