#pragma once
#include <opent1/gfx/bitmap_palette_font.hpp>
#include <opent1/sim/sim_environment_render.hpp>

namespace opent1::sim {
// Explicit camera input for Layer 5 consumers; no TS camera/scene is borrowed.
// +Y forward, +X right, +Z up. Screen = center + (focal.x*x/y,-focal.y*z/y).
// Orthographic projection omits the divide. Viewport endpoints are inclusive.
// Scene inverseDepth = 1/y (perspective), or (near+far-y)/(near*far) (ortho).
struct EnvironmentCamera {
    TMat3F worldToCamera;
    gfx::RectI viewport;
    Point2F center, focal{1, 1};
    Flt32 nearDistance{1}, farDistance{1500};
    bool perspective{true};
};

struct EnvironmentShadow {
    const ShadowGeometry& geometry;
    const ShadowBitmap& bitmap;
};
} // namespace opent1::sim
