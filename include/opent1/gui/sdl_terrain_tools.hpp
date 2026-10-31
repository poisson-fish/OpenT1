#pragma once
#include <opent1/gui/terrain_tools.hpp>
#include <opent1/gui/sdl_gui.hpp>
#include <opent1/gfx/sdl_gpu_surface.hpp>

namespace opent1::gui {
// One native Ted or LSEditor window. Multiple windows retain one shared model,
// with independent canvases, focus, input bindings and native output resources.
class SdlTerrainWindow {
public:
    SdlTerrainWindow(std::shared_ptr<TerrainTools>,
                     bool landscapeWindow,
                     const platform::WindowConfig& = {},
                     const gfx::GpuSurfaceConfig& = {},
                     std::function<float()> requestedScale = {});
    bool process(const platform::Event&);
    bool redraw(bool present = true);
    void detach(); // retains the last native frame after model/resource teardown

    platform::SdlWindow& window() {
        return window_;
    }

    gfx::SdlGpuSurface& surface() {
        return *surface_;
    }

private:
    Canvas& canvas();
    std::shared_ptr<TerrainTools> tools_;
    bool landscape_{};
    platform::SdlWindow window_;
    std::unique_ptr<gfx::SdlGpuSurface> surface_;
    std::unique_ptr<SdlInput> input_;
    std::function<float()> requestedScale_;
    Presentation presentation();
};
} // namespace opent1::gui
