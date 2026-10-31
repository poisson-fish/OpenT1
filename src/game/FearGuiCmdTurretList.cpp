#include <opent1/game/commander_huds.hpp>

#include <opent1/game/data_block_manager.hpp>

namespace opent1::game {

CommandTurretCtrl::CommandTurretCtrl() {
    cellSize = {140, 16};
}

void CommandTurretCtrl::populateFromCommander(const TSCommander& cmdr) {
    entries_.clear();
    const auto targs = cmdr.targets();
    for (std::size_t i = 0; i < targs.size(); ++i) {
        const auto& t = targs[i];
        if (t.datId < 0 || t.team != cmdr.localTeam ||
            t.datGroup != DataBlockManager::TurretDataType) {
            continue;
        }
        entries_.push_back({static_cast<Int32>(i), t.name, t.damageStatus, t.activeStatus});
    }
    setSize({1, static_cast<Int32>(entries_.size())});
}

bool CommandTurretCtrl::event(const gui::Event& event) {
    if (event.type == gui::EventType::Down && event.button == 1 && active && cellSize.y > 0) {
        const auto point = globalToLocal(event.point);
        const auto row = point.y / cellSize.y;
        if (point.y >= 0 && row >= 0 && std::size_t(row) < entries_.size()) {
            selected = {0, row};
            if (clicked) {
                clicked(entries_[std::size_t(row)].targIndex, event.control, event.clicks > 1);
            }
            return true;
        }
    }
    return Parent::event(event);
}

bool CommandTurretCtrl::drawCell(
    gfx::SoftwareSurface& surface, Point2I offset, Point2I cell, bool chosen, bool hover) const {
    if (cell.y < 0 || std::size_t(cell.y) >= entries_.size()) {
        return true;
    }
    const auto& entry = entries_[std::size_t(cell.y)];
    surface.fill({offset.x + 2, offset.y + 3, offset.x + 5, offset.y + cellSize.y - 4},
                 entry.damageStatus >= 2   ? 249
                 : entry.damageStatus == 1 ? 244
                                           : 245);
    surface.fill({offset.x + 8, offset.y + 3, offset.x + 11, offset.y + cellSize.y - 4},
                 entry.activeStatus ? 245 : 249);
    return Parent::drawCell(surface, offset + Point2I{15, 0}, cell, chosen, hover);
}

std::string CommandTurretCtrl::getCellText(Point2I cell) const {
    if (cell.y < 0 || static_cast<std::size_t>(cell.y) >= entries_.size()) {
        return {};
    }
    return entries_[static_cast<std::size_t>(cell.y)].name;
}

} // namespace opent1::game
