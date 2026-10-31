#include "opent1/game/server_irc_controls.hpp"

namespace opent1::game {

ServerDetailCtrl::ServerDetailCtrl() {
    columnInfo_ = {
        ColumnInfo{0, 10, 640, 0, 0, true, 140, "PLAYER"},
        ColumnInfo{1, 10, 640, 1, 1, true, 80, "TRIBE"},
        ColumnInfo{2, 10, 640, 2, 2, true, 60, "SCORE"},
        ColumnInfo{3, 10, 640, 3, 3, true, 60, "PING"},
    };
    setSize({4, 0});
}

void ServerDetailCtrl::setServer(const ServerListCtrl::ServerInfo& server) {
    currentServer_ = server;
    setSize({4,
             static_cast<Int32>(currentServer_.detailLines.empty()
                                    ? currentServer_.players.size()
                                    : currentServer_.detailLines.size())});
}

std::string ServerDetailCtrl::getCellText(Point2I cell) const {
    if (!currentServer_.detailLines.empty()) {
        if (cell.y < 0 || static_cast<std::size_t>(cell.y) >= currentServer_.detailLines.size()) {
            return {};
        }
        std::string_view line = currentServer_.detailLines[static_cast<std::size_t>(cell.y)];
        if (line.size() < 2) {
            return {};
        }
        line.remove_prefix(2);
        for (Int32 i = 0; i < cell.x; ++i) {
            const auto tab = line.find('\t');
            if (tab == line.npos) {
                return {};
            }
            line.remove_prefix(tab + 1);
        }
        return std::string(line.substr(0, line.find('\t')));
    }

    if (cell.y < 0 || static_cast<std::size_t>(cell.y) >= currentServer_.players.size()) {
        return {};
    }
    const auto& player = currentServer_.players[static_cast<std::size_t>(cell.y)];
    switch (cell.x) {
        case 0:
            return player.name;
        case 1:
            return player.tribe;
        case 2:
            return std::to_string(player.score);
        case 3:
            return std::to_string(player.ping);
        default:
            return {};
    }
}

} // namespace opent1::game
