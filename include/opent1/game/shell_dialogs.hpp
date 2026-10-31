#pragma once

#include "opent1/game/chat_inventory_huds.hpp"
#include "opent1/game/commander_huds.hpp"
#include "opent1/game/hud_controls.hpp"
#include "opent1/game/server_irc_controls.hpp"
#include "opent1/game/shell_controls.hpp"
#include "opent1/game/status_combat_huds.hpp"
#include <opent1/gui/runtime.hpp>

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

namespace opent1::game {

// -----------------------------------------------------------------------------
// FearGuiDialog (FearGuiDialog.cpp)
// -----------------------------------------------------------------------------
class FearGuiDialog : public gui::Control {
    using Parent = gui::Control;

public:
    FearGuiDialog() = default;
    ~FearGuiDialog() override = default;

    [[nodiscard]] Point2I fixedSize() const noexcept {
        return fixedSize_;
    }

    void setFixedSize(Point2I size) noexcept {
        fixedSize_ = size;
    }

    void parentResized(const Point2I& oldParentExtent, const Point2I& newParentExtent);

    std::function<void()> checkpoint() override;
    bool inspectRead(sim::Inspect& inspector) override;
    bool inspectWrite(sim::Inspect& inspector) const override;

    Persistent::Error
    read(StreamIO& sio, Int32 version, Int32 user, Persistent::LoadContext& ctx) override;
    Persistent::Error
    write(StreamIO& sio, Int32 version, Int32 user, Persistent::StoreContext& ctx) const override;

protected:
    Point2I fixedSize_{240, 120};
};

// -----------------------------------------------------------------------------
// FGDlgBox (FearGuiDlgBox.cpp)
// -----------------------------------------------------------------------------
class FGDlgBox : public gui::Active {
    using Parent = gui::Active;

public:
    static constexpr Int32 DlgBoxVersion = 0;

    FGDlgBox();
    ~FGDlgBox() override = default;

    void setBitmaps(std::array<std::shared_ptr<const gfx::Bitmap>, 5> bitmaps) {
        bitmaps_ = std::move(bitmaps);
    }

    bool draw(gfx::SoftwareSurface&, Point2I, RectI) const override;

    std::function<void()> checkpoint() override {
        auto base = Parent::checkpoint();
        const auto bitmaps = bitmaps_;
        return [this, base = std::move(base), bitmaps] {
            base();
            bitmaps_ = bitmaps;
        };
    }

    [[nodiscard]] bool isModalNamedDialog() const noexcept;
    bool handleReturnKey(gui::Control* rootSearch = nullptr);
    bool handleEscapeKey(gui::Control* rootSearch = nullptr);

    Persistent::Error
    read(StreamIO& sio, Int32 version, Int32 user, Persistent::LoadContext& ctx) override;
    Persistent::Error
    write(StreamIO& sio, Int32 version, Int32 user, Persistent::StoreContext& ctx) const override;

private:
    std::array<std::shared_ptr<const gfx::Bitmap>, 5> bitmaps_;
};

// -----------------------------------------------------------------------------
// FearGuiAbortDlg (FearGuiAbort.cpp)
// -----------------------------------------------------------------------------
class FearGuiAbortDlg : public FearGuiDialog {
    using Parent = FearGuiDialog;

public:
    FearGuiAbortDlg() = default;
    ~FearGuiAbortDlg() override = default;

    [[nodiscard]] Int32 fontNameTag() const noexcept {
        return fontNameTag_;
    }

    void setFontNameTag(Int32 tag) noexcept {
        fontNameTag_ = tag;
    }

    [[nodiscard]] Int32 textTag() const noexcept {
        return textTag_;
    }

    void setTextTag(Int32 tag) noexcept {
        textTag_ = tag;
    }

    std::function<void()> checkpoint() override;
    bool inspectRead(sim::Inspect& inspector) override;
    bool inspectWrite(sim::Inspect& inspector) const override;

    Persistent::Error
    read(StreamIO& sio, Int32 version, Int32 user, Persistent::LoadContext& ctx) override;
    Persistent::Error
    write(StreamIO& sio, Int32 version, Int32 user, Persistent::StoreContext& ctx) const override;

private:
    Int32 fontNameTag_{0};
    Int32 textTag_{0};
};

// -----------------------------------------------------------------------------
// Shell Dialog Delegates (dlgMainMenu.cpp .. dlgQuit.cpp)
// -----------------------------------------------------------------------------
enum ShellMessageTag : UInt32 {
    IDDLG_QuickStart = 7001,
    IDDLG_MultiPlayer = 7002,
    IDDLG_SinglePlayer = 7003,
    IDDLG_Options = 7004,
    IDDLG_Recordings = 7005,
    IDDLG_Quit = 7006,
    IDDLG_MainMenu = 7007,
    IDDLG_HostGame = 7008,
    IDDLG_JoinGame = 7009,
    IDDLG_PlayerSetup = 7010,
    IDDLG_Connect = 7011,
    IDDLG_MissionSelect = 7012,
    IDDLG_IRCLogin = 7013,
    IDDLG_IRCChat = 7014,
    IDDLG_Cancel = 7015,
    IDDLG_StartGame = 7016,
    IDDLG_PlayDemo = 7017,
};

class ShellDialogDelegate : public gui::Delegate, public sim::SimObject {
    using Parent = gui::Delegate;

public:
    ShellDialogDelegate() = default;
    ~ShellDialogDelegate() override = default;

    virtual UInt32 onMessage(sim::SimObject* sender, UInt32 msg) {
        (void)sender;
        (void)msg;
        return 0;
    }

    bool message(gui::Control& sender, UInt32 msg) override {
        return onMessage(&sender, msg) != 0;
    }

    [[nodiscard]] const std::string& activeScreen() const noexcept {
        return activeScreen_;
    }

    [[nodiscard]] const std::string& lastScriptAction() const noexcept {
        return lastScriptAction_;
    }

    void setActiveScreen(std::string_view screen) {
        activeScreen_ = std::string(screen);
    }

protected:
    std::string activeScreen_{};
    std::string lastScriptAction_{};
};

class MainMenuDelegate : public ShellDialogDelegate {
public:
    MainMenuDelegate();
    UInt32 onMessage(sim::SimObject* sender, UInt32 msg) override;
};

class PlayDelegate : public ShellDialogDelegate {
public:
    PlayDelegate();
    UInt32 onMessage(sim::SimObject* sender, UInt32 msg) override;
};

class ConnectDelegate : public ShellDialogDelegate {
public:
    ConnectDelegate();
    UInt32 onMessage(sim::SimObject* sender, UInt32 msg) override;

    [[nodiscard]] const std::string& serverAddress() const noexcept {
        return serverAddress_;
    }

    void setServerAddress(std::string_view addr) {
        serverAddress_ = std::string(addr);
    }

private:
    std::string serverAddress_{"IP:127.0.0.1:28001"};
};

class JoinGameDelegate : public ShellDialogDelegate {
public:
    JoinGameDelegate();
    UInt32 onMessage(sim::SimObject* sender, UInt32 msg) override;
};

class CreateServerDelegate : public ShellDialogDelegate {
public:
    CreateServerDelegate();
    UInt32 onMessage(sim::SimObject* sender, UInt32 msg) override;
};

class MultiSetupDelegate : public ShellDialogDelegate {
public:
    MultiSetupDelegate();
    UInt32 onMessage(sim::SimObject* sender, UInt32 msg) override;
};

class PlayerSetupDelegate : public ShellDialogDelegate {
public:
    PlayerSetupDelegate();
    UInt32 onMessage(sim::SimObject* sender, UInt32 msg) override;
};

class MissionSelectDelegate : public ShellDialogDelegate {
public:
    MissionSelectDelegate();
    UInt32 onMessage(sim::SimObject* sender, UInt32 msg) override;
};

class OptionsDelegate : public ShellDialogDelegate {
public:
    OptionsDelegate();
    UInt32 onMessage(sim::SimObject* sender, UInt32 msg) override;
};

class RecordingsDelegate : public ShellDialogDelegate {
public:
    RecordingsDelegate();
    UInt32 onMessage(sim::SimObject* sender, UInt32 msg) override;
};

class IRCLoginDelegate : public ShellDialogDelegate {
public:
    IRCLoginDelegate();
    UInt32 onMessage(sim::SimObject* sender, UInt32 msg) override;
};

class IRCChatDelegate : public ShellDialogDelegate {
public:
    IRCChatDelegate();
    UInt32 onMessage(sim::SimObject* sender, UInt32 msg) override;
};

class QuitDelegate : public ShellDialogDelegate {
public:
    QuitDelegate();
    UInt32 onMessage(sim::SimObject* sender, UInt32 msg) override;

    [[nodiscard]] bool quitRequested() const noexcept {
        return quitRequested_;
    }

private:
    bool quitRequested_{false};
};

// Registers all Layer 7 L7-B06 Tribes HUD & Shell GUI persistent classes
void registerFearGuiClasses(Persistent::ClassRegistry& registry);

} // namespace opent1::game
