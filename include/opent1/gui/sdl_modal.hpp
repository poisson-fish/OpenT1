#pragma once
#include <opent1/gui/modal.hpp>
#include <opent1/gui/sdl_gui.hpp>
#include <opent1/gfx/sdl_gpu_surface.hpp>

namespace opent1::gui {
// Main-thread owner. Application and dialog models outlive each run.
class SdlModalHost {
public:
    SdlModalHost(platform::SdlApplication&,
                 platform::WindowId owner,
                 gfx::GpuSurfaceConfig = {},
                 bool hidden = false,
                 std::function<float()> requestedScale = {});
    bool run(ModalView);

    [[nodiscard]] platform::WindowId window() const {
        return window_;
    }

private:
    platform::SdlApplication& app_;
    platform::WindowId owner_{}, window_{};
    gfx::GpuSurfaceConfig gpu_;
    bool hidden_{};
    std::function<float()> requestedScale_;
};
}
