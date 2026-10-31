#pragma once
#include <opent1/gui/sdl_gui.hpp>
#include <opent1/gfx/sdl_gpu_surface.hpp>
#include <opent1/console/console.hpp>

namespace opent1::gui {
// Tool panels inherit the game window's scale, even when their own authored
// canvas is smaller than 640x480. Pixel density is accounted for only once.
[[nodiscard]] float nativeUiScale(const platform::SdlWindow* reference, float requested = 0);

[[nodiscard]] inline float nativeUiScale(const platform::SdlWindow* reference,
                                         const Console& console) {
    return nativeUiScale(reference,
                         Presentation::requestedScale(console.getVariable("pref::UI::scale")));
}

class SdlPresentation {
public:
    [[nodiscard]] Presentation layout(platform::SdlWindow&, Point2I logical, float scale);
    static bool upload(gfx::SdlGpuSurface&, const gfx::Bitmap&, const Presentation&, bool present);
    bool draw(Canvas&,
              platform::SdlWindow&,
              gfx::SdlGpuSurface&,
              SdlInput&,
              const gfx::Palette&,
              Point2I logical,
              float scale,
              bool present);

private:
    float appliedScale_{}, appliedDensity_{};
};
} // namespace opent1::gui
