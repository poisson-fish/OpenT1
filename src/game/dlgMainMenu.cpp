#include "opent1/game/shell_dialogs.hpp"

namespace opent1::game {

MainMenuDelegate::MainMenuDelegate() {
    activeScreen_ = "MainMenu.gui";
}

UInt32 MainMenuDelegate::onMessage(sim::SimObject* /*sender*/, UInt32 msg) {
    switch (msg) {
        case IDDLG_QuickStart:
        case IDDLG_MultiPlayer:
            activeScreen_ = "Connect.gui";
            lastScriptAction_ = "GuiPushDialog(MainWindow, \"gui\\\\Connect.gui\");";
            return 1;
        case IDDLG_SinglePlayer:
            activeScreen_ = "MissionSelect.gui";
            lastScriptAction_ = "GuiPushDialog(MainWindow, \"gui\\\\MissionSelect.gui\");";
            return 1;
        case IDDLG_Options:
            activeScreen_ = "Options.gui";
            lastScriptAction_ = "GuiPushDialog(MainWindow, \"gui\\\\Options.gui\");";
            return 1;
        case IDDLG_Recordings:
            activeScreen_ = "Recordings.gui";
            lastScriptAction_ = "GuiPushDialog(MainWindow, \"gui\\\\Recordings.gui\");";
            return 1;
        case IDDLG_Quit:
            activeScreen_ = "Quit.gui";
            lastScriptAction_ = "GuiPushDialog(MainWindow, \"gui\\\\Quit.gui\");";
            return 1;
        default:
            return 0;
    }
}

} // namespace opent1::game
