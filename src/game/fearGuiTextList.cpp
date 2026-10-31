#include <opent1/game/shell_controls.hpp>

namespace opent1::game {
bool FGTextList::drawCell(
    gfx::SoftwareSurface& surface, Point2I pos, Point2I cell, bool chosen, bool hovered) const {
    if (cell.y < 0 || std::size_t(cell.y) >= entries.size()) {
        return false;
    }
    const auto& face = (chosen || hovered) && highlightFont ? highlightFont : font;
    return !face || surface.text(*face, {pos.x + 4, pos.y + verticalDelta}, entries[cell.y].text);
}
} // namespace opent1::game
