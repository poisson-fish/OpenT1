#include <opent1/game/hud_controls.hpp>

namespace opent1::game {

namespace {
inline constexpr Int32 kIdStrAiActive = 131411;
} // namespace

bool FearGuiRadio::onAdd() {
    return Parent::onAdd();
}

void FearGuiRadio::action() {
    set_ = true;
    messageSiblings(IDSYS_SIBLING_DISABLE);
    if (gui::Active::message != 0) {
        messageSiblings(gui::Active::message);
    }
    Parent::action();
}

bool FearGuiRadio::message(gui::Control& sender, UInt32 msg) {
    if (msg == IDSYS_SIBLING_DISABLE && &sender != this) {
        set_ = false;
        return true;
    }
    return gui::Control::message(sender, msg);
}

bool FearGuiRadio::onSimActionEvent(const sim::SimActionEvent& event) {
    switch (event.action) {
        case IDACTION_TOGGLE:
            set_ = !set_;
            return true;
        case IDACTION_TURN_ON:
            set_ = true;
            return true;
        case IDACTION_TURN_OFF:
            set_ = false;
            return true;
        default:
            return false;
    }
}

bool FearGuiRadio::processEvent(const sim::SimEvent& event) {
    if (const auto* act = dynamic_cast<const sim::SimActionEvent*>(&event)) {
        if (onSimActionEvent(*act)) {
            return true;
        }
    }
    return Parent::processEvent(event);
}

std::function<void()> FearGuiRadio::checkpoint() {
    auto base = Parent::checkpoint();
    const bool s = set_;
    return [this, base = std::move(base), s] {
        base();
        set_ = s;
    };
}

bool FearGuiRadio::inspectWrite(sim::Inspect& inspector) const {
    inspector.write(kIdStrAiActive, set_);
    return true;
}

bool FearGuiRadio::inspectRead(sim::Inspect& inspector) {
    sim::Inspect::Value v{};
    if (!inspector.read(sim::Inspect::Type::Bool, kIdStrAiActive, v)) {
        return false;
    }
    set_ = std::get<bool>(v);
    return true;
}

Persistent::Error
FearGuiRadio::read(StreamIO& sio, Int32 version, Int32 user, Persistent::LoadContext& ctx) {
    bool s{};
    if (!sio.read(s)) {
        return Persistent::Error::ReadError;
    }
    const auto err = Parent::read(sio, version, user, ctx);
    if (err != Persistent::Error::Ok) {
        return err;
    }
    set_ = s;
    return Persistent::Error::Ok;
}

Persistent::Error
FearGuiRadio::write(StreamIO& sio, Int32 version, Int32 user, Persistent::StoreContext& ctx) const {
    if (!sio.write(set_)) {
        return Persistent::Error::WriteError;
    }
    return Parent::write(sio, version, user, ctx);
}

} // namespace opent1::game
