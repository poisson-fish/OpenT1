#include <opent1/game/shell_controls.hpp>

namespace opent1::game {

bool FGUniversalButton::draw(gfx::SoftwareSurface& surface, Point2I offset, RectI clip) const {
    const bool background = canvas() && canvas()->top() && !canvas()->contains(this);
    const auto index = background                                         ? (set_ ? 7 : 6)
                       : !active                                          ? 5
                       : radioTag_ > 0 && isSwitch_ && set_ && !depressed ? 0
                       : (depressed || set_)                              ? 4
                       : over                                             ? 2
                                                                          : 0;
    const auto bitmap = bitmaps_[index] ? bitmaps_[index] : bitmaps_[0];
    if (bitmap && !surface.blit(*bitmap, offset)) {
        return false;
    }
    if (radioTag_ > 0 && isSwitch_ && set_) {
        auto check =
            (!active || background) && radioDisabledBitmap_ ? radioDisabledBitmap_ : radioBitmap_;
        if (check && !surface.blit(*check, {offset.x + 1, offset.y})) {
            return false;
        }
    }
    return drawChildren(surface, offset, clip);
}

namespace {
inline constexpr Int32 kIdStrRadioSet = 131901;
inline constexpr Int32 kIdItgBmpRootTag = 200100;
inline constexpr Int32 kIdItgIsASwitch = 200103;
inline constexpr Int32 kIdItgLrPosition = 200144;
inline constexpr Int32 kIdStrAiActive = 131411;
inline constexpr Int32 kIdItgMirrorConsVar = 200152;
} // namespace

FGUniversalButton::FGUniversalButton() {
    active = true;
    fields_.extent = {80, 24};
}

bool FGUniversalButton::variableChanged(std::string_view value) {
    set_ = value == "True" || value == "TRUE" || value == "true" || value == "1";
    return true;
}

void FGUniversalButton::setBitmapRootName(std::string_view name) {
    bitmapRootName_ = std::string(name.substr(0, 255));
}

bool FGUniversalButton::event(const gui::Event& event) {
    if (active && playSound && event.type == gui::EventType::Down && event.button == 1) {
        if (!isSwitch_ || !set_) {
            playSound(isSwitch_ ? "Button2.wav" : "Button1.wav");
        }
    }
    return Parent::event(event);
}

void FGUniversalButton::action() {
    if (!active) {
        return;
    }
    if (radioTag_ > 0) {
        set_ = true;
        messageSiblings(IDSYS_SIBLING_DISABLE);
        if (gui::Active::message != 0) {
            if (auto* ctrl = findRootTag(gui::Active::message)) {
                ctrl->visible(true);
            }
        }
    } else if (isSwitch_) {
        set_ = !set_;
        if (!set_ && playSound) {
            playSound("Button1.wav");
        }
    }
    const auto& command = isSwitch_ && !set_ ? fields_.alternateCommand : fields_.command;
    if (canvas() && canvas()->onAction &&
        !canvas()->onAction(*this, gui::Active::message, command)) {
        throw std::runtime_error("GUI button action rejected");
    }
}

bool FGUniversalButton::message(gui::Control& sender, UInt32 msg) {
    if (msg == IDSYS_SIBLING_DISABLE && &sender != this) {
        if (const auto* other = dynamic_cast<const FGUniversalButton*>(&sender)) {
            if (other->radioTag() == radioTag_ && radioTag_ > 0) {
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

std::function<void()> FGUniversalButton::checkpoint() {
    auto base = Parent::checkpoint();
    const auto bitmaps = bitmaps_;
    const auto radioBitmap = radioBitmap_, radioDisabledBitmap = radioDisabledBitmap_;
    const auto rt = radioTag_;
    auto rootName = bitmapRootName_;
    const auto bt = bitmapTag_;
    const auto lp = loResPosition_;
    const auto st = set_;
    const auto sw = isSwitch_;
    const auto mv = mirrorConsoleVar_;
    const auto fl = flashing_;
    return [this,
            base = std::move(base),
            rt,
            rootName = std::move(rootName),
            bt,
            lp,
            st,
            sw,
            mv,
            fl,
            bitmaps,
            radioBitmap,
            radioDisabledBitmap] {
        base();
        radioTag_ = rt;
        bitmaps_ = bitmaps;
        radioBitmap_ = radioBitmap;
        radioDisabledBitmap_ = radioDisabledBitmap;
        bitmapRootName_ = rootName;
        bitmapTag_ = bt;
        loResPosition_ = lp;
        set_ = st;
        isSwitch_ = sw;
        mirrorConsoleVar_ = mv;
        flashing_ = fl;
    };
}

bool FGUniversalButton::inspectWrite(sim::Inspect& inspector) const {
    inspector.write(kIdItgIsASwitch, isSwitch_);
    inspector.write(kIdStrRadioSet, sim::Inspect::Integer{radioTag_});
    inspector.write(kIdItgBmpRootTag, bitmapRootName_);
    inspector.write(kIdItgBmpRootTag, sim::Inspect::Integer{bitmapTag_});
    inspector.write(kIdItgLrPosition, loResPosition_);
    inspector.write(kIdStrAiActive, set_);
    inspector.write(kIdItgMirrorConsVar, mirrorConsoleVar_);
    return true;
}

bool FGUniversalButton::inspectRead(sim::Inspect& inspector) {
    sim::Inspect::Value v{};
    if (!inspector.read(sim::Inspect::Type::Bool, kIdItgIsASwitch, v)) {
        return false;
    }
    isSwitch_ = std::get<bool>(v);
    if (!inspector.read(sim::Inspect::Type::Int32, kIdStrRadioSet, v)) {
        return false;
    }
    radioTag_ = std::get<sim::Inspect::Integer>(v).value;
    if (!inspector.read(sim::Inspect::Type::IString, kIdItgBmpRootTag, v)) {
        return false;
    }
    bitmapRootName_ = std::get<std::string>(v);
    if (!inspector.read(sim::Inspect::Type::Int32, kIdItgBmpRootTag, v)) {
        return false;
    }
    bitmapTag_ = std::get<sim::Inspect::Integer>(v).value;
    if (!inspector.read(sim::Inspect::Type::Point2I, kIdItgLrPosition, v)) {
        return false;
    }
    loResPosition_ = std::get<Point2I>(v);
    if (!inspector.read(sim::Inspect::Type::Bool, kIdStrAiActive, v)) {
        return false;
    }
    set_ = std::get<bool>(v);
    if (!inspector.read(sim::Inspect::Type::Bool, kIdItgMirrorConsVar, v)) {
        return false;
    }
    mirrorConsoleVar_ = std::get<bool>(v);
    return true;
}

Persistent::Error
FGUniversalButton::read(StreamIO& sio, Int32 version, Int32 user, Persistent::LoadContext& ctx) {
    Int32 curVer{};
    Int32 rTag{};
    if (!sio.read(curVer) || curVer < 0 || curVer > ButtonVersion || !sio.read(rTag)) {
        return Persistent::Error::ReadError;
    }
    std::string bmpName{};
    Int32 bmpTag = 0;
    if (curVer >= 2) {
        if (!sio.readString(bmpName)) {
            return Persistent::Error::ReadError;
        }
    } else {
        if (!sio.read(bmpTag)) {
            return Persistent::Error::ReadError;
        }
    }
    Point2I lrPos{};
    bool st{};
    if (!sio.read(lrPos.x) || !sio.read(lrPos.y) || !sio.read(st)) {
        return Persistent::Error::ReadError;
    }
    bool sw = true;
    if (curVer >= 1 && !sio.read(sw)) {
        return Persistent::Error::ReadError;
    }
    bool mirror = false;
    if (curVer >= 3 && !sio.read(mirror)) {
        return Persistent::Error::ReadError;
    }
    const auto err = Parent::read(sio, version, user, ctx);
    if (err != Persistent::Error::Ok) {
        return err;
    }
    radioTag_ = rTag;
    bitmapRootName_ = std::move(bmpName);
    bitmapTag_ = bmpTag;
    loResPosition_ = lrPos;
    set_ = st;
    isSwitch_ = sw;
    mirrorConsoleVar_ = mirror;
    return Persistent::Error::Ok;
}

Persistent::Error FGUniversalButton::write(StreamIO& sio,
                                           Int32 version,
                                           Int32 user,
                                           Persistent::StoreContext& ctx) const {
    if (bitmapRootName_.size() > 255) {
        return Persistent::Error::WriteError;
    }
    if (!sio.write(ButtonVersion) || !sio.write(radioTag_) || !sio.writeString(bitmapRootName_) ||
        !sio.write(loResPosition_.x) || !sio.write(loResPosition_.y) || !sio.write(set_) ||
        !sio.write(isSwitch_) || !sio.write(mirrorConsoleVar_)) {
        return Persistent::Error::WriteError;
    }
    return Parent::write(sio, version, user, ctx);
}

} // namespace opent1::game
