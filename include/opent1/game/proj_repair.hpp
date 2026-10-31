#pragma once

#include <opent1/game/proj_lightning.hpp>

namespace opent1::game {

class RepairEffect : public Projectile {
    using Parent = Projectile;

public:
    using RepairEffectData = opent1::game::RepairEffectData;

    enum RepairNetMaskBits : UInt32 {
        TargetMask = Projectile::NextFreeMask,
    };

    static inline constexpr UInt32 csm_SubdivisionMenuSize = Lightning::csm_SubdivisionMenuSize;
    using LineSegment = Lightning::LineSegment;
    using Line = Lightning::Line;

    explicit RepairEffect(Int32 in_datFileId);
    RepairEffect();
    ~RepairEffect() override = default;

    [[nodiscard]] Int32 getDatGroup() const noexcept override {
        return DataBlockManager::RepairEffectDataType;
    }

    bool initResources(GameBaseData* io_pDat) override;
    bool onAdd() override;
    bool processQuery(sim::SimQuery& query) override;
    void onDeleteNotify(sim::SimObject& io_pDelete) override;

    void clientProcess(UInt32 in_currTime) override;
    void serverProcess(UInt32 in_currTime) override;

    void initProjectile(const TMat3F& in_rTrans,
                        const Point3F& in_rShooterVel,
                        sim::SimObjectId in_shooterId) override;
    [[nodiscard]] bool isSustained() const noexcept override;
    void updateImageTransform(const TMat3F& in_rTrans) override;
    void shutOffProjectile() override;

    UInt32
    packUpdate(sim::network::GhostManager& gm, UInt32 mask, BitStream& stream) const override;
    bool unpackUpdate(sim::network::GhostManager& gm, BitStream& stream) override;

    [[nodiscard]] const RepairEffectData* repairData() const noexcept {
        return m_pRepairData_;
    }

    [[nodiscard]] ShapeBase* currentTarget() const noexcept {
        return m_pCurrentTarget_;
    }

    [[nodiscard]] Int32 explicitTargetGhostIndex() const noexcept {
        return m_explicitTargetGhostIndex_;
    }

    void setExplicitTargetGhostIndex(Int32 idx) noexcept {
        m_explicitTargetGhostIndex_ = idx;
    }

    [[nodiscard]] Int32
    targetGhostIndex(const sim::network::GhostManager* gm = nullptr) const noexcept;

    [[nodiscard]] const Point3F& startPoint() const noexcept {
        return m_startPoint_;
    }

    [[nodiscard]] const Point3F& endPoint() const noexcept {
        return m_endPoint_;
    }

    [[nodiscard]] const Point3F& targetOffset() const noexcept {
        return m_targetOffset_;
    }

    void setEndPoint(const Point3F& pt) noexcept {
        m_endPoint_ = pt;
    }

    [[nodiscard]] const Line& line() const noexcept {
        return m_line_;
    }

    void createSegments(UInt32 in_currTime,
                        const Point3F& startPt,
                        const Point3F& endPt,
                        const RMat3F& xform);

    [[nodiscard]] std::string_view persistClassName() const noexcept {
        return "RepairEffect";
    }

    [[nodiscard]] UInt32 persistTag() const noexcept {
        return RepairEffectPersTag;
    }

    [[nodiscard]] UInt16 getGhostTag() const override {
        return static_cast<UInt16>(RepairEffectPersTag);
    }

protected:
    bool readInitialPacket(sim::network::GhostManager* io_pGM, BitStream& io_pStream) override;
    void writeInitialPacket(sim::network::GhostManager* io_pGM,
                            BitStream& io_pStream) const override;
    bool readExplosion(BitStream& io_pStream) override;
    void writeExplosion(BitStream& io_pStream) const override;
    void loadResources(bool in_isGhost) override;
    Box3F buildContainerBox(const TMat3F& in_rMat) const override;
    void determineTarget();

    RepairEffectData* m_pRepairData_{nullptr};
    UInt32 m_lastUpdatedClient_{0};
    UInt32 m_lastTargetTime_{0};
    bool m_targetEvaluated_{false};
    Point3F m_startPoint_{0.0f, 0.0f, 0.0f};
    Point3F m_endPoint_{0.0f, 0.0f, 0.0f};
    Point3F m_targetOffset_{0.0f, 0.0f, 0.0f};
    ShapeBase* m_pCurrentTarget_{nullptr};
    Int32 m_explicitTargetGhostIndex_{-1};
    Line m_line_{};
    Resource<gfx::Bitmap> m_beamBitmap_{};
};

} // namespace opent1::game
