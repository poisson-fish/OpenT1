#include <opent1/game/shell_controls.hpp>
#include <ranges>

namespace opent1::game {

namespace {
inline constexpr Int32 kIdItgBorderWidth = 200146;
} // namespace

FearGuiScrollCtrl::FearGuiScrollCtrl() {
    borderThickness = 6;
}

void FearGuiScrollCtrl::drawBorder(gfx::SoftwareSurface& s, RectI r) const {
    // Borderless shell menus deliberately ignore the serialized editor fill.
    if (borderThickness == 0) {
        return;
    }
    if (fields_.style.opaque) {
        s.fill(r, fields_.style.fill);
    }
    const bool ghost = disabled || (canvas() && !canvas()->contains(this));
    const UInt8 colors[]{UInt8(ghost ? 236 : 240), UInt8(ghost ? 235 : 239), 244};
    for (auto i : std::views::iota(0) | std::views::take_while([&](int i) {
                      return i < std::min(3, borderThickness);
                  })) {
        s.rectangle(r, colors[i]);
        ++r.left;
        ++r.top;
        --r.right;
        --r.bottom;
    }
}

std::function<void()> FearGuiScrollCtrl::checkpoint() {
    return Parent::checkpoint();
}

bool FearGuiScrollCtrl::inspectWrite(sim::Inspect& inspector) const {
    inspector.write(kIdItgBorderWidth, sim::Inspect::Integer{borderThickness});
    return true;
}

bool FearGuiScrollCtrl::inspectRead(sim::Inspect& inspector) {
    sim::Inspect::Value v{};
    if (!inspector.read(sim::Inspect::Type::Int32, kIdItgBorderWidth, v)) {
        return false;
    }
    borderThickness = std::get<sim::Inspect::Integer>(v).value;
    return true;
}

Persistent::Error
FearGuiScrollCtrl::read(StreamIO& sio, Int32 version, Int32 user, Persistent::LoadContext& ctx) {
    Int32 curVer{};
    Int32 bt{};
    if (!sio.read(curVer) || curVer != CtrlVersion || !sio.read(bt) || bt < 0 || bt > 1024) {
        return Persistent::Error::ReadError;
    }
    const auto err = Parent::read(sio, version, user, ctx);
    if (err != Persistent::Error::Ok) {
        return err;
    }
    borderThickness = bt;
    return Persistent::Error::Ok;
}

Persistent::Error FearGuiScrollCtrl::write(StreamIO& sio,
                                           Int32 version,
                                           Int32 user,
                                           Persistent::StoreContext& ctx) const {
    if (borderThickness < 0 || borderThickness > 1024 || !sio.write(CtrlVersion) ||
        !sio.write(borderThickness)) {
        return Persistent::Error::WriteError;
    }
    return Parent::write(sio, version, user, ctx);
}

} // namespace opent1::game
