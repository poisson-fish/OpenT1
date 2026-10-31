#pragma once

#include <opent1/game/projectile.hpp>

#include <memory>

namespace opent1::game {

class Grenade : public Projectile {
    using Parent = Projectile;

public:
    using GrenadeData = opent1::game::GrenadeData;

    explicit Grenade(Int32 in_datFileId);
    Grenade();
    ~Grenade() override = default;

    [[nodiscard]] Int32 getDatGroup() const noexcept override {
        return DataBlockManager::GrenadeDataType;
    }

    bool initResources(GameBaseData* io_pDat) override;
    bool onAdd() override;
    void buildContainerBox(const TMat3F& in_rMat, Box3F* out_pBox);
    Box3F buildContainerBox(const TMat3F& in_rMat) const override;

    void clientProcess(UInt32 in_currTime) override;
    void serverProcess(UInt32 in_currTime) override;

    UInt32
    packUpdate(sim::network::GhostManager& gm, UInt32 mask, BitStream& stream) const override;
    bool unpackUpdate(sim::network::GhostManager& gm, BitStream& stream) override;

    [[nodiscard]] const GrenadeData* grenadeData() const noexcept {
        return m_pGrenadeData_;
    }

    [[nodiscard]] bool hasBounced() const noexcept {
        return m_hasBounced_;
    }

    void setHasBounced(bool bounced) noexcept {
        m_hasBounced_ = bounced;
    }

    [[nodiscard]] Int32 lastSurfaceType() const noexcept {
        return m_lastSurfaceType_;
    }

    void setLastSurfaceType(Int32 surfaceType) noexcept {
        m_lastSurfaceType_ = surfaceType;
    }

    static inline constexpr UInt32 FGCollisionSoundMask = Projectile::NextFreeMask;
    static inline constexpr UInt32 NextFreeMask = Projectile::NextFreeMask << 1;

    [[nodiscard]] std::string_view persistClassName() const noexcept {
        return "Grenade";
    }

    [[nodiscard]] UInt32 persistTag() const noexcept {
        return GrenadePersTag;
    }

    [[nodiscard]] UInt16 getGhostTag() const override {
        return static_cast<UInt16>(GrenadePersTag);
    }

protected:
    void loadResources(bool in_isGhost) override;
    void killProjectile() override;
    void destroyProjectile() override;
    std::shared_ptr<sim::SimCollisionImage> getSelfImage() override;

    bool readInitialPacket(sim::network::GhostManager* io_pGM, BitStream& io_pStream) override;
    void writeInitialPacket(sim::network::GhostManager* io_pGM,
                            BitStream& io_pStream) const override;
    bool readExplosion(BitStream& io_pStream) override;
    void writeExplosion(BitStream& io_pStream) const override;

    void predictForward(UInt32 in_currTime,
                        UInt32 in_predictTime,
                        Point3F& out_rPosition,
                        Point3F& out_rVelocity);
    bool advanceGrenade(UInt32 currTime, bool collisions);
    void createExplosionEffect();
    void playBounceSound(Int32 surfaceType);

    UInt32 m_lastPuff_{0};
    UInt32 m_puffInterval_{100};
    GrenadeData* m_pGrenadeData_{nullptr};
    std::shared_ptr<sim::SimCollisionSphereImage> m_sphereCollImage_;
    bool m_hasBounced_{false};
    bool m_pendingExplosion_{false};
    Int32 m_lastSurfaceType_{0};
};

} // namespace opent1::game
