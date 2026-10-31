#pragma once

#include <opent1/core/types.hpp>
#include <SDL3/SDL_events.h>
#include <cstddef>
#include <optional>
#include <string>
#include <variant>

namespace opent1::platform {
using WindowId = UInt32;
inline constexpr std::size_t MaxEventTextBytes = 1024 * 1024;

struct WindowEvent {
    UInt32 type{};
    Int32 data1{}, data2{};
};

struct KeyEvent {
    UInt32 scancode{}, key{};
    UInt16 modifiers{};
    bool down{}, repeat{};
};

struct TextEvent {
    std::string text;
    bool editing{};
    Int32 start{}, length{};
};

struct MouseMotionEvent {
    UInt32 device{}, buttons{};
    float x{}, y{}, dx{}, dy{};
};

struct MouseButtonEvent {
    UInt32 device{};
    UInt8 button{}, clicks{};
    bool down{};
    float x{}, y{};
};

struct MouseWheelEvent {
    UInt32 device{};
    float x{}, y{};
};

struct GamepadAxisEvent {
    UInt32 device{};
    UInt8 axis{};
    Int16 value{};
};

struct GamepadButtonEvent {
    UInt32 device{};
    UInt8 button{};
    bool down{};
};

struct GamepadDeviceEvent {
    UInt32 type{}, device{};
};

struct AppEvent {
    UInt32 type{};
};

struct DropEvent {
    std::string data;
    bool file{};
    float x{}, y{};
};

// Owned data: safe to retain after SDL_AppEvent returns, including text and drops.
// SDL key/scancode/modifier/button values deliberately remain SDL3 values.
struct Event {
    WindowId window{};  // zero for application and gamepad events
    UInt64 timestamp{}; // SDL nanoseconds
    std::variant<WindowEvent,
                 KeyEvent,
                 TextEvent,
                 MouseMotionEvent,
                 MouseButtonEvent,
                 MouseWheelEvent,
                 GamepadAxisEvent,
                 GamepadButtonEvent,
                 GamepadDeviceEvent,
                 AppEvent,
                 DropEvent>
        data;
};

[[nodiscard]] std::optional<Event> translateEvent(const SDL_Event& event);
}
