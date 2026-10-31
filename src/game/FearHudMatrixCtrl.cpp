#include <opent1/game/hud_controls.hpp>

namespace opent1::game {

namespace {
inline constexpr Int32 kIdItgLrPosition = 200144;
inline constexpr Int32 kIdItgLrExtent = 200145;
} // namespace

void FGHudMatrix::applyResolution(bool lowRes) {
    lowRes_ = lowRes;
    if (lowRes_) {
        resize(loResPosition_, loResExtent_);
    } else {
        resize(hiResPosition_, hiResExtent_);
    }
}

void FGHudMatrix::wake() {
    hiResPosition_ = fields_.position;
    hiResExtent_ = fields_.extent;
    Parent::wake();
}

std::function<void()> FGHudMatrix::checkpoint() {
    auto base = Parent::checkpoint();
    const auto hp = hiResPosition_;
    const auto he = hiResExtent_;
    const auto lp = loResPosition_;
    const auto le = loResExtent_;
    const auto lr = lowRes_;
    return [this, base = std::move(base), hp, he, lp, le, lr] {
        base();
        hiResPosition_ = hp;
        hiResExtent_ = he;
        loResPosition_ = lp;
        loResExtent_ = le;
        lowRes_ = lr;
    };
}

bool FGHudMatrix::inspectWrite(sim::Inspect& inspector) const {
    inspector.write(kIdItgLrPosition, loResPosition_);
    inspector.write(kIdItgLrExtent, loResExtent_);
    return true;
}

bool FGHudMatrix::inspectRead(sim::Inspect& inspector) {
    sim::Inspect::Value v{};
    if (!inspector.read(sim::Inspect::Type::Point2I, kIdItgLrPosition, v)) {
        return false;
    }
    const Point2I lp = std::get<Point2I>(v);
    if (!inspector.read(sim::Inspect::Type::Point2I, kIdItgLrExtent, v)) {
        return false;
    }
    const Point2I le = std::get<Point2I>(v);
    loResPosition_ = lp;
    loResExtent_ = le;
    return true;
}

Persistent::Error
FGHudMatrix::read(StreamIO& sio, Int32 version, Int32 user, Persistent::LoadContext& ctx) {
    Point2I lp{};
    Point2I le{};
    if (!sio.read(lp.x) || !sio.read(lp.y) || !sio.read(le.x) || !sio.read(le.y)) {
        return Persistent::Error::ReadError;
    }
    if (le.x < 0 || le.y < 0 || le.x > 32768 || le.y > 32768) {
        return Persistent::Error::ReadError;
    }
    const auto err = Parent::read(sio, version, user, ctx);
    if (err != Persistent::Error::Ok) {
        return err;
    }
    loResPosition_ = lp;
    loResExtent_ = le;
    return Persistent::Error::Ok;
}

Persistent::Error
FGHudMatrix::write(StreamIO& sio, Int32 version, Int32 user, Persistent::StoreContext& ctx) const {
    if (loResExtent_.x < 0 || loResExtent_.y < 0 || loResExtent_.x > 32768 ||
        loResExtent_.y > 32768) {
        return Persistent::Error::WriteError;
    }
    if (!sio.write(loResPosition_.x) || !sio.write(loResPosition_.y) ||
        !sio.write(loResExtent_.x) || !sio.write(loResExtent_.y)) {
        return Persistent::Error::WriteError;
    }
    return Parent::write(sio, version, user, ctx);
}

} // namespace opent1::game
