#pragma once
#include <opent1/gui/runtime.hpp>
#include <opent1/gui/sdl_gui.hpp>
#include <opent1/gfx/sdl_gpu_surface.hpp>

namespace opent1::gui {
// One host per runtime. The application outlives the host. This owns the main
// window's GUI input binding and installs scoped toolbar/inspector/input hooks.
// Every panel owns its controls, font and last frame. Targets are revalidated
// against the runtime graph and lifetime token before Apply or input dispatch.
class NativeEditorHost {
public:
    NativeEditorHost(platform::SdlApplication&,
                     platform::SdlWindow&,
                     std::shared_ptr<Runtime>,
                     std::shared_ptr<const gfx::BitmapFont>,
                     gfx::GpuSurfaceConfig = {},
                     bool hidden = false);
    ~NativeEditorHost();
    NativeEditorHost(const NativeEditorHost&) = delete;
    NativeEditorHost& operator=(const NativeEditorHost&) = delete;
    bool process(const platform::Event&);
    bool redraw(bool inspector, bool present = true);
    bool action(std::string_view); // the same endpoints as the visible buttons
    bool apply();
    bool setField(std::string_view label, std::string_view value);
    [[nodiscard]] Canvas* canvas(bool inspector) const;
    [[nodiscard]] platform::SdlWindow* window(bool inspector) const;
    [[nodiscard]] gfx::SdlGpuSurface* surface(bool inspector) const;
    [[nodiscard]] SdlInput* input() const;
    void detach(); // release runtime/input hooks while retaining panel frames
private:
    struct State;
    std::shared_ptr<State> state_;
};
}
