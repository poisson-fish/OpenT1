#include "opent1/game/shell_controls.hpp"
#include <ranges>

#include <array>
#include <charconv>

namespace opent1::game {

namespace {

inline constexpr Int32 kIdCtgFilterPing = 1;
inline constexpr Int32 kIdCtgFilterPlayers = 2;
inline constexpr Int32 kIdCtgFilterDedicated = 3;
inline constexpr Int32 kIdCtgFilterServer = 4;
inline constexpr Int32 kIdCtgFilterMission = 5;
inline constexpr Int32 kIdCtgFilterMissionType = 6;
inline constexpr Int32 kIdCtgFilterFavorite = 7;
inline constexpr Int32 kIdCtgFilterServerType = 8;

constexpr std::array<FilterCtrl::FilterVars, 8> kFilterVariableTable{{
    {kIdCtgFilterPing, FilterCtrl::FT_COND_NUMERICAL, "Ping"},
    {kIdCtgFilterPlayers, FilterCtrl::FT_COND_NUMERICAL, "Players"},
    {kIdCtgFilterMission, FilterCtrl::FT_COND_TEXT, "Mission"},
    {kIdCtgFilterServer, FilterCtrl::FT_COND_TEXT, "Server"},
    {kIdCtgFilterMissionType, FilterCtrl::FT_COND_TEXT, "Mission Type"},
    {kIdCtgFilterServerType, FilterCtrl::FT_COND_TEXT, "Server Type"},
    {kIdCtgFilterFavorite, FilterCtrl::FT_COND_BOOL, "Favorite"},
    {kIdCtgFilterDedicated, FilterCtrl::FT_COND_BOOL, "Dedicated"},
}};

Int32 parseIntToken(std::string_view token, Int32 fallback = 0) noexcept {
    Int32 value = fallback;
    const auto* begin = token.data();
    const auto* end = token.data() + token.size();
    std::from_chars(begin, end, value);
    return value;
}

} // namespace

FilterListCtrl::FilterListCtrl() {
    setSize({1, 0});
    cellSize = {160, 16};
}

void FilterListCtrl::addFilterName(std::string_view name) {
    filterNames_.emplace_back(name);
    setSize({1, static_cast<Int32>(filterNames_.size())});
}

FilterCtrl::FilterCtrl() {
    columnInfo_.push_back(ColumnInfo{0, 10, 640, 0, 0, true, 120, "VARIABLE"});
    columnInfo_.push_back(ColumnInfo{1, 10, 640, 1, 1, true, 100, "CONDITION"});
    columnInfo_.push_back(ColumnInfo{2, 10, 640, 2, 2, true, 140, "VALUE"});
    setSize({3, 0});
}

std::span<const FilterCtrl::FilterVars, 8> FilterCtrl::variableTable() noexcept {
    return kFilterVariableTable;
}

std::vector<FilterCtrl::FilterInfo> FilterCtrl::parseFilterSpec(std::string_view spec,
                                                                std::string* filterNameOut) {
    std::vector<FilterInfo> result{};
    const auto colonPos = spec.find(':');
    std::string_view body = spec;
    if (colonPos != std::string_view::npos) {
        if (filterNameOut != nullptr) {
            *filterNameOut = std::string(spec.substr(0, colonPos));
        }
        body = spec.substr(colonPos + 1);
    } else if (filterNameOut != nullptr) {
        *filterNameOut = {};
    }

    std::vector<std::string_view> tokens{};
    std::size_t pos = 0;
    while (pos <= body.size()) {
        const auto nextTab = body.find('\t', pos);
        if (nextTab == std::string_view::npos) {
            tokens.push_back(body.substr(pos));
            break;
        }
        tokens.push_back(body.substr(pos, nextTab - pos));
        pos = nextTab + 1;
    }

    for (std::size_t i = 0; i + 2 < tokens.size(); i += 3) {
        if (tokens[i].empty() && tokens[i + 1].empty() && tokens[i + 2].empty()) {
            continue;
        }
        FilterInfo info{};
        info.variable = parseIntToken(tokens[i], 0);
        info.condition = parseIntToken(tokens[i + 1], 0);
        info.value = std::string(tokens[i + 2]);
        result.push_back(std::move(info));
    }
    return result;
}

std::string FilterCtrl::formatFilterSpec(std::string_view filterName,
                                         std::span<const FilterInfo> conditions) {
    std::string out(filterName);
    out.push_back(':');
    for (auto i : std::views::iota(std::size_t{0}) | std::views::take_while([&](std::size_t i) {
                      return i < conditions.size();
                  })) {
        if (i > 0) {
            out.push_back('\t');
        }
        out += std::to_string(conditions[i].variable);
        out.push_back('\t');
        out += std::to_string(conditions[i].condition);
        out.push_back('\t');
        out += conditions[i].value;
    }
    return out;
}

void FilterCtrl::loadFromSpec(std::string_view spec) {
    conditions_ = parseFilterSpec(spec, &filterName_);
    setSize({3, static_cast<Int32>(conditions_.size())});
}

std::string FilterCtrl::saveToSpec() const {
    return formatFilterSpec(filterName_, conditions_);
}

void FilterCtrl::addCondition(FilterInfo cond) {
    conditions_.push_back(std::move(cond));
    setSize({3, static_cast<Int32>(conditions_.size())});
}

bool FilterCtrl::removeCondition(std::size_t index) {
    if (index >= conditions_.size()) {
        return false;
    }
    conditions_.erase(conditions_.begin() + static_cast<std::ptrdiff_t>(index));
    setSize({3, static_cast<Int32>(conditions_.size())});
    return true;
}

std::string FilterCtrl::getCellText(Point2I cell) const {
    if (cell.y < 0 || static_cast<std::size_t>(cell.y) >= conditions_.size()) {
        return {};
    }
    const auto& cond = conditions_[static_cast<std::size_t>(cell.y)];
    if (cell.x == 0) {
        if (cond.variable >= 0 &&
            static_cast<std::size_t>(cond.variable) < kFilterVariableTable.size()) {
            return std::string(kFilterVariableTable[static_cast<std::size_t>(cond.variable)].label);
        }
        return std::to_string(cond.variable);
    }
    if (cell.x == 1) {
        return std::to_string(cond.condition);
    }
    if (cell.x == 2) {
        return cond.value;
    }
    return {};
}

} // namespace opent1::game
