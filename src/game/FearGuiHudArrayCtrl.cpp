#include <opent1/game/hud_controls.hpp>

namespace opent1::game {

FGHudArrayCtrl::FGHudArrayCtrl() {
    columnsSortable_ = false;
    columnsResizeable_ = false;
}

bool FGHudArrayCtrl::drawCell(gfx::SoftwareSurface& surface,
                              Point2I position,
                              Point2I cell,
                              bool chosen,
                              bool hovered) const {
    const auto& font = (chosen || hovered) && shellHighlightFont ? shellHighlightFont : shellFont;
    if (!font) {
        return true;
    }
    if (chosen) {
        surface.fill(
            {position.x, position.y, position.x + cellSize.x - 1, position.y + cellSize.y - 1},
            BOX_FRAME);
    }
    const auto value = getCellText(cell);
    const auto tab = value.find('\t');
    const auto right =
        tab == value.npos ? std::string_view{} : std::string_view(value).substr(tab + 1);
    const auto width = static_cast<Int32>(font->textWidth(right));
    const auto label = truncateInfoText(
        std::string_view(value).substr(0, tab), cellSize.x - width - 12, font.get());
    return surface.text(*font, position + Point2I{4, 2}, label) &&
           (right.empty() ||
            surface.text(*font, position + Point2I{cellSize.x - width - 4, 2}, right));
}

bool FGHudArrayCtrl::onAdd() {
    if (!Parent::onAdd()) {
        return false;
    }
    columnsSortable_ = false;
    columnsResizeable_ = false;
    return true;
}

} // namespace opent1::game
