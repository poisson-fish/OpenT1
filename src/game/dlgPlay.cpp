#include "opent1/game/shell_dialogs.hpp"

namespace opent1::game {

PlayDelegate::PlayDelegate() {
    activeScreen_ = "Play.gui";
}

UInt32 PlayDelegate::onMessage(sim::SimObject* /*sender*/, UInt32 msg) {
    if (msg == IDDLG_MainMenu || msg == IDDLG_Cancel) {
        activeScreen_ = "MainMenu.gui";
        lastScriptAction_ = "endGame();";
        return 1;
    }
    if (msg == IDDLG_Options) {
        activeScreen_ = "Options.gui";
        lastScriptAction_ = "GuiPushDialog(MainWindow, \"gui\\\\Options.gui\");";
        return 1;
    }
    return 0;
}

} // namespace opent1::game
