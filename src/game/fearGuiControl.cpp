#include <opent1/game/shell_controls.hpp>

namespace opent1::game {

bool FGControl::draw(gfx::SoftwareSurface& s, Point2I offset, RectI clip) const {
    return Parent::draw(s, offset, clip);
}

} // namespace opent1::game
