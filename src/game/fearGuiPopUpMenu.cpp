#include "opent1/game/shell_controls.hpp"

#include <algorithm>

namespace opent1::game {

FGPopUpMenu::FGPopUpMenu() {
    fields_.extent = {150, 0};
}

void FGPopUpMenu::appendMenu(UInt32 flags, UInt32 tag, std::string_view text) {
    Int32 curY = 4;
    if (!items_.empty()) {
        curY = items_.back().offset.y + items_.back().extent.y;
    }
    MenuEntry entry{};
    entry.targeted = false;
    entry.tag = tag;
    entry.flags = flags;
    entry.text = std::string(text);
    entry.offset = {4, curY};
    const Int32 height = ((flags & MENU_SEPARATOR) != 0U) ? 8 : 18;
    const Int32 textWidth = std::max(150, static_cast<Int32>(text.size()) * 8 + 24);
    entry.extent = {textWidth, height};
    items_.push_back(std::move(entry));

    Int32 maxW = 150;
    for (const auto& item : items_) {
        maxW = std::max(maxW, item.extent.x + 8);
    }
    for (auto& item : items_) {
        item.extent.x = maxW - 8;
    }
    resize(fields_.position, {maxW, curY + height + 4});
}

void FGPopUpMenu::clearMenu() noexcept {
    items_.clear();
    resize(fields_.position, {150, 0});
}

std::optional<UInt32> FGPopUpMenu::selectAt(Point2I localPt) {
    std::optional<UInt32> hitTag{};
    for (auto& item : items_) {
        const bool inside =
            localPt.x >= item.offset.x && localPt.x < (item.offset.x + item.extent.x) &&
            localPt.y >= item.offset.y && localPt.y < (item.offset.y + item.extent.y);
        const bool actionable =
            (item.flags & MENU_SEPARATOR) == 0U && (item.flags & MENU_DISABLED) == 0U;
        item.targeted = inside && actionable;
        if (item.targeted && !hitTag.has_value()) {
            hitTag = item.tag;
        }
    }
    return hitTag;
}

} // namespace opent1::game
