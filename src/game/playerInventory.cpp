#include <opent1/game/sound.hpp>
#include <ranges>
#include <opent1/game/player.hpp>
#include <opent1/game/projectile_factory.hpp>
#include <algorithm>

namespace opent1::game {

namespace {

struct BitStreamRollbackGuard {
    BitStream& stream;
    Int32 savedBitPos;
    bool committed{false};

    explicit BitStreamRollbackGuard(BitStream& s) noexcept
        : stream(s), savedBitPos(s.getCurPos()) {}

    ~BitStreamRollbackGuard() noexcept {
        if (!committed) {
            stream.setCurPos(savedBitPos);
        }
    }
};

} // namespace

const ItemImageData* Player::getImageData(Int32 imageId) const noexcept {
    if (imageId < 0) {
        return nullptr;
    }
    if (auto* dbm = resolveDataBlockManager()) {
        return dynamic_cast<const ItemImageData*>(
            dbm->lookupDataBlock(imageId, DataBlockManager::ItemImageDataType));
    }
    return nullptr;
}

const ItemData* Player::getItemData(Int32 itemId) const noexcept {
    if (itemId < 0) {
        return nullptr;
    }
    if (auto* dbm = resolveDataBlockManager()) {
        return dynamic_cast<const ItemData*>(
            dbm->lookupDataBlock(itemId, DataBlockManager::ItemDataType));
    }
    return nullptr;
}

bool Player::setItemCount(Int32 type, Int32 count) {
    if (type < 0 || type >= MaxItemTypes) {
        return false;
    }
    count = std::max<Int32>(0, count);
    auto& entry = itemTypeList_[static_cast<std::size_t>(type)];
    entry.count = count;

    if (!isGhost()) {
        if (entry.count == 0 && entry.imageSlot >= 0) {
            unmountItem(entry.imageSlot);
        }
    }
    for (auto slot : std::views::iota(Int32{0}) | std::views::take_while([&](Int32 slot) {
                         return slot < MaxImageSlots;
                     })) {
        auto& img = itemImageList_[static_cast<std::size_t>(slot)];
        if (img.itemId == type) {
            img.itemCount = count;
        }
    }
    return entry.count > 0;
}

bool Player::incItemCount(Int32 type, Int32 count) {
    if (type < 0 || type >= MaxItemTypes || count <= 0) {
        return false;
    }
    return setItemCount(type, getItemCount(type) + count);
}

bool Player::decItemCount(Int32 type, Int32 count) {
    if (type < 0 || type >= MaxItemTypes || count <= 0) {
        return false;
    }
    return setItemCount(type, getItemCount(type) - count);
}

Int32 Player::getItemCount(Int32 type) const noexcept {
    if (type < 0 || type >= MaxItemTypes) {
        return 0;
    }
    return itemTypeList_[static_cast<std::size_t>(type)].count;
}

bool Player::useItem(Int32 type) {
    if (dead_ || type < 0 || type >= MaxItemTypes) {
        return false;
    }
    if (getItemCount(type) <= 0) {
        return false;
    }
    if (const auto* itemData = getItemData(type)) {
        if (!isGhost()) {
            if (auto* console = resolveConsole()) {
                for (const auto& name :
                     {itemData->dbmName, itemData->className, std::string("Item")}) {
                    const auto callback = name + "::onUse";
                    if (!name.empty() && console->isFunction(callback)) {
                        const std::array<std::string, 2> args{scriptThis(), itemData->dbmName};
                        return console->execute(callback, args).ok();
                    }
                }
            }
        }
        if (itemData->imageId >= 0) {
            if (const auto* imgData = getImageData(itemData->imageId)) {
                return mountItem(type, imgData->mountPoint, getTeam());
            }
        }
    }
    return true;
}

void Player::scriptItemCallback(Int32 type, std::string_view callback, bool deployment) {
    if (isGhost()) {
        return;
    }
    const auto* item = getItemData(type);
    auto* console = resolveConsole();
    if (!item || !console) {
        return;
    }
    for (const auto& name : {item->dbmName, item->className, std::string("Item")}) {
        if (name.empty()) {
            continue;
        }
        const auto function = name + "::" + std::string(callback);
        if (!console->isFunction(function)) {
            continue;
        }
        std::vector<std::string> args{scriptThis(), item->dbmName};
        if (deployment) {
            args.emplace_back("0.000000,0.000000,0.000000");
        }
        (void)console->execute(function, args);
        return;
    }
}

bool Player::dropItem(Int32 type) {
    if (type < 0 || type >= MaxItemTypes) {
        return false;
    }
    if (getItemCount(type)) {
        scriptItemCallback(type, "onDrop");
    }
    return true;
}

bool Player::deployItem(Int32 type) {
    if (type < 0 || type >= MaxItemTypes) {
        return false;
    }
    if (getItemCount(type)) {
        scriptItemCallback(type, "onDeploy", true);
    }
    return true;
}

bool Player::mountItem(Int32 type, Int32 imageSlot, Int32 team) {
    if (imageSlot < 0 || imageSlot >= MaxImageSlots || type < 0 || type >= MaxItemTypes) {
        return false;
    }
    const auto* itemData = getItemData(type);
    if (!itemData || itemData->imageId < 0) {
        return false;
    }
    const auto* imgData = getImageData(itemData->imageId);
    if (!imgData) {
        return false;
    }

    auto& image = itemImageList_[static_cast<std::size_t>(imageSlot)];
    if (image.itemId == type && image.state != ItemImageEntry::Reset &&
        image.state != ItemImageEntry::Deactivate) {
        image.nextId = -2;
        return true;
    }

    if (image.itemId != -1) {
        scriptItemCallback(image.itemId, "onUnmount");
    }
    itemTypeList_[static_cast<std::size_t>(type)].imageSlot = imageSlot;
    setMaskBits(VisItemMask | (1u << (ItemImageMaskBits + imageSlot)));
    setImageType(imageSlot, type, team);
    scriptItemCallback(type, "onMount");
    return true;
}

void Player::setImageType(Int32 imageSlot, Int32 type, Int32 team) {
    auto& image = itemImageList_[static_cast<std::size_t>(imageSlot)];
    if (image.itemId == type) {
        image.nextId = -2;
        return;
    }
    if (!isGhost() && type != -1 &&
        (image.state == ItemImageEntry::Fire || image.state == ItemImageEntry::Reload)) {
        image.nextId = type;
        image.nextTeam = team;
        return;
    }
    resetImageSlot(imageSlot);
    if (type == -1) {
        return;
    }
    const auto* itemData = getItemData(type);
    const auto* imgData = itemData ? getImageData(itemData->imageId) : nullptr;
    if (!imgData) {
        return;
    }
    image.itemId = type;
    image.itemCount = getItemCount(type);
    image.imageId = itemData->imageId;
    image.teamId = team;
    image.ammo = imgData->ammoType == -1 ? energy_ > imgData->minEnergy
                                         : getItemCount(imgData->ammoType) > 0;
    itemTypeList_[static_cast<std::size_t>(type)].imageSlot = imageSlot;
    setImageState(imageSlot, ItemImageEntry::Activate, true);
    updateImageMass();
}

bool Player::unmountItem(Int32 imageSlot) {
    if (imageSlot < 0 || imageSlot >= MaxImageSlots) {
        return false;
    }
    auto& image = itemImageList_[static_cast<std::size_t>(imageSlot)];
    if (image.itemId == -1 && image.imageId == -1) {
        return false;
    }
    if (image.itemId >= 0 && image.itemId < MaxItemTypes) {
        itemTypeList_[static_cast<std::size_t>(image.itemId)].imageSlot = -1;
    }
    setMaskBits(VisItemMask | (1u << (ItemImageMaskBits + imageSlot)));
    const auto oldType = image.itemId;
    setImageType(imageSlot, -1, -1);
    if (oldType != -1) {
        scriptItemCallback(oldType, "onUnmount");
    }
    return true;
}

Int32 Player::getMountedItem(Int32 imageSlot) const noexcept {
    if (imageSlot < 0 || imageSlot >= MaxImageSlots) {
        return -1;
    }
    return itemImageList_[static_cast<std::size_t>(imageSlot)].itemId;
}

void Player::setImageTriggerDown(Int32 imageSlot) {
    if (isGhost() || dead_ || imageSlot < 0 || imageSlot >= MaxImageSlots) {
        return;
    }
    auto& image = itemImageList_[static_cast<std::size_t>(imageSlot)];
    if (image.imageId == -1) {
        return;
    }
    const auto* imgData = getImageData(image.imageId);
    if (!imgData || imgData->weaponType == -1) {
        return;
    }
    if (!image.triggerDown) {
        image.triggerDown = true;
        setMaskBits(VisItemMask | (1u << (ItemImageMaskBits + imageSlot)));
        if (image.state == ItemImageEntry::Fire &&
            (imgData->weaponType == Spinning || imgData->weaponType == Sustained)) {
            image.fireCount = (image.fireCount + 1) & 0x7;
        }
        updateImageState(imageSlot, 0.0f);
    }
}

void Player::setImageTriggerUp(Int32 imageSlot) {
    if (isGhost() || imageSlot < 0 || imageSlot >= MaxImageSlots) {
        return;
    }
    auto& image = itemImageList_[static_cast<std::size_t>(imageSlot)];
    if (image.imageId == -1) {
        return;
    }
    if (image.triggerDown) {
        image.triggerDown = false;
        setMaskBits(VisItemMask | (1u << (ItemImageMaskBits + imageSlot)));
        updateImageState(imageSlot, 0.0f);
    }
}

bool Player::isImageTriggerDown(Int32 imageSlot) const noexcept {
    if (isGhost() || imageSlot < 0 || imageSlot >= MaxImageSlots) {
        return false;
    }
    const auto& image = itemImageList_[static_cast<std::size_t>(imageSlot)];
    if (image.imageId == -1) {
        return false;
    }
    return image.triggerDown;
}

void Player::stopImageSound(Int32 slot, bool loopingOnly) {
    auto& image = itemImageList_[static_cast<std::size_t>(slot)];
    if (loopingOnly && !image.animLoopingSound) {
        return;
    }
    if (image.animSound && getManager()) {
        auto* playback =
            dynamic_cast<SoundPlayback*>(getManager()->findObject("GameSoundPlayback"));
        if (playback && playback->stop) {
            playback->stop(image.animSound);
        }
    }
    image.animSound = 0;
    image.animLoopingSound = false;
}

void Player::resetImageSlot(Int32 imageSlot) {
    if (imageSlot < 0 || imageSlot >= MaxImageSlots) {
        return;
    }
    auto& image = itemImageList_[static_cast<std::size_t>(imageSlot)];
    stopImageFire(imageSlot);
    stopImageSound(imageSlot);
    if (image.itemId >= 0 && image.itemId < MaxItemTypes) {
        itemTypeList_[static_cast<std::size_t>(image.itemId)].imageSlot = -1;
    }
    image.state = ItemImageEntry::Reset;
    image.itemId = -1;
    image.itemCount = 0;
    image.imageId = -1;
    image.teamId = -1;
    image.nextId = -2;
    image.nextTeam = -1;
    image.delayTime = 0.0f;
    image.fireCount = 0;
    image.triggerDown = false;
    image.ammo = false;
    image.shape.reset();
    image.resources.reset();
    image.renderedImageId = -1;
    updateImageMass();
    if (!isGhost()) {
        setMaskBits(VisItemMask | (1u << (ItemImageMaskBits + imageSlot)));
    }
}

Int32 Player::getImageState(Int32 imageSlot) const noexcept {
    if (imageSlot < 0 || imageSlot >= MaxImageSlots) {
        return ItemImageEntry::Reset;
    }
    return itemImageList_[static_cast<std::size_t>(imageSlot)].state;
}

void Player::scriptImageCallback(Int32 imageSlot, std::string_view callback) {
    if (imageSlot < 0 || imageSlot >= MaxImageSlots) {
        return;
    }
    auto* console = resolveConsole();
    const auto& image = itemImageList_[static_cast<std::size_t>(imageSlot)];
    const auto* imageData = getImageData(image.imageId);
    const auto* itemData = getItemData(image.itemId);
    if (!console || !imageData || !itemData) {
        return;
    }
    // Player::scriptImageName in 1.11 resolves image name, item class, then
    // ItemImage. Script weapons (including TRPG melee) have no projectile.
    for (const std::string_view name : {std::string_view(imageData->dbmName),
                                        std::string_view(itemData->className),
                                        std::string_view("ItemImage")}) {
        if (name.empty()) {
            continue;
        }
        const auto function = std::format("{}::{}", name, callback);
        if (console->isFunction(function)) {
            const std::array<std::string, 2> args{scriptThis(), std::to_string(imageSlot)};
            (void)console->execute(function, args);
            return;
        }
    }
}

void Player::fireImage(Int32 imageSlot) {
    if (imageSlot < 0 || imageSlot >= MaxImageSlots) {
        return;
    }
    auto& image = itemImageList_[static_cast<std::size_t>(imageSlot)];
    const auto* imgData = getImageData(image.imageId);
    if (!imgData) {
        return;
    }
    if (!isGhost()) {
        if (imgData->projectile.type == -1) {
            scriptImageCallback(imageSlot, "onFire");
            return;
        }
        Flt32 energyUsed = 0.0f;
        Flt32 pOfM = 1.0f;
        const bool sustained = imgData->weaponType == Sustained;
        if (sustained && getManager() &&
            dynamic_cast<Projectile*>(image.sustainedProjectile.resolve(*getManager()))) {
            return;
        }
        if (!sustained && imgData->ammoType != -1) {
            decItemCount(imgData->ammoType, 1);
            image.ammo = (getItemCount(imgData->ammoType) > 0);
        } else if (!sustained && imgData->projectile.type != -1 && imgData->maxEnergy > 0.0f) {
            if (imgData->maxEnergy > energy_) {
                energyUsed = energy_;
                energy_ = 0.0f;
            } else {
                energyUsed = imgData->maxEnergy;
                energy_ -= imgData->maxEnergy;
            }
            pOfM = energyUsed / imgData->maxEnergy;
            image.ammo = (energy_ > imgData->minEnergy);
        }

        if (getManager() && imgData->projectile.type != -1 && imgData->projectile.dataType != -1) {
            TMat3F muzzleTrans = getTransform();
            if (imgData->accuFire && !dead_) {
                getAimedMuzzleTransform(imageSlot, &muzzleTrans);
            } else {
                TMat3F local;
                if (getMuzzleTransform(imageSlot, &local)) {
                    m_mul(local, getTransform(), muzzleTrans);
                }
            }
            if (auto proj = createProjectile(imgData->projectile)) {
                proj->bindDataBlockManager(resolveDataBlockManager());
                proj->initProjectile(muzzleTrans, getLinearVelocity(), getId());
                if (!sustained) {
                    proj->setEnergy(energyUsed, pOfM);
                }
                if (proj->isTargetable()) {
                    Point3F position, normal;
                    proj->setTarget(dynamic_cast<ShapeBase*>(findLOSObject(getEyeTransform(),
                                                                           proj->getTargetRange(),
                                                                           AllObjectTypes,
                                                                           &position,
                                                                           &normal)));
                }
                auto* projectile = proj.get();
                if (getManager()->addObject(std::move(proj)) && sustained) {
                    image.sustainedProjectile = projectile->address();
                }
            }
        }
    }
}

void Player::updateImageFire(Int32 imageSlot) {
    if (isGhost() || !getManager() || imageSlot < 0 || imageSlot >= MaxImageSlots) {
        return;
    }
    auto& image = itemImageList_[static_cast<std::size_t>(imageSlot)];
    const auto* data = getImageData(image.imageId);
    auto* projectile = dynamic_cast<Projectile*>(image.sustainedProjectile.resolve(*getManager()));
    if (!data || !projectile) {
        return;
    }
    TMat3F transform = getTransform();
    if (data->accuFire && !dead_) {
        getAimedMuzzleTransform(imageSlot, &transform);
    } else {
        TMat3F local;
        if (getMuzzleTransform(imageSlot, &local)) {
            m_mul(local, getTransform(), transform);
        }
    }
    projectile->updateImageTransform(transform);
}

void Player::stopImageFire(Int32 imageSlot) {
    if (imageSlot < 0 || imageSlot >= MaxImageSlots) {
        return;
    }
    auto& image = itemImageList_[static_cast<std::size_t>(imageSlot)];
    const auto address = image.sustainedProjectile;
    image.sustainedProjectile = {};
    if (!isGhost() && getManager()) {
        if (auto* projectile = dynamic_cast<Projectile*>(address.resolve(*getManager()))) {
            projectile->shutOffProjectile();
        }
    }
}

void Player::setImageState(Int32 imageSlot, Int32 state, bool force) {
    (void)force;
    if (imageSlot < 0 || imageSlot >= MaxImageSlots) {
        return;
    }
    auto& image = itemImageList_[static_cast<std::size_t>(imageSlot)];
    if (image.imageId == -1) {
        return;
    }
    const auto* imgData = getImageData(image.imageId);
    if (!imgData) {
        return;
    }

    const Int32 lastState = image.state;
    const Flt32 lastDelay = image.delayTime;
    if (lastState == ItemImageEntry::Fire && imgData->weaponType == Sustained) {
        stopImageFire(imageSlot);
    }
    image.state = state;
    if (image.nextId >= -1 &&
        (lastState == ItemImageEntry::Fire || lastState == ItemImageEntry::Reload)) {
        setImageType(imageSlot, image.nextId, image.nextTeam);
        return;
    }
    if (isGhost()) {
        stopImageSound(imageSlot, true);
        Int32 sound = -1;
        switch (state) {
            case ItemImageEntry::Activate:
                sound = imgData->sfxActivateTag;
                break;
            case ItemImageEntry::Fire:
                sound = imgData->sfxFireTag;
                break;
            case ItemImageEntry::Reload:
                sound = imgData->sfxReloadTag;
                break;
            case ItemImageEntry::Ready:
                sound = imgData->sfxReadyTag;
                break;
            case ItemImageEntry::SpinUp:
                sound = imgData->sfxSpinUpTag;
                break;
            case ItemImageEntry::SpinDown:
                sound = imgData->sfxSpinDownTag;
                break;
            default:
                break;
        }
        if (sound >= 0) {
            image.animSound =
                TSFX::PlayAt(UInt32(sound), getTransform(), getLinearVelocity(), getManager());
            auto* dbm = resolveDataBlockManager();
            const auto* soundData = dbm ? dynamic_cast<const SoundData*>(dbm->lookupDataBlock(
                                              sound, DataBlockManager::SoundDataType))
                                        : nullptr;
            const auto* profile =
                soundData ? dynamic_cast<const SoundProfileData*>(dbm->lookupDataBlock(
                                soundData->profileIndex, DataBlockManager::SoundProfileDataType))
                          : nullptr;
            // Native SFX_IS_LOOPING profile bit; one-shots finish across state changes.
            image.animLoopingSound = profile && (profile->profile.flags & (1u << 1)) != 0;
        }
    }

    switch (state) {
        case ItemImageEntry::Reset:
            resetImageSlot(imageSlot);
            return;
        case ItemImageEntry::Fire:
            image.delayTime = imgData->fireTime;
            if (!isGhost()) {
                setMaskBits(VisItemMask | (1u << (ItemImageMaskBits + imageSlot)));
                image.fireCount = (image.fireCount + 1) & 0x7;
            }
            fireImage(imageSlot);
            break;
        case ItemImageEntry::Reload:
            if (imgData->ammoType != -1 && !image.ammo) {
                setImageState(imageSlot, ItemImageEntry::NoAmmo);
                return;
            }
            image.delayTime = imgData->reloadTime;
            if (imgData->reloadTime == 0.0f) {
                setImageState(imageSlot, ItemImageEntry::Ready);
                return;
            }
            break;
        case ItemImageEntry::NoAmmo:
            break;
        case ItemImageEntry::Idle:
            break;
        case ItemImageEntry::Ready:
            if (!isGhost() && image.triggerDown) {
                setImageState(imageSlot, ItemImageEntry::Fire);
                return;
            }
            break;
        case ItemImageEntry::SpinUp:
            if (lastState == ItemImageEntry::SpinDown && imgData->spinDownTime > 0.0f) {
                const Flt32 scale = 1.0f - (lastDelay / imgData->spinDownTime);
                image.delayTime = imgData->spinUpTime * scale;
            } else {
                image.delayTime = imgData->spinUpTime;
            }
            break;
        case ItemImageEntry::SpinDown:
            if (lastState == ItemImageEntry::SpinUp && imgData->spinUpTime > 0.0f) {
                const Flt32 scale = 1.0f - (lastDelay / imgData->spinUpTime);
                image.delayTime = imgData->spinDownTime * scale;
            } else {
                image.delayTime = imgData->spinDownTime;
            }
            break;
        case ItemImageEntry::Activate:
            image.delayTime = imgData->activateTime;
            break;
        case ItemImageEntry::Deactivate:
            break;
        default:
            break;
    }
}

void Player::updateImageState(Int32 imageSlot, Flt32 dt) {
    if (imageSlot < 0 || imageSlot >= MaxImageSlots) {
        return;
    }
    auto& image = itemImageList_[static_cast<std::size_t>(imageSlot)];
    if (image.imageId == -1) {
        return;
    }
    const auto* imgData = getImageData(image.imageId);
    if (!imgData) {
        return;
    }

    image.delayTime -= dt;

    if (!isGhost()) {
        const bool hasAmmo = (imgData->ammoType == -1) ? (energy_ > imgData->minEnergy)
                                                       : (getItemCount(imgData->ammoType) > 0);
        if (hasAmmo != image.ammo) {
            setMaskBits(VisItemMask | (1u << (ItemImageMaskBits + imageSlot)));
            image.ammo = hasAmmo;
        }
    }

    switch (image.state) {
        case ItemImageEntry::Fire:
            if (imgData->weaponType == Sustained) {
                updateImageFire(imageSlot);
                if (imgData->ammoType == -1) {
                    energy_ -= imgData->maxEnergy * dt;
                    if (energy_ < 0.0f) {
                        energy_ = 0.0f;
                    } else if (playerData_ && energy_ > playerData_->maxEnergy) {
                        energy_ = playerData_->maxEnergy;
                    }
                }
            }
            if (image.delayTime <= 0.0f) {
                if (imgData->weaponType == Spinning) {
                    if (!image.triggerDown || !image.ammo) {
                        setImageState(imageSlot, ItemImageEntry::SpinDown);
                    } else {
                        image.delayTime = imgData->fireTime;
                        fireImage(imageSlot);
                    }
                } else if (imgData->weaponType == Sustained) {
                    if (!image.triggerDown || !image.ammo) {
                        setImageState(imageSlot, ItemImageEntry::Reload);
                    }
                } else {
                    setImageState(imageSlot, ItemImageEntry::Reload);
                }
            }
            break;

        case ItemImageEntry::Reload:
            if (image.delayTime <= 0.0f) {
                if (imgData->weaponType == DiscLauncher) {
                    setImageState(imageSlot, ItemImageEntry::SpinUp);
                } else {
                    setImageState(imageSlot, ItemImageEntry::Ready);
                }
            }
            break;

        case ItemImageEntry::NoAmmo:
            if (image.ammo) {
                if (imgData->weaponType == Spinning) {
                    if (image.triggerDown) {
                        setImageState(imageSlot, ItemImageEntry::SpinUp);
                    } else {
                        setImageState(imageSlot, ItemImageEntry::Idle);
                    }
                } else {
                    setImageState(imageSlot, ItemImageEntry::Reload);
                }
            }
            break;

        case ItemImageEntry::Idle:
            if (!image.ammo) {
                setImageState(imageSlot, ItemImageEntry::NoAmmo);
            } else if (image.triggerDown) {
                if (imgData->weaponType == Spinning) {
                    setImageState(imageSlot, ItemImageEntry::SpinUp);
                } else {
                    setImageState(imageSlot, ItemImageEntry::Ready);
                }
            }
            break;

        case ItemImageEntry::Ready:
            if (imgData->weaponType == Spinning) {
                if (!image.triggerDown || !image.ammo) {
                    setImageState(imageSlot, ItemImageEntry::SpinDown);
                }
            } else {
                if (!isGhost() && image.triggerDown) {
                    setImageState(imageSlot, ItemImageEntry::Fire);
                }
            }
            break;

        case ItemImageEntry::SpinUp:
            if (imgData->weaponType == DiscLauncher) {
                if (!image.ammo) {
                    setImageState(imageSlot, ItemImageEntry::NoAmmo);
                    break;
                }
            } else if (!image.triggerDown || !image.ammo) {
                setImageState(imageSlot, ItemImageEntry::SpinDown);
                break;
            }
            if (image.delayTime <= 0.0f) {
                setImageState(imageSlot, ItemImageEntry::Ready);
            }
            break;

        case ItemImageEntry::SpinDown:
            if (image.triggerDown && image.ammo) {
                setImageState(imageSlot, ItemImageEntry::SpinUp);
            } else if (image.delayTime <= 0.0f) {
                setImageState(imageSlot, ItemImageEntry::Idle);
            }
            break;

        case ItemImageEntry::Reset:
        case ItemImageEntry::Activate:
            if (image.delayTime <= 0.0f) {
                if (imgData->weaponType == Spinning) {
                    setImageState(imageSlot, ItemImageEntry::Idle);
                } else if (!image.ammo) {
                    setImageState(imageSlot, ItemImageEntry::NoAmmo);
                } else if (imgData->weaponType == DiscLauncher) {
                    setImageState(imageSlot, ItemImageEntry::Reload);
                } else {
                    setImageState(imageSlot, ItemImageEntry::Ready);
                }
            }
            break;

        case ItemImageEntry::Deactivate:
            break;

        default:
            break;
    }
}

void Player::updateItemImages(Flt32 dt) {
    for (auto slot : std::views::iota(Int32{0}) | std::views::take_while([&](Int32 slot) {
                         return slot < MaxImageSlots;
                     })) {
        updateImageState(slot, dt);
        updateImageAnimation(itemImageList_[slot], dt);
        const auto& image = itemImageList_[slot];
        if (image.animSound && getManager()) {
            auto* playback =
                dynamic_cast<SoundPlayback*>(getManager()->findObject("GameSoundPlayback"));
            if (playback && playback->move) {
                playback->move(image.animSound, getTransform(), getLinearVelocity());
            }
        }
    }
}

void Player::updateImageMass() {
    Flt32 totalMass = playerData_ ? playerData_->mass : 9.0f;
    if (totalMass <= 0.0f) {
        totalMass = 9.0f;
    }
    for (auto slot : std::views::iota(Int32{0}) | std::views::take_while([&](Int32 slot) {
                         return slot < MaxImageSlots;
                     })) {
        const auto& img = itemImageList_[static_cast<std::size_t>(slot)];
        if (img.imageId != -1) {
            if (const auto* imgData = getImageData(img.imageId)) {
                totalMass += imgData->mass;
            }
        }
    }
    setMass(totalMass);
}

void Player::packItemImages(BitStream& stream, UInt32 mask) const {
    if (stream.writeFlag((mask & VisItemMask) != 0)) {
        for (auto i : std::views::iota(Int32{0}) | std::views::take_while([&](Int32 i) {
                          return i < MaxItemImages;
                      })) {
            if (stream.writeFlag((mask & (1u << (ItemImageMaskBits + i))) != 0)) {
                const auto& itemImage = itemImageList_[static_cast<std::size_t>(i)];
                stream.writeInt(itemImage.itemId + 1, ItemTypeBits);
                if (stream.writeFlag(itemImage.teamId != -1)) {
                    stream.writeInt(itemImage.teamId, 3);
                }
                stream.writeFlag(itemImage.triggerDown);
                stream.writeFlag(itemImage.ammo);
                stream.writeInt(itemImage.fireCount & 0x7, 3);
                if ((mask & InitialUpdate) != 0) {
                    stream.writeFlag(itemImage.state == ItemImageEntry::Fire);
                }
            }
        }
    }
}

bool Player::unpackItemImages(BitStream& stream, bool initialUpdate) {
    BitStreamRollbackGuard guard(stream);
    const auto savedImages = itemImageList_;

    if (stream.readFlag()) {
        for (auto i : std::views::iota(Int32{0}) | std::views::take_while([&](Int32 i) {
                          return i < MaxItemImages;
                      })) {
            if (stream.readFlag()) {
                auto& itemImage = itemImageList_[static_cast<std::size_t>(i)];
                const Int32 type = stream.readInt(ItemTypeBits) - 1;
                Int32 team = -1;
                if (stream.readFlag()) {
                    team = stream.readInt(3);
                }
                const bool trig = stream.readFlag();
                const bool hasAmmo = stream.readFlag();
                const Int32 count = stream.readInt(3);
                bool initFire = false;
                if (initialUpdate) {
                    initFire = stream.readFlag();
                }
                if (!stream.isValid()) {
                    itemImageList_ = savedImages;
                    return false;
                }

                if (itemImage.itemId != type) {
                    // Mount changes reset the animation, sound, pending image
                    // and fire counter together, as in legacy setImageType.
                    // Comparing against the previous weapon's counter turns a
                    // newly equipped (count zero) image into a phantom shot.
                    setImageType(i, type, team);
                } else {
                    itemImage.teamId = team;
                }

                itemImage.triggerDown = trig;
                itemImage.ammo = hasAmmo;
                if (!initialUpdate) {
                    if (count != itemImage.fireCount) {
                        itemImage.fireCount = count;
                        setImageState(i, ItemImageEntry::Fire);
                    }
                    updateImageState(i, 0.0f);
                } else {
                    itemImage.fireCount = count;
                    if (initFire) {
                        itemImage.state = ItemImageEntry::Fire;
                    }
                }
            }
        }
    }

    if (!stream.isValid()) {
        itemImageList_ = savedImages;
        return false;
    }

    updateImageMass();
    guard.committed = true;
    return true;
}

} // namespace opent1::game
