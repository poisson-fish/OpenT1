#pragma once
#include <opent1/math/matrix.hpp>
#include <opent1/world/terrain_geometry.hpp>
#include <compare>
#include <vector>

namespace opent1::world {
struct TerrainVisibilityView {
    RMat3F cameraToGrid;
    Point2F halfExtent{1,
                       .75f}; // centered near-plane extents; world extents for orthographic cameras
    Flt32 nearDistance{1}, visibleDistance{1500};
    bool perspective{true};
    Point2F
        viewportCenter{}; // original orthographic world-viewport offset; perspective ignores this offset
};

struct TerrainVisibilityRow {
    Int32 minimum{}, maximum{};
    auto operator<=>(const TerrainVisibilityRow&) const = default;
};

struct TerrainVisibilityTable {
    Point2I camera;
    Int32 startY{}; // camera-relative, inclusive rows and X ranges
    std::vector<TerrainVisibilityRow> rows;
    [[nodiscard]] bool visible(Int32 x, Int32 y) const;
};

struct TerrainVisibility {
    UInt32 scanLevel{};
    std::array<TerrainVisibilityTable, 8> tables;
};

// Original strict visible-distance gates. The caller may cap the result to its
// block size / explicit maximum level before building visibility tables.
[[nodiscard]] std::optional<UInt32> terrainGeometryScanLevel(const TerrainGeometryTable&,
                                                             Flt32 visibleDistance);
// Original GridEdgeTable/ViewCone footprint and 16.16 rasterization. Heights
// are grid-local authored extrema. Results own their storage; an empty cone
// produces empty tables. Invalid/oversized footprints return no result.
[[nodiscard]] std::optional<TerrainVisibility> terrainVisibility(Int32 scale,
                                                                 UInt32 scanLevel,
                                                                 const Point3F& camera,
                                                                 const Point2F& heightRange,
                                                                 const TerrainVisibilityView&);
} // namespace opent1::world
