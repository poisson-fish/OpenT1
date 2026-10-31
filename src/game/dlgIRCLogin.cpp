#include "opent1/game/shell_dialogs.hpp"

namespace opent1::game {

IRCLoginDelegate::IRCLoginDelegate() {
    activeScreen_ = "IRCLogin.gui";
}

UInt32 IRCLoginDelegate::onMessage(sim::SimObject* /*sender*/, UInt32 msg) {
    if (msg == IDDLG_IRCChat || msg == IDDLG_Connect) {
        activeScreen_ = "IRCChat.gui";
        lastScriptAction_ = "GuiLoadContentCtrl(MainWindow, \"gui\\\\IRCChat.gui\");";
        return 1;
    }
    if (msg == IDDLG_Cancel || msg == IDDLG_MainMenu) {
        activeScreen_ = "Connect.gui";
        lastScriptAction_ = "GuiLoadContentCtrl(MainWindow, \"gui\\\\Connect.gui\");";
        return 1;
    }
    return 0;
}

} // namespace opent1::game
