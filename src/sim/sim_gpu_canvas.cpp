#include <opent1/sim/sim_gpu_canvas.hpp>
#include <cmath>
#include <stdexcept>

namespace opent1::sim {
SdlGpuCanvas::SdlGpuCanvas(platform::SdlApplication& application,
                           Draw draw,
                           bool hidden,
                           gfx::GpuSurfaceConfig config)
    : SdlSimCanvas(application, {}, hidden), application_(application), draw_(std::move(draw)),
      config_(std::move(config)) {}

bool SdlGpuCanvas::processArguments(std::span<const std::string> args) {
    if (!SdlSimCanvas::processArguments(args)) {
        return false;
    }
    auto* window = application_.findWindow(windowId());
    if (!window) {
        return false;
    }
    const auto state = window->state();
    surface_ = gfx::SdlGpuSurface::create(*window, state.width, state.height, config_);
    if (!surface_) {
        return false;
    }
    surface_->setGamma(gamma_);
    paletteChanged();
    return true;
}

void SdlGpuCanvas::paletteChanged() {
    if (!surface_) {
        return;
    }
    const auto palette = getPalette();
    if (palette) {
        if (!surface_->software().setPalette(*palette)) {
            throw std::runtime_error("Invalid canvas palette");
        }
        if (paletteTransitionColor() >= 0) {
            surface_->software().clear(UInt8(paletteTransitionColor()));
        }
    } else {
        surface_->software().clearPalette();
    }
}

void SdlGpuCanvas::renderFrame() {
    if (!application_.findWindow(windowId())) {
        SdlSimCanvas::renderFrame();
        return;
    }
    // A draw callback may remove the canvas/window. Retain the surface, whose
    // window cleanup invalidates its GPU state before the native handle dies.
    const auto surface = surface_;
    const auto draw = draw_;
    const auto hardware = hardwareDraw_;
    if (!surface || !surface->ready() || !getPalette()) {
        return;
    }
    if (hardware) {
        if (!hardware(*surface)) {
            throw std::runtime_error("Hardware canvas frame failed: " +
                                     std::string(SDL_GetError()));
        }
    } else if (draw) {
        draw(surface->software());
    }
    if (surface->ready() &&
        (hardware ? surface->presentFrame() : surface->present()) == gfx::PresentResult::Error) {
        throw std::runtime_error(SDL_GetError());
    }
}

void SdlGpuCanvas::onRemove() {
    surface_.reset();
    SdlSimCanvas::onRemove();
}

bool SdlGpuCanvas::setGamma(Flt32 gamma) noexcept {
    if (!std::isfinite(gamma) || gamma < 0.05f || gamma > 20.f) {
        return false;
    }
    gamma_ = gamma;
    return !surface_ || surface_->setGamma(gamma);
}

bool installGpuCanvasFactory(SimGame& game,
                             platform::SdlApplication& application,
                             SdlGpuCanvas::Draw draw,
                             bool hidden,
                             gfx::GpuSurfaceConfig config) {
    return game.registerFactory(
        "SimCanvas", [&application, draw = std::move(draw), hidden, config = std::move(config)] {
            return std::make_unique<SdlGpuCanvas>(application, draw, hidden, config);
        });
}

bool installHardwareCanvasFactory(SimGame& game,
                                  platform::SdlApplication& application,
                                  SdlGpuCanvas::HardwareDraw draw,
                                  bool hidden,
                                  gfx::GpuSurfaceConfig config) {
    if (!draw) {
        return false;
    }
    return game.registerFactory(
        "SimCanvas", [&application, draw = std::move(draw), hidden, config = std::move(config)] {
            auto canvas =
                std::make_unique<SdlGpuCanvas>(application, SdlGpuCanvas::Draw{}, hidden, config);
            canvas->setHardwareDraw(draw);
            return canvas;
        });
}
} // namespace opent1::sim
