#pragma once

#include <opent1/platform/sdl_events.hpp>
#include <SDL3/SDL_init.h>
#include <SDL3/SDL_video.h>
#include <algorithm>
#include <exception>
#include <functional>
#include <iterator>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

namespace opent1::platform {
struct WindowConfig {
    std::string title{"OpenT1"};
    Int32 width{640}, height{480}; // client coordinates, not framebuffer pixels
    Int32 minWidth{20}, minHeight{20};
    Int32 maxWidth{}, maxHeight{}; // zero means no upper bound
    bool hidden{}, fullscreen{}, highPixelDensity{true}, resizable{true};
    bool quitOnDestroy{}; // legacy GWWindow default
    bool altEnterFullscreen{true};
};

struct WindowState {
    Int32 width{}, height{}, pixelWidth{}, pixelHeight{}, x{}, y{};
    bool focused{}, visible{}, minimized{}, fullscreen{};
};

// All window operations, moves and destruction belong to the SDL main thread.
// Each window retains a video-subsystem reference until its native handle dies.
class SdlWindow {
public:
    explicit SdlWindow(const WindowConfig& config = {});
    ~SdlWindow();
    SdlWindow(SdlWindow&&) noexcept;
    SdlWindow& operator=(SdlWindow&&) noexcept;
    SdlWindow(const SdlWindow&) = delete;
    SdlWindow& operator=(const SdlWindow&) = delete;

    [[nodiscard]] WindowId id() const noexcept;
    [[nodiscard]] SDL_Window* nativeHandle() const noexcept; // borrowed for Layer 5
    // Retain the opaque token while native resources need cleanup. Destroying
    // it cancels the callback. Runs before the native window/video reference
    // dies, including move-assignment and application-driven native close.
    // Main-thread only; callbacks must not throw or destroy/reenter this window.
    [[nodiscard]] std::shared_ptr<void> beforeNativeDestroy(std::function<void()>);
    [[nodiscard]] WindowState state() const noexcept;
    void setTitle(std::string_view title);
    void setClientSize(Int32 width, Int32 height);
    void setPosition(Int32 x, Int32 y);
    void setSizeLocked(bool locked);
    void setFullscreen(bool fullscreen);
    void show();
    void hide();
    void startTextInput();
    void stopTextInput();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    void apply(const WindowEvent& event);
    friend class SdlApplication;
};

struct LoopConfig {
    bool checkFocus{true};
    UInt32 nonfocusTimeoutMs{100};
    UInt32 foregroundRateHz{}; // zero lets SDL run freely
    std::size_t maxPendingEvents{4096};
};

// SDL_MAIN_USE_CALLBACKS adapter. User callbacks always run on the main thread.
// event() copies into a bounded queue and may be called by a producer thread.
// Main-thread lifecycle events dispatch immediately, including queued predecessors.
// Other methods require the main thread. Do not recursively call lifecycle methods.
class SdlApplication {
public:
    explicit SdlApplication(LoopConfig config = {});
    virtual ~SdlApplication();
    SdlApplication(const SdlApplication&) = delete;
    SdlApplication& operator=(const SdlApplication&) = delete;

    SdlWindow& createWindow(const WindowConfig& config = {});
    [[nodiscard]] SdlWindow* findWindow(WindowId id) noexcept;
    void destroyWindow(WindowId id);
    void requestQuit(Int32 exitCode = 0) noexcept; // first request wins
    [[nodiscard]] Int32 exitCode() const noexcept;
    [[nodiscard]] std::string error() const;
    [[nodiscard]] bool foreground() const noexcept;

    SDL_AppResult initialize(std::span<const std::string_view> arguments,
                             bool mainCallbacks = false) noexcept;
    SDL_AppResult event(const SDL_Event& event) noexcept;
    SDL_AppResult iterate() noexcept;
    void shutdown(SDL_AppResult result) noexcept; // idempotent; onExit before windows die

    // Synchronous main-thread dialog pump, callable from onEvent/onIdle. Input
    // goes only to the modal window; application/window lifecycle still runs.
    // No recursive iterate/onIdle or nested pump. False means admission, frame,
    // callback, owner/window destruction or application termination failure.
    bool runModal(WindowId owner,
                  WindowId dialog,
                  std::function<void(const Event&)> input,
                  std::function<bool()> active,
                  std::function<bool()> frame);

protected:
    // Windowless hosts can request events without initializing a video driver.
    virtual SDL_InitFlags requiredSubsystems(std::span<const std::string_view>) const {
        return SDL_INIT_VIDEO | SDL_INIT_GAMEPAD;
    }

    virtual bool onInit(std::span<const std::string_view>) {
        return true;
    }

    virtual void onEvent(const Event&) {}

    virtual bool onCloseRequested(WindowId) {
        return true;
    }

    virtual void onWindowDestroyed(WindowId) {}

    virtual void onIdle() {}

    virtual void onExit(Int32) {}

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    SDL_AppResult result() const noexcept;
    void fail(const char* message) noexcept;
    void updateRate();
    void dispatch(const Event& event);
    void drainEvents();
    void retireWindow(WindowId, bool notify);
};

// Use these from the four SDL_App* entry points in exactly one executable TU.
// Construction and host exceptions never cross SDL's C ABI.
template <class Application>
SDL_AppResult initApplication(void** state, int argc, char** argv) noexcept {
    *state = nullptr;
    try {
        auto app = std::make_unique<Application>();
        std::vector<std::string_view> arguments;
        if (argc < 0 || (argc > 0 && !argv)) {
            return SDL_APP_FAILURE;
        }
        arguments.reserve(static_cast<std::size_t>(argc));
        std::ranges::transform(std::span(argv, static_cast<std::size_t>(argc)),
                               std::back_inserter(arguments),
                               [](const char* arg) {
                                   return std::string_view(arg ? arg : "");
                               });
        SdlApplication* instance = app.release();
        *state = instance;
        return instance->initialize(arguments, true);
    } catch (const std::exception& e) {
        SDL_SetError("%s", e.what());
    } catch (...) {
        SDL_SetError("OpenT1 application construction failed");
    }
    return SDL_APP_FAILURE;
}

SDL_AppResult iterateApplication(void* state) noexcept;
SDL_AppResult handleApplicationEvent(void* state, SDL_Event* event) noexcept;
void quitApplication(void* state, SDL_AppResult result) noexcept;
}
