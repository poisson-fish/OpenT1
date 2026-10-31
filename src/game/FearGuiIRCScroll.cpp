#include "opent1/game/server_irc_controls.hpp"

namespace opent1::game {

namespace {
inline constexpr Int32 kIdItgRotation = 200148;
} // namespace

std::function<void()> FGIRCScrollChannel::checkpoint() {
    auto baseSnap = Parent::checkpoint();
    const auto savedScrollLeft = mbScrollLeft_;
    return [this, baseSnap = std::move(baseSnap), savedScrollLeft]() {
        baseSnap();
        mbScrollLeft_ = savedScrollLeft;
    };
}

bool FGIRCScrollChannel::inspectRead(sim::Inspect& inspector) {
    sim::Inspect::Value v{};
    if (!inspector.read(sim::Inspect::Type::Bool, kIdItgRotation, v)) {
        return false;
    }
    mbScrollLeft_ = std::get<bool>(v);
    return true;
}

bool FGIRCScrollChannel::inspectWrite(sim::Inspect& inspector) const {
    return inspector.write(kIdItgRotation, mbScrollLeft_);
}

Persistent::Error
FGIRCScrollChannel::read(StreamIO& sio, Int32 version, Int32 user, Persistent::LoadContext& ctx) {
    auto snap = checkpoint();
    if (!sio.read(mbScrollLeft_)) {
        snap();
        return Persistent::Error::ReadError;
    }
    const auto err = Parent::read(sio, version, user, ctx);
    if (err != Persistent::Error::Ok) {
        snap();
    }
    return err;
}

Persistent::Error FGIRCScrollChannel::write(StreamIO& sio,
                                            Int32 version,
                                            Int32 user,
                                            Persistent::StoreContext& ctx) const {
    if (!sio.write(mbScrollLeft_)) {
        return Persistent::Error::WriteError;
    }
    return Parent::write(sio, version, user, ctx);
}

} // namespace opent1::game
