#include <opent1/game/shell_controls.hpp>

namespace opent1::game {

bool FearGuiBox::onAdd() {
    ghosted_ = false;
    return Parent::onAdd();
}

bool FearGuiBox::draw(gfx::SoftwareSurface& s, Point2I offset, RectI clip) const {
    const RectI r{
        offset.x, offset.y, offset.x + fields_.extent.x - 1, offset.y + fields_.extent.y - 1};
    if (fields_.style.opaque) {
        s.fill(r, fields_.style.fill);
    }
    const bool ghost = ghosted_ || (canvas() && !canvas()->contains(this));
    const UInt8 inside = ghost ? 236 : 240, outside = ghost ? 235 : 239, corner = ghost ? 234 : 238;
    for (int i = 0; i < 2; ++i) {
        const auto color = i ? inside : outside;
        s.line({r.left + 4, r.top + i}, {r.right - 4, r.top + i}, color);
        s.line({r.left + 4, r.bottom - i}, {r.right - 4, r.bottom - i}, color);
        s.line({r.left + i, r.top + 4}, {r.left + i, r.bottom - 4}, color);
        s.line({r.right - i, r.top + 4}, {r.right - i, r.bottom - 4}, color);
    }
    for (const int x : {r.left + 1, r.right - 1}) {
        for (const int y : {r.top + 3, r.bottom - 3}) {
            s.point({x, y}, corner);
        }
    }
    for (const int x : {r.left + 3, r.right - 3}) {
        for (const int y : {r.top + 1, r.bottom - 1}) {
            s.point({x, y}, corner);
        }
    }
    s.rectangle({r.left + 3, r.top + 3, r.right - 3, r.bottom - 3}, ghost ? 233 : 237);
    return drawChildren(s, offset, clip);
}

} // namespace opent1::game
