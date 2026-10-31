#pragma once

#include <opent1/game/chat_menu.hpp>
#include <opent1/game/data_block_manager.hpp>
#include <opent1/game/fear_player_psc.hpp>
#include <opent1/game/hud_controls.hpp>
#include <opent1/game/player_manager.hpp>

#include <array>
#include <deque>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace opent1::game {

// -----------------------------------------------------------------------------
// FearGuiChat (FearGuiChat.cpp)
// -----------------------------------------------------------------------------
class FearGuiChat : public gui::TextEdit {
    using Parent = gui::TextEdit;

public:
    static constexpr std::size_t HistoryLimit = 20;
    static constexpr Int32 DisplayBorderX = 12;
    static constexpr Int32 DisplayBorderY = 6;

    FearGuiChat();
    ~FearGuiChat() override = default;

    [[nodiscard]] Point2F fracPos() const noexcept {
        return fracPos_;
    }

    void setFracPos(Point2F fp) noexcept {
        fracPos_ = fp;
    }

    [[nodiscard]] Int32 lineCount() const noexcept {
        return lineCount_;
    }

    void setLineCount(Int32 count) noexcept {
        lineCount_ = count;
    }

    [[nodiscard]] Int32 channel() const noexcept {
        return channel_;
    }

    void setChannel(Int32 ch) noexcept {
        channel_ = ch;
    }

    void pushHistory(std::string_view line);
    [[nodiscard]] std::string_view historyPrev();
    [[nodiscard]] std::string_view historyNext();

    [[nodiscard]] std::span<const std::string> history() const noexcept {
        return history_;
    }

    [[nodiscard]] std::string submitLine(std::string_view rawInput);

    void activate(Int32 chatChannel = 0);
    void deactivate();
    bool event(const gui::Event&) override;
    std::function<void(Int32, std::string_view)> sendLine;
    std::shared_ptr<const gfx::BitmapFont> teamFont, smallFont, smallTeamFont;
    bool draw(gfx::SoftwareSurface&, Point2I, RectI) const override;
    bool layout(Point2I extent);
    void wake() override;
    std::function<void()> checkpoint() override;

    Persistent::Error
    read(StreamIO& sio, Int32 version, Int32 user, Persistent::LoadContext& ctx) override;
    Persistent::Error
    write(StreamIO& sio, Int32 version, Int32 user, Persistent::StoreContext& ctx) const override;

private:
    Point2F fracPos_{0.0f, 0.0f};
    Int32 lineCount_{2};
    Int32 channel_{0};
    std::vector<std::string> history_{};
    Int32 historyCursor_{-1};
};

// -----------------------------------------------------------------------------
// FearGuiChatDisplay (FearGuiChatDisplay.cpp)
// -----------------------------------------------------------------------------
class FearGuiChatDisplay : public HudCtrl {
    using Parent = HudCtrl;

public:
    static constexpr std::size_t MessageHistoryLimit = 200;

    enum MessageType : Int32 {
        MSG_System = 0,
        MSG_Game = 1,
        MSG_Chat = 2,
        MSG_TeamChat = 3,
        MSG_Command = 4,
        MSG_IRC = 5,
    };

    struct MessageEntry {
        Int32 type{MSG_System};
        std::string text{};
        std::string soundName{};
    };

    FearGuiChatDisplay();
    ~FearGuiChatDisplay() override = default;
    std::array<std::shared_ptr<const gfx::BitmapFont>, 4> messageFonts, smallMessageFonts;
    bool layout(Point2I extent);

    void clear() noexcept;
    MessageEntry newMessage(std::string_view rawMsg, Int32 msgType = MSG_Chat);
    void setCommandMessage(std::string_view rawMsg);

    [[nodiscard]] const std::deque<MessageEntry>& messages() const noexcept {
        return messages_;
    }

    [[nodiscard]] const MessageEntry& commandMessage() const noexcept {
        return commandSlot_;
    }

    [[nodiscard]] Int32 lineCount() const noexcept {
        return lineCount_;
    }

    void setSize(Int32 value = -1, Int32 fontHeight = 12);
    void page(Int32 delta);

    [[nodiscard]] Int32 pageUpCount() const noexcept {
        return pgUpCount_;
    }

    [[nodiscard]] bool isCommanderChat() const noexcept {
        return commanderChat_;
    }

    void setCommanderChat(bool cmd) noexcept {
        commanderChat_ = cmd;
    }

    bool pointInControl(Point2I p) const override;
    bool draw(gfx::SoftwareSurface&, Point2I, RectI) const override;
    bool onSimActionEvent(const sim::SimActionEvent& event);
    bool processEvent(const sim::SimEvent& event) override;
    std::function<void()> checkpoint() override;

    bool inspectRead(sim::Inspect& inspector) override;
    bool inspectWrite(sim::Inspect& inspector) const override;

    Persistent::Error
    read(StreamIO& sio, Int32 version, Int32 user, Persistent::LoadContext& ctx) override;
    Persistent::Error
    write(StreamIO& sio, Int32 version, Int32 user, Persistent::StoreContext& ctx) const override;

private:
    std::deque<MessageEntry> messages_{};
    MessageEntry commandSlot_{};
    Int32 lineCount_{3};
    Int32 pgUpCount_{0};
    bool commanderChat_{false};
};

// -----------------------------------------------------------------------------
// FGChatMenu (FearGuiChatMenu.cpp)
// -----------------------------------------------------------------------------
class FGChatMenu : public gui::Control {
    using Parent = gui::Control;

public:
    FGChatMenu() = default;
    ~FGChatMenu() override = default;

    [[nodiscard]] const std::string& menuName() const noexcept {
        return menuName_;
    }

    void setMenuName(std::string_view name);

    [[nodiscard]] std::vector<std::string> buildVisibleRows(const ChatMenu& menu) const;
    std::shared_ptr<const gfx::BitmapFont> font;
    std::function<const ChatMenu*()> resolveMenu;
    bool draw(gfx::SoftwareSurface&, Point2I, RectI) const override;
    std::function<void()> checkpoint() override;

    bool inspectRead(sim::Inspect& inspector) override;
    bool inspectWrite(sim::Inspect& inspector) const override;

    Persistent::Error
    read(StreamIO& sio, Int32 version, Int32 user, Persistent::LoadContext& ctx) override;
    Persistent::Error
    write(StreamIO& sio, Int32 version, Int32 user, Persistent::StoreContext& ctx) const override;

private:
    std::string menuName_{};
};

// -----------------------------------------------------------------------------
// FearGuiMenu (FearGuiMenu.cpp)
// -----------------------------------------------------------------------------
class FearGuiMenu : public gui::Control {
    using Parent = gui::Control;

public:
    FearGuiMenu();
    ~FearGuiMenu() override = default;

    [[nodiscard]] Point2I fixedSize() const noexcept {
        return fixedSize_;
    }

    void setFixedSize(Point2I sz) noexcept {
        fixedSize_ = sz;
    }

    bool draw(gfx::SoftwareSurface&, Point2I, RectI) const override;

    bool onSimActionEvent(const sim::SimActionEvent& event);
    bool processEvent(const sim::SimEvent& event) override;
    std::function<void()> checkpoint() override;

    bool inspectRead(sim::Inspect& inspector) override;
    bool inspectWrite(sim::Inspect& inspector) const override;

    Persistent::Error
    read(StreamIO& sio, Int32 version, Int32 user, Persistent::LoadContext& ctx) override;
    Persistent::Error
    write(StreamIO& sio, Int32 version, Int32 user, Persistent::StoreContext& ctx) const override;

private:
    Point2I fixedSize_{1, 1};
};

// -----------------------------------------------------------------------------
// FGMenuCtrl & FGMenuItem (FearGuiMenuCtrl.cpp)
// -----------------------------------------------------------------------------
class FGMenuCtrl : public gui::Active {
    using Parent = gui::Active;

public:
    FGMenuCtrl() = default;
    ~FGMenuCtrl() override = default;

    void disableAll();
    void wake() override;
    void layout();
};

class FGMenuItem : public gui::Text {
    using Parent = gui::Text;

public:
    FGMenuItem() = default;
    ~FGMenuItem() override = default;

    [[nodiscard]] Int32 pageTag() const noexcept {
        return pageTag_;
    }

    void setPageTag(Int32 tag) noexcept {
        pageTag_ = tag;
    }

    [[nodiscard]] bool isSet() const noexcept {
        return set_;
    }

    void setState(bool turnOn);

    [[nodiscard]] bool isExpanded() const noexcept {
        return expanded_;
    }

    void setExpanded(bool val) noexcept {
        expanded_ = val;
    }

    void wake() override;
    bool draw(gfx::SoftwareSurface&, Point2I, RectI) const override;
    std::function<void(std::string_view)> playSound;
    void action() override;
    bool message(gui::Control& sender, UInt32 msg) override;
    std::function<void()> checkpoint() override;

    bool inspectRead(sim::Inspect& inspector) override;
    bool inspectWrite(sim::Inspect& inspector) const override;

    Persistent::Error
    read(StreamIO& sio, Int32 version, Int32 user, Persistent::LoadContext& ctx) override;
    Persistent::Error
    write(StreamIO& sio, Int32 version, Int32 user, Persistent::StoreContext& ctx) const override;

private:
    Int32 pageTag_{0};
    bool set_{false};
    bool expanded_{false};
};

// -----------------------------------------------------------------------------
// MenuDisplayCtrl (FearGuiMenuDisplay.cpp)
// -----------------------------------------------------------------------------
class MenuDisplayCtrl : public gui::Active {
    using Parent = gui::Active;

public:
    MenuDisplayCtrl() = default;
    ~MenuDisplayCtrl() override = default;
    std::string heading;
    std::vector<std::string> entries;
    std::shared_ptr<const gfx::BitmapFont> font, highlightFont;
    std::function<void(std::size_t)> selectEntry;
    bool event(const gui::Event&) override;
    bool draw(gfx::SoftwareSurface&, Point2I, RectI) const override;
};

// -----------------------------------------------------------------------------
// FearGuiInventory & FearGuiPurchase (fearGuiInventory.cpp)
// -----------------------------------------------------------------------------
struct InventoryDisplayRow {
    Int32 itemIndex{-1}; // -1 indicates a category header row
    std::string heading{};
    std::string description{};
    Int32 count{0};
    Int32 price{0};
    bool favorite{false};
    bool enabled{true};
};

class FearGuiInventory : public FGHudArrayCtrl {
    using Parent = FGHudArrayCtrl;

public:
    FearGuiInventory();
    ~FearGuiInventory() override = default;
    bool select(Point2I cell) override;
    bool event(const gui::Event&) override;
    std::function<void(Int32, bool)> itemSelected;
    std::shared_ptr<const gfx::BitmapFont> disabledFont;
    std::shared_ptr<const gfx::Bitmap> favoriteBitmap;
    bool drawCell(gfx::SoftwareSurface&, Point2I, Point2I, bool, bool) const override;

    void populateFromPSC(const PlayerPSC* psc,
                         const DataBlockManager* dbm,
                         std::span<const Int32> favoriteItemIds = {});

    [[nodiscard]] std::span<const InventoryDisplayRow> rows() const noexcept {
        return rows_;
    }

    [[nodiscard]] Int32 selectedItemIndex() const noexcept;

    std::string getScriptValue() const override {
        return std::to_string(selectedItemIndex());
    }

    std::string getCellText(Point2I cell) const override;
    [[nodiscard]] std::string getVisibleSetString() const;

protected:
    void restoreSelection(Int32 item);
    std::vector<InventoryDisplayRow> rows_{};
};

class FearGuiPurchase : public FearGuiInventory {
    using Parent = FearGuiInventory;

public:
    FearGuiPurchase();
    ~FearGuiPurchase() override = default;
    void populateFromShoppingList(const PlayerPSC* psc,
                                  const DataBlockManager* dbm,
                                  std::span<const Int32> favoriteItemIds = {});
    using Parent::getVisibleSetString;
    [[nodiscard]] static std::string getVisibleSetString(const PlayerPSC* psc,
                                                         const DataBlockManager* dbm);
    static std::vector<Int32> parseFavoritesList(std::span<const std::string> args,
                                                 const DataBlockManager* dbm);
    std::string getCellText(Point2I cell) const override;
};

// -----------------------------------------------------------------------------
// InventoryListCtrl (FearGuiInvList.cpp)
// -----------------------------------------------------------------------------
class InventoryListCtrl : public FGHudArrayCtrl {
    using Parent = FGHudArrayCtrl;

public:
    InventoryListCtrl();
    ~InventoryListCtrl() override = default;

    void syncFromPSC(const PlayerPSC* psc, const DataBlockManager* dbm);
    std::string getCellText(Point2I cell) const override;

    [[nodiscard]] std::span<const InventoryDisplayRow> rows() const noexcept {
        return rows_;
    }

private:
    std::vector<InventoryDisplayRow> rows_{};
};

// -----------------------------------------------------------------------------
// FGHBuySell (FearHudBuySell.cpp), FGHInventory (FearHudInvList.cpp), FHInventory (FHinventory.cpp)
// -----------------------------------------------------------------------------
class FGHBuySell : public FGHudArrayCtrl {
    using Parent = FGHudArrayCtrl;

public:
    FGHBuySell();
    ~FGHBuySell() override = default;

    void populateShoppingItems(const PlayerPSC* psc, const DataBlockManager* dbm);

    [[nodiscard]] std::span<const InventoryDisplayRow> rows() const noexcept {
        return rows_;
    }

private:
    std::vector<InventoryDisplayRow> rows_{};
};

class FGHInventory : public FGHudArrayCtrl {
    using Parent = FGHudArrayCtrl;

public:
    FGHInventory();
    ~FGHInventory() override = default;

    void populateOwnedItems(const PlayerPSC* psc, const DataBlockManager* dbm);

    [[nodiscard]] std::span<const InventoryDisplayRow> rows() const noexcept {
        return rows_;
    }

private:
    std::vector<InventoryDisplayRow> rows_{};
};

class FHInventory : public FGHudArrayCtrl {
    using Parent = FGHudArrayCtrl;

public:
    FHInventory();
    ~FHInventory() override = default;
};

// -----------------------------------------------------------------------------
// ScoreListCtrl (FearGuiScoreList.cpp)
// -----------------------------------------------------------------------------
class ScoreListCtrl : public FGHudArrayCtrl {
    using Parent = FGHudArrayCtrl;

public:
    static constexpr std::size_t MaxTabStops = 16;

    struct FormattedScoreLine {
        bool isTeamSection{false};
        bool isHeader{false};
        Int32 id{0};
        Int32 sortValue{0};
        std::string text{};
    };

    ScoreListCtrl();
    ~ScoreListCtrl() override = default;

    [[nodiscard]] static std::vector<Int32> parseHeadingTabStops(std::string_view heading);
    [[nodiscard]] static std::string expandClientScoreFormat(std::string_view formatStr,
                                                             const PlayerManager::ClientRep& client,
                                                             std::string_view teamName);
    [[nodiscard]] static std::string expandTeamScoreFormat(std::string_view formatStr,
                                                           const PlayerManager::TeamRep& team);

    void rebuildScores(const PlayerManager& pm);
    bool drawCell(gfx::SoftwareSurface&, Point2I, Point2I, bool, bool) const override;

    [[nodiscard]] std::span<const Int32> teamTabStops() const noexcept {
        return teamTabStops_;
    }

    [[nodiscard]] std::span<const Int32> clientTabStops() const noexcept {
        return clientTabStops_;
    }

    [[nodiscard]] std::span<const FormattedScoreLine> lines() const noexcept {
        return lines_;
    }

    std::string getCellText(Point2I cell) const override;

private:
    std::vector<Int32> teamTabStops_{};
    std::vector<Int32> clientTabStops_{};
    std::vector<FormattedScoreLine> lines_{};
};

// -----------------------------------------------------------------------------
// FearGuiPage (FearGuiPage.cpp)
// -----------------------------------------------------------------------------
class FearGuiPage : public gui::Control {
    using Parent = gui::Control;

public:
    FearGuiPage();
    ~FearGuiPage() override = default;

    bool onAdd() override;
    bool message(gui::Control& sender, UInt32 msg) override;

    Persistent::Error
    read(StreamIO& sio, Int32 version, Int32 user, Persistent::LoadContext& ctx) override;
    Persistent::Error
    write(StreamIO& sio, Int32 version, Int32 user, Persistent::StoreContext& ctx) const override;
};

} // namespace opent1::game
