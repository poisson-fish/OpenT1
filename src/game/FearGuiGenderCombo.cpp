#include "opent1/game/shell_controls.hpp"

namespace opent1::game {

GenderListCtrl::GenderListCtrl() {
    setSort(false);
    addEntry("MALE", 0);
    addEntry("FEMALE", 1);
    setSelectedEntry(0);
}

FGGenderComboBox::FGGenderComboBox() {
    comboEntries_.setSort(false);
    addEntry("MALE", 0);
    addEntry("FEMALE", 1);
    setSelectedEntry(0);
}

} // namespace opent1::game
