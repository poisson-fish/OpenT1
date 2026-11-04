#pragma once

#include <opent1/game/projectile.hpp>

#include <array>

namespace opent1::game {

class Lightning : public Projectile {
    using Parent = Projectile;

public:
    using LightningData = opent1::game::LightningData;

    enum LightningNetMaskBits : UInt32 {
        TargetMask = Projectile::NextFreeMask,
        TargetAndPositionMask = TargetMask,
    };

    static inline constexpr UInt32 csm_SubdivisionMenuSize = 32;
    static inline constexpr UInt32 csm_numLines = 2;

    struct LineSegment {
        UInt32 startIndex{0};
        UInt32 endIndex{0};
        Point3F startPoint{0.0f, 0.0f, 0.0f};
        Point3F endPoint{0.0f, 0.0f, 0.0f};
    };

    struct Line {
        UInt32 createTime{0};
        UInt32 expireTime{0};
        std::array<Point3F, csm_SubdivisionMenuSize + 1> points{};
        std::array<LineSegment, csm_SubdivisionMenuSize> segs{};
    };

    explicit Lightning(Int32 in_datFileId);
    Lightning();
    ~Lightning() override = default;

    [[nodiscard]] Int32 getDatGroup() const noexcept override {
        return DataBlockManager::LightningDataType;
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

    [[nodiscard]] const LightningData* lightningData() const noexcept {
        return m_pLightningData_;
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

    [[nodiscard]] const std::array<Line, csm_numLines>& lines() const noexcept {
        return m_lines_;
    }

    void createSegments(UInt32 in_lineIndex,
                        UInt32 in_currTime,
                        const Point3F& startPt,
                        const Point3F& endPt,
                        const RMat3F& xform);

    static void resetRandomSeed(UInt32 seed = Random::defaultSeed) noexcept;

    [[nodiscard]] std::string_view persistClassName() const noexcept {
        return "Lightning";
    }

    [[nodiscard]] UInt32 persistTag() const noexcept {
        return LightningPersTag;
    }

    [[nodiscard]] UInt16 getGhostTag() const override {
        return static_cast<UInt16>(LightningPersTag);
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

    LightningData* m_pLightningData_{nullptr};
    UInt32 m_lastUpdatedClient_{0};
    UInt32 m_lastChargeTime_{0};
    UInt32 m_lastTargetTime_{0};
    bool m_targetEvaluated_{false};
    Point3F m_startPoint_{0.0f, 0.0f, 0.0f};
    Point3F m_endPoint_{0.0f, 0.0f, 0.0f};
    Point3F m_targetOffset_{0.0f, 0.0f, 0.0f};
    ShapeBase* m_pCurrentTarget_{nullptr};
    Int32 m_explicitTargetGhostIndex_{-1};
    std::array<Line, csm_numLines> m_lines_{};
    Resource<gfx::Bitmap> m_beamBitmap_{};
};

} // namespace opent1::game
