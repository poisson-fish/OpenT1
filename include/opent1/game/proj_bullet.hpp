#pragma once

#include <opent1/game/projectile.hpp>

#include <memory>

namespace opent1::game {

class Bullet : public Projectile {
    using Parent = Projectile;

public:
    using BulletData = opent1::game::BulletData;

    struct TracerRenderImage : public FearRenderImage {
        Point3F m_startPoint{0.0f, 0.0f, 0.0f};
        Point3F m_endPoint{0.0f, 0.0f, 0.0f};
        Flt32 m_factor{1.0f};
    };

    static inline constexpr Int32 csm_normalBits = 20;

    explicit Bullet(Int32 in_datFileId);
    Bullet();
    ~Bullet() override = default;

    [[nodiscard]] Int32 getDatGroup() const noexcept override {
        return DataBlockManager::BulletDataType;
    }

    bool onAdd() override;
    bool processQuery(sim::SimQuery& query) override;
    bool initResources(GameBaseData* io_pDat) override;

    void clientProcess(UInt32 in_currTime) override;
    void serverProcess(UInt32 in_currTime) override;

    UInt32
    packUpdate(sim::network::GhostManager& gm, UInt32 mask, BitStream& stream) const override;
    bool unpackUpdate(sim::network::GhostManager& gm, BitStream& stream) override;

    [[nodiscard]] const BulletData* bulletData() const noexcept {
        return m_pBulletData_;
    }

    [[nodiscard]] bool isTracer() const noexcept {
        return m_isTracer_;
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

    void setSpawnState(const Point3F& pos, const Point3F& vel, UInt32 spawnTimeMs = 0) noexcept;
    void quantizeServerVelocity() noexcept;
    void predictForward(UInt32 in_currTime,
                        UInt32 in_predictTime,
                        Point3F& out_rPosition) const noexcept;

    [[nodiscard]] std::string_view persistClassName() const noexcept {
        return "Bullet";
    }

    [[nodiscard]] UInt32 persistTag() const noexcept {
        return BulletPersTag;
    }

    [[nodiscard]] UInt16 getGhostTag() const override {
        return static_cast<UInt16>(BulletPersTag);
    }

protected:
    bool applyBaseDamage(sim::SimCollisionInfo& io_rCollision, sim::SimObject* io_pHit) override;
    void spawnImpactEffect(sim::SimCollisionInfo& io_rCollision) override;

    void spawnImpactEffect(const Point3F& in_rPosition) override {
        Parent::spawnImpactEffect(in_rPosition);
    }

    [[nodiscard]] Int32
    getBulletHoleIndex(const sim::SimCollisionInfo& io_rCollision) const noexcept override;

    bool readInitialPacket(sim::network::GhostManager* io_pGM, BitStream& io_pStream) override;
    void writeInitialPacket(sim::network::GhostManager* io_pGM,
                            BitStream& io_pStream) const override;
    bool readExplosion(BitStream& io_pStream) override;
    void writeExplosion(BitStream& io_pStream) const override;

    void setSearchBoundaries();
    bool onSimLightQuery(sim::SimLightQuery& query) const;
    bool onWorldSurfaceQuery(world::WorldSurfaceQuery& query);
    Box3F buildContainerBox(const TMat3F& in_rMat) const override;
    std::shared_ptr<sim::SimCollisionImage> getSelfImage() override;

private:
    TracerRenderImage m_tracerImage_{};
    Resource<gfx::Bitmap> m_tracerTexture_;
    bool m_isTracer_{false};
    bool m_resetInterpFrom_{true};
    UInt32 m_spawnTime_{0};
    Point3F m_spawnPosition_{0.0f, 0.0f, 0.0f};
    Point3F m_spawnVelocity_{0.0f, 0.0f, 0.0f};
    Flt32 m_spawnVelocityLen_{0.0f};
    UInt32 m_finalTime_{0};
    Point3F m_finalPosition_{0.0f, 0.0f, 0.0f};
    Point3F m_finalNormal_{0.0f, 0.0f, 1.0f};
    sim::SimObject* m_finalObject_{nullptr};
    sim::SimCollisionInfo storedInfo_{};
    std::shared_ptr<sim::SimCollisionSphereImage> m_sphereCollImage_;
    BulletData* m_pBulletData_{nullptr};
};

} // namespace opent1::game
