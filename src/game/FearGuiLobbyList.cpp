#include "opent1/game/server_irc_controls.hpp"

namespace opent1::game {

LobbyListCtrl::LobbyListCtrl() {
    columnInfo_ = {
        ColumnInfo{0, 10, 640, 0, 0, true, 140, "ROOM"},
        ColumnInfo{1, 10, 640, 1, 1, true, 60, "USERS"},
        ColumnInfo{2, 10, 640, 2, 2, true, 220, "TOPIC"},
    };
    setSize({3, 0});
}

void LobbyListCtrl::addRoom(std::string_view name, std::string_view topic, Int32 userCount) {
    rooms_.push_back(RoomEntry{std::string(name), std::string(topic), userCount});
    setSize({3, static_cast<Int32>(rooms_.size())});
}

void LobbyListCtrl::clearRooms() {
    rooms_.clear();
    setSize({3, 0});
}

std::string LobbyListCtrl::getCellText(Point2I cell) const {
    if (cell.y < 0 || static_cast<std::size_t>(cell.y) >= rooms_.size()) {
        return {};
    }
    const auto& room = rooms_[static_cast<std::size_t>(cell.y)];
    switch (cell.x) {
        case 0:
            return room.name;
        case 1:
            return std::to_string(room.userCount);
        case 2:
            return room.topic;
        default:
            return {};
    }
}

} // namespace opent1::game
