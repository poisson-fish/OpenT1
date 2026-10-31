#include "opent1/game/shell_dialogs.hpp"

#include <algorithm>

namespace opent1::game {

namespace {
inline constexpr Int32 kIdItgMenuFixedSize = 200130;
} // namespace

void FearGuiDialog::parentResized(const Point2I& /*oldParentExtent*/,
                                  const Point2I& newParentExtent) {
    fields_.extent.x = std::max(std::min(newParentExtent.x - 1, fixedSize_.x), 1);
    fields_.extent.y = std::max(std::min(newParentExtent.y - 1, fixedSize_.y), 1);
    fields_.position.x = std::max((newParentExtent.x - fields_.extent.x) / 2, 0);
    fields_.position.y = std::max((newParentExtent.y - fields_.extent.y) / 2, 0);
}

std::function<void()> FearGuiDialog::checkpoint() {
    auto baseSnap = Parent::checkpoint();
    const auto savedFixed = fixedSize_;
    return [this, baseSnap = std::move(baseSnap), savedFixed]() {
        baseSnap();
        fixedSize_ = savedFixed;
    };
}

bool FearGuiDialog::inspectRead(sim::Inspect& inspector) {
    sim::Inspect::Value v{};
    if (!inspector.read(sim::Inspect::Type::Int32, kIdItgMenuFixedSize, v)) {
        return false;
    }
    const Int32 fx = std::get<sim::Inspect::Integer>(v).value;
    if (!inspector.read(sim::Inspect::Type::Int32, kIdItgMenuFixedSize, v)) {
        return false;
    }
    fixedSize_.x = fx;
    fixedSize_.y = std::get<sim::Inspect::Integer>(v).value;
    return true;
}

bool FearGuiDialog::inspectWrite(sim::Inspect& inspector) const {
    return inspector.write(kIdItgMenuFixedSize, sim::Inspect::Integer{fixedSize_.x}) &&
           inspector.write(kIdItgMenuFixedSize, sim::Inspect::Integer{fixedSize_.y});
}

Persistent::Error
FearGuiDialog::read(StreamIO& sio, Int32 version, Int32 user, Persistent::LoadContext& ctx) {
    auto snap = checkpoint();
    if (!sio.read(fixedSize_.x) || !sio.read(fixedSize_.y)) {
        snap();
        return Persistent::Error::ReadError;
    }
    const auto err = Parent::read(sio, version, user, ctx);
    if (err != Persistent::Error::Ok) {
        snap();
    }
    return err;
}

Persistent::Error FearGuiDialog::write(StreamIO& sio,
                                       Int32 version,
                                       Int32 user,
                                       Persistent::StoreContext& ctx) const {
    if (!sio.write(fixedSize_.x) || !sio.write(fixedSize_.y)) {
        return Persistent::Error::WriteError;
    }
    return Parent::write(sio, version, user, ctx);
}

} // namespace opent1::game
