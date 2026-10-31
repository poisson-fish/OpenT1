#include <opent1/game/shell_controls.hpp>

#include <algorithm>

namespace opent1::game {

bool FGTextFormat::setScriptValue(std::string_view rawText) {
    const Int32 w = std::max<Int32>(20, fields_.extent.x);
    if (!format(rawText, w)) {
        return false;
    }
    return resize(fields_.position, {fields_.extent.x, height() + 4});
}

Persistent::Error
FGTextFormat::read(StreamIO& sio, Int32 version, Int32 user, Persistent::LoadContext& ctx) {
    Int32 ver{};
    if (!sio.read(ver) || ver != CtrlVersion) {
        return Persistent::Error::ReadError;
    }
    return Parent::read(sio, version, user, ctx);
}

Persistent::Error
FGTextFormat::write(StreamIO& sio, Int32 version, Int32 user, Persistent::StoreContext& ctx) const {
    if (!sio.write(CtrlVersion)) {
        return Persistent::Error::WriteError;
    }
    return Parent::write(sio, version, user, ctx);
}

} // namespace opent1::game
