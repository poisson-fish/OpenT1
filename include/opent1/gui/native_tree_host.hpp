#pragma once
#include <opent1/gui/mission_view.hpp>
#include <opent1/gui/native_object_hosts.hpp>
#include <opent1/gfx/sdl_gpu_surface.hpp>
#include <opent1/sim/sim_inspector.hpp>

namespace opent1::gui {
// The application outlives the host/plugin. Each window owns its controls and
// resources; the simulation owns only a removable identity. Targets and parent
// canvases are resolved again for every operation. Alternate native server/client
// views can be supplied explicitly, without borrowing a replacement world.
// An optional SimGame service must outlive the supplied view and host.
class NativeTreeHost {
public:
    using BitmapLoader = std::function<std::shared_ptr<const gfx::Bitmap>(std::string_view)>;
    using Target = std::function<MissionView*(std::string_view)>;
    NativeTreeHost(platform::SdlApplication&,
                   MissionView&,
                   BitmapLoader,
                   std::shared_ptr<const gfx::BitmapFont> = {},
                   gfx::GpuSurfaceConfig = {},
                   bool hidden = false,
                   Target = {},
                   std::shared_ptr<const Persistent::ClassRegistry> = {},
                   sim::SimGame* = nullptr);
    [[nodiscard]] world::ObjectHost host() const;
    bool process(const platform::Event&);
    bool redraw(std::string_view, bool present = true);
    bool runScript(std::string_view, std::size_t);
    // Same endpoints used by native menu/keyboard/drag input. Clipboard clones
    // require the optional game's typed resource preparation/adoption services.
    bool action(std::string_view tree, std::string_view action, std::string_view value = {});
    bool drop(std::string_view tree,
              std::size_t source,
              std::size_t target,
              bool before = false,
              bool copy = false);
    [[nodiscard]] sim::SimInspector* inspector(std::string_view) const;
    [[nodiscard]] platform::SdlWindow* inspectorWindow(std::string_view) const;
    [[nodiscard]] MissionTree* tree(std::string_view) const;
    [[nodiscard]] Canvas* canvas(std::string_view) const;
    [[nodiscard]] platform::SdlWindow* window(std::string_view) const;

private:
    struct State;
    std::shared_ptr<State> state_;
};
}
