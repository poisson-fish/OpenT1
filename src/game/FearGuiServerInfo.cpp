#include "opent1/game/server_irc_controls.hpp"

namespace opent1::game {

ServerInfoCtrl::ServerInfoCtrl() {
    columnInfo_ = {
        ColumnInfo{0, 10, 640, 0, 0, true, 160, "TEAM"},
        ColumnInfo{1, 10, 640, 1, 1, true, 80, "SCORE"},
    };
    setSize({1, 0});
}

void ServerInfoCtrl::setServer(const ServerListCtrl::ServerInfo& server) {
    currentServer_ = server;
    setSize({1, static_cast<Int32>(currentServer_.detailLines.size())});
}

std::string ServerInfoCtrl::getCellText(Point2I cell) const {
    if (cell.y < 0 || static_cast<std::size_t>(cell.y) >= currentServer_.detailLines.size()) {
        return {};
    }
    const auto& line = currentServer_.detailLines[static_cast<std::size_t>(cell.y)];
    return line.size() > 2 ? line.substr(2) : std::string{};
}

bool ServerInfoCtrl::drawCell(
    gfx::SoftwareSurface& surface, Point2I offset, Point2I cell, bool, bool) const {
    if (cell.y < 0 || static_cast<std::size_t>(cell.y) >= currentServer_.detailLines.size()) {
        return true;
    }
    const auto& row = currentServer_.detailLines[static_cast<std::size_t>(cell.y)];
    if (row.size() < 2) {
        return true;
    }
    const auto& heading = row[0] == '0' ? currentServer_.teamHeading : currentServer_.playerHeading;
    std::vector<Int32> stops{0};
    for (std::size_t i = 0; i + 1 < heading.size() && stops.size() < 16; ++i) {
        if (heading[i] == '\t') {
            stops.push_back(static_cast<UInt8>(heading[++i]));
        }
    }
    stops.push_back(640);
    const auto font =
        row[1] == '1' ? shellFont : (shellHighlightFont ? shellHighlightFont : shellFont);
    if (!font) {
        return true;
    }
    std::string_view text(row);
    text.remove_prefix(2);
    std::size_t column{};
    while (!text.empty() && column + 1 < stops.size()) {
        const auto end = text.find('\t');
        const auto part = text.substr(0, end);
        const auto clip = surface.clip();
        if (const auto bounded = gfx::intersect(clip,
                                                {offset.x + stops[column],
                                                 offset.y,
                                                 offset.x + stops[column + 1] - 2,
                                                 offset.y + cellSize.y - 1})) {
            surface.setClip(*bounded);
            const bool ok = surface.text(*font, {offset.x + stops[column] + 4, offset.y}, part);
            surface.setClip(clip);
            if (!ok) {
                return false;
            }
        }
        if (end == text.npos) {
            break;
        }
        text.remove_prefix(end + 1);
        if (row[1] == '1' && !text.empty()) {
            text.remove_prefix(1);
        }
        ++column;
    }
    return true;
}

} // namespace opent1::game
