#pragma once

#include <opent1/game/data_block_manager.hpp>
#include <opent1/game/fear_player_psc.hpp>
#include <opent1/game/hud_controls.hpp>
#include <opent1/game/player.hpp>
#include <opent1/game/player_manager.hpp>
#include <opent1/gui/text_format.hpp>
#include <opent1/sim/sim_environment_render.hpp>

#include <array>
#include <string>
#include <string_view>
#include <vector>

namespace opent1::game {

// -----------------------------------------------------------------------------
// HealthHud (HealthHud.cpp)
// -----------------------------------------------------------------------------
class HealthHud : public HudCtrl {
    using Parent = HudCtrl;

public:
    struct Metrics {
        Flt32 energyLevel{1.0f};
        Int32 energyWidth{85};
        Int32 barWidth{84};
        Point2I barOffset{37, 10};
        UInt8 color{ShellPaletteColor::GREEN};
        bool flashIcon{false};
        bool lowRes{false};
    };

    HealthHud();
    ~HealthHud() override = default;

    [[nodiscard]] static Metrics
    computeMetrics(Flt32 damageLevel, bool lowRes, UInt32 timeElapsedMs = 0) noexcept;

    void
    updateFromPSC(const PlayerPSC* psc, bool lowRes = false, UInt32 timeElapsedMs = 0) noexcept;

    [[nodiscard]] const Metrics& metrics() const noexcept {
        return metrics_;
    }

    bool onAdd() override;
    bool draw(gfx::SoftwareSurface& s, Point2I offset, RectI clip) const override;

private:
    Metrics metrics_{};
};

// -----------------------------------------------------------------------------
// JetPackHud (FearGuiJetHud.cpp)
// -----------------------------------------------------------------------------
class JetPackHud : public HudCtrl {
    using Parent = HudCtrl;

public:
    struct Metrics {
        Flt32 energyLevel{1.0f};
        Int32 energyWidth{85};
        Int32 barWidth{84};
        Point2I barOffset{37, 11};
        UInt8 color{ShellPaletteColor::LIGHT_BLUE};
        bool lowRes{false};
    };

    JetPackHud();
    ~JetPackHud() override = default;

    [[nodiscard]] static Metrics computeMetrics(Flt32 energyRatio, bool lowRes) noexcept;

    void updateFromPSC(const PlayerPSC* psc, bool lowRes = false) noexcept;

    [[nodiscard]] const Metrics& metrics() const noexcept {
        return metrics_;
    }

    bool onAdd() override;
    bool draw(gfx::SoftwareSurface& s, Point2I offset, RectI clip) const override;

private:
    Metrics metrics_{};
};

// -----------------------------------------------------------------------------
// ClockHud (clockhud.cpp)
// -----------------------------------------------------------------------------
class ClockHud : public HudCtrl {
    using Parent = HudCtrl;

public:
    ClockHud();
    ~ClockHud() override = default;

    [[nodiscard]] static std::string formatTime(Flt32 clockSeconds);

    void setClockTime(Flt32 seconds);

    [[nodiscard]] Flt32 clockTime() const noexcept {
        return clockSeconds_;
    }

    [[nodiscard]] const std::string& formattedText() const noexcept {
        return formattedText_;
    }

    bool onAdd() override;
    bool draw(gfx::SoftwareSurface& s, Point2I offset, RectI clip) const override;

private:
    Flt32 clockSeconds_{0.0f};
    std::string formattedText_{"00:00:00.0"};
};

// -----------------------------------------------------------------------------
// CompassHud (compasshud.cpp)
// -----------------------------------------------------------------------------
class CompassHud : public HudCtrl {
    using Parent = HudCtrl;

public:
    struct State {
        Point3F playerPos{0.0f, 0.0f, 0.0f};
        Flt32 playerYaw{0.0f};
        Point2F wayPoint{0.0f, 0.0f};
        bool wayPointActive{false};
        Int32 distanceDisplay{0};
        std::string distanceText{"000"};
        Flt32 wayPointAngle{0.0f};
        std::array<Point2I, 3> arrowVertices{};
        std::array<Point2I, 12> compassVertices{};
        bool lowRes{false};
    };

    CompassHud();
    ~CompassHud() override = default;

    [[nodiscard]] static State computeState(const Point3F& playerPos,
                                            Flt32 playerYaw,
                                            Point2F wayPoint,
                                            bool wayPointActive,
                                            bool lowRes);

    void updateState(const Point3F& playerPos,
                     Flt32 playerYaw,
                     Point2F wayPoint,
                     bool wayPointActive,
                     bool lowRes = false);

    [[nodiscard]] const State& state() const noexcept {
        return state_;
    }

    bool onAdd() override;
    bool draw(gfx::SoftwareSurface& s, Point2I offset, RectI clip) const override;

private:
    State state_{};
};

// -----------------------------------------------------------------------------
// CurrentWeaponHud (CurWeapHud.cpp)
// -----------------------------------------------------------------------------
class CurrentWeaponHud : public HudCtrl {
    using Parent = HudCtrl;

public:
    struct WeaponSlot {
        Int32 itemTypeId{0};
        std::string hudIcon{};
        Int32 ammoCount{0};
        bool infiniteAmmo{false};
        bool mounted{false};
        std::string ammoText{};
    };

    struct Layout {
        std::vector<WeaponSlot> slots{};
        Point2I extent{79, 0};
        Int32 transWidth{36};
        Int32 iconSpacing{4};
        Int32 iconOffsetY{7};
        bool lowRes{false};
    };

    CurrentWeaponHud();
    ~CurrentWeaponHud() override = default;

    [[nodiscard]] static Layout computeLayout(const PlayerPSC* psc,
                                              const DataBlockManager* dbm,
                                              bool lowRes,
                                              Int32 defaultIconHeight = 20);

    void updateFromPSC(const PlayerPSC* psc, const DataBlockManager* dbm, bool lowRes = false);

    [[nodiscard]] const Layout& layout() const noexcept {
        return layout_;
    }

    bool onAdd() override;
    bool draw(gfx::SoftwareSurface& s, Point2I offset, RectI clip) const override;

private:
    Layout layout_{};
};

// -----------------------------------------------------------------------------
// Crosshair (fearGuiCrosshair.cpp)
// -----------------------------------------------------------------------------
class Crosshair : public HudCtrl {
    using Parent = HudCtrl;

public:
    struct ReticleMetrics {
        bool zoomReticle{false};
        RectI zoomRect{0, 0, 0, 0};
        Int32 zoomHalfWidth{0};
        bool hasTarget{false};
        bool targetFriendly{false};
        Flt32 targetDamage{-1.0f};
        std::string targetName{};
        bool hasLeadIndicator{false};
        Point3F leadPosition{0.0f, 0.0f, 0.0f};
        bool lowRes{false};
        bool enabled{true}, showZoomRectangle{};
        Int32 zoomHalfHeight{}, zoomMagnification{};

        struct Marker {
            Point2I point;
            bool friendly{}, lead{}, inRange{};
            std::optional<Point2I> targetPoint{};
            Flt32 distance{};
        };

        std::vector<Marker> markers;
    };

    Crosshair();
    std::shared_ptr<const gfx::BitmapFont> lowResFont;
    ~Crosshair() override = default;

    [[nodiscard]] static ReticleMetrics computeReticleMetrics(Point2I parentExtent,
                                                              Flt32 currentFovDeg,
                                                              Flt32 targetFovDeg,
                                                              Flt32 sniperFovDeg,
                                                              bool sniperCrosshairPref,
                                                              Int32 observerTeam = -1,
                                                              const ShapeBase* targetObj = nullptr,
                                                              std::string_view targetLabel = {},
                                                              Flt32 projectileVelocity = 0.0f,
                                                              const Point3F& muzzlePos = {
                                                                  0.0f, 0.0f, 0.0f});

    void updateReticle(const ReticleMetrics& m) noexcept {
        metrics_ = m;
    }

    void updateLive(sim::SimManager&,
                    const PlayerPSC&,
                    const PlayerManager*,
                    const DataBlockManager*,
                    const sim::EnvironmentCamera&,
                    UInt32 now,
                    bool sniperCrosshair,
                    bool sensors,
                    bool targets);

    [[nodiscard]] const ReticleMetrics& reticleMetrics() const noexcept {
        return metrics_;
    }

    bool onAdd() override;
    bool draw(gfx::SoftwareSurface& s, Point2I offset, RectI clip) const override;

private:
    ReticleMetrics metrics_{};
    Flt32 lastSniperFov_{-1};
    UInt32 zoomChangeTime_{};
};

// -----------------------------------------------------------------------------
// FGHSensorPing (FearHudRadarPing.cpp)
// -----------------------------------------------------------------------------
class FGHSensorPing : public HudCtrl {
    using Parent = HudCtrl;

public:
    enum FrameState : Int32 {
        FrameHidden = -1,
        FrameLow = 0,
        FrameMed = 1,
        FrameHigh = 2,
        FrameSuppressed = 3,
    };

    FGHSensorPing();
    ~FGHSensorPing() override = default;

    [[nodiscard]] static FrameState
    computePingFrame(UInt8 pingStatus, Flt32 curTimeSeconds, Flt32& startBlinkTimeInOut) noexcept;

    void updatePing(UInt8 pingStatus, Flt32 curTimeSeconds, bool lowRes = false) noexcept;

    [[nodiscard]] FrameState frameState() const noexcept {
        return frameState_;
    }

    [[nodiscard]] Flt32 startBlinkTime() const noexcept {
        return startBlinkTime_;
    }

    bool onAdd() override;
    bool draw(gfx::SoftwareSurface& s, Point2I offset, RectI clip) const override;

private:
    FrameState frameState_{FrameHidden};
    Flt32 startBlinkTime_{-1.0f};
};

// -----------------------------------------------------------------------------
// FGLagIndicator (fearGuiLag.cpp)
// -----------------------------------------------------------------------------
class FGLagIndicator : public gui::Control {
    using Parent = gui::Control;

public:
    static constexpr UInt32 DefaultLagTimeoutMs = 700;

    FGLagIndicator();
    ~FGLagIndicator() override = default;
    std::shared_ptr<const gfx::Bitmap> bitmap;

    void setLagTimeout(UInt32 timeoutMs) noexcept {
        lagTimeoutMs_ = timeoutMs;
    }

    [[nodiscard]] UInt32 lagTimeout() const noexcept {
        return lagTimeoutMs_;
    }

    [[nodiscard]] bool evaluateLag(UInt32 currentTimeMs,
                                   UInt32 lastReceivedTickMs,
                                   bool playbackMode = false) noexcept;

    [[nodiscard]] bool isLagging() const noexcept {
        return lagging_;
    }

    bool onAdd() override;
    bool draw(gfx::SoftwareSurface& s, Point2I offset, RectI clip) const override;

private:
    UInt32 lagTimeoutMs_{DefaultLagTimeoutMs};
    bool lagging_{false};
};

// -----------------------------------------------------------------------------
// CenterPrint (FearGuiCenterPrint.cpp)
// -----------------------------------------------------------------------------
class CenterPrint : public gui::Control {
    using Parent = gui::Control;

public:
    enum Placement : Int32 {
        Center = 0,
        Bottom = 1,
        Top = 2,
    };

    struct LayoutMetrics {
        Point2I position{35, 35};
        Point2I extent{570, 20};
        Int32 posOffset{35};
        bool lowRes{false};
        bool active{false};
    };

    CenterPrint();
    ~CenterPrint() override = default;

    // Legacy CenterPrint is a display-only overlay, including on inventory GUIs.
    bool pointInControl(Point2I) const override {
        return false;
    }

    void setMessage(std::string_view msg,
                    Placement placement = Center,
                    Flt32 timeoutSec = 5.0f,
                    Flt32 nowSec = 0.0f);
    void clearMessage() noexcept;
    std::function<std::pair<std::string, Int32>()> resolveMessage;
    std::array<std::shared_ptr<const gfx::BitmapFont>, 3> messageFonts{}, smallMessageFonts{};
    void tick(double nowSec) override;

    [[nodiscard]] const std::string& messageText() const noexcept {
        return messageText_;
    }

    [[nodiscard]] Placement placement() const noexcept {
        return placement_;
    }

    [[nodiscard]] LayoutMetrics computeLayout(Point2I parentExtent,
                                              Int32 formattedHeight = 16,
                                              Flt32 nowSec = 0.0f) const noexcept;

    bool onAdd() override;
    bool draw(gfx::SoftwareSurface& s, Point2I offset, RectI clip) const override;

private:
    std::string messageText_{};
    Placement placement_{Center};
    Flt32 startTimeSec_{0.0f};
    Flt32 timeoutSec_{0.0f};
    UInt32 resetCounter_{0};
    gui::TextFormat formatted_;
    bool renderActive_{};
};

} // namespace opent1::game
