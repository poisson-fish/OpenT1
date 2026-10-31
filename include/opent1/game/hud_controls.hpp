#pragma once
#include <map>

#include <opent1/core/persist.hpp>
#include <opent1/core/res_manager.hpp>
#include <opent1/core/types.hpp>
#include <opent1/game/f_base.hpp>
#include <opent1/game/string_table.hpp>
#include <opent1/gfx/bitmap_palette_font.hpp>
#include <opent1/gfx/software_rasterizer.hpp>
#include <opent1/gui/containers.hpp>
#include <opent1/gui/controls.hpp>
#include <opent1/gui/gui.hpp>
#include <opent1/sim/sim_input_action.hpp>
#include <opent1/sim/sim_inspector.hpp>

#include <array>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace opent1::game {

using gfx::RectI;

// -----------------------------------------------------------------------------
// Shell & HUD Palette Constants (FearGuiShellPal.cpp / FearHudPalTable.cpp)
// -----------------------------------------------------------------------------
enum ShellPaletteColor : UInt8 {
    LIGHT_BLUE = 231,
    BOX_GHOST_FRAME = 233,
    BOX_GHOST_LAST_PIX = 234,
    BOX_GHOST_OUTSIDE = 235,
    BOX_GHOST_INSIDE = 236,
    BOX_FRAME = 237,
    BOX_LAST_PIX = 238,
    BOX_OUTSIDE = 239,
    BOX_INSIDE = 240,
    HILITE_COLOR = 240,
    GREEN_78 = 241,
    YELLOW = 242,
    RED = 243,
    BLACK = 244,
    MEDIUM_GREY = 245,
    STANDARD_COLOR = 247,
    GREEN_40 = 247,
    SELECT_COLOR = 248,
    GREEN_132 = 248,
    BLUE_OUTLINE = 250,
    BLUE_HILITE = 251,
    GREEN = 252,
    BLUE = 253,
    WHITE = 255,
};

inline constexpr Int32 ALPHAPAL_INDEX = 1976;
inline constexpr UInt32 IDSYS_SIBLING_DISABLE = 100001U;

void InitializePaletteTable(gfx::Palette* palette) noexcept;

class FGHPaletteTable : public gui::Control {
    using Parent = gui::Control;

public:
    enum ColorSlot : std::size_t {
        _BLACK = 0,
        _RED,
        _GREEN_40,
        _GREEN_78,
        _GREEN_132,
        _GREEN,
        _BLUE,
        _YELLOW,
        _BLUE_OUTLINE,
        _BLUE_HILITE,
        _WHITE,
        _COLOR_COUNT
    };

    FGHPaletteTable();
    ~FGHPaletteTable() override = default;

    void loadTable(const gfx::Palette* palette = nullptr) noexcept;
    [[nodiscard]] UInt8 color(ColorSlot slot) const noexcept;

    [[nodiscard]] std::span<const UInt8, _COLOR_COUNT> colors() const noexcept {
        return table_;
    }

    Persistent::Error
    read(StreamIO& sio, Int32 version, Int32 user, Persistent::LoadContext& ctx) override;
    Persistent::Error
    write(StreamIO& sio, Int32 version, Int32 user, Persistent::StoreContext& ctx) const override;

private:
    std::array<UInt8, _COLOR_COUNT> table_{};
};

// -----------------------------------------------------------------------------
//HudCtrl (fearGuiHudCtrl.cpp)
// -----------------------------------------------------------------------------
class HudCtrl : public gui::Control {
    using Parent = gui::Control;

public:
    // Runtime assets are retained separately from the serialized HUD layout.
    std::map<std::string, std::shared_ptr<const gfx::Bitmap>, std::less<>> sprites;
    std::shared_ptr<const gfx::BitmapFont> hudFont, activeFont;

    [[nodiscard]] const gfx::Bitmap* sprite(std::string_view name) const {
        const auto found = sprites.find(name);
        return found == sprites.end() ? nullptr : found->second.get();
    }

    bool drawSprite(gfx::SoftwareSurface& surface,
                    std::string_view name,
                    Point2I at,
                    gfx::Flip flip = gfx::Flip::None) const {
        const auto* bitmap = sprite(name);
        return !bitmap || surface.blit(*bitmap, at, flip);
    }

    static constexpr Int32 SnapDistance = 8;

    HudCtrl();
    ~HudCtrl() override = default;

    [[nodiscard]] Point2F fracPos() const noexcept {
        return fracPos_;
    }

    void setFracPos(Point2F pos) noexcept {
        fracPos_ = pos;
    }

    [[nodiscard]] bool isActive() const noexcept {
        return visible();
    }

    void activate();
    void deactivate();
    void toggleActive();

    [[nodiscard]] bool isDragging() const noexcept {
        return stateDragging_;
    }

    [[nodiscard]] bool isLowRes() const noexcept {
        return lowResMode_;
    }

    void setLowRes(bool lowRes) noexcept {
        lowResMode_ = lowRes;
    }

    void snap();
    void startDrag(Point2I mouseGlobal);
    void dragTo(Point2I mouseGlobal);
    void endDrag();

    bool onAdd() override;
    void wake() override;
    bool parentResized(Point2I oldParentExtent, Point2I newParentExtent);
    void layoutInParent(Point2I, Point2I, Point2I&, Point2I&) const override;
    bool event(const gui::Event& e) override;
    bool processEvent(const sim::SimEvent& event) override;
    bool onSimActionEvent(const sim::SimActionEvent& event);
    bool draw(gfx::SoftwareSurface& s, Point2I offset, RectI clip) const override;
    std::function<void()> checkpoint() override;

    Persistent::Error
    read(StreamIO& sio, Int32 version, Int32 user, Persistent::LoadContext& ctx) override;
    Persistent::Error
    write(StreamIO& sio, Int32 version, Int32 user, Persistent::StoreContext& ctx) const override;

protected:
    Point2F fracPos_{0.0f, 0.0f};
    Point2I hitPt_{0, 0};
    bool stateDragging_{false};
    bool lowResMode_{false};
};

// -----------------------------------------------------------------------------
// HudManager (HudManager.cpp)
// -----------------------------------------------------------------------------
class HudManager : public HudCtrl {
    using Parent = HudCtrl;

public:
    static constexpr Int32 HUD_SPACING = 4;
    static constexpr Int32 HUDS_PER_ROW = 4;

    struct HudEntry {
        HudCtrl* hud{nullptr};
        Int32 onBitmapTag{0};
        Int32 offBitmapTag{0};
        Point2I bmpExtent{24, 24};
        std::shared_ptr<const gfx::Bitmap> onBitmap{};
        std::shared_ptr<const gfx::Bitmap> offBitmap{};
        bool pressed{false};
    };

    HudManager();
    ~HudManager() override = default;

    void
    addHud(HudCtrl* hud, Int32 onBmpTag = 0, Int32 offBmpTag = 0, Point2I bmpExtent = {24, 24});
    void removeHud(const HudCtrl* hud);
    void clearHuds() noexcept;

    [[nodiscard]] std::span<const HudEntry> entries() const noexcept {
        return entries_;
    }

    [[nodiscard]] bool isMinimized() const noexcept {
        return minimize_;
    }

    void setMinimized(bool min);

    [[nodiscard]] Point2I maxExtent() const noexcept {
        return maxExtent_;
    }

    [[nodiscard]] Int32 hudWidth() const noexcept {
        return hudWidth_;
    }

    [[nodiscard]] Int32 hudHeight() const noexcept {
        return hudHeight_;
    }

    bool clickIndex(std::size_t index);
    bool event(const gui::Event& e) override;
    bool draw(gfx::SoftwareSurface& s, Point2I offset, RectI clip) const override;

private:
    void recomputeLayout();

    std::vector<HudEntry> entries_{};
    Point2I maxExtent_{24, 24};
    Int32 hudWidth_{28};
    Int32 hudHeight_{28};
    bool minimize_{false};
};

// -----------------------------------------------------------------------------
// FGArrayCtrl (FearGuiArrayCtrl.cpp)
// -----------------------------------------------------------------------------
class FGArrayCtrl : public gui::Array {
    using Parent = gui::Array;

public:
    struct ColumnInfo {
        Int32 headerTag{0};
        Int32 minWidth{10};
        Int32 maxWidth{640};
        Int32 origPosition{0};
        Int32 position{0};
        bool active{true};
        Int32 width{100};
        std::string title{};
    };

    FGArrayCtrl();
    ~FGArrayCtrl() override = default;
    std::shared_ptr<const gfx::BitmapFont> shellFont, shellHighlightFont;
    void wake() override;
    std::function<void()> checkpoint() override;

    void setColumnsSortable(bool sortable) noexcept {
        columnsSortable_ = sortable;
    }

    [[nodiscard]] bool columnsSortable() const noexcept {
        return columnsSortable_;
    }

    void setColumnsDualSortable(bool dual) noexcept {
        columnsDualSortable_ = dual;
    }

    [[nodiscard]] bool columnsDualSortable() const noexcept {
        return columnsDualSortable_;
    }

    void setColumnsResizeable(bool resizeable) noexcept {
        columnsResizeable_ = resizeable;
    }

    [[nodiscard]] bool columnsResizeable() const noexcept {
        return columnsResizeable_;
    }

    [[nodiscard]] Int32 columnToSort() const noexcept {
        return columnToSort_;
    }

    void setColumnToSort(Int32 col) noexcept {
        columnToSort_ = col;
        refresh_ = true;
    }

    [[nodiscard]] bool sortAscending() const noexcept {
        return sortAscending_;
    }

    void setSortAscending(bool asc) noexcept {
        sortAscending_ = asc;
        refresh_ = true;
    }

    [[nodiscard]] Int32 columnToSortSecond() const noexcept {
        return columnToSortSecond_;
    }

    void setColumnToSortSecond(Int32 col) noexcept {
        columnToSortSecond_ = col;
        refresh_ = true;
    }

    [[nodiscard]] bool sortAscendingSecond() const noexcept {
        return sortAscendingSecond_;
    }

    void setSortAscendingSecond(bool asc) noexcept {
        sortAscendingSecond_ = asc;
        refresh_ = true;
    }

    [[nodiscard]] bool needsRefresh() const noexcept {
        return refresh_;
    }

    void setNeedsRefresh(bool ref) noexcept {
        refresh_ = ref;
    }

    void setColumns(std::vector<ColumnInfo> cols);

    [[nodiscard]] std::span<const ColumnInfo> columns() const noexcept {
        return columnInfo_;
    }

    [[nodiscard]] std::span<ColumnInfo> columnsMutable() noexcept {
        return columnInfo_;
    }

    bool clickHeader(Point2I localPt, bool rightClick = false);
    bool beginResizeColumn(Point2I localPt);
    bool dragResizeColumn(Point2I localPt);
    void endResizeColumn() noexcept;

    [[nodiscard]] static std::string truncateInfoText(std::string_view text,
                                                      Int32 maxPixels,
                                                      const gfx::BitmapFont* font = nullptr,
                                                      Int32 fallbackCharWidth = 6);

    virtual std::string getCellText(Point2I cell) const;

protected:
    bool drawCell(gfx::SoftwareSurface&, Point2I, Point2I, bool, bool) const override;
    std::vector<ColumnInfo> columnInfo_{};
    bool columnsResizeable_{true};
    Int32 columnToResize_{-1};
    Point2I dragPoint_{0, 0};
    Int32 startWidth_{0};
    bool columnsSortable_{true};
    bool columnsDualSortable_{false};
    bool sortAscending_{true};
    bool sortAscendingSecond_{true};
    Int32 columnToSort_{-1};
    Int32 columnToSortSecond_{-1};
    bool refresh_{true};
};

// -----------------------------------------------------------------------------
// FGHudArrayCtrl (FearGuiHudArrayCtrl.cpp / FearHudArrayCtrl.cpp)
// -----------------------------------------------------------------------------
class FGHudArrayCtrl : public FGArrayCtrl {
    using Parent = FGArrayCtrl;

public:
    FGHudArrayCtrl();
    ~FGHudArrayCtrl() override = default;

    bool onAdd() override;
    bool drawCell(gfx::SoftwareSurface&, Point2I, Point2I, bool, bool) const override;
};

// -----------------------------------------------------------------------------
// HudListCtrl (FearGuiHudList.cpp)
// -----------------------------------------------------------------------------
class HudListCtrl : public FGHudArrayCtrl {
    using Parent = FGHudArrayCtrl;

public:
    static constexpr std::size_t HudCount = 10;

    struct HudToggleSpec {
        Int32 tag{0};
        std::string_view label{};
        bool active{true};
    };

    HudListCtrl();
    ~HudListCtrl() override = default;

    void updateResolution(Point2I surfaceExtent, Int32 hiFontHeight = 12, Int32 loFontHeight = 6);

    [[nodiscard]] bool isLowRes() const noexcept {
        return lowRes_;
    }

    [[nodiscard]] std::span<const HudToggleSpec, HudCount> specs() const noexcept {
        return specs_;
    }

    bool toggleRow(std::size_t row);

    void wake() override;
    bool select(Point2I cell) override;
    std::string getCellText(Point2I cell) const override;

private:
    std::array<HudToggleSpec, HudCount> specs_{};
    bool lowRes_{false};
};

// -----------------------------------------------------------------------------
// FGHudMatrix (FearHudMatrixCtrl.cpp)
// -----------------------------------------------------------------------------
class FGHudMatrix : public gui::Matrix {
    using Parent = gui::Matrix;

public:
    FGHudMatrix() = default;
    ~FGHudMatrix() override = default;

    [[nodiscard]] Point2I loResPosition() const noexcept {
        return loResPosition_;
    }

    void setLoResPosition(Point2I p) noexcept {
        loResPosition_ = p;
    }

    [[nodiscard]] Point2I loResExtent() const noexcept {
        return loResExtent_;
    }

    void setLoResExtent(Point2I e) noexcept {
        loResExtent_ = e;
    }

    void applyResolution(bool lowRes);
    void wake() override;
    std::function<void()> checkpoint() override;

    bool inspectRead(sim::Inspect& inspector) override;
    bool inspectWrite(sim::Inspect& inspector) const override;

    Persistent::Error
    read(StreamIO& sio, Int32 version, Int32 user, Persistent::LoadContext& ctx) override;
    Persistent::Error
    write(StreamIO& sio, Int32 version, Int32 user, Persistent::StoreContext& ctx) const override;

private:
    Point2I hiResPosition_{0, 0};
    Point2I hiResExtent_{100, 100};
    Point2I loResPosition_{0, 0};
    Point2I loResExtent_{64, 64};
    bool lowRes_{false};
};

// -----------------------------------------------------------------------------
// FearGuiRadio (FearGuiRadio.cpp)
// -----------------------------------------------------------------------------
class FearGuiRadio : public gui::Active {
    using Parent = gui::Active;

public:
    FearGuiRadio() = default;
    ~FearGuiRadio() override = default;

    [[nodiscard]] bool isSet() const noexcept {
        return set_;
    }

    void setState(bool val) noexcept {
        set_ = val;
    }

    bool onAdd() override;
    void action() override;
    bool message(gui::Control& sender, UInt32 msg) override;
    bool processEvent(const sim::SimEvent& event) override;
    bool onSimActionEvent(const sim::SimActionEvent& event);
    std::function<void()> checkpoint() override;

    bool inspectRead(sim::Inspect& inspector) override;
    bool inspectWrite(sim::Inspect& inspector) const override;

    Persistent::Error
    read(StreamIO& sio, Int32 version, Int32 user, Persistent::LoadContext& ctx) override;
    Persistent::Error
    write(StreamIO& sio, Int32 version, Int32 user, Persistent::StoreContext& ctx) const override;

protected:
    bool set_{false};
};

} // namespace opent1::game
