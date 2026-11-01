#pragma once
#include <opent1/gfx/bitmap_palette_font.hpp>

namespace opent1::gfx {
// Bounds of nonzero indices, excluding row padding. Invalid input fails;
// a valid all-zero bitmap returns an empty rectangle.
[[nodiscard]] std::optional<RectI> indexedContentBounds(const Bitmap&);
}
