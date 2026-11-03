#include <opent1/game/chat_inventory_huds.hpp>
#include <ranges>
#include <opent1/core/text.hpp>

#include <algorithm>
#include <format>

namespace opent1::game {

namespace {

void insertCategorizedRow(std::vector<InventoryDisplayRow>& rows, InventoryDisplayRow item) {
    const char catKey = item.heading.empty() ? '\0' : item.heading[0];
    std::size_t insertPos = rows.size();
    bool foundCategory = false;
    for (auto i : std::views::iota(std::size_t{0}, rows.size())) {
        const char existingKey = rows[i].heading.empty() ? '\0' : rows[i].heading[0];
        if (existingKey == catKey) {
            foundCategory = true;
            insertPos = i + 1;
            while (insertPos < rows.size()) {
                const char nextKey =
                    rows[insertPos].heading.empty() ? '\0' : rows[insertPos].heading[0];
                if (nextKey != catKey) {
                    break;
                }
                ++insertPos;
            }
            break;
        }
        if (existingKey > catKey) {
            insertPos = i;
            break;
        }
    }
    if (!foundCategory) {
        InventoryDisplayRow header{};
        header.itemIndex = -1;
        header.heading = item.heading;
        header.description = item.heading.empty() ? std::string{} : item.heading.substr(1);
        header.enabled = false;
        rows.insert(rows.begin() + static_cast<std::ptrdiff_t>(insertPos), std::move(header));
        ++insertPos;
    }
    rows.insert(rows.begin() + static_cast<std::ptrdiff_t>(insertPos), std::move(item));
}

} // namespace

FearGuiInventory::FearGuiInventory() {
    cellSize = {300, 16};
}

bool FearGuiInventory::select(Point2I cell) {
    if (cell != Point2I{-1, -1} &&
        (cell.x != 0 || cell.y < 0 || std::size_t(cell.y) >= rows_.size() ||
         !rows_[cell.y].enabled)) {
        selected = {-1, -1};
        return false;
    }
    if (!Parent::select(cell)) {
        return false;
    }
    if (selectedItemIndex() >= 0 && itemSelected) {
        itemSelected(selectedItemIndex(), false);
    }
    return true;
}

bool FearGuiInventory::event(const gui::Event& e) {
    if (active && e.type == gui::EventType::Down && e.button == 1) {
        const auto point = globalToLocal(e.point) - header;
        if (point.x >= 0 && point.x < cellSize.x && point.y >= 0 && cellSize.y > 0) {
            const auto row = std::size_t(point.y / cellSize.y);
            if (row < rows_.size() && !rows_[row].enabled) {
                // Consume the click so Canvas commits the cleared selection.
                selected = {-1, -1};
                return true;
            }
        }
    }
    const auto previous = selectedItemIndex();
    const auto handled = Parent::event(e);
    if (handled && e.type == gui::EventType::Down && e.button == 1 && e.clicks > 1 &&
        previous >= 0 && previous == selectedItemIndex() && itemSelected) {
        itemSelected(previous, true);
    }
    return handled;
}

void FearGuiInventory::populateFromPSC(const PlayerPSC* psc,
                                       const DataBlockManager* dbm,
                                       std::span<const Int32> favoriteItemIds) {
    const auto selectedItem = selectedItemIndex();
    rows_.clear();
    if (psc == nullptr || dbm == nullptr) {
        setSize({1, 0});
        selected = {-1, -1};
        return;
    }
    const Int32 numItemTypes = dbm->getDataGroupSize(DataBlockManager::ItemDataType);
    for (auto i : std::views::iota(Int32{0}) | std::views::take_while([&](Int32 i) {
                      return i < numItemTypes;
                  })) {
        const Int32 count = psc->itemCount(i);
        if (count <= 0) {
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
        row.count = count;
        row.price = itemData->price;
        row.favorite = std::ranges::find(favoriteItemIds, i) != favoriteItemIds.end();
        insertCategorizedRow(rows_, std::move(row));
    }
    restoreSelection(selectedItem);
}

void FearGuiInventory::restoreSelection(Int32 item) {
    setSize({1, static_cast<Int32>(rows_.size())});
    selected = {-1, -1};
    for (auto row : std::views::iota(std::size_t{0}) | std::views::take_while([&](std::size_t row) {
                        return item >= 0 && row < rows_.size();
                    })) {
        if (rows_[row].itemIndex == item && rows_[row].enabled) {
            selected = {0, Int32(row)};
            break;
        }
    }
}

Int32 FearGuiInventory::selectedItemIndex() const noexcept {
    if (selected.y < 0 || static_cast<std::size_t>(selected.y) >= rows_.size()) {
        return -1;
    }
    return rows_[static_cast<std::size_t>(selected.y)].itemIndex;
}

std::string FearGuiInventory::getCellText(Point2I cell) const {
    if (cell.y < 0 || static_cast<std::size_t>(cell.y) >= rows_.size()) {
        return {};
    }
    const auto& r = rows_[static_cast<std::size_t>(cell.y)];
    if (r.itemIndex < 0) {
        return r.description;
    }
    return std::format("{}\t{}", r.description, r.count);
}

bool FearGuiInventory::drawCell(gfx::SoftwareSurface& surface,
                                Point2I position,
                                Point2I cell,
                                bool chosen,
                                bool hovered) const {
    if (cell.y < 0 || std::size_t(cell.y) >= rows_.size()) {
        return true;
    }
    const auto& row = rows_[cell.y];
    const RectI bounds{
        position.x, position.y, position.x + cellSize.x - 1, position.y + cellSize.y - 1};
    if (row.itemIndex < 0) {
        // The leading heading character is a script-defined sort/group key.
        surface.rectangle(
            {position.x - 1, position.y, position.x + cellSize.x - 1, position.y + cellSize.y - 1},
            HILITE_COLOR);
        const auto& font = shellHighlightFont ? shellHighlightFont : shellFont;
        return !font || surface.text(*font,
                                     position + Point2I{2, 0},
                                     truncateInfoText(row.description, cellSize.x - 4, font.get()));
    }
    surface.fill(bounds, 0);
    const auto& font = !row.enabled && disabledFont                ? disabledFont
                       : (chosen || hovered) && shellHighlightFont ? shellHighlightFont
                                                                   : shellFont;
    if (font) {
        const auto value = getCellText(cell);
        const auto tab = value.find('\t');
        const auto label = truncateInfoText(
            std::string_view(value).substr(0, tab), std::min(139, cellSize.x - 15), font.get());
        if (!surface.text(*font, position + Point2I{15, 0}, label)) {
            return false;
        }
        if (tab != value.npos && !surface.text(*font,
                                               position + Point2I{158, 0},
                                               std::string_view(value).substr(tab + 1))) {
            return false;
        }
    }
    if (row.favorite && favoriteBitmap) {
        return surface.blit(*favoriteBitmap,
                            position + Point2I{3, (cellSize.y - favoriteBitmap->height) / 2});
    }
    return true;
}

std::string FearGuiInventory::getVisibleSetString() const {
    std::string result;
    for (const auto& row : rows_) {
        if (row.itemIndex >= 0) {
            result += std::to_string(row.itemIndex) + ' ';
        }
    }
    return result;
}

FearGuiPurchase::FearGuiPurchase() = default;

void FearGuiPurchase::populateFromShoppingList(const PlayerPSC* psc,
                                               const DataBlockManager* dbm,
                                               std::span<const Int32> favoriteItemIds) {
    const auto selectedItem = selectedItemIndex();
    rows_.clear();
    if (psc == nullptr || dbm == nullptr) {
        setSize({1, 0});
        selected = {-1, -1};
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
        row.favorite = std::ranges::find(favoriteItemIds, i) != favoriteItemIds.end();
        row.enabled = psc->isInBuyList(i);
        insertCategorizedRow(rows_, std::move(row));
    }
    restoreSelection(selectedItem);
}

std::string FearGuiPurchase::getVisibleSetString(const PlayerPSC* psc,
                                                 const DataBlockManager* dbm) {
    FearGuiPurchase list;
    list.populateFromShoppingList(psc, dbm);
    return list.getVisibleSetString();
}

std::vector<Int32> FearGuiPurchase::parseFavoritesList(std::span<const std::string> args,
                                                       const DataBlockManager* dbm) {
    std::vector<Int32> favs{};
    if (dbm == nullptr) {
        return favs;
    }
    const Int32 numItemTypes = dbm->getDataGroupSize(DataBlockManager::ItemDataType);
    for (const auto& arg : args) {
        for (auto i : std::views::iota(Int32{0}) | std::views::take_while([&](Int32 i) {
                          return i < numItemTypes;
                      })) {
            const auto* itemData = dynamic_cast<const ItemData*>(
                dbm->lookupDataBlock(i, DataBlockManager::ItemDataType));
            if (itemData == nullptr) {
                continue;
            }
            if (equalIgnoreCase(itemData->description, arg) ||
                equalIgnoreCase(itemData->dbmName, arg)) {
                if (std::ranges::find(favs, i) == favs.end()) {
                    favs.push_back(i);
                }
                break;
            }
        }
    }
    return favs;
}

std::string FearGuiPurchase::getCellText(Point2I cell) const {
    if (cell.y < 0 || static_cast<std::size_t>(cell.y) >= rows_.size()) {
        return {};
    }
    const auto& r = rows_[static_cast<std::size_t>(cell.y)];
    if (r.itemIndex < 0) {
        return r.description;
    }
    return std::format("{}\t{}", r.description, r.price);
}

} // namespace opent1::game
