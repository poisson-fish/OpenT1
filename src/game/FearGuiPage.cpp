#include <opent1/game/chat_inventory_huds.hpp>

namespace opent1::game {

namespace {
inline constexpr UInt32 kIdCtgGsPageDisable = 140105U;
} // namespace

FearGuiPage::FearGuiPage() {
    visible(false);
}

bool FearGuiPage::onAdd() {
    visible(false);
    return Parent::onAdd();
}

bool FearGuiPage::message(gui::Control& sender, UInt32 msg) {
    if (msg == fields_.tag && fields_.tag != 0) {
        visible(true);
        messageSiblings(kIdCtgGsPageDisable);
        return true;
    }
    if (msg == kIdCtgGsPageDisable) {
        visible(false);
        return true;
    }
    return Parent::message(sender, msg);
}

Persistent::Error
FearGuiPage::read(StreamIO& sio, Int32 version, Int32 user, Persistent::LoadContext& ctx) {
    return Parent::read(sio, version, user, ctx);
}

Persistent::Error
FearGuiPage::write(StreamIO& sio, Int32 version, Int32 user, Persistent::StoreContext& ctx) const {
    return Parent::write(sio, version, user, ctx);
}

} // namespace opent1::game
