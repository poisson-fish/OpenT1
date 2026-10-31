#include "opent1/game/shell_dialogs.hpp"

namespace opent1::game {

CreateServerDelegate::CreateServerDelegate() {
    activeScreen_ = "CreateServer.gui";
}

UInt32 CreateServerDelegate::onMessage(sim::SimObject* /*sender*/, UInt32 msg) {
    if (msg == IDDLG_StartGame || msg == 140120) { // IDCTG_SERVER_CREATE in original shell graphs.
        activeScreen_ = "Play.gui";
        lastScriptAction_ = "createServer($pref::LastMission, false);";
        return 1;
    }
    if (msg == IDDLG_Cancel || msg == IDDLG_Connect) {
        activeScreen_ = "Connect.gui";
        lastScriptAction_ = "GuiLoadContentCtrl(MainWindow, \"gui\\\\Connect.gui\");";
        return 1;
    }
    return 0;
}

} // namespace opent1::game
