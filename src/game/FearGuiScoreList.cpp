#include <opent1/game/chat_inventory_huds.hpp>

#include <algorithm>
#include <format>

namespace opent1::game {

ScoreListCtrl::ScoreListCtrl() {
    cellSize = {640, 16};
}

std::vector<Int32> ScoreListCtrl::parseHeadingTabStops(std::string_view heading) {
    std::vector<Int32> stops{};
    stops.push_back(0);
    for (std::size_t i = 0; i + 1 < heading.size(); ++i) {
        if (heading[i] == '\t') {
            const Int32 stop = static_cast<Int32>(static_cast<UInt8>(heading[i + 1]));
            stops.push_back(stop);
            ++i;
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
    for (std::size_t i = 0; i < formatStr.size(); ++i) {
        if (formatStr[i] == '%' && i + 1 < formatStr.size()) {
            const char spec = formatStr[++i];
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
            out.push_back(formatStr[i]);
        }
    }
    return out;
}

std::string ScoreListCtrl::expandTeamScoreFormat(std::string_view formatStr,
                                                 const PlayerManager::TeamRep& team) {
    std::string out{};
    out.reserve(formatStr.size() + 32);
    for (std::size_t i = 0; i < formatStr.size(); ++i) {
        if (formatStr[i] == '%' && i + 1 < formatStr.size()) {
            const char spec = formatStr[++i];
            if (spec == 't' || spec == 'n') {
                out.append(team.name);
            } else {
                out.push_back('%');
                out.push_back(spec);
            }
        } else {
            out.push_back(formatStr[i]);
        }
    }
    return out;
}

void ScoreListCtrl::rebuildScores(const PlayerManager& pm) {
    lines_.clear();
    teamTabStops_ = parseHeadingTabStops(pm.teamScoreHeading());
    clientTabStops_ = parseHeadingTabStops(pm.clientScoreHeading());

    if (!pm.teamScoreHeading().empty() && pm.getNumTeams() > 0) {
        lines_.push_back({true, true, -1, 0, std::string(pm.teamScoreHeading())});
        std::vector<const PlayerManager::TeamRep*> sortedTeams{};
        for (Int32 t = 0; t < pm.getNumTeams(); ++t) {
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
    setSize({1, static_cast<Int32>(lines_.size())});
}

std::string ScoreListCtrl::getCellText(Point2I cell) const {
    if (cell.y < 0 || static_cast<std::size_t>(cell.y) >= lines_.size()) {
        return {};
    }
    return lines_[static_cast<std::size_t>(cell.y)].text;
}

bool ScoreListCtrl::drawCell(
    gfx::SoftwareSurface& surface, Point2I position, Point2I cell, bool, bool) const {
    if (cell.y < 0 || std::size_t(cell.y) >= lines_.size()) {
        return true;
    }
    const auto& line = lines_[cell.y];
    const auto& font = line.isHeader || !shellHighlightFont ? shellFont : shellHighlightFont;
    if (!font) {
        return true;
    }
    const auto& stops = line.isTeamSection ? teamTabStops_ : clientTabStops_;
    const auto authoredWidth = stops.size() > 2 ? stops[stops.size() - 2] + 36 : cellSize.x;
    const auto scale = std::min(1.f, float(cellSize.x) / float(std::max(1, authoredWidth)));
    std::string_view remaining(line.text);
    for (std::size_t column = 0; !remaining.empty() && column + 1 < stops.size(); ++column) {
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
