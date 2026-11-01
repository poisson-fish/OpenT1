#pragma once
#include <opent1/math/collision.hpp>
#include <opent1/math/plane.hpp>
#include <optional>

namespace opent1::world {
// Native retains the original collider's authored polygons, including rounding
// and concave/collinear outlines. Both policies validate storage and the BSP.
enum class InteriorSurfacePolicy { Strict, Native };

struct InteriorCollisionNode {
    UInt32 plane{};
    Int32 front{}, back{};
};

struct InteriorCollisionLeaf {
    bool solid{};
    std::vector<UInt32> surfaces;
};

struct InteriorCollisionSurface {
    UInt32 plane{};
    bool planeFront{true};
    Int32 material{-1};            // -1 is the original null material
    std::vector<Point3F> vertices; // original UInt8 count, 3..255; Strict requires convex/coplanar
};

// Owned, validated BSP collision geometry. Child >=0 indexes a node; negative
// child encodes leaf -(child+1). Root is node 0. Native ITR decoding is separate.
class InteriorCollision {
public:
    [[nodiscard]] static std::optional<InteriorCollision>
        prepare(std::span<const PlaneF>,
                std::span<const InteriorCollisionNode>,
                std::span<const InteriorCollisionLeaf>,
                std::span<const InteriorCollisionSurface>,
                InteriorSurfacePolicy = InteriorSurfacePolicy::Strict);
    bool line(Point3F start, Point3F end, CollisionSurfaceList&) const;
    bool tube(Point3F start, Point3F end, Flt32 radius, CollisionSurfaceList&) const;
    bool sphere(Point3F center, Flt32 radius, CollisionSurfaceList&) const;
    bool box(const Box3F&, const TMat3F& toInterior, CollisionSurfaceList&) const;

private:
    InteriorCollision() = default;
    std::vector<PlaneF> planes_;
    std::vector<InteriorCollisionNode> nodes_;
    std::vector<InteriorCollisionLeaf> leaves_;
    std::vector<InteriorCollisionSurface> surfaces_;
};
} // namespace opent1::world
