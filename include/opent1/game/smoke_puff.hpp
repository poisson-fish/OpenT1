#pragma once

#include <opent1/game/f_base.hpp>
#include <opent1/game/game_base.hpp>
#include <opent1/math/random.hpp>
#include <opent1/sim/sim_collision_movement.hpp>
#include <opent1/world/ts_shape.hpp>
#include <opent1/world/ts_shape_instance.hpp>

#include <array>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace opent1::game {

struct SmokePuff {
    std::string shapeName;
    Resource<world::TsShape> shape;
    std::shared_ptr<world::TsShapeInstance> shapeInstance;
    std::optional<UInt32> threadIndex;
    FearRenderImage renderImage{};
    Point3F position{0.0f, 0.0f, 0.0f};
    Point3F velocity{0.0f, 0.0f, 0.0f};
    Flt32 totalTime{1.0f};
    Flt32 spin{0.0f};
    UInt32 spawnTime{0};
    UInt32 expireTime{0};
    UInt32 lastUpdated{0};
    bool inUse{false};
};

class SmokeManager : public sim::SimContainer {
    using Parent = sim::SimContainer;

public:
    static constexpr std::size_t csm_maxPuffs = 256;

    SmokeManager();
    ~SmokeManager() override = default;

    bool onAdd() override;
    void onRemove() override;
    bool processQuery(sim::SimQuery& query) override;

    static SmokeManager* get(sim::SimManager* io_pManager) noexcept;

    void addPuff(const Resource<world::TsShape>& in_rShape,
                 Flt32 in_totalTime,
                 const Point3F& in_rPosition,
                 const Point3F& in_rVelocity,
                 UInt32 explicitTimeMs = 0);

    void addPuff(std::string_view in_shapeName,
                 Flt32 in_totalTime,
                 const Point3F& in_rPosition,
                 const Point3F& in_rVelocity,
                 UInt32 explicitTimeMs = 0);

    void removeAllInBounds(const Box3F& in_rBox) noexcept;
    void removeAllInRadius(const Point3F& in_rPosition, Flt32 in_radius) noexcept;
    void clientProcess(UInt32 in_currTime);

    [[nodiscard]] std::size_t activePuffCount() const noexcept;

    [[nodiscard]] const std::array<SmokePuff, csm_maxPuffs>& puffs() const noexcept {
        return puffs_;
    }

    void resetRandomSeed(UInt32 seed = Random::defaultSeed) noexcept {
        random_.setSeed(seed);
    }

private:
    SmokePuff* allocatePuff(UInt32 currTimeMs) noexcept;
    void updateBoundingBox() noexcept;

    std::array<SmokePuff, csm_maxPuffs> puffs_{};
    Random random_{};
    UInt32 currentTimeMs_{0};
};

} // namespace opent1::game
