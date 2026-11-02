#pragma once
#include <opent1/world/ts_shape.hpp>
#include <opent1/gfx/directional_shadow.hpp>

namespace opent1::world {
// All positions, directions and normals use shape space. Directions point along
// the light rays, as in TS::Light. Directional inputs must have unit length.
struct TsLight {
    enum class Type { Directional, DirectionalNonlinear, DirectionalWrap, Point };
    Type type{Type::Directional};
    Point3F direction{0, 0, -1}, position{}, intensity{1, 1, 1};
    Flt32 range{1}, shadowAttenuation{1};
    Point3F attenuation{1, 0, 0}; // a0 + (a1 + a2 * distance) * distance
    std::optional<gfx::DirectionalShadowMap> shadowMap{};
    bool operator==(const TsLight&) const = default;
};

struct TsLighting {
    struct WorldSource {
        Point3F position{}, direction{};
        bool operator==(const WorldSource&) const = default;
    };

    Point3F ambient{};
    std::vector<TsLight> lights; // maximum 64; renderer-independent owned inputs
    TMat3F shapeToWorld{};       // directional shadow lookup uses the mesh sphere in world XY
    // The simulation adapter retains original coordinates so per-mesh native
    // preparation inverts the composed transform once, without double rounding.
    std::vector<WorldSource> worldSources{}; // empty for manually supplied shape-space lights

    bool operator==(const TsLighting& other) const {
        return ambient == other.ambient && lights == other.lights &&
               worldSources == other.worldSources && shapeToWorld.m == other.shapeToWorld.m &&
               shapeToWorld.p == other.shapeToWorld.p &&
               shapeToWorld.flags == other.shapeToWorld.flags;
    }

    [[nodiscard]] bool valid() const;
};

// The original 256-entry packed normal table, including its authored rounding.
[[nodiscard]] Point3F decodeTsNormal(UInt8) noexcept;
// Original directional, nonlinear/wrap and nonoptimized point vertex equations.
// Finite nonzero normals retain Quat16 transform rounding without normalization.
// Saturates the final sum to [0,1]. Invalid inputs/overflow return no value.
[[nodiscard]] std::optional<Point3F>
lightTsVertex(const TsLighting&, Point3F position, Point3F normal);

// Original per-mesh preparation. Packed bounds (vertices 0 and 1) define the
// sphere; point attenuation deliberately truncates its formula to int here.
class TsPreparedLighting {
public:
    enum class PointMode { OutOfRange, NotOptimized, FullyOptimized, MostlyOptimized };

    struct State {
        PointMode mode{PointMode::NotOptimized};
        Point3F direction{};
        Flt32 attenuation{1};
    };

    [[nodiscard]] static std::optional<TsPreparedLighting>
    prepare(const TsLighting&, Point3F center, Flt32 radius, const TMat3F& objectToShape = {});
    [[nodiscard]] std::optional<Point3F> lightVertex(Point3F position, Point3F normal) const;

    [[nodiscard]] std::span<const State> states() const {
        return states_;
    }

    [[nodiscard]] const TsLighting& lighting() const {
        return lighting_;
    }

private:
    TsLighting lighting_;
    std::vector<State> states_;
};
} // namespace opent1::world
