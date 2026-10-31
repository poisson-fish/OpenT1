#include <opent1/game/input_handler.hpp>
#include <algorithm>
#include <cmath>

namespace opent1::game {

InputHandler::InputHandler() noexcept = default;

void InputHandler::reset() noexcept {
    rValue_ = 0.0f;
    rTarget_ = 0.0f;
}

void InputHandler::sample(Flt32 rDelta, Int32 iDevice) noexcept {
    if ((iDevice & sim::ActionDigital) != 0) {
        sampleDigital(std::max(std::min(rDelta, 1.0f), -1.0f), (iDevice & sim::ActionPov) == 0);
    } else if ((iDevice & sim::ActionDelta) != 0) {
        sampleMouse(rDelta);
    } else if ((iDevice & sim::ActionAnalog) != 0) {
        sampleAnalog(std::max(std::min(rDelta, 1.0f), -1.0f));
    }
}

void InputHandler::sampleDigital(Flt32 rDelta, bool fIsKeyboard) noexcept {
    static_cast<void>(fIsKeyboard);
    diFocus_ = &diDigital_;

    if (rDelta == 0.0f) {
        reset();
    } else {
        rTarget_ = limitInputRange(rDelta, 1.0f);
    }
}

void InputHandler::sampleAnalog(Flt32 rDelta) noexcept {
    if (fAnalogStick_) {
        if (std::fabs(rDelta - rAnalogStickValue_) > rAnalogStickRange_) {
            fAnalogStick_ = false;
        } else {
            return;
        }
    }

    if (diFocus_ == &diAnalog_ || rDelta != 0.0f) {
        diFocus_ = &diAnalog_;
        rTarget_ = limitInputRange(rDelta, 1.0f);
    }
}

void InputHandler::sampleMouse(Flt32 rDelta) noexcept {
    // SDL delivers individual motion events; DirectInput supplied the summed
    // relative motion for a polling interval. Preserve every event until read.
    if (diFocus_ != &diMouse_) {
        rTarget_ = 0.0f;
    }
    diFocus_ = &diMouse_;
    rTarget_ = limitInputRange(rTarget_ + rDelta, 1.0f);
}

Flt32 InputHandler::getInput() noexcept {
    const auto rTime = static_cast<Flt32>(timer_.getElapsed());
    timer_.reset();
    return getInput(rTime);
}

Flt32 InputHandler::getInput(Flt32 explicitElapsedSeconds) noexcept {
    if (diFocus_ == nullptr) {
        return 0.0f;
    }

    if (diFocus_ == &diMouse_) {
        rValue_ = rTarget_;
        rTarget_ = 0.0f;
    } else {
        rValue_ = limitInputRange(rTarget_ * explicitElapsedSeconds, 1.0f);
    }
    return rValue_;
}

} // namespace opent1::game
