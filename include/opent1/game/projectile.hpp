#pragma once

#include <opent1/game/data_block_manager.hpp>
#include <opent1/game/explosion.hpp>
#include <opent1/game/f_base.hpp>
#include <opent1/game/game_base.hpp>
#include <opent1/game/player_manager.hpp>
#include <opent1/game/shape_base.hpp>
#include <opent1/math/random.hpp>
#include <opent1/sim/sim_collision_movement.hpp>
#include <opent1/sim/sim_world_game.hpp>
#include <opent1/world/ts_shape.hpp>
#include <opent1/world/ts_shape_instance.hpp>

#include <memory>
#include <optional>
#include <string_view>

namespace opent1::game {

class Projectile : public GameBase {
    using Parent = GameBase;

public:
    using ProjectileData = opent1::game::ProjectileData;

    enum NetMaskBits : UInt32 {
        FPPositionMask = 1u << 0,
        FPVelocityMask = 1u << 1,
        FPExplosionMask = 1u << 2,
        NextFreeMask = 1u << 3,
        InitialUpdate = 1u << 31,
    };

    static inline constexpr UInt32 csm_damageMask = SimPlayerObjectType | StaticObjectType |
                                                    VehicleObjectType | MoveableObjectType |
                                                    MineObjectType;

    static inline constexpr UInt32 csm_collisionMask =
        SimPlayerObjectType | SimInteriorObjectType | SimTerrainObjectType | SimShapeObjectType |
        StaticObjectType | VehicleObjectType | MoveableObjectType | ItemObjectType | MineObjectType;

    explicit Projectile(Int32 in_datFileId = -1);
    ~Projectile() override;

    [[nodiscard]] Int32 getDatGroup() const noexcept override {
        return DataBlockManager::ProjectileDataType;
    }

    bool initResources(GameBaseData* io_pDat) override;
    bool processQuery(sim::SimQuery& query) override;
    bool onAdd() override;
    void onRemove() override;
    void onDeleteNotify(sim::SimObject& io_pDelete) override;

    void clientProcess(UInt32 in_currTime) override;
    void serverProcess(UInt32 in_currTime) override;

    UInt32
    packUpdate(sim::network::GhostManager& gm, UInt32 mask, BitStream& stream) const override;
    bool unpackUpdate(sim::network::GhostManager& gm, BitStream& stream) override;

    [[nodiscard]] Flt32 getInheritedVelScale() const noexcept;

    virtual void initProjectile(const TMat3F& in_rTrans,
                                const Point3F& in_rShooterVel,
                                sim::SimObjectId in_shooterId);
    virtual void setEnergy(Flt32 in_energy, Flt32 in_pOfM);
    [[nodiscard]] virtual bool isSustained() const noexcept;
    virtual void shutOffProjectile();
    virtual void updateImageTransform(const TMat3F& in_rTrans);
    [[nodiscard]] virtual bool isTargetable() const noexcept;
    [[nodiscard]] virtual Flt32 getTargetRange() const noexcept;
    virtual void setTarget(ShapeBase* io_pTarget);

    [[nodiscard]] const ProjectileData* projectileData() const noexcept {
        return m_projectileData_;
    }

    [[nodiscard]] ShapeBase* shooter() const noexcept {
        return m_pShooter_;
    }

    void setShooter(ShapeBase* s, Int32 explicitGhostIndex = -1) noexcept {
        m_pShooter_ = s;
        m_explicitShooterGhostIndex_ = explicitGhostIndex;
    }

    [[nodiscard]] Int32
    shooterGhostIndex(const sim::network::GhostManager* gm = nullptr) const noexcept;

    void setExplicitShooterGhostIndex(Int32 ghostIdx) noexcept {
        m_explicitShooterGhostIndex_ = ghostIdx;
    }

    [[nodiscard]] bool hasExploded() const noexcept {
        return m_hasExploded_;
    }

    [[nodiscard]] bool isProjectileLive() const noexcept {
        return m_projectileIsLive_;
    }

    [[nodiscard]] bool isWaitingForDelete() const noexcept {
        return m_waitingForDelete_;
    }

    [[nodiscard]] const Point3F& explosionPosition() const noexcept {
        return m_explosionPosition_;
    }

    [[nodiscard]] const Point3F& explosionAxis() const noexcept {
        return m_explosionAxis_;
    }

    [[nodiscard]] Int32 explosionId() const noexcept {
        return m_explosionId_;
    }

    void setExplosionState(const Point3F& pos,
                           const Point3F& axis,
                           Int32 id,
                           bool exploded = true) noexcept {
        m_explosionPosition_ = pos;
        m_explosionAxis_ = axis;
        m_explosionId_ = id;
        m_hasExploded_ = exploded;
        if (exploded) {
            setMaskBits(FPExplosionMask);
        }
    }

    static void resetDeflectionSeed(UInt32 seed = Random::defaultSeed) noexcept;
    static void resetExplosionTagSeed(UInt32 seed = Random::defaultSeed) noexcept;

protected:
    [[nodiscard]] bool isProjStatic(const sim::SimObject* pObj) const noexcept;
    virtual void loadResources(bool in_isGhost);

    virtual bool applyBaseDamage(sim::SimCollisionInfo& io_rCollision, sim::SimObject* io_pHit);
    virtual void applyRadiusDamage(const Point3F& io_rDetonationPoint);
    virtual void killProjectile();
    virtual void destroyProjectile();

    virtual bool readExplosion(BitStream& stream) = 0;
    virtual void writeExplosion(BitStream& stream) const = 0;

    virtual void spawnImpactEffect(sim::SimCollisionInfo& io_rCollision);
    virtual void spawnImpactEffect(const Point3F& in_rPosition);
    void deflectProjectile(Flt32 in_deflection);

    bool onSimLightQuery(sim::SimLightQuery& query) const;
    bool onSimCollisionImageQuery(sim::SimCollisionImageQuery& query) const;
    bool onWorldShapeQuery(world::WorldShapeQuery& query) const;
    Box3F buildContainerBox(const TMat3F& in_rMat) const override;

    void decayGraceTime(UInt32 in_decayStep) noexcept;
    bool moveProjectile(UInt32 in_currTime,
                        sim::SimCollisionInfo& out_rCollision,
                        sim::SimObject*& out_pObject,
                        bool in_doCollisions,
                        bool in_overrideTimeSlice);
    bool processCollision(MovementInfo& info) override;

    [[nodiscard]] virtual Int32
    getBulletHoleIndex(const sim::SimCollisionInfo& io_rCollision) const noexcept;
    [[nodiscard]] virtual Int32
    getExplosionTag(const sim::SimCollisionInfo& io_rCollision) const noexcept;
    [[nodiscard]] virtual Int32 getExplosionTag() const noexcept;

    virtual bool readInitialPacket(sim::network::GhostManager* io_pGM, BitStream& io_pStream);
    virtual void writeInitialPacket(sim::network::GhostManager* io_pGM,
                                    BitStream& io_pStream) const;

    void updateProjectileSound(const TMat3F& in_rTrans, const Point3F& in_rVel);
    void stopProjectileSound();
    void commitImpactEffect(bool clearSmoke);
    bool m_pendingImpactEffect_{false};
    [[nodiscard]] Int32 getDamageId() const noexcept;

    ProjectileData* m_projectileData_{nullptr};
    Resource<world::TsShape> m_bulletRes_{};
    std::shared_ptr<world::TsShapeInstance> m_translucentShape_{};
    std::shared_ptr<world::TsShapeInstance> m_nonTranslucentShape_{};
    std::optional<UInt32> m_translucentThread_{};
    std::optional<UInt32> m_nonTranslucentThread_{};
    FearRenderImage m_renderImage_{};
    bool m_renderFacingCamera_{false};
    UInt32 m_projectileSound_{0};
    Int32 m_ownerGraceMS_{0};
    UInt32 m_startInterp_{0};
    UInt32 m_endInterp_{0};
    Point3F m_interpFrom_{0.0f, 0.0f, 0.0f};
    Point3F m_interpTo_{0.0f, 0.0f, 0.0f};
    UInt32 m_lastUpdated_{0};
    UInt32 m_lastGhosted_{0};
    bool m_waitingForDelete_{false};
    UInt32 m_deleteTime_{0};
    bool m_projectileIsLive_{true};
    UInt32 m_liveTime_{0};
    UInt32 m_totalTime_{0};
    Int32 m_shooterClientId_{0};
    UInt32 m_shooterClientLoginId_{0};
    sim::SimObjectId m_shooterId_{static_cast<sim::SimObjectId>(-1)};
    ShapeBase* m_pShooter_{nullptr};
    Int32 m_explicitShooterGhostIndex_{-1};
    Point3F m_shooterVel_{0.0f, 0.0f, 0.0f};
    Point3F m_instTerminalVelocity_{0.0f, 0.0f, 0.0f};
    bool m_overrideRender_{false};
    mutable sim::SimLight m_light_{};
    bool m_testCollision_{false};
    bool m_hasExploded_{false};
    Point3F m_explosionPosition_{0.0f, 0.0f, 0.0f};
    Point3F m_explosionAxis_{0.0f, 0.0f, 1.0f};
    Int32 m_explosionId_{-1};
    bool m_faceForward_{true};
    Flt32 m_axisSpin_{0.0f};
    mutable std::optional<world::TsShapeResources> m_cachedWorldResources_{};
    mutable std::shared_ptr<world::TsShapeInstance> m_cachedWorldInstance_{};
};

} // namespace opent1::game
