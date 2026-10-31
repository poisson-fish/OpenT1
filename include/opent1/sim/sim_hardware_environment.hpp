#pragma once
#include <opent1/gfx/hardware_batch.hpp>
#include <opent1/sim/sim_environment_camera.hpp>
#include <opent1/sim/sim_world_game.hpp>

namespace opent1::sim {
// Layer 6 supplies material frames; names match snapshot materialName with ASCII
// case folding and slash aliases. No DML/TS loader or resource handle is retained by the batch.
// Missing/empty material lists use the original opaque 244/245/246 fallback.
struct VolumeMaterialFrames {
    std::string_view name;
    std::span<const gfx::Bitmap* const> frames;
};

// Shared camera clipping preserves repeat UVs and interpolates haze/UVs before
// projection. Animated textures use image.frame(), vertex haze and constant
// opacity modulating texture alpha. Textured volumes alpha-blend, write depth
// and defer haze as in original OpenGL. Scene input is opaque TestAndWrite or blended
// TestOnly geometry; blended scene polygons join the textured volume depth sort.
// Submission: scene, fallback volumes, shadows, then textured faces sorted by
// mean camera depth back-to-front. This is a polygon sort, not intersecting
// translucent-surface partitioning. Shadows use either zero-index cutouts or
// exact black Alpha88 opacity, without indexed palette quantization.
// Returns an independent HardwareBatch; all inputs may expire after preparation.
// In addition to HardwareBatch budgets: <=512 volumes, <=256 named lists,
// <=65536 total frame references (<=32768/list), <=32 shadows, <=100 receiver
// polygons/shadow, <=65536 world vertices. Camera limits match SoftwareEnvironment.
[[nodiscard]] std::optional<gfx::HardwareBatch>
prepareHardwareEnvironment(const EnvironmentCamera&,
                           std::span<const SimVolumetricRenderImage>,
                           std::span<const VolumeMaterialFrames>,
                           const gfx::Palette&,
                           std::span<const EnvironmentShadow> = {},
                           std::span<const gfx::HardwarePolygon> scene = {},
                           Flt32 paletteGamma = 1);
} // namespace opent1::sim
