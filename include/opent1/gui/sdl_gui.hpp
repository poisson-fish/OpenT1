#pragma once
#include <opent1/gui/gui.hpp>
#include <opent1/gui/presentation.hpp>
#include <opent1/platform/sdl_window.hpp>

namespace opent1::gui {
// One binding per canvas. Canvas must outlive the binding. Window destruction
// invalidates native access through SdlWindow's pre-destruction callback.
class SdlInput {
public:
    SdlInput(platform::SdlWindow&, Canvas&);
    ~SdlInput();
    SdlInput(const SdlInput&) = delete;
    SdlInput& operator=(const SdlInput&) = delete;
    bool process(const platform::Event&);
    bool synchronize();
    bool enabled(bool mouse, bool value);

    // A fixed framebuffer may be presented at a different window size.
    void logicalExtent(Point2I extent) {
        presentation(Presentation{.extent = extent});
    }

    void presentation(Presentation);

    [[nodiscard]] bool enabled(bool mouse) const {
        return mouse ? mouseEnabled_ : keyboardEnabled_;
    }

private:
    void release() noexcept;
    std::optional<Point2I> position(float x, float y) const;
    Canvas& canvas_;
    SDL_Window* window_{};
    platform::WindowId id_{};
    bool focused_{}, textInput_{};
    Point2I lastPoint_{};
    std::shared_ptr<void> cleanup_;
    bool mouseEnabled_{true}, keyboardEnabled_{true};
    UInt16 modifiers_{};
    Point2I logicalExtent_{};
    Presentation presentation_{};
    std::optional<Point2F> lastClientPoint_;
};
} // namespace opent1::gui
