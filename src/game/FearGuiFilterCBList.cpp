#include "opent1/game/shell_controls.hpp"
#include <ranges>

#include <string>

namespace opent1::game {

FGFilterCBListCtrl::FGFilterCBListCtrl() {
    setSort(false);
    addEntry("None", 0);
    setSelectedEntry(0);
}

void FGFilterCBListCtrl::loadFiltersFromConsole(Console& console) {
    clearEntries();
    addEntry("None", 0);
    for (auto i : std::views::iota(Int32{1}) | std::views::take_while([&](Int32 i) {
                      return i <= 64;
                  })) {
        const std::string varName = "pref::Filter" + std::to_string(i);
        const std::string spec(console.getVariable(varName));
        if (spec.empty()) {
            break;
        }
        const auto colon = spec.find(':');
        const std::string filterName = (colon == std::string::npos) ? spec : spec.substr(0, colon);
        addEntry(filterName, i);
    }
    setSelectedEntry(0);
}

Int32 FGFilterCBListCtrl::addFilterToConsole(Console& console, std::string_view filterName) {
    Int32 idx = 1;
    while (idx <= 64) {
        const std::string varName = "pref::Filter" + std::to_string(idx);
        if (console.getVariable(varName).empty()) {
            break;
        }
        ++idx;
    }
    const std::string varName = "pref::Filter" + std::to_string(idx);
    console.setVariable(varName, std::string(filterName) + ":");
    loadFiltersFromConsole(console);
    setSelectedEntry(idx);
    return idx;
}

bool FGFilterCBListCtrl::removeFilterFromConsole(Console& console, Int32 filterNum) {
    if (filterNum <= 0) {
        return false;
    }
    for (auto i :
         std::views::iota(static_cast<Int32>(filterNum)) | std::views::take_while([&](Int32 i) {
             return i < 64;
         })) {
        const std::string nextVar = "pref::Filter" + std::to_string(i + 1);
        const std::string curVar = "pref::Filter" + std::to_string(i);
        const std::string nextVal(console.getVariable(nextVar));
        console.setVariable(curVar, nextVal);
        if (nextVal.empty()) {
            break;
        }
    }
    loadFiltersFromConsole(console);
    return true;
}

} // namespace opent1::game
