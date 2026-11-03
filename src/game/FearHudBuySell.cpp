#include <opent1/game/chat_inventory_huds.hpp>
#include <ranges>

namespace opent1::game {

FGHBuySell::FGHBuySell() {
    cellSize = {220, 16};
}

void FGHBuySell::populateShoppingItems(const PlayerPSC* psc, const DataBlockManager* dbm) {
    rows_.clear();
    if (psc == nullptr || dbm == nullptr) {
        setSize({1, 0});
        return;
    }
    const Int32 numItemTypes = dbm->getDataGroupSize(DataBlockManager::ItemDataType);
    for (auto i : std::views::iota(Int32{0}) | std::views::take_while([&](Int32 i) {
                      return i < numItemTypes;
                  })) {
        if (!psc->isInShoppingList(i)) {
            continue;
        }
        const auto* itemData =
            dynamic_cast<const ItemData*>(dbm->lookupDataBlock(i, DataBlockManager::ItemDataType));
        if (itemData == nullptr || !itemData->showInventory) {
            continue;
        }
        InventoryDisplayRow row{};
        row.itemIndex = i;
        row.heading = itemData->typeString;
        row.description = itemData->description;
        row.count = psc->itemCount(i);
        row.price = itemData->price;
        rows_.push_back(std::move(row));
    }
    setSize({1, static_cast<Int32>(rows_.size())});
}

} // namespace opent1::game
