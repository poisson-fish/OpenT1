#include "test_support.hpp"
#include <opent1/platform/sdl_window.hpp>
#include <opent1/gui/sdl_gui.hpp>
#include <SDL3/SDL.h>
#include <thread>
#include <type_traits>
using namespace opent1;
using namespace opent1::platform;

namespace {
void canvasCursorOwnership() {
    SdlApplication app;
    CHECK(app.initialize({}) == SDL_APP_CONTINUE);
    WindowConfig config;
    config.hidden = true;
    auto& window = app.createWindow(config);
    gui::Canvas canvas;
    CHECK(SDL_ShowCursor());
    {
        gui::SdlInput input(window, canvas);
        const auto event = [&](UInt32 type) {
            (void)input.process(Event{window.id(), 0, WindowEvent{type}});
        };
        event(SDL_EVENT_WINDOW_FOCUS_GAINED);
        event(SDL_EVENT_WINDOW_MOUSE_ENTER);
        CHECK(!SDL_CursorVisible());
        event(SDL_EVENT_WINDOW_MOUSE_LEAVE);
        CHECK(SDL_CursorVisible());
        event(SDL_EVENT_WINDOW_MOUSE_ENTER);
        CHECK(!SDL_CursorVisible());
        event(SDL_EVENT_WINDOW_FOCUS_LOST);
        CHECK(SDL_CursorVisible());
        event(SDL_EVENT_WINDOW_FOCUS_GAINED);
        CHECK(!SDL_CursorVisible());
    }
    CHECK(SDL_CursorVisible());
    // An already hidden cursor belongs to another caller; do not expose it.
    CHECK(SDL_HideCursor());
    {
        gui::SdlInput input(window, canvas);
        (void)input.process(Event{window.id(), 0, WindowEvent{SDL_EVENT_WINDOW_FOCUS_GAINED}});
        (void)input.process(Event{window.id(), 0, WindowEvent{SDL_EVENT_WINDOW_MOUSE_ENTER}});
        (void)input.process(Event{window.id(), 0, WindowEvent{SDL_EVENT_WINDOW_MOUSE_LEAVE}});
        CHECK(!SDL_CursorVisible());
    }
    CHECK(!SDL_CursorVisible());
    CHECK(SDL_ShowCursor());
    {
        gui::SdlInput input(window, canvas);
        (void)input.process(Event{window.id(), 0, WindowEvent{SDL_EVENT_WINDOW_FOCUS_GAINED}});
        (void)input.process(Event{window.id(), 0, WindowEvent{SDL_EVENT_WINDOW_MOUSE_ENTER}});
        CHECK(!SDL_CursorVisible());
        app.destroyWindow(window.id());
        CHECK(SDL_CursorVisible() && input.synchronize());
    }
}

SDL_Event winEvent(WindowId id, UInt32 type, Int32 a = 0, Int32 b = 0) {
    SDL_Event e{};
    e.type = type;
    e.window.windowID = id;
    e.window.data1 = a;
    e.window.data2 = b;
    return e;
}

struct App : SdlApplication {
    using SdlApplication::SdlApplication;
    WindowId id{}, secondary{};
    int idle{}, exits{}, received{}, destroyed{};
    bool veto{}, throwEvent{}, throwInit{}, failInit{};
    std::string text;

    bool onInit(std::span<const std::string_view>) override {
        WindowConfig c;
        c.hidden = true;
        c.quitOnDestroy = true;
        id = createWindow(c).id();
        c.quitOnDestroy = false;
        secondary = createWindow(c).id();
        if (throwInit) {
            throw std::runtime_error("init probe");
        }
        return !failInit;
    }

    bool onCloseRequested(WindowId) override {
        return !veto;
    }

    void onEvent(const Event& e) override {
        ++received;
        if (throwEvent) {
            throw std::runtime_error("event probe");
        }
        if (const auto* w = std::get_if<WindowEvent>(&e.data)) {
            if (w->type == SDL_EVENT_WINDOW_FOCUS_GAINED) {
                CHECK(findWindow(e.window)->state().focused);
            }
            if (w->type == SDL_EVENT_WINDOW_FOCUS_LOST) {
                CHECK(!findWindow(e.window)->state().focused);
            }
        }
        if (const auto* t = std::get_if<TextEvent>(&e.data)) {
            text = t->text;
        }
    }

    void onIdle() override {
        ++idle;
    }

    void onWindowDestroyed(WindowId removed) override {
        ++destroyed;
        CHECK(!findWindow(removed));
    }

    void onExit(Int32) override {
        ++exits;
        CHECK(findWindow(id) || !findWindow(secondary));
    }
};

void windowOwnership() {
    static_assert(!std::is_copy_constructible_v<SdlWindow>);
    static_assert(std::is_nothrow_move_constructible_v<SdlWindow>);
    WindowConfig c;
    c.hidden = true;
    {
        SdlWindow a(c);
        const auto id = a.id();
        CHECK(id && a.nativeHandle());
        CHECK(a.state().width == 640 && a.state().pixelWidth > 0);
        CHECK(!a.state().visible);
        a.setTitle("OpenT1 UTF-8 \xc3\xa9");
        CHECK(std::string_view(SDL_GetWindowTitle(a.nativeHandle())) == "OpenT1 UTF-8 \xc3\xa9");
        a.setClientSize(320, 240);
        CHECK(a.state().width == 320);
        if (SDL_SetWindowResizable(a.nativeHandle(), false)) {
            CHECK(SDL_SetWindowResizable(a.nativeHandle(), true));
            a.setSizeLocked(true);
            a.setClientSize(800, 600);
            CHECK(a.state().width == 320);
            a.setSizeLocked(false);
        } else {
            // Newer SDL dummy drivers reject native resizability changes.
            // Surface that error without latching the wrapper's size lock.
            bool rejected{};
            try {
                a.setSizeLocked(true);
            } catch (const std::runtime_error&) {
                rejected = true;
            }
            CHECK(rejected);
            SDL_ClearError();
        }
        a.setClientSize(800, 600);
        CHECK(a.state().width == 800);
        a.startTextInput();
        CHECK(SDL_TextInputActive(a.nativeHandle()));
        a.stopTextInput();
        SdlWindow b(std::move(a));
        CHECK(a.id() == 0 && b.id() == id);
        SdlWindow d(c);
        const auto replaced = d.id();
        d = std::move(b);
        CHECK(!SDL_GetWindowFromID(replaced));
        CHECK(d.id() == id && !b.nativeHandle());
        try {
            d.setClientSize(0, 1);
            CHECK(false);
        } catch (const std::invalid_argument&) {
        }
        try {
            d.setTitle(std::string_view("a\0b", 3));
            CHECK(false);
        } catch (const std::invalid_argument&) {
        }
    }
    CHECK(!(SDL_WasInit(SDL_INIT_VIDEO) & SDL_INIT_VIDEO));
    c.width = 0;
    try {
        SdlWindow invalid(c);
        CHECK(false);
    } catch (const std::invalid_argument&) {
    }
    CHECK(!(SDL_WasInit(SDL_INIT_VIDEO) & SDL_INIT_VIDEO));
}

void translation() {
    SDL_Event e{};
    e.type = SDL_EVENT_TEXT_EDITING;
    e.edit.windowID = 12;
    e.edit.timestamp = 123;
    std::string source = "utf8 \xe2\x98\x83";
    e.edit.text = source.c_str();
    e.edit.start = 2;
    e.edit.length = 3;
    auto copy = translateEvent(e);
    source.clear();
    CHECK(copy->window == 12 && copy->timestamp == 123);
    CHECK(std::get<TextEvent>(copy->data).text == "utf8 \xe2\x98\x83");
    CHECK(std::get<TextEvent>(copy->data).editing && std::get<TextEvent>(copy->data).length == 3);
    e = {};
    e.type = SDL_EVENT_MOUSE_WHEEL;
    e.wheel.x = 1.5f;
    e.wheel.y = -2;
    e.wheel.direction = SDL_MOUSEWHEEL_FLIPPED;
    CHECK(std::get<MouseWheelEvent>(translateEvent(e)->data).x == -1.5f);
    e = {};
    e.type = SDL_EVENT_GAMEPAD_AXIS_MOTION;
    e.gaxis.which = 17;
    e.gaxis.value = -32768;
    CHECK(std::get<GamepadAxisEvent>(translateEvent(e)->data).value == -32768);
    e = {};
    e.type = SDL_EVENT_GAMEPAD_BUTTON_DOWN;
    e.gbutton.down = true;
    CHECK(std::get<GamepadButtonEvent>(translateEvent(e)->data).down);
    e = {};
    e.type = SDL_EVENT_GAMEPAD_REMOVED;
    e.gdevice.which = 33;
    CHECK(std::get<GamepadDeviceEvent>(translateEvent(e)->data).device == 33);
    e = {};
    e.type = SDL_EVENT_DROP_FILE;
    source = "mission.mis";
    e.drop.data = source.c_str();
    copy = translateEvent(e);
    source.clear();
    CHECK(std::get<DropEvent>(copy->data).data == "mission.mis");
    e = {};
    e.type = SDL_EVENT_USER;
    CHECK(!translateEvent(e));
    source.assign(MaxEventTextBytes + 1, 'x');
    e = {};
    e.type = SDL_EVENT_TEXT_INPUT;
    e.text.text = source.c_str();
    try {
        (void)translateEvent(e);
        CHECK(false);
    } catch (const std::length_error&) {
    }
}

void eventsOnlyApplication() {
    struct EventsOnlyApp : SdlApplication {
        SDL_InitFlags requiredSubsystems(std::span<const std::string_view>) const override {
            return SDL_INIT_EVENTS;
        }

        bool onInit(std::span<const std::string_view>) override {
            CHECK(SDL_WasInit(SDL_INIT_EVENTS));
            CHECK(!SDL_WasInit(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD));
            CHECK(!SDL_GetCurrentVideoDriver());
            return true;
        }

        void onIdle() override {
            requestQuit();
        }
    } app;
    CHECK(app.initialize({}) == SDL_APP_CONTINUE);
    CHECK(app.iterate() == SDL_APP_SUCCESS);
    app.shutdown(SDL_APP_SUCCESS);
    CHECK(!SDL_WasInit(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD | SDL_INIT_EVENTS));
}

void lifecycle() {
    SDL_SetHint(SDL_HINT_MAIN_CALLBACK_RATE, "77");
    App a;
    CHECK(a.initialize({}) == SDL_APP_CONTINUE);
    CHECK(!a.foreground());
    CHECK(std::stod(SDL_GetHint(SDL_HINT_MAIN_CALLBACK_RATE)) == 10);
    a.event(winEvent(a.id, SDL_EVENT_WINDOW_SHOWN));
    a.event(winEvent(a.id, SDL_EVENT_WINDOW_FOCUS_GAINED));
    CHECK(a.iterate() == SDL_APP_CONTINUE);
    CHECK(a.foreground() && a.idle == 1);
    CHECK(std::stod(SDL_GetHint(SDL_HINT_MAIN_CALLBACK_RATE)) == 0);
    a.event(winEvent(a.id, SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED, 1280, 960));
    a.event(winEvent(a.id, SDL_EVENT_WINDOW_RESIZED, 640, 480));
    a.event(winEvent(a.id, SDL_EVENT_WINDOW_FOCUS_LOST));
    CHECK(a.iterate() == SDL_APP_CONTINUE);
    CHECK(!a.foreground());
    CHECK(a.findWindow(a.id)->state().pixelWidth == 1280);
    const int before = a.received;
    a.event(winEvent(999999, SDL_EVENT_WINDOW_FOCUS_GAINED));
    a.iterate();
    CHECK(a.received == before);
    a.event(winEvent(0, SDL_EVENT_WINDOW_FOCUS_GAINED));
    a.iterate();
    CHECK(a.received == before);
    a.veto = true;
    a.event(winEvent(a.id, SDL_EVENT_WINDOW_CLOSE_REQUESTED));
    a.iterate();
    CHECK(a.findWindow(a.id));
    a.veto = false;
    a.event(winEvent(a.secondary, SDL_EVENT_WINDOW_CLOSE_REQUESTED));
    CHECK(a.iterate() == SDL_APP_CONTINUE);
    CHECK(!a.findWindow(a.secondary) && a.destroyed == 1);
    // The callback queue owns transient SDL text, including across a producer thread.
    std::thread producer([&] {
        std::string transient = "copied";
        SDL_Event e{};
        e.type = SDL_EVENT_TEXT_INPUT;
        e.text.windowID = a.id;
        e.text.text = transient.c_str();
        CHECK(a.event(e) == SDL_APP_CONTINUE);
    });
    producer.join();
    a.iterate();
    CHECK(a.text == "copied");
    const int idle = a.idle;
    a.event(winEvent(a.id, SDL_EVENT_WINDOW_CLOSE_REQUESTED));
    CHECK(a.iterate() == SDL_APP_SUCCESS);
    CHECK(a.idle == idle && !a.findWindow(a.id) && a.destroyed == 2);
    a.shutdown(SDL_APP_SUCCESS);
    a.shutdown(SDL_APP_SUCCESS);
    CHECK(a.exits == 1);
    CHECK(std::string_view(SDL_GetHint(SDL_HINT_MAIN_CALLBACK_RATE)) == "77");
    CHECK(!SDL_WasInit(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD));
}

void failurePaths() {
    {
        App a;
        a.shutdown(SDL_APP_SUCCESS);
        CHECK(a.initialize({}) == SDL_APP_FAILURE);
        CHECK(a.exits == 0);
    }
    {
        App a;
        a.throwInit = true;
        CHECK(a.initialize({}) == SDL_APP_FAILURE);
        CHECK(a.error() == "init probe");
        a.shutdown(SDL_APP_FAILURE);
        CHECK(a.exits == 1);
    }
    {
        App a;
        a.failInit = true;
        CHECK(a.initialize({}) == SDL_APP_FAILURE);
        a.shutdown(SDL_APP_FAILURE);
        CHECK(a.exits == 1);
    }
    {
        App a;
        a.initialize({});
        a.throwEvent = true;
        a.event(winEvent(a.id, SDL_EVENT_WINDOW_EXPOSED));
        CHECK(a.iterate() == SDL_APP_FAILURE);
        CHECK(a.error() == "event probe");
        a.shutdown(SDL_APP_FAILURE);
    }
    {
        App a;
        a.initialize({});
        a.requestQuit(17);
        a.requestQuit(0);
        CHECK(a.exitCode() == 17);
        CHECK(a.iterate() == SDL_APP_FAILURE);
        a.shutdown(SDL_APP_FAILURE);
    }
    {
        App a(LoopConfig{false, 100, 0, 1});
        a.initialize({});
        a.event(winEvent(a.id, SDL_EVENT_WINDOW_EXPOSED));
        CHECK(a.event(winEvent(a.id, SDL_EVENT_WINDOW_EXPOSED)) == SDL_APP_FAILURE);
        a.shutdown(SDL_APP_FAILURE);
    }
    {
        App a;
        a.initialize({});
        SDL_Event e{};
        e.type = SDL_EVENT_QUIT;
        a.event(e);
        CHECK(a.iterate() == SDL_APP_SUCCESS);
        CHECK(a.idle == 0);
        a.shutdown(SDL_APP_SUCCESS);
    }
    {
        App a;
        a.initialize({});
        SDL_Event e{};
        e.type = SDL_EVENT_TERMINATING;
        CHECK(a.event(e) == SDL_APP_SUCCESS);
        CHECK(a.received == 1 && a.idle == 0);
        a.shutdown(SDL_APP_SUCCESS);
    }
    {
        App a;
        a.initialize({});
        const int received = a.received;
        a.shutdown(SDL_APP_SUCCESS);
        CHECK(a.event(winEvent(a.id, SDL_EVENT_WINDOW_EXPOSED)) == SDL_APP_FAILURE);
        CHECK(a.received == received);
    }
    CHECK(!SDL_WasInit(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD));
}

void fullscreenShortcut() {
    App a;
    a.initialize({});
    SDL_Event e{};
    e.type = SDL_EVENT_KEY_DOWN;
    e.key.windowID = a.id;
    e.key.key = SDLK_RETURN;
    e.key.mod = SDL_KMOD_ALT;
    e.key.down = true;
    a.event(e);
    a.iterate();
    CHECK(!(SDL_GetWindowFlags(a.findWindow(a.id)->nativeHandle()) & SDL_WINDOW_FULLSCREEN));
    e.type = SDL_EVENT_KEY_UP;
    e.key.down = false;
    e.key.repeat = true;
    a.event(e);
    a.iterate();
    CHECK(!(SDL_GetWindowFlags(a.findWindow(a.id)->nativeHandle()) & SDL_WINDOW_FULLSCREEN));
    e.key.repeat = false;
    a.event(e);
    a.iterate();
    CHECK(SDL_GetWindowFlags(a.findWindow(a.id)->nativeHandle()) & SDL_WINDOW_FULLSCREEN);
    a.event(e);
    a.iterate();
    CHECK(!(SDL_GetWindowFlags(a.findWindow(a.id)->nativeHandle()) & SDL_WINDOW_FULLSCREEN));
    a.shutdown(SDL_APP_SUCCESS);
}

void modalPump() {
    struct ModalApp : App {
        bool active{}, entered{};
        std::vector<std::string> trace;

        void onEvent(const Event& e) override {
            App::onEvent(e);
            if (const auto* t = std::get_if<TextEvent>(&e.data)) {
                trace.push_back(t->text);
                if (t->text == "enter") {
                    entered = true;
                    active = true;
                    CHECK(runModal(
                        id,
                        secondary,
                        [&](const Event& modal) {
                            if (std::holds_alternative<TextEvent>(modal.data)) {
                                trace.push_back("modal");
                            }
                            if (const auto* w = std::get_if<WindowEvent>(&modal.data);
                                w && w->type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) {
                                active = false;
                                trace.push_back("close");
                            }
                        },
                        [&] {
                            return active;
                        },
                        [&] {
                            CHECK(!runModal(
                                id,
                                secondary,
                                [](const auto&) {},
                                [] {
                                    return false;
                                },
                                [] {
                                    return true;
                                }));
                            return true;
                        }));
                }
            }
        }
    } a;

    CHECK(a.initialize({}) == SDL_APP_CONTINUE);
    const auto send = [&](WindowId id, const char* value) {
        SDL_Event e{};
        e.type = SDL_EVENT_TEXT_INPUT;
        e.text.windowID = id;
        e.text.text = value;
        CHECK(a.event(e) == SDL_APP_CONTINUE);
    };
    send(a.id, "enter");
    send(a.secondary, "child");
    send(a.id, "blocked");
    a.event(winEvent(a.secondary, SDL_EVENT_WINDOW_CLOSE_REQUESTED));
    send(a.id, "after");
    CHECK(a.iterate() == SDL_APP_CONTINUE && a.entered && a.idle == 1 && a.findWindow(a.secondary));
    CHECK(a.trace == std::vector<std::string>({"enter", "modal", "close", "after"}));
    CHECK(!a.runModal(
        999,
        a.secondary,
        [](const auto&) {},
        [] {
            return true;
        },
        [] {
            return true;
        }));
    CHECK(!a.runModal(
        a.id,
        a.secondary,
        [](const auto&) {},
        [] {
            return true;
        },
        [] {
            return false;
        }));
    CHECK(!a.runModal(
        a.id,
        a.secondary,
        [](const auto&) {},
        [] {
            return true;
        },
        []() -> bool {
            throw std::bad_alloc();
        }));
    CHECK(a.runModal(
        a.id,
        a.secondary,
        [](const auto&) {},
        [] {
            return false;
        },
        [] {
            return true;
        }));
    a.event(winEvent(a.id, SDL_EVENT_WINDOW_CLOSE_REQUESTED));
    CHECK(!a.runModal(
              a.id,
              a.secondary,
              [](const auto&) {},
              [] {
                  return true;
              },
              [] {
                  return true;
              }) &&
          !a.findWindow(a.id));
    a.shutdown(SDL_APP_SUCCESS);
}

void ownedChildWindows() {
    for (bool shutdown : {false, true}) {
        SdlApplication app;
        CHECK(app.initialize({}) == SDL_APP_CONTINUE);
        WindowConfig config;
        config.hidden = true;
        auto& parent = app.createWindow(config);
        const auto parentId = parent.id();
        auto& child = app.createWindow(config);
        const auto childId = child.id();
        auto& grandchild = app.createWindow(config);
        const auto grandchildId = grandchild.id();
        CHECK(SDL_SetWindowParent(child.nativeHandle(), parent.nativeHandle()));
        CHECK(SDL_SetWindowParent(grandchild.nativeHandle(), child.nativeHandle()));
        std::vector<int> trace;
        auto p = parent.beforeNativeDestroy([&] {
            CHECK(SDL_GetWindowFromID(parentId) && !SDL_GetWindowFromID(childId));
            trace.push_back(1);
        });
        auto c = child.beforeNativeDestroy([&] {
            CHECK(SDL_GetWindowFromID(childId) && !SDL_GetWindowFromID(grandchildId));
            trace.push_back(2);
        });
        auto g = grandchild.beforeNativeDestroy([&] {
            CHECK(SDL_GetWindowFromID(grandchildId));
            trace.push_back(3);
        });
        if (shutdown) {
            app.shutdown(SDL_APP_SUCCESS);
        } else {
            app.destroyWindow(parentId);
        }
        CHECK(!app.findWindow(parentId) && !app.findWindow(childId) &&
              !app.findWindow(grandchildId));
        CHECK(trace == std::vector<int>({3, 2, 1}));
    }
}
}

int opent1_test_platform_window(int argc, char**) {
    return test::run([&] {
        if (argc > 1) {
            ownedChildWindows();
            return;
        }
        windowOwnership();
        canvasCursorOwnership();
        translation();
        eventsOnlyApplication();
        lifecycle();
        failurePaths();
        fullscreenShortcut();
        modalPump();
    });
}

namespace {
int opent1_test_platform_window_default() {
    char program[] = "opent1_tests";
    char* args[] = {program, nullptr};
    return opent1_test_platform_window(1, args);
}
}

#include "test_registry.hpp"
OPENT1_REGISTER_TEST("platform", "platform_window", opent1_test_platform_window_default);
