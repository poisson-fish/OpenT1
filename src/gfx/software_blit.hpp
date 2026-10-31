#pragma once
#include <opent1/gfx/bitmap_palette_font.hpp>

namespace opent1::gfx::detail {
constexpr UInt32 drawMask =
    BitmapTransparent | BitmapFuzzy | BitmapTranslucent | BitmapAdditive | BitmapSubtractive;

inline bool translucent(const Bitmap& bitmap) {
    const auto flags = bitmap.attributes & drawMask;
    return flags == BitmapTranslucent || flags == (BitmapTranslucent | BitmapTransparent);
}

inline bool blendAvailable(const Bitmap& bitmap, const std::optional<Palette>& palette) {
    return !translucent(bitmap) ||
           (palette && palette->find(bitmap.paletteIndex).remap.size() == 65536 &&
            palette->find(bitmap.paletteIndex).type == PaletteType::Translucent);
}

struct BitmapInk {
    const SubPalette* palette{};
    bool transparent{}, blend{}, remap{};

    BitmapInk(const Bitmap& bitmap, const std::optional<Palette>& colors)
        : palette(colors ? &colors->find(bitmap.paletteIndex) : nullptr),
          transparent((bitmap.attributes & drawMask) == BitmapTransparent),
          blend(translucent(bitmap)), remap(palette && bitmap.paletteIndex != 0xffffffffU) {}

    void put(UInt8 value, std::byte& destination) const {
        if (blend) {
            destination = palette->remap[UInt32(value) * 256 + std::to_integer<UInt8>(destination)];
        } else if (!transparent || value) {
            destination = std::byte(remap ? palette->identity[value] : value);
        }
    }
};
} // namespace opent1::gfx::detail
