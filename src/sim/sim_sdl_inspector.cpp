#include <opent1/sim/sim_sdl_inspector.hpp>
#include <SDL3/SDL_keyboard.h>
#include <SDL3/SDL_keycode.h>
#include <SDL3/SDL_mouse.h>
#include <algorithm>
#include <cmath>

namespace opent1::sim {
SdlInspectorInput::SdlInspectorInput(platform::SdlWindow& window, SimInspector& inspector)
    : inspector_(inspector), window_(window.nativeHandle()), id_(window.id()),
      focused_(window.state().focused) {
    cleanup_ = window.beforeNativeDestroy([this] {
        release();
    });
}

SdlInspectorInput::~SdlInspectorInput() {
    cleanup_.reset();
    release();
}

void SdlInspectorInput::release() noexcept {
    if (window_ && textInput_) {
        SDL_StopTextInput(window_);
    }
    window_ = nullptr;
    textInput_ = focused_ = false;
}

void SdlInspectorInput::syncTextInput() {
    const bool enable = window_ && focused_ && inspector_.isOpen() &&
                        (inspector_.editing() || inspector_.choicesOpen());
    if (enable == textInput_) {
        return;
    }
    if (enable) {
        textInput_ = SDL_StartTextInput(window_);
    } else {
        if (window_) {
            SDL_StopTextInput(window_);
        }
        textInput_ = false;
    }
}

bool SdlInspectorInput::process(const platform::Event& event, SimGame& game) {
    if (!window_ || event.window != id_) {
        return false;
    }
    if (const auto* e = std::get_if<platform::WindowEvent>(&event.data)) {
        if (e->type == SDL_EVENT_WINDOW_FOCUS_GAINED) {
            focused_ = true;
        }
        if (e->type == SDL_EVENT_WINDOW_FOCUS_LOST || e->type == SDL_EVENT_WINDOW_HIDDEN) {
            focused_ = false;
        }
        syncTextInput();
        return false; // lifecycle events still belong to the host
    }
    if (!inspector_.isOpen() || !focused_) {
        syncTextInput();
        return false;
    }
    bool handled{};
    if (const auto* e = std::get_if<platform::KeyEvent>(&event.data)) {
        if (!e->down) {
            return true;
        }
        const bool ctrl = (e->modifiers & SDL_KMOD_CTRL) != 0;
        const bool shift = (e->modifiers & SDL_KMOD_SHIFT) != 0;
        switch (e->key) {
            case SDLK_TAB:
                inspector_.key(shift ? InspectorKey::Previous : InspectorKey::Next);
                break;
            case SDLK_RETURN:
            case SDLK_KP_ENTER:
                if (e->repeat) {
                    break;
                }
                if (ctrl) {
                    inspector_.apply(game);
                } else if (const auto index = inspector_.focused();
                           index &&
                           inspector_.fields()[*index].control == InspectorControl::Action) {
                    inspector_.activate(game, *index);
                } else {
                    inspector_.key(InspectorKey::Accept);
                }
                break;
            case SDLK_SPACE:
                if (!e->repeat && !inspector_.editing()) {
                    if (const auto index = inspector_.focused();
                        index && inspector_.fields()[*index].control == InspectorControl::Action) {
                        inspector_.activate(game, *index);
                    } else {
                        inspector_.key(InspectorKey::Accept);
                    }
                }
                break;
            case SDLK_LEFT:
                inspector_.key(InspectorKey::Left);
                break;
            case SDLK_RIGHT:
                inspector_.key(InspectorKey::Right);
                break;
            case SDLK_UP:
                inspector_.scroll(-1);
                break;
            case SDLK_DOWN:
                inspector_.scroll(1);
                break;
            case SDLK_PAGEUP:
                inspector_.scroll(-5);
                break;
            case SDLK_PAGEDOWN:
                inspector_.scroll(5);
                break;
            case SDLK_HOME:
                inspector_.key(InspectorKey::Home);
                break;
            case SDLK_END:
                inspector_.key(InspectorKey::End);
                break;
            case SDLK_BACKSPACE:
                inspector_.key(InspectorKey::Backspace);
                break;
            case SDLK_DELETE:
                inspector_.key(InspectorKey::Delete);
                break;
            case SDLK_ESCAPE:
                inspector_.key(InspectorKey::Cancel);
                break;
            case SDLK_A:
                if (ctrl) {
                    inspector_.key(InspectorKey::SelectAll);
                }
                break;
            default:
                break;
        }
        handled = true;
    } else if (const auto* text = std::get_if<platform::TextEvent>(&event.data)) {
        // Legacy inspectors edit bytes. UTF-8 input is retained as bytes; no
        // synthetic key-to-text mapping or partial IME composition is committed.
        if (!text->editing) {
            inspector_.insert(text->text);
        }
        handled = true;
    } else if (const auto* wheel = std::get_if<platform::MouseWheelEvent>(&event.data)) {
        if (std::isfinite(wheel->y)) {
            inspector_.scroll(Int32(std::clamp(-wheel->y * 3.f, -4096.f, 4096.f)));
        }
        handled = true;
    } else if (const auto* button = std::get_if<platform::MouseButtonEvent>(&event.data)) {
        if (button->down && button->button == SDL_BUTTON_LEFT && std::isfinite(button->x) &&
            std::isfinite(button->y)) {
            int width{}, height{};
            if (SDL_GetWindowSize(window_, &width, &height) && width > 0 && height > 0 &&
                button->x >= 0 && button->y >= 0 && button->x < float(width) &&
                button->y < float(height)) {
                const auto size = inspector_.size();
                inspector_.click({Int32(double(button->x) * size.x / width),
                                  Int32(double(button->y) * size.y / height)},
                                 game);
            }
        }
        handled = true;
    } else if (std::holds_alternative<platform::MouseMotionEvent>(event.data)) {
        handled = true;
    }
    syncTextInput();
    return handled;
}
} // namespace opent1::sim
