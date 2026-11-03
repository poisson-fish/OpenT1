#include <opent1/game/status_combat_huds.hpp>
#include <ranges>

#include <algorithm>
#include <format>

namespace opent1::game {

CurrentWeaponHud::CurrentWeaponHud() {
    fields_.extent = {79, 34};
}

CurrentWeaponHud::Layout CurrentWeaponHud::computeLayout(const PlayerPSC* psc,
                                                         const DataBlockManager* dbm,
                                                         bool lowRes,
                                                         Int32 defaultIconHeight) {
    Layout out{};
    out.lowRes = lowRes;
    out.transWidth = lowRes ? 26 : 36;
    out.iconSpacing = lowRes ? 2 : 4;
    out.iconOffsetY = lowRes ? 6 : 7;
    out.extent.x = lowRes ? 52 : 79;

    if (psc == nullptr || dbm == nullptr) {
        out.extent.y = 2 * out.iconOffsetY;
        return out;
    }

    const Int32 numItemTypes = dbm->getDataGroupSize(DataBlockManager::ItemDataType);
    for (auto i : std::views::iota(Int32{0}) | std::views::take_while([&](Int32 i) {
                      return i < numItemTypes;
                  })) {
        if (psc->itemCount(i) <= 0) {
            continue;
        }
        const auto* itemData =
            dynamic_cast<const ItemData*>(dbm->lookupDataBlock(i, DataBlockManager::ItemDataType));
        if (itemData == nullptr || !itemData->showWeaponBar || itemData->hudIcon.empty()) {
            continue;
        }
        WeaponSlot slot{};
        slot.itemTypeId = i;
        slot.hudIcon = itemData->hudIcon;
        if (const auto* player = dynamic_cast<const Player*>(psc->getControlObject())) {
            for (auto mount : std::views::iota(Int32{0}) | std::views::take_while([&](Int32 mount) {
                                  return mount < Player::MaxItemImages;
                              })) {
                slot.mounted = slot.mounted || player->getMountedItem(mount) == i;
            }
        }
        if (itemData->imageId >= 0) {
            const auto* imgData = dynamic_cast<const ItemImageData*>(
                dbm->lookupDataBlock(itemData->imageId, DataBlockManager::ItemImageDataType));
            if (imgData != nullptr) {
                if (imgData->ammoType < 0) {
                    slot.infiniteAmmo = true;
                } else {
                    slot.ammoCount = std::min<Int32>(999, psc->itemCount(imgData->ammoType));
                    slot.ammoText = std::format("{}", slot.ammoCount);
                }
            }
        }
        out.slots.push_back(std::move(slot));
    }

    if (!out.slots.empty()) {
        const Int32 weapCount = static_cast<Int32>(out.slots.size());
        const Int32 iconH = std::max<Int32>(1, defaultIconHeight);
        out.extent.y =
            weapCount * (iconH + out.iconSpacing) - out.iconSpacing + 2 * out.iconOffsetY;
    } else {
        out.extent.y = 2 * out.iconOffsetY;
    }
    return out;
}

void CurrentWeaponHud::updateFromPSC(const PlayerPSC* psc,
                                     const DataBlockManager* dbm,
                                     bool lowRes) {
    layout_ = computeLayout(psc, dbm, lowRes, lowRes ? 13 : 20);
    lowResMode_ = lowRes;
    resize(fields_.position, layout_.extent);
}

bool CurrentWeaponHud::onAdd() {
    if (!Parent::onAdd()) {
        return false;
    }
    return true;
}

bool CurrentWeaponHud::draw(gfx::SoftwareSurface& s, Point2I offset, RectI clip) const {
    if (!Parent::draw(s, offset, clip)) {
        return false;
    }
    Point2I at = offset + Point2I{7, layout_.iconOffsetY};
    for (const auto& slot : layout_.slots) {
        const auto* icon = sprite("I_" + slot.hudIcon + (slot.mounted ? "_on.bmp" : "_off.bmp"));
        if (!icon) {
            icon = sprite("I_weapon_off.bmp");
        }
        if (icon && !s.blit(*icon, at)) {
            return false;
        }
        const auto textAt = at + Point2I{(icon ? icon->width : 20) + 4, 0};
        if (!drawSprite(s, "ammoSh.bmp", textAt)) {
            return false;
        }
        if (slot.infiniteAmmo) {
            if (!drawSprite(s,
                            slot.mounted ? "I_Infinity_on.bmp" : "I_Infinity_off.bmp",
                            textAt + Point2I{4, 5})) {
                return false;
            }
        } else if (const auto& font = slot.mounted ? activeFont : hudFont;
                   font && !s.text(*font, textAt + Point2I{3, -1}, slot.ammoText)) {
            return false;
        }
        at.y += (icon ? icon->height : 20) + layout_.iconSpacing;
    }
    return true;
}

} // namespace opent1::game
