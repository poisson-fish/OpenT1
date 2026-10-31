#pragma once

#include <opent1/game/proj_attach.hpp>
#include <opent1/game/projectile.hpp>

namespace opent1::game {

class SeekingMissile : public Projectile {
    using Parent = Projectile;

public:
    using SeekingMissileData = opent1::game::SeekingMissileData;

    explicit SeekingMissile(Int32 in_datFileId);
    SeekingMissile();
    ~SeekingMissile() override = default;

    [[nodiscard]] Int32 getDatGroup() const noexcept override {
        return DataBlockManager::SeekingMissileDataType;
    }

    bool initResources(GameBaseData* io_pDat) override;
    bool onAdd() override;
    void onDeleteNotify(sim::SimObject& io_pDelete) override;
    void buildContainerBox(const TMat3F& in_rMat, Box3F* out_pBox);
    Box3F buildContainerBox(const TMat3F& in_rMat) const override;

    void clientProcess(UInt32 in_currTime) override;
    void serverProcess(UInt32 in_currTime) override;

    [[nodiscard]] bool isTargetable() const noexcept override;
    void setTarget(ShapeBase* io_pTarget) override;
    [[nodiscard]] Flt32 getTargetRange() const noexcept override;

    [[nodiscard]] std::string_view getScriptDefaultClass() const noexcept override {
        return "SeekingMissile";
    }

    UInt32
    packUpdate(sim::network::GhostManager& gm, UInt32 mask, BitStream& stream) const override;
    bool unpackUpdate(sim::network::GhostManager& gm, BitStream& stream) override;

    [[nodiscard]] const SeekingMissileData* missileData() const noexcept {
        return m_pMissileData_;
    }

    [[nodiscard]] ShapeBase* target() const noexcept {
        return m_pTarget_;
    }

    [[nodiscard]] Int32 explicitTargetGhostIndex() const noexcept {
        return m_explicitTargetGhostIndex_;
    }

    void setExplicitTargetGhostIndex(Int32 idx) noexcept {
        m_explicitTargetGhostIndex_ = idx;
    }

    [[nodiscard]] Int32
    targetGhostIndex(const sim::network::GhostManager* gm = nullptr) const noexcept;
    void updateTrailPercentage();

    [[nodiscard]] Flt32 currentTurnPercentage() const noexcept {
        return m_currTrailPercentage_;
    }

    [[nodiscard]] bool isTimeoutExplosion() const noexcept {
        return m_timeoutExplosion_;
    }

    void setTimeoutExplosion(bool timeout) noexcept {
        m_timeoutExplosion_ = timeout;
    }

    [[nodiscard]] std::string_view persistClassName() const noexcept {
        return "SeekingMissile";
    }

    [[nodiscard]] UInt32 persistTag() const noexcept {
        return SeekingMissilePersTag;
    }

    [[nodiscard]] UInt16 getGhostTag() const override {
        return static_cast<UInt16>(SeekingMissilePersTag);
    }

protected:
    static inline constexpr UInt32 csm_puffInterval = 50;
    static inline constexpr Flt32 csm_maxTurnAngleCos = 0.8660254f;

    void loadResources(bool in_isGhost) override;
    void killProjectile() override;
    void destroyProjectile() override;
    void spawnImpactEffect(sim::SimCollisionInfo& io_rCollision) override;

    void spawnImpactEffect(const Point3F& in_rPosition) override {
        Parent::spawnImpactEffect(in_rPosition);
    }

    bool readInitialPacket(sim::network::GhostManager* io_pGM, BitStream& io_pStream) override;
    void writeInitialPacket(sim::network::GhostManager* io_pGM,
                            BitStream& io_pStream) const override;
    bool readExplosion(BitStream& io_pStream) override;
    void writeExplosion(BitStream& io_pStream) const override;

    void predictForward(UInt32 in_currTime,
                        UInt32 in_predictTime,
                        Point3F& out_rPosition,
                        Point3F& out_rVelocity);

    UInt32 m_lastPuff_{0};
    UInt32 m_lastTurn_{0};
    bool m_lockLost_{false};
    Flt32 m_angularVelocity_{0.0f};
    Point3F m_lastPuffPosition_{};
    Flt32 m_currTrailPercentage_{0.0f};
    bool m_timeoutExplosion_{false};
    ShapeBase* m_pTarget_{nullptr};
    Int32 m_explicitTargetGhostIndex_{-1};
    std::unique_ptr<SimpleSwooshTrail> m_pTrail_;
    SeekingMissileData* m_pMissileData_{nullptr};
};

} // namespace opent1::game
