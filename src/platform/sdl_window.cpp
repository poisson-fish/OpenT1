#include <opent1/platform/sdl_window.hpp>
#include <ranges>
#include <SDL3/SDL.h>
#include <algorithm>
#include <deque>
#include <mutex>
#include <stdexcept>
#include <unordered_map>
#include <utility>

namespace opent1::platform {
namespace {
void require(bool success) {
    if (!success) {
        throw std::runtime_error(SDL_GetError());
    }
}

struct Subsystem {
    SDL_InitFlags flags;

    explicit Subsystem(SDL_InitFlags value) : flags(value) {
        require(SDL_InitSubSystem(flags));
    }

    ~Subsystem() {
        SDL_QuitSubSystem(flags);
    }

    Subsystem(const Subsystem&) = delete;
    Subsystem& operator=(const Subsystem&) = delete;
};

struct WindowDeleter {
    void operator()(SDL_Window* window) const noexcept {
        SDL_DestroyWindow(window);
    }
};

std::string cString(std::string_view text) {
    if (text.find('\0') != std::string_view::npos) {
        throw std::invalid_argument("Embedded NUL in window title");
    }
    return std::string(text);
}

std::string ownedText(const char* text) {
    if (!text) {
        return {};
    }
    const auto size = SDL_strnlen(text, MaxEventTextBytes + 1);
    if (size > MaxEventTextBytes) {
        throw std::length_error("SDL event text exceeds 1 MiB");
    }
    return std::string(text, size);
}
}

std::optional<Event> translateEvent(const SDL_Event& e) {
    Event out;
    out.timestamp = e.common.timestamp;
    if (e.type >= SDL_EVENT_WINDOW_FIRST && e.type <= SDL_EVENT_WINDOW_LAST) {
        out.window = e.window.windowID;
        out.data = WindowEvent{e.type, e.window.data1, e.window.data2};
        return out;
    }
    switch (e.type) {
        case SDL_EVENT_QUIT:
        case SDL_EVENT_TERMINATING:
        case SDL_EVENT_LOW_MEMORY:
        case SDL_EVENT_WILL_ENTER_BACKGROUND:
        case SDL_EVENT_DID_ENTER_BACKGROUND:
        case SDL_EVENT_WILL_ENTER_FOREGROUND:
        case SDL_EVENT_DID_ENTER_FOREGROUND:
            out.data = AppEvent{e.type};
            break;
        case SDL_EVENT_KEY_DOWN:
        case SDL_EVENT_KEY_UP:
            out.window = e.key.windowID;
            out.data = KeyEvent{static_cast<UInt32>(e.key.scancode),
                                e.key.key,
                                e.key.mod,
                                e.key.down,
                                e.key.repeat};
            break;
        case SDL_EVENT_TEXT_INPUT:
            out.window = e.text.windowID;
            out.data = TextEvent{ownedText(e.text.text), false, 0, 0};
            break;
        case SDL_EVENT_TEXT_EDITING:
            out.window = e.edit.windowID;
            out.data = TextEvent{ownedText(e.edit.text), true, e.edit.start, e.edit.length};
            break;
        case SDL_EVENT_MOUSE_MOTION:
            out.window = e.motion.windowID;
            out.data = MouseMotionEvent{e.motion.which,
                                        e.motion.state,
                                        e.motion.x,
                                        e.motion.y,
                                        e.motion.xrel,
                                        e.motion.yrel};
            break;
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
        case SDL_EVENT_MOUSE_BUTTON_UP:
            out.window = e.button.windowID;
            out.data = MouseButtonEvent{e.button.which,
                                        e.button.button,
                                        e.button.clicks,
                                        e.button.down,
                                        e.button.x,
                                        e.button.y};
            break;
        case SDL_EVENT_MOUSE_WHEEL: {
            out.window = e.wheel.windowID;
            const float sign = e.wheel.direction == SDL_MOUSEWHEEL_FLIPPED ? -1.0f : 1.0f;
            out.data = MouseWheelEvent{e.wheel.which, sign * e.wheel.x, sign * e.wheel.y};
            break;
        }
        case SDL_EVENT_GAMEPAD_AXIS_MOTION:
            out.data = GamepadAxisEvent{e.gaxis.which, e.gaxis.axis, e.gaxis.value};
            break;
        case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
        case SDL_EVENT_GAMEPAD_BUTTON_UP:
            out.data = GamepadButtonEvent{e.gbutton.which, e.gbutton.button, e.gbutton.down};
            break;
        case SDL_EVENT_GAMEPAD_ADDED:
        case SDL_EVENT_GAMEPAD_REMOVED:
        case SDL_EVENT_GAMEPAD_REMAPPED:
            out.data = GamepadDeviceEvent{e.type, e.gdevice.which};
            break;
        case SDL_EVENT_DROP_FILE:
        case SDL_EVENT_DROP_TEXT:
            out.window = e.drop.windowID;
            out.data = DropEvent{
                ownedText(e.drop.data), e.type == SDL_EVENT_DROP_FILE, e.drop.x, e.drop.y};
            break;
        default:
            return std::nullopt;
    }
    return out;
}

struct SdlWindow::Impl {
    std::vector<std::weak_ptr<std::function<void()>>> cleanup;
    Subsystem video{SDL_INIT_VIDEO}; // destroyed after window
    std::unique_ptr<SDL_Window, WindowDeleter> window;
    WindowConfig config;
    WindowState state;
    bool sizeLocked{};

    explicit Impl(const WindowConfig& value) : config(value) {
        if (config.width <= 0 || config.height <= 0 || config.minWidth <= 0 ||
            config.minHeight <= 0 || config.maxWidth < 0 || config.maxHeight < 0 ||
            (config.maxWidth && config.maxWidth < config.minWidth) ||
            (config.maxHeight && config.maxHeight < config.minHeight)) {
            throw std::invalid_argument("Invalid window dimensions or limits");
        }
        auto flags = SDL_WindowFlags{0};
        if (config.hidden) {
            flags |= SDL_WINDOW_HIDDEN;
        }
        if (config.resizable) {
            flags |= SDL_WINDOW_RESIZABLE;
        }
        if (config.fullscreen) {
            flags |= SDL_WINDOW_FULLSCREEN;
        }
        if (config.highPixelDensity) {
            flags |= SDL_WINDOW_HIGH_PIXEL_DENSITY;
        }
        const auto title = cString(config.title);
        window.reset(SDL_CreateWindow(title.c_str(), config.width, config.height, flags));
        require(window != nullptr);
        require(SDL_SetWindowMinimumSize(window.get(), config.minWidth, config.minHeight));
        require(SDL_SetWindowMaximumSize(window.get(), config.maxWidth, config.maxHeight));
        refresh();
    }

    ~Impl() {
        for (const auto& weak : cleanup) {
            if (const auto callback = weak.lock()) {
                try {
                    (*callback)();
                } catch (...) {
                }
            }
        }
    }

    void refresh() {
        require(SDL_GetWindowSize(window.get(), &state.width, &state.height));
        require(SDL_GetWindowSizeInPixels(window.get(), &state.pixelWidth, &state.pixelHeight));
        require(SDL_GetWindowPosition(window.get(), &state.x, &state.y));
        const auto flags = SDL_GetWindowFlags(window.get());
        state.focused = (flags & SDL_WINDOW_INPUT_FOCUS) != 0;
        state.visible = (flags & SDL_WINDOW_HIDDEN) == 0;
        state.minimized = (flags & SDL_WINDOW_MINIMIZED) != 0;
        state.fullscreen = (flags & SDL_WINDOW_FULLSCREEN) != 0;
    }
};

SdlWindow::SdlWindow(const WindowConfig& config) : impl_(std::make_unique<Impl>(config)) {}

SdlWindow::~SdlWindow() = default;
SdlWindow::SdlWindow(SdlWindow&&) noexcept = default;
SdlWindow& SdlWindow::operator=(SdlWindow&&) noexcept = default;

WindowId SdlWindow::id() const noexcept {
    return impl_ ? SDL_GetWindowID(impl_->window.get()) : 0;
}

SDL_Window* SdlWindow::nativeHandle() const noexcept {
    return impl_ ? impl_->window.get() : nullptr;
}

std::shared_ptr<void> SdlWindow::beforeNativeDestroy(std::function<void()> callback) {
    if (!impl_ || !callback) {
        throw std::invalid_argument("Invalid native-window cleanup");
    }
    std::erase_if(impl_->cleanup, [](const auto& weak) {
        return weak.expired();
    });
    auto token = std::make_shared<std::function<void()>>(std::move(callback));
    impl_->cleanup.push_back(token);
    return token;
}

WindowState SdlWindow::state() const noexcept {
    return impl_ ? impl_->state : WindowState{};
}

void SdlWindow::setTitle(std::string_view text) {
    const auto title = cString(text);
    require(SDL_SetWindowTitle(nativeHandle(), title.c_str()));
}

void SdlWindow::setClientSize(Int32 width, Int32 height) {
    if (!impl_) {
        throw std::logic_error("Moved-from window");
    }
    if (width <= 0 || height <= 0) {
        throw std::invalid_argument("Invalid client size");
    }
    if (impl_->sizeLocked) {
        return;
    }
    require(SDL_SetWindowSize(nativeHandle(), width, height));
    impl_->refresh();
}

void SdlWindow::setPosition(Int32 x, Int32 y) {
    require(SDL_SetWindowPosition(nativeHandle(), x, y));
    if (impl_) {
        impl_->refresh();
    }
}

void SdlWindow::setSizeLocked(bool locked) {
    if (!impl_) {
        throw std::logic_error("Moved-from window");
    }
    require(SDL_SetWindowResizable(nativeHandle(), locked ? false : impl_->config.resizable));
    impl_->sizeLocked = locked;
}

void SdlWindow::setFullscreen(bool fullscreen) {
    require(SDL_SetWindowFullscreen(nativeHandle(), fullscreen));
}

void SdlWindow::show() {
    require(SDL_ShowWindow(nativeHandle()));
    impl_->refresh();
}

void SdlWindow::hide() {
    require(SDL_HideWindow(nativeHandle()));
    impl_->refresh();
}

void SdlWindow::startTextInput() {
    require(SDL_StartTextInput(nativeHandle()));
}

void SdlWindow::stopTextInput() {
    require(SDL_StopTextInput(nativeHandle()));
}

void SdlWindow::apply(const WindowEvent& e) {
    auto& s = impl_->state;
    switch (e.type) {
        case SDL_EVENT_WINDOW_FOCUS_GAINED:
            s.focused = true;
            break;
        case SDL_EVENT_WINDOW_FOCUS_LOST:
            s.focused = false;
            break;
        case SDL_EVENT_WINDOW_SHOWN:
            s.visible = true;
            break;
        case SDL_EVENT_WINDOW_HIDDEN:
            s.visible = false;
            break;
        case SDL_EVENT_WINDOW_MINIMIZED:
            s.minimized = true;
            break;
        case SDL_EVENT_WINDOW_RESTORED:
        case SDL_EVENT_WINDOW_MAXIMIZED:
            s.minimized = false;
            break;
        case SDL_EVENT_WINDOW_RESIZED:
            s.width = e.data1;
            s.height = e.data2;
            break;
        case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
            s.pixelWidth = e.data1;
            s.pixelHeight = e.data2;
            break;
        case SDL_EVENT_WINDOW_MOVED:
            s.x = e.data1;
            s.y = e.data2;
            break;
        case SDL_EVENT_WINDOW_ENTER_FULLSCREEN:
            s.fullscreen = true;
            break;
        case SDL_EVENT_WINDOW_LEAVE_FULLSCREEN:
            s.fullscreen = false;
            break;
        default:
            break;
    }
}

struct SdlApplication::Impl {
    LoopConfig config;
    std::unique_ptr<Subsystem> subsystems;
    std::unordered_map<WindowId, std::unique_ptr<SdlWindow>> windows;
    mutable std::mutex mutex;
    std::deque<Event> pending;

    struct Modal {
        WindowId window;
        std::function<void(const Event&)> input;
        std::function<bool()> active;
    };

    std::shared_ptr<Modal> modal;
    bool mainCallbacks{};
    std::optional<Int32> code;
    std::string error;
    std::optional<std::string> previousRate;
    bool attempted{}, active{}, stopped{}, background{}, rateSaved{}, accepting{};
    std::string currentRate;

    explicit Impl(LoopConfig value) : config(value) {
        if (config.maxPendingEvents == 0) {
            throw std::invalid_argument("Empty event queue limit");
        }
    }
};

SdlApplication::SdlApplication(LoopConfig config) : impl_(std::make_unique<Impl>(config)) {}

SdlApplication::~SdlApplication() {
    shutdown(result());
}

SdlWindow& SdlApplication::createWindow(const WindowConfig& config) {
    if (!impl_->active) {
        throw std::logic_error("Application is not active");
    }
    auto window = std::make_unique<SdlWindow>(config);
    const auto id = window->id();
    auto& ref = *window;
    impl_->windows.emplace(id, std::move(window));
    return ref;
}

SdlWindow* SdlApplication::findWindow(WindowId id) noexcept {
    const auto it = impl_->windows.find(id);
    return it == impl_->windows.end() ? nullptr : it->second.get();
}

void SdlApplication::destroyWindow(WindowId id) {
    retireWindow(id, true);
}

void SdlApplication::retireWindow(WindowId id, bool notify) {
    // SDL_DestroyWindow recursively frees native children. Retire their C++
    // owners first so GPU/input cleanup runs while each SDL_Window still lives.
    // Scan without allocations: this path is also used by noexcept shutdown.
    for (;;) {
        const auto* parent = findWindow(id);
        if (!parent) {
            return;
        }
        if (!parent->nativeHandle()) {
            break;
        }
        WindowId child{};
        for (const auto& [candidate, window] : impl_->windows) {
            if (candidate != id && window->nativeHandle() &&
                SDL_GetWindowParent(window->nativeHandle()) == parent->nativeHandle()) {
                child = candidate;
                break;
            }
        }
        if (!child) {
            break;
        }
        retireWindow(child, notify);
    }
    if (const auto* window = findWindow(id)) {
        const bool quit = window->impl_ && window->impl_->config.quitOnDestroy;
        impl_->windows.erase(id);
        if (quit && !impl_->stopped) {
            requestQuit();
        }
        if (notify) {
            onWindowDestroyed(id);
        }
    }
}

void SdlApplication::requestQuit(Int32 code) noexcept {
    std::scoped_lock lock(impl_->mutex);
    if (!impl_->code) {
        impl_->code = code;
    }
}

Int32 SdlApplication::exitCode() const noexcept {
    std::scoped_lock lock(impl_->mutex);
    return impl_->code.value_or(0);
}

std::string SdlApplication::error() const {
    std::scoped_lock lock(impl_->mutex);
    return impl_->error;
}

SDL_AppResult SdlApplication::result() const noexcept {
    std::scoped_lock lock(impl_->mutex);
    return !impl_->code ? SDL_APP_CONTINUE : *impl_->code == 0 ? SDL_APP_SUCCESS : SDL_APP_FAILURE;
}

void SdlApplication::fail(const char* message) noexcept {
    std::scoped_lock lock(impl_->mutex);
    impl_->code = 1;
    try {
        impl_->error = message;
    } catch (...) {
    }
    SDL_SetError("%s", message);
}

bool SdlApplication::foreground() const noexcept {
    return !impl_->background && std::ranges::any_of(impl_->windows, [](const auto& pair) {
        const auto s = pair.second->state();
        return s.focused && s.visible && !s.minimized;
    });
}

void SdlApplication::updateRate() {
    const bool throttle =
        impl_->config.checkFocus && !foreground() && impl_->config.nonfocusTimeoutMs;
    const auto rate =
        throttle ? 1000.0 / impl_->config.nonfocusTimeoutMs : impl_->config.foregroundRateHz;
    const auto value = std::to_string(rate);
    if (value != impl_->currentRate) {
        SDL_SetHint(SDL_HINT_MAIN_CALLBACK_RATE, value.c_str());
        impl_->currentRate = value;
    }
}

SDL_AppResult SdlApplication::initialize(std::span<const std::string_view> arguments,
                                         bool mainCallbacks) noexcept {
    if (impl_->attempted || impl_->stopped) {
        fail("Application already initialized or stopped");
        return result();
    }
    impl_->attempted = true;
    impl_->mainCallbacks = mainCallbacks;
    try {
        impl_->subsystems = std::make_unique<Subsystem>(requiredSubsystems(arguments));
        if (const auto* previous = SDL_GetHint(SDL_HINT_MAIN_CALLBACK_RATE)) {
            impl_->previousRate = previous;
        }
        impl_->rateSaved = true;
        impl_->active = true;
        {
            std::scoped_lock lock(impl_->mutex);
            impl_->accepting = true;
        }
        if (!onInit(arguments)) {
            fail("Application initialization failed");
        }
        updateRate();
    } catch (const std::exception& e) {
        fail(e.what());
    } catch (...) {
        fail("Unknown application initialization exception");
    }
    return result();
}

SDL_AppResult SdlApplication::event(const SDL_Event& e) noexcept {
    try {
        {
            std::scoped_lock lock(impl_->mutex);
            if (!impl_->accepting) {
                return SDL_APP_FAILURE;
            }
        }
        auto translated = translateEvent(e);
        if (translated) {
            // SDL requires lifecycle work to finish before returning to the OS.
            // Producer-thread lifecycle events are deferred to the main thread.
            if (std::holds_alternative<AppEvent>(translated->data) && e.type != SDL_EVENT_QUIT &&
                SDL_IsMainThread()) {
                drainEvents();
                if (result() == SDL_APP_CONTINUE) {
                    dispatch(*translated);
                    updateRate();
                }
                return result();
            }
            std::scoped_lock lock(impl_->mutex);
            if (impl_->accepting && !impl_->code) {
                if (impl_->pending.size() >= impl_->config.maxPendingEvents) {
                    throw std::length_error("Application event queue overflow");
                }
                impl_->pending.push_back(std::move(*translated));
            }
        }
    } catch (const std::exception& ex) {
        fail(ex.what());
    } catch (...) {
        fail("Unknown event translation exception");
    }
    return result();
}

void SdlApplication::dispatch(const Event& e) {
    const bool global = std::holds_alternative<AppEvent>(e.data) ||
                        std::holds_alternative<GamepadAxisEvent>(e.data) ||
                        std::holds_alternative<GamepadButtonEvent>(e.data) ||
                        std::holds_alternative<GamepadDeviceEvent>(e.data);
    if (!global && !findWindow(e.window)) {
        return;
    }
    const auto* windowEvent = std::get_if<WindowEvent>(&e.data);
    if (windowEvent) {
        if (auto* window = findWindow(e.window)) {
            window->apply(*windowEvent);
        }
    }
    if (const auto* app = std::get_if<AppEvent>(&e.data)) {
        if (app->type == SDL_EVENT_WILL_ENTER_BACKGROUND ||
            app->type == SDL_EVENT_DID_ENTER_BACKGROUND) {
            impl_->background = true;
        }
        if (app->type == SDL_EVENT_DID_ENTER_FOREGROUND) {
            impl_->background = false;
        }
    }
    const auto modal = impl_->modal;
    if (modal && e.window == modal->window && !global) {
        modal->input(e);
        return; // close and Alt+Enter belong to the modal host as well
    }
    if (modal && !global && !windowEvent) {
        return;
    }
    // Device arrival/removal remains live; gamepad input cannot execute
    // background editor commands.
    if (modal && global && !std::holds_alternative<AppEvent>(e.data) &&
        !std::holds_alternative<GamepadDeviceEvent>(e.data)) {
        return;
    }
    onEvent(e);
    if (result() != SDL_APP_CONTINUE) {
        return;
    }
    if (const auto* app = std::get_if<AppEvent>(&e.data)) {
        if (app->type == SDL_EVENT_QUIT || app->type == SDL_EVENT_TERMINATING) {
            requestQuit();
        }
    }
    if (windowEvent && windowEvent->type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) {
        if (findWindow(e.window) && onCloseRequested(e.window)) {
            destroyWindow(e.window);
        }
    }
    if (const auto* key = std::get_if<KeyEvent>(&e.data)) {
        if (!key->down && !key->repeat && key->key == SDLK_RETURN &&
            (key->modifiers & SDL_KMOD_ALT)) {
            if (auto* window = findWindow(e.window);
                window && window->impl_->config.altEnterFullscreen) {
                window->setFullscreen(
                    (SDL_GetWindowFlags(window->nativeHandle()) & SDL_WINDOW_FULLSCREEN) == 0);
            }
        }
    }
}

void SdlApplication::drainEvents() {
    std::size_t budget{};
    {
        std::scoped_lock lock(impl_->mutex);
        budget = impl_->pending.size();
    }
    while (budget-- && result() == SDL_APP_CONTINUE) {
        std::optional<Event> next;
        {
            std::scoped_lock lock(impl_->mutex);
            if (impl_->pending.empty()) {
                break;
            }
            next = std::move(impl_->pending.front());
            impl_->pending.pop_front();
        }
        const auto modal = impl_->modal;
        dispatch(*next);
        if (modal && !modal->active()) {
            break;
        }
    }
}

bool SdlApplication::runModal(WindowId owner,
                              WindowId dialog,
                              std::function<void(const Event&)> input,
                              std::function<bool()> active,
                              std::function<bool()> frame) {
    if (!SDL_IsMainThread() || !impl_->active || impl_->stopped || impl_->modal ||
        result() != SDL_APP_CONTINUE || owner == dialog || !findWindow(owner) ||
        !findWindow(dialog) || !input || !active || !frame) {
        return false;
    }
    auto modal =
        std::make_shared<Impl::Modal>(Impl::Modal{dialog, std::move(input), std::move(active)});
    impl_->modal = modal;

    struct Reset {
        std::shared_ptr<Impl::Modal>& slot;

        ~Reset() {
            slot.reset();
        }
    } reset{impl_->modal};

    const auto live = [&] {
        return result() == SDL_APP_CONTINUE && findWindow(owner) && findWindow(dialog);
    };
    try {
        while (live() && modal->active()) {
            if (!frame()) {
                return false;
            }
            if (!live() || !modal->active()) {
                break;
            }
            drainEvents();
            // SDL's callback watcher already delivers these lifecycle events
            // synchronously. Polling must not deliver them a second time.
            SDL_Event e{};
            for ([[maybe_unused]] auto n :
                 std::views::iota(unsigned{0}) | std::views::take_while([&](unsigned n) {
                     return n < 64 && live() && modal->active() && SDL_PollEvent(&e);
                 })) {
                const bool immediate = e.type == SDL_EVENT_TERMINATING ||
                                       e.type == SDL_EVENT_LOW_MEMORY ||
                                       (e.type >= SDL_EVENT_WILL_ENTER_BACKGROUND &&
                                        e.type <= SDL_EVENT_DID_ENTER_FOREGROUND);
                if (!impl_->mainCallbacks || !immediate) {
                    event(e);
                }
                drainEvents();
            }
            if (live() && modal->active()) {
                SDL_Delay(1);
            }
        }
        return live() && !modal->active();
    } catch (...) {
        return false;
    }
}

SDL_AppResult SdlApplication::iterate() noexcept {
    if (!impl_->active || impl_->stopped) {
        return SDL_APP_FAILURE;
    }
    try {
        drainEvents();
        updateRate();
        if (result() == SDL_APP_CONTINUE) {
            onIdle();
        }
    } catch (const std::exception& e) {
        fail(e.what());
    } catch (...) {
        fail("Unknown application callback exception");
    }
    return result();
}

void SdlApplication::shutdown(SDL_AppResult status) noexcept {
    if (impl_->stopped) {
        return;
    }
    impl_->stopped = true;
    impl_->active = false;
    {
        std::scoped_lock lock(impl_->mutex);
        impl_->accepting = false;
    }
    if (status == SDL_APP_FAILURE && result() != SDL_APP_FAILURE) {
        fail("SDL application failed");
    } else {
        requestQuit();
    }
    if (impl_->attempted) {
        try {
            onExit(exitCode());
        } catch (const std::exception& e) {
            fail(e.what());
        } catch (...) {
            fail("Unknown application exit exception");
        }
    }
    while (!impl_->windows.empty()) {
        retireWindow(impl_->windows.begin()->first, false);
    }
    {
        std::scoped_lock lock(impl_->mutex);
        impl_->pending.clear();
    }
    if (impl_->rateSaved) {
        if (impl_->previousRate) {
            SDL_SetHint(SDL_HINT_MAIN_CALLBACK_RATE, impl_->previousRate->c_str());
        } else {
            SDL_ResetHint(SDL_HINT_MAIN_CALLBACK_RATE);
        }
    }
    impl_->subsystems.reset();
}

SDL_AppResult iterateApplication(void* state) noexcept {
    return state ? static_cast<SdlApplication*>(state)->iterate() : SDL_APP_FAILURE;
}

SDL_AppResult handleApplicationEvent(void* state, SDL_Event* event) noexcept {
    return state && event ? static_cast<SdlApplication*>(state)->event(*event) : SDL_APP_FAILURE;
}

void quitApplication(void* state, SDL_AppResult result) noexcept {
    std::unique_ptr<SdlApplication> app(static_cast<SdlApplication*>(state));
    if (app) {
        app->shutdown(result);
    }
}
}
