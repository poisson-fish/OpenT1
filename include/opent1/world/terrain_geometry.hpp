#pragma once
#include <opent1/world/terrain_grid.hpp>

namespace opent1::world {
struct TerrainGeometryView {
    Flt32 focalLength{320}, screenSize{50}, pixelScale{1};
    Flt32 nearDistance{.1f}, farDistance{65536};
    bool perspective{true};
};

struct TerrainGeometryDetail {
    Flt32 detailDistance{}, minDistance{}, maxDistance{}, distanceScale{};
};

using TerrainGeometryTable = std::array<TerrainGeometryDetail, 8>;
// Original GridRender::setContext distances for a rigid camera. screenSize is
// measured at the original 640-pixel window size; pixelScale is 640/viewport.
[[nodiscard]] std::optional<TerrainGeometryTable>
terrainGeometryDetails(Int32 scale, const TerrainGeometryView&);
// x/y are native block coordinates, aligned to 2^level. The caller supplies
// distance to the AUTHORED point, before morphing. Height pins prevent movement.
// Scan-row/coarsest vertices must be retained by the caller without morphing.
[[nodiscard]] std::optional<Flt32> morphTerrainHeight(const TerrainGridBlock&,
                                                      UInt32 x,
                                                      UInt32 y,
                                                      UInt32 level,
                                                      Flt32 distance,
                                                      const TerrainGeometryDetail&);
// First-writer GPU inputs: authored height, merge height, fraction, authored distance.
[[nodiscard]] std::optional<std::array<Flt32, 4>> terrainMorphInputs(const TerrainGridBlock&,
                                                                     UInt32 x,
                                                                     UInt32 y,
                                                                     UInt32 level,
                                                                     Flt32 distance,
                                                                     const TerrainGeometryDetail&);
} // namespace opent1::world
