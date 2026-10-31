#include <opent1/game/hud_controls.hpp>

#include <algorithm>
#include <cmath>

namespace opent1::game {

HudCtrl::HudCtrl() {
    fields_.position = {0, 0};
    fields_.extent = {64, 64};
    visible(true);
}

void HudCtrl::activate() {
    visible(true);
}

void HudCtrl::deactivate() {
    visible(false);
}

void HudCtrl::toggleActive() {
    visible(!visible());
}

void HudCtrl::snap() {
    auto* p = parent();
    if (p == nullptr) {
        return;
    }
    const Point2I parentExt = p->fields().extent;
    Point2I pos = fields_.position;
    const Point2I ext = fields_.extent;

    if (pos.x < SnapDistance) {
        pos.x = 0;
    } else if (pos.x + ext.x > parentExt.x - SnapDistance) {
        pos.x = std::max<Int32>(0, parentExt.x - ext.x);
    }

    if (pos.y < SnapDistance) {
        pos.y = 0;
    } else if (pos.y + ext.y > parentExt.y - SnapDistance) {
        pos.y = std::max<Int32>(0, parentExt.y - ext.y);
    }

    const Point2I oldPos = fields_.position;
    if (pos != oldPos) {
        resize(pos, ext);
        const Int32 denomX = parentExt.x - ext.x;
        const Int32 denomY = parentExt.y - ext.y;
        fracPos_.x = denomX > 0 ? static_cast<Flt32>(pos.x) / static_cast<Flt32>(denomX) : 0.0f;
        fracPos_.y = denomY > 0 ? static_cast<Flt32>(pos.y) / static_cast<Flt32>(denomY) : 0.0f;
    }
}

void HudCtrl::startDrag(Point2I mouseGlobal) {
    hitPt_ = globalToLocal(mouseGlobal);
    stateDragging_ = true;
}

void HudCtrl::dragTo(Point2I mouseGlobal) {
    if (!stateDragging_) {
        return;
    }
    auto* p = parent();
    const Point2I parentOrigin = p != nullptr ? p->localToGlobal({}) : Point2I{0, 0};
    Point2I newPos{mouseGlobal.x - parentOrigin.x - hitPt_.x,
                   mouseGlobal.y - parentOrigin.y - hitPt_.y};
    if (p != nullptr) {
        const Point2I parentExt = p->fields().extent;
        newPos.x =
            std::clamp<Int32>(newPos.x, 0, std::max<Int32>(0, parentExt.x - fields_.extent.x));
        newPos.y =
            std::clamp<Int32>(newPos.y, 0, std::max<Int32>(0, parentExt.y - fields_.extent.y));
        const Int32 denomX = parentExt.x - fields_.extent.x;
        const Int32 denomY = parentExt.y - fields_.extent.y;
        fracPos_.x = denomX > 0 ? static_cast<Flt32>(newPos.x) / static_cast<Flt32>(denomX) : 0.0f;
        fracPos_.y = denomY > 0 ? static_cast<Flt32>(newPos.y) / static_cast<Flt32>(denomY) : 0.0f;
    }
    resize(newPos, fields_.extent);
    snap();
}

void HudCtrl::endDrag() {
    if (stateDragging_) {
        stateDragging_ = false;
        snap();
    }
}

bool HudCtrl::onAdd() {
    if (!Parent::onAdd()) {
        return false;
    }
    return true;
}

void HudCtrl::wake() {
    if (auto* p = parent()) {
        parentResized(p->fields().extent, p->fields().extent);
    }
}

bool HudCtrl::parentResized(Point2I, Point2I newParentExtent) {
    lowResMode_ = (newParentExtent.x < 512 || newParentExtent.y < 384);
    auto position = fields_.position, extent = fields_.extent;
    layoutInParent({}, newParentExtent, position, extent);
    return resize(position, extent);
}

void HudCtrl::layoutInParent(Point2I, Point2I parent, Point2I& position, Point2I& extent) const {
    position = {Int32(fracPos_.x * std::max(0, parent.x - extent.x)),
                Int32(fracPos_.y * std::max(0, parent.y - extent.y))};
}

bool HudCtrl::event(const gui::Event& e) {
    if (e.type == gui::EventType::Down && e.button == 1) {
        startDrag(e.point);
        if (canvas() != nullptr) {
            canvas()->lockMouse(this);
        }
        return true;
    }
    if (e.type == gui::EventType::Drag && e.button == 1 && stateDragging_) {
        dragTo(e.point);
        return true;
    }
    if (e.type == gui::EventType::Up && e.button == 1 && stateDragging_) {
        if (canvas() != nullptr && canvas()->capture() == this) {
            canvas()->unlockMouse();
        }
        endDrag();
        return true;
    }
    if (e.type == gui::EventType::Cancel) {
        stateDragging_ = false;
        return true;
    }
    return Parent::event(e);
}

bool HudCtrl::onSimActionEvent(const sim::SimActionEvent& event) {
    switch (event.action) {
        case IDACTION_TOGGLE:
            toggleActive();
            return true;
        case IDACTION_TURN_ON:
            activate();
            return true;
        case IDACTION_TURN_OFF:
            deactivate();
            return true;
        default:
            return false;
    }
}

bool HudCtrl::processEvent(const sim::SimEvent& event) {
    if (event.type == sim::ActionEventType || event.type == SimActionEventType) {
        if (const auto* act = dynamic_cast<const sim::SimActionEvent*>(&event)) {
            if (onSimActionEvent(*act)) {
                return true;
            }
        }
    }
    return Parent::processEvent(event);
}

bool HudCtrl::draw(gfx::SoftwareSurface& s, Point2I offset, RectI clip) const {
    return Parent::draw(s, offset, clip);
}

std::function<void()> HudCtrl::checkpoint() {
    auto base = Parent::checkpoint();
    const auto fp = fracPos_;
    const auto hp = hitPt_;
    const auto drag = stateDragging_;
    const auto lr = lowResMode_;
    return [this, base = std::move(base), fp, hp, drag, lr] {
        base();
        fracPos_ = fp;
        hitPt_ = hp;
        stateDragging_ = drag;
        lowResMode_ = lr;
    };
}

Persistent::Error
HudCtrl::read(StreamIO& sio, Int32 version, Int32 user, Persistent::LoadContext& ctx) {
    Point2F fp{};
    Point2I ext{};
    Int32 vis{};
    if (!sio.read(fp.x) || !sio.read(fp.y) || !sio.read(ext.x) || !sio.read(ext.y) ||
        !sio.read(vis)) {
        return Persistent::Error::ReadError;
    }
    if (!std::isfinite(fp.x) || !std::isfinite(fp.y) || ext.x < 0 || ext.y < 0 || ext.x > 32768 ||
        ext.y > 32768) {
        return Persistent::Error::ReadError;
    }
    const auto err = Parent::read(sio, version, user, ctx);
    if (err != Persistent::Error::Ok) {
        return err;
    }
    fracPos_ = fp;
    fields_.extent = ext;
    visible(vis != 0);
    return Persistent::Error::Ok;
}

Persistent::Error
HudCtrl::write(StreamIO& sio, Int32 version, Int32 user, Persistent::StoreContext& ctx) const {
    if (!std::isfinite(fracPos_.x) || !std::isfinite(fracPos_.y)) {
        return Persistent::Error::WriteError;
    }
    const Int32 vis = visible() ? 1 : 0;
    if (!sio.write(fracPos_.x) || !sio.write(fracPos_.y) || !sio.write(fields_.extent.x) ||
        !sio.write(fields_.extent.y) || !sio.write(vis)) {
        return Persistent::Error::WriteError;
    }
    return Parent::write(sio, version, user, ctx);
}

} // namespace opent1::game
