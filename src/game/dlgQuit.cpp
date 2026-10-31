#include "opent1/game/shell_dialogs.hpp"

namespace opent1::game {
namespace {
// Older shell graphs persist the original unqualified class spelling.
class LegacyFormattedText final : public FearGuiFormattedText {};
}

QuitDelegate::QuitDelegate() {
    activeScreen_ = "Quit.gui";
}

UInt32 QuitDelegate::onMessage(sim::SimObject* /*sender*/, UInt32 msg) {
    if (msg == IDDLG_Quit) {
        quitRequested_ = true;
        lastScriptAction_ = "quit();";
        return 1;
    }
    if (msg == IDDLG_Cancel || msg == IDDLG_MainMenu) {
        quitRequested_ = false;
        activeScreen_ = "MainMenu.gui";
        lastScriptAction_ = "GuiPopDialog(MainWindow, 0);";
        return 1;
    }
    return 0;
}

void registerFearGuiClasses(Persistent::ClassRegistry& registry) {
    // Ensure underlying SimGui base and container classes (e.g. ScrollContentCtrl) are registered
    gui::registerClasses(registry);
    gui::registerExtendedClasses(registry);
    gui::registerContainerClasses(registry);

    // Group 1: HUD Manager & Core HUD Controls
    registry.registerClass<FGHPaletteTable>("FearGui::FGHPaletteTable",
                                            makeFourCC('F', 'G', 'p', 't'));
    registry.registerClass<HudCtrl>("FearGui::HudCtrl", makeFourCC('F', 'G', 'h', 'c'));
    registry.registerClass<HudManager>("FearGui::HudManager");
    registry.registerClass<FGHudArrayCtrl>("FearGui::FGHudArrayCtrl",
                                           makeFourCC('F', 'G', 'h', 'a'));
    registry.registerClass<HudListCtrl>("FearGui::HudListCtrl", makeFourCC('F', 'G', 'h', 'l'));
    registry.registerClass<FGHudMatrix>("FearGui::FGHudMatrix", makeFourCC('F', 'G', 'm', 'x'));

    // Group 2: Status & Combat HUDs
    registry.registerClass<HealthHud>("FearGui::HealthHud");
    registry.registerClass<JetPackHud>("FearGui::JetPackHud");
    registry.registerClass<ClockHud>("FearGui::ClockHud");
    registry.registerClass<CompassHud>("FearGui::CompassHud");
    registry.registerClass<CurrentWeaponHud>("FearGui::CurrentWeaponHud");
    registry.registerClass<Crosshair>("FearGui::Crosshair", makeFourCC('F', 'G', 'c', 'h'));
    registry.registerClass<FGHSensorPing>("FearGui::FGHSensorPing");
    registry.registerClass<FGLagIndicator>("FearGui::FGLagIndicator",
                                           makeFourCC('F', 'G', 'l', 'g'));
    registry.registerClass<CenterPrint>("FearGui::CenterPrint", makeFourCC('F', 'C', 'p', 'R'));

    // Group 3: Chat, Menu & Inventory HUDs
    registry.registerClass<FearGuiChat>("FearGui::FearGuiChat", makeFourCC('F', 'G', 'c', 't'));
    registry.registerClass<FearGuiChatDisplay>("FearGui::FearGuiChatDisplay",
                                               makeFourCC('F', 'G', 'c', 'd'));
    registry.registerClass<FGChatMenu>("FearGui::FGChatMenu", makeFourCC('F', 'G', 'c', 'm'));
    registry.registerClass<FearGuiMenu>("FearGui::FearGuiMenu", makeFourCC('F', 'G', 'm', 'u'));
    registry.registerClass<FGMenuCtrl>("FearGui::FGMenuCtrl", makeFourCC('F', 'G', 'm', 'z'));
    registry.registerClass<FGMenuItem>("FearGui::FGMenuItem", makeFourCC('F', 'G', 'm', 'i'));
    registry.registerClass<MenuDisplayCtrl>("FearGui::MenuDisplayCtrl");
    registry.registerClass<FearGuiInventory>("FearGui::FearGuiInventory",
                                             makeFourCC('F', 'g', 'I', 'd'));
    registry.registerClass<FearGuiPurchase>("FearGui::FearGuiPurchase",
                                            makeFourCC('F', 'g', 'P', 'd'));
    registry.registerClass<InventoryListCtrl>("FearGui::InventoryListCtrl",
                                              makeFourCC('F', 'G', 'i', 'l'));
    registry.registerClass<FGHBuySell>("FearGui::FGHBuySell", makeFourCC('F', 'G', 'b', 's'));
    registry.registerClass<FGHInventory>("FearGui::FGHInventory", makeFourCC('F', 'G', 'i', 'v'));
    registry.registerClass<FHInventory>("FearGui::FHInventory");
    registry.registerClass<ScoreListCtrl>("FearGui::ScoreListCtrl", makeFourCC('F', 'G', 's', 'c'));
    registry.registerClass<FearGuiPage>("FearGui::FearGuiPage", makeFourCC('F', 'G', 'p', 'g'));

    // Group 4: Commander & Tactical Map Controls
    registry.registerClass<MissionCenterPos>("MissionCenterPos", MissionCenterPosPersTag);
    registry.registerClass<TSCommander>("FearGui::TSCommander", makeFourCC('F', 'G', 'C', 'c'));
    registry.registerClass<FGCommandView>("FearGui::FGCommandView", makeFourCC('F', 'G', 'c', 'v'));
    registry.registerClass<FGCommandBox>("FearGui::FGCommandBox", makeFourCC('F', 'G', 'c', 'x'));
    registry.registerClass<FGCommandPanel>("FearGui::FGCommandPanel",
                                           makeFourCC('F', 'G', 'c', 'p'));
    registry.registerClass<FGCommandRadio>("FearGui::FGCommandRadio",
                                           makeFourCC('F', 'G', 'c', 'r'));
    registry.registerClass<CommandTurretCtrl>("FearGui::CommandTurretCtrl",
                                              makeFourCC('F', 'G', 't', 'I'));
    registry.registerClass<CommandItemCtrl>("FearGui::CommandItemCtrl",
                                            makeFourCC('F', 'G', 'c', 'I'));
    registry.registerClass<CommandTeamCtrl>("FearGui::CommandTeamCtrl",
                                            makeFourCC('F', 'G', 'c', 'T'));
    registry.registerClass<CMDObserve>("FearGui::CMDObserve", makeFourCC('F', 'G', 'o', 's'));
    registry.registerClass<FGCurrentCommand>("FearGui::FGCurrentCommand",
                                             makeFourCC('F', 'G', 'c', 'z'));
    registry.registerClass<FGHCommandObjectivePage>("FearGui::FGHCommandObjectivePage",
                                                    makeFourCC('F', 'G', 'c', 'o'));

    // Group 5: Shell Controls & Widgets
    registry.registerClass<FGControl>("FearGui::FGControl", makeFourCC('F', 'G', 'k', 't'));
    registry.registerClass<FearGuiBox>("FearGui::FearGuiBox", makeFourCC('F', 'G', 'b', 'x'));
    registry.registerClass<FGShellBorder>("FearGui::FGShellBorder", makeFourCC('F', 'G', 's', 'b'));
    registry.registerClass<MMShellBorder>("FearGui::MMShellBorder", makeFourCC('M', 'M', 's', 'b'));
    registry.registerClass<FGPaletteCtrl>("FearGui::FGPaletteCtrl", makeFourCC('F', 'G', 'p', 'a'));
    registry.registerClass<FGUniversalButton>("FearGui::FGUniversalButton",
                                              makeFourCC('F', 'G', 'u', 'b'));
    registry.registerClass<CFGButton>("FearGui::CFGButton", makeFourCC('F', 'G', 'c', 'f'));
    registry.registerClass<FearGuiRadio>("FearGui::FearGuiRadio", makeFourCC('F', 'G', 'r', 'b'));
    registry.registerClass<FearGuiSwitch>("FearGui::FearGuiSwitch", makeFourCC('F', 'G', 's', 'w'));
    registry.registerClass<FGTab>("FearGui::FGTab", makeFourCC('F', 'G', 't', 'b'));
    registry.registerClass<FGTabMenu>("FearGui::FGTabMenu", makeFourCC('F', 'G', 't', 'm'));
    registry.registerClass<FGSlider>("FearGui::FGSlider", makeFourCC('F', 'G', 's', 'k'));
    registry.registerClass<FearGuiScrollCtrl>("FearGui::FearGuiScrollCtrl",
                                              makeFourCC('F', 'G', 's', 'l'));
    registry.registerClass<FGArrayCtrl>("FearGui::FGArrayCtrl", makeFourCC('F', 'G', 'a', 'c'));
    registry.registerClass<ColumnListCtrl>("FearGui::ColumnListCtrl",
                                           makeFourCC('F', 'G', 'c', 'l'));
    registry.registerClass<FGTextList>("FearGui::FGTextList", makeFourCC('F', 'G', 't', 'l'));
    registry.registerClass<FGSimpleText>("FearGui::FGSimpleText", makeFourCC('F', 'G', 's', 't'));
    registry.registerClass<FearGuiFormattedText>("FearGui::FearGuiFormattedText");
    registry.registerClass<LegacyFormattedText>("FearGuiFormattedText");
    registry.registerClass<FGTextFormat>("FearGui::FGTextFormat", makeFourCC('F', 'G', 't', 'f'));
    registry.registerClass<TestEdit>("FearGui::TestEdit", makeFourCC('F', 'G', 't', 'e'));
    registry.registerClass<FGBitmapCtrl>("FearGui::FGBitmapCtrl", makeFourCC('F', 'G', 'b', 'c'));
    registry.registerClass<ShapeView>("FearGui::ShapeView", makeFourCC('F', 'G', 's', 'v'));
    registry.registerClass<SkinView>("FearGui::SkinView", makeFourCC('F', 's', 'k', 'v'));
    registry.registerClass<SmackerCtrl>("FearGui::SmackerCtrl", makeFourCC('F', 'G', 's', 'm'));
    registry.registerClass<FGPopUpMenu>("FearGui::FGPopUpMenu", makeFourCC('F', 'G', 'p', 'm'));
    registry.registerClass<FGComboBox>("FearGui::FGComboBox", makeFourCC('F', 'G', 'c', 'b'));
    registry.registerClass<FGStandardComboBox>("FearGui::FGStandardComboBox",
                                               makeFourCC('F', 'G', 's', 'j'));
    registry.registerClass<StandardListCtrl>("FearGui::StandardListCtrl",
                                             makeFourCC('F', 'G', 's', 'y'));
    registry.registerClass<FGControlsComboBox>("FearGui::FGControlsComboBox",
                                               makeFourCC('F', 'G', 'q', 'c'));
    registry.registerClass<ControlsListCtrl>("FearGui::ControlsListCtrl",
                                             makeFourCC('F', 'G', 'q', 'l'));
    registry.registerClass<FilterVarCtrl>("FearGui::FilterVarCtrl", makeFourCC('F', 'G', 'f', 'v'));
    registry.registerClass<FGGenderComboBox>("FearGui::FGGenderComboBox",
                                             makeFourCC('F', 'G', 'g', 'd'));
    registry.registerClass<FGPlayerComboBox>("FearGui::FGPlayerComboBox",
                                             makeFourCC('F', 'G', 'p', 'c'));
    registry.registerClass<FGBuddyComboBox>("FearGui::FGBuddyComboBox",
                                            makeFourCC('F', 'G', 'x', 'x'));
    registry.registerClass<FGMissionComboBox>("FearGui::FGMissionComboBox",
                                              makeFourCC('F', 'G', 'm', 'c'));
    registry.registerClass<FGFilterComboBox>("FearGui::FGFilterComboBox",
                                             makeFourCC('F', 'G', 'f', 'c'));
    registry.registerClass<FGGenericComboBox>("FearGui::FGGenericComboBox",
                                              makeFourCC('F', 'G', 'g', 'c'));
    registry.registerClass<FGServerFilterComboBox>("FearGui::FGServerFilterComboBox",
                                                   makeFourCC('F', 'G', 's', 'x'));
    registry.registerClass<FGFilterCBListCtrl>("FearGui::FGFilterCBListCtrl",
                                               makeFourCC('F', 'G', 'f', 'b'));
    registry.registerClass<FilterListCtrl>("FearGui::FilterListCtrl",
                                           makeFourCC('F', 'G', 'f', 'l'));
    registry.registerClass<FilterCtrl>("FearGui::FilterCtrl", makeFourCC('F', 'G', 'f', 't'));
    registry.registerClass<FGHelpCtrl>("FearGui::FGHelpCtrl", makeFourCC('F', 'G', 'h', 'x'));

    // Group 6: Server Browser, IRC & Recordings Controls
    registry.registerClass<ServerListCtrl>("FearGui::ServerListCtrl",
                                           makeFourCC('F', 'G', 's', 'L'));
    registry.registerClass<FGMasterList>("FearGui::FGMasterList", makeFourCC('F', 'G', 'm', 's'));
    registry.registerClass<LobbyListCtrl>("FearGui::LobbyListCtrl", makeFourCC('F', 'G', 'l', 'l'));
    registry.registerClass<ServerInfoCtrl>("FearGui::ServerInfoCtrl",
                                           makeFourCC('F', 'G', 's', 'i'));
    registry.registerClass<ServerDetailCtrl>("FearGui::ServerDetailCtrl",
                                             makeFourCC('F', 'G', 's', 'd'));
    registry.registerClass<PlayerListCtrl>("FearGui::PlayerListCtrl",
                                           makeFourCC('F', 'G', 'p', 'l'));
    registry.registerClass<MissionListCtrl>("FearGui::MissionListCtrl",
                                            makeFourCC('F', 'G', 'm', 'l'));
    registry.registerClass<FGRecordingList>("FearGui::FGRecordingList",
                                            makeFourCC('F', 'G', 'r', 'l'));
    registry.registerClass<FGRecordingComboBox>("FearGui::FGRecordingComboBox");
    registry.registerClass<FGRecordingListCtrl>("FearGui::FGRecordingListCtrl",
                                                makeFourCC('F', 'G', 'r', 'c'),
                                                makeFourCC('F', 'G', 'r', 'L'));
    registry.registerClass<FGIRCView>("FearGui::FGIRCView", makeFourCC('F', 'G', 'i', 'x'));
    registry.registerClass<FGIRCChannelCtrl>("FearGui::FGIRCChannelCtrl",
                                             makeFourCC('F', 'G', 'i', 'h'));
    registry.registerClass<FGIRCChannelList>("FearGui::FGIRCChannelList",
                                             makeFourCC('F', 'i', 'c', 'l'));
    registry.registerClass<FGIRCChannelPropsCtrl>("FearGui::FGIRCChannelPropsCtrl",
                                                  makeFourCC('F', 'i', 'c', 'p'));
    registry.registerClass<FGIRCNicks>("FearGui::FGIRCNicks", makeFourCC('F', 'G', 'i', 'n'));
    registry.registerClass<FGIRCTopic>("FearGui::FGIRCTopic", makeFourCC('F', 'G', 't', 'c'));
    registry.registerClass<FGIRCTextEdit>("FearGui::FGIRCTextEdit", makeFourCC('F', 'G', 'i', 't'));
    registry.registerClass<FGIRCActiveTextEdit>("FearGui::FGIRCActiveTextEdit",
                                                makeFourCC('F', 'i', 'a', 'e'));
    registry.registerClass<FGIRCActiveTextFormat>("FearGui::FGIRCActiveTextFormat",
                                                  makeFourCC('F', 'i', 'a', 'f'));
    registry.registerClass<FGIRCScrollChannel>("FearGui::FGIRCScrollChannel",
                                               makeFourCC('F', 'G', 'i', 'k'));
    registry.registerClass<FGIRCServersComboBox>("FearGui::FGIRCServersComboBox",
                                                 makeFourCC('F', 'G', 'i', 'b'));
    registry.registerClass<FGIRCBanCtrl>("FearGui::FGIRCBanCtrl", makeFourCC('F', 'i', 'b', 'c'));
    registry.registerClass<FGIRCHelpCtrl>("FearGui::FGIRCHelpCtrl", makeFourCC('F', 'i', 'h', 'c'));
    registry.registerClass<FGIRCJoinCtrl>("FearGui::FGIRCJoinCtrl", makeFourCC('F', 'i', 'j', 'c'));

    // Group 7: Shell Dialogs & Delegates
    registry.registerClass<FearGuiDialog>("FearGui::FearGuiDialog", makeFourCC('F', 'G', 'd', 'g'));
    registry.registerClass<FGDlgBox>(
        "FearGui::FGDlgBox", makeFourCC('F', 'G', 'd', 'b'), makeFourCC('F', 'G', 'd', 'x'));
    registry.registerClass<FearGuiAbortDlg>("FearGui::FearGuiAbortDlg",
                                            makeFourCC('F', 'G', 'a', 'd'));
    registry.registerClass<MainMenuDelegate>("MainMenuDelegate");
    registry.registerClass<PlayDelegate>("PlayDelegate");
    registry.registerClass<ConnectDelegate>("ConnectDelegate");
    registry.registerClass<JoinGameDelegate>("JoinGameDelegate");
    registry.registerClass<CreateServerDelegate>("CreateServerDelegate");
    registry.registerClass<MultiSetupDelegate>("MultiSetupDelegate");
    registry.registerClass<PlayerSetupDelegate>("PlayerSetupDelegate");
    registry.registerClass<MissionSelectDelegate>("MissionSelectDelegate");
    registry.registerClass<OptionsDelegate>("OptionsDelegate");
    registry.registerClass<RecordingsDelegate>("RecordingsDelegate");
    registry.registerClass<IRCLoginDelegate>("IRCLoginDelegate");
    registry.registerClass<IRCChatDelegate>("IRCChatDelegate");
    registry.registerClass<QuitDelegate>("QuitDelegate");
}

} // namespace opent1::game
