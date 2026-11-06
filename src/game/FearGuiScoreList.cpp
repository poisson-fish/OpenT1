#include <opent1/game/chat_inventory_huds.hpp>
#include <opent1/gui/containers.hpp>
#include <iterator>
#include <ranges>

#include <algorithm>
#include <format>

namespace opent1::game {

ScoreListCtrl::ScoreListCtrl() {
    cells = {1, 0};
    cellSize = {640, 16};
}

void ScoreListCtrl::wake() {
    active = true;
    // Vanilla inherits ArrayCtrl directly: its 640px rows scroll horizontally
    // instead of using FGArrayCtrl's viewport-fitting shell layout.
    cellSize.x = 640;
    gui::Array::wake();
}

bool ScoreListCtrl::select(Point2I cell) {
    if (cell == Point2I{-1, -1}) {
        selected = cell;
        return true;
    }
    if (cell.x != 0 || cell.y < 0 || std::size_t(cell.y) >= lines_.size()) {
        return false;
    }
    const auto& line = lines_[cell.y];
    if (line.isHeader || line.isTeamSection) {
        return true;
    }
    // Native cellSelected keeps the current scroll position, including the
    // horizontal offset. Array::select would scroll the entire 640px cell.
    selected = cell;
    if (clientSelected) {
        const auto callback = clientSelected;
        const auto id = line.id;
        if (canvas() && canvas()->dispatching()) {
            canvas()->afterDispatch([callback, id] {
                callback(id);
            });
        } else {
            callback(id);
        }
    }
    return true;
}

std::vector<Int32> ScoreListCtrl::parseHeadingTabStops(std::string_view heading) {
    std::vector<Int32> stops{};
    stops.push_back(0);
    for (auto characterIt = heading.begin();
         characterIt != heading.end() && std::next(characterIt) != heading.end();
         ++characterIt) {
        if (*characterIt == '\t') {
            const Int32 stop = static_cast<Int32>(static_cast<UInt8>(*std::next(characterIt)));
            stops.push_back(stop);
            ++characterIt;
            if (stops.size() >= MaxTabStops - 1) {
                break;
            }
        }
    }
    stops.push_back(640);
    return stops;
}

std::string ScoreListCtrl::expandClientScoreFormat(std::string_view formatStr,
                                                   const PlayerManager::ClientRep& client,
                                                   std::string_view teamName) {
    std::string out{};
    out.reserve(formatStr.size() + 32);
    for (auto characterIt = formatStr.begin(); characterIt != formatStr.end(); ++characterIt) {
        if (*characterIt == '%' && std::next(characterIt) != formatStr.end()) {
            const char spec = *++characterIt;
            switch (spec) {
                case 'n':
                    out.append(client.name);
                    break;
                case 't':
                    out.append(teamName);
                    break;
                case 'p':
                    out.append(std::format("{}", client.ping));
                    break;
                case 'l':
                    out.append(std::format("{}", client.packetLoss));
                    break;
                default:
                    out.push_back('%');
                    out.push_back(spec);
                    break;
            }
        } else {
            out.push_back(*characterIt);
        }
    }
    return out;
}

std::string ScoreListCtrl::expandTeamScoreFormat(std::string_view formatStr,
                                                 const PlayerManager::TeamRep& team) {
    std::string out{};
    out.reserve(formatStr.size() + 32);
    for (auto characterIt = formatStr.begin(); characterIt != formatStr.end(); ++characterIt) {
        if (*characterIt == '%' && std::next(characterIt) != formatStr.end()) {
            const char spec = *++characterIt;
            if (spec == 't' || spec == 'n') {
                out.append(team.name);
            } else {
                out.push_back('%');
                out.push_back(spec);
            }
        } else {
            out.push_back(*characterIt);
        }
    }
    return out;
}

void ScoreListCtrl::rebuildScores(const PlayerManager& pm) {
    const auto selectedId = selected.y >= 0 && std::size_t(selected.y) < lines_.size() &&
                                    !lines_[selected.y].isHeader &&
                                    !lines_[selected.y].isTeamSection
                                ? lines_[selected.y].id
                                : 0;
    lines_.clear();
    teamTabStops_ = parseHeadingTabStops(pm.teamScoreHeading());
    clientTabStops_ = parseHeadingTabStops(pm.clientScoreHeading());

    if (!pm.teamScoreHeading().empty() && pm.getNumTeams() > 0) {
        lines_.push_back({true, true, -1, 0, std::string(pm.teamScoreHeading())});
        std::vector<const PlayerManager::TeamRep*> sortedTeams{};
        for (auto t : std::views::iota(Int32{0}) | std::views::take_while([&](Int32 t) {
                          return t < pm.getNumTeams();
                      })) {
            if (const auto* team = pm.findTeam(t)) {
                sortedTeams.push_back(team);
            }
        }
        std::stable_sort(sortedTeams.begin(), sortedTeams.end(), [](const auto* a, const auto* b) {
            return a->scoreSortValue > b->scoreSortValue;
        });
        for (const auto* team : sortedTeams) {
            lines_.push_back({true,
                              false,
                              team->id,
                              team->scoreSortValue,
                              expandTeamScoreFormat(team->scoreString, *team)});
        }
    }

    if (!pm.clientScoreHeading().empty()) {
        lines_.push_back({false, true, -1, 0, std::string(pm.clientScoreHeading())});
        std::vector<const PlayerManager::ClientRep*> sortedClients{};
        for (const auto* cl = pm.getClientList(); cl != nullptr;
             cl = static_cast<const PlayerManager::ClientRep*>(cl->nextClient)) {
            sortedClients.push_back(cl);
        }
        std::stable_sort(
            sortedClients.begin(), sortedClients.end(), [](const auto* a, const auto* b) {
                return a->scoreSortValue > b->scoreSortValue;
            });
        for (const auto* cl : sortedClients) {
            std::string_view tName{};
            if (const auto* team = pm.findTeam(cl->team)) {
                tName = team->name;
            }
            lines_.push_back(
                {false,
                 false,
                 cl->id,
                 cl->scoreSortValue,
                 expandClientScoreFormat(
                     cl->scoreString.empty() ? "%n\t%t\t0\t%p\t%l" : cl->scoreString, *cl, tName)});
        }
    }
    selected = {-1, -1};
    for (std::size_t row = 0; row < lines_.size(); ++row) {
        const auto& line = lines_[row];
        if (line.id == selectedId && !line.isHeader && !line.isTeamSection) {
            selected = {0, Int32(row)};
            break;
        }
    }
    const Point2I size{1, static_cast<Int32>(lines_.size())};
    if (cells != size && setSize(size)) {
        // Match native ScrollContent::childResized: publish scrollbar geometry
        // as soon as the roster changes, even before another simulation tick.
        if (auto* scroll = parent() ? dynamic_cast<gui::Scroll*>(parent()->parent()) : nullptr) {
            (void)scroll->computeSizes();
        }
    }
}

std::string ScoreListCtrl::getCellText(Point2I cell) const {
    if (cell.y < 0 || static_cast<std::size_t>(cell.y) >= lines_.size()) {
        return {};
    }
    return lines_[static_cast<std::size_t>(cell.y)].text;
}

bool ScoreListCtrl::drawCell(
    gfx::SoftwareSurface& surface, Point2I position, Point2I cell, bool chosen, bool) const {
    if (cell.y < 0 || std::size_t(cell.y) >= lines_.size()) {
        return true;
    }
    const auto& line = lines_[cell.y];
    if (chosen && !line.isHeader && !line.isTeamSection) {
        surface.fill(
            {position.x, position.y, position.x + cellSize.x - 1, position.y + cellSize.y - 1},
            GREEN_78);
    }
    const auto& font = line.isHeader || !shellHighlightFont ? shellFont : shellHighlightFont;
    if (!font) {
        return true;
    }
    const auto& stops = line.isTeamSection ? teamTabStops_ : clientTabStops_;
    const auto authoredWidth = stops.size() > 2 ? stops[stops.size() - 2] + 36 : cellSize.x;
    const auto scale = std::min(1.f, float(cellSize.x) / float(std::max(1, authoredWidth)));
    std::string_view remaining(line.text);
    for (auto column :
         std::views::iota(std::size_t{0}) | std::views::take_while([&](std::size_t column) {
             return !remaining.empty() && column + 1 < stops.size();
         })) {
        const auto tab = remaining.find('\t');
        const auto left = Int32(float(stops[column]) * scale);
        const auto right =
            column + 2 == stops.size() ? cellSize.x : Int32(float(stops[column + 1]) * scale);
        const auto text = truncateInfoText(remaining.substr(0, tab), right - left - 4, font.get());
        if (!surface.text(*font, position + Point2I{2 + left, 0}, text)) {
            return false;
        }
        if (tab == remaining.npos) {
            break;
        }
        remaining.remove_prefix(tab + 1);
        if (line.isHeader && !remaining.empty()) {
            remaining.remove_prefix(1);
        }
    }
    return true;
}

} // namespace opent1::game
