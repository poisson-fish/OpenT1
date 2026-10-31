#pragma once
#include <opent1/core/bit_stream.hpp>
#include <opent1/core/res_manager.hpp>
#include <opent1/math/box.hpp>
#include <opent1/math/point.hpp>
#include <opent1/sim/sim_base.hpp>
#include <opent1/sim/sim_net.hpp>
#include <opent1/world/ts_shape.hpp>
#include <array>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace opent1::game {

inline constexpr sim::SimObjectId DataBlockManagerId = 721;
inline constexpr Int32 DataBlockEventType = 1112;

class DataBlockManager;

struct ProjectileDataType {
    Int32 type{-1};
    Int32 dataType{-1};
    bool operator==(const ProjectileDataType&) const = default;
};

struct GameBaseData {
    Int32 dbmIndex{-1};
    Int32 dbmDatFileIndex{-1};
    Int32 dbmGroup{-1};
    std::string dbmName;
    std::string pluginClassName;
    std::string className;
    bool visibleToSensor{};
    Int32 mapFilter{-1};
    std::string description{"Shape"};
    std::string mapIcon;
    bool validateShape{};
    bool validateMaterials{};

    GameBaseData();
    virtual ~GameBaseData() = default;

    [[nodiscard]] virtual std::unique_ptr<GameBaseData> clone() const {
        return std::make_unique<GameBaseData>(*this);
    }

    virtual bool preload(ResourceManager* rm, bool server, std::string& errorBuffer);
    virtual void pack(BitStream& stream, DataBlockManager* dbm) const;
    virtual bool unpack(BitStream& stream, DataBlockManager* dbm);
};

struct SoundProfileData : GameBaseData {
    struct Profile {
        UInt32 flags{};
        Flt32 baseVolume{};
        Flt32 coneInsideAngle{90.0f};
        Flt32 coneOutsideAngle{180.0f};
        Point3F coneVector{0.0f, -1.0f, 0.0f};
        Flt32 coneOutsideVolume{};
        Flt32 minDistance{30.0f};
        Flt32 maxDistance{300.0f};
    };

    Profile profile{};
    Flt32 dist{};

    SoundProfileData();

    [[nodiscard]] std::unique_ptr<GameBaseData> clone() const override {
        return std::make_unique<SoundProfileData>(*this);
    }

    bool preload(ResourceManager* rm, bool server, std::string& errorBuffer) override;
    void pack(BitStream& stream, DataBlockManager* dbm) const override;
    bool unpack(BitStream& stream, DataBlockManager* dbm) override;
};

struct SoundData : GameBaseData {
    std::string wavFileName;
    Flt32 priority{};
    Int32 profileIndex{-1};

    SoundData();

    [[nodiscard]] std::unique_ptr<GameBaseData> clone() const override {
        return std::make_unique<SoundData>(*this);
    }

    bool preload(ResourceManager* rm, bool server, std::string& errorBuffer) override;
    void pack(BitStream& stream, DataBlockManager* dbm) const override;
    bool unpack(BitStream& stream, DataBlockManager* dbm) override;
};

struct DamageSkinData : GameBaseData {
    static constexpr std::size_t kBmpCount = 10;
    std::array<std::string, kBmpCount> bmpName{};
    Int32 numBmps{};

    DamageSkinData();

    [[nodiscard]] std::unique_ptr<GameBaseData> clone() const override {
        return std::make_unique<DamageSkinData>(*this);
    }

    bool preload(ResourceManager* rm, bool server, std::string& errorBuffer) override;
    void pack(BitStream& stream, DataBlockManager* dbm) const override;
    bool unpack(BitStream& stream, DataBlockManager* dbm) override;
};

struct ShapeBaseData : GameBaseData {
    static constexpr std::size_t kMaxSequenceIndex = 16;

    struct SequenceSound {
        std::string sequence;
        Int32 sound{-1};
    };

    std::string fileName;
    std::string shieldShapeName;
    Int32 damageSkinId{-1};
    Int32 sfxShield{-1};
    Int32 shadowDetailMask{};
    Int32 explosionId{-1};
    Int32 debrisId{-1};
    Flt32 maxEnergy{};
    Flt32 maxDamage{1.0f};
    Flt32 repairRate{0.1f};
    std::array<SequenceSound, kMaxSequenceIndex> sequenceSound{};
    UInt32 shapeCRC{};
    bool isPerspective{};
    Resource<world::TsShape> shape;
    Resource<world::TsShape> shieldShape;

    ShapeBaseData();

    [[nodiscard]] std::unique_ptr<GameBaseData> clone() const override {
        return std::make_unique<ShapeBaseData>(*this);
    }

    bool preload(ResourceManager* rm, bool server, std::string& errorBuffer) override;
    void pack(BitStream& stream, DataBlockManager* dbm) const override;
    bool unpack(BitStream& stream, DataBlockManager* dbm) override;
};

struct StaticBaseData : ShapeBaseData {
    struct Damage {
        Flt32 disabled{0.5f};
        Flt32 destroyed{0.75f};
    };

    Int32 sfxAmbient{-1};
    Damage damageLevel{};
    bool isTranslucent{};
    Flt32 outerRadius{};
    bool disableCollision{};

    StaticBaseData();

    [[nodiscard]] std::unique_ptr<GameBaseData> clone() const override {
        return std::make_unique<StaticBaseData>(*this);
    }

    void pack(BitStream& stream, DataBlockManager* dbm) const override;
    bool unpack(BitStream& stream, DataBlockManager* dbm) override;
};

struct StaticShapeData : StaticBaseData {
    StaticShapeData() = default;

    [[nodiscard]] std::unique_ptr<GameBaseData> clone() const override {
        return std::make_unique<StaticShapeData>(*this);
    }
};

struct PlayerData : ShapeBaseData {
    static constexpr std::size_t kNumAnims = 51;
    static constexpr std::size_t kNumFootSounds = 15;

    struct AnimData {
        std::string name;
        Int32 soundTag{-1};
        Int32 direction{1};
        Int32 viewFlags{};
        Int32 priority{};
    };

    std::string flameShapeName;
    std::array<AnimData, kNumAnims> animData{};
    Flt32 maxForwardSpeed{};
    Flt32 maxBackwardSpeed{};
    Flt32 maxSideSpeed{};
    Flt32 groundForce{20.0f};
    Flt32 groundTraction{1.0f};
    Flt32 maxJetForwardVelocity{20.0f};
    Flt32 maxJetSideForceFactor{1.0f};
    Flt32 minJetEnergy{1.0f};
    Flt32 jetEnergyDrain{1.0f};
    Flt32 jumpImpulse{100.0f};
    Flt32 jumpSurfaceMinDot{0.2f};
    Flt32 mass{};
    Flt32 drag{};
    Flt32 density{};
    Flt32 jetForce{};
    Int32 jetSound{-1};
    std::array<Int32, kNumFootSounds> rFootSounds{};
    std::array<Int32, kNumFootSounds> lFootSounds{};
    std::array<Int32, 2> footPrints{};
    bool canCrouch{};
    Flt32 boxWidth{};
    Flt32 boxDepth{};
    Flt32 boxNormalHeight{};
    Flt32 boxCrouchHeight{};
    Flt32 minDamageSpeed{13.0f};
    Flt32 damageScale{0.009f};
    Flt32 boxNormalHeadPercentage{0.6666f};
    Flt32 boxNormalTorsoPercentage{0.3333f};
    Flt32 boxCrouchHeadPercentage{0.6666f};
    Flt32 boxCrouchTorsoPercentage{0.3333f};
    Flt32 boxHeadLeftPercentage{};
    Flt32 boxHeadRightPercentage{1.0f};
    Flt32 boxHeadFrontPercentage{1.0f};
    Flt32 boxHeadBackPercentage{};

    PlayerData();

    [[nodiscard]] std::unique_ptr<GameBaseData> clone() const override {
        return std::make_unique<PlayerData>(*this);
    }

    void pack(BitStream& stream, DataBlockManager* dbm) const override;
    bool unpack(BitStream& stream, DataBlockManager* dbm) override;
};

struct ItemImageData : GameBaseData {
    std::string shapeFile;
    Int32 mountPoint{2};
    Point3F mountOffset{};
    Point3F mountRotation{};
    Int32 weaponType{-1};
    Int32 ammoType{-1};
    Flt32 activateTime{0.5f};
    Flt32 fireTime{0.2f};
    Flt32 reloadTime{1.0f};
    Flt32 spinUpTime{2.0f};
    Flt32 spinDownTime{2.0f};
    ProjectileDataType projectile{};
    Flt32 minEnergy{2.0f};
    Flt32 maxEnergy{10.0f};
    Flt32 mass{};
    Int32 lightType{};
    Flt32 lightTime{1.5f};
    Flt32 lightRadius{3.0f};
    Point3F lightColor{1.0f, 1.0f, 1.0f};
    Int32 sfxActivateTag{-1};
    Int32 sfxFireTag{-1};
    Int32 sfxReadyTag{-1};
    Int32 sfxReloadTag{-1};
    Int32 sfxSpinUpTag{-1};
    Int32 sfxSpinDownTag{-1};
    bool firstPerson{true};
    bool accuFire{};
    Resource<world::TsShape> shape;

    ItemImageData();

    [[nodiscard]] std::unique_ptr<GameBaseData> clone() const override {
        return std::make_unique<ItemImageData>(*this);
    }

    bool preload(ResourceManager* rm, bool server, std::string& errorBuffer) override;
    void pack(BitStream& stream, DataBlockManager* dbm) const override;
    bool unpack(BitStream& stream, DataBlockManager* dbm) override;
};

struct ItemData : StaticBaseData {
    Int32 imageId{-1};
    Int32 price{};
    Flt32 mass{1.0f};
    Flt32 drag{1.0f};
    Flt32 density{1.0f};
    Flt32 friction{1.0f};
    Flt32 elasticity{0.2f};
    std::string hudIcon;
    std::string typeString{"z----"};
    bool showInventory{true};
    bool showWeaponBar{};
    bool hiliteOnActive{};
    Int32 lightType{};
    Flt32 lightTime{1.5f};
    Flt32 lightRadius{3.0f};
    Point3F lightColor{1.0f, 1.0f, 1.0f};

    ItemData();

    [[nodiscard]] std::unique_ptr<GameBaseData> clone() const override {
        return std::make_unique<ItemData>(*this);
    }

    bool preload(ResourceManager* rm, bool server, std::string& errorBuffer) override;
    void pack(BitStream& stream, DataBlockManager* dbm) const override;
    bool unpack(BitStream& stream, DataBlockManager* dbm) override;
};

struct MoveableData : StaticBaseData {
    bool displace{true};
    bool blockForward{true};
    bool blockBackward{true};
    Int32 sfxStart{-1};
    Int32 sfxStop{-1};
    Int32 sfxRun{-1};
    Int32 sfxBlocked{-1};
    Flt32 speed{1.0f};
    std::string side;

    MoveableData();

    [[nodiscard]] std::unique_ptr<GameBaseData> clone() const override {
        return std::make_unique<MoveableData>(*this);
    }

    void pack(BitStream& stream, DataBlockManager* dbm) const override;
    bool unpack(BitStream& stream, DataBlockManager* dbm) override;
};

struct SensorData : StaticBaseData {
    bool fCastLOS{};
    Int32 iRange{100};
    Int32 iDopplerVelocity{};
    bool supression{};
    bool supressable{true};
    bool pinger{true};

    SensorData();

    [[nodiscard]] std::unique_ptr<GameBaseData> clone() const override {
        return std::make_unique<SensorData>(*this);
    }

    void pack(BitStream& stream, DataBlockManager* dbm) const override;
    bool unpack(BitStream& stream, DataBlockManager* dbm) override;
};

struct VehicleData : StaticBaseData {
    Flt32 mass{};
    Flt32 drag{};
    Flt32 density{};
    Flt32 minSpeed{};
    Flt32 maxSpeed{};
    ProjectileDataType projectile{};
    Int32 fireSound{-1};
    Int32 damageSound{-1};
    Int32 mountSound{-1};
    Int32 dismountSound{-1};
    Int32 idleSound{-1};
    Int32 moveSound{-1};
    Flt32 reloadDelay{};
    Flt32 ramDamage{};
    Int32 ramDamageType{};
    std::array<Point3F, 4> suspension{};
    bool visibleDriver{};
    Int32 driverPose{};

    VehicleData();

    [[nodiscard]] std::unique_ptr<GameBaseData> clone() const override {
        return std::make_unique<VehicleData>(*this);
    }

    void pack(BitStream& stream, DataBlockManager* dbm) const override;
    bool unpack(BitStream& stream, DataBlockManager* dbm) override;
};

struct FlierData : VehicleData {
    Flt32 maxBank{};
    Flt32 maxPitch{};
    Flt32 lift{};
    Flt32 maxAlt{};
    Flt32 maxVertical{};
    Flt32 accel{};
    Flt32 groundDamageScale{};

    FlierData() = default;

    [[nodiscard]] std::unique_ptr<GameBaseData> clone() const override {
        return std::make_unique<FlierData>(*this);
    }

    void pack(BitStream& stream, DataBlockManager* dbm) const override;
    bool unpack(BitStream& stream, DataBlockManager* dbm) override;
};

struct TankData : VehicleData {
    Flt32 maxGrade{};

    TankData() = default;

    [[nodiscard]] std::unique_ptr<GameBaseData> clone() const override {
        return std::make_unique<TankData>(*this);
    }

    void pack(BitStream& stream, DataBlockManager* dbm) const override;
    bool unpack(BitStream& stream, DataBlockManager* dbm) override;
};

struct ProjectileData : GameBaseData {
    std::string bulletShapeName;
    Int32 explosionTag{};
    Int32 expRandCycle{};
    bool collideWithOwner{};
    UInt32 ownerGraceMS{1000};
    Flt32 collisionRadius{};
    Flt32 mass{1.0f};
    Int32 damageClass{};
    Flt32 damageValue{};
    Int32 damageType{};
    Flt32 explosionRadius{};
    Flt32 kickBackStrength{};
    Flt32 aimDeflection{};
    Flt32 muzzleVelocity{};
    Flt32 terminalVelocity{};
    Flt32 acceleration{};
    Flt32 totalTime{10000.0f};
    Flt32 liveTime{10000.0f};
    Flt32 projSpecialTime{};
    bool projSpecialBool{};
    Flt32 lightRange{};
    ColorF lightColor{0.0f, 0.0f, 0.0f};
    Flt32 inheritedVelocityScale{};
    Int32 soundId{-1};
    UInt32 shapeCRC{};
    Resource<world::TsShape> shape;

    ProjectileData();

    [[nodiscard]] std::unique_ptr<GameBaseData> clone() const override {
        return std::make_unique<ProjectileData>(*this);
    }

    bool preload(ResourceManager* rm, bool server, std::string& errorBuffer) override;
    void pack(BitStream& stream, DataBlockManager* dbm) const override;
    bool unpack(BitStream& stream, DataBlockManager* dbm) override;
    [[nodiscard]] virtual Flt32 getTerminalVelocity() const noexcept;
    virtual bool getTargetVector(const Point3F& rShootFrom,
                                 const Point3F& rShootTo,
                                 Flt32 gravComp,
                                 Point3F* pVector,
                                 bool* pInRange,
                                 bool secondSolution = false) const noexcept;
};

struct BulletData : ProjectileData {
    Flt32 rotationPeriod{};
    Int32 bulletHoleIndex{-1};
    Flt32 tracerLength{10.0f};

    BulletData();

    [[nodiscard]] std::unique_ptr<GameBaseData> clone() const override {
        return std::make_unique<BulletData>(*this);
    }

    void pack(BitStream& stream, DataBlockManager* dbm) const override;
    bool unpack(BitStream& stream, DataBlockManager* dbm) override;
    [[nodiscard]] Flt32 getTerminalVelocity() const noexcept override;
    bool getTargetVector(const Point3F& rShootFrom,
                         const Point3F& rShootTo,
                         Flt32 gravComp,
                         Point3F* pVector,
                         bool* pInRange,
                         bool secondSolution = false) const noexcept override;
};

struct GrenadeData : ProjectileData {
    Flt32 elasticity{0.5f};
    std::string pSmokeName{"smoke.dts"};
    Resource<world::TsShape> smokeShape;

    GrenadeData();

    [[nodiscard]] std::unique_ptr<GameBaseData> clone() const override {
        return std::make_unique<GrenadeData>(*this);
    }

    bool preload(ResourceManager* rm, bool server, std::string& errorBuffer) override;
    void pack(BitStream& stream, DataBlockManager* dbm) const override;
    bool unpack(BitStream& stream, DataBlockManager* dbm) override;
    [[nodiscard]] Flt32 getTerminalVelocity() const noexcept override;
    bool getTargetVector(const Point3F& rShootFrom,
                         const Point3F& rShootTo,
                         Flt32 gravComp,
                         Point3F* pVector,
                         bool* pInRange,
                         bool secondSolution = false) const noexcept override;
};

struct RocketData : ProjectileData {
    enum TrailType : Int32 {
        None = 0,
        Swoosh = 1,
        Smoke = 2,
    };

    Int32 trailType{};
    std::string trailString;
    Flt32 trailLength{10.0f};
    Flt32 trailWidth{0.3f};
    Resource<world::TsShape> smokeShape;

    RocketData();

    [[nodiscard]] std::unique_ptr<GameBaseData> clone() const override {
        return std::make_unique<RocketData>(*this);
    }

    bool preload(ResourceManager* rm, bool server, std::string& errorBuffer) override;
    void pack(BitStream& stream, DataBlockManager* dbm) const override;
    bool unpack(BitStream& stream, DataBlockManager* dbm) override;
    [[nodiscard]] Flt32 getTerminalVelocity() const noexcept override;
    bool getTargetVector(const Point3F& rShootFrom,
                         const Point3F& rShootTo,
                         Flt32 gravComp,
                         Point3F* pVector,
                         bool* pInRange,
                         bool secondSolution = false) const noexcept override;
};

struct LaserData : ProjectileData {
    std::string pBitmapName{"laserPulse.bmp"};
    std::string pHitShapeName;
    bool detachFromShooter{};
    Resource<world::TsShape> hitShape;

    LaserData();

    [[nodiscard]] std::unique_ptr<GameBaseData> clone() const override {
        return std::make_unique<LaserData>(*this);
    }

    bool preload(ResourceManager* rm, bool server, std::string& errorBuffer) override;
    void pack(BitStream& stream, DataBlockManager* dbm) const override;
    bool unpack(BitStream& stream, DataBlockManager* dbm) override;
    [[nodiscard]] Flt32 getTerminalVelocity() const noexcept override;
    bool getTargetVector(const Point3F& rShootFrom,
                         const Point3F& rShootTo,
                         Flt32 gravComp,
                         Point3F* pVector,
                         bool* pInRange,
                         bool secondSolution = false) const noexcept override;
};

struct TargetLaserData : LaserData {
    TargetLaserData() = default;

    [[nodiscard]] std::unique_ptr<GameBaseData> clone() const override {
        return std::make_unique<TargetLaserData>(*this);
    }
};

struct SeekingMissileData : ProjectileData {
    Flt32 seekingTurningRadius{15.0f};
    Flt32 nonSeekingTurningRadius{50.0f};
    Flt32 proximityDist{2.0f};
    Resource<world::TsShape> smokeShape;

    SeekingMissileData();

    [[nodiscard]] std::unique_ptr<GameBaseData> clone() const override {
        return std::make_unique<SeekingMissileData>(*this);
    }

    bool preload(ResourceManager* rm, bool server, std::string& errorBuffer) override;
    void pack(BitStream& stream, DataBlockManager* dbm) const override;
    bool unpack(BitStream& stream, DataBlockManager* dbm) override;
};

struct LightningData : ProjectileData {
    std::string pBitmapName{"lightningTemp.bmp"};
    Flt32 boltLength{5.0f};
    Flt32 coneAngle{};
    Flt32 damagePerSec{};
    Flt32 energyDrainPerSec{};
    Int32 segmentDivisions{1};
    Int32 numSegments{};
    Flt32 beamWidth{0.2f};
    UInt32 updateTime{};
    Flt32 skipPercent{};
    Flt32 displaceBias{0.25f};

    LightningData();

    [[nodiscard]] std::unique_ptr<GameBaseData> clone() const override {
        return std::make_unique<LightningData>(*this);
    }

    void pack(BitStream& stream, DataBlockManager* dbm) const override;
    bool unpack(BitStream& stream, DataBlockManager* dbm) override;
    [[nodiscard]] Flt32 getTerminalVelocity() const noexcept override;
    bool getTargetVector(const Point3F& rShootFrom,
                         const Point3F& rShootTo,
                         Flt32 gravComp,
                         Point3F* pVector,
                         bool* pInRange,
                         bool secondSolution = false) const noexcept override;
};

struct RepairEffectData : LightningData {
    RepairEffectData();

    [[nodiscard]] std::unique_ptr<GameBaseData> clone() const override {
        return std::make_unique<RepairEffectData>(*this);
    }
};

struct TurretData : SensorData {
    ProjectileDataType projectile{};
    Flt32 gunRange{-1.0f};
    Flt32 speed{1.0f};
    Flt32 speedModifier{};
    Flt32 FOV{-1.0f};
    Flt32 minGunEnergy{};
    Flt32 maxGunEnergy{};
    Flt32 reloadDelay{};
    Int32 fireSound{-1};
    Int32 activationSound{-1};
    Int32 deactivateSound{-1};
    Int32 whirSound{-1};
    Flt32 deflection{};
    Flt32 targetableFovRatio{0.5f};
    bool isSustained{};
    Int32 firingTime{};
    Flt32 energyRate{};

    TurretData();

    [[nodiscard]] std::unique_ptr<GameBaseData> clone() const override {
        return std::make_unique<TurretData>(*this);
    }

    void pack(BitStream& stream, DataBlockManager* dbm) const override;
    bool unpack(BitStream& stream, DataBlockManager* dbm) override;
};

struct ExplosionData : GameBaseData {
    static constexpr std::size_t kNumLightColors = 3;

    std::string pShapeName;
    Int32 soundId{-1};
    Flt32 timeScale{1.0f};
    bool faceCamera{};
    bool randomSpin{};
    bool hasLight{};
    Flt32 lightRange{};
    Flt32 timeZero{};
    Flt32 timeOne{};
    std::array<ColorF, kNumLightColors> colors{};
    std::array<Flt32, 3> radFactors{};
    bool shiftPosition{};
    UInt32 shapeCRC{};
    Resource<world::TsShape> shape;

    ExplosionData();

    [[nodiscard]] std::unique_ptr<GameBaseData> clone() const override {
        return std::make_unique<ExplosionData>(*this);
    }

    bool preload(ResourceManager* rm, bool server, std::string& errorBuffer) override;
    void pack(BitStream& stream, DataBlockManager* dbm) const override;
    bool unpack(BitStream& stream, DataBlockManager* dbm) override;
};

struct MarkerData : GameBaseData {
    std::string fileName;
    Resource<world::TsShape> shape;

    MarkerData();

    [[nodiscard]] std::unique_ptr<GameBaseData> clone() const override {
        return std::make_unique<MarkerData>(*this);
    }

    bool preload(ResourceManager* rm, bool server, std::string& errorBuffer) override;
    void pack(BitStream& stream, DataBlockManager* dbm) const override;
    bool unpack(BitStream& stream, DataBlockManager* dbm) override;
};

struct DebrisData : GameBaseData {
    Int32 type{};
    std::string pShapeName;
    Int32 imageType{};
    Int32 imageSortValue{};
    Flt32 mass{1.0f};
    Flt32 elasticity{1.0f};
    Flt32 friction{1.0f};
    std::array<Flt32, 3> center{};
    Int32 collisionMask{6}; // SimTerrainObjectType | SimInteriorObjectType
    Int32 knockMask{};
    Int32 animationSequence{-1};
    Flt32 animRestartTime{};
    Flt32 minTimeout{1.0f};
    Flt32 maxTimeout{2.0f};
    Flt32 explodeOnBounce{0.3f};
    Flt32 damage{100.0f};
    Flt32 damageThreshold{1.0f};
    Int32 spawnedDebrisMask{1};
    Flt32 spawnedDebrisStrength{50.0f};
    Flt32 spawnedDebrisRadius{0.2f};
    Int32 spawnedExplosionID{-1};
    Flt32 p{};
    bool explodeOnRest{true};
    Int32 collisionDetail{};
    Resource<world::TsShape> shape;

    DebrisData();

    [[nodiscard]] std::unique_ptr<GameBaseData> clone() const override {
        return std::make_unique<DebrisData>(*this);
    }

    bool preload(ResourceManager* rm, bool server, std::string& errorBuffer) override;
    void pack(BitStream& stream, DataBlockManager* dbm) const override;
    bool unpack(BitStream& stream, DataBlockManager* dbm) override;
};

struct MineData : ItemData {
    Int32 damageType{};
    Flt32 damageValue{1.0f};
    Flt32 explosionRadius{1.0f};
    Flt32 kickBackStrength{100.0f};

    MineData();

    [[nodiscard]] std::unique_ptr<GameBaseData> clone() const override {
        return std::make_unique<MineData>(*this);
    }

    void pack(BitStream& stream, DataBlockManager* dbm) const override;
    bool unpack(BitStream& stream, DataBlockManager* dbm) override;
};

struct TriggerData : GameBaseData {
    Flt32 rate{0.2f};

    TriggerData();

    [[nodiscard]] std::unique_ptr<GameBaseData> clone() const override {
        return std::make_unique<TriggerData>(*this);
    }
};

struct CarData : VehicleData {
    struct WheelInfo {
        bool steerForward{};
        bool steerReverse{};
        Point3F wheelBase{};
    };

    Int32 wheelCount{4};
    std::array<WheelInfo, 6> wheels{};
    Flt32 wheelRadius{1.0f};
    Flt32 wheelRestDist{2.5f};
    Flt32 wheelOneGDist{2.3f};
    Flt32 wheelSideBrakeForce{100.0f};
    Flt32 turnAngle{0.19634954f};
    std::string wheelShapeName{"generator"};

    CarData();

    [[nodiscard]] std::unique_ptr<GameBaseData> clone() const override {
        return std::make_unique<CarData>(*this);
    }

    void pack(BitStream& stream, DataBlockManager* dbm) const override;
    bool unpack(BitStream& stream, DataBlockManager* dbm) override;
};

struct IRCChannelData : GameBaseData {
    std::string name;
    std::string topic;

    IRCChannelData() = default;

    [[nodiscard]] std::unique_ptr<GameBaseData> clone() const override {
        return std::make_unique<IRCChannelData>(*this);
    }
};

struct DataBlockEvent final : sim::SimEvent {
    Int32 group{-1};
    Int32 block{-1};
    Int32 totalBlocks{};
    std::unique_ptr<GameBaseData> data;

    DataBlockEvent() {
        type = DataBlockEventType;
        flags = Guaranteed | Ordered;
        address.objectId = DataBlockManagerId;
    }

    bool pack(BitStream& stream, DataBlockManager& dbm) const;
    bool unpack(BitStream& stream, DataBlockManager& dbm);
};

bool registerDataBlockEvent(sim::network::Registry& registry);

class DataBlockManager final : public sim::SimObject {
public:
    enum Constants : Int32 {
        SoundProfileDataType = 0,
        SoundDataType = 1,
        DamageSkinDataType = 2,
        PlayerDataType = 3,
        StaticShapeDataType = 4,
        ItemDataType = 5,
        ItemImageDataType = 6,
        MoveableDataType = 7,
        SensorDataType = 8,
        VehicleDataType = 9,
        FlierDataType = 10,
        TankDataType = 11,
        HoverDataType = 12,
        ProjectileDataType = 13,
        BulletDataType = 14,
        GrenadeDataType = 15,
        RocketDataType = 16,
        LaserDataType = 17,
        InteriorShapeType = 18,
        TurretDataType = 19,
        ExplosionDataType = 20,
        MarkerDataType = 21,
        DebrisDataType = 22,
        MineDataType = 23,
        TargetLaserDataType = 24,
        SeekingMissileDataType = 25,
        TriggerDataType = 26,
        CarDataType = 27,
        LightningDataType = 28,
        RepairEffectDataType = 29,
        IRCChannelType = 30,
        NumDataTypes = 31,
        DoorDataType = 31,
        ElevatorDataType = 32
    };

    [[nodiscard]] static std::string_view dataGroupDisplayName(Int32 group) noexcept;

    struct Snapshot {
        std::array<std::vector<std::unique_ptr<GameBaseData>>, NumDataTypes> groups;
        std::string lastError;
    };

    explicit DataBlockManager(bool isServer = true,
                              std::shared_ptr<ResourceManager> resources = {});
    ~DataBlockManager() override = default;

    [[nodiscard]] bool isServer() const noexcept {
        return isServer_;
    }

    void setIsServer(bool server) noexcept {
        isServer_ = server;
    }

    void setResources(std::shared_ptr<ResourceManager> resources) {
        resources_ = std::move(resources);
    }

    [[nodiscard]] ResourceManager* resources() const noexcept;

    [[nodiscard]] static std::unique_ptr<GameBaseData> instantiateBlock(Int32 group);

    GameBaseData*
    createDataBlock(std::string_view name, Int32 group, GameBaseData** prevBlock = nullptr);
    bool createBlock(Int32 group, Int32 block, Int32 totalBlocks);
    bool preloadBlock(Int32 group, Int32 block);
    bool serverPreload();
    void reset();

    [[nodiscard]] Int32 getDataGroupSize(Int32 group) const noexcept;
    [[nodiscard]] Int32 getDataGroupBitSize(Int32 group) const noexcept;
    void writeBlockId(BitStream& stream, Int32 group, Int32 value) const;
    [[nodiscard]] Int32 readBlockId(BitStream& stream, Int32 group) const;

    [[nodiscard]] GameBaseData* lookupDataBlock(Int32 blockId, Int32 groupId) const noexcept;
    [[nodiscard]] GameBaseData*
    lookupDataBlock(std::string_view name, Int32 groupId, Int32* blockId = nullptr) const noexcept;
    [[nodiscard]] GameBaseData* lookupDataBlock(std::string_view name) const noexcept;
    [[nodiscard]] std::string_view lookupBlockName(Int32 blockId, Int32 groupId) const noexcept;

    [[nodiscard]] std::vector<std::unique_ptr<DataBlockEvent>> buildClientEvents() const;
    bool sendDataToClient(sim::SimObjectId clientManagerId);
    bool sendDataToStream(sim::network::PacketStream& stream) const;

    bool processEvent(const sim::SimEvent& event) override;

    [[nodiscard]] const std::string& lastDataBlockError() const noexcept {
        return lastError_;
    }

    std::function<void()> onDataFinished;

    void setLastDataBlockError(std::string error) {
        lastError_ = std::move(error);
    }

    [[nodiscard]] Snapshot captureSnapshot() const;
    void restoreSnapshot(const Snapshot& snapshot);

private:
    static constexpr std::size_t kHashTableSize = 1024;

    struct HashNode {
        Int32 group{-1};
        Int32 index{-1};
    };

    [[nodiscard]] static std::size_t hashName(std::string_view name) noexcept;
    void rebuildLookupTable();

    bool isServer_{true};
    std::shared_ptr<ResourceManager> resources_;
    std::array<std::vector<std::unique_ptr<GameBaseData>>, NumDataTypes> dataBlocks_{};
    std::array<std::vector<HashNode>, kHashTableSize> hashTable_{};
    std::string lastError_;
};

} // namespace opent1::game
