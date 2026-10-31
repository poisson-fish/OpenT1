#include <opent1/game/shell_controls.hpp>
#include <opent1/sim/sim_input_action.hpp>

#include <cmath>

namespace opent1::game {

namespace {
inline constexpr UInt16 kModShift = 0x0001U;
inline constexpr UInt16 kModCtrl = 0x0002U;
inline constexpr UInt16 kModAlt = 0x0004U;
inline constexpr UInt16 kModLShift = 0x0008U;
inline constexpr UInt16 kModRShift = 0x0010U;
inline constexpr UInt16 kModLCtrl = 0x0020U;
inline constexpr UInt16 kModRCtrl = 0x0040U;
inline constexpr UInt16 kModLAlt = 0x0080U;
inline constexpr UInt16 kModRAlt = 0x0100U;

inline constexpr Int32 kIdItgActionMap = 200160;
inline constexpr Int32 kIdItgActionMake = 200162;
inline constexpr Int32 kIdItgActionBreak = 200163;
inline constexpr Int32 kIdItgActionMakeValue = 200164;
inline constexpr Int32 kIdItgActionBreakValue = 200165;
inline constexpr Int32 kIdItgActionFlags = 200166;
} // namespace

CFGButton::CFGButton() {
    fields_.extent = {120, 28};
}

bool CFGButton::draw(gfx::SoftwareSurface& s, Point2I p, RectI clip) const {
    const auto f = !active && disabledFont                ? disabledFont
                   : (over || depressed) && highlightFont ? highlightFont
                                                          : font;
    const auto bmp = bitmaps_[!active ? 2 : depressed ? 1 : 0];
    if (f && !s.text(*f,
                     {p.x + (fields_.extent.x - static_cast<Int32>(f->textWidth(text))) / 2, p.y},
                     text)) {
        return false;
    }
    const auto top = p.y + (f ? f->info.height : 10);
    if (bmp && !s.blit(*bmp, {p.x + (fields_.extent.x - bmp->width) / 2, top})) {
        return false;
    }
    std::string label;
    for (const auto c : bindingLabel_) {
        if (c == 1) {
            label += "Shift + ";
        } else if (c == 2) {
            label += "Ctrl + ";
        } else if (c == 3) {
            label += "Alt + ";
        } else {
            label += c;
        }
    }
    if (capturing_) {
        label = "Press a key...";
    }
    if (f &&
        !s.text(*f,
                {p.x + (fields_.extent.x - static_cast<Int32>(f->textWidth(label))) / 2, top + 2},
                label)) {
        return false;
    }
    return drawChildren(s, p, clip);
}

void CFGButton::action() {
    if (active && rebind && canvas()) {
        capturing_ = true;
        canvas()->makeFirstResponder(this);
        canvas()->lockMouse(this);
    }
}

bool CFGButton::event(const gui::Event& e) {
    if (!capturing_) {
        return Parent::event(e);
    }
    if (e.type == gui::EventType::Cancel ||
        (e.type == gui::EventType::KeyDown && e.key == gui::Key::Escape)) {
        capturing_ = false;
        if (canvas() && canvas()->capture() == this) {
            canvas()->unlockMouse();
        }
        return true;
    }
    BindingSpec spec;
    std::string name;
    if (e.type == gui::EventType::KeyDown && !e.repeat) {
        name = e.text;
        const auto code = name.size() == 1 ? sim::inputKeyCode(name[0]) : sim::inputKeyCode(name);
        if (!code) {
            return true;
        }
        spec = {sim::InputKeyboard, 0, sim::InputKey, code, 0, true};
    } else if (e.type == gui::EventType::Down) {
        const UInt8 index = e.button == 1   ? 0
                            : e.button == 3 ? 1
                            : e.button == 2 ? 2
                                            : UInt8(e.button - 1);
        spec = {sim::InputMouse, 0, sim::InputButton, index, 0, true};
        name = "Mouse " + std::to_string(index + 1);
    } else {
        return true;
    }
    spec.modifier = UInt16((e.shift ? 1 : 0) | (e.control ? 2 : 0) | (e.alt ? 4 : 0));
    if (rebind && rebind(*this, spec, name)) {
        setBinding(spec, name);
        capturing_ = false;
        if (canvas() && canvas()->capture() == this) {
            canvas()->unlockMouse();
        }
    }
    return true;
}

UInt16 CFGButton::normalizeModifiers(UInt16 rawMods) noexcept {
    UInt16 out = rawMods;
    if ((out & (kModLShift | kModRShift)) != 0U) {
        out &= static_cast<UInt16>(~(kModLShift | kModRShift));
        out |= kModShift;
    }
    if ((out & (kModLCtrl | kModRCtrl)) != 0U) {
        out &= static_cast<UInt16>(~(kModLCtrl | kModRCtrl));
        out |= kModCtrl;
    }
    if ((out & (kModLAlt | kModRAlt)) != 0U) {
        out &= static_cast<UInt16>(~(kModLAlt | kModRAlt));
        out |= kModAlt;
    }
    return out;
}

std::string CFGButton::formatBindingLabel(const BindingSpec& spec, std::string_view keyName) {
    if (!spec.bound || keyName.empty()) {
        return "<None>";
    }
    const UInt16 norm = normalizeModifiers(spec.modifier);
    std::string out{};
    if ((norm & kModShift) != 0U) {
        out.push_back('\x01');
    }
    if ((norm & kModCtrl) != 0U) {
        out.push_back('\x02');
    }
    if ((norm & kModAlt) != 0U) {
        out.push_back('\x03');
    }
    out.append(keyName);
    return out;
}

void CFGButton::setBinding(const BindingSpec& spec, std::string_view keyName) {
    binding_ = spec;
    binding_.modifier = normalizeModifiers(spec.modifier);
    bindingLabel_ = formatBindingLabel(binding_, keyName);
}

void CFGButton::clearBinding() {
    binding_ = {};
    bindingLabel_ = "<None>";
}

bool CFGButton::bindingConflict(const BindingSpec& candidate) const noexcept {
    if (!binding_.bound || !candidate.bound) {
        return false;
    }
    return binding_.deviceType == candidate.deviceType &&
           binding_.deviceInst == candidate.deviceInst && binding_.objType == candidate.objType &&
           binding_.objInst == candidate.objInst &&
           normalizeModifiers(binding_.modifier) == normalizeModifiers(candidate.modifier);
}

std::function<void()> CFGButton::checkpoint() {
    auto base = Parent::checkpoint();
    auto am = actionMap_;
    const auto ma = makeAction_;
    const auto mv = makeValue_;
    const auto ba = breakAction_;
    const auto bv = breakValue_;
    const auto fl = flags_;
    const auto bnd = binding_;
    auto lbl = bindingLabel_;
    const auto bitmaps = bitmaps_;
    const auto capturing = capturing_;
    return [this,
            base = std::move(base),
            am = std::move(am),
            ma,
            mv,
            ba,
            bv,
            fl,
            bnd,
            lbl = std::move(lbl),
            bitmaps,
            capturing] {
        base();
        actionMap_ = am;
        makeAction_ = ma;
        makeValue_ = mv;
        breakAction_ = ba;
        breakValue_ = bv;
        flags_ = fl;
        binding_ = bnd;
        bindingLabel_ = lbl;
        bitmaps_ = bitmaps;
        capturing_ = capturing;
    };
}

bool CFGButton::inspectWrite(sim::Inspect& inspector) const {
    inspector.write(kIdItgActionMap, actionMap_);
    inspector.write(kIdItgActionMake, sim::Inspect::Integer{makeAction_});
    inspector.write(kIdItgActionMakeValue, makeValue_);
    inspector.write(kIdItgActionBreak, sim::Inspect::Integer{breakAction_});
    inspector.write(kIdItgActionBreakValue, breakValue_);
    inspector.write(kIdItgActionFlags, sim::Inspect::Integer{flags_});
    return true;
}

bool CFGButton::inspectRead(sim::Inspect& inspector) {
    sim::Inspect::Value v{};
    if (!inspector.read(sim::Inspect::Type::IString, kIdItgActionMap, v)) {
        return false;
    }
    actionMap_ = std::get<std::string>(v);
    if (!inspector.read(sim::Inspect::Type::Int32, kIdItgActionMake, v)) {
        return false;
    }
    makeAction_ = std::get<sim::Inspect::Integer>(v).value;
    if (!inspector.read(sim::Inspect::Type::RealF, kIdItgActionMakeValue, v)) {
        return false;
    }
    makeValue_ = std::get<Flt32>(v);
    if (!inspector.read(sim::Inspect::Type::Int32, kIdItgActionBreak, v)) {
        return false;
    }
    breakAction_ = std::get<sim::Inspect::Integer>(v).value;
    if (!inspector.read(sim::Inspect::Type::RealF, kIdItgActionBreakValue, v)) {
        return false;
    }
    breakValue_ = std::get<Flt32>(v);
    if (!inspector.read(sim::Inspect::Type::Int32, kIdItgActionFlags, v)) {
        return false;
    }
    flags_ = std::get<sim::Inspect::Integer>(v).value;
    return true;
}

Persistent::Error
CFGButton::read(StreamIO& sio, Int32 version, Int32 user, Persistent::LoadContext& ctx) {
    Int32 localVer{};
    std::string am{};
    Int32 ma{};
    Flt32 mv{};
    Int32 ba{};
    Flt32 bv{};
    Int32 fl{};
    if (!sio.read(localVer) || localVer != CtrlVersion || !sio.readString(am) || !sio.read(ma) ||
        !sio.read(mv) || !sio.read(ba) || !sio.read(bv) || !sio.read(fl) || !std::isfinite(mv) ||
        !std::isfinite(bv)) {
        return Persistent::Error::ReadError;
    }
    const auto err = Parent::read(sio, version, user, ctx);
    if (err != Persistent::Error::Ok) {
        return err;
    }
    actionMap_ = std::move(am);
    makeAction_ = ma;
    makeValue_ = mv;
    breakAction_ = ba;
    breakValue_ = bv;
    flags_ = fl;
    return Persistent::Error::Ok;
}

Persistent::Error
CFGButton::write(StreamIO& sio, Int32 version, Int32 user, Persistent::StoreContext& ctx) const {
    if (actionMap_.size() > 255 || !std::isfinite(makeValue_) || !std::isfinite(breakValue_)) {
        return Persistent::Error::WriteError;
    }
    if (!sio.write(CtrlVersion) || !sio.writeString(actionMap_) || !sio.write(makeAction_) ||
        !sio.write(makeValue_) || !sio.write(breakAction_) || !sio.write(breakValue_) ||
        !sio.write(flags_)) {
        return Persistent::Error::WriteError;
    }
    return Parent::write(sio, version, user, ctx);
}

} // namespace opent1::game
