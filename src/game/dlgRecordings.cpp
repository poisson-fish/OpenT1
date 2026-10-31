#include "opent1/game/shell_dialogs.hpp"

namespace opent1::game {

RecordingsDelegate::RecordingsDelegate() {
    activeScreen_ = "Recordings.gui";
}

UInt32 RecordingsDelegate::onMessage(sim::SimObject* /*sender*/, UInt32 msg) {
    if (msg == IDDLG_PlayDemo || msg == IDDLG_StartGame) {
        activeScreen_ = "Play.gui";
        lastScriptAction_ = "playDemo($RecordingsGui::selectedDemo);";
        return 1;
    }
    if (msg == IDDLG_Cancel || msg == IDDLG_MainMenu) {
        activeScreen_ = "MainMenu.gui";
        lastScriptAction_ = "GuiLoadContentCtrl(MainWindow, \"gui\\\\MainMenu.gui\");";
        return 1;
    }
    return 0;
}

} // namespace opent1::game
