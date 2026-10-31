#pragma once
#include <opent1/gfx/bitmap_palette_font.hpp>

namespace opent1::gfx {
enum class DepthOrder : UInt8 { LargerInFront, SmallerInFront };

struct VisibilityPolygon {
    std::span<const Point3F> vertices; // projected x,y and positive depth value
    bool transparent{}, alwaysBehind{};
};

struct VisibilitySpan {
    std::size_t polygon{};
    Int32 y{}, left{}, right{}; // right exclusive
    bool operator==(const VisibilitySpan&) const = default;
};

// Portable p_scane.asm edge/depth visibility. Opaque polygons occlude one another;
// transparent polygons are clipped only by opaque polygons. Transparent drawing
// retains submission order (the original does not sort transparent layers).
// Convex clockwise polygons, 3..100 vertices, +/-16384 screen coordinates,
// positive depths [1/65536,65536]. Reverse/degenerate winding draws nothing.
// Output owns all spans: opaque polygons first, then transparent polygons,
// retaining submission order within each group. There
// are no borrowed inputs after return. Invalid input/budget exhaustion returns
// nullopt. Valid scene storage grows dynamically; ScopedRenderLimits optionally
// applies explicit work/memory limits. Signed clips retain unclipped interpolation
// origins and edge tie history.
[[nodiscard]] std::optional<std::vector<VisibilitySpan>> sortVisibility(
    std::span<const VisibilityPolygon>, RectI clip, DepthOrder = DepthOrder::LargerInFront);

struct SortedFill {
    std::span<const Point3F> vertices;
    UInt8 color{};
    UInt32 paletteIndex{0xffffffffU};
    bool blend{}, alwaysBehind{}; // blend requires a 256x256 palette table
};
} // namespace opent1::gfx
