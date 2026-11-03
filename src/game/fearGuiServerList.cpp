#include "opent1/game/server_irc_controls.hpp"
#include <ranges>

#include <algorithm>
#include <cctype>
#include <charconv>

namespace opent1::game {

namespace {

std::string toLowerCopy(std::string_view sv) {
    std::string out(sv);
    for (auto& ch : out) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return out;
}

bool containsIgnoreCase(std::string_view haystack, std::string_view needle) {
    if (needle.empty()) {
        return true;
    }
    const auto h = toLowerCopy(haystack);
    const auto n = toLowerCopy(needle);
    return h.find(n) != std::string::npos;
}

Int32 parseIntValue(std::string_view sv, Int32 fallback = 0) noexcept {
    Int32 v = fallback;
    std::from_chars(sv.data(), sv.data() + sv.size(), v);
    return v;
}

bool evaluateNumerical(Int32 actual, Int32 condCode, Int32 target) noexcept {
    // These codes are persisted in original client preferences.
    switch (condCode) {
        case 0:
            return actual < target;
        case 1:
            return actual > target;
        case 2:
            return actual == target;
        case 3:
            return actual != target;
        default:
            return true;
    }
}

bool evaluateText(std::string_view actual, Int32 condCode, std::string_view target) {
    // Unknown conditions are ignored by the native filter.
    switch (condCode) {
        case 0:
            return containsIgnoreCase(actual, target);
        case 1:
            return !containsIgnoreCase(actual, target);
        default:
            return true;
    }
}

bool evaluateBool(bool actual, Int32 condCode, std::string_view) {
    return actual == (condCode == 0);
}

} // namespace

ServerListCtrl::ServerListCtrl() {
    columnInfo_ = {
        ColumnInfo{0, 10, 640, Favorites, Favorites, true, 24, "FAV"},
        ColumnInfo{1, 10, 640, Connection, Connection, true, 24, "ST"},
        ColumnInfo{2, 10, 640, Name, Name, true, 180, "SERVER NAME"},
        ColumnInfo{3, 10, 640, Ping, Ping, true, 50, "PING"},
        ColumnInfo{4, 10, 640, MissionName, MissionName, true, 120, "MISSION"},
        ColumnInfo{5, 10, 640, MissionType, MissionType, true, 100, "TYPE"},
        ColumnInfo{6, 10, 640, Players, Players, true, 64, "PLAYERS"},
        ColumnInfo{7, 10, 640, ServerType, ServerType, true, 80, "MOD"},
        ColumnInfo{8, 10, 640, Version, Version, true, 50, "VER"},
        ColumnInfo{9, 10, 640, Address, Address, true, 120, "ADDRESS"},
        ColumnInfo{10, 10, 640, CPU, CPU, true, 50, "CPU"},
    };
    setSize({1, 0});
    cellSize = {862, 16};
    header = {0, 16};
    setColumnsDualSortable(true);
    columnToSort_ = Ping;
    sortAscending_ = true;
}

void ServerListCtrl::addOrUpdateServer(ServerInfo info) {
    if (info.favorite) {
        info.statusFlags |= SERVER_FAVORITE;
    }
    if (info.password) {
        info.statusFlags |= SERVER_PASSWORD;
    }
    if (info.dedicated) {
        info.statusFlags |= SERVER_DEDICATED;
    }
    if (info.hasBuddy) {
        info.statusFlags |= SERVER_BUDDY;
    }

    for (auto& existing : servers_) {
        if (!info.transportAddress.empty() && existing.transportAddress == info.transportAddress) {
            existing = std::move(info);
            rebuildVisibleList();
            return;
        }
    }
    servers_.push_back(std::move(info));
    rebuildVisibleList();
}

void ServerListCtrl::clearServers() {
    servers_.clear();
    visibleIndices_.clear();
    setSize({1, 0});
    selected = {-1, -1};
}

void ServerListCtrl::applyFilterSpec(std::string_view spec, std::string_view buddyList) {
    activeConditions_ = FilterCtrl::parseFilterSpec(spec);
    activeBuddyList_ = std::string(buddyList);
    rebuildVisibleList();
}

bool ServerListCtrl::matchesFilter(const ServerInfo& server,
                                   std::span<const FilterCtrl::FilterInfo> conditions,
                                   std::string_view /*buddyList*/) {
    for (const auto& cond : conditions) {
        switch (cond.variable) {
            case 0: // Ping
                if (!evaluateNumerical(
                        server.pingTime, cond.condition, parseIntValue(cond.value, 9999))) {
                    return false;
                }
                break;
            case 1: // Players
                if (!evaluateNumerical(
                        server.numPlayers, cond.condition, parseIntValue(cond.value, 0))) {
                    return false;
                }
                break;
            case 2: // Mission
                if (!evaluateText(server.missionName, cond.condition, cond.value)) {
                    return false;
                }
                break;
            case 3: // Server Name
                if (!evaluateText(server.name, cond.condition, cond.value)) {
                    return false;
                }
                break;
            case 4: // Mission Type
                if (!evaluateText(server.missionType, cond.condition, cond.value)) {
                    return false;
                }
                break;
            case 5: // Server/mod Type
                if (!evaluateText(server.modType, cond.condition, cond.value)) {
                    return false;
                }
                break;
            case 6: // Favorite
                if (!evaluateBool(server.favorite, cond.condition, cond.value)) {
                    return false;
                }
                break;
            case 7: { // Dedicated
                if (!evaluateBool(server.dedicated, cond.condition, cond.value)) {
                    return false;
                }
                break;
            }
            default:
                break;
        }
    }
    return true;
}

void ServerListCtrl::sortServers(Int32 columnIndex, bool ascending) {
    columnToSort_ = columnIndex;
    sortAscending_ = ascending;
    rebuildVisibleList();
}

void ServerListCtrl::tick(double time) {
    Parent::tick(time);
    if (discoverServers) {
        const auto selectedAddress =
            selectedServer() ? selectedServer()->transportAddress : std::string{};
        servers_ = discoverServers();
        rebuildVisibleList();
        selected = {-1, -1};
        for (auto i : std::views::iota(std::size_t{0}, visibleIndices_.size())) {
            if (servers_[visibleIndices_[i]].transportAddress == selectedAddress) {
                selected = {0, static_cast<Int32>(i)};
                break;
            }
        }
    }
    if (needsRefresh()) {
        rebuildVisibleList();
    }
}

bool ServerListCtrl::select(Point2I cell) {
    if (!Parent::select(cell)) {
        return false;
    }
    if (const auto* server = selectedServer(); server && serverSelected) {
        serverSelected(*server, false);
    }
    return true;
}

const ServerListCtrl::ServerInfo* ServerListCtrl::selectedServer() const noexcept {
    if (selected.y < 0 || static_cast<std::size_t>(selected.y) >= visibleIndices_.size()) {
        return nullptr;
    }
    const auto idx = visibleIndices_[static_cast<std::size_t>(selected.y)];
    return idx < servers_.size() ? &servers_[idx] : nullptr;
}

std::string ServerListCtrl::getCellText(Point2I cell) const {
    if (cell.y < 0 || static_cast<std::size_t>(cell.y) >= visibleIndices_.size()) {
        return {};
    }
    const auto& s = servers_[visibleIndices_[static_cast<std::size_t>(cell.y)]];
    switch (cell.x) {
        case Favorites:
            return s.favorite ? "*" : "";
        case Connection:
            return std::to_string(s.status);
        case Name:
            return s.name;
        case Ping:
            return std::to_string(s.pingTime);
        case MissionName:
            return s.missionName;
        case MissionType:
            return s.missionType;
        case Players:
            return std::to_string(s.numPlayers) + "/" + std::to_string(s.maxPlayers);
        case ServerType:
            return s.modType;
        case Version:
            return s.version;
        case Address:
            return s.transportAddress;
        case CPU:
            return std::to_string(s.cpuSpeed);
        default:
            return {};
    }
}

void ServerListCtrl::rebuildVisibleList() {
    const auto address = selectedServer() ? selectedServer()->transportAddress : std::string{};
    visibleIndices_.clear();
    for (auto i : std::views::iota(std::size_t{0}) | std::views::take_while([&](std::size_t i) {
                      return i < servers_.size();
                  })) {
        if (matchesFilter(servers_[i], activeConditions_, activeBuddyList_)) {
            visibleIndices_.push_back(i);
        }
    }

    std::stable_sort(
        visibleIndices_.begin(), visibleIndices_.end(), [this](std::size_t ia, std::size_t ib) {
            const auto& a = servers_[ia];
            const auto& b = servers_[ib];
            const auto compareColumn = [&](Int32 column) {
                int cmp = 0;
                if (a.neverPing != b.neverPing) {
                    return a.neverPing ? 1 : -1;
                }
                switch (column) {
                    case Favorites:
                        cmp = (a.favorite == b.favorite) ? 0 : (a.favorite ? -1 : 1);
                        if (!cmp && a.hasBuddy != b.hasBuddy) {
                            cmp = a.hasBuddy ? -1 : 1;
                        }
                        break;
                    case Connection:
                        cmp = a.status < b.status ? -1 : a.status > b.status ? 1 : 0;
                        if (!cmp && a.dedicated != b.dedicated) {
                            cmp = a.dedicated ? -1 : 1;
                        }
                        if (!cmp && a.password != b.password) {
                            cmp = a.password ? 1 : -1;
                        }
                        break;
                    case Name:
                        cmp = toLowerCopy(a.name).compare(toLowerCopy(b.name));
                        break;
                    case Ping:
                        cmp = (a.pingTime < b.pingTime) ? -1 : (a.pingTime > b.pingTime ? 1 : 0);
                        break;
                    case MissionName:
                        cmp = toLowerCopy(a.missionName).compare(toLowerCopy(b.missionName));
                        break;
                    case MissionType:
                        cmp = toLowerCopy(a.missionType).compare(toLowerCopy(b.missionType));
                        break;
                    case Players:
                        cmp = (a.numPlayers < b.numPlayers) ? -1
                                                            : (a.numPlayers > b.numPlayers ? 1 : 0);
                        break;
                    case CPU:
                        cmp = (a.cpuSpeed < b.cpuSpeed) ? -1 : (a.cpuSpeed > b.cpuSpeed ? 1 : 0);
                        break;
                    case ServerType:
                        cmp = toLowerCopy(a.modType).compare(toLowerCopy(b.modType));
                        break;
                    case Version:
                        cmp = toLowerCopy(a.version).compare(toLowerCopy(b.version));
                        break;
                    case Address:
                        cmp = toLowerCopy(a.transportAddress)
                                  .compare(toLowerCopy(b.transportAddress));
                        break;
                    default:
                        cmp = (a.pingTime < b.pingTime) ? -1 : (a.pingTime > b.pingTime ? 1 : 0);
                        break;
                }
                return cmp;
            };
            int cmp = compareColumn(columnToSort_);
            if (!cmp && columnToSortSecond_ >= 0 && columnToSortSecond_ != columnToSort_) {
                cmp = compareColumn(columnToSortSecond_);
                if (!cmp) {
                    cmp = toLowerCopy(a.name).compare(toLowerCopy(b.name));
                }
                if (!sortAscendingSecond_) {
                    cmp = -cmp;
                }
            } else if (!cmp) {
                cmp = toLowerCopy(a.name).compare(toLowerCopy(b.name));
            }
            return sortAscending_ ? (cmp < 0) : (cmp > 0);
        });

    setSize({1, static_cast<Int32>(visibleIndices_.size())});
    refresh_ = false;
    for (auto i : std::views::iota(std::size_t{0}, visibleIndices_.size())) {
        if (!address.empty() && servers_[visibleIndices_[i]].transportAddress == address) {
            selected = {0, Int32(i)};
            return;
        }
    }
    if (visibleIndices_.empty()) {
        selected = {-1, -1};
    } else if (selected.y < 0 || static_cast<std::size_t>(selected.y) >= visibleIndices_.size()) {
        selected = {0, 0};
    }
}

void ServerListCtrl::wake() {
    Parent::wake();
    if (refreshServers) {
        refreshServers();
    }
}

void ServerListCtrl::close() {
    if (saveServers) {
        saveServers();
    }
    Parent::close();
}

bool ServerListCtrl::setServerPreference(std::string_view address, bool favorite, bool neverPing) {
    const auto found = std::ranges::find(servers_, address, &ServerInfo::transportAddress);
    if (found == servers_.end()) {
        return false;
    }
    found->favorite = favorite;
    found->neverPing = neverPing;
    const auto snapshot = *found;
    if (preferenceChanged) {
        preferenceChanged(snapshot);
    }
    rebuildVisibleList();
    return true;
}

bool ServerListCtrl::event(const gui::Event& e) {
    const auto local = globalToLocal(e.point);
    if (e.type == gui::EventType::Down && local.y < header.y && clickHeader(local, e.button == 2)) {
        rebuildVisibleList();
        return true;
    }
    const bool handled = Parent::event(e);
    if (handled && e.type == gui::EventType::Down && e.button == 1) {
        if (const auto* server = selectedServer()) {
            Int32 x{};
            bool favoriteColumn{};
            for (auto position : std::views::iota(std::size_t{0}, columnInfo_.size())) {
                for (const auto& column : columnInfo_) {
                    if (column.position != static_cast<Int32>(position) || !column.active) {
                        continue;
                    }
                    if (column.origPosition == Favorites && local.x >= x &&
                        local.x < x + column.width) {
                        favoriteColumn = true;
                    }
                    x += column.width;
                }
            }
            const auto snapshot = *server;
            if (favoriteColumn) {
                (void)setServerPreference(snapshot.transportAddress,
                                          !snapshot.favorite && !snapshot.neverPing,
                                          snapshot.favorite);
            } else if (e.clicks > 1 && serverSelected) {
                serverSelected(snapshot, true);
            }
        }
    }
    return handled;
}

bool ServerListCtrl::draw(gfx::SoftwareSurface& surface, Point2I offset, gui::RectI clip) const {
    if (!Parent::draw(surface, offset, clip)) {
        return false;
    }
    Int32 x = offset.x;
    for (auto position : std::views::iota(std::size_t{0}, columnInfo_.size())) {
        for (const auto& column : columnInfo_) {
            if (column.position != static_cast<Int32>(position) || !column.active) {
                continue;
            }
            surface.rectangle({x, offset.y, x + column.width - 1, offset.y + header.y - 1}, 100);
            if (shellFont &&
                !surface.text(*shellFont,
                              {x + 4, offset.y + 2},
                              truncateInfoText(column.title, column.width - 8, shellFont.get()))) {
                return false;
            }
            x += column.width;
        }
    }
    return true;
}

bool ServerListCtrl::drawCell(
    gfx::SoftwareSurface& surface, Point2I offset, Point2I cell, bool chosen, bool hovered) const {
    if (chosen) {
        surface.fill({offset.x, offset.y, offset.x + cellSize.x - 1, offset.y + cellSize.y - 1},
                     100);
    }
    const auto font = (chosen || hovered) && shellHighlightFont ? shellHighlightFont : shellFont;
    if (!font) {
        return true;
    }
    Int32 x = offset.x;
    for (auto position : std::views::iota(std::size_t{0}, columnInfo_.size())) {
        for (const auto& column : columnInfo_) {
            if (column.position != static_cast<Int32>(position) || !column.active) {
                continue;
            }
            const auto text = truncateInfoText(
                getCellText({column.origPosition, cell.y}), column.width - 8, font.get());
            if (!surface.text(*font, {x + 4, offset.y + 2}, text)) {
                return false;
            }
            x += column.width;
        }
    }
    return true;
}

} // namespace opent1::game
