#include <opent1/gfx/software_rasterizer.hpp>
#include "software_blit.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <ranges>

namespace opent1::gfx {
using detail::blendAvailable;
using detail::drawMask;
using detail::translucent;

std::unique_ptr<SoftwareSurface> SoftwareSurface::create(Int32 width, Int32 height) {
    auto bitmap = Bitmap::create(width, height);
    return bitmap ? std::unique_ptr<SoftwareSurface>(new SoftwareSurface(std::move(*bitmap)))
                  : nullptr;
}

void SoftwareSurface::setClip(RectI region) noexcept {
    clip_ = intersect(bitmap_.bounds(), region).value_or(RectI{});
}

bool SoftwareSurface::setPalette(const Palette& palette) {
    if (!palette.valid()) {
        return false;
    }
    if (ownedPalette_ && ownedPalette_.use_count() == 2 && palette_.get() == ownedPalette_.get()) {
        *ownedPalette_ = palette;
    } else {
        ownedPalette_ = std::make_shared<Palette>(palette);
        palette_ = ownedPalette_;
    }
    return true;
}

bool SoftwareSurface::setSharedPalette(std::shared_ptr<const Palette> palette) {
    if (!palette || (palette != palette_ && !palette->valid())) {
        return false;
    }
    palette_ = std::move(palette);
    ownedPalette_.reset();
    return true;
}

void SoftwareSurface::clear(UInt8 color) {
    std::ranges::fill(bitmap_.pixels, std::byte(color));
}

void SoftwareSurface::point(Point2I at, UInt8 color) {
    if (clip_.contains(at)) {
        bitmap_.row(at.y)[std::size_t(at.x)] = std::byte(color);
    }
}

void SoftwareSurface::fill(RectI region, UInt8 color) {
    const auto clipped = intersect(region, clip_);
    if (!clipped) {
        return;
    }
    std::ranges::for_each(std::views::iota(clipped->top, clipped->bottom + 1), [&](auto y) {
        std::ranges::fill(
            bitmap_.row(y).subspan(std::size_t(clipped->left), std::size_t(clipped->width())),
            std::byte(color));
    });
}

void SoftwareSurface::rectangle(RectI region, UInt8 color) {
    if (!region.valid()) {
        return;
    }
    line({region.left, region.top}, {region.right, region.top}, color);
    line({region.left, region.bottom}, {region.right, region.bottom}, color);
    line({region.left, region.top}, {region.left, region.bottom}, color);
    line({region.right, region.top}, {region.right, region.bottom}, color);
}

void SoftwareSurface::line(Point2I first, Point2I second, UInt8 color) {
    if (!clipLine(first, second, clip_)) {
        return;
    }
    if (first.y > second.y) {
        std::swap(first, second);
    }
    const Int32 rows = second.y - first.y + 1, sign = second.x >= first.x ? 1 : -1;
    const Int32 columns = second.x - first.x + sign, slope = columns / rows;
    const auto residual = UInt32(std::abs(columns % rows));
    std::array<Int32, 2> step{slope, slope}, count{};
    if (slope != 0) {
        count = {std::abs(slope) - 1, std::abs(slope) - 1};
        if (residual) {
            step[1] += sign;
            ++count[1];
        }
    } else {
        step[1] = sign;
    }
    const auto fraction = UInt32((UInt64(residual) << 32) / UInt32(rows));
    const auto increment = fraction + 1U;
    std::size_t index = fraction >= 0x80000000U ? 1 : 0;
    Int32 span = count[index];
    const auto half = slope ? rows / 2 : (std::abs(second.x - first.x) + 1) / 2;
    UInt32 accumulator = UInt32((UInt64(half) << 32) / UInt32(rows)) + increment;
    if (slope == 0) {
        index = rows <= (std::abs(second.x - first.x) + 1) * 3 / 2 ? 1 : 0;
    }
    Int32 x = first.x;
    if (sign < 0) {
        if (slope) {
            x += step[index] + 1;
        } else {
            accumulator -= increment;
        }
    }
    // Exactly the original unsigned .32 accumulator/carry policy, with bounded
    // local spans in place of shared assembly globals and linked raster pointers.
    std::ranges::for_each(std::views::iota(Int32{0}, rows), [&](auto row) {
        fill({x, first.y + row, x + span, first.y + row}, color);
        if (sign > 0) {
            x += step[index];
        }
        const UInt64 next = UInt64(accumulator) + increment;
        accumulator = UInt32(next);
        index = std::size_t(next >> 32);
        span = count[index];
        if (sign < 0) {
            x += step[index];
        }
    });
}

bool SoftwareSurface::blit(const Bitmap& input, RectI source, Point2I destination, Flip flip) {
    if (!input.valid() || input.bitDepth != 8 || UInt8(flip) > 3 || !source.valid() ||
        !blendAvailable(input, palette_)) {
        return false;
    }
    const Int64 right = Int64(destination.x) + source.width() - 1,
                bottom = Int64(destination.y) + source.height() - 1;
    if (right > std::numeric_limits<Int32>::max() || bottom > std::numeric_limits<Int32>::max()) {
        return false;
    }
    RectI target{destination.x, destination.y, Int32(right), Int32(bottom)};
    if (!clipBlit(source, input.bounds(), target, clip_)) {
        return true;
    }
    // Snapshot a self-blit so overlap and flips cannot observe pixels already written.
    std::optional<Bitmap> snapshot;
    if (&input == &bitmap_) {
        snapshot = input;
    }
    const auto& bitmap = snapshot ? *snapshot : input;
    const bool transparent = (bitmap.attributes & drawMask) == BitmapTransparent;
    const bool blend = translucent(bitmap);
    const auto* palette = palette_ ? &palette_->find(bitmap.paletteIndex) : nullptr;
    if (!transparent && !blend && flip == Flip::None &&
        (!palette || bitmap.paletteIndex == 0xffffffffU)) {
        for (Int32 y = 0; y < source.height(); ++y) {
            const auto src = bitmap.row(source.top + y)
                                 .subspan(std::size_t(source.left), std::size_t(source.width()));
            auto dst = bitmap_.row(target.top + y)
                           .subspan(std::size_t(target.left), std::size_t(target.width()));
            std::memcpy(dst.data(), src.data(), src.size());
        }
        return true;
    }
    std::ranges::for_each(std::views::iota(Int32{0}, Int32(source.height())), [&](auto y) {
        const auto sy =
            (UInt8(flip) & UInt8(Flip::Y)) ? bitmap.height - 1 - source.top - y : source.top + y;
        const auto src = bitmap.row(sy);
        auto dst = bitmap_.row(target.top + y)
                       .subspan(std::size_t(target.left), std::size_t(target.width()));
        std::ranges::for_each(std::views::iota(Int32{0}, Int32(source.width())), [&](auto x) {
            const auto sx = (UInt8(flip) & UInt8(Flip::X)) ? bitmap.width - 1 - source.left - x
                                                           : source.left + x;
            const auto value = std::to_integer<UInt8>(src[std::size_t(sx)]);
            auto& out = dst[std::size_t(x)];
            if (blend) {
                out = palette->remap[UInt32(value) * 256 + std::to_integer<UInt8>(out)];
            } else if (!transparent || value != 0) {
                out = palette && bitmap.paletteIndex != 0xffffffffU
                          ? std::byte(palette->identity[value])
                          : std::byte(value);
            }
        });
    });
    return true;
}

bool SoftwareSurface::stretch(const Bitmap& input, RectI destination, Flip flip) {
    if (!input.valid() || input.bitDepth != 8 || UInt8(flip) > 3 || !destination.valid() ||
        destination.width() > Bitmap::MaxDimension || destination.height() > Bitmap::MaxDimension ||
        !blendAvailable(input, palette_)) {
        return false;
    }
    RectI source = input.bounds(), target = destination;
    Point2I phase{};
    if (!clipScaledBlit(source, target, clip_, phase, UInt8(flip))) {
        return true;
    }
    const bool flipX = (UInt8(flip) & 1) != 0, flipY = (UInt8(flip) & 2) != 0;
    // Reflect the clipped source rectangle exactly as b_draw.cpp does. Build
    // bounded sample lists before drawing: some original clipping combinations
    // address pixels outside the source. Those are rejected atomically here.
    const Int32 startX = flipX ? input.width - 1 - source.right : source.left;
    const Int32 startY = flipY ? input.height - 1 - source.top : source.top;
    const auto samples = [](Int32 start,
                            Int32 sign,
                            Int32 size,
                            Int32 storageSize,
                            Int32 destSize,
                            Int32 count,
                            Int32 adjustment) -> std::optional<std::vector<Int32>> {
        const auto step = UInt32(size / destSize);
        const auto increment =
            UInt32((UInt64(UInt32(size % destSize)) << 32) / UInt32(destSize)) + 1U;
        UInt32 accumulator = increment * UInt32(adjustment);
        Int64 position = start;
        std::vector<Int32> result;
        result.reserve(std::size_t(count));
        for (Int32 i = 0; i < count; ++i) {
            if (position < 0 || position >= storageSize) {
                return {};
            }
            result.push_back(Int32(position));
            const auto next = UInt64(accumulator) + increment;
            accumulator = UInt32(next);
            position += Int64(sign) * (Int64(step) + Int64(next >> 32));
        }
        return result;
    };
    const auto xs = samples(startX,
                            1,
                            input.width,
                            input.stride(),
                            Int32(destination.width()),
                            Int32(target.width()),
                            phase.x);
    const auto ys = samples(startY,
                            flipY ? -1 : 1,
                            input.height,
                            input.height,
                            Int32(destination.height()),
                            Int32(target.height()),
                            phase.y);
    if (!xs || !ys) {
        return false;
    }
    std::optional<Bitmap> snapshot;
    if (&input == &bitmap_) {
        snapshot = input;
    }
    const auto& bitmap = snapshot ? *snapshot : input;
    const bool transparent = (bitmap.attributes & drawMask) == BitmapTransparent,
               blend = translucent(bitmap);
    const auto* palette = palette_ ? &palette_->find(bitmap.paletteIndex) : nullptr;
    for (std::size_t y = 0; y < ys->size(); ++y) {
        const auto src = bitmap.level().subspan(
            std::size_t((*ys)[y]) * std::size_t(bitmap.stride()), std::size_t(bitmap.stride()));
        auto dst = bitmap_.row(target.top + Int32(y));
        for (std::size_t x = 0; x < xs->size(); ++x) {
            const auto value = std::to_integer<UInt8>(src[std::size_t((*xs)[x])]);
            auto& out = dst[std::size_t(flipX ? target.right - Int32(x) : target.left + Int32(x))];
            if (blend) {
                out = palette->remap[UInt32(value) * 256 + std::to_integer<UInt8>(out)];
            } else if (!transparent || value != 0) {
                // Original opaque X-flipped stretch omits identity lookup.
                const bool remap =
                    palette && bitmap.paletteIndex != 0xffffffffU && (!flipX || transparent);
                out = remap ? std::byte(palette->identity[value]) : std::byte(value);
            }
        }
    }
    return true;
}

bool SoftwareSurface::rgba(std::span<std::byte> output, Flt32 gamma, UInt32 paletteIndex) const {
    if (!palette_ || !std::isfinite(gamma) || gamma < 0.05f || gamma > 20.f ||
        output.size() != std::size_t(bitmap_.width) * std::size_t(bitmap_.height) * 4) {
        return false;
    }
    std::array<UInt8, 256> correction{};
    std::ranges::for_each(std::views::iota(0U, 256U), [&](auto i) {
        correction[i] = gamma == 1
                            ? UInt8(i)
                            : UInt8(std::lround(255.0 * std::pow(double(i) / 255.0, 1.0 / gamma)));
    });
    const auto& colors = palette_->find(paletteIndex).colors;
    std::ranges::for_each(std::views::iota(Int32{0}, bitmap_.height), [&](auto y) {
        const auto row = bitmap_.row(y);
        std::ranges::for_each(std::views::iota(Int32{0}, bitmap_.width), [&](auto x) {
            const auto& c = colors[std::to_integer<UInt8>(row[std::size_t(x)])];
            auto pixel = output.subspan(
                (std::size_t(y) * std::size_t(bitmap_.width) + std::size_t(x)) * 4, 4);
            pixel[0] = std::byte(correction[c.red]);
            pixel[1] = std::byte(correction[c.green]);
            pixel[2] = std::byte(correction[c.blue]);
            pixel[3] = std::byte{255};
        });
    });
    return true;
}
} // namespace opent1::gfx
