#include <opent1/game/status_combat_huds.hpp>

namespace opent1::game {

FGLagIndicator::FGLagIndicator() {
    fields_.extent = {32, 32};
}

bool FGLagIndicator::evaluateLag(UInt32 currentTimeMs,
                                 UInt32 lastReceivedTickMs,
                                 bool playbackMode) noexcept {
    if (playbackMode) {
        lagging_ = false;
        return false;
    }
    const UInt32 elapsed =
        currentTimeMs >= lastReceivedTickMs ? (currentTimeMs - lastReceivedTickMs) : 0U;
    lagging_ = (elapsed >= lagTimeoutMs_);
    return lagging_;
}

bool FGLagIndicator::onAdd() {
    if (!Parent::onAdd()) {
        return false;
    }
    return true;
}

bool FGLagIndicator::draw(gfx::SoftwareSurface& s, Point2I offset, RectI clip) const {
    if (!Parent::draw(s, offset, clip)) {
        return false;
    }
    return !lagging_ || !bitmap || s.blit(*bitmap, offset);
}

} // namespace opent1::game
