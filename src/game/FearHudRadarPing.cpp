#include <opent1/game/status_combat_huds.hpp>

namespace opent1::game {

FGHSensorPing::FGHSensorPing() {
    fields_.extent = {32, 23};
}

FGHSensorPing::FrameState FGHSensorPing::computePingFrame(UInt8 pingStatus,
                                                          Flt32 curTimeSeconds,
                                                          Flt32& startBlinkTimeInOut) noexcept {
    constexpr UInt8 kPingStatusOn = 1;
    constexpr UInt8 kPingStatusSuppressed = 2;
    if (pingStatus == kPingStatusSuppressed) {
        startBlinkTimeInOut = -1.0f;
        return FrameSuppressed;
    }
    if (pingStatus == kPingStatusOn) {
        if (startBlinkTimeInOut < 0.0f) {
            startBlinkTimeInOut = curTimeSeconds;
        }
        const Int32 elapsed = static_cast<Int32>((curTimeSeconds - startBlinkTimeInOut) * 10.0f);
        const Int32 bmpVal = (elapsed >= 0 ? elapsed : 0) % 6;
        if (bmpVal == 0) {
            return FrameLow;
        }
        if (bmpVal == 1) {
            return FrameMed;
        }
        return FrameHigh;
    }
    startBlinkTimeInOut = -1.0f;
    return FrameHidden;
}

void FGHSensorPing::updatePing(UInt8 pingStatus, Flt32 curTimeSeconds, bool lowRes) noexcept {
    frameState_ = computePingFrame(pingStatus, curTimeSeconds, startBlinkTime_);
    lowResMode_ = lowRes;
    resize(fields_.position, lowRes ? Point2I{20, 15} : Point2I{32, 23});
}

bool FGHSensorPing::onAdd() {
    if (!Parent::onAdd()) {
        return false;
    }
    fields_.extent = {32, 23};
    return true;
}

bool FGHSensorPing::draw(gfx::SoftwareSurface& s, Point2I offset, RectI clip) const {
    if (!Parent::draw(s, offset, clip)) {
        return false;
    }
    const std::string prefix = lowResMode_ ? "lr_" : "";
    const auto* base = sprite(prefix + "ping.bmp");
    if (base && !s.blit(*base, offset)) {
        return false;
    }
    constexpr std::array names{
        "HP_PingedLo.bmp", "HP_PingedMed.bmp", "HP_PingedHi.bmp", "HP_Supressed.bmp"};
    if (frameState_ == FrameHidden) {
        return true;
    }
    const auto* overlay = sprite(prefix + names[std::size_t(frameState_)]);
    return !overlay || s.blit(*overlay, offset);
}

} // namespace opent1::game
