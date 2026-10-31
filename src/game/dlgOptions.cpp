#include "opent1/game/shell_dialogs.hpp"

namespace opent1::game {

OptionsDelegate::OptionsDelegate() {
    activeScreen_ = "Options.gui";
}

UInt32 OptionsDelegate::onMessage(sim::SimObject* /*sender*/, UInt32 msg) {
    if (msg == IDDLG_Cancel || msg == IDDLG_MainMenu) {
        activeScreen_ = "MainMenu.gui";
        lastScriptAction_ = "GuiPopDialog(MainWindow, 0);";
        return 1;
    }
    return 0;
}

} // namespace opent1::game
