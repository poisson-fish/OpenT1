#pragma once
#include <opent1/core/types.hpp>
#include <optional>

namespace opent1::world {
struct TerrainTextureView {
    Flt32 focalLength{320}; // horizontal projection scale, in pixels
    Int32 detail{};         // original terrainTextureDetail bias, in [0,8]
    bool perspective{true};
};

struct TerrainTextureSelection {
    UInt32 mipLevel{}, pixels{};
};

// Original static renderer policy for a rigid camera. Distance is the average
// of the four authored corner distances, before geomorphing. Level zero uses
// the direct texture path; other levels use the composite texture-size clamp.
// mipLevel is the composite builder's source mip, or the direct path's request
// before availability clamping. Rejects invalid inputs/unsupported upsampling.
[[nodiscard]] std::optional<TerrainTextureSelection> selectTerrainTexture(
    Flt32 distance, Int32 gridScale, UInt32 level, UInt32 textureWidth, const TerrainTextureView&);
} // namespace opent1::world
