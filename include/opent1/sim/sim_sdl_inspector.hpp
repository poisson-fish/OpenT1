#pragma once
#include <opent1/sim/sim_inspector.hpp>
#include <opent1/platform/sdl_window.hpp>

namespace opent1::sim {
// Bind an inspector to an existing native SDL window. Route owned platform
// events here before game input; true means the event belongs to the inspector.
// The model must outlive this adapter. No game/object pointers are retained.
// Draw the model into SdlGpuSurface::software(), then present normally. Logical
// coordinates scale with window resize independently of native pixel density.
class SdlInspectorInput {
public:
    SdlInspectorInput(platform::SdlWindow&, SimInspector&);
    ~SdlInspectorInput();
    SdlInspectorInput(const SdlInspectorInput&) = delete;
    SdlInspectorInput& operator=(const SdlInspectorInput&) = delete;
    bool process(const platform::Event&, SimGame&);
    // Call after programmatic model focus/open/close changes as well.
    void syncTextInput();

private:
    SimInspector& inspector_;
    SDL_Window* window_{};
    platform::WindowId id_{};
    bool focused_{}, textInput_{};
    std::shared_ptr<void> cleanup_;
    void release() noexcept;
};
} // namespace opent1::sim
