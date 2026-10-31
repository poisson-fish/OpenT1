#include <opent1/game/commander_huds.hpp>

namespace opent1::game {

namespace {
inline constexpr Int32 kIdStrCurrentCommand = 131900;
inline constexpr Int32 kIdStrRadioSet = 131901;
inline constexpr Int32 kIdItgBmpRootTag = 200100;
inline constexpr Int32 kIdItgOffBmpTag = 200101;
inline constexpr Int32 kIdItgLrOffBmpTag = 200104;
inline constexpr Int32 kIdItgLrPosition = 200144;
inline constexpr Int32 kIdItgLrExtent = 200145;
} // namespace

FGCommandRadio::FGCommandRadio() {
    fields_.extent = {100, 50};
}

void FGCommandRadio::setLowRes(bool lores) {
    lowRes_ = lores;
    if (lores) {
        resize(loResPosition_, loResExtent_);
    } else {
        resize(hiResPosition_, hiResExtent_);
    }
}

void FGCommandRadio::wake() {
    hiResPosition_ = fields_.position;
    hiResExtent_ = fields_.extent;
    Parent::wake();
}

void FGCommandRadio::action() {
    if (radioTag_ != 0) {
        set_ = true;
        messageSiblings(IDSYS_SIBLING_DISABLE);
        if (gui::Active::message != 0) {
            if (auto* ctrl = findRootTag(gui::Active::message)) {
                ctrl->visible(set_);
            }
        }
        if (showCommand) {
            showCommand(cmdTag_);
        }
    } else {
        set_ = !set_;
    }
    if (canvas() && canvas()->onAction &&
        !canvas()->onAction(
            *this, gui::Active::message, set_ ? fields_.command : fields_.alternateCommand)) {
        throw std::runtime_error("GUI command radio action rejected");
    }
}

bool FGCommandRadio::draw(gfx::SoftwareSurface& surface, Point2I offset, RectI clip) const {
    const std::size_t base = lowRes_ ? 2 : 0;
    auto bitmap = commandBitmaps_[base + ((set_ || depressed) ? 0 : 1)];
    if (!bitmap) {
        bitmap = commandBitmaps_[base];
    }
    const auto previous = surface.clip();
    surface.setClip(clip);
    const bool drawn = !bitmap || surface.blit(*bitmap, offset);
    surface.setClip(previous);
    return drawn;
}

bool FGCommandRadio::message(gui::Control& sender, UInt32 msg) {
    if (msg == IDSYS_SIBLING_DISABLE && &sender != this) {
        if (const auto* other = dynamic_cast<const FGCommandRadio*>(&sender)) {
            if (other->radioTag() == radioTag_) {
                set_ = false;
                if (gui::Active::message != 0) {
                    if (auto* ctrl = findRootTag(gui::Active::message)) {
                        ctrl->visible(false);
                    }
                }
                return true;
            }
        }
    }
    return gui::Control::message(sender, msg);
}

std::function<void()> FGCommandRadio::checkpoint() {
    auto base = Parent::checkpoint();
    const auto ct = cmdTag_;
    const auto rt = radioTag_;
    const auto bon = bmpOnTag_;
    const auto boff = bmpOffTag_;
    const auto lbon = loResBmpOnTag_;
    const auto lboff = loResBmpOffTag_;
    const auto lp = loResPosition_;
    const auto le = loResExtent_;
    const auto lr = lowRes_;
    return [this, base = std::move(base), ct, rt, bon, boff, lbon, lboff, lp, le, lr] {
        base();
        cmdTag_ = ct;
        radioTag_ = rt;
        bmpOnTag_ = bon;
        bmpOffTag_ = boff;
        loResBmpOnTag_ = lbon;
        loResBmpOffTag_ = lboff;
        loResPosition_ = lp;
        loResExtent_ = le;
        lowRes_ = lr;
    };
}

bool FGCommandRadio::inspectWrite(sim::Inspect& inspector) const {
    if (!Parent::inspectWrite(inspector)) {
        return false;
    }
    inspector.write(kIdStrCurrentCommand, sim::Inspect::Integer{cmdTag_});
    inspector.write(kIdStrRadioSet, sim::Inspect::Integer{radioTag_});
    inspector.write(kIdItgBmpRootTag, sim::Inspect::Integer{bmpOnTag_});
    inspector.write(kIdItgOffBmpTag, sim::Inspect::Integer{bmpOffTag_});
    inspector.write(kIdItgLrPosition, loResPosition_);
    inspector.write(kIdItgLrExtent, loResExtent_);
    inspector.write(kIdItgBmpRootTag, sim::Inspect::Integer{loResBmpOnTag_});
    inspector.write(kIdItgLrOffBmpTag, sim::Inspect::Integer{loResBmpOffTag_});
    return true;
}

bool FGCommandRadio::inspectRead(sim::Inspect& inspector) {
    if (!Parent::inspectRead(inspector)) {
        return false;
    }
    sim::Inspect::Value v{};
    if (!inspector.read(sim::Inspect::Type::Int32, kIdStrCurrentCommand, v)) {
        return false;
    }
    cmdTag_ = std::get<sim::Inspect::Integer>(v).value;
    if (!inspector.read(sim::Inspect::Type::Int32, kIdStrRadioSet, v)) {
        return false;
    }
    radioTag_ = std::get<sim::Inspect::Integer>(v).value;
    if (!inspector.read(sim::Inspect::Type::Int32, kIdItgBmpRootTag, v)) {
        return false;
    }
    bmpOnTag_ = std::get<sim::Inspect::Integer>(v).value;
    if (!inspector.read(sim::Inspect::Type::Int32, kIdItgOffBmpTag, v)) {
        return false;
    }
    bmpOffTag_ = std::get<sim::Inspect::Integer>(v).value;
    if (!inspector.read(sim::Inspect::Type::Point2I, kIdItgLrPosition, v)) {
        return false;
    }
    loResPosition_ = std::get<Point2I>(v);
    if (!inspector.read(sim::Inspect::Type::Point2I, kIdItgLrExtent, v)) {
        return false;
    }
    loResExtent_ = std::get<Point2I>(v);
    if (!inspector.read(sim::Inspect::Type::Int32, kIdItgBmpRootTag, v)) {
        return false;
    }
    loResBmpOnTag_ = std::get<sim::Inspect::Integer>(v).value;
    if (!inspector.read(sim::Inspect::Type::Int32, kIdItgLrOffBmpTag, v)) {
        return false;
    }
    loResBmpOffTag_ = std::get<sim::Inspect::Integer>(v).value;
    return true;
}

Persistent::Error
FGCommandRadio::read(StreamIO& sio, Int32 version, Int32 user, Persistent::LoadContext& ctx) {
    Int32 ct{};
    Int32 rt{};
    Int32 bon{};
    Int32 boff{};
    Int32 lbon{};
    Int32 lboff{};
    Point2I lp{};
    Point2I le{};
    if (!sio.read(ct) || !sio.read(rt) || !sio.read(bon) || !sio.read(boff) || !sio.read(lbon) ||
        !sio.read(lboff) || !sio.read(lp.x) || !sio.read(lp.y) || !sio.read(le.x) ||
        !sio.read(le.y)) {
        return Persistent::Error::ReadError;
    }
    if (le.x < 0 || le.y < 0 || le.x > 32768 || le.y > 32768) {
        return Persistent::Error::ReadError;
    }
    const auto err = Parent::read(sio, version, user, ctx);
    if (err != Persistent::Error::Ok) {
        return err;
    }
    cmdTag_ = ct;
    radioTag_ = rt;
    bmpOnTag_ = bon;
    bmpOffTag_ = boff;
    loResBmpOnTag_ = lbon;
    loResBmpOffTag_ = lboff;
    loResPosition_ = lp;
    loResExtent_ = le;
    return Persistent::Error::Ok;
}

Persistent::Error FGCommandRadio::write(StreamIO& sio,
                                        Int32 version,
                                        Int32 user,
                                        Persistent::StoreContext& ctx) const {
    if (loResExtent_.x < 0 || loResExtent_.y < 0 || loResExtent_.x > 32768 ||
        loResExtent_.y > 32768) {
        return Persistent::Error::WriteError;
    }
    if (!sio.write(cmdTag_) || !sio.write(radioTag_) || !sio.write(bmpOnTag_) ||
        !sio.write(bmpOffTag_) || !sio.write(loResBmpOnTag_) || !sio.write(loResBmpOffTag_) ||
        !sio.write(loResPosition_.x) || !sio.write(loResPosition_.y) ||
        !sio.write(loResExtent_.x) || !sio.write(loResExtent_.y)) {
        return Persistent::Error::WriteError;
    }
    return Parent::write(sio, version, user, ctx);
}

} // namespace opent1::game
