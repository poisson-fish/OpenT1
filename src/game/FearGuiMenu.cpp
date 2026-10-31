#include <opent1/game/chat_inventory_huds.hpp>
#include <opent1/game/hud_panel.hpp>

namespace opent1::game {

namespace {
inline constexpr Int32 kIdItgMenuFixedSize = 200120;
} // namespace

FearGuiMenu::FearGuiMenu() {
    fields_.extent = {1, 1};
    fixedSize_ = {1, 1};
}

bool FearGuiMenu::draw(gfx::SoftwareSurface& surface, Point2I offset, RectI clip) const {
    if (!drawHudPanel(
            surface,
            {offset.x, offset.y, offset.x + fields_.extent.x, offset.y + fields_.extent.y},
            6)) {
        return false;
    }
    return drawChildren(surface, offset, clip);
}

bool FearGuiMenu::onSimActionEvent(const sim::SimActionEvent& event) {
    switch (event.action) {
        case IDACTION_TOGGLE:
            visible(!visible());
            return true;
        case IDACTION_TURN_ON:
            visible(true);
            return true;
        case IDACTION_TURN_OFF:
            visible(false);
            return true;
        default:
            return false;
    }
}

bool FearGuiMenu::processEvent(const sim::SimEvent& event) {
    if (event.type == sim::ActionEventType || event.type == SimActionEventType) {
        if (const auto* act = dynamic_cast<const sim::SimActionEvent*>(&event)) {
            if (onSimActionEvent(*act)) {
                return true;
            }
        }
    }
    return Parent::processEvent(event);
}

std::function<void()> FearGuiMenu::checkpoint() {
    auto base = Parent::checkpoint();
    const auto fs = fixedSize_;
    return [this, base = std::move(base), fs] {
        base();
        fixedSize_ = fs;
    };
}

bool FearGuiMenu::inspectWrite(sim::Inspect& inspector) const {
    inspector.write(kIdItgMenuFixedSize, fixedSize_);
    return true;
}

bool FearGuiMenu::inspectRead(sim::Inspect& inspector) {
    sim::Inspect::Value v{};
    if (!inspector.read(sim::Inspect::Type::Point2I, kIdItgMenuFixedSize, v)) {
        return false;
    }
    fixedSize_ = std::get<Point2I>(v);
    return true;
}

Persistent::Error
FearGuiMenu::read(StreamIO& sio, Int32 version, Int32 user, Persistent::LoadContext& ctx) {
    Point2I fs{};
    if (!sio.read(fs.x) || !sio.read(fs.y) || fs.x < 0 || fs.y < 0 || fs.x > 32768 ||
        fs.y > 32768) {
        return Persistent::Error::ReadError;
    }
    const auto err = Parent::read(sio, version, user, ctx);
    if (err != Persistent::Error::Ok) {
        return err;
    }
    fixedSize_ = fs;
    return Persistent::Error::Ok;
}

Persistent::Error
FearGuiMenu::write(StreamIO& sio, Int32 version, Int32 user, Persistent::StoreContext& ctx) const {
    if (fixedSize_.x < 0 || fixedSize_.y < 0 || fixedSize_.x > 32768 || fixedSize_.y > 32768) {
        return Persistent::Error::WriteError;
    }
    if (!sio.write(fixedSize_.x) || !sio.write(fixedSize_.y)) {
        return Persistent::Error::WriteError;
    }
    return Parent::write(sio, version, user, ctx);
}

} // namespace opent1::game
