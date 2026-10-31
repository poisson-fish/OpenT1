#include <opent1/gfx/software_rasterizer.hpp>
#include "software_blit.hpp"
#include <algorithm>
#include <cmath>
#include <numbers>
#include <ranges>

namespace opent1::gfx {
namespace {
constexpr Int32 unitScale = 65536, maxScale = 64 * unitScale;

Int64 scaled(Int64 value, Int32 scale) {
    const auto product = value * scale;
    return product >= 0 ? product / unitScale : -((-product + unitScale - 1) / unitScale);
}

template <class Text> Int64 logicalWidth(const BitmapFont& font, Text text) {
    Int64 width = -Int64(font.info.spacing);
    for (const auto c : text) {
        using C = std::make_unsigned_t<typename Text::value_type>;
        const auto code = UInt32(static_cast<C>(c));
        if (!code) {
            break;
        }
        width += ((font.info.flags & FontFixed) ? font.info.width : font.glyph(code).info.width) +
                 Int64(font.info.spacing);
    }
    return width;
}

// Pixel-center nearest sampling, with widened products and no row-padding reads.
Int32 sample(Int64 offset, Int32 source, Int32 destination) {
    return Int32(((offset * 2 + 1) * source) / (Int64(destination) * 2));
}
} // namespace

bool SoftwareSurface::rotate(
    const Bitmap& input, Point2F center, Point2F size, Int32 angle, Flip flip) {
    if (!input.valid() || input.bitDepth != 8 || UInt8(flip) > 3 ||
        !detail::blendAvailable(input, palette_) || !std::isfinite(center.x) ||
        !std::isfinite(center.y) || !std::isfinite(size.x) || !std::isfinite(size.y) ||
        size.x <= 0 || size.y <= 0 || size.x > Bitmap::MaxDimension ||
        size.y > Bitmap::MaxDimension) {
        return false;
    }
    if (!clip_.valid()) {
        return true;
    }
    const auto turn = UInt32(angle) & 65535U;
    double cosine{}, sine{};
    // Cardinal rotations are exact, including their edge ownership.
    if ((turn & 16383U) == 0) {
        constexpr std::array<double, 4> cosines{1, 0, -1, 0}, sines{0, 1, 0, -1};
        cosine = cosines[turn / 16384];
        sine = sines[turn / 16384];
    } else {
        const double radians = double(turn) * (2 * std::numbers::pi / 65536);
        cosine = std::cos(radians);
        sine = std::sin(radians);
    }
    const double rx = (std::abs(cosine) * size.x + std::abs(sine) * size.y) / 2;
    const double ry = (std::abs(sine) * size.x + std::abs(cosine) * size.y) / 2;
    const double left = std::max(double(clip_.left), std::ceil(double(center.x) - rx - .5));
    const double right = std::min(double(clip_.right), std::floor(double(center.x) + rx - .5));
    const double top = std::max(double(clip_.top), std::ceil(double(center.y) - ry - .5));
    const double bottom = std::min(double(clip_.bottom), std::floor(double(center.y) + ry - .5));
    if (left > right || top > bottom) {
        return true;
    }
    std::optional<Bitmap> snapshot;
    if (&input == &bitmap_) {
        snapshot = input;
    }
    const auto& bitmap = snapshot ? *snapshot : input;
    const detail::BitmapInk ink(bitmap, palette_);
    std::ranges::for_each(std::views::iota(Int32(top), Int32(bottom) + 1), [&](auto y) {
        auto output = bitmap_.row(y);
        std::ranges::for_each(std::views::iota(Int32(left), Int32(right) + 1), [&](auto x) {
            const double dx = double(x) + .5 - center.x, dy = double(y) + .5 - center.y;
            const double u = (cosine * dx + sine * dy) / size.x + .5;
            const double v = (-sine * dx + cosine * dy) / size.y + .5;
            if (u < 0 || u >= 1 || v < 0 || v >= 1) {
                return;
            }
            // Rounding at the last representable texel cannot escape the bitmap.
            Int32 sx = std::min(Int32(u * bitmap.width), bitmap.width - 1);
            Int32 sy = std::min(Int32(v * bitmap.height), bitmap.height - 1);
            if ((UInt8(flip) & 1) != 0) {
                sx = bitmap.width - 1 - sx;
            }
            if ((UInt8(flip) & 2) != 0) {
                sy = bitmap.height - 1 - sy;
            }
            ink.put(std::to_integer<UInt8>(bitmap.row(sy)[std::size_t(sx)]),
                    output[std::size_t(x)]);
        });
    });
    return true;
}

template <class Text>
bool SoftwareSurface::drawText(const BitmapFont& font, Point2I at, Text text, Point2I scale) {
    if (!font.valid() || text.size() > 1024U * 1024U || scale.x < 0 || scale.y < 0 ||
        scale.x > maxScale || scale.y > maxScale ||
        !std::ranges::all_of(font.sheets.bitmaps, [&](const auto& bitmap) {
            return detail::blendAvailable(bitmap, palette_);
        })) {
        return false;
    }
    if (!clip_.valid() || !scale.x || !scale.y) {
        return true;
    }

    struct Draw {
        Glyph glyph;
        Int64 x{}, y{};
        Int32 width{}, height{};
        RectI visible;
    };

    std::vector<Draw> draws;
    Int64 pen = 0;
    // Validate/layout the entire bounded string before drawing. Positions stay
    // widened until intersection, so huge advances and negative spacing are safe.
    for (const auto c : text) {
        using C = std::make_unsigned_t<typename Text::value_type>;
        const auto code = UInt32(static_cast<C>(c));
        if (!code) {
            break;
        }
        const auto glyph = font.glyph(code);
        const Int64 x = Int64(at.x) + scaled(pen, scale.x);
        const Int64 y = Int64(at.y) + scaled(Int64(font.info.baseline) + glyph.info.baseline -
                                                 glyph.info.height + 2,
                                             scale.y);
        const Int32 width = Int32(scaled(glyph.info.width, scale.x)),
                    height = Int32(scaled(glyph.info.height, scale.y));
        const Int64 left = std::max(x, Int64(clip_.left)), top = std::max(y, Int64(clip_.top));
        const Int64 right = std::min(x + width - 1, Int64(clip_.right)),
                    bottom = std::min(y + height - 1, Int64(clip_.bottom));
        if (left <= right && top <= bottom) {
            draws.push_back({glyph,
                             x,
                             y,
                             width,
                             height,
                             {Int32(left), Int32(top), Int32(right), Int32(bottom)}});
        }
        pen += ((font.info.flags & FontFixed) ? font.info.width : glyph.info.width) +
               Int64(font.info.spacing);
    }
    std::ranges::for_each(draws, [&](const auto& draw) {
        if (!draw.glyph.sheet) {
            fill(draw.visible, 253);
            return;
        }
        const auto& sheet = *draw.glyph.sheet;
        const detail::BitmapInk ink(sheet, palette_);
        std::ranges::for_each(
            std::views::iota(draw.visible.top, draw.visible.bottom + 1), [&](auto y) {
                const auto source =
                    sheet.row(draw.glyph.bounds.top +
                              sample(Int64(y) - draw.y, draw.glyph.info.height, draw.height));
                auto destination = bitmap_.row(y);
                std::ranges::for_each(
                    std::views::iota(draw.visible.left, draw.visible.right + 1), [&](auto x) {
                        const auto sx =
                            draw.glyph.bounds.left +
                            sample(Int64(x) - draw.x, draw.glyph.info.width, draw.width);
                        ink.put(std::to_integer<UInt8>(source[std::size_t(sx)]),
                                destination[std::size_t(x)]);
                    });
            });
    });
    return true;
}

bool SoftwareSurface::text(const BitmapFont& font, Point2I at, std::string_view text) {
    return drawText(font, at, text, font.info.scale);
}

bool SoftwareSurface::text(const BitmapFont& font, Point2I at, std::u16string_view text) {
    return drawText(font, at, text, font.info.scale);
}

template <class Text>
bool SoftwareSurface::drawTextRect(const BitmapFont& font, RectI at, Text text) {
    if (!at.valid() || !font.valid() || text.size() > 1024U * 1024U) {
        return false;
    }
    auto scale = font.info.scale;
    const auto logical = logicalWidth(font, text);
    if ((font.info.justification & TextFit) != 0) {
        if (text.empty() || text.front() == 0) {
            return drawText(font, {at.left, at.top}, text, scale);
        }
        if (logical <= 0 || font.info.height <= 0) {
            return false;
        }
        const Int64 sx = at.width() * unitScale / logical,
                    sy = at.height() * unitScale / font.info.height;
        if (sx > maxScale || sy > maxScale) {
            return false;
        }
        scale = {Int32(sx), Int32(sy)};
    }
    Int64 x = at.left, y = at.top;
    const auto width = scaled(logical, scale.x), baseline = scaled(font.info.baseline, scale.y);
    switch (font.info.justification & (TextLeft | TextRight | TextCenterH)) {
        case TextRight:
            x = Int64(at.right) - width + 1;
            break;
        case TextCenterH:
            x += (Int64(at.right) - x - width) / 2;
            break;
        default:
            break;
    }
    switch (font.info.justification & (TextTop | TextBottom | TextCenterV)) {
        case TextBottom:
            y = Int64(at.bottom) - baseline + 1;
            break;
        case TextCenterV:
            y += (Int64(at.bottom) - y - baseline) / 2;
            break;
        default:
            break;
    }
    if (x < INT32_MIN || x > INT32_MAX || y < INT32_MIN || y > INT32_MAX) {
        return false;
    }
    return drawText(font, {Int32(x), Int32(y)}, text, scale);
}

bool SoftwareSurface::textRect(const BitmapFont& font, RectI at, std::string_view text) {
    return drawTextRect(font, at, text);
}

bool SoftwareSurface::textRect(const BitmapFont& font, RectI at, std::u16string_view text) {
    return drawTextRect(font, at, text);
}
} // namespace opent1::gfx
