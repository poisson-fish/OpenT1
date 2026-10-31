#include <opent1/game/shell_controls.hpp>

namespace opent1::game {

namespace {
inline constexpr Int32 kIdItgTabPageCtrl = 200140;
inline constexpr Int32 kIdItgTabSet = 200141;
inline constexpr Int32 kIdItgTabRowCount = 200142;
} // namespace

void FGTab::setState(bool on) {
    set_ = on;
    if (pageTag_ > 0 && parent() != nullptr) {
        if (auto* page = parent()->findTag(static_cast<UInt32>(pageTag_))) {
            page->visible(on);
        }
    }
    if (on) {
        messageSiblings(IDSYS_SIBLING_DISABLE);
    }
}

bool FGTab::message(gui::Control& sender, UInt32 msg) {
    if (msg == IDSYS_SIBLING_DISABLE && &sender != this) {
        setState(false);
        return true;
    }
    return gui::Control::message(sender, msg);
}

std::function<void()> FGTab::checkpoint() {
    auto base = Parent::checkpoint();
    const auto pt = pageTag_;
    const auto st = set_;
    return [this, base = std::move(base), pt, st] {
        base();
        pageTag_ = pt;
        set_ = st;
    };
}

bool FGTab::inspectWrite(sim::Inspect& inspector) const {
    inspector.write(kIdItgTabPageCtrl, sim::Inspect::Integer{pageTag_});
    inspector.write(kIdItgTabSet, set_);
    return true;
}

bool FGTab::inspectRead(sim::Inspect& inspector) {
    sim::Inspect::Value v{};
    if (!inspector.read(sim::Inspect::Type::Int32, kIdItgTabPageCtrl, v)) {
        return false;
    }
    pageTag_ = std::get<sim::Inspect::Integer>(v).value;
    if (!inspector.read(sim::Inspect::Type::Bool, kIdItgTabSet, v)) {
        return false;
    }
    set_ = std::get<bool>(v);
    return true;
}

Persistent::Error
FGTab::read(StreamIO& sio, Int32 version, Int32 user, Persistent::LoadContext& ctx) {
    Int32 pt{};
    Int32 st{};
    if (!sio.read(pt) || !sio.read(st)) {
        return Persistent::Error::ReadError;
    }
    const auto err = Parent::read(sio, version, user, ctx);
    if (err != Persistent::Error::Ok) {
        return err;
    }
    pageTag_ = pt;
    set_ = (st != 0);
    return Persistent::Error::Ok;
}

Persistent::Error
FGTab::write(StreamIO& sio, Int32 version, Int32 user, Persistent::StoreContext& ctx) const {
    const Int32 st = set_ ? 1 : 0;
    if (!sio.write(pageTag_) || !sio.write(st)) {
        return Persistent::Error::WriteError;
    }
    return Parent::write(sio, version, user, ctx);
}

bool FGTabMenu::activateTabIndex(std::size_t index) {
    std::size_t cur = 0;
    for (auto* obj : objects()) {
        if (auto* tab = dynamic_cast<FGTab*>(obj)) {
            if (cur == index) {
                tab->setState(true);
                return true;
            }
            ++cur;
        }
    }
    return false;
}

std::function<void()> FGTabMenu::checkpoint() {
    auto base = Parent::checkpoint();
    const auto rc = rowCount_;
    return [this, base = std::move(base), rc] {
        base();
        rowCount_ = rc;
    };
}

bool FGTabMenu::inspectWrite(sim::Inspect& inspector) const {
    inspector.write(kIdItgTabRowCount, sim::Inspect::Integer{rowCount_});
    return true;
}

bool FGTabMenu::inspectRead(sim::Inspect& inspector) {
    sim::Inspect::Value v{};
    if (!inspector.read(sim::Inspect::Type::Int32, kIdItgTabRowCount, v)) {
        return false;
    }
    rowCount_ = std::get<sim::Inspect::Integer>(v).value;
    return true;
}

Persistent::Error
FGTabMenu::read(StreamIO& sio, Int32 version, Int32 user, Persistent::LoadContext& ctx) {
    Int32 rc{};
    if (!sio.read(rc) || rc < 0 || rc > 64) {
        return Persistent::Error::ReadError;
    }
    const auto err = Parent::read(sio, version, user, ctx);
    if (err != Persistent::Error::Ok) {
        return err;
    }
    rowCount_ = rc;
    return Persistent::Error::Ok;
}

Persistent::Error
FGTabMenu::write(StreamIO& sio, Int32 version, Int32 user, Persistent::StoreContext& ctx) const {
    if (rowCount_ < 0 || rowCount_ > 64 || !sio.write(rowCount_)) {
        return Persistent::Error::WriteError;
    }
    return Parent::write(sio, version, user, ctx);
}

} // namespace opent1::game
