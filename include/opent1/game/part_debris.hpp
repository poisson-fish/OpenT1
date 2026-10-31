#pragma once

#include <opent1/game/debris.hpp>

#include <memory>
#include <span>
#include <vector>

namespace opent1::game {

struct DebrisPartInfo {
    Int32 id{-1};
    Flt32 mass{250.0f};
    Flt32 radius{1.0f};
    Flt32 p{1.0f};
    Flt32 detailScaleMin{0.0f};
    Flt32* shapeDetailScale{nullptr};
};

struct PartDebrisPiece {
    Box3F bounds{Point3F{-0.5f, -0.5f, -0.5f}, Point3F{0.5f, 0.5f, 0.5f}};
    TMat3F rootDeltaTransform{};
    Int32 objectCount{1};
    std::shared_ptr<world::TsShapeInstance> shapeInstance{};
};

class PartDebrisCloud;

class PartDebris : public Debris {
    using Parent = Debris;
    friend class PartDebrisCloud;

public:
    PartDebris(const PartDebrisPiece& part, Int32 id, Flt32 spawnTO);
    PartDebris(const PartDebrisPiece& part,
               std::span<const DebrisPartInfo> chooserTable,
               Flt32 spawnTO);
    ~PartDebris() override = default;

    void initShape();
    bool onSimMessageEvent(const sim::SimMessageEvent& event) override;

    [[nodiscard]] const PartDebrisPiece& piece() const noexcept {
        return piece_;
    }

    [[nodiscard]] std::span<const DebrisPartInfo> chooser() const noexcept {
        return chooser_;
    }

private:
    PartDebrisPiece piece_{};
    std::vector<DebrisPartInfo> chooser_{};
};

class PartDebrisCloud : public DebrisCloud {
    using Parent = DebrisCloud;

public:
    explicit PartDebrisCloud(std::vector<std::unique_ptr<PartDebris>> list);

    PartDebrisCloud(std::shared_ptr<world::TsShapeInstance> shape,
                    const TMat3F& transform,
                    const Point3F& pos,
                    Int32 id,
                    const Point3F& vel,
                    Flt32 outJ,
                    const Point3F& inJ,
                    Flt32 spawnTO);

    PartDebrisCloud(std::shared_ptr<world::TsShapeInstance> shape,
                    const TMat3F& transform,
                    const Point3F& pos,
                    std::span<const DebrisPartInfo> chooser,
                    const Point3F& vel,
                    Flt32 outJ,
                    const Point3F& inJ,
                    Flt32 spawnTO);

    ~PartDebrisCloud() override = default;

    void setBreakProbability(Flt32 p) noexcept;

    [[nodiscard]] bool usesBreakProbability() const noexcept {
        return useBreakP_;
    }

    [[nodiscard]] Flt32 breakProbability() const noexcept {
        return breakP_;
    }

    void setSyntheticPieces(std::vector<PartDebrisPiece> pieces) {
        syntheticPieces_ = std::move(pieces);
    }

    bool onAdd() override;
    void generateDebris();

    [[nodiscard]] static std::vector<PartDebrisPiece>
    breakShapeIntoParts(const std::shared_ptr<world::TsShapeInstance>& shapeInst,
                        bool useBreakP = false,
                        Flt32 breakP = 1.0f);

private:
    void addDebrisList(std::vector<std::unique_ptr<PartDebris>>& list);

    TMat3F transform_{};
    std::shared_ptr<world::TsShapeInstance> cannibalize_{};
    Int32 debrisId_{-1};
    std::vector<DebrisPartInfo> chooser_{};
    std::vector<std::unique_ptr<PartDebris>> initialList_{};
    std::vector<PartDebrisPiece> syntheticPieces_{};
    bool useBreakP_{false};
    Flt32 breakP_{1.0f};
};

} // namespace opent1::game
