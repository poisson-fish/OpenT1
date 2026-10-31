#include "opent1/game/server_irc_controls.hpp"
#include <opent1/console/console.hpp>

#include <string>

namespace opent1::game {

FGMasterList::FGMasterList() {
    setSize({1, 0});
}

void FGMasterList::addMasterServer(std::string_view name, std::string_view address) {
    masters_.push_back(MasterServerEntry{std::string(name), std::string(address)});
    setSize({1, static_cast<Int32>(masters_.size())});
}

void FGMasterList::loadFromConsole(Console& console) {
    masters_.clear();
    setSize({1, 0});
    selected = {-1, -1};
    for (Int32 i = 0; i < 32; ++i) {
        const std::string varName = "Server::MasterAddressN" + std::to_string(i);
        const std::string spec(console.getVariable(varName));
        if (spec.empty()) {
            continue;
        }
        const auto tabPos = spec.find('\t');
        if (tabPos != std::string::npos) {
            addMasterServer(spec.substr(0, tabPos), spec.substr(tabPos + 1));
        } else {
            addMasterServer(spec, spec);
        }
    }
}

std::string FGMasterList::selectedAddress() const {
    if (selected.y < 0 || static_cast<std::size_t>(selected.y) >= masters_.size()) {
        return {};
    }
    return masters_[static_cast<std::size_t>(selected.y)].address;
}

std::string FGMasterList::getCellText(Point2I cell) const {
    if (cell.y < 0 || static_cast<std::size_t>(cell.y) >= masters_.size()) {
        return {};
    }
    const auto& entry = masters_[static_cast<std::size_t>(cell.y)];
    return entry.name + " (" + entry.address + ")";
}

} // namespace opent1::game
