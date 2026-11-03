#include <opent1/game/hud_controls.hpp>
#include <ranges>

#include <algorithm>
#include <cmath>

namespace opent1::game {

FGArrayCtrl::FGArrayCtrl() {
    cellSize = {640, 16};
}

void FGArrayCtrl::wake() {
    if (cells.x == 1 && parent()) {
        cellSize.x = std::max(1, parent()->fields().extent.x);
    }
    Parent::wake();
}

bool FGArrayCtrl::drawCell(
    gfx::SoftwareSurface& s, Point2I p, Point2I cell, bool chosen, bool hovered) const {
    const auto f = (chosen || hovered) && shellHighlightFont ? shellHighlightFont : shellFont;
    if (chosen) {
        s.fill({p.x, p.y, p.x + cellSize.x - 1, p.y + cellSize.y - 1}, BOX_FRAME);
    }
    if (!f) {
        return true;
    }
    const auto text = truncateInfoText(getCellText(cell), cellSize.x - 8, f.get());
    return s.text(*f, {p.x + 4, p.y + 2}, text);
}

std::function<void()> FGArrayCtrl::checkpoint() {
    auto base = Parent::checkpoint();
    const auto f = shellFont, h = shellHighlightFont;
    return [this, base = std::move(base), f, h] {
        base();
        shellFont = f;
        shellHighlightFont = h;
    };
}

void FGArrayCtrl::setColumns(std::vector<ColumnInfo> cols) {
    columnInfo_ = std::move(cols);
    refresh_ = true;
}

bool FGArrayCtrl::clickHeader(Point2I localPt, bool rightClick) {
    if (!columnsSortable_ || columnInfo_.empty() || localPt.x < 0) {
        return false;
    }
    Int32 curX = 0;
    for (auto pos : std::views::iota(std::size_t{0}, columnInfo_.size())) {
        for (auto i : std::views::iota(std::size_t{0}, columnInfo_.size())) {
            const auto& col = columnInfo_[i];
            if (!col.active || col.position != static_cast<Int32>(pos)) {
                continue;
            }
            if (localPt.x >= curX && localPt.x < curX + col.width) {
                const Int32 colIdx = static_cast<Int32>(i);
                if (rightClick) {
                    if (!columnsDualSortable_) {
                        return false;
                    }
                    if (columnToSortSecond_ == colIdx) {
                        sortAscendingSecond_ = !sortAscendingSecond_;
                    } else {
                        columnToSortSecond_ = colIdx;
                        sortAscendingSecond_ = true;
                    }
                } else {
                    if (columnToSort_ == colIdx) {
                        sortAscending_ = !sortAscending_;
                    } else {
                        columnToSort_ = colIdx;
                        sortAscending_ = true;
                    }
                }
                refresh_ = true;
                return true;
            }
            curX += col.width;
        }
    }
    return false;
}

bool FGArrayCtrl::beginResizeColumn(Point2I localPt) {
    if (!columnsResizeable_ || columnInfo_.empty()) {
        return false;
    }
    Int32 curX = 0;
    for (auto pos : std::views::iota(std::size_t{0}, columnInfo_.size())) {
        for (auto i : std::views::iota(std::size_t{0}, columnInfo_.size())) {
            const auto& col = columnInfo_[i];
            if (!col.active || col.position != static_cast<Int32>(pos)) {
                continue;
            }
            curX += col.width;
            if (std::abs(localPt.x - curX) <= 2) {
                columnToResize_ = static_cast<Int32>(i);
                dragPoint_ = localPt;
                startWidth_ = col.width;
                return true;
            }
        }
    }
    return false;
}

bool FGArrayCtrl::dragResizeColumn(Point2I localPt) {
    if (!columnsResizeable_ || columnToResize_ < 0 ||
        static_cast<std::size_t>(columnToResize_) >= columnInfo_.size()) {
        return false;
    }
    auto& col = columnInfo_[static_cast<std::size_t>(columnToResize_)];
    const Int32 dx = localPt.x - dragPoint_.x;
    const Int32 minW = std::max<Int32>(10, col.minWidth);
    const Int32 maxW = std::max<Int32>(minW, col.maxWidth);
    col.width = std::clamp<Int32>(startWidth_ + dx, minW, maxW);
    return true;
}

void FGArrayCtrl::endResizeColumn() noexcept {
    columnToResize_ = -1;
}

std::string FGArrayCtrl::truncateInfoText(std::string_view text,
                                          Int32 maxPixels,
                                          const gfx::BitmapFont* font,
                                          Int32 fallbackCharWidth) {
    if (maxPixels <= 0 || text.empty()) {
        return {};
    }
    const auto measure = [font, fallbackCharWidth](std::string_view s) -> Int32 {
        if (font != nullptr) {
            return static_cast<Int32>(font->textWidth(s));
        }
        return static_cast<Int32>(s.size()) * std::max<Int32>(1, fallbackCharWidth);
    };
    if (measure(text) <= maxPixels) {
        return std::string(text);
    }
    constexpr std::string_view kEllipsis = "...";
    const Int32 ellipsisWidth = measure(kEllipsis);
    if (ellipsisWidth > maxPixels) {
        return {};
    }
    std::size_t len = text.size();
    while (len > 0 && measure(text.substr(0, len)) + ellipsisWidth > maxPixels) {
        --len;
    }
    std::string out(text.substr(0, len));
    out.append(kEllipsis);
    return out;
}

std::string FGArrayCtrl::getCellText(Point2I) const {
    return {};
}

} // namespace opent1::game
