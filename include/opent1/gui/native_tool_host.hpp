#pragma once
#include <opent1/gui/native_object_hosts.hpp>
#include <opent1/gui/sdl_gui.hpp>
#include <opent1/gfx/sdl_gpu_surface.hpp>

namespace opent1::gui {
// Application and game outlive the host/plugin. Tool identities live in the
// active manager; removing them or their parent retires their owned SDL panels.
// The optional fallback supplies field/file dialogs and receives lifecycle
// notifications. It may resolve mainWindow() when starting a modal operation.
class NativeToolHost {
public:
    using BitmapLoader = std::function<std::shared_ptr<const gfx::Bitmap>(std::string_view)>;
    NativeToolHost(platform::SdlApplication&,
                   sim::SimGame&,
                   BitmapLoader,
                   std::shared_ptr<const gfx::BitmapFont>,
                   gfx::GpuSurfaceConfig = {},
                   bool hidden = false,
                   world::ObjectHost fallback = {});
    ~NativeToolHost();
    [[nodiscard]] world::ObjectHost host() const;
    bool process(const platform::Event&);
    bool redraw(std::string_view, bool present = true);
    bool click(std::string_view, std::string_view);
    [[nodiscard]] platform::WindowId mainWindow() const;
    [[nodiscard]] Canvas* canvas(std::string_view) const;
    [[nodiscard]] platform::SdlWindow* window(std::string_view) const;
    [[nodiscard]] gfx::SdlGpuSurface* surface(std::string_view) const;
    [[nodiscard]] std::string status(std::string_view, std::size_t) const;

private:
    struct State;
    std::shared_ptr<State> state_;
};
}
