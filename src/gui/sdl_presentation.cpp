#include <opent1/gui/sdl_presentation.hpp>
#include <SDL3/SDL_video.h>

namespace opent1::gui {
float nativeUiScale(const platform::SdlWindow* reference, float requested) {
    if (reference) {
        const auto state = reference->state();
        return Presentation::fit({state.pixelWidth, state.pixelHeight}, requested).scale;
    }
    SDL_Rect bounds{};
    const auto display = SDL_GetPrimaryDisplay();
    const auto density = std::max(1.f, SDL_GetDisplayContentScale(display));
    if (SDL_GetDisplayUsableBounds(display, &bounds)) {
        return Presentation::fit({Int32(bounds.w * density), Int32(bounds.h * density)}, requested)
            .scale;
    }
    return 1;
}

Presentation SdlPresentation::layout(platform::SdlWindow& window, Point2I logical, float scale) {
    auto state = window.state();
    const auto density = state.width > 0 ? float(state.pixelWidth) / state.width : 1.f;
    scale = std::isfinite(scale) ? std::clamp(scale, 1.f, 8.f) : 1.f;
    if (scale != appliedScale_ || density != appliedDensity_) {
        window.setClientSize(std::max(1, Int32(std::round(logical.x * scale / density))),
                             std::max(1, Int32(std::round(logical.y * scale / density))));
        appliedScale_ = scale;
        appliedDensity_ = density;
        state = window.state();
    }
    return {{state.pixelWidth, state.pixelHeight},
            logical,
            {0, 0, state.pixelWidth - 1, state.pixelHeight - 1},
            scale};
}

bool SdlPresentation::upload(gfx::SdlGpuSurface& surface,
                             const gfx::Bitmap& bitmap,
                             const Presentation& presentation,
                             bool present) {
    if (!presentation.viewport.valid() ||
        !surface.resize(presentation.pixels.x, presentation.pixels.y) ||
        (bitmap.palette && !surface.software().setPalette(*bitmap.palette))) {
        return false;
    }
    surface.software().clear(0);
    return surface.software().stretch(bitmap, presentation.viewport) && surface.upload() &&
           (!present || surface.presentFrame() != gfx::PresentResult::Error);
}

bool SdlPresentation::draw(Canvas& canvas,
                           platform::SdlWindow& window,
                           gfx::SdlGpuSurface& surface,
                           SdlInput& input,
                           const gfx::Palette& palette,
                           Point2I logical,
                           float scale,
                           bool present) {
    const auto presentation = layout(window, logical, scale);
    input.presentation(presentation);
    auto frame = canvas.render(logical.x, logical.y, &palette);
    return frame && upload(surface, *frame, presentation, present);
}
} // namespace opent1::gui
