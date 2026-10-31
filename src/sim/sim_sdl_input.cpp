#include <opent1/sim/sim_sdl_input.hpp>
#include <SDL3/SDL_gamepad.h>
#include <SDL3/SDL_init.h>
#include <SDL3/SDL_scancode.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <stdexcept>
#include <set>

namespace opent1::sim {
namespace {
struct ScanCode {
    UInt8 legacy;
    SDL_Scancode sdl;
};

constexpr auto scanCodes = std::to_array<ScanCode>({{1, SDL_SCANCODE_ESCAPE},
                                                    {2, SDL_SCANCODE_1},
                                                    {3, SDL_SCANCODE_2},
                                                    {4, SDL_SCANCODE_3},
                                                    {5, SDL_SCANCODE_4},
                                                    {6, SDL_SCANCODE_5},
                                                    {7, SDL_SCANCODE_6},
                                                    {8, SDL_SCANCODE_7},
                                                    {9, SDL_SCANCODE_8},
                                                    {10, SDL_SCANCODE_9},
                                                    {11, SDL_SCANCODE_0},
                                                    {12, SDL_SCANCODE_MINUS},
                                                    {13, SDL_SCANCODE_EQUALS},
                                                    {14, SDL_SCANCODE_BACKSPACE},
                                                    {15, SDL_SCANCODE_TAB},
                                                    {16, SDL_SCANCODE_Q},
                                                    {17, SDL_SCANCODE_W},
                                                    {18, SDL_SCANCODE_E},
                                                    {19, SDL_SCANCODE_R},
                                                    {20, SDL_SCANCODE_T},
                                                    {21, SDL_SCANCODE_Y},
                                                    {22, SDL_SCANCODE_U},
                                                    {23, SDL_SCANCODE_I},
                                                    {24, SDL_SCANCODE_O},
                                                    {25, SDL_SCANCODE_P},
                                                    {26, SDL_SCANCODE_LEFTBRACKET},
                                                    {27, SDL_SCANCODE_RIGHTBRACKET},
                                                    {28, SDL_SCANCODE_RETURN},
                                                    {29, SDL_SCANCODE_LCTRL},
                                                    {30, SDL_SCANCODE_A},
                                                    {31, SDL_SCANCODE_S},
                                                    {32, SDL_SCANCODE_D},
                                                    {33, SDL_SCANCODE_F},
                                                    {34, SDL_SCANCODE_G},
                                                    {35, SDL_SCANCODE_H},
                                                    {36, SDL_SCANCODE_J},
                                                    {37, SDL_SCANCODE_K},
                                                    {38, SDL_SCANCODE_L},
                                                    {39, SDL_SCANCODE_SEMICOLON},
                                                    {40, SDL_SCANCODE_APOSTROPHE},
                                                    {41, SDL_SCANCODE_GRAVE},
                                                    {42, SDL_SCANCODE_LSHIFT},
                                                    {43, SDL_SCANCODE_BACKSLASH},
                                                    {44, SDL_SCANCODE_Z},
                                                    {45, SDL_SCANCODE_X},
                                                    {46, SDL_SCANCODE_C},
                                                    {47, SDL_SCANCODE_V},
                                                    {48, SDL_SCANCODE_B},
                                                    {49, SDL_SCANCODE_N},
                                                    {50, SDL_SCANCODE_M},
                                                    {51, SDL_SCANCODE_COMMA},
                                                    {52, SDL_SCANCODE_PERIOD},
                                                    {53, SDL_SCANCODE_SLASH},
                                                    {54, SDL_SCANCODE_RSHIFT},
                                                    {55, SDL_SCANCODE_KP_MULTIPLY},
                                                    {56, SDL_SCANCODE_LALT},
                                                    {57, SDL_SCANCODE_SPACE},
                                                    {58, SDL_SCANCODE_CAPSLOCK},
                                                    {59, SDL_SCANCODE_F1},
                                                    {60, SDL_SCANCODE_F2},
                                                    {61, SDL_SCANCODE_F3},
                                                    {62, SDL_SCANCODE_F4},
                                                    {63, SDL_SCANCODE_F5},
                                                    {64, SDL_SCANCODE_F6},
                                                    {65, SDL_SCANCODE_F7},
                                                    {66, SDL_SCANCODE_F8},
                                                    {67, SDL_SCANCODE_F9},
                                                    {68, SDL_SCANCODE_F10},
                                                    {69, SDL_SCANCODE_NUMLOCKCLEAR},
                                                    {70, SDL_SCANCODE_SCROLLLOCK},
                                                    {71, SDL_SCANCODE_KP_7},
                                                    {72, SDL_SCANCODE_KP_8},
                                                    {73, SDL_SCANCODE_KP_9},
                                                    {74, SDL_SCANCODE_KP_MINUS},
                                                    {75, SDL_SCANCODE_KP_4},
                                                    {76, SDL_SCANCODE_KP_5},
                                                    {77, SDL_SCANCODE_KP_6},
                                                    {78, SDL_SCANCODE_KP_PLUS},
                                                    {79, SDL_SCANCODE_KP_1},
                                                    {80, SDL_SCANCODE_KP_2},
                                                    {81, SDL_SCANCODE_KP_3},
                                                    {82, SDL_SCANCODE_KP_0},
                                                    {83, SDL_SCANCODE_KP_PERIOD},
                                                    {86, SDL_SCANCODE_NONUSBACKSLASH},
                                                    {87, SDL_SCANCODE_F11},
                                                    {88, SDL_SCANCODE_F12},
                                                    {100, SDL_SCANCODE_F13},
                                                    {101, SDL_SCANCODE_F14},
                                                    {102, SDL_SCANCODE_F15},
                                                    {141, SDL_SCANCODE_KP_EQUALS},
                                                    {156, SDL_SCANCODE_KP_ENTER},
                                                    {157, SDL_SCANCODE_RCTRL},
                                                    {179, SDL_SCANCODE_KP_COMMA},
                                                    {181, SDL_SCANCODE_KP_DIVIDE},
                                                    {183, SDL_SCANCODE_PRINTSCREEN},
                                                    {184, SDL_SCANCODE_RALT},
                                                    {197, SDL_SCANCODE_PAUSE},
                                                    {199, SDL_SCANCODE_HOME},
                                                    {200, SDL_SCANCODE_UP},
                                                    {201, SDL_SCANCODE_PAGEUP},
                                                    {203, SDL_SCANCODE_LEFT},
                                                    {205, SDL_SCANCODE_RIGHT},
                                                    {207, SDL_SCANCODE_END},
                                                    {208, SDL_SCANCODE_DOWN},
                                                    {209, SDL_SCANCODE_PAGEDOWN},
                                                    {210, SDL_SCANCODE_INSERT},
                                                    {211, SDL_SCANCODE_DELETE},
                                                    {219, SDL_SCANCODE_LGUI},
                                                    {220, SDL_SCANCODE_RGUI},
                                                    {221, SDL_SCANCODE_APPLICATION}});

struct CloseGamepad {
    void operator()(SDL_Gamepad* pad) const {
        SDL_CloseGamepad(pad);
    }
};
}

struct SdlSimInput::Impl {
    SimManager& manager;
    SimAddress input;
    platform::WindowId window;
    bool captured{true}, focused{true};
    std::array<bool, 32> active{};
    std::set<std::pair<UInt8, UInt32>> inactive;

    struct Pad {
        UInt32 id{}, slot{};
        std::unique_ptr<SDL_Gamepad, CloseGamepad> handle;
        std::array<bool, 4> dpad{};
    };

    std::vector<Pad> pads;

    Impl(SimManager& m, SimAddress i, platform::WindowId w) : manager(m), input(i), window(w) {
        active.fill(true);
    }

    SimInputManager* service() const {
        return dynamic_cast<SimInputManager*>(input.resolve(manager));
    }

    bool send(UInt8 type, UInt32 device, UInt8 object, UInt8 instance, UInt8 action, float value) {
        if (!captured || !focused || !active[type] || inactive.contains({type, device}) ||
            !std::isfinite(value)) {
            return false;
        }
        SimInputEvent event;
        event.deviceType = type;
        event.deviceInst = device;
        event.objType = object;
        event.objInst = instance;
        event.action = action;
        event.fValue = value;
        if (auto* target = service()) {
            return target->submit(event);
        }
        return false;
    }

    void open(UInt32 id) {
        if (pads.size() >= 32 || std::ranges::any_of(pads, [&](const auto& pad) {
                return pad.id == id;
            })) {
            return;
        }
        auto handle = std::unique_ptr<SDL_Gamepad, CloseGamepad>(SDL_OpenGamepad(id));
        if (!handle) {
            return;
        }
        const auto candidates = std::views::iota(UInt32{0}, UInt32{32});
        const auto slot = *std::ranges::find_if(candidates, [&](auto value) {
            return std::ranges::none_of(pads, [&](const auto& pad) {
                return pad.slot == value;
            });
        });
        pads.push_back({id, slot, std::move(handle), {}});
    }
};

SdlSimInput::SdlSimInput(SimInputManager& input, platform::WindowId window) {
    if (!input.getManager() || !window) {
        throw std::invalid_argument("SDL input requires a registered service and window");
    }
    impl_ = std::make_unique<Impl>(*input.getManager(), input.address(), window);
    if (!SDL_InitSubSystem(SDL_INIT_GAMEPAD)) {
        throw std::runtime_error(SDL_GetError());
    }
    try {
        int count = 0;
        auto* ids = SDL_GetGamepads(&count);
        std::unique_ptr<SDL_JoystickID, decltype(&SDL_free)> owner(ids, SDL_free);
        if (ids && count > 0) {
            std::ranges::for_each(std::span(ids, static_cast<std::size_t>(count)), [&](auto id) {
                impl_->open(id);
            });
        }
    } catch (...) {
        impl_->pads.clear();
        SDL_QuitSubSystem(SDL_INIT_GAMEPAD);
        throw;
    }
}

SdlSimInput::~SdlSimInput() {
    // Destructors never call user simulation callbacks. Release explicitly before
    // destroying a live adapter when held actions need notification.
    impl_->pads.clear();
    SDL_QuitSubSystem(SDL_INIT_GAMEPAD);
}

UInt8 SdlSimInput::legacyScanCode(UInt32 scan) {
    const auto found = std::ranges::find_if(scanCodes, [&](auto pair) {
        return static_cast<UInt32>(pair.sdl) == scan;
    });
    return found == scanCodes.end() ? 0 : found->legacy;
}

void SdlSimInput::release() {
    if (auto* service = impl_->service()) {
        service->release();
    }
}

void SdlSimInput::capture(bool enabled) {
    impl_->captured = enabled;
    if (!enabled) {
        release();
    }
}

void SdlSimInput::activate(UInt8 type, bool enabled) {
    if (type >= impl_->active.size()) {
        return;
    }
    impl_->active[type] = enabled;
    if (!enabled) {
        std::vector<UInt32> slots;
        if (type == InputJoystick) {
            std::ranges::transform(impl_->pads, std::back_inserter(slots), [](const auto& pad) {
                return pad.slot;
            });
        } else {
            slots.push_back(0);
        }
        std::ranges::for_each(slots, [&](auto slot) {
            if (auto* service = impl_->service()) {
                service->releaseDevice(type, slot);
            }
        });
    }
}

std::size_t SdlSimInput::gamepadCount() const {
    return impl_->pads.size();
}

std::vector<UInt32> SdlSimInput::gamepadSlots() const {
    std::vector<UInt32> slots;
    for (const auto& pad : impl_->pads) {
        slots.push_back(pad.slot);
    }
    return slots;
}

void SdlSimInput::activateDevice(UInt8 type, UInt32 instance, bool enabled) {
    if (enabled) {
        impl_->inactive.erase({type, instance});
    } else {
        impl_->inactive.emplace(type, instance);
        if (auto* service = impl_->service()) {
            service->releaseDevice(type, instance);
        }
    }
}

std::optional<UInt32> SdlSimInput::gamepadSlot(UInt32 id) const {
    const auto found = std::ranges::find_if(impl_->pads, [&](const auto& pad) {
        return pad.id == id;
    });
    return found == impl_->pads.end() ? std::nullopt : std::optional(found->slot);
}

bool SdlSimInput::process(const platform::Event& event) {
    auto& state = *impl_;
    if (event.window && event.window != state.window) {
        return false;
    }
    if (const auto* window = std::get_if<platform::WindowEvent>(&event.data)) {
        if (window->type == SDL_EVENT_WINDOW_FOCUS_LOST ||
            window->type == SDL_EVENT_WINDOW_HIDDEN || window->type == SDL_EVENT_WINDOW_MINIMIZED) {
            state.focused = false;
            release();
        } else if (window->type == SDL_EVENT_WINDOW_FOCUS_GAINED) {
            state.focused = true;
        }
        return false;
    }
    if (const auto* app = std::get_if<platform::AppEvent>(&event.data)) {
        if (app->type == SDL_EVENT_WILL_ENTER_BACKGROUND) {
            state.focused = false;
            release();
        } else if (app->type == SDL_EVENT_DID_ENTER_FOREGROUND) {
            state.focused = true;
        }
        return false;
    }
    if (const auto* device = std::get_if<platform::GamepadDeviceEvent>(&event.data)) {
        if (device->type == SDL_EVENT_GAMEPAD_ADDED) {
            state.open(device->device);
        } else if (device->type == SDL_EVENT_GAMEPAD_REMOVED) {
            if (auto slot = gamepadSlot(device->device)) {
                // Erase before callbacks so reentry never retains a removed handle.
                std::erase_if(state.pads, [&](const auto& pad) {
                    return pad.id == device->device;
                });
                if (auto* service = state.service()) {
                    service->releaseDevice(InputJoystick, *slot);
                }
            }
        }
        return false;
    }
    bool handled = false;
    if (const auto* key = std::get_if<platform::KeyEvent>(&event.data)) {
        const auto scan = legacyScanCode(key->scancode);
        return scan && !key->repeat &&
               state.send(InputKeyboard,
                          0,
                          InputKey,
                          scan,
                          key->down ? InputMake : InputBreak,
                          key->down ? 1.f : 0.f);
    }
    if (const auto* mouse = std::get_if<platform::MouseMotionEvent>(&event.data)) {
        if (mouse->dx != 0) {
            handled |= state.send(InputMouse, 0, InputXAxis, 0, InputMove, mouse->dx);
        }
        if (mouse->dy != 0) {
            handled |= state.send(InputMouse, 0, InputYAxis, 0, InputMove, mouse->dy);
        }
    } else if (const auto* button = std::get_if<platform::MouseButtonEvent>(&event.data)) {
        // SDL right/middle are 3/2; legacy button1 is right, button2 middle.
        const auto instance = button->button == 3   ? 1
                              : button->button == 2 ? 2
                                                    : static_cast<int>(button->button) - 1;
        if (instance >= 0) {
            handled = state.send(InputMouse,
                                 0,
                                 InputButton,
                                 static_cast<UInt8>(instance),
                                 button->down ? InputMake : InputBreak,
                                 button->down ? 1.f : 0.f);
        }
    } else if (const auto* wheel = std::get_if<platform::MouseWheelEvent>(&event.data)) {
        // Preserve DirectInput wheel units: one notch is 120, including fractions.
        handled = state.send(InputMouse, 0, InputZAxis, 0, InputMove, wheel->y * 120.f);
    } else if (const auto* axis = std::get_if<platform::GamepadAxisEvent>(&event.data)) {
        constexpr std::array<UInt8, 6> types{
            InputXAxis, InputYAxis, InputRxAxis, InputRyAxis, InputZAxis, InputRzAxis};
        if (auto slot = gamepadSlot(axis->device); slot && axis->axis < types.size()) {
            const auto value = axis->axis >= SDL_GAMEPAD_AXIS_LEFT_TRIGGER
                                   ? std::max(0.f, float(axis->value) / 32767.f)
                                   : (float(axis->value) + 32768.f) / 65535.f;
            handled = state.send(InputJoystick, *slot, types[axis->axis], 0, InputMove, value);
        }
    } else if (const auto* padButton = std::get_if<platform::GamepadButtonEvent>(&event.data)) {
        if (auto slot = gamepadSlot(padButton->device);
            slot && padButton->button < SDL_GAMEPAD_BUTTON_COUNT) {
            handled = state.send(InputJoystick,
                                 *slot,
                                 InputButton,
                                 padButton->button,
                                 padButton->down ? InputMake : InputBreak,
                                 padButton->down ? 1.f : 0.f);
            if (padButton->button >= SDL_GAMEPAD_BUTTON_DPAD_UP &&
                padButton->button <= SDL_GAMEPAD_BUTTON_DPAD_RIGHT) {
                const auto index = padButton->button - SDL_GAMEPAD_BUTTON_DPAD_UP;
                constexpr std::array<UInt8, 4> types{
                    InputUpPov, InputDownPov, InputLeftPov, InputRightPov};
                // Re-resolve after callbacks, which can remove a gamepad.
                auto pad = std::ranges::find_if(state.pads, [&](const auto& value) {
                    return value.id == padButton->device;
                });
                if (pad == state.pads.end()) {
                    return handled;
                }
                pad->dpad[index] = padButton->down;
                const auto dpad = pad->dpad;
                handled |= state.send(InputJoystick,
                                      *slot,
                                      types[index],
                                      0,
                                      padButton->down ? InputMake : InputBreak,
                                      padButton->down ? 1.f : 0.f);
                handled |= state.send(InputJoystick,
                                      *slot,
                                      InputXPov,
                                      0,
                                      InputMove,
                                      .5f + .5f * (int(dpad[3]) - int(dpad[2])));
                handled |= state.send(InputJoystick,
                                      *slot,
                                      InputYPov,
                                      0,
                                      InputMove,
                                      .5f + .5f * (int(dpad[1]) - int(dpad[0])));
            }
        }
    }
    return handled;
}
} // namespace opent1::sim
