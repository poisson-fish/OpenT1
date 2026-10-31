#include "opent1/game/shell_dialogs.hpp"

namespace opent1::game {

ConnectDelegate::ConnectDelegate() {
    activeScreen_ = "Connect.gui";
}

UInt32 ConnectDelegate::onMessage(sim::SimObject* /*sender*/, UInt32 msg) {
    switch (msg) {
        case IDDLG_JoinGame:
            activeScreen_ = "JoinGame.gui";
            lastScriptAction_ = "connect(\"" + serverAddress_ + "\");";
            return 1;
        case IDDLG_HostGame:
            activeScreen_ = "CreateServer.gui";
            lastScriptAction_ = "GuiLoadContentCtrl(MainWindow, \"gui\\\\CreateServer.gui\");";
            return 1;
        case IDDLG_PlayerSetup:
            activeScreen_ = "PlayerSetup.gui";
            lastScriptAction_ = "GuiLoadContentCtrl(MainWindow, \"gui\\\\PlayerSetup.gui\");";
            return 1;
        case IDDLG_IRCChat:
            activeScreen_ = "IRCChat.gui";
            lastScriptAction_ = "GuiLoadContentCtrl(MainWindow, \"gui\\\\IRCChat.gui\");";
            return 1;
        case IDDLG_MainMenu:
            activeScreen_ = "MainMenu.gui";
            lastScriptAction_ = "GuiLoadContentCtrl(MainWindow, \"gui\\\\MainMenu.gui\");";
            return 1;
        default:
            return 0;
    }
}

} // namespace opent1::game
