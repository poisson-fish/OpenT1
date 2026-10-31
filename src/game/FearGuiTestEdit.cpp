#include <opent1/game/shell_controls.hpp>

namespace opent1::game {
bool TestEdit::event(const gui::Event& event) {
    if (!active) {
        return Parent::event(event);
    }
    if (event.type == gui::EventType::KeyDown &&
        (event.key == gui::Key::Return || event.key == gui::Key::Escape) && canvas()) {
        auto* root = canvas()->top();
        auto* control = root->findNamed(event.key == gui::Key::Return ? "DialogReturnButton"
                                                                      : "DialogEscapeButton");
        if (auto* button = dynamic_cast<gui::Active*>(control)) {
            if (button->active) {
                button->action();
            }
            return true;
        }
    }
    const auto previous = text;
    const bool handled = Parent::event(event);
    // SDL composed text arrives separately from keydown (and paste may have no
    // key release). Publish the edit immediately for validation and variables.
    if (handled && text != previous) {
        action();
    }
    return handled;
}

void TestEdit::drawBackground(gfx::SoftwareSurface& surface, Point2I p) const {
    const auto x = p.x + fields_.extent.x - 1, y = p.y + fields_.extent.y - 1;
    surface.line({p.x, y - 1}, {x, y - 1}, active ? 240 : 236);
    surface.line({p.x + 1, y}, {x - 1, y}, active ? 239 : 235);
    surface.point({p.x, y - 1}, active ? 238 : 234);
    surface.point({x, y - 1}, active ? 238 : 234);
}
} // namespace opent1::game
