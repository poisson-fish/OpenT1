#pragma once
#include <opent1/core/types.hpp>
#include <opent1/sim/sim_base.hpp>
#include <opent1/sim/sim_collision_movement.hpp>
#include <format>
#include <functional>
#include <string>
#include <string_view>

namespace opent1::game {

using SimTime = sim::SimTime;

struct SphereF {
    Point3F center{};
    RealF radius{0.0f};
    constexpr SphereF() noexcept = default;

    constexpr SphereF(const Point3F& c, RealF r) noexcept : center(c), radius(r) {}

    [[nodiscard]] constexpr bool operator==(const SphereF&) const = default;
};

// -----------------------------------------------------------------------------
// Fear Object / Set IDs (feardcl.h)
// -----------------------------------------------------------------------------
inline constexpr sim::SimObjectId FearIdRange = 712;
inline constexpr sim::SimObjectId PlayerManagerId = 713;
inline constexpr sim::SimObjectId SnowfallId = 714;
inline constexpr sim::SimObjectId PlayerPSCId = 715;
inline constexpr sim::SimObjectId MissionCenterPosId = 716;
inline constexpr sim::SimObjectId MoveableSetId = 718;
inline constexpr sim::SimObjectId PlayerSetId = 719;
inline constexpr sim::SimObjectId SensorManagerId = 720;
// DataBlockManagerId = 721 (defined in data_block_manager.hpp)
inline constexpr sim::SimObjectId SensorVisibleSetId = 723;
inline constexpr sim::SimObjectId DecalManagerId = 724;
inline constexpr sim::SimObjectId TargetableSetId = 725;
inline constexpr sim::SimObjectId AIManagerId = 726;
inline constexpr sim::SimObjectId ObserverSetId = 727;
inline constexpr sim::SimObjectId SmokeManagerId = 728;
inline constexpr sim::SimObjectId TribesSkyId = 729;

// -----------------------------------------------------------------------------
// Fear Event Types (feardcl.h: FearTypeRange = 1090)
// -----------------------------------------------------------------------------
inline constexpr Int32 FearTypeRange = 1090;
inline constexpr Int32 PlayerAddEventType = 1099;
inline constexpr Int32 PlayerRemoveEventType = 1100;
inline constexpr Int32 DeltaScoreEventType = 1101;
inline constexpr Int32 TeamAddEventType = 1103;
inline constexpr Int32 MissionResetEventType = 1104;
inline constexpr Int32 PlayerTeamChangeEventType = 1106;
inline constexpr Int32 PlayerSayEventType = 1107;
inline constexpr Int32 PlayerCommandEventType = 1109;
inline constexpr Int32 PlayerSelectCmdrEventType = 1110;
// DataBlockEventType = 1112 (defined in data_block_manager.hpp)
inline constexpr Int32 TeamObjectiveEventType = 1113;
inline constexpr Int32 LocSoundEventType = 1116;
inline constexpr Int32 TargetNameEventType = 1117;
inline constexpr Int32 VoiceEventType = 1119;
inline constexpr Int32 SoundEventType = 1120;
inline constexpr Int32 PingPLEventType = 1121;
inline constexpr Int32 PlayerSkinEventType = 1122;

inline constexpr Int32 FearEventTypeRange = 1106;
inline constexpr Int32 FearRemoteCommandEventType = 1107;
inline constexpr Int32 FearPlayerInputEventType = 1108;
inline constexpr Int32 FearMoveEventType = 1109;
inline constexpr Int32 FearDamageSkinEventType = 1110;
inline constexpr Int32 FearTeamSkinEventType = 1111;
inline constexpr Int32 WinEventType = 1113;
inline constexpr Int32 GhostAlwaysDoneEventType = 1114;
inline constexpr Int32 FearCSDelegateRepickEventType = 1115;
inline constexpr Int32 ProjectileFactoryEventType = 1116;
inline constexpr Int32 TestEventType = 1118;
inline constexpr Int32 MarkerDropEventType = 1120;
inline constexpr Int32 HudSubmittedEventType = 1121;
inline constexpr Int32 SimActionEventType = 1122;
inline constexpr Int32 DropPointEventType = 1123;

// -----------------------------------------------------------------------------
// Fear Force Types (FearForceDcl.h: SimMovementAppForce = 8)
// -----------------------------------------------------------------------------
inline constexpr UInt32 SimMovementAppForce = 8;
inline constexpr UInt32 TribesJetForce = 1u << (SimMovementAppForce + 0);
inline constexpr UInt32 TribesRocketForce = 1u << (SimMovementAppForce + 1);

// -----------------------------------------------------------------------------
// Fear Persistent Tags (feardcl.h)
// -----------------------------------------------------------------------------
inline constexpr UInt32 FearPersTagRange = 512;
inline constexpr UInt32 FearTurretPersTag = 514;
inline constexpr UInt32 FearInteriorShapePersTag = 521;
inline constexpr UInt32 InteriorShapePersTag = 522;
inline constexpr UInt32 DebrisCloudPersTag = 524;
inline constexpr UInt32 PlayerPersTag = 536;
inline constexpr UInt32 MarkerPersTag = 539;
inline constexpr UInt32 TankPersTag = 548;
inline constexpr UInt32 FlierPersTag = 547;
inline constexpr UInt32 BulletPersTag = 600;
inline constexpr UInt32 GrenadePersTag = 601;
inline constexpr UInt32 RocketPersTag = 602;
inline constexpr UInt32 RocketDumbPersTag = RocketPersTag;
inline constexpr UInt32 LaserPersTag = 603;
inline constexpr UInt32 LaserProjectilePersTag = LaserPersTag;
inline constexpr UInt32 SoundEmitterPersTag = 520;
inline constexpr UInt32 FearInteriorPersTag = 521;
inline constexpr UInt32 ExplosionPersTag = 535;
inline constexpr UInt32 DebrisPersTag = 523;
inline constexpr UInt32 FearMarkerPersTag = 524;
inline constexpr UInt32 FearMissionMarkerPersTag = 525;
inline constexpr UInt32 FearDropPointMarkerPersTag = 526;
inline constexpr UInt32 TurretPersTag = 514;
inline constexpr UInt32 FearSimInteriorPersTag = 528;
inline constexpr UInt32 SensorPersTag = 553;
inline constexpr UInt32 CarPersTag = 549;
inline constexpr UInt32 HoverPersTag = 546;
inline constexpr UInt32 DoorPersTag = 532;
inline constexpr UInt32 MoveableBasePersTag = 532;
inline constexpr UInt32 ElevatorPersTag = 533;
inline constexpr UInt32 MoveableBaseDataPersTag = 533;
inline constexpr UInt32 TribesSkyPersTag = 560;
inline constexpr UInt32 PowerGeneratorPersTag = 535;
inline constexpr UInt32 CommandStationPersTag = 536;
inline constexpr UInt32 SupplyStationPersTag = 537;
inline constexpr UInt32 StarFieldPersTag = 562;
inline constexpr UInt32 SnowfallPersTag = 534;
inline constexpr UInt32 TargetLaserPersTag = 604;
inline constexpr UInt32 MinePersTag = 542;
inline constexpr UInt32 ObserverCamPersTag = 561;
inline constexpr UInt32 ItemPersTag = 554;
inline constexpr UInt32 FearMoveablePersTag = 544;
inline constexpr UInt32 StaticShapePersTag = 545;
inline constexpr UInt32 TriggerPersTag = 543;
inline constexpr UInt32 LightningPersTag = 606;
inline constexpr UInt32 SeekingMissilePersTag = 605;
inline constexpr UInt32 RepairEffectPersTag = 607;
inline constexpr UInt32 VehiclePersTag = 556;
inline constexpr UInt32 PlanetPersTag = 559;

// -----------------------------------------------------------------------------
// Fear Query Types (feardcl.h)
// -----------------------------------------------------------------------------
inline constexpr Int32 FearQueryTypeRange = 1106;
inline constexpr Int32 WaganQueryType = 1107;

// -----------------------------------------------------------------------------
// SimObjectType Bitmasks (simObjectTypes.h + feardcl.h)
// -----------------------------------------------------------------------------
inline constexpr UInt32 SimDefaultObjectType = 1u << 0;
inline constexpr UInt32 SimTerrainObjectType = 1u << 1;
inline constexpr UInt32 SimInteriorObjectType = 1u << 2;
inline constexpr UInt32 SimCameraObjectType = 1u << 3;
inline constexpr UInt32 SimMissionObjectType = 1u << 4;
inline constexpr UInt32 SimShapeObjectType = 1u << 5;
inline constexpr UInt32 SimContainerObjectType = 1u << 6;
inline constexpr UInt32 SimPlayerObjectType = 1u << 7;
inline constexpr UInt32 SimProjectileObjectType = 1u << 8;
inline constexpr UInt32 SimVehicleObjectType = 1u << 9;
inline constexpr UInt32 SimDebrisObjectType = SimDefaultObjectType;

inline constexpr Int32 LaserDamageType = 6;

inline constexpr UInt32 AIObjectType = 1u << 19;
inline constexpr UInt32 FearMarkerObjectType = 1u << 20;
inline constexpr UInt32 ItemObjectType = 1u << 21;
inline constexpr UInt32 MoveableObjectType = 1u << 22;
inline constexpr UInt32 StaticObjectType = 1u << 23;
inline constexpr UInt32 MoveableBaseObjectType = 1u << 24;
inline constexpr UInt32 StaticInteriorObjectType = 1u << 25;
inline constexpr UInt32 MineObjectType = 1u << 26;
inline constexpr UInt32 TriggerObjectType = 1u << 28;
inline constexpr UInt32 VehicleObjectType = 1u << 29;
inline constexpr UInt32 CorpseObjectType = 1u << 30;
inline constexpr UInt32 AllObjectTypes = 0xFFFFFFFFu;

// -----------------------------------------------------------------------------
// Fear Collision Image Types (feardcl.h)
// -----------------------------------------------------------------------------
inline constexpr Int32 ShapeCollisionImageTypeId = 8;
inline constexpr Int32 PlayerCollisionImageTypeId = 9;
inline constexpr auto ShapeCollisionImageType =
    static_cast<sim::SimCollisionImage::Type>(ShapeCollisionImageTypeId);
inline constexpr auto PlayerCollisionImageType =
    static_cast<sim::SimCollisionImage::Type>(PlayerCollisionImageTypeId);

// -----------------------------------------------------------------------------
// FBase: PhoenixAssert (f_base.cpp)
// -----------------------------------------------------------------------------
enum PhoenixAssertFlag : UInt32 {
    ASSERT_NO_FLAGS = 0u,
    ASSERT_NO_LOG = 1u << 0,
    ASSERT_NO_DIALOG = 1u << 1,
    ASSERT_NO_WARNING = 1u << 2,
    ASSERT_NO_MESSAGE = 1u << 3,
};

enum PhoenixAssertType : UInt32 {
    ASSERT_MSG = 1u,
    ASSERT_WRN = 2u,
    ASSERT_ISV = 3u,
    ASSERT_EXC = 4u,
};

struct PhoenixAssertException {
    std::string file{};
    UInt32 line{0};
    std::string message{};
};

class PhoenixAssert {
public:
    static constexpr std::string_view kSeparator =
        "========================================================================\r\n";
    static constexpr std::size_t kMaxLength = 255;

    using Callback = std::function<void(std::string_view line1, std::string_view line2)>;

    PhoenixAssert();
    ~PhoenixAssert() noexcept = default;

    static void install(PhoenixAssert* instance = nullptr) noexcept;
    [[nodiscard]] static PhoenixAssert& active() noexcept;

    void setFlags(UInt32 flags) noexcept {
        flags_ = flags;
    }

    [[nodiscard]] UInt32 flags() const noexcept {
        return flags_;
    }

    void installCallback(Callback cb) {
        callback_ = std::move(cb);
    }

    void enableExceptions() noexcept {
        exceptionsEnabled_ = true;
    }

    void disableExceptions() noexcept {
        exceptionsEnabled_ = false;
    }

    [[nodiscard]] bool exceptionsEnabled() const noexcept {
        return exceptionsEnabled_;
    }

    void setLogFile(std::string_view path);

    [[nodiscard]] std::string_view logFile() const noexcept {
        return logFile_;
    }

    void process(PhoenixAssertType type,
                 std::string_view fileName,
                 UInt32 lineNumber,
                 std::string_view message);

    void reset() noexcept;

    [[nodiscard]] bool firstAssert() const noexcept {
        return firstAssert_;
    }

    [[nodiscard]] UInt32 countMessage() const noexcept {
        return countMessage_;
    }

    [[nodiscard]] UInt32 countWarning() const noexcept {
        return countWarning_;
    }

    [[nodiscard]] UInt32 countISV() const noexcept {
        return countISV_;
    }

    [[nodiscard]] std::string_view logBuffer() const noexcept {
        return logBuffer_;
    }

    [[nodiscard]] std::string_view lastLine1() const noexcept {
        return lastLine1_;
    }

    [[nodiscard]] std::string_view lastLine2() const noexcept {
        return lastLine2_;
    }

private:
    void log(std::string_view line1, std::string_view line2);

    Callback callback_{};
    UInt32 flags_{ASSERT_NO_DIALOG};
    bool firstAssert_{true};
    bool exceptionsEnabled_{false};
    std::string logFile_{};
    UInt32 countMessage_{0};
    UInt32 countWarning_{0};
    UInt32 countISV_{0};
    std::string logBuffer_{};
    std::string lastLine1_{};
    std::string lastLine2_{};
};

} // namespace opent1::game
