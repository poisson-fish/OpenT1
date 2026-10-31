#include <opent1/game/commander_huds.hpp>

namespace opent1::game {

namespace {
inline constexpr UInt32 kIdCtgCmdRadio0 = 140620U;
inline constexpr UInt32 kIdCtgCmdRadio4 = 140624U;
} // namespace

FGCommandPanel::FGCommandPanel() {
    fields_.extent = {160, 320};
}

bool FGCommandPanel::draw(gfx::SoftwareSurface& surface, Point2I offset, RectI clip) const {
    const RectI bounds{
        offset.x, offset.y, offset.x + fields_.extent.x - 1, offset.y + fields_.extent.y - 1};
    const auto corner = std::min({9, fields_.extent.x, fields_.extent.y});
    surface.fill({bounds.left, bounds.top, bounds.right, bounds.bottom - corner}, 247);
    for (Int32 row = 0; row < corner; ++row) {
        surface.line({bounds.left + row + 1, bounds.bottom - corner + row + 1},
                     {bounds.right, bounds.bottom - corner + row + 1},
                     247);
    }
    surface.line({bounds.left, bounds.top}, {bounds.right, bounds.top}, 248);
    surface.line({bounds.right, bounds.top}, {bounds.right, bounds.bottom}, 248);
    surface.line({bounds.right, bounds.bottom}, {bounds.left + corner, bounds.bottom}, 248);
    surface.line({bounds.left + corner, bounds.bottom}, {bounds.left, bounds.bottom - corner}, 248);
    surface.line({bounds.left, bounds.bottom - corner}, {bounds.left, bounds.top}, 248);
    return drawChildren(surface, offset, clip);
}

bool FGCommandPanel::message(gui::Control& sender, UInt32 msg) {
    if (msg >= kIdCtgCmdRadio0 && msg <= kIdCtgCmdRadio4) {
        activeSlide_ = static_cast<Int32>(msg - kIdCtgCmdRadio0);
        return true;
    }
    return Parent::message(sender, msg);
}

} // namespace opent1::game
