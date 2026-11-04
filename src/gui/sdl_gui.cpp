#include <opent1/gui/sdl_gui.hpp>
#include <SDL3/SDL_clipboard.h>
#include <SDL3/SDL_keyboard.h>
#include <SDL3/SDL_keycode.h>
#include <SDL3/SDL_mouse.h>
#include <cmath>

namespace opent1::gui {
namespace {
Key key(UInt32 k) {
    switch (k) {
        case SDLK_RETURN:
        case SDLK_KP_ENTER:
            return Key::Return;
        case SDLK_TAB:
            return Key::Tab;
        case SDLK_LEFT:
            return Key::Left;
        case SDLK_RIGHT:
            return Key::Right;
        case SDLK_UP:
            return Key::Up;
        case SDLK_DOWN:
            return Key::Down;
        case SDLK_HOME:
            return Key::Home;
        case SDLK_END:
            return Key::End;
        case SDLK_DELETE:
            return Key::Delete;
        case SDLK_BACKSPACE:
            return Key::Backspace;
        case SDLK_INSERT:
            return Key::Insert;
        case SDLK_ESCAPE:
            return Key::Escape;
        case SDLK_PAGEUP:
            return Key::PageUp;
        case SDLK_PAGEDOWN:
            return Key::PageDown;
        case SDLK_A:
            return Key::A;
        case SDLK_C:
            return Key::C;
        case SDLK_V:
            return Key::V;
        case SDLK_X:
            return Key::X;
        case SDLK_Z:
            return Key::Z;
        case SDLK_U:
            return Key::U;
        case SDLK_F1:
            return Key::F1;
        case SDLK_F2:
            return Key::F2;
        case SDLK_F3:
            return Key::F3;
        case SDLK_F4:
            return Key::F4;
        case SDLK_F5:
            return Key::F5;
        case SDLK_F6:
            return Key::F6;
        case SDLK_F7:
            return Key::F7;
        case SDLK_F9:
            return Key::F9;
        case SDLK_F10:
            return Key::F10;
        default:
            return Key::None;
    }
}
}

void SdlInput::presentation(Presentation value) {
    presentation_ = value;
    logicalExtent_ = value.extent;
    // Resize/DPI/preference changes can occur without a new motion event.
    // Reproject the stationary cursor and the position used by wheel events.
    if (lastClientPoint_) {
        if (auto p = position(lastClientPoint_->x, lastClientPoint_->y)) {
            lastPoint_ = *p;
            (void)canvas_.setCursorPosition(*p);
        }
    }
}

std::optional<Point2I> SdlInput::position(float x, float y) const {
    if (logicalExtent_.x > 0 && logicalExtent_.y > 0) {
        int width{}, height{};
        if (!window_ || !SDL_GetWindowSize(window_, &width, &height) || width <= 0 || height <= 0) {
            return {};
        }
        if (presentation_.viewport.valid()) {
            x = (x * float(presentation_.pixels.x) / float(width) - presentation_.viewport.left) *
                float(logicalExtent_.x) / float(presentation_.viewport.width());
            y = (y * float(presentation_.pixels.y) / float(height) - presentation_.viewport.top) *
                float(logicalExtent_.y) / float(presentation_.viewport.height());
        } else {
            x *= float(logicalExtent_.x) / float(width);
            y *= float(logicalExtent_.y) / float(height);
        }
    }
    if (!std::isfinite(x) || !std::isfinite(y) || std::abs(x) > Control::MaxCoordinate ||
        std::abs(y) > Control::MaxCoordinate) {
        return {};
    }
    return Point2I{Int32(std::floor(x)), Int32(std::floor(y))};
}

SdlInput::SdlInput(platform::SdlWindow& window, Canvas& canvas)
    : canvas_(canvas), window_(window.nativeHandle()), id_(window.id()),
      focused_(window.state().focused), mouseFocused_(SDL_GetMouseFocus() == window_) {
    if (canvas_.captureMouse || canvas_.readClipboard || canvas_.writeClipboard) {
        throw std::logic_error("GUI canvas already has a platform binding");
    }
    cleanup_ = window.beforeNativeDestroy([this] {
        release();
    });
    canvas_.readClipboard = [this]() -> std::optional<std::string> {
        if (!window_) {
            return {};
        }
        char* text = SDL_GetClipboardText();
        if (!text) {
            return {};
        }
        const auto deleter = [](char* p) {
            SDL_free(p);
        };
        std::unique_ptr<char, decltype(deleter)> owner(text, deleter);
        std::size_t n{};
        while (n <= 65536 && text[n]) {
            ++n;
        }
        if (n > 65536) {
            return {};
        }
        return std::string(text, n);
    };
    canvas_.writeClipboard = [this](std::string_view s) {
        if (!window_ || s.size() > 65536 || s.find('\0') != std::string_view::npos) {
            return false;
        }
        return SDL_SetClipboardText(std::string(s).c_str());
    };
    canvas_.captureMouse = [this](bool capture) {
        return window_ && SDL_CaptureMouse(capture);
    };
}

SdlInput::~SdlInput() {
    cleanup_.reset();
    release();
    canvas_.readClipboard = {};
    canvas_.writeClipboard = {};
    canvas_.captureMouse = {};
}

void SdlInput::release() noexcept {
    try {
        canvas_.cancel();
    } catch (...) {
    }
    if (window_ && textInput_) {
        SDL_StopTextInput(window_);
    }
    if (cursorHidden_) {
        (void)SDL_ShowCursor();
        cursorHidden_ = false;
    }
    window_ = nullptr;
    textInput_ = focused_ = mouseFocused_ = false;
}

bool SdlInput::synchronize() {
    // Tribes draws the mod's cursor into the canvas. The OS pointer otherwise
    // sits on top of it; restore it when the mouse leaves this canvas/window.
    if (window_ && focused_ && mouseFocused_) {
        if (!cursorHidden_ && SDL_CursorVisible()) {
            if (!SDL_HideCursor()) {
                return false;
            }
            cursorHidden_ = true;
        }
    } else if (cursorHidden_) {
        if (!SDL_ShowCursor()) {
            return false;
        }
        cursorHidden_ = false;
    }
    const bool enable =
        window_ && focused_ && keyboardEnabled_ && dynamic_cast<TextEdit*>(canvas_.focus());
    if (enable == textInput_) {
        return true;
    }
    if (!window_) {
        return !enable;
    }
    const bool ok = enable ? SDL_StartTextInput(window_) : SDL_StopTextInput(window_);
    if (ok) {
        textInput_ = enable;
    }
    return ok;
}

bool SdlInput::enabled(bool mouse, bool value) {
    if (!window_) {
        return false;
    }
    if (mouse) {
        mouseEnabled_ = value;
        return true;
    }
    const auto previous = keyboardEnabled_;
    keyboardEnabled_ = value;
    if (!synchronize()) {
        keyboardEnabled_ = previous;
        return false;
    }
    return true;
}

bool SdlInput::process(const platform::Event& input) {
    if (!window_ || input.window != id_) {
        return false;
    }
    if (const auto* e = std::get_if<platform::WindowEvent>(&input.data)) {
        if (e->type == SDL_EVENT_WINDOW_MOUSE_ENTER) {
            mouseFocused_ = true;
        } else if (e->type == SDL_EVENT_WINDOW_MOUSE_LEAVE) {
            mouseFocused_ = false;
        }
        if (e->type == SDL_EVENT_WINDOW_FOCUS_GAINED) {
            focused_ = true;
            modifiers_ = SDL_GetModState();
        }
        if (e->type == SDL_EVENT_WINDOW_FOCUS_LOST || e->type == SDL_EVENT_WINDOW_HIDDEN) {
            canvas_.cancel();
            focused_ = false;
            if (e->type == SDL_EVENT_WINDOW_HIDDEN) {
                mouseFocused_ = false;
            }
            modifiers_ = 0;
        }
        synchronize();
        return false;
    }
    if (!focused_) {
        return false;
    }
    Event e;
    e.shift = (modifiers_ & SDL_KMOD_SHIFT) != 0;
    e.control = (modifiers_ & SDL_KMOD_CTRL) != 0;
    if (const auto* k = std::get_if<platform::KeyEvent>(&input.data)) {
        modifiers_ = k->modifiers;
        if (!keyboardEnabled_) {
            return false;
        }
        e.type = k->down ? EventType::KeyDown : EventType::KeyUp;
        e.key = key(k->key);
        e.repeat = k->repeat;
        e.shift = (k->modifiers & SDL_KMOD_SHIFT) != 0;
        e.control = (k->modifiers & SDL_KMOD_CTRL) != 0;
        e.alt = (k->modifiers & SDL_KMOD_ALT) != 0;
        e.text = SDL_GetKeyName(k->key);
    } else if (const auto* text = std::get_if<platform::TextEvent>(&input.data)) {
        if (!keyboardEnabled_) {
            return false;
        }
        if (text->editing) {
            return dynamic_cast<TextEdit*>(canvas_.focus()) != nullptr;
        }
        e.type = EventType::Text;
        e.text = text->text;
    } else if (const auto* motion = std::get_if<platform::MouseMotionEvent>(&input.data)) {
        if (!mouseEnabled_) {
            return false;
        }
        auto p = position(motion->x, motion->y);
        if (!p) {
            return false;
        }
        lastClientPoint_ = Point2F{motion->x, motion->y};
        lastPoint_ = e.point = *p;
        e.type = motion->buttons ? EventType::Drag : EventType::Move;
        if (motion->buttons) {
            e.button = (motion->buttons & SDL_BUTTON_LMASK)   ? 1
                       : (motion->buttons & SDL_BUTTON_RMASK) ? 3
                                                              : 2;
        }
    } else if (const auto* button = std::get_if<platform::MouseButtonEvent>(&input.data)) {
        if (!mouseEnabled_) {
            return false;
        }
        auto p = position(button->x, button->y);
        if (!p) {
            return false;
        }
        lastClientPoint_ = Point2F{button->x, button->y};
        lastPoint_ = e.point = *p;
        e.type = button->down ? EventType::Down : EventType::Up;
        e.button = button->button;
        e.clicks = button->clicks;
    } else if (const auto* wheel = std::get_if<platform::MouseWheelEvent>(&input.data)) {
        if (!mouseEnabled_) {
            return false;
        }
        if (!std::isfinite(wheel->y)) {
            return false;
        }
        e.type = EventType::Wheel;
        e.wheel = wheel->y;
        e.point = lastPoint_;
    } else {
        return false;
    }
    const bool handled = canvas_.dispatch(e);
    synchronize();
    return handled;
}
} // namespace opent1::gui
