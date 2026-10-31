#pragma once

#include <opent1/game/projectile.hpp>

namespace opent1::game {

class LaserProjectile : public Projectile {
    using Parent = Projectile;

public:
    using LaserData = opent1::game::LaserData;

    explicit LaserProjectile(Int32 in_datFileId);
    LaserProjectile();
    ~LaserProjectile() override = default;

    [[nodiscard]] Int32 getDatGroup() const noexcept override {
        return DataBlockManager::LaserDataType;
    }

    bool initResources(GameBaseData* io_pDat) override;
    bool onAdd() override;
    void onRemove() override;
    bool processQuery(sim::SimQuery& query) override;

    void clientProcess(UInt32 in_currTime) override;
    void serverProcess(UInt32 in_currTime) override;

    void setEnergy(Flt32 in_energy, Flt32 in_pOfM) override;
    void initProjectile(const TMat3F& in_rTrans,
                        const Point3F& in_rShooterVel,
                        sim::SimObjectId in_shooterId) override;

    UInt32
    packUpdate(sim::network::GhostManager& gm, UInt32 mask, BitStream& stream) const override;
    bool unpackUpdate(sim::network::GhostManager& gm, BitStream& stream) override;

    [[nodiscard]] const LaserData* laserData() const noexcept {
        return m_pLaserData_;
    }

    [[nodiscard]] const Point3F& startPoint() const noexcept {
        return m_startPoint_;
    }

    [[nodiscard]] const Point3F& endPoint() const noexcept {
        return m_endPoint_;
    }

    [[nodiscard]] Flt32 startIntensity() const noexcept {
        return m_startIntensity_;
    }

    [[nodiscard]] Flt32 finalDamage() const noexcept {
        return m_finalDamage_;
    }

    [[nodiscard]] bool hitSomething() const noexcept {
        return m_hitSomething_;
    }

    void
    setBeamEndpoints(const Point3F& start, const Point3F& end, bool hitSomething = false) noexcept {
        m_startPoint_ = start;
        m_endPoint_ = end;
        m_hitSomething_ = hitSomething;
    }

    [[nodiscard]] std::string_view persistClassName() const noexcept {
        return "LaserProjectile";
    }

    [[nodiscard]] UInt32 persistTag() const noexcept {
        return LaserPersTag;
    }

    [[nodiscard]] UInt16 getGhostTag() const override {
        return static_cast<UInt16>(LaserPersTag);
    }

protected:
    bool readInitialPacket(sim::network::GhostManager* io_pGM, BitStream& io_pStream) override;
    void writeInitialPacket(sim::network::GhostManager* io_pGM,
                            BitStream& io_pStream) const override;
    bool readExplosion(BitStream& io_pStream) override;
    void writeExplosion(BitStream& io_pStream) const override;
    void loadResources(bool in_isGhost) override;
    bool onSimLightQuery(sim::SimLightQuery& query) const;
    bool readjustEnd();
    void updateHitImage();
    Box3F buildContainerBox(const TMat3F& in_rMat) const override;

    LaserData* m_pLaserData_{nullptr};
    bool m_hitSomething_{false};
    UInt32 m_spawnTime_{0};
    Flt32 m_finalDamage_{0.0f};
    Flt32 m_startIntensity_{1.0f};
    Point3F m_startPoint_{0.0f, 0.0f, 0.0f};
    Point3F m_endPoint_{0.0f, 0.0f, 0.0f};
    mutable sim::SimLight m_endLight_{};
    Resource<gfx::Bitmap> m_beamBitmap_{};
    std::optional<world::TsShapeResources> m_hitResources_{};
    sim::SimAddress m_hitImage_{};
};

} // namespace opent1::game
