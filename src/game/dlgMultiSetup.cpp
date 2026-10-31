#include "opent1/game/shell_dialogs.hpp"

namespace opent1::game {

MultiSetupDelegate::MultiSetupDelegate() {
    activeScreen_ = "MultiSetup.gui";
}

UInt32 MultiSetupDelegate::onMessage(sim::SimObject* /*sender*/, UInt32 msg) {
    if (msg == IDDLG_Connect) {
        activeScreen_ = "Connect.gui";
        lastScriptAction_ = "GuiLoadContentCtrl(MainWindow, \"gui\\\\Connect.gui\");";
        return 1;
    }
    return 0;
}

} // namespace opent1::game
