#include <opent1/gui/sdl_modal.hpp>
#include <ranges>
#include <opent1/gui/sdl_presentation.hpp>
#include <SDL3/SDL.h>

namespace opent1::gui {
SdlModalHost::SdlModalHost(platform::SdlApplication& app,
                           platform::WindowId owner,
                           gfx::GpuSurfaceConfig gpu,
                           bool hidden,
                           std::function<float()> requestedScale)
    : app_(app), owner_(owner), gpu_(std::move(gpu)), hidden_(hidden),
      requestedScale_(std::move(requestedScale)) {}

bool SdlModalHost::run(ModalView view) {
    const auto* parent = app_.findWindow(owner_);
    if (window_ || !parent ||
        (!hidden_ && (!parent->state().visible || parent->state().minimized)) || !view.canvas ||
        !view.active || !view.update || !view.cancel) {
        return false;
    }
    std::unique_ptr<SdlInput> input;
    std::unique_ptr<gfx::SdlGpuSurface> surface;
    Canvas* bound{};
    bool completed{};
    SdlPresentation presentation;
    try {
        platform::WindowConfig config;
        config.title = "OpenT1 Tools";
        config.hidden = true;
        config.resizable = false;
        config.altEnterFullscreen = false;
        auto& window = app_.createWindow(config);
        window_ = window.id();
        auto* owner = app_.findWindow(owner_);
        if (!owner || !SDL_SetWindowParent(window.nativeHandle(), owner->nativeHandle()) ||
            !SDL_SetWindowModal(window.nativeHandle(), true)) {
            throw std::runtime_error("modal window admission");
        }
        surface = gfx::SdlGpuSurface::create(window, 640, 480, gpu_);
        if (!surface) {
            throw std::runtime_error("modal GPU admission");
        }
        const auto bind = [&] {
            auto* current = &view.canvas();
            if (bound != current) {
                input.reset();
                bound = nullptr;
                auto* native = app_.findWindow(window_);
                if (!native) {
                    return false;
                }
                input = std::make_unique<SdlInput>(*native, *current);
                bound = current;
                if (!current->focus()) {
                    current->tab();
                }
                // The binding reads native focus. Preserve the dialog's initial
                // responder until an actual focus-loss event arrives.
            }
            return input->synchronize();
        };
        gfx::Palette palette;
        for (auto i : std::views::iota(unsigned{0}, unsigned{256})) {
            palette.palettes[0].colors[i] = {UInt8(i), UInt8(i), UInt8(i), 0};
        }
        const auto draw = [&] {
            if (!view.update()) {
                return false;
            }
            if (!view.active()) {
                return true;
            }
            if (!bind()) {
                return false;
            }
            auto* native = app_.findWindow(window_);
            if (!native) {
                return false;
            }
            return presentation.draw(
                *bound,
                *native,
                *surface,
                *input,
                palette,
                {640, 480},
                nativeUiScale(app_.findWindow(owner_), requestedScale_ ? requestedScale_() : 0),
                true);
        };
        if (!draw()) {
            throw std::runtime_error("modal first frame");
        }
        auto* admitted = app_.findWindow(window_);
        if (!admitted) {
            throw std::runtime_error("modal window retired");
        }
        if (!hidden_) {
            admitted->show();
            if (!SDL_RaiseWindow(admitted->nativeHandle())) {
                throw std::runtime_error("modal focus admission");
            }
        }
        completed = app_.runModal(
            owner_,
            window_,
            [&](const platform::Event& e) {
                if (!view.update() || !view.active() || !bind()) {
                    throw std::runtime_error("modal input preparation");
                }
                if (const auto* w = std::get_if<platform::WindowEvent>(&e.data);
                    w && w->type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) {
                    view.cancel();
                } else {
                    input->process(e);
                }
                if (!view.update()) {
                    throw std::runtime_error("modal input completion");
                }
            },
            view.active,
            draw);
    } catch (...) {
        completed = false;
    }
    // Unbind before retiring controls/window, including failed cancellation.
    input.reset();
    surface.reset();
    const auto retiring = window_;
    window_ = 0;
    try {
        if (auto* window = app_.findWindow(retiring)) {
            if (!SDL_SetWindowModal(window->nativeHandle(), false)) {
                completed = false;
            }
            app_.destroyWindow(retiring);
        }
    } catch (...) {
        completed = false;
    }
    if (!completed && view.active()) {
        try {
            view.cancel();
        } catch (...) {
        }
    }
    return completed;
}
}
