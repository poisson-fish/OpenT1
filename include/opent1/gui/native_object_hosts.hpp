#pragma once
#include <opent1/world/object_plugins.hpp>
#include <opent1/platform/sdl_window.hpp>
#include <opent1/gfx/bitmap_palette_font.hpp>
#include <opent1/world/input_commands.hpp>

namespace opent1::gui {
// Application must outlive its plugin. A supplied launcher is an explicit OS
// capability (and allows tests to exercise routing without launching anything).
using ShellLauncher = std::function<bool(std::string_view, std::string_view)>;
[[nodiscard]] world::ObjectHost nativeShellHost(platform::SdlApplication* = nullptr,
                                                ShellLauncher = {});
// Windows console allocation is opt-in through WinConsoleEnabled. Removal
// releases only a console allocated by this host; attached consoles are borrowed.
[[nodiscard]] world::ObjectHost nativeConsoleHost();
using ScreenshotWriter = std::function<bool(std::string_view, const gfx::Bitmap&)>;
// Canvas names resolve afresh on every command; the host never keeps a borrowed
// native window or GPU surface across canvas removal or world replacement.
[[nodiscard]] world::ObjectHost nativeGraphicsHost(platform::SdlApplication&,
                                                   ScreenshotWriter = {});
using HostFileWriter = std::function<bool(std::string_view, std::string_view)>;

// Owns normalized SDL devices and action-map editing. Plugin manager changes
// detach before world teardown; the native window can disappear first.
class NativeInputHost {
public:
    explicit NativeInputHost(platform::SdlWindow&, HostFileWriter = {});
    [[nodiscard]] world::ObjectHost host() const;
    bool process(const platform::Event&);
    [[nodiscard]] std::shared_ptr<const sim::SimActionMap> map(std::string_view) const;

private:
    struct State;
    std::shared_ptr<State> state_;
};
} // namespace opent1::gui
