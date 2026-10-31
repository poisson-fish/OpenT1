#pragma once
#include <opent1/world/terrain_grid.hpp>

namespace opent1::world {
// Full-resolution native GridFile queries over immutable, possibly sparse DTB
// handles. Missing authored placements are empty; querying an unloaded placement
// throws instead of publishing a false miss. Pins affect rendering only.
class TerrainCollisionSnapshot {
public:
    [[nodiscard]] static std::optional<TerrainCollisionSnapshot> prepare(TerrainGridResources);

    [[nodiscard]] const Box3F& bounds() const {
        return bounds_;
    }

    [[nodiscard]] std::optional<CollisionSurface> surface(Point2F) const;
    bool line(Point3F, Point3F, CollisionSurfaceList&, bool editor = false) const;
    bool tube(Point3F, Point3F, Flt32, CollisionSurfaceList&) const;
    bool sphere(Point3F, Flt32, CollisionSurfaceList&) const;
    // Native SimCollisionTerrImage has no direct box implementation.
    bool box(const Box3F&, const TMat3F&, CollisionSurfaceList&) const;
    // Native TS support queries the authored center and bounding-box bottom.
    // Position is returned in shape space; normal retains native terrain space.
    bool
    shape(Point3F center, Flt32 bottom, const TMat3F& terrainToShape, CollisionSurfaceList&) const;

private:
    TerrainGridResources resources_;
    Box3F bounds_;
};
} // namespace opent1::world
