#pragma once
#include <opent1/math/collision.hpp>
#include <array>
#include <optional>

namespace opent1::world {
struct TerrainCollisionCell {
    Int32 material{};
    bool hole{};
};

// Collision geometry only. Row-major (width+1)*(height+1) heights and width*height
// cells, with legacy alternating 45/135 degree diagonals. No borrowed storage.
// Native GridFile/GridBlock decoding and rendering are separate adapters.
// Square size >= .0001; <=4096 cells per axis and <=1,048,576 total cells.
class TerrainCollision {
public:
    [[nodiscard]] static std::optional<TerrainCollision>
    prepare(UInt32 width,
            UInt32 height,
            Flt32 squareSize,
            std::span<const Flt32> heights,
            std::span<const TerrainCollisionCell> cells);

    [[nodiscard]] const Box3F& bounds() const {
        return bounds_;
    }

    [[nodiscard]] std::optional<CollisionSurface> surface(Point2F, bool hitHoles = false) const;
    bool line(Point3F start, Point3F end, CollisionSurfaceList&, bool hitHoles = false) const;
    bool sphere(Point3F center, Flt32 radius, CollisionSurfaceList&) const;
    // Plane-offset terrain approximation, not a swept sphere over triangle edges.
    bool tube(Point3F start, Point3F end, Flt32 radius, CollisionSurfaceList&) const;
    // Height/support contact beneath transformed box center. Also used by TS.
    bool box(const Box3F&, const TMat3F& toTerrain, CollisionSurfaceList&) const;

private:
    TerrainCollision() = default;
    [[nodiscard]] std::array<std::array<Point3F, 3>, 2> triangles(UInt32 x, UInt32 y) const;
    bool segment(Point3F, Point3F, Flt32, bool, CollisionSurfaceList&) const;
    UInt32 width_{}, height_{};
    Flt32 size_{};
    std::vector<Flt32> heights_;
    std::vector<TerrainCollisionCell> cells_;
    Box3F bounds_;
};
} // namespace opent1::world
