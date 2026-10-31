#pragma once

#include <opent1/game/fear_player_psc.hpp>
#include <opent1/game/projectile.hpp>
#include <opent1/game/sensor.hpp>

#include <optional>
#include <span>

namespace opent1::game {

class Turret : public Sensor {
    using Parent = Sensor;

public:
    using TurretData = opent1::game::TurretData;

    enum State : Int32 {
        RETRACTED = 0,
        RETRACTING = 1,
        EXTENDING = 2,
        EXTENDED = 3,
    };

    enum FireState : UInt32 {
        Reloading = 0,
        Waiting = 1,
        Firing = 2,
    };

    enum TurretMaskBits : UInt32 {
        TRotationMask = Parent::NextFreeMask << 0,
        ElevationMask = Parent::NextFreeMask << 1,
        ShootingMask = Parent::NextFreeMask << 2,
        StateMask = Parent::NextFreeMask << 3,
        NextFreeMask = Parent::NextFreeMask << 4,
    };

    Turret();
    ~Turret() override;

    [[nodiscard]] Int32 getDatGroup() const noexcept override {
        return DataBlockManager::TurretDataType;
    }

    bool initResources(GameBaseData* in_data) override;
    bool onAdd() override;
    void onRemove() override;
    void onDeleteNotify(sim::SimObject& object) override;
    bool processQuery(sim::SimQuery& query) override;

    void serverProcess(UInt32 curTime) override;
    void clientProcess(UInt32 curTime) override;

    void onActivate() override;
    void onDeactivate() override;
    void onPowerStateChange(GameBase* generator) override;

    void serverUpdateMove(std::span<const PlayerMove> moves) override;
    void updateMove(const PlayerMove* move, Flt32 interval);
    [[nodiscard]] bool isControlable() const noexcept override;
    [[nodiscard]] bool cameraZoomAndCrosshairsEnabled() const noexcept override;
    [[nodiscard]] const TMat3F& getEyeTransform() const override;
    void getCameraTransform(Flt32 camDist, TMat3F* transform) const override;
    bool getMuzzleTransform(Int32 slot, TMat3F* transform) const override;
    bool getWeaponAmmoType(ProjectileDataType& outPrimary,
                           ProjectileDataType& outSecondary) const override;

    void writePacketData(BitStream& bstream) const override;
    bool readPacketData(BitStream& bstream) override;

    UInt32 packUpdate(sim::network::GhostManager& g, UInt32 mask, BitStream& stream) const override;
    bool unpackUpdate(sim::network::GhostManager& g, BitStream& stream) override;

    Persistent::Error
    read(StreamIO& sio, Int32 version, Int32 user, Persistent::LoadContext& ctx) override;
    Persistent::Error
    write(StreamIO& sio, Int32 version, Int32 user, Persistent::StoreContext& ctx) const override;

    // Turret aiming & firing helpers
    [[nodiscard]] Flt32 elevation(const Point3F& pos, bool clamp) const noexcept;
    [[nodiscard]] Flt32 elevation(Flt32 dx, Flt32 dy, Flt32 dz) const noexcept;
    [[nodiscard]] Flt32 rotation(const Point3F& pos) const noexcept;
    [[nodiscard]] Flt32 rotation(Flt32 dx, Flt32 dy) const noexcept;
    [[nodiscard]] bool
    inDeadZone(const Point3F& pos, Flt32* r = nullptr, Flt32* e = nullptr) const noexcept;
    [[nodiscard]] bool inDeadZone(Flt32 desRot, Flt32 desEle) const noexcept;
    [[nodiscard]] Point3F leadPosition(GameBase* target, bool useProjSpeed = true) const noexcept;
    [[nodiscard]] Flt32 wrapRotation(Flt32 rot) const noexcept;
    [[nodiscard]] Flt32 wrapElevation(Flt32 elev) const noexcept;
    [[nodiscard]] bool isTargetable(GameBase* target);

    void chooseTarget();
    bool trackAndFire(Flt32 interval);
    void extend();
    void retract();
    void shoot();
    void unshoot();

    [[nodiscard]] const TurretData* turretData() const noexcept {
        return turretData_;
    }

    [[nodiscard]] Flt32 turretRotation() const noexcept {
        return turretRotation_;
    }

    void setTurretRotation(Flt32 r) noexcept {
        turretRotation_ = r;
        setMaskBits(TRotationMask);
    }

    [[nodiscard]] Flt32 turretElevation() const noexcept {
        return turretElevation_;
    }

    void setTurretElevation(Flt32 e) noexcept {
        turretElevation_ = e;
        setMaskBits(ElevationMask);
    }

    [[nodiscard]] Flt32 minElevation() const noexcept {
        return minElevation_;
    }

    [[nodiscard]] Flt32 maxElevation() const noexcept {
        return maxElevation_;
    }

    void setElevationLimits(Flt32 minEl, Flt32 maxEl) noexcept {
        minElevation_ = minEl;
        maxElevation_ = maxEl;
    }

    [[nodiscard]] Flt32 minRotation() const noexcept {
        return minRotation_;
    }

    [[nodiscard]] Flt32 maxRotation() const noexcept {
        return maxRotation_;
    }

    void setRotationLimits(Flt32 minRot, Flt32 maxRot) noexcept {
        minRotation_ = minRot;
        maxRotation_ = maxRot;
    }

    [[nodiscard]] State turretState() const noexcept {
        return state_;
    }

    void setTurretState(State s) noexcept {
        setAnimationState(s);
        setMaskBits(StateMask);
    }

    [[nodiscard]] FireState fireState() const noexcept {
        return m_fireState_;
    }

    void setFireState(FireState fs) noexcept {
        m_fireState_ = fs;
    }

    [[nodiscard]] Int32 fireCount() const noexcept {
        return fireCount_;
    }

    void setFireCount(Int32 fc) noexcept {
        fireCount_ = fc & 0x3;
        setMaskBits(ShootingMask);
    }

    [[nodiscard]] GameBase* currentTarget() const noexcept {
        return currentTarget_;
    }

    void setCurrentTarget(GameBase* t);

    bool setPrimaryTarget(GameBase* target) {
        setCurrentTarget(target);
        return true;
    }

    [[nodiscard]] Projectile* currentProjectile() const noexcept {
        return m_pCurrentProjectile_;
    }

    [[nodiscard]] std::string_view persistClassName() const noexcept {
        return "Turret";
    }

    [[nodiscard]] UInt32 persistTag() const noexcept {
        return FearTurretPersTag;
    }

    [[nodiscard]] UInt16 getGhostTag() const override {
        return static_cast<UInt16>(FearTurretPersTag);
    }

protected:
    [[nodiscard]] std::optional<world::TsShapePose> turretPose() const;
    [[nodiscard]] bool hasTargetLOS(const GameBase* target) const;
    void setAnimationState(State state);
    void advanceAnimation(Flt32 interval);
    void playFireAnimation();
    void serverProcessWaiting(Flt32 in_deltaT, UInt32 in_currTime);
    void serverProcessReloading(Flt32 in_deltaT, UInt32 in_currTime);
    void serverProcessFiring(Flt32 in_deltaT, UInt32 in_currTime);
    bool onSimLightQuery(sim::SimLightQuery& query) const;

    TurretData* turretData_{nullptr};
    ProjectileData* projData_{nullptr};
    State state_{RETRACTED};
    FireState m_fireState_{Waiting};
    Flt32 m_waitScale_{0.0f};
    Int32 m_lastSustainedUpdate_{0};
    Projectile* m_pCurrentProjectile_{nullptr};

    Flt32 turretRotation_{3.14159265f};
    Flt32 turretElevation_{0.0f};
    Flt32 minElevation_{-0.785398163f};
    Flt32 maxElevation_{0.785398163f};
    Flt32 minRotation_{0.0f};
    Flt32 maxRotation_{0.0f};
    Flt32 desiredRotation_{0.0f};
    Flt32 desiredElevation_{0.0f};
    Flt32 rotationStep_{0.0f};
    Flt32 elevationStep_{0.0f};
    Flt32 interpTime_{0.0f};

    Int32 fireCount_{0};
    Int32 ghostFireCount_{0};
    bool shooting_{false};
    bool hasPrimary_{false};
    bool hasSecondary_{false};
    bool flipBarrel_{false};
    bool canShift_{true};
    bool isSustained_{false};

    UInt32 fireSound_{0};
    UInt32 moveSound_{0};

    Int32 primaryNode_{-1};
    Int32 secondaryNode_{-1};
    Int32 cameraNode_{-1};
    Int32 eyeNode_{-1};
    Int32 gunNode_{-1};

    std::optional<UInt32> animationThread_{};
    Int32 powerSequence_{-1};
    Int32 fireSequence_{-1};

    Point3F gunPos_{0.0f, 0.0f, 0.0f};
    GameBase* currentTarget_{nullptr};
    mutable TMat3F cachedEyeTransform_{};
};

bool registerTurretClass(Persistent::ClassRegistry& registry);
bool registerTurretNetwork(sim::network::Registry& registry);

} // namespace opent1::game
