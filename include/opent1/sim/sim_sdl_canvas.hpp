#pragma once
#include <opent1/sim/sim_world_game.hpp>
#include <opent1/platform/sdl_window.hpp>

namespace opent1::sim {
// Main-thread adapter. The SDL application must outlive the game and canvases.
// It owns/dispatches native windows; canvas removal releases its window. The
// optional draw callback receives a borrowed window for Layer 5 surface setup.
class SdlSimCanvas : public SimCanvas {
public:
    using Draw = std::function<void(platform::SdlWindow&)>;
    using PaletteChange =
        std::function<void(platform::SdlWindow&, Resource<gfx::Palette>, Int32 transitionColor)>;
    explicit SdlSimCanvas(platform::SdlApplication&,
                          Draw = {},
                          bool hidden = false,
                          PaletteChange = {});
    ~SdlSimCanvas() override;
    bool processArguments(std::span<const std::string>) override;
    bool setTitle(std::string_view) override;
    void renderFrame() override;
    void onRemove() override;

    [[nodiscard]] platform::WindowId windowId() const {
        return window_;
    }

protected:
    void paletteChanged() override;

private:
    platform::SdlApplication& application_;
    Draw draw_;
    PaletteChange paletteChange_;
    platform::WindowId window_{};
    bool hidden_{};
};

// Installs the original SimCanvas constructor name, with an application-owned
// renderer callback. Duplicate factory registration returns false.
bool installSdlCanvasFactory(SimGame&,
                             platform::SdlApplication&,
                             SdlSimCanvas::Draw = {},
                             bool hidden = false,
                             SdlSimCanvas::PaletteChange = {});
}
