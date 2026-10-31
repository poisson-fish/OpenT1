#include <opent1/world/terrain_texture_detail.hpp>
#include <algorithm>
#include <bit>
#include <cmath>

namespace opent1::world {
std::optional<TerrainTextureSelection> selectTerrainTexture(
    Flt32 distance, Int32 scale, UInt32 level, UInt32 width, const TerrainTextureView& view) {
    if (!std::isfinite(distance) || distance < 0 || scale < 0 || scale > 16 || level > 7 ||
        !std::has_single_bit(width) || width < 2 || width > 256 ||
        !std::isfinite(view.focalLength) || view.focalLength <= 0 || view.focalLength > 65536 ||
        view.detail < 0 || view.detail > 8 || (level && width < 4)) {
        return {};
    }
    const auto squareDistance = distance - Flt32(1U << (UInt32(scale) + level));
    UInt32 mip{};
    if (squareDistance > 0) {
        const auto radius = Flt32(1U << scale) / Flt32(width);
        const auto projected = view.perspective ? (radius * view.focalLength) / squareDistance
                                                : radius * view.focalLength;
        const auto inverse = 1.f / projected;
        // Original getPower is bit width, including at exact powers of two.
        // Saturate before conversion: distant finite cameras must not overflow
        // the original signed-int reciprocal conversion (or hang its shift).
        mip = inverse >= 2147483648.f ? 31U : std::bit_width(UInt32(inverse));
    }
    mip += UInt32(view.detail);
    const auto shift = UInt32(std::countr_zero(width));
    if (!level) {
        return TerrainTextureSelection{mip, width >> std::min(mip, shift)};
    }
    const auto textureShift = std::min(8U, UInt32(std::max(2, Int32(shift) - Int32(mip))) + level);
    const auto pixels = 1U << textureShift;
    if (pixels > (width << level)) {
        return {};
    }
    return TerrainTextureSelection{shift - (textureShift - level), pixels};
}
} // namespace opent1::world
