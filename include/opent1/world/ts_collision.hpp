#pragma once
#include <opent1/world/ts_shape_pose.hpp>
#include <opent1/math/collision.hpp>

namespace opent1::world {
enum class TsCollisionLevel { Parts, Faces };

// Immutable owned pose snapshot. Uses the selected detail hierarchy only (the
// render-only always hierarchy is excluded). Authored packed vertices 0/1 are
// each mesh frame's bounds. No renderer, texture or simulation dependency.
class TsShapeCollision {
public:
    [[nodiscard]] static std::optional<TsShapeCollision> prepare(const TsShapePose&, Int32 detail);

    [[nodiscard]] const std::optional<Box3F>& bounds() const {
        return bounds_;
    }

    [[nodiscard]] Point3F authoredCenter() const {
        return center_;
    }

    [[nodiscard]] const Box3F& authoredBounds() const {
        return authoredBounds_;
    }

    // Inputs and outputs are in shape space. Misses leave output intact;
    // malformed arguments/work limits throw. At most 4096 returned surfaces.
    bool line(Point3F start,
              Point3F end,
              CollisionSurfaceList&,
              TsCollisionLevel = TsCollisionLevel::Faces) const;
    bool tube(Point3F start,
              Point3F end,
              Flt32 radius,
              CollisionSurfaceList&,
              TsCollisionLevel = TsCollisionLevel::Faces) const;
    bool sphere(Point3F center,
                Flt32 radius,
                CollisionSurfaceList&,
                TsCollisionLevel = TsCollisionLevel::Faces) const;
    // As in TS3, boxes and shape pairs report intersecting part IDs, without
    // face normals/contact positions. toShape maps the argument into this shape.
    bool box(const Box3F&, const TMat3F& toShape, CollisionSurfaceList&) const;
    bool shape(const TsShapeCollision&, const TMat3F& toShape, CollisionSurfaceList&) const;

private:
    struct Triangle {
        std::array<Point3F, 3> vertices;
        Int32 material{}, surface{};
    };

    struct Part {
        Int32 id{};
        Box3F box;
        TMat3F transform, inverse;
        std::vector<Triangle> triangles;
    };

    std::vector<Part> parts_;
    std::optional<Box3F> bounds_;
    Point3F center_;
    Box3F authoredBounds_;
};
} // namespace opent1::world
