#pragma once

#include <opent1/game/f_base.hpp>
#include <opent1/math/matrix.hpp>
#include <opent1/math/random.hpp>
#include <opent1/sim/sim_base.hpp>
#include <opent1/sim/sim_collision_movement.hpp>
#include <opent1/world/ts_static_shape.hpp>
#include <opent1/sim/sim_environment_camera.hpp>

#include <array>
#include <optional>
#include <string_view>

namespace opent1::game {

class DecalManager : public sim::SimObject {
    using Parent = sim::SimObject;

public:
    enum Constants : Int32 {
        DECALS = 7,
        MAX_DECALS = 200,
    };

    enum DECALTYPE : Int32 {
        TYPE_LIGHT_FOOT_L = 0,
        TYPE_LIGHT_FOOT_R = 1,
        TYPE_MED_FOOT_L = 2,
        TYPE_MED_FOOT_R = 3,
        TYPE_HEAVY_FOOT_L = 4,
        TYPE_HEAVY_FOOT_R = 5,
        TYPE_HOLE_BASIC = 6,
    };

    struct Decal {
        Point3F center{};
        std::array<Point3F, 4> worldPts{};
        DECALTYPE type{TYPE_HOLE_BASIC};
    };

    struct DecalTypeDesc {
        std::string_view fileName{};
        Flt32 size{0.15f};
    };

    static inline constexpr std::array<DecalTypeDesc, DECALS> kDecalTypes{{
        {"lightfootl.bmp", 0.15f},
        {"lightfootr.bmp", 0.15f},
        {"medfootl.bmp", 0.20f},
        {"medfootr.bmp", 0.20f},
        {"heavyfootl.bmp", 0.32f},
        {"heavyfootl.bmp", 0.32f},
        {"bullethole.bmp", 0.15f},
    }};

    static inline Int32 iPrefNumDecals = 50;

    DecalManager();
    ~DecalManager() override;

    void reset();
    void addFootprint(const TMat3F& mat,
                      DECALTYPE type,
                      Flt32 footspan,
                      std::optional<Point3F> explicitHitPos = std::nullopt,
                      std::optional<Point3F> explicitHitNormal = std::nullopt);
    void addBulletHole(Point3F pos, Point3F normal, DECALTYPE type);
    void addDecal(const Point3F& pos,
                  const Point3F& normal,
                  const Point3F& perp1,
                  const Point3F& perp2,
                  DECALTYPE type);

    [[nodiscard]] Int32 numDecalsLimit() const noexcept {
        return numDecals_;
    }

    [[nodiscard]] Int32 curFirstDecal() const noexcept {
        return curFirstDecal_;
    }

    [[nodiscard]] Int32 curLastDecal() const noexcept {
        return curLastDecal_;
    }

    [[nodiscard]] Int32 activeDecalCount() const noexcept;

    [[nodiscard]] const std::array<Decal, MAX_DECALS>& decals() const noexcept {
        return rgDecals_;
    }

    [[nodiscard]] Random& random() noexcept {
        return random_;
    }

    [[nodiscard]] std::optional<world::TsStaticShapeFrame>
    prepareFrame(ResourceManager&, const sim::EnvironmentCamera&);

    bool onAdd() override;
    bool processQuery(sim::SimQuery& query) override;

private:
    Random random_{};
    Int32 numDecals_{0};
    Int32 curFirstDecal_{0};
    Int32 curLastDecal_{0};
    Int32 activeCount_{0};
    std::array<Flt32, DECALS> sizes_{0.15f, 0.15f, 0.20f, 0.20f, 0.32f, 0.32f, 0.15f};
    std::array<Decal, MAX_DECALS> rgDecals_{};
    std::optional<world::TsMaterialResources> materials_;
};

} // namespace opent1::game
