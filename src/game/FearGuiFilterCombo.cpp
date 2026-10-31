#include "opent1/game/shell_controls.hpp"

namespace opent1::game {

FilterVarCtrl::FilterVarCtrl() {
    setSort(false);
}

void FilterVarCtrl::setTable(std::span<const FilterCtrl::FilterVars> table) {
    clearEntries();
    for (std::size_t i = 0; i < table.size(); ++i) {
        addEntry(table[i].label, static_cast<Int32>(i));
    }
}

} // namespace opent1::game
