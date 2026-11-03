#include "opent1/game/shell_controls.hpp"
#include <ranges>

namespace opent1::game {

FilterVarCtrl::FilterVarCtrl() {
    setSort(false);
}

void FilterVarCtrl::setTable(std::span<const FilterCtrl::FilterVars> table) {
    clearEntries();
    for (auto i : std::views::iota(std::size_t{0}, table.size())) {
        addEntry(table[i].label, static_cast<Int32>(i));
    }
}

} // namespace opent1::game
