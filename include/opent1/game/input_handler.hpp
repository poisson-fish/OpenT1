#pragma once
#include <opent1/core/timer.hpp>
#include <opent1/core/types.hpp>
#include <opent1/sim/sim_input_action.hpp>

namespace opent1::game {

inline constexpr Flt32 kAnalogStickRange = 0.25f;

[[nodiscard]] constexpr Flt32 limitInputRange(Flt32 rVal, Flt32 rRange) noexcept {
    if (rVal > rRange) {
        return rRange;
    }
    if (rVal < -rRange) {
        return -rRange;
    }
    return rVal;
}

class InputHandler {
public:
    struct DeviceInput {
        Flt32 rSpeed{1.0f};
        Flt32 rAccel{1.0f};

        constexpr DeviceInput() noexcept = default;
    };

    InputHandler() noexcept;
    ~InputHandler() noexcept = default;

    void reset() noexcept;
    void sample(Flt32 rDelta, Int32 iDevice) noexcept;
    void sampleDigital(Flt32 rDelta, bool fIsKeyboard) noexcept;
    void sampleAnalog(Flt32 rDelta) noexcept;
    void sampleMouse(Flt32 rDelta) noexcept;

    [[nodiscard]] Flt32 getInput() noexcept;
    [[nodiscard]] Flt32 getInput(Flt32 explicitElapsedSeconds) noexcept;

    void setStickRange(Flt32 rRange) noexcept {
        if (rRange >= 0.0f && rRange <= 1.0f) {
            rAnalogStickRange_ = rRange;
        }
    }

    void startStickWait(Flt32 rCurrVal) noexcept {
        if (rCurrVal >= -1.0f && rCurrVal <= 1.0f) {
            rAnalogStickValue_ = rCurrVal;
            fAnalogStick_ = true;
        }
    }

    [[nodiscard]] const DeviceInput& digital() const noexcept {
        return diDigital_;
    }

    [[nodiscard]] const DeviceInput& analog() const noexcept {
        return diAnalog_;
    }

    [[nodiscard]] const DeviceInput& mouse() const noexcept {
        return diMouse_;
    }

    [[nodiscard]] const DeviceInput* focusDevice() const noexcept {
        return diFocus_;
    }

    [[nodiscard]] Flt32 value() const noexcept {
        return rValue_;
    }

    [[nodiscard]] Flt32 target() const noexcept {
        return rTarget_;
    }

    [[nodiscard]] bool analogStickWaiting() const noexcept {
        return fAnalogStick_;
    }

    [[nodiscard]] Flt32 analogStickRange() const noexcept {
        return rAnalogStickRange_;
    }

    [[nodiscard]] Flt32 analogStickValue() const noexcept {
        return rAnalogStickValue_;
    }

private:
    DeviceInput* diFocus_{nullptr};
    DeviceInput diDigital_{};
    DeviceInput diAnalog_{};
    DeviceInput diMouse_{};
    Timer timer_{};
    bool fAnalogStick_{false};
    Flt32 rAnalogStickRange_{kAnalogStickRange};
    Flt32 rAnalogStickValue_{0.0f};
    Flt32 rValue_{0.0f};
    Flt32 rTarget_{0.0f};
};

} // namespace opent1::game
