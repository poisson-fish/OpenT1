#include <opent1/game/commander_huds.hpp>
#include <ranges>

namespace opent1::game {

namespace {

void appendPeonSubtree(const PlayerManager::ClientRep* cl,
                       Int32 depth,
                       std::vector<CommandTeamCtrl::TeamMemberRow>& out) {
    for (const auto* cur = cl; cur != nullptr; cur = cur->nextPeon) {
        CommandTeamCtrl::TeamMemberRow row{};
        row.clientId = cur->id;
        row.commanderId = cur->commanderId;
        row.depth = depth;
        row.name = cur->name;
        row.commandStatus = cur->commandStatus;
        row.statusSymbol = std::string(CommandTeamCtrl::statusStringForCode(cur->commandStatus));
        row.selected = cur->selected;
        out.push_back(std::move(row));
        if (cur->firstPeon != nullptr && cur->expanded) {
            appendPeonSubtree(cur->firstPeon, depth + 1, out);
        }
    }
}

} // namespace

bool CommandTeamCtrl::event(const gui::Event& event) {
    if (event.type == gui::EventType::Down && event.button == 1 && active && cellSize.y > 0) {
        const auto point = globalToLocal(event.point);
        const auto row = point.y / cellSize.y;
        if (point.y >= 0 && row >= 0 && std::size_t(row) < rows_.size()) {
            const auto& entry = rows_[std::size_t(row)];
            if (clicked) {
                const bool expand =
                    point.x >= 16 + entry.depth * 8 && point.x < 24 + entry.depth * 8;
                if (event.shift && selected.y >= 0 && !expand) {
                    bool first = true;
                    for (auto i : std::views::iota(std::min(row, selected.y)) |
                                      std::views::take_while([&](auto i) {
                                          return i <= std::max(row, selected.y);
                                      })) {
                        clicked(rows_[std::size_t(i)].clientId, !first, false, false);
                        first = false;
                    }
                } else {
                    clicked(entry.clientId, event.control, event.clicks > 1, expand);
                }
            }
            selected = {0, row};
            return true;
        }
    }
    return Parent::event(event);
}

bool CommandTeamCtrl::drawCell(
    gfx::SoftwareSurface& surface, Point2I offset, Point2I cell, bool, bool hover) const {
    if (cell.y < 0 || std::size_t(cell.y) >= rows_.size()) {
        return true;
    }
    const auto& row = rows_[std::size_t(cell.y)];
    if (shellFont && !surface.text(*shellFont, offset, row.statusSymbol)) {
        return false;
    }
    const auto branch = offset + Point2I{16 + row.depth * 8, cellSize.y / 2};
    surface.line({branch.x, offset.y}, {branch.x, branch.y}, 245);
    surface.line(branch, {branch.x + 6, branch.y}, 245);
    return Parent::drawCell(
        surface, offset + Point2I{24 + row.depth * 8, 0}, cell, row.selected, hover);
}

CommandTeamCtrl::CommandTeamCtrl() {
    cellSize = {140, 16};
}

std::string_view CommandTeamCtrl::statusStringForCode(Int32 status) noexcept {
    switch (status) {
        case PlayerManager::CMD_NONE:
            return "N";
        case PlayerManager::CMD_RECEIVED:
            return "R";
        case PlayerManager::CMD_ACKNOWLEDGED:
            return "A";
        default:
            return "U";
    }
}

void CommandTeamCtrl::buildTeamHierarchy(PlayerManager& pm, Int32 teamId) {
    rows_.clear();
    pm.buildCommandTrees();
    if (const auto* team = pm.findTeam(teamId)) {
        appendPeonSubtree(team->firstCommander, 0, rows_);
    }
    setSize({1, static_cast<Int32>(rows_.size())});
}

std::string CommandTeamCtrl::getCellText(Point2I cell) const {
    if (cell.y < 0 || static_cast<std::size_t>(cell.y) >= rows_.size()) {
        return {};
    }
    return rows_[static_cast<std::size_t>(cell.y)].name;
}

} // namespace opent1::game
