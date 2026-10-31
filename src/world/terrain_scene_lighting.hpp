#pragma once
#include <opent1/world/world_surfaces.hpp>
#include <opent1/core/bitset.hpp>

namespace opent1::world::terrain_detail {
struct PointLightRegion {
    UInt32 source, x0, y0, x1, y1;
};

struct PointLightMask {
    UInt32 width{}, lights{};
    std::optional<BitSquare> squares;
    [[nodiscard]] std::optional<bool>
    under(UInt32 x, UInt32 y, UInt32 cells, std::size_t& work) const;
};

// One native block-cube pre-scan per placement, followed by aligned tile tests.
// Oversized native blocks have no mask and never admit dynamic lighting.
[[nodiscard]] std::optional<PointLightMask>
pointLightMask(UInt32 width, Int32 scale, Point3F origin, const TsLighting&, std::size_t& work);
// Shared work budget covers height selection and all merged samples. Positions
// are grid-local; origins distinguish repeated placements of the same block.
[[nodiscard]] bool validScene(const TsLighting&);
[[nodiscard]] std::optional<std::vector<PointLightRegion>>
pointLightRegions(const TerrainGridBlock&,
                  UInt32 x,
                  UInt32 y,
                  UInt32 cells,
                  Int32 scale,
                  Point3F origin,
                  const TsLighting&,
                  std::size_t& work);
[[nodiscard]] std::optional<std::shared_ptr<const gfx::LightMap>>
mergePointLights(const TerrainGridBlock&,
                 UInt32 x,
                 UInt32 y,
                 UInt32 cells,
                 Int32 scale,
                 Point3F origin,
                 const TsLighting&,
                 std::span<const PointLightRegion>,
                 const gfx::LightMap&,
                 std::size_t& work);
} // namespace opent1::world::terrain_detail
