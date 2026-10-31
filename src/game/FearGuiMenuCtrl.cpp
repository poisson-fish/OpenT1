#include <opent1/game/chat_inventory_huds.hpp>

namespace opent1::game {

namespace {
inline constexpr Int32 kIdItgTabPageCtrl = 200140;
inline constexpr Int32 kIdItgTabSet = 200141;
inline constexpr Int32 kIdItgOptExpanded = 200150;
} // namespace

void FGMenuCtrl::disableAll() {
    const auto visit = [&](auto&& self, gui::Control& c) -> void {
        if (auto* item = dynamic_cast<FGMenuItem*>(&c)) {
            item->setState(false);
        }
        for (auto* child : c.objects()) {
            self(self, *static_cast<gui::Control*>(child));
        }
    };
    for (auto* child : objects()) {
        visit(visit, *static_cast<gui::Control*>(child));
    }
}

void FGMenuCtrl::wake() {
    Parent::wake();
    layout();
}

void FGMenuCtrl::layout() {
    const int width = parent() ? parent()->fields().extent.x : fields_.extent.x;
    const auto itemLayout = [&](auto&& self, FGMenuItem& item, int y) -> int {
        int height = 19;
        for (auto* o : item.objects()) {
            if (auto* child = dynamic_cast<FGMenuItem*>(o)) {
                child->visible(item.isExpanded());
                if (item.isExpanded()) {
                    height += self(self, *child, height);
                }
            }
        }
        item.resize({0, y}, {width, height});
        return height;
    };
    int height = 0;
    for (auto* o : objects()) {
        if (auto* item = dynamic_cast<FGMenuItem*>(o)) {
            height += itemLayout(itemLayout, *item, height);
        }
    }
    resize(fields_.position, {width, height});
}

void FGMenuItem::setState(bool turnOn) {
    if (turnOn) {
        for (auto* ancestor = parent(); ancestor; ancestor = ancestor->parent()) {
            if (auto* menu = dynamic_cast<FGMenuCtrl*>(ancestor)) {
                menu->disableAll();
                break;
            }
        }
    }
    set_ = turnOn;
    if (pageTag_ > 0) {
        if (auto* page = findRootTag(static_cast<UInt32>(pageTag_))) {
            page->visible(turnOn);
        }
    }
}

void FGMenuItem::wake() {
    setState(set_);
}

bool FGMenuItem::draw(gfx::SoftwareSurface& surface, Point2I p, RectI clip) const {
    auto f = !active && disabledFont           ? disabledFont
             : (set_ || over) && highlightFont ? highlightFont
                                               : font;
    const int inset = dynamic_cast<FGMenuItem*>(parent()) ? 14 : 4;
    if (f && !surface.text(*f, {p.x + inset, p.y + 2}, text)) {
        return false;
    }
    return drawChildren(surface, p, clip);
}

void FGMenuItem::action() {
    if (!active) {
        return;
    }
    if (playSound) {
        playSound("Button1.wav");
    }
    if (!objects().empty()) {
        expanded_ = !expanded_;
        for (auto* ancestor = parent(); ancestor; ancestor = ancestor->parent()) {
            if (auto* menu = dynamic_cast<FGMenuCtrl*>(ancestor)) {
                menu->layout();
                break;
            }
        }
    } else {
        setState(true);
        messageSiblings(IDSYS_SIBLING_DISABLE);
    }
    Parent::action();
}

bool FGMenuItem::message(gui::Control& sender, UInt32 msg) {
    if (msg == IDSYS_SIBLING_DISABLE && &sender != this) {
        setState(false);
        return true;
    }
    return gui::Control::message(sender, msg);
}

std::function<void()> FGMenuItem::checkpoint() {
    auto base = Parent::checkpoint();
    const auto pt = pageTag_;
    const auto st = set_;
    const auto ex = expanded_;
    return [this, base = std::move(base), pt, st, ex] {
        base();
        pageTag_ = pt;
        set_ = st;
        expanded_ = ex;
    };
}

bool FGMenuItem::inspectWrite(sim::Inspect& inspector) const {
    inspector.write(kIdItgTabPageCtrl, sim::Inspect::Integer{pageTag_});
    inspector.write(kIdItgTabSet, set_);
    inspector.write(kIdItgOptExpanded, expanded_);
    return true;
}

bool FGMenuItem::inspectRead(sim::Inspect& inspector) {
    sim::Inspect::Value v{};
    if (!inspector.read(sim::Inspect::Type::Int32, kIdItgTabPageCtrl, v)) {
        return false;
    }
    const Int32 pt = std::get<sim::Inspect::Integer>(v).value;
    if (!inspector.read(sim::Inspect::Type::Bool, kIdItgTabSet, v)) {
        return false;
    }
    const bool st = std::get<bool>(v);
    if (!inspector.read(sim::Inspect::Type::Bool, kIdItgOptExpanded, v)) {
        return false;
    }
    pageTag_ = pt;
    set_ = st;
    expanded_ = std::get<bool>(v);
    return true;
}

Persistent::Error
FGMenuItem::read(StreamIO& sio, Int32 version, Int32 user, Persistent::LoadContext& ctx) {
    Int32 pt{};
    bool st{};
    bool ex{};
    if (!sio.read(pt) || !sio.read(st) || !sio.read(ex)) {
        return Persistent::Error::ReadError;
    }
    const auto err = Parent::read(sio, version, user, ctx);
    if (err != Persistent::Error::Ok) {
        return err;
    }
    pageTag_ = pt;
    set_ = st;
    expanded_ = ex;
    return Persistent::Error::Ok;
}

Persistent::Error
FGMenuItem::write(StreamIO& sio, Int32 version, Int32 user, Persistent::StoreContext& ctx) const {
    if (!sio.write(pageTag_) || !sio.write(set_) || !sio.write(expanded_)) {
        return Persistent::Error::WriteError;
    }
    return Parent::write(sio, version, user, ctx);
}

} // namespace opent1::game
