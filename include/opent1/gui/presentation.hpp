#pragma once
#include <opent1/gfx/bitmap_palette_font.hpp>
#include <algorithm>
#include <cmath>
#include <charconv>
#include <string_view>

namespace opent1::gui {
// Shared by the compositor and absolute mouse input. Window coordinates and
// framebuffer pixels may differ on high-density displays; pixels is the latter.
struct Presentation {
    Point2I pixels{}, extent{};
    gfx::RectI viewport{0, 0, -1, -1};
    float scale{1};

    [[nodiscard]] static float requestedScale(std::string_view value) {
        float scale{};
        const auto parsed = std::from_chars(value.data(), value.data() + value.size(), scale);
        return parsed.ec == std::errc{} && parsed.ptr == value.data() + value.size() &&
                       std::isfinite(scale) && scale >= 1
                   ? scale
                   : 0.f;
    }

    [[nodiscard]] Point2F toPixels(Point2F point) const {
        return {viewport.left + point.x * float(viewport.width()) / extent.x,
                viewport.top + point.y * float(viewport.height()) / extent.y};
    }

    // Zero selects the largest integer scale that retains at least 640x480
    // logical pixels. Manual fractional scales are allowed, subject to that fit.
    [[nodiscard]] static Presentation fit(Point2I pixels, float requested = 0) {
        if (pixels.x <= 0 || pixels.y <= 0) {
            return {};
        }
        const auto maximum =
            std::max(1.f, std::min({float(pixels.x) / 640, float(pixels.y) / 480, 8.f}));
        const auto scale = std::isfinite(requested) && requested >= 1
                               ? std::clamp(requested, 1.f, maximum)
                               : std::floor(maximum);
        const Point2I extent{std::max(1, Int32(std::floor(pixels.x / scale))),
                             std::max(1, Int32(std::floor(pixels.y / scale)))};
        const Point2I size{std::min(pixels.x, Int32(std::round(extent.x * scale))),
                           std::min(pixels.y, Int32(std::round(extent.y * scale)))};
        const Point2I origin{(pixels.x - size.x) / 2, (pixels.y - size.y) / 2};
        return {pixels,
                extent,
                {origin.x, origin.y, origin.x + size.x - 1, origin.y + size.y - 1},
                scale};
    }
};
} // namespace opent1::gui
