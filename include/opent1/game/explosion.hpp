#pragma once

#include <opent1/game/data_block_manager.hpp>
#include <opent1/game/f_base.hpp>
#include <opent1/game/game_base.hpp>
#include <opent1/game/shape_base.hpp>
#include <opent1/math/random.hpp>
#include <opent1/sim/sim_world_game.hpp>
#include <opent1/world/ts_shape.hpp>
#include <opent1/world/ts_shape_instance.hpp>

#include <memory>
#include <optional>

namespace opent1::game {

class Explosion : public GameBase {
    using Parent = GameBase;

public:
    using ExplosionData = opent1::game::ExplosionData;

    static inline constexpr UInt32 DamageMask = SimPlayerObjectType | StaticObjectType |
                                                VehicleObjectType | MoveableObjectType |
                                                MineObjectType;

    static inline bool sm_useDynamicLighting = true;

    explicit Explosion(Int32 in_datFileId = -1);
    ~Explosion() override;

    void setPosition(const Point3F& in_rPosition);

    [[nodiscard]] const Point3F& position() const noexcept {
        return m_position_;
    }

    void setAxis(const Point3F& in_rAxis) noexcept;

    [[nodiscard]] const Point3F& axis() const noexcept {
        return m_axis_;
    }

    [[nodiscard]] Int32 getDatGroup() const noexcept override {
        return DataBlockManager::ExplosionDataType;
    }

    bool initResources(GameBaseData* io_pDat) override;
    bool onAdd() override;
    void onRemove() override;
    bool processQuery(sim::SimQuery& query) override;
    void clientProcess(UInt32 in_currTime) override;

    [[nodiscard]] UInt32 spawnTime() const noexcept {
        return m_spawnTime_;
    }

    [[nodiscard]] UInt32 expireTime() const noexcept {
        return m_expireTime_;
    }

    [[nodiscard]] Int32 timeZeroMS() const noexcept {
        return m_timeZeroMS_;
    }

    [[nodiscard]] Int32 timeOneMS() const noexcept {
        return m_timeOneMS_;
    }

    [[nodiscard]] Flt32 axisSpin() const noexcept {
        return m_axisSpin_;
    }

    [[nodiscard]] const ExplosionData* explosionData() const noexcept {
        return m_explosionData_;
    }

    void setExplicitTiming(UInt32 spawnTimeMs, Flt32 sequenceDurationSec = 1.0f) noexcept;
    [[nodiscard]] bool computeLightAtTime(UInt32 currTimeMs,
                                          sim::SimLight& outLight) const noexcept;
    [[nodiscard]] Point3F computeShiftedPosition(const Point3F& cameraAxis,
                                                 Flt32 cameraDistance) const noexcept;

    static void applyRadiusDamage(sim::SimManager* manager,
                                  Int32 type,
                                  const Point3F& pos,
                                  const Point3F& vec,
                                  Flt32 radius,
                                  Flt32 damageValue,
                                  Flt32 force,
                                  Int32 srcObjectId = -1);

    static void resetRandomSeed(UInt32 seed = Random::defaultSeed) noexcept;

    [[nodiscard]] std::string_view persistClassName() const noexcept {
        return "Explosion";
    }

    [[nodiscard]] UInt32 persistTag() const noexcept {
        return ExplosionPersTag;
    }

    [[nodiscard]] UInt16 getGhostTag() const override {
        return static_cast<UInt16>(ExplosionPersTag);
    }

protected:
    void loadResources();
    Box3F buildContainerBox(const TMat3F& in_rMat) const override;
    bool onSimLightQuery(sim::SimLightQuery& query) const;
    bool onWorldShapeQuery(world::WorldShapeQuery& query);

    ExplosionData* m_explosionData_{nullptr};
    Point3F m_axis_{0.0f, 0.0f, 0.0f};
    Point3F m_position_{0.0f, 0.0f, 0.0f};
    FearRenderImage m_renderImage_{};
    Resource<world::TsShape> m_hShape_{};
    std::optional<world::TsShapeResources> m_worldResources_{};
    std::shared_ptr<world::TsShapeInstance> m_shapeInstance_{};
    std::optional<UInt32> m_mainThread_{};
    std::optional<UInt32> m_visThread_{};
    Box3F m_bbox_{Point3F{-0.5f, -0.5f, -0.5f}, Point3F{0.5f, 0.5f, 0.5f}};
    Flt32 m_axisSpin_{0.0f};
    Flt32 m_sequenceDuration_{1.0f};
    UInt32 m_spawnTime_{0};
    UInt32 m_lastUpdated_{0};
    UInt32 m_expireTime_{0};
    Int32 m_timeZeroMS_{0};
    Int32 m_timeOneMS_{0};
    mutable sim::SimLight m_light_{};
};

bool registerExplosionClass(Persistent::ClassRegistry& registry);
bool registerExplosionNetwork(sim::network::Registry& registry);

} // namespace opent1::game
