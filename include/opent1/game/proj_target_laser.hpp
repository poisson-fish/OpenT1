#pragma once

#include <opent1/game/projectile.hpp>

namespace opent1::game {

class TargetLaser : public Projectile {
    using Parent = Projectile;

public:
    using TargetLaserData = opent1::game::TargetLaserData;

    enum TargetLaserNetMaskBits : UInt32 {
        EndPointMask = 1u << 1,
        TeamMask = 1u << 2,
    };

    explicit TargetLaser(Int32 in_datFileId);
    TargetLaser();
    ~TargetLaser() override = default;

    [[nodiscard]] Int32 getDatGroup() const noexcept override {
        return DataBlockManager::TargetLaserDataType;
    }

    bool initResources(GameBaseData* io_pDat) override;
    bool onAdd() override;
    void onRemove() override;
    bool processQuery(sim::SimQuery& query) override;

    void clientProcess(UInt32 in_currTime) override;
    void serverProcess(UInt32 in_currTime) override;

    void initProjectile(const TMat3F& in_rTrans,
                        const Point3F& in_rShooterVel,
                        sim::SimObjectId in_shooterId) override;
    [[nodiscard]] bool isSustained() const noexcept override;
    void updateImageTransform(const TMat3F& in_rTrans) override;
    void shutOffProjectile() override;

    bool getTarget(Point3F* out_pTarget, Int32* out_pTeam) const noexcept override;

    UInt32
    packUpdate(sim::network::GhostManager& gm, UInt32 mask, BitStream& stream) const override;
    bool unpackUpdate(sim::network::GhostManager& gm, BitStream& stream) override;

    [[nodiscard]] const TargetLaserData* laserData() const noexcept {
        return m_pLaserData_;
    }

    [[nodiscard]] const Point3F& startPoint() const noexcept {
        return m_startPoint_;
    }

    [[nodiscard]] const Point3F& endPoint() const noexcept {
        return m_endPoint_;
    }

    [[nodiscard]] bool hasTarget() const noexcept {
        return m_hasTarget_;
    }

    [[nodiscard]] Int32 clientId() const noexcept {
        return m_clientId_;
    }

    void setClientId(Int32 id) noexcept {
        m_clientId_ = id;
    }

    void
    setBeamState(const Point3F& start, const Point3F& end, bool hasTarg, Int32 teamId) noexcept {
        m_startPoint_ = start;
        m_endPoint_ = end;
        m_hasTarget_ = hasTarg;
        setTeam(teamId);
    }

    [[nodiscard]] std::string_view persistClassName() const noexcept {
        return "TargetLaser";
    }

    [[nodiscard]] UInt32 persistTag() const noexcept {
        return TargetLaserPersTag;
    }

    [[nodiscard]] UInt16 getGhostTag() const override {
        return static_cast<UInt16>(TargetLaserPersTag);
    }

protected:
    bool readInitialPacket(sim::network::GhostManager* io_pGM, BitStream& io_pStream) override;
    void writeInitialPacket(sim::network::GhostManager* io_pGM,
                            BitStream& io_pStream) const override;
    bool readExplosion(BitStream& io_pStream) override;
    void writeExplosion(BitStream& io_pStream) const override;
    void loadResources(bool in_isGhost) override;
    bool onSimLightQuery(sim::SimLightQuery& query) const;
    Box3F buildContainerBox(const TMat3F& in_rMat) const override;

    TargetLaserData* m_pLaserData_{nullptr};
    bool m_hasTarget_{false};
    Int32 m_clientId_{2048};
    UInt32 m_spawnTime_{0};
    Resource<gfx::Bitmap> m_beamBitmap_{};
    Point3F m_startPoint_{0.0f, 0.0f, 0.0f};
    Point3F m_endPoint_{0.0f, 0.0f, 0.0f};
    mutable sim::SimLight m_endLight_{};
};

} // namespace opent1::game
