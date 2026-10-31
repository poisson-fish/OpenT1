#pragma once

#include <opent1/game/proj_attach.hpp>
#include <opent1/game/projectile.hpp>

#include <memory>

namespace opent1::game {

class RocketDumb : public Projectile {
    using Parent = Projectile;

public:
    using RocketData = opent1::game::RocketData;

    explicit RocketDumb(Int32 in_datFileId);
    RocketDumb();
    ~RocketDumb() override = default;

    [[nodiscard]] Int32 getDatGroup() const noexcept override {
        return DataBlockManager::RocketDataType;
    }

    bool initResources(GameBaseData* io_pDat) override;
    bool onAdd() override;
    void onRemove() override;
    void buildContainerBox(const TMat3F& in_rMat, Box3F* out_pBox);
    Box3F buildContainerBox(const TMat3F& in_rMat) const override;

    void clientProcess(UInt32 in_currTime) override;
    void serverProcess(UInt32 in_currTime) override;

    UInt32
    packUpdate(sim::network::GhostManager& gm, UInt32 mask, BitStream& stream) const override;
    bool unpackUpdate(sim::network::GhostManager& gm, BitStream& stream) override;

    [[nodiscard]] const RocketData* rocketData() const noexcept {
        return m_pRocketData_;
    }

    [[nodiscard]] const Point3F& spawnPosition() const noexcept {
        return m_spawnPosition_;
    }

    [[nodiscard]] const Point3F& spawnVelocity() const noexcept {
        return m_spawnVelocity_;
    }

    [[nodiscard]] Flt32 spawnVelocityLen() const noexcept {
        return m_spawnVelocityLen_;
    }

    [[nodiscard]] bool isTimeoutExplosion() const noexcept {
        return m_timeoutExplosion_;
    }

    void setTimeoutExplosion(bool timeout) noexcept {
        m_timeoutExplosion_ = timeout;
    }

    void setSpawnState(const Point3F& pos, const Point3F& vel) noexcept;
    void quantizeServerVelocity() noexcept;
    void predictForward(UInt32 in_currTime,
                        UInt32 in_predictTime,
                        Point3F& out_rPosition,
                        Point3F& out_rVelocity);

    [[nodiscard]] std::string_view persistClassName() const noexcept {
        return "RocketDumb";
    }

    [[nodiscard]] UInt32 persistTag() const noexcept {
        return RocketPersTag;
    }

    [[nodiscard]] UInt16 getGhostTag() const override {
        return static_cast<UInt16>(RocketPersTag);
    }

protected:
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

    static inline constexpr Int32 csm_normalBits = 10;

    RocketData* m_pRocketData_{nullptr};
    bool m_timeoutExplosion_{false};
    Point3F m_accelVector_{0.0f, 0.0f, 0.0f};
    std::shared_ptr<SimpleSwooshTrail> m_pTrail_;
    sim::SimObjectId m_trailObjectId_{0};

    Point3F m_spawnPosition_{0.0f, 0.0f, 0.0f};
    Point3F m_spawnVelocity_{0.0f, 0.0f, 0.0f};
    Flt32 m_spawnVelocityLen_{0.0f};
    UInt32 m_spawnTime_{0};
    Point3F m_lastPuffPosition_{0.0f, 0.0f, 0.0f};
    void updateMotor();
    void updateTrail();
};

} // namespace opent1::game
