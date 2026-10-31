#include <opent1/game/shell_controls.hpp>

namespace opent1::game {

namespace {
inline constexpr Int32 kIdItgBmpRootTag = 200100;
inline constexpr Int32 kIdItgOffBmpTag = 200101;
inline constexpr Int32 kIdItgTargetControlTag = 200102;
inline constexpr Int32 kIdStrAiActive = 131411;
} // namespace

void FearGuiSwitch::action() {
    set_ = !set_;
    if (targetTag_ > 0) {
        if (auto* target = findRootTag(static_cast<UInt32>(targetTag_))) {
            target->visible(set_);
        }
    }
    Parent::action();
}

std::function<void()> FearGuiSwitch::checkpoint() {
    auto base = Parent::checkpoint();
    const auto on = bmpOnTag_;
    const auto off = bmpOffTag_;
    const auto tgt = targetTag_;
    const auto st = set_;
    return [this, base = std::move(base), on, off, tgt, st] {
        base();
        bmpOnTag_ = on;
        bmpOffTag_ = off;
        targetTag_ = tgt;
        set_ = st;
    };
}

bool FearGuiSwitch::inspectWrite(sim::Inspect& inspector) const {
    inspector.write(kIdItgBmpRootTag, sim::Inspect::Integer{bmpOnTag_});
    inspector.write(kIdItgOffBmpTag, sim::Inspect::Integer{bmpOffTag_});
    inspector.write(kIdItgTargetControlTag, sim::Inspect::Integer{targetTag_});
    inspector.write(kIdStrAiActive, set_);
    return true;
}

bool FearGuiSwitch::inspectRead(sim::Inspect& inspector) {
    sim::Inspect::Value v{};
    if (!inspector.read(sim::Inspect::Type::Int32, kIdItgBmpRootTag, v)) {
        return false;
    }
    bmpOnTag_ = std::get<sim::Inspect::Integer>(v).value;
    if (!inspector.read(sim::Inspect::Type::Int32, kIdItgOffBmpTag, v)) {
        return false;
    }
    bmpOffTag_ = std::get<sim::Inspect::Integer>(v).value;
    if (!inspector.read(sim::Inspect::Type::Int32, kIdItgTargetControlTag, v)) {
        return false;
    }
    targetTag_ = std::get<sim::Inspect::Integer>(v).value;
    if (!inspector.read(sim::Inspect::Type::Bool, kIdStrAiActive, v)) {
        return false;
    }
    set_ = std::get<bool>(v);
    return true;
}

Persistent::Error
FearGuiSwitch::read(StreamIO& sio, Int32 version, Int32 user, Persistent::LoadContext& ctx) {
    Int32 on{};
    Int32 off{};
    Int32 tgt{};
    bool st{};
    if (!sio.read(on) || !sio.read(off) || !sio.read(tgt) || !sio.read(st)) {
        return Persistent::Error::ReadError;
    }
    const auto err = Parent::read(sio, version, user, ctx);
    if (err != Persistent::Error::Ok) {
        return err;
    }
    bmpOnTag_ = on;
    bmpOffTag_ = off;
    targetTag_ = tgt;
    set_ = st;
    return Persistent::Error::Ok;
}

Persistent::Error FearGuiSwitch::write(StreamIO& sio,
                                       Int32 version,
                                       Int32 user,
                                       Persistent::StoreContext& ctx) const {
    if (!sio.write(bmpOnTag_) || !sio.write(bmpOffTag_) || !sio.write(targetTag_) ||
        !sio.write(set_)) {
        return Persistent::Error::WriteError;
    }
    return Parent::write(sio, version, user, ctx);
}

} // namespace opent1::game
