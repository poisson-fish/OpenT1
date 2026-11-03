#include <opent1/game/shell_controls.hpp>

namespace opent1::game {
bool FGTextList::drawCell(
    gfx::SoftwareSurface& surface, Point2I pos, Point2I cell, bool chosen, bool /*hovered*/) const {
    if (cell.y < 0 || std::size_t(cell.y) >= entries.size()) {
        return false;
    }
    if (chosen) {
        surface.fill({pos.x, pos.y, pos.x + cellSize.x - 1, pos.y + cellSize.y - 1}, 254);
    }
    const auto& face = chosen && highlightFont ? highlightFont : font;
    return !face || surface.text(*face, {pos.x + 10, pos.y + verticalDelta}, entries[cell.y].text);
}
} // namespace opent1::game
