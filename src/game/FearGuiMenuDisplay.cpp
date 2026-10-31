#include <opent1/game/chat_inventory_huds.hpp>

namespace opent1::game {
bool MenuDisplayCtrl::event(const gui::Event& event) {
    if (event.type == gui::EventType::Down && event.button == 1 && selectEntry && font &&
        font->info.height > 0) {
        const auto point = globalToLocal(event.point);
        const auto rowHeight = font->info.height;
        if (point.x >= 0 && point.x < fields_.extent.x && point.y >= rowHeight) {
            const auto row = std::size_t(point.y / rowHeight - 1);
            if (row < entries.size()) {
                const auto select = selectEntry;
                if (auto* host = canvas()) {
                    host->afterDispatch([select, row] {
                        select(row);
                    });
                } else {
                    select(row);
                }
                return true;
            }
        }
    }
    return Parent::event(event);
}

bool MenuDisplayCtrl::draw(gfx::SoftwareSurface& surface, Point2I offset, RectI clip) const {
    if (font && font->info.height > 0) {
        if (!surface.text(*font, offset, heading)) {
            return false;
        }
        // Legacy samples the canvas cursor every frame, including immediately
        // after opening a menu beneath a stationary pointer.
        const auto* host = canvas();
        const auto point = host ? globalToLocal(host->cursorPosition()) : Point2I{-1, -1};
        const auto rowHeight = font->info.height;
        Int32 y = rowHeight;
        for (const auto& entry : entries) {
            const bool hovered = point.x >= 0 && point.x < fields_.extent.x && point.y >= y &&
                                 point.y < y + rowHeight;
            const auto& rowFont = hovered && highlightFont ? *highlightFont : *font;
            if (!surface.text(rowFont, offset + Point2I{0, y}, entry)) {
                return false;
            }
            y += rowHeight;
        }
    }
    return Parent::draw(surface, offset, clip);
}
} // namespace opent1::game
