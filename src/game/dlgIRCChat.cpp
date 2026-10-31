#include "opent1/game/shell_dialogs.hpp"

namespace opent1::game {

IRCChatDelegate::IRCChatDelegate() {
    activeScreen_ = "IRCChat.gui";
}

UInt32 IRCChatDelegate::onMessage(sim::SimObject* /*sender*/, UInt32 msg) {
    if (msg == IDDLG_JoinGame) {
        activeScreen_ = "JoinGame.gui";
        lastScriptAction_ = "GuiLoadContentCtrl(MainWindow, \"gui\\\\JoinGame.gui\");";
        return 1;
    }
    if (msg == IDDLG_MainMenu) {
        activeScreen_ = "MainMenu.gui";
        lastScriptAction_ = "GuiLoadContentCtrl(MainWindow, \"gui\\\\MainMenu.gui\");";
        return 1;
    }
    return 0;
}

} // namespace opent1::game
