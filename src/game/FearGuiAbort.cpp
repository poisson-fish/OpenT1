#include "opent1/game/shell_dialogs.hpp"

namespace opent1::game {

namespace {
inline constexpr Int32 kIdItgFontNameTag = 200131;
inline constexpr Int32 kIdItgTextTag = 200132;
} // namespace

std::function<void()> FearGuiAbortDlg::checkpoint() {
    auto baseSnap = Parent::checkpoint();
    const auto savedFont = fontNameTag_;
    const auto savedText = textTag_;
    return [this, baseSnap = std::move(baseSnap), savedFont, savedText]() {
        baseSnap();
        fontNameTag_ = savedFont;
        textTag_ = savedText;
    };
}

bool FearGuiAbortDlg::inspectRead(sim::Inspect& inspector) {
    sim::Inspect::Value v{};
    if (!inspector.read(sim::Inspect::Type::Int32, kIdItgFontNameTag, v)) {
        return false;
    }
    const Int32 fTag = std::get<sim::Inspect::Integer>(v).value;
    if (!inspector.read(sim::Inspect::Type::Int32, kIdItgTextTag, v)) {
        return false;
    }
    fontNameTag_ = fTag;
    textTag_ = std::get<sim::Inspect::Integer>(v).value;
    return true;
}

bool FearGuiAbortDlg::inspectWrite(sim::Inspect& inspector) const {
    return inspector.write(kIdItgFontNameTag, sim::Inspect::Integer{fontNameTag_}) &&
           inspector.write(kIdItgTextTag, sim::Inspect::Integer{textTag_});
}

Persistent::Error
FearGuiAbortDlg::read(StreamIO& sio, Int32 version, Int32 user, Persistent::LoadContext& ctx) {
    auto snap = checkpoint();
    if (!sio.read(fontNameTag_) || !sio.read(textTag_)) {
        snap();
        return Persistent::Error::ReadError;
    }
    const auto err = Parent::read(sio, version, user, ctx);
    if (err != Persistent::Error::Ok) {
        snap();
    }
    return err;
}

Persistent::Error FearGuiAbortDlg::write(StreamIO& sio,
                                         Int32 version,
                                         Int32 user,
                                         Persistent::StoreContext& ctx) const {
    if (!sio.write(fontNameTag_) || !sio.write(textTag_)) {
        return Persistent::Error::WriteError;
    }
    return Parent::write(sio, version, user, ctx);
}

} // namespace opent1::game
