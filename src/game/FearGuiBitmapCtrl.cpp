#include <opent1/game/shell_controls.hpp>

#include <cmath>

namespace opent1::game {

std::function<void()> FGBitmapCtrl::checkpoint() {
    auto base = Parent::checkpoint();
    const auto alpha = alphaPercentage_;
    return [this, base = std::move(base), alpha] {
        base();
        alphaPercentage_ = alpha;
    };
}

Persistent::Error
FGBitmapCtrl::read(StreamIO& sio, Int32 version, Int32 user, Persistent::LoadContext& ctx) {
    auto restore = checkpoint();
    const auto err = Parent::read(sio, version, user, ctx);
    if (err != Persistent::Error::Ok) {
        restore();
        return err;
    }
    Flt32 alpha{};
    if (!sio.read(alpha) || !std::isfinite(alpha) || alpha < 0.0f || alpha > 1.0f) {
        restore();
        return Persistent::Error::ReadError;
    }
    alphaPercentage_ = alpha;
    return Persistent::Error::Ok;
}

Persistent::Error
FGBitmapCtrl::write(StreamIO& sio, Int32 version, Int32 user, Persistent::StoreContext& ctx) const {
    if (!std::isfinite(alphaPercentage_) || alphaPercentage_ < 0.0f || alphaPercentage_ > 1.0f) {
        return Persistent::Error::WriteError;
    }
    const auto err = Parent::write(sio, version, user, ctx);
    if (err != Persistent::Error::Ok) {
        return err;
    }
    if (!sio.write(alphaPercentage_)) {
        return Persistent::Error::WriteError;
    }
    return Persistent::Error::Ok;
}

} // namespace opent1::game
