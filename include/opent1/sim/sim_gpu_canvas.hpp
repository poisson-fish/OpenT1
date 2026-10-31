#pragma once
#include <opent1/gfx/sdl_gpu_surface.hpp>
#include <opent1/sim/sim_sdl_canvas.hpp>

namespace opent1::sim {
// Layer 5 consumer of simulation palette/canvas interfaces. Framebuffer size
// is the initial client size; native resizes scale it during GPU presentation.
class SdlGpuCanvas : public SdlSimCanvas {
public:
    using Draw = std::function<void(gfx::SoftwareSurface&)>;
    // Return true after rendering a complete hardware frame with surface.render.
    // The callback may remove its canvas/window; the frame retains the surface.
    using HardwareDraw = std::function<bool(gfx::SdlGpuSurface&)>;
    explicit SdlGpuCanvas(platform::SdlApplication&,
                          Draw = {},
                          bool hidden = false,
                          gfx::GpuSurfaceConfig = {});
    bool processArguments(std::span<const std::string>) override;
    void renderFrame() override;
    void onRemove() override;
    bool setGamma(Flt32) noexcept;

    void setHardwareDraw(HardwareDraw draw) {
        hardwareDraw_ = std::move(draw);
    }

    [[nodiscard]] gfx::SdlGpuSurface* surface() const noexcept {
        return surface_.get();
    }

protected:
    void paletteChanged() override;

private:
    platform::SdlApplication& application_;
    Draw draw_;
    HardwareDraw hardwareDraw_;
    gfx::GpuSurfaceConfig config_;
    std::shared_ptr<gfx::SdlGpuSurface> surface_;
    Flt32 gamma_{1};
};

bool installGpuCanvasFactory(SimGame&,
                             platform::SdlApplication&,
                             SdlGpuCanvas::Draw = {},
                             bool hidden = false,
                             gfx::GpuSurfaceConfig = {});
bool installHardwareCanvasFactory(SimGame&,
                                  platform::SdlApplication&,
                                  SdlGpuCanvas::HardwareDraw,
                                  bool hidden = false,
                                  gfx::GpuSurfaceConfig = {});
} // namespace opent1::sim
