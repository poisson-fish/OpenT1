#include "opent1/game/shell_dialogs.hpp"

namespace opent1::game {

JoinGameDelegate::JoinGameDelegate() {
    activeScreen_ = "JoinGame.gui";
}

UInt32 JoinGameDelegate::onMessage(sim::SimObject* /*sender*/, UInt32 msg) {
    if (msg == IDDLG_StartGame || msg == IDDLG_JoinGame) {
        activeScreen_ = "Play.gui";
        lastScriptAction_ = "JoinSelectedServer();";
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
