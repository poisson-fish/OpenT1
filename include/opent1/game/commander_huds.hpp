#pragma once

#include <opent1/game/hud_controls.hpp>
#include <opent1/game/player_manager.hpp>
#include <opent1/game/sensor_manager.hpp>
#include <opent1/gui/ts_control.hpp>
#include <opent1/gui/text_format.hpp>
#include <opent1/sim/sim_net.hpp>

#include <array>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace opent1::game {

inline constexpr UInt32 MissionCenterPosPersTag = MissionCenterPosId;

// -----------------------------------------------------------------------------
// MissionCenterPos & TSCommander (FearGuiTSCommander.cpp)
// -----------------------------------------------------------------------------
class MissionCenterPos : public sim::SimNetObject {
    using Parent = sim::SimNetObject;

public:
    Point2F pos{1230.0f, 1230.0f};
    Point2F extent{1400.0f, 1400.0f};

    MissionCenterPos();
    ~MissionCenterPos() override = default;

    static void initPersistFields();

    bool onAdd() override;
    bool processArguments(std::span<const std::string> args) override;
    bool inspectRead(sim::Inspect& inspector) override;
    bool inspectWrite(sim::Inspect& inspector) const override;

    [[nodiscard]] UInt16 getGhostTag() const override {
        return static_cast<UInt16>(MissionCenterPosPersTag);
    }

    UInt32
    packUpdate(sim::network::GhostManager& gm, UInt32 mask, BitStream& stream) const override;
    bool unpackUpdate(sim::network::GhostManager& gm, BitStream& stream) override;

    Persistent::Error
    read(StreamIO& sio, Int32 version, Int32 user, Persistent::LoadContext& ctx) override;
    Persistent::Error
    write(StreamIO& sio, Int32 version, Int32 user, Persistent::StoreContext& ctx) const override;
};

class TSCommander : public gui::TSControl {
    using Parent = gui::TSControl;

public:
    enum Mode : Int32 {
        SelectingMode = 0,
        WaypointMode = 1,
        MaxNumTargs = 256,
        MapFilterPlayers = (1 << 0),
        MapFilterTurrets = (1 << 1),
        MapFilterItems = (1 << 2),
        MapFilterAll = (1 << 7),
    };

    struct SensorTargetInfo {
        Point2I pos{0, 0}; // normalized [0..1023]
        Int32 datGroup{0};
        Int32 datId{0};
        Int32 playerId{-1};
        Int32 team{0};
        std::string name{};
        Int32 damageStatus{0};
        bool activeStatus{true};
        bool inDanger{false};
        bool selected{false};
        Int32 filter{-1}, command{-1}, commandStatus{}, commandTarget{-1};
        Point2I waypoint{};
        Flt32 sensorRange{};
        std::shared_ptr<const gfx::Bitmap> icon;
    };

    TSCommander();
    ~TSCommander() override = default;

    [[nodiscard]] static Point2I normalizedToScreen(Point2I normPos, Point2I viewExtent) noexcept;
    [[nodiscard]] static Point2I worldToNormalized(Point2F worldPos,
                                                   Point2F missionCenterPos,
                                                   Point2F missionCenterExt) noexcept;
    [[nodiscard]] static Int32 whereAmIBitmapSector(Flt32 yawRadians) noexcept;

    void setZoomMode(bool zoomIn, UInt32 transitionTimeMs = 250) noexcept;

    [[nodiscard]] bool isZoomed() const noexcept {
        return zoomMode_;
    }

    [[nodiscard]] Int32 zoomLevel() const noexcept {
        return curZoom_;
    }

    void stepZoom(bool zoomIn) noexcept;
    [[nodiscard]] Point2F effectiveWorldExtent(Point2F baseMissionExt) const noexcept;

    void setMapFilter(UInt32 filterMask) noexcept {
        mapFilter_ = filterMask;
    }

    [[nodiscard]] UInt32 mapFilter() const noexcept {
        return mapFilter_;
    }

    void setCommandMode(Mode mode) noexcept {
        mode_ = mode;
    }

    [[nodiscard]] Mode commandMode() const noexcept {
        return mode_;
    }

    void addTarget(SensorTargetInfo target);
    void clearTargets() noexcept;
    void refreshTargets(const PlayerPSC&,
                        const PlayerManager&,
                        const DataBlockManager&,
                        const SensorManager*,
                        Int32 clientId);
    void bindResources(std::shared_ptr<ResourceManager>);
    bool rebuildMap(sim::SimWorld&, const gfx::Palette&, Point2F origin, Point2F extent);

    [[nodiscard]] bool hasMap() const noexcept {
        return bool(map_);
    }

    [[nodiscard]] UInt32 mapRevision() const noexcept {
        return mapRevision_;
    }

    std::function<void(std::string_view, std::span<const std::string>)> executeCommand;
    std::function<void(std::string_view, std::string_view)> setVariable;
    std::function<void(Int32, bool)> playerSelectionChanged;
    std::function<void(std::string_view)> commandDisplayChanged;
    Int32 localTeam{}, localClient{}, selectType{DataBlockManager::PlayerDataType};
    bool showNames{true}, showSensorRange{}, translucentSensors{}, centerMode{};
    Flt32 facing{};
    void issueCommand(Int32, std::string_view);
    bool issueWaypoint(Point2I, Int32 target = -1);
    bool selectPlayer(Int32, bool, bool additive = true);
    bool selectObject(Int32, bool additive = false);
    [[nodiscard]] Point2I screenPosition(Point2I normalized) const;
    void tick(double) override;
    bool event(const gui::Event&) override;
    bool draw(gfx::SoftwareSurface&, Point2I, RectI) const override;
    std::function<void()> checkpoint() override;

    [[nodiscard]] std::span<const SensorTargetInfo> targets() const noexcept {
        return targets_;
    }

    bool selectTargetByKey(Int32 keyIndex);
    bool selectTargetsInBox(RectI screenBox);

    [[nodiscard]] Int32 selectedTargetKey() const noexcept {
        return selectedTargKey_;
    }

    [[nodiscard]] Int32 selectedPlayerId() const noexcept {
        return selectedPlayerId_;
    }

    [[nodiscard]] std::string buildIssueCommandScript(Int32 commandId,
                                                      std::string_view commandText,
                                                      Point2I normWayPoint) const;

    bool onSimActionEvent(const sim::SimActionEvent& event);
    bool processEvent(const sim::SimEvent& event) override;

private:
    std::vector<SensorTargetInfo> targets_{};
    Mode mode_{SelectingMode};
    UInt32 mapFilter_{MapFilterAll | MapFilterPlayers | MapFilterTurrets | MapFilterItems};
    Int32 curZoom_{0};
    bool zoomMode_{false};
    UInt32 zoomTransitionTimeMs_{250};
    Int32 selectedTargKey_{-1};
    Int32 selectedPlayerId_{-1};
    std::shared_ptr<ResourceManager> resources_;
    std::shared_ptr<const gfx::Bitmap> map_;
    std::shared_ptr<const gfx::BitmapArray> orderIcons_, facingIcons_;
    std::shared_ptr<const gfx::BitmapFont> textFont_;
    UInt32 mapRevision_{};
    Point2F missionExtent_{1400, 1400}, mapCenter_{512, 512};
    Point2I mouseStart_{}, mousePoint_{};
    bool dragging_{}, scrolling_{}, additiveSelection_{}, clickedTarget_{}, starredCommand_{};
    Int32 currentCommand_{-1};
    std::string commandText_;
    Flt32 currentScale_{1}, startScale_{1};
    double lastTick_{}, zoomStarted_{};
    void setSelected(std::size_t, bool);
    void clearSelection();
    [[nodiscard]] bool targetVisible(const SensorTargetInfo&) const;
    [[nodiscard]] Point2I normalizedPosition(Point2I) const;
};

// -----------------------------------------------------------------------------
// FGCommandView (FearGuiCmdView.cpp)
// -----------------------------------------------------------------------------
class FGCommandView : public gui::Control {
    using Parent = gui::Control;

public:
    FGCommandView();
    ~FGCommandView() override = default;

    [[nodiscard]] bool isLowRes() const noexcept {
        return lowRes_;
    }

    void setLowRes(bool lr) noexcept {
        lowRes_ = lr;
    }

    bool onAdd() override;

    void setBorder(std::shared_ptr<const gfx::BitmapArray> border) {
        border_ = std::move(border);
    }

    bool draw(gfx::SoftwareSurface&, Point2I, RectI) const override;
    std::function<void()> checkpoint() override;

private:
    bool lowRes_{false};
    std::shared_ptr<const gfx::BitmapArray> border_;
};

// -----------------------------------------------------------------------------
// FGCommandPanel & FGCommandBox (FearGuiCmdPanel.cpp)
// -----------------------------------------------------------------------------
class FGCommandPanel : public gui::Control {
    using Parent = gui::Control;

public:
    FGCommandPanel();
    ~FGCommandPanel() override = default;

    [[nodiscard]] Int32 activeSlideIndex() const noexcept {
        return activeSlide_;
    }

    void selectSlide(Int32 index) noexcept {
        activeSlide_ = index;
    }

    bool message(gui::Control& sender, UInt32 msg) override;
    bool draw(gfx::SoftwareSurface&, Point2I, RectI) const override;

private:
    Int32 activeSlide_{0};
};

class FGCommandBox : public gui::Control {
    using Parent = gui::Control;

public:
    FGCommandBox() = default;
    ~FGCommandBox() override = default;
};

// -----------------------------------------------------------------------------
// FGCommandRadio (FearGuiCmdRadio.cpp)
// -----------------------------------------------------------------------------
class FGCommandRadio : public FearGuiRadio {
    using Parent = FearGuiRadio;

public:
    FGCommandRadio();
    ~FGCommandRadio() override = default;

    [[nodiscard]] Int32 cmdTag() const noexcept {
        return cmdTag_;
    }

    void setCmdTag(Int32 tag) noexcept {
        cmdTag_ = tag;
    }

    [[nodiscard]] Int32 radioTag() const noexcept {
        return radioTag_;
    }

    void setRadioTag(Int32 tag) noexcept {
        radioTag_ = tag;
    }

    [[nodiscard]] Int32 bmpOnTag() const noexcept {
        return bmpOnTag_;
    }

    void setBmpOnTag(Int32 tag) noexcept {
        bmpOnTag_ = tag;
    }

    [[nodiscard]] Int32 bmpOffTag() const noexcept {
        return bmpOffTag_;
    }

    void setBmpOffTag(Int32 tag) noexcept {
        bmpOffTag_ = tag;
    }

    [[nodiscard]] Int32 loResBmpOnTag() const noexcept {
        return loResBmpOnTag_;
    }

    void setLoResBmpOnTag(Int32 tag) noexcept {
        loResBmpOnTag_ = tag;
    }

    [[nodiscard]] Int32 loResBmpOffTag() const noexcept {
        return loResBmpOffTag_;
    }

    void setLoResBmpOffTag(Int32 tag) noexcept {
        loResBmpOffTag_ = tag;
    }

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

    void setLowRes(bool lores);
    std::function<void(Int32)> showCommand;

    void setCommandBitmaps(std::array<std::shared_ptr<const gfx::Bitmap>, 4> values) {
        commandBitmaps_ = std::move(values);
    }

    bool draw(gfx::SoftwareSurface&, Point2I, RectI) const override;

    void wake() override;
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
    Int32 cmdTag_{-1};
    std::array<std::shared_ptr<const gfx::Bitmap>, 4> commandBitmaps_;
    Int32 radioTag_{-1};
    Int32 bmpOnTag_{0};
    Int32 bmpOffTag_{0};
    Int32 loResBmpOnTag_{0};
    Int32 loResBmpOffTag_{0};
    Point2I hiResPosition_{0, 0};
    Point2I hiResExtent_{100, 50};
    Point2I loResPosition_{0, 0};
    Point2I loResExtent_{100, 50};
    bool lowRes_{false};
};

// -----------------------------------------------------------------------------
// CommandTurretCtrl (FearGuiCmdTurretList.cpp)
// -----------------------------------------------------------------------------
class CommandTurretCtrl : public FGHudArrayCtrl {
    using Parent = FGHudArrayCtrl;

public:
    struct TurretEntry {
        Int32 targIndex{0};
        std::string name{};
        Int32 damageStatus{0};
        bool activeStatus{true};
    };

    CommandTurretCtrl();
    ~CommandTurretCtrl() override = default;

    void populateFromCommander(const TSCommander& cmdr);

    [[nodiscard]] std::span<const TurretEntry> entries() const noexcept {
        return entries_;
    }

    std::string getCellText(Point2I cell) const override;
    std::function<void(Int32, bool, bool)> clicked;
    bool event(const gui::Event&) override;
    bool drawCell(gfx::SoftwareSurface&, Point2I, Point2I, bool, bool) const override;

private:
    std::vector<TurretEntry> entries_{};
};

// -----------------------------------------------------------------------------
// CommandItemCtrl (FearGuiCommandItemList.cpp)
// -----------------------------------------------------------------------------
class CommandItemCtrl : public FGHudArrayCtrl {
    using Parent = FGHudArrayCtrl;

public:
    struct ItemEntry {
        Int32 targIndex{0};
        Int32 datGroup{0};
        Int32 datId{0};
        std::string name{};
        Int32 damageStatus{0};
        bool activeStatus{true};
    };

    CommandItemCtrl();
    ~CommandItemCtrl() override = default;

    void populateFromCommander(const TSCommander& cmdr);

    [[nodiscard]] std::span<const ItemEntry> entries() const noexcept {
        return entries_;
    }

    std::string getCellText(Point2I cell) const override;
    std::function<void(Int32, bool, bool)> clicked;
    bool event(const gui::Event&) override;
    bool drawCell(gfx::SoftwareSurface&, Point2I, Point2I, bool, bool) const override;

private:
    std::vector<ItemEntry> entries_{};
};

// -----------------------------------------------------------------------------
// CommandTeamCtrl (FearGuiCommandTeamList.cpp)
// -----------------------------------------------------------------------------
class CommandTeamCtrl : public FGHudArrayCtrl {
    using Parent = FGHudArrayCtrl;

public:
    struct TeamMemberRow {
        Int32 clientId{0};
        Int32 commanderId{-1};
        Int32 depth{0};
        std::string name{};
        Int32 commandStatus{0};
        std::string statusSymbol{};
        bool selected{false};
    };

    CommandTeamCtrl();
    ~CommandTeamCtrl() override = default;

    [[nodiscard]] static std::string_view statusStringForCode(Int32 status) noexcept;
    void buildTeamHierarchy(PlayerManager& pm, Int32 teamId);

    [[nodiscard]] std::span<const TeamMemberRow> rows() const noexcept {
        return rows_;
    }

    std::string getCellText(Point2I cell) const override;
    std::function<void(Int32, bool, bool, bool)> clicked;
    bool event(const gui::Event&) override;
    bool drawCell(gfx::SoftwareSurface&, Point2I, Point2I, bool, bool) const override;

private:
    std::vector<TeamMemberRow> rows_{};
};

// -----------------------------------------------------------------------------
// CMDObserve (fearGuiCMDObserve.cpp)
// -----------------------------------------------------------------------------
class CMDObserve : public gui::TSControl {
    using Parent = gui::TSControl;

public:
    CMDObserve() = default;
    ~CMDObserve() override = default;

    void setObservedPlayer(Int32 playerId) noexcept {
        observedPlayerId_ = playerId;
    }

    [[nodiscard]] Int32 observedPlayer() const noexcept {
        return observedPlayerId_;
    }

    void setControllingTurret(bool ctrl) noexcept {
        controllingTurret_ = ctrl;
    }

    [[nodiscard]] bool isControllingTurret() const noexcept {
        return controllingTurret_;
    }

    std::function<void(Int32)> requestObservation;
    std::function<std::pair<Flt32, Flt32>()> status;
    bool activate(bool, Int32 selectedPlayer = -1);
    bool observePlayer(Int32);
    [[nodiscard]] gui::TSControl* sceneControl();
    void wake() override;
    void tick(double) override;
    bool draw(gfx::SoftwareSurface&, Point2I, RectI) const override;

    // The shipped observer is a Control wrapper containing a TSControl.
    // Keep the standalone viewport API without adding its byte to the wire.
    Persistent::Error read(StreamIO&, Int32, Int32, Persistent::LoadContext&) override;
    Persistent::Error write(StreamIO&, Int32, Int32, Persistent::StoreContext&) const override;

private:
    Int32 observedPlayerId_{-1};
    bool controllingTurret_{false};
    double lastFrame_{}, frameRemainder_{};
    Int32 startupWidth_{}, startupHeight_{2}, snowDensity_{4}, healthPixels_{}, energyPixels_{};
    UInt32 snowFrame_{};
};

// -----------------------------------------------------------------------------
// FGCurrentCommand (FearGuiCurCmd.cpp)
// -----------------------------------------------------------------------------
class FGCurrentCommand : public gui::Text {
    using Parent = gui::Text;

public:
    FGCurrentCommand() = default;
    ~FGCurrentCommand() override = default;

    void setCommandDisplay(std::string_view cmdText, Int32 cmdStatus = 0);

    [[nodiscard]] Int32 commandStatus() const noexcept {
        return cmdStatus_;
    }

private:
    Int32 cmdStatus_{0};
};

// -----------------------------------------------------------------------------
// FGHCommandObjectivePage (FearHudCmdObj.cpp)
// -----------------------------------------------------------------------------
class FGHCommandObjectivePage : public FGHudArrayCtrl {
    using Parent = FGHudArrayCtrl;

public:
    static constexpr std::size_t MaxObjectives = 16;

    FGHCommandObjectivePage();
    ~FGHCommandObjectivePage() override = default;

    void setObjectives(std::vector<std::string> objectives);

    [[nodiscard]] std::span<const std::string> objectives() const noexcept {
        return objectives_;
    }

    std::string getCellText(Point2I cell) const override;
    std::array<gui::TextFormat::Font, 10> objectiveFonts;
    std::function<std::shared_ptr<const gfx::Bitmap>(std::string_view)> objectiveBitmap;
    std::function<std::optional<std::string>(std::string_view)> objectiveVariable;
    void tick(double) override;
    bool draw(gfx::SoftwareSurface&, Point2I, RectI) const override;

private:
    std::vector<std::string> objectives_{};
    std::vector<std::unique_ptr<gui::TextFormat>> formatted_;
    Int32 formattedWidth_{};
};

} // namespace opent1::game
