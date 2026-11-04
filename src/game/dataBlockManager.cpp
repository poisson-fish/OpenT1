#include <opent1/console/console.hpp>
#include <ranges>
#include <opent1/core/text.hpp>
#include <opent1/game/data_block_manager.hpp>
#include <opent1/game/version.hpp>
#include <opent1/sim/sim_resource_volume.hpp>
#include <algorithm>
#include <bit>
#include <cctype>
#include <cmath>
#include <cstring>
#include <format>
#include <limits>

namespace opent1::game {
namespace {

constexpr std::array<std::string_view, DataBlockManager::NumDataTypes> kDataNames{{
    "Sound Profile",  "Sound",           "Damage Skin", "Armor",   "Static Shape", "Item",
    "Item Image",     "Moveable",        "Sensor",      "Vehicle", "Flier",        "Tank",
    "Hover",          "Projectile",      "Bullet",      "Grenade", "Rocket",       "Laser",
    "Interior Shape", "Turret",          "Explosion",   "Marker",  "Debris",       "Mine",
    "Target Laser",   "Seeking Missile", "Trigger",     "Car",     "Lightning",    "Repair Effect",
    "IRC Channel",
}};

[[nodiscard]] char toLowerAscii(char c) noexcept {
    return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
}

[[nodiscard]] Int32 countBits(Int32 val) noexcept {
    return static_cast<Int32>(std::bit_width(static_cast<UInt32>(val)));
}

template <class T> void writePod(BitStream& stream, const T& val) {
    static_assert(std::is_trivially_copyable_v<T>);
    std::array<std::byte, sizeof(T)> bytes{};
    std::memcpy(bytes.data(), &val, sizeof(T));
    stream.writeBytes(bytes);
}

void writeLegacyFloat(BitStream& stream, Flt32 value, Int32 bits) {
    // Original datablocks truncate the scaled float and write its low bits.
    // Mods use values outside [0,1] (Wasteland's MutantArmor uses 1.2).
    // Keep the general BitStream normalized-float API strict.
    OPENT1_ASSERT_FATAL(bits >= 1 && bits <= 30, "Invalid datablock float bit width");
    const auto encoded = value * Flt32((UInt32{1} << bits) - 1);
    OPENT1_ASSERT_FATAL(std::isfinite(encoded) &&
                            double(encoded) >= double(std::numeric_limits<Int32>::min()) &&
                            double(encoded) <= double(std::numeric_limits<Int32>::max()),
                        "Invalid quantized datablock float");
    stream.writeInt(static_cast<Int32>(encoded), bits);
}

template <class T> bool readPod(BitStream& stream, T& val) {
    static_assert(std::is_trivially_copyable_v<T>);
    std::array<std::byte, sizeof(T)> bytes{};
    if (!stream.readBytes(bytes) || !stream.isValid()) {
        return false;
    }
    std::memcpy(&val, bytes.data(), sizeof(T));
    return true;
}

void writePoint3F(BitStream& stream, const Point3F& pt) {
    writePod(stream, pt.x);
    writePod(stream, pt.y);
    writePod(stream, pt.z);
}

bool readPoint3F(BitStream& stream, Point3F& pt) {
    return readPod(stream, pt.x) && readPod(stream, pt.y) && readPod(stream, pt.z);
}

void writeColorF(BitStream& stream, const ColorF& c) {
    writePod(stream, c.red);
    writePod(stream, c.green);
    writePod(stream, c.blue);
}

bool readColorF(BitStream& stream, ColorF& c) {
    return readPod(stream, c.red) && readPod(stream, c.green) && readPod(stream, c.blue);
}

void writeBlockIdHelper(BitStream& stream, DataBlockManager* dbm, Int32 group, Int32 blockId) {
    if (dbm) {
        dbm->writeBlockId(stream, group, blockId);
    } else {
        stream.writeInt(blockId + 1, 8);
    }
}

Int32 readBlockIdHelper(BitStream& stream, DataBlockManager* dbm, Int32 group) {
    if (dbm) {
        return dbm->readBlockId(stream, group);
    }
    return stream.readInt(8) - 1;
}

[[nodiscard]] std::string ensureDtsExtension(std::string_view baseName) {
    std::string result(baseName);
    if (result.size() < 4 ||
        !equalIgnoreCase(std::string_view(result).substr(result.size() - 4), ".dts")) {
        result += ".dts";
    }
    return result;
}

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

// -----------------------------------------------------------------------------
// GameBaseData
// -----------------------------------------------------------------------------

GameBaseData::GameBaseData() = default;

bool GameBaseData::preload(ResourceManager* /*rm*/, bool /*server*/, std::string& /*errorBuffer*/) {
    return true;
}

void GameBaseData::pack(BitStream& stream, DataBlockManager* /*dbm*/) const {
    stream.writeSignedInt(mapFilter, 8);
    stream.writeString(mapIcon);
    stream.writeString(description);
}

bool GameBaseData::unpack(BitStream& stream, DataBlockManager* /*dbm*/) {
    BitStreamRollbackGuard streamGuard(stream);
    const auto backup = *this;
    mapFilter = stream.readSignedInt(8);
    if (!stream.readString(mapIcon) || !stream.readString(description) || !stream.isValid()) {
        *this = backup;
        return false;
    }
    streamGuard.committed = true;
    return true;
}

// -----------------------------------------------------------------------------
// SoundProfileData
// -----------------------------------------------------------------------------

SoundProfileData::SoundProfileData() = default;

bool SoundProfileData::preload(ResourceManager* /*rm*/,
                               bool /*server*/,
                               std::string& /*errorBuffer*/) {
    dist = profile.minDistance + (profile.maxDistance - profile.minDistance) * 0.65f;
    return true;
}

void SoundProfileData::pack(BitStream& stream, DataBlockManager* /*dbm*/) const {
    const SoundProfileData test;
    stream.writeInt(static_cast<Int32>(profile.flags), 6);
    writeLegacyFloat(stream, profile.baseVolume, 10);
    if (stream.writeFlag(test.profile.coneInsideAngle != profile.coneInsideAngle)) {
        writeLegacyFloat(stream, profile.coneInsideAngle / 360.0f, 10);
    }
    if (stream.writeFlag(test.profile.coneOutsideAngle != profile.coneOutsideAngle)) {
        writeLegacyFloat(stream, profile.coneOutsideAngle / 360.0f, 10);
    }
    if (stream.writeFlag(profile.coneVector.x != test.profile.coneVector.x ||
                         profile.coneVector.y != test.profile.coneVector.y ||
                         profile.coneVector.z != test.profile.coneVector.z)) {
        const NormalVector3F nv{profile.coneVector.x, profile.coneVector.y, profile.coneVector.z};
        stream.writeNormalVector(nv, 10);
    }
    if (stream.writeFlag(test.profile.coneOutsideVolume != profile.coneOutsideVolume)) {
        writePod(stream, profile.coneOutsideVolume);
    }
    if (stream.writeFlag(test.profile.minDistance != profile.minDistance)) {
        writePod(stream, profile.minDistance);
    }
    if (stream.writeFlag(test.profile.maxDistance != profile.maxDistance)) {
        writePod(stream, profile.maxDistance);
    }
}

bool SoundProfileData::unpack(BitStream& stream, DataBlockManager* /*dbm*/) {
    BitStreamRollbackGuard streamGuard(stream);
    const auto backup = *this;
    profile = Profile{};
    profile.flags = static_cast<UInt32>(stream.readInt(6));
    profile.baseVolume = stream.readFloat(10);
    if (stream.readFlag()) {
        profile.coneInsideAngle = stream.readFloat(10) * 360.0f;
    }
    if (stream.readFlag()) {
        profile.coneOutsideAngle = stream.readFloat(10) * 360.0f;
    }
    if (stream.readFlag()) {
        NormalVector3F nv{};
        stream.readNormalVector(nv, 10);
        profile.coneVector = Point3F{nv.x, nv.y, nv.z};
    }
    if (stream.readFlag() && !readPod(stream, profile.coneOutsideVolume)) {
        *this = backup;
        return false;
    }
    if (stream.readFlag() && !readPod(stream, profile.minDistance)) {
        *this = backup;
        return false;
    }
    if (stream.readFlag() && !readPod(stream, profile.maxDistance)) {
        *this = backup;
        return false;
    }
    if (!stream.isValid()) {
        *this = backup;
        return false;
    }
    streamGuard.committed = true;
    return true;
}

// -----------------------------------------------------------------------------
// SoundData
// -----------------------------------------------------------------------------

SoundData::SoundData() = default;

bool SoundData::preload(ResourceManager* /*rm*/, bool /*server*/, std::string& /*errorBuffer*/) {
    return true;
}

void SoundData::pack(BitStream& stream, DataBlockManager* dbm) const {
    stream.writeString(wavFileName);
    writeLegacyFloat(stream, priority, 6);
    writeBlockIdHelper(stream, dbm, DataBlockManager::SoundProfileDataType, profileIndex);
}

bool SoundData::unpack(BitStream& stream, DataBlockManager* dbm) {
    BitStreamRollbackGuard streamGuard(stream);
    const auto backup = *this;
    if (!stream.readString(wavFileName)) {
        *this = backup;
        return false;
    }
    priority = stream.readFloat(6);
    profileIndex = readBlockIdHelper(stream, dbm, DataBlockManager::SoundProfileDataType);
    if (!stream.isValid()) {
        *this = backup;
        return false;
    }
    streamGuard.committed = true;
    return true;
}

// -----------------------------------------------------------------------------
// DamageSkinData
// -----------------------------------------------------------------------------

DamageSkinData::DamageSkinData() = default;

bool DamageSkinData::preload(ResourceManager* /*rm*/,
                             bool /*server*/,
                             std::string& /*errorBuffer*/) {
    return true;
}

void DamageSkinData::pack(BitStream& stream, DataBlockManager* /*dbm*/) const {
    for (auto i : std::views::iota(std::size_t{0}) | std::views::take_while([&](std::size_t i) {
                      return i < kBmpCount;
                  })) {
        stream.writeString(bmpName[i]);
    }
}

bool DamageSkinData::unpack(BitStream& stream, DataBlockManager* /*dbm*/) {
    const auto backup = *this;
    numBmps = 0;
    for (auto i : std::views::iota(std::size_t{0}) | std::views::take_while([&](std::size_t i) {
                      return i < kBmpCount;
                  })) {
        if (!stream.readString(bmpName[i])) {
            *this = backup;
            return false;
        }
        if (!bmpName[i].empty() && static_cast<Int32>(i) == numBmps) {
            ++numBmps;
        }
    }
    if (!stream.isValid()) {
        *this = backup;
        return false;
    }
    return true;
}

// -----------------------------------------------------------------------------
// ShapeBaseData
// -----------------------------------------------------------------------------

ShapeBaseData::ShapeBaseData() = default;

bool ShapeBaseData::preload(ResourceManager* rm, bool server, std::string& errorBuffer) {
    if (rm) {
        if (!shieldShapeName.empty()) {
            const auto name = ensureDtsExtension(shieldShapeName);
            shieldShape = rm->load<world::TsShape>(name);
            if (!shieldShape && rm->contains(name)) {
                // Allow raw CRC-only test resources when TsShape loader isn't needed,
                // or report missing when neither present nor loadable.
            } else if (!shieldShape && !rm->contains(name)) {
                errorBuffer = "Unable to model: " + name +
                              ".  The server has new art data you are missing.  Check server info "
                              "for possible download locations.";
                return false;
            }
        }
        if (!fileName.empty()) {
            const auto name = ensureDtsExtension(fileName);
            shape = rm->load<world::TsShape>(name);
            if (!shape && !rm->contains(name)) {
                errorBuffer = "Unable to model: " + name +
                              ".  The server has new art data you are missing.  Check server info "
                              "for possible download locations.";
                return false;
            }
            if (validateShape) {
                const auto crc = getShapeCRC(*rm, name, validateMaterials, shape.get());
                if (!crc) {
                    errorBuffer = "Unable to model: " + name +
                                  ".  The server has new art data you are missing.  Check server "
                                  "info for possible download locations.";
                    return false;
                }
                if (server) {
                    shapeCRC = *crc;
                } else if (*crc != shapeCRC) {
                    errorBuffer =
                        std::format("Client side model {} or its textures do not match the "
                                    "server's and the server requires validation of this model or "
                                    "textures (CRC {:08x}, expected {:08x}).",
                                    name,
                                    *crc,
                                    shapeCRC);
                    return false;
                }
            }
        }
    }
    return GameBaseData::preload(rm, server, errorBuffer);
}

void ShapeBaseData::pack(BitStream& stream, DataBlockManager* dbm) const {
    GameBaseData::pack(stream, dbm);
    writeValidatedString(stream, fileName, validateShape, shapeCRC, validateMaterials);
    stream.writeString(shieldShapeName);
    writeBlockIdHelper(stream, dbm, DataBlockManager::SoundDataType, sfxShield);
    stream.writeInt(shadowDetailMask, 8);
    writeBlockIdHelper(stream, dbm, DataBlockManager::ExplosionDataType, explosionId);
    writeBlockIdHelper(stream, dbm, DataBlockManager::DamageSkinDataType, damageSkinId);
    stream.writeInt(debrisId, 32);
    writePod(stream, maxEnergy);

    for (auto j : std::views::iota(std::size_t{0}) | std::views::take_while([&](std::size_t j) {
                      return j < kMaxSequenceIndex;
                  })) {
        const auto& as = sequenceSound[j];
        if (stream.writeFlag(!as.sequence.empty())) {
            stream.writeString(as.sequence);
            writeBlockIdHelper(stream, dbm, DataBlockManager::SoundDataType, as.sound);
        }
    }
    stream.writeFlag(isPerspective);
}

bool ShapeBaseData::unpack(BitStream& stream, DataBlockManager* dbm) {
    BitStreamRollbackGuard streamGuard(stream);
    const auto backup = *this;
    if (!GameBaseData::unpack(stream, dbm)) {
        *this = backup;
        return false;
    }
    if (!readValidatedString(stream, fileName, validateShape, shapeCRC, validateMaterials)) {
        *this = backup;
        return false;
    }
    if (!stream.readString(shieldShapeName)) {
        *this = backup;
        return false;
    }
    sfxShield = readBlockIdHelper(stream, dbm, DataBlockManager::SoundDataType);
    shadowDetailMask = stream.readInt(8);
    explosionId = readBlockIdHelper(stream, dbm, DataBlockManager::ExplosionDataType);
    damageSkinId = readBlockIdHelper(stream, dbm, DataBlockManager::DamageSkinDataType);
    debrisId = stream.readInt(32);
    if (!readPod(stream, maxEnergy)) {
        *this = backup;
        return false;
    }
    for (auto j : std::views::iota(std::size_t{0}) | std::views::take_while([&](std::size_t j) {
                      return j < kMaxSequenceIndex;
                  })) {
        if (stream.readFlag()) {
            if (!stream.readString(sequenceSound[j].sequence)) {
                *this = backup;
                return false;
            }
            sequenceSound[j].sound =
                readBlockIdHelper(stream, dbm, DataBlockManager::SoundDataType);
        } else {
            sequenceSound[j].sequence.clear();
            sequenceSound[j].sound = -1;
        }
    }
    isPerspective = stream.readFlag();
    if (!stream.isValid()) {
        *this = backup;
        return false;
    }
    streamGuard.committed = true;
    return true;
}

// -----------------------------------------------------------------------------
// StaticBaseData
// -----------------------------------------------------------------------------

StaticBaseData::StaticBaseData() = default;

void StaticBaseData::pack(BitStream& stream, DataBlockManager* dbm) const {
    ShapeBaseData::pack(stream, dbm);
    writeBlockIdHelper(stream, dbm, DataBlockManager::SoundDataType, sfxAmbient);
    stream.writeFlag(isTranslucent);
    stream.writeFlag(disableCollision);
}

bool StaticBaseData::unpack(BitStream& stream, DataBlockManager* dbm) {
    BitStreamRollbackGuard streamGuard(stream);
    const auto backup = *this;
    if (!ShapeBaseData::unpack(stream, dbm)) {
        *this = backup;
        return false;
    }
    sfxAmbient = readBlockIdHelper(stream, dbm, DataBlockManager::SoundDataType);
    isTranslucent = stream.readFlag();
    disableCollision = stream.readFlag();
    if (!stream.isValid()) {
        *this = backup;
        return false;
    }
    streamGuard.committed = true;
    return true;
}

// -----------------------------------------------------------------------------
// PlayerData
// -----------------------------------------------------------------------------

PlayerData::PlayerData() {
    rFootSounds.fill(-1);
    lFootSounds.fill(-1);
}

void PlayerData::pack(BitStream& stream, DataBlockManager* dbm) const {
    ShapeBaseData::pack(stream, dbm);
    stream.writeString(flameShapeName);

    for (auto i : std::views::iota(std::size_t{0}) | std::views::take_while([&](std::size_t i) {
                      return i < kNumAnims;
                  })) {
        stream.writeString(animData[i].name);
        if (stream.writeFlag(animData[i].soundTag != -1)) {
            writeBlockIdHelper(stream, dbm, DataBlockManager::SoundDataType, animData[i].soundTag);
        }
        stream.writeFlag(animData[i].direction > 0);
        stream.writeInt(animData[i].viewFlags, 4);
        stream.writeInt(animData[i].priority, 4);
    }

    writePod(stream, minDamageSpeed);
    writePod(stream, jetEnergyDrain);
    writeLegacyFloat(stream, maxForwardSpeed / 64.0f, 10);
    writeLegacyFloat(stream, maxBackwardSpeed / 64.0f, 10);
    writeLegacyFloat(stream, maxSideSpeed / 64.0f, 10);
    stream.writeFlag(canCrouch);

    writeLegacyFloat(stream, maxJetSideForceFactor, 8);
    writePod(stream, groundForce);
    writePod(stream, groundTraction);
    writePod(stream, maxJetForwardVelocity);
    writeLegacyFloat(stream, jumpSurfaceMinDot, 8);
    writePod(stream, jumpImpulse);
    writePod(stream, mass);
    writePod(stream, drag);
    writePod(stream, density);
    writePod(stream, jetForce);
    writeBlockIdHelper(stream, dbm, DataBlockManager::SoundDataType, jetSound);
    for (auto i : std::views::iota(std::size_t{0}) | std::views::take_while([&](std::size_t i) {
                      return i < kNumFootSounds;
                  })) {
        writeBlockIdHelper(stream, dbm, DataBlockManager::SoundDataType, rFootSounds[i]);
        writeBlockIdHelper(stream, dbm, DataBlockManager::SoundDataType, lFootSounds[i]);
    }
    writePod(stream, footPrints[0]);
    writePod(stream, footPrints[1]);

    writePod(stream, boxWidth);
    writePod(stream, boxDepth);
    writePod(stream, boxNormalHeight);
    writePod(stream, boxCrouchHeight);
}

bool PlayerData::unpack(BitStream& stream, DataBlockManager* dbm) {
    const auto backup = *this;
    if (!ShapeBaseData::unpack(stream, dbm) || !stream.readString(flameShapeName)) {
        *this = backup;
        return false;
    }
    for (auto i : std::views::iota(std::size_t{0}) | std::views::take_while([&](std::size_t i) {
                      return i < kNumAnims;
                  })) {
        if (!stream.readString(animData[i].name)) {
            *this = backup;
            return false;
        }
        if (stream.readFlag()) {
            animData[i].soundTag = readBlockIdHelper(stream, dbm, DataBlockManager::SoundDataType);
        } else {
            animData[i].soundTag = -1;
        }
        animData[i].direction = stream.readFlag() ? 1 : -1;
        animData[i].viewFlags = stream.readInt(4);
        animData[i].priority = stream.readInt(4);
    }
    if (!readPod(stream, minDamageSpeed) || !readPod(stream, jetEnergyDrain)) {
        *this = backup;
        return false;
    }
    maxForwardSpeed = stream.readFloat(10) * 64.0f;
    maxBackwardSpeed = stream.readFloat(10) * 64.0f;
    maxSideSpeed = stream.readFloat(10) * 64.0f;
    canCrouch = stream.readFlag();

    maxJetSideForceFactor = stream.readFloat(8);
    if (!readPod(stream, groundForce) || !readPod(stream, groundTraction) ||
        !readPod(stream, maxJetForwardVelocity)) {
        *this = backup;
        return false;
    }
    jumpSurfaceMinDot = stream.readFloat(8);
    if (!readPod(stream, jumpImpulse) || !readPod(stream, mass) || !readPod(stream, drag) ||
        !readPod(stream, density) || !readPod(stream, jetForce)) {
        *this = backup;
        return false;
    }
    jetSound = readBlockIdHelper(stream, dbm, DataBlockManager::SoundDataType);
    for (auto i : std::views::iota(std::size_t{0}) | std::views::take_while([&](std::size_t i) {
                      return i < kNumFootSounds;
                  })) {
        rFootSounds[i] = readBlockIdHelper(stream, dbm, DataBlockManager::SoundDataType);
        lFootSounds[i] = readBlockIdHelper(stream, dbm, DataBlockManager::SoundDataType);
    }
    if (!readPod(stream, footPrints[0]) || !readPod(stream, footPrints[1]) ||
        !readPod(stream, boxWidth) || !readPod(stream, boxDepth) ||
        !readPod(stream, boxNormalHeight) || !readPod(stream, boxCrouchHeight) ||
        !stream.isValid()) {
        *this = backup;
        return false;
    }
    return true;
}

// -----------------------------------------------------------------------------
// ItemImageData
// -----------------------------------------------------------------------------

ItemImageData::ItemImageData() = default;

bool ItemImageData::preload(ResourceManager* rm, bool /*server*/, std::string& errorBuffer) {
    if (rm && !shapeFile.empty()) {
        const auto name = ensureDtsExtension(shapeFile);
        shape = rm->load<world::TsShape>(name);
        if (!shape && !rm->contains(name)) {
            errorBuffer = "Unable to load ItemImageData shape: " + name;
            return false;
        }
    }
    return true;
}

void ItemImageData::pack(BitStream& stream, DataBlockManager* dbm) const {
    GameBaseData::pack(stream, dbm);
    stream.writeString(shapeFile);
    writePod(stream, projectile.type);
    writePod(stream, projectile.dataType);
    writePod(stream, mountPoint);
    writePoint3F(stream, mountOffset);
    writePoint3F(stream, mountRotation);
    stream.writeFlag(firstPerson);

    writePod(stream, weaponType);
    writePod(stream, ammoType);
    writePod(stream, activateTime);
    writePod(stream, fireTime);
    writePod(stream, reloadTime);
    writePod(stream, spinUpTime);
    writePod(stream, spinDownTime);
    writePod(stream, mass);

    writeBlockIdHelper(stream, dbm, DataBlockManager::SoundDataType, sfxActivateTag);
    writeBlockIdHelper(stream, dbm, DataBlockManager::SoundDataType, sfxFireTag);
    writeBlockIdHelper(stream, dbm, DataBlockManager::SoundDataType, sfxReadyTag);
    writeBlockIdHelper(stream, dbm, DataBlockManager::SoundDataType, sfxReloadTag);
    writeBlockIdHelper(stream, dbm, DataBlockManager::SoundDataType, sfxSpinUpTag);
    writeBlockIdHelper(stream, dbm, DataBlockManager::SoundDataType, sfxSpinDownTag);

    writePod(stream, lightType);
    writePod(stream, lightRadius);
    writePod(stream, lightTime);
    writePoint3F(stream, lightColor);

    writePod(stream, minEnergy);
    writePod(stream, maxEnergy);
}

bool ItemImageData::unpack(BitStream& stream, DataBlockManager* dbm) {
    const auto backup = *this;
    if (!GameBaseData::unpack(stream, dbm) || !stream.readString(shapeFile) ||
        !readPod(stream, projectile.type) || !readPod(stream, projectile.dataType) ||
        !readPod(stream, mountPoint) || !readPoint3F(stream, mountOffset) ||
        !readPoint3F(stream, mountRotation)) {
        *this = backup;
        return false;
    }
    firstPerson = stream.readFlag();
    if (!readPod(stream, weaponType) || !readPod(stream, ammoType) ||
        !readPod(stream, activateTime) || !readPod(stream, fireTime) ||
        !readPod(stream, reloadTime) || !readPod(stream, spinUpTime) ||
        !readPod(stream, spinDownTime) || !readPod(stream, mass)) {
        *this = backup;
        return false;
    }
    sfxActivateTag = readBlockIdHelper(stream, dbm, DataBlockManager::SoundDataType);
    sfxFireTag = readBlockIdHelper(stream, dbm, DataBlockManager::SoundDataType);
    sfxReadyTag = readBlockIdHelper(stream, dbm, DataBlockManager::SoundDataType);
    sfxReloadTag = readBlockIdHelper(stream, dbm, DataBlockManager::SoundDataType);
    sfxSpinUpTag = readBlockIdHelper(stream, dbm, DataBlockManager::SoundDataType);
    sfxSpinDownTag = readBlockIdHelper(stream, dbm, DataBlockManager::SoundDataType);

    if (!readPod(stream, lightType) || !readPod(stream, lightRadius) ||
        !readPod(stream, lightTime) || !readPoint3F(stream, lightColor) ||
        !readPod(stream, minEnergy) || !readPod(stream, maxEnergy) || !stream.isValid()) {
        *this = backup;
        return false;
    }
    return true;
}

// -----------------------------------------------------------------------------
// ItemData
// -----------------------------------------------------------------------------

ItemData::ItemData() {
    description = "Item";
}

bool ItemData::preload(ResourceManager* rm, bool server, std::string& errorBuffer) {
    return StaticBaseData::preload(rm, server, errorBuffer);
}

void ItemData::pack(BitStream& stream, DataBlockManager* dbm) const {
    // The 1.11 ItemData wire layout extends ShapeBaseData directly. The
    // StaticBase-only sound/collision fields shift every following field.
    ShapeBaseData::pack(stream, dbm);
    writePod(stream, imageId);
    writePod(stream, price);
    writePod(stream, mass);
    writePod(stream, drag);
    writePod(stream, density);
    writePod(stream, friction);
    writePod(stream, elasticity);
    stream.writeString(hudIcon);
    stream.writeFlag(showInventory);
    stream.writeFlag(showWeaponBar);
    stream.writeFlag(hiliteOnActive);
    stream.writeString(typeString);
    writePod(stream, lightType);
    writePod(stream, lightRadius);
    writePod(stream, lightTime);
    writePoint3F(stream, lightColor);
}

bool ItemData::unpack(BitStream& stream, DataBlockManager* dbm) {
    BitStreamRollbackGuard streamGuard(stream);
    const auto backup = *this;
    if (!ShapeBaseData::unpack(stream, dbm) || !readPod(stream, imageId) ||
        !readPod(stream, price) || !readPod(stream, mass) || !readPod(stream, drag) ||
        !readPod(stream, density) || !readPod(stream, friction) || !readPod(stream, elasticity) ||
        !stream.readString(hudIcon)) {
        *this = backup;
        return false;
    }
    showInventory = stream.readFlag();
    showWeaponBar = stream.readFlag();
    hiliteOnActive = stream.readFlag();
    if (!stream.readString(typeString) || !readPod(stream, lightType) ||
        !readPod(stream, lightRadius) || !readPod(stream, lightTime) ||
        !readPoint3F(stream, lightColor) || !stream.isValid()) {
        *this = backup;
        return false;
    }
    streamGuard.committed = true;
    return true;
}

// -----------------------------------------------------------------------------
// MoveableData
// -----------------------------------------------------------------------------

MoveableData::MoveableData() = default;

void MoveableData::pack(BitStream& stream, DataBlockManager* dbm) const {
    StaticBaseData::pack(stream, dbm);
    stream.writeFlag(displace);
    stream.writeFlag(blockForward);
    stream.writeFlag(blockBackward);
    writeBlockIdHelper(stream, dbm, DataBlockManager::SoundDataType, sfxStart);
    writeBlockIdHelper(stream, dbm, DataBlockManager::SoundDataType, sfxStop);
    writeBlockIdHelper(stream, dbm, DataBlockManager::SoundDataType, sfxRun);
    writeBlockIdHelper(stream, dbm, DataBlockManager::SoundDataType, sfxBlocked);
    writePod(stream, speed);
}

bool MoveableData::unpack(BitStream& stream, DataBlockManager* dbm) {
    BitStreamRollbackGuard streamGuard(stream);
    const auto backup = *this;
    if (!StaticBaseData::unpack(stream, dbm)) {
        *this = backup;
        return false;
    }
    displace = stream.readFlag();
    blockForward = stream.readFlag();
    blockBackward = stream.readFlag();
    sfxStart = readBlockIdHelper(stream, dbm, DataBlockManager::SoundDataType);
    sfxStop = readBlockIdHelper(stream, dbm, DataBlockManager::SoundDataType);
    sfxRun = readBlockIdHelper(stream, dbm, DataBlockManager::SoundDataType);
    sfxBlocked = readBlockIdHelper(stream, dbm, DataBlockManager::SoundDataType);
    if (!readPod(stream, speed) || !stream.isValid()) {
        *this = backup;
        return false;
    }
    streamGuard.committed = true;
    return true;
}

// -----------------------------------------------------------------------------
// SensorData
// -----------------------------------------------------------------------------

SensorData::SensorData() = default;

void SensorData::pack(BitStream& stream, DataBlockManager* dbm) const {
    StaticBaseData::pack(stream, dbm);
    writePod(stream, iRange);
    writePod(stream, iDopplerVelocity);
    stream.writeFlag(supression);
    stream.writeFlag(pinger);
}

bool SensorData::unpack(BitStream& stream, DataBlockManager* dbm) {
    BitStreamRollbackGuard streamGuard(stream);
    const auto backup = *this;
    if (!StaticBaseData::unpack(stream, dbm) || !readPod(stream, iRange) ||
        !readPod(stream, iDopplerVelocity)) {
        *this = backup;
        return false;
    }
    supression = stream.readFlag();
    pinger = stream.readFlag();
    if (!stream.isValid()) {
        *this = backup;
        return false;
    }
    streamGuard.committed = true;
    return true;
}

// -----------------------------------------------------------------------------
// VehicleData, FlierData, TankData, CarData
// -----------------------------------------------------------------------------

VehicleData::VehicleData() = default;

void VehicleData::pack(BitStream& stream, DataBlockManager* dbm) const {
    StaticBaseData::pack(stream, dbm);
    writePod(stream, mass);
    writePod(stream, drag);
    writePod(stream, density);
    writePod(stream, maxSpeed);
    writePod(stream, minSpeed);
    writePod(stream, visibleDriver);
    writePod(stream, driverPose);
    writeBlockIdHelper(stream, dbm, DataBlockManager::SoundDataType, fireSound);
    writeBlockIdHelper(stream, dbm, DataBlockManager::SoundDataType, damageSound);
    writeBlockIdHelper(stream, dbm, DataBlockManager::SoundDataType, mountSound);
    writeBlockIdHelper(stream, dbm, DataBlockManager::SoundDataType, dismountSound);
    writeBlockIdHelper(stream, dbm, DataBlockManager::SoundDataType, idleSound);
    writeBlockIdHelper(stream, dbm, DataBlockManager::SoundDataType, moveSound);
    for (auto i : std::views::iota(std::size_t{0}, std::size_t{4})) {
        writePoint3F(stream, suspension[i]);
    }
}

bool VehicleData::unpack(BitStream& stream, DataBlockManager* dbm) {
    BitStreamRollbackGuard streamGuard(stream);
    const auto backup = *this;
    if (!StaticBaseData::unpack(stream, dbm) || !readPod(stream, mass) || !readPod(stream, drag) ||
        !readPod(stream, density) || !readPod(stream, maxSpeed) || !readPod(stream, minSpeed) ||
        !readPod(stream, visibleDriver) || !readPod(stream, driverPose)) {
        *this = backup;
        return false;
    }
    fireSound = readBlockIdHelper(stream, dbm, DataBlockManager::SoundDataType);
    damageSound = readBlockIdHelper(stream, dbm, DataBlockManager::SoundDataType);
    mountSound = readBlockIdHelper(stream, dbm, DataBlockManager::SoundDataType);
    dismountSound = readBlockIdHelper(stream, dbm, DataBlockManager::SoundDataType);
    idleSound = readBlockIdHelper(stream, dbm, DataBlockManager::SoundDataType);
    moveSound = readBlockIdHelper(stream, dbm, DataBlockManager::SoundDataType);
    for (auto i : std::views::iota(std::size_t{0}, std::size_t{4})) {
        if (!readPoint3F(stream, suspension[i])) {
            *this = backup;
            return false;
        }
    }
    if (!stream.isValid()) {
        *this = backup;
        return false;
    }
    streamGuard.committed = true;
    return true;
}

void FlierData::pack(BitStream& stream, DataBlockManager* dbm) const {
    VehicleData::pack(stream, dbm);
    writePod(stream, maxBank);
    writePod(stream, maxPitch);
    writePod(stream, lift);
    writePod(stream, maxAlt);
    writePod(stream, maxVertical);
    writePod(stream, accel);
    writePod(stream, groundDamageScale);
}

bool FlierData::unpack(BitStream& stream, DataBlockManager* dbm) {
    BitStreamRollbackGuard streamGuard(stream);
    const auto backup = *this;
    if (!VehicleData::unpack(stream, dbm) || !readPod(stream, maxBank) ||
        !readPod(stream, maxPitch) || !readPod(stream, lift) || !readPod(stream, maxAlt) ||
        !readPod(stream, maxVertical) || !readPod(stream, accel) ||
        !readPod(stream, groundDamageScale) || !stream.isValid()) {
        *this = backup;
        return false;
    }
    streamGuard.committed = true;
    return true;
}

void TankData::pack(BitStream& stream, DataBlockManager* dbm) const {
    VehicleData::pack(stream, dbm);
    writePod(stream, maxGrade);
}

bool TankData::unpack(BitStream& stream, DataBlockManager* dbm) {
    BitStreamRollbackGuard streamGuard(stream);
    const auto backup = *this;
    if (!VehicleData::unpack(stream, dbm) || !readPod(stream, maxGrade) || !stream.isValid()) {
        *this = backup;
        return false;
    }
    streamGuard.committed = true;
    return true;
}

CarData::CarData() {
    wheels[0].steerForward = true;
    wheels[1].steerForward = true;
    wheels[4].steerReverse = true;
    wheels[5].steerReverse = true;
    wheels[0].wheelBase = Point3F{-3.0f, 3.0f, 1.5f};
    wheels[1].wheelBase = Point3F{3.0f, 3.0f, 1.5f};
    wheels[2].wheelBase = Point3F{-3.0f, -3.0f, 1.5f};
    wheels[3].wheelBase = Point3F{3.0f, -3.0f, 1.5f};
    wheels[4].wheelBase = Point3F{-3.0f, -3.0f, 0.0f};
    wheels[5].wheelBase = Point3F{3.0f, -3.0f, 0.0f};
}

void CarData::pack(BitStream& stream, DataBlockManager* dbm) const {
    VehicleData::pack(stream, dbm);
}

bool CarData::unpack(BitStream& stream, DataBlockManager* dbm) {
    return VehicleData::unpack(stream, dbm);
}

// -----------------------------------------------------------------------------
// ProjectileData and subclasses
// -----------------------------------------------------------------------------

ProjectileData::ProjectileData() = default;

bool ProjectileData::preload(ResourceManager* rm, bool server, std::string& errorBuffer) {
    if (rm && !bulletShapeName.empty()) {
        shape = rm->load<world::TsShape>(bulletShapeName);
        if (!shape && !rm->contains(bulletShapeName)) {
            errorBuffer = "Unable to load ProjectileData shape: " + bulletShapeName;
            return false;
        }
        if (validateShape) {
            const auto crc = getShapeCRC(*rm, bulletShapeName, validateMaterials, shape.get());
            if (!crc) {
                errorBuffer = "Unable to load ProjectileData shape: " + bulletShapeName;
                return false;
            }
            if (server) {
                shapeCRC = *crc;
            } else if (*crc != shapeCRC) {
                errorBuffer = "Client side model " + bulletShapeName +
                              " or its textures do not match the server's and the server requires "
                              "validation of this model or textures.";
                return false;
            }
        }
    }
    return true;
}

void ProjectileData::pack(BitStream& stream, DataBlockManager* dbm) const {
    GameBaseData::pack(stream, dbm);
    writeValidatedString(stream, bulletShapeName, validateShape, shapeCRC, validateMaterials);
    writePod(stream, explosionTag);
    writePod(stream, expRandCycle);
    stream.writeFlag(collideWithOwner);
    writePod(stream, ownerGraceMS);
    writePod(stream, collisionRadius);
    writePod(stream, mass);
    writePod(stream, muzzleVelocity);
    writePod(stream, terminalVelocity);
    writePod(stream, acceleration);
    writePod(stream, totalTime);
    writePod(stream, liveTime);
    writePod(stream, projSpecialTime);
    stream.writeFlag(projSpecialBool);
    writePod(stream, lightRange);
    writeColorF(stream, lightColor);
    writePod(stream, soundId);
    writePod(stream, explosionRadius);
}

bool ProjectileData::unpack(BitStream& stream, DataBlockManager* dbm) {
    BitStreamRollbackGuard streamGuard(stream);
    const auto backup = *this;
    if (!GameBaseData::unpack(stream, dbm) ||
        !readValidatedString(stream, bulletShapeName, validateShape, shapeCRC, validateMaterials) ||
        !readPod(stream, explosionTag) || !readPod(stream, expRandCycle)) {
        *this = backup;
        return false;
    }
    collideWithOwner = stream.readFlag();
    if (!readPod(stream, ownerGraceMS) || !readPod(stream, collisionRadius) ||
        !readPod(stream, mass) || !readPod(stream, muzzleVelocity) ||
        !readPod(stream, terminalVelocity) || !readPod(stream, acceleration) ||
        !readPod(stream, totalTime) || !readPod(stream, liveTime) ||
        !readPod(stream, projSpecialTime)) {
        *this = backup;
        return false;
    }
    projSpecialBool = stream.readFlag();
    if (!readPod(stream, lightRange) || !readColorF(stream, lightColor) ||
        !readPod(stream, soundId) || !readPod(stream, explosionRadius) || !stream.isValid()) {
        *this = backup;
        return false;
    }
    streamGuard.committed = true;
    return true;
}

Flt32 ProjectileData::getTerminalVelocity() const noexcept {
    return muzzleVelocity;
}

bool ProjectileData::getTargetVector(const Point3F& rShootFrom,
                                     const Point3F& rShootTo,
                                     Flt32 /*gravComp*/,
                                     Point3F* pVector,
                                     bool* pInRange,
                                     bool secondSolution) const noexcept {
    if (!pVector || !pInRange || secondSolution) {
        return false;
    }
    *pVector = rShootTo - rShootFrom;
    if (pVector->len() > 0.0f) {
        pVector->normalize();
    }
    *pInRange = true;
    return true;
}

BulletData::BulletData() = default;

Flt32 BulletData::getTerminalVelocity() const noexcept {
    return muzzleVelocity;
}

bool BulletData::getTargetVector(const Point3F& rShootFrom,
                                 const Point3F& rShootTo,
                                 Flt32 /*gravComp*/,
                                 Point3F* pVector,
                                 bool* pInRange,
                                 bool secondSolution) const noexcept {
    if (!pVector || !pInRange || secondSolution) {
        return false;
    }
    *pVector = rShootTo - rShootFrom;
    const Flt32 dist = pVector->len();
    if (dist > 0.0f) {
        pVector->normalize();
    }
    *pInRange = (dist < getTerminalVelocity() * totalTime);
    return true;
}

void BulletData::pack(BitStream& stream, DataBlockManager* dbm) const {
    ProjectileData::pack(stream, dbm);
    writePod(stream, rotationPeriod);
    writePod(stream, bulletHoleIndex);
    writePod(stream, tracerLength);
}

bool BulletData::unpack(BitStream& stream, DataBlockManager* dbm) {
    BitStreamRollbackGuard streamGuard(stream);
    const auto backup = *this;
    if (!ProjectileData::unpack(stream, dbm) || !readPod(stream, rotationPeriod) ||
        !readPod(stream, bulletHoleIndex) || !readPod(stream, tracerLength) || !stream.isValid()) {
        *this = backup;
        return false;
    }
    streamGuard.committed = true;
    return true;
}

GrenadeData::GrenadeData() = default;

bool GrenadeData::preload(ResourceManager* rm, bool server, std::string& errorBuffer) {
    if (!ProjectileData::preload(rm, server, errorBuffer)) {
        return false;
    }
    if (rm && !server && !pSmokeName.empty()) {
        smokeShape = rm->load<world::TsShape>(pSmokeName);
        if (!smokeShape && !rm->contains(pSmokeName)) {
            errorBuffer = "Unable to load Smoke Shape: " + pSmokeName;
            return false;
        }
    }
    return true;
}

void GrenadeData::pack(BitStream& stream, DataBlockManager* dbm) const {
    ProjectileData::pack(stream, dbm);
    writePod(stream, elasticity);
    stream.writeString(pSmokeName);
}

bool GrenadeData::unpack(BitStream& stream, DataBlockManager* dbm) {
    BitStreamRollbackGuard streamGuard(stream);
    const auto backup = *this;
    if (!ProjectileData::unpack(stream, dbm) || !readPod(stream, elasticity) ||
        !stream.readString(pSmokeName) || !stream.isValid()) {
        *this = backup;
        return false;
    }
    streamGuard.committed = true;
    return true;
}

Flt32 GrenadeData::getTerminalVelocity() const noexcept {
    return muzzleVelocity;
}

bool GrenadeData::getTargetVector(const Point3F& rShootFrom,
                                  const Point3F& rShootTo,
                                  Flt32 gravComp,
                                  Point3F* pVector,
                                  bool* pInRange,
                                  bool secondSolution) const noexcept {
    if (!pVector || !pInRange) {
        return false;
    }
    const Flt32 g = gravComp;
    const Flt32 h = rShootTo.z - rShootFrom.z;
    const Flt32 d_sq = (rShootTo.x - rShootFrom.x) * (rShootTo.x - rShootFrom.x) +
                       (rShootTo.y - rShootFrom.y) * (rShootTo.y - rShootFrom.y);
    if (d_sq <= 0.0f) {
        *pVector = rShootTo - rShootFrom;
        if (pVector->len() > 0.0f) {
            pVector->normalize();
        }
        *pInRange = false;
        return true;
    }

    const Flt32 v0_sq = getTerminalVelocity() * getTerminalVelocity();
    const Flt32 det =
        ((h * g + v0_sq) * (h * g + v0_sq)) - ((1.0f + (h * h) / d_sq) * g * g * d_sq);
    if (det < 0.0f) {
        *pVector = rShootTo - rShootFrom;
        if (pVector->len() > 0.0f) {
            pVector->normalize();
        }
        *pInRange = false;
        return true;
    }
    if (det < 1.0f && secondSolution) {
        return false;
    }

    const Flt32 factor = secondSolution ? -1.0f : 1.0f;
    Flt32 v0x = ((h * g + v0_sq) + (factor * std::sqrt(det))) / (2.0f * (1.0f + ((h * h) / d_sq)));
    if (v0x < 0.0f) {
        *pVector = rShootTo - rShootFrom;
        if (pVector->len() > 0.0f) {
            pVector->normalize();
        }
        *pInRange = false;
        return true;
    }

    Flt32 v0y = std::sqrt(std::max(0.0f, v0_sq - v0x));
    v0x = std::sqrt(v0x);
    if (v0x <= 0.0f) {
        *pVector = rShootTo - rShootFrom;
        if (pVector->len() > 0.0f) {
            pVector->normalize();
        }
        *pInRange = false;
        return true;
    }

    const Flt32 t = std::sqrt(d_sq) / v0x;
    const Flt32 pos = (0.5f * g * t * t) + (t * v0y) - h;
    const Flt32 neg = (0.5f * g * t * t) - (t * v0y) - h;
    if (std::fabs(neg) < std::fabs(pos)) {
        v0y *= -1.0f;
    }

    Point3F planeVector = rShootTo - rShootFrom;
    planeVector.z = 0.0f;
    if (planeVector.len() > 0.0f) {
        planeVector.normalize();
    }

    *pVector = (planeVector * v0x) + (Point3F{0.0f, 0.0f, 1.0f} * v0y);
    if (pVector->len() > 0.0f) {
        pVector->normalize();
    }
    *pInRange = true;
    return true;
}

RocketData::RocketData() = default;

Flt32 RocketData::getTerminalVelocity() const noexcept {
    return terminalVelocity;
}

bool RocketData::getTargetVector(const Point3F& rShootFrom,
                                 const Point3F& rShootTo,
                                 Flt32 /*gravComp*/,
                                 Point3F* pVector,
                                 bool* pInRange,
                                 bool secondSolution) const noexcept {
    if (!pVector || !pInRange || secondSolution) {
        return false;
    }
    *pVector = rShootTo - rShootFrom;
    const Flt32 dist = pVector->len();
    if (dist > 0.0f) {
        pVector->normalize();
    }
    *pInRange = (dist < getTerminalVelocity() * totalTime);
    return true;
}

bool RocketData::preload(ResourceManager* rm, bool server, std::string& errorBuffer) {
    if (!ProjectileData::preload(rm, server, errorBuffer)) {
        return false;
    }
    if (rm && !server && trailType == 2 && !trailString.empty()) {
        smokeShape = rm->load<world::TsShape>(trailString);
        if (!smokeShape && !rm->contains(trailString)) {
            errorBuffer = "Unable to load Smoke Shape: " + trailString;
            return false;
        }
    }
    return true;
}

void RocketData::pack(BitStream& stream, DataBlockManager* dbm) const {
    ProjectileData::pack(stream, dbm);
    writePod(stream, trailType);
    stream.writeString(trailString);
    writePod(stream, trailLength);
    writePod(stream, trailWidth);
}

bool RocketData::unpack(BitStream& stream, DataBlockManager* dbm) {
    BitStreamRollbackGuard streamGuard(stream);
    const auto backup = *this;
    if (!ProjectileData::unpack(stream, dbm) || !readPod(stream, trailType) ||
        !stream.readString(trailString) || !readPod(stream, trailLength) ||
        !readPod(stream, trailWidth) || !stream.isValid()) {
        *this = backup;
        return false;
    }
    streamGuard.committed = true;
    return true;
}

LaserData::LaserData() = default;

bool LaserData::preload(ResourceManager* rm, bool server, std::string& errorBuffer) {
    if (!ProjectileData::preload(rm, server, errorBuffer)) {
        return false;
    }
    if (rm && !server && !pHitShapeName.empty()) {
        hitShape = rm->load<world::TsShape>(pHitShapeName);
    }
    return true;
}

void LaserData::pack(BitStream& stream, DataBlockManager* dbm) const {
    ProjectileData::pack(stream, dbm);
    stream.writeString(pBitmapName);
    stream.writeString(pHitShapeName);
    stream.writeFlag(detachFromShooter);
}

bool LaserData::unpack(BitStream& stream, DataBlockManager* dbm) {
    BitStreamRollbackGuard streamGuard(stream);
    const auto backup = *this;
    if (!ProjectileData::unpack(stream, dbm) || !stream.readString(pBitmapName) ||
        !stream.readString(pHitShapeName)) {
        *this = backup;
        return false;
    }
    detachFromShooter = stream.readFlag();
    if (!stream.isValid()) {
        *this = backup;
        return false;
    }
    streamGuard.committed = true;
    return true;
}

Flt32 LaserData::getTerminalVelocity() const noexcept {
    return 10000000.0f;
}

bool LaserData::getTargetVector(const Point3F& rShootFrom,
                                const Point3F& rShootTo,
                                Flt32 gravComp,
                                Point3F* pVector,
                                bool* pInRange,
                                bool secondSolution) const noexcept {
    return ProjectileData::getTargetVector(
        rShootFrom, rShootTo, gravComp, pVector, pInRange, secondSolution);
}

SeekingMissileData::SeekingMissileData() = default;

bool SeekingMissileData::preload(ResourceManager* rm, bool server, std::string& errorBuffer) {
    if (!ProjectileData::preload(rm, server, errorBuffer))
        return false;
    if (rm && !server) {
        smokeShape = rm->load<world::TsShape>("rSmoke.dts");
        if (!smokeShape) {
            errorBuffer = "Unable to load SeekingMissileData smoke: rSmoke.dts";
            return false;
        }
    }
    return true;
}

void SeekingMissileData::pack(BitStream& stream, DataBlockManager* dbm) const {
    ProjectileData::pack(stream, dbm);
    writePod(stream, projSpecialTime);
    writePod(stream, seekingTurningRadius);
    writePod(stream, nonSeekingTurningRadius);
}

bool SeekingMissileData::unpack(BitStream& stream, DataBlockManager* dbm) {
    BitStreamRollbackGuard streamGuard(stream);
    const auto backup = *this;
    if (!ProjectileData::unpack(stream, dbm) || !readPod(stream, projSpecialTime) ||
        !readPod(stream, seekingTurningRadius) || !readPod(stream, nonSeekingTurningRadius) ||
        !stream.isValid()) {
        *this = backup;
        return false;
    }
    streamGuard.committed = true;
    return true;
}

LightningData::LightningData() = default;

void LightningData::pack(BitStream& stream, DataBlockManager* dbm) const {
    ProjectileData::pack(stream, dbm);
    writePod(stream, segmentDivisions);
    writePod(stream, numSegments);
    writePod(stream, updateTime);
    writePod(stream, skipPercent);
    writePod(stream, displaceBias);
    writePod(stream, beamWidth);
    stream.writeString(pBitmapName);
}

bool LightningData::unpack(BitStream& stream, DataBlockManager* dbm) {
    BitStreamRollbackGuard streamGuard(stream);
    const auto backup = *this;
    if (!ProjectileData::unpack(stream, dbm) || !readPod(stream, segmentDivisions) ||
        !readPod(stream, numSegments) || !readPod(stream, updateTime) ||
        !readPod(stream, skipPercent) || !readPod(stream, displaceBias) ||
        !readPod(stream, beamWidth) || !stream.readString(pBitmapName) || !stream.isValid()) {
        *this = backup;
        return false;
    }
    streamGuard.committed = true;
    return true;
}

Flt32 LightningData::getTerminalVelocity() const noexcept {
    return 10000000.0f;
}

bool LightningData::getTargetVector(const Point3F& rShootFrom,
                                    const Point3F& rShootTo,
                                    Flt32 gravComp,
                                    Point3F* pVector,
                                    bool* pInRange,
                                    bool secondSolution) const noexcept {
    return ProjectileData::getTargetVector(
        rShootFrom, rShootTo, gravComp, pVector, pInRange, secondSolution);
}

RepairEffectData::RepairEffectData() {
    numSegments = 2;
}

// -----------------------------------------------------------------------------
// TurretData, ExplosionData, MarkerData, DebrisData, MineData, TriggerData
// -----------------------------------------------------------------------------

TurretData::TurretData() = default;

void TurretData::pack(BitStream& stream, DataBlockManager* dbm) const {
    SensorData::pack(stream, dbm);
    writePod(stream, projectile.type);
    writePod(stream, projectile.dataType);
    writePod(stream, gunRange);
    writePod(stream, speed);
    writePod(stream, speedModifier);
    writePod(stream, FOV);
    writeBlockIdHelper(stream, dbm, DataBlockManager::SoundDataType, fireSound);
    writeBlockIdHelper(stream, dbm, DataBlockManager::SoundDataType, activationSound);
    writeBlockIdHelper(stream, dbm, DataBlockManager::SoundDataType, deactivateSound);
    writeBlockIdHelper(stream, dbm, DataBlockManager::SoundDataType, whirSound);
    stream.writeFlag(isSustained);
}

bool TurretData::unpack(BitStream& stream, DataBlockManager* dbm) {
    BitStreamRollbackGuard streamGuard(stream);
    const auto backup = *this;
    if (!SensorData::unpack(stream, dbm) || !readPod(stream, projectile.type) ||
        !readPod(stream, projectile.dataType) || !readPod(stream, gunRange) ||
        !readPod(stream, speed) || !readPod(stream, speedModifier) || !readPod(stream, FOV)) {
        *this = backup;
        return false;
    }
    fireSound = readBlockIdHelper(stream, dbm, DataBlockManager::SoundDataType);
    activationSound = readBlockIdHelper(stream, dbm, DataBlockManager::SoundDataType);
    deactivateSound = readBlockIdHelper(stream, dbm, DataBlockManager::SoundDataType);
    whirSound = readBlockIdHelper(stream, dbm, DataBlockManager::SoundDataType);
    isSustained = stream.readFlag();
    if (!stream.isValid()) {
        *this = backup;
        return false;
    }
    streamGuard.committed = true;
    return true;
}

ExplosionData::ExplosionData() = default;

bool ExplosionData::preload(ResourceManager* rm, bool server, std::string& errorBuffer) {
    if (rm && !pShapeName.empty()) {
        shape = rm->load<world::TsShape>(pShapeName);
        if (!shape && !rm->contains(pShapeName)) {
            errorBuffer = "Unable to load ExplosionData shape: " + pShapeName;
            return false;
        }
        if (validateShape) {
            const auto crc = getShapeCRC(*rm, pShapeName, validateMaterials, shape.get());
            if (!crc) {
                errorBuffer = "Unable to load ExplosionData shape: " + pShapeName;
                return false;
            }
            if (server) {
                shapeCRC = *crc;
            } else if (*crc != shapeCRC) {
                errorBuffer = "Client side model " + pShapeName +
                              " or its textures do not match the server's and the server requires "
                              "validation of this model or textures.";
                return false;
            }
        }
    }
    return true;
}

void ExplosionData::pack(BitStream& stream, DataBlockManager* dbm) const {
    GameBaseData::pack(stream, dbm);
    writeValidatedString(stream, pShapeName, validateShape, shapeCRC, validateMaterials);
    writePod(stream, soundId);
    writePod(stream, timeScale);
    stream.writeFlag(faceCamera);
    stream.writeFlag(randomSpin);
    stream.writeFlag(hasLight);
    if (hasLight) {
        writePod(stream, lightRange);
        writePod(stream, timeZero);
        writePod(stream, timeOne);
        for (auto i : std::views::iota(std::size_t{0}) | std::views::take_while([&](std::size_t i) {
                          return i < kNumLightColors;
                      })) {
            writePod(stream, colors[i].red);
            writePod(stream, colors[i].green);
            writePod(stream, colors[i].blue);
        }
        for (auto i : std::views::iota(std::size_t{0}, std::size_t{3})) {
            writePod(stream, radFactors[i]);
        }
    }
    stream.writeFlag(shiftPosition);
}

bool ExplosionData::unpack(BitStream& stream, DataBlockManager* dbm) {
    BitStreamRollbackGuard streamGuard(stream);
    const auto backup = *this;
    if (!GameBaseData::unpack(stream, dbm) ||
        !readValidatedString(stream, pShapeName, validateShape, shapeCRC, validateMaterials) ||
        !readPod(stream, soundId) || !readPod(stream, timeScale)) {
        *this = backup;
        return false;
    }
    faceCamera = stream.readFlag();
    randomSpin = stream.readFlag();
    hasLight = stream.readFlag();
    if (hasLight) {
        if (!readPod(stream, lightRange) || !readPod(stream, timeZero) ||
            !readPod(stream, timeOne)) {
            *this = backup;
            return false;
        }
        for (auto i : std::views::iota(std::size_t{0}) | std::views::take_while([&](std::size_t i) {
                          return i < kNumLightColors;
                      })) {
            if (!readPod(stream, colors[i].red) || !readPod(stream, colors[i].green) ||
                !readPod(stream, colors[i].blue)) {
                *this = backup;
                return false;
            }
        }
        for (auto i : std::views::iota(std::size_t{0}, std::size_t{3})) {
            if (!readPod(stream, radFactors[i])) {
                *this = backup;
                return false;
            }
        }
    }
    shiftPosition = stream.readFlag();
    if (!stream.isValid()) {
        *this = backup;
        return false;
    }
    streamGuard.committed = true;
    return true;
}

MarkerData::MarkerData() = default;

bool MarkerData::preload(ResourceManager* rm, bool /*server*/, std::string& errorBuffer) {
    if (rm && !fileName.empty()) {
        const auto name = ensureDtsExtension(fileName);
        shape = rm->load<world::TsShape>(name);
        if (!shape && !rm->contains(name)) {
            errorBuffer = "Unable to load MarkerData shape: " + name;
            return false;
        }
    }
    return true;
}

void MarkerData::pack(BitStream& stream, DataBlockManager* dbm) const {
    GameBaseData::pack(stream, dbm);
    stream.writeString(fileName);
}

bool MarkerData::unpack(BitStream& stream, DataBlockManager* dbm) {
    BitStreamRollbackGuard streamGuard(stream);
    const auto backup = *this;
    if (!GameBaseData::unpack(stream, dbm) || !stream.readString(fileName) || !stream.isValid()) {
        *this = backup;
        return false;
    }
    streamGuard.committed = true;
    return true;
}

DebrisData::DebrisData() = default;

bool DebrisData::preload(ResourceManager* rm, bool /*server*/, std::string& errorBuffer) {
    if (rm && !pShapeName.empty()) {
        const auto name = ensureDtsExtension(pShapeName);
        shape = rm->load<world::TsShape>(name);
        if (!shape && !rm->contains(name)) {
            errorBuffer = "Unable to load DebrisData shape: " + name;
            return false;
        }
    }
    return true;
}

void DebrisData::pack(BitStream& stream, DataBlockManager* dbm) const {
    GameBaseData::pack(stream, dbm);
    writePod(stream, type);
    stream.writeString(pShapeName);
    writePod(stream, imageType);
    writePod(stream, imageSortValue);
    writePod(stream, mass);
    writePod(stream, elasticity);
    writePod(stream, friction);
    writePod(stream, center[0]);
    writePod(stream, center[1]);
    writePod(stream, center[2]);
    writePod(stream, collisionMask);
    writePod(stream, knockMask);
    writePod(stream, animationSequence);
    writePod(stream, animRestartTime);
    writePod(stream, minTimeout);
    writePod(stream, maxTimeout);
    writePod(stream, explodeOnBounce);
    writePod(stream, damage);
    writePod(stream, damageThreshold);
    writePod(stream, spawnedDebrisMask);
    writePod(stream, spawnedDebrisStrength);
    writePod(stream, spawnedDebrisRadius);
    writePod(stream, spawnedExplosionID);
    writePod(stream, p);
    stream.writeFlag(explodeOnRest);
    stream.writeInt(collisionDetail, 2);
}

bool DebrisData::unpack(BitStream& stream, DataBlockManager* dbm) {
    BitStreamRollbackGuard streamGuard(stream);
    const auto backup = *this;
    if (!GameBaseData::unpack(stream, dbm) || !readPod(stream, type) ||
        !stream.readString(pShapeName) || !readPod(stream, imageType) ||
        !readPod(stream, imageSortValue) || !readPod(stream, mass) ||
        !readPod(stream, elasticity) || !readPod(stream, friction) || !readPod(stream, center[0]) ||
        !readPod(stream, center[1]) || !readPod(stream, center[2]) ||
        !readPod(stream, collisionMask) || !readPod(stream, knockMask) ||
        !readPod(stream, animationSequence) || !readPod(stream, animRestartTime) ||
        !readPod(stream, minTimeout) || !readPod(stream, maxTimeout) ||
        !readPod(stream, explodeOnBounce) || !readPod(stream, damage) ||
        !readPod(stream, damageThreshold) || !readPod(stream, spawnedDebrisMask) ||
        !readPod(stream, spawnedDebrisStrength) || !readPod(stream, spawnedDebrisRadius) ||
        !readPod(stream, spawnedExplosionID) || !readPod(stream, p)) {
        *this = backup;
        return false;
    }
    explodeOnRest = stream.readFlag();
    collisionDetail = stream.readInt(2);
    if (!stream.isValid()) {
        *this = backup;
        return false;
    }
    streamGuard.committed = true;
    return true;
}

MineData::MineData() = default;

void MineData::pack(BitStream& stream, DataBlockManager* dbm) const {
    ItemData::pack(stream, dbm);
}

bool MineData::unpack(BitStream& stream, DataBlockManager* dbm) {
    return ItemData::unpack(stream, dbm);
}

TriggerData::TriggerData() = default;

// -----------------------------------------------------------------------------
// DataBlockEvent
// -----------------------------------------------------------------------------

bool DataBlockEvent::pack(BitStream& stream, DataBlockManager& dbm) const {
    if (group < 0 || group >= DataBlockManager::NumDataTypes) {
        return false;
    }
    const GameBaseData* payload = data ? data.get() : dbm.lookupDataBlock(block, group);
    const Int32 wireBlock = (block < 0 || !payload) ? 255 : block;
    stream.writeInt(group, 6);
    stream.writeInt(totalBlocks, 8);
    stream.writeInt(wireBlock, 8);
    if (wireBlock != 255 && payload) {
        payload->pack(stream, &dbm);
    }
    return stream.isValid();
}

bool DataBlockEvent::unpack(BitStream& stream, DataBlockManager& dbm) {
    const Int32 inGroup = stream.readInt(6);
    const Int32 inGSize = stream.readInt(8);
    const Int32 inBlock = stream.readInt(8);
    if (!stream.isValid() || inGroup < 0 || inGroup >= DataBlockManager::NumDataTypes ||
        inGSize < 0) {
        return false;
    }
    if (inBlock != 255) {
        if (inBlock < 0 || inBlock >= inGSize) {
            return false;
        }
        auto candidate = DataBlockManager::instantiateBlock(inGroup);
        if (!candidate || !candidate->unpack(stream, &dbm) || !stream.isValid()) {
            return false;
        }
        group = inGroup;
        totalBlocks = inGSize;
        block = inBlock;
        data = std::move(candidate);
    } else {
        group = inGroup;
        totalBlocks = inGSize;
        block = -1;
        data.reset();
    }
    address.objectId = DataBlockManagerId;
    return true;
}

bool registerDataBlockEvent(sim::network::Registry& registry) {
    return registry.registerEvent<DataBlockEvent>(
        DataBlockEventType,
        [](const DataBlockEvent& evt, BitStream& bs, sim::network::EventContext& ctx) {
            auto* dbm = dynamic_cast<DataBlockManager*>(ctx.manager.findObject(DataBlockManagerId));
            DataBlockManager fallback(true);
            return evt.pack(bs, dbm ? *dbm : fallback);
        },
        [](DataBlockEvent& evt, BitStream& bs, sim::network::EventContext& ctx) {
            if (ctx.server) {
                return false;
            }
            auto* dbm = dynamic_cast<DataBlockManager*>(ctx.manager.findObject(DataBlockManagerId));
            DataBlockManager fallback(false);
            const auto start = bs.getCurPos();
            const auto group = bs.readInt(6), size = bs.readInt(8), block = bs.readInt(8);
            if (!bs.isValid() || !bs.setCurPos(start)) {
                return false;
            }
            if (!evt.unpack(bs, dbm ? *dbm : fallback)) {
                ctx.stream.fail(
                    std::format("Datablock group {} block {}/{} decoding failed at bit {}",
                                group,
                                block,
                                size,
                                bs.getCurPos()));
                return false;
            }
            return true;
        },
        sim::network::ReceivePolicy::Client);
}

// -----------------------------------------------------------------------------
// DataBlockManager
// -----------------------------------------------------------------------------

DataBlockManager::DataBlockManager(bool isServer, std::shared_ptr<ResourceManager> resources)
    : isServer_(isServer), resources_(std::move(resources)) {
    setId(DataBlockManagerId);
}

std::string_view DataBlockManager::dataGroupDisplayName(Int32 group) noexcept {
    if (group >= 0 && group < NumDataTypes) {
        return kDataNames[static_cast<std::size_t>(group)];
    }
    return {};
}

ResourceManager* DataBlockManager::resources() const noexcept {
    if (resources_) {
        return resources_.get();
    }
    if (auto* mgr = getManager()) {
        if (auto* sr = sim::SimResource::find(*mgr)) {
            return &sr->get();
        }
    }
    return nullptr;
}

std::unique_ptr<GameBaseData> DataBlockManager::instantiateBlock(Int32 group) {
    switch (group) {
        case MineDataType:
            return std::make_unique<MineData>();
        case TurretDataType:
            return std::make_unique<TurretData>();
        case TankDataType:
            return std::make_unique<TankData>();
        case FlierDataType:
            return std::make_unique<FlierData>();
        case VehicleDataType:
            return std::make_unique<VehicleData>();
        case PlayerDataType:
            return std::make_unique<PlayerData>();
        case StaticShapeDataType:
            return std::make_unique<StaticShapeData>();
        case ItemDataType:
            return std::make_unique<ItemData>();
        case ItemImageDataType:
            return std::make_unique<ItemImageData>();
        case MoveableDataType:
            return std::make_unique<MoveableData>();
        case SensorDataType:
            return std::make_unique<SensorData>();
        case ProjectileDataType:
            return std::make_unique<ProjectileData>();
        case BulletDataType:
            return std::make_unique<BulletData>();
        case GrenadeDataType:
            return std::make_unique<GrenadeData>();
        case LaserDataType:
            return std::make_unique<LaserData>();
        case TargetLaserDataType:
            return std::make_unique<TargetLaserData>();
        case RocketDataType:
            return std::make_unique<RocketData>();
        case SeekingMissileDataType:
            return std::make_unique<SeekingMissileData>();
        case LightningDataType:
            return std::make_unique<LightningData>();
        case RepairEffectDataType:
            return std::make_unique<RepairEffectData>();
        case SoundProfileDataType:
            return std::make_unique<SoundProfileData>();
        case SoundDataType:
            return std::make_unique<SoundData>();
        case ExplosionDataType:
            return std::make_unique<ExplosionData>();
        case DebrisDataType:
            return std::make_unique<DebrisData>();
        case MarkerDataType:
            return std::make_unique<MarkerData>();
        case DamageSkinDataType:
            return std::make_unique<DamageSkinData>();
        case TriggerDataType:
            return std::make_unique<TriggerData>();
        case IRCChannelType:
            return std::make_unique<IRCChannelData>();
        case CarDataType:
            return std::make_unique<CarData>();
        default:
            return nullptr;
    }
}

std::size_t DataBlockManager::hashName(std::string_view name) noexcept {
    UInt32 h = 2166136261U;
    for (char c : name) {
        h ^= static_cast<UInt8>(toLowerAscii(c));
        h *= 16777619U;
    }
    return static_cast<std::size_t>(h & (kHashTableSize - 1));
}

void DataBlockManager::rebuildLookupTable() {
    for (auto& bucket : hashTable_) {
        bucket.clear();
    }
    for (auto g : std::views::iota(Int32{0}) | std::views::take_while([&](Int32 g) {
                      return g < NumDataTypes;
                  })) {
        const auto& grp = dataBlocks_[static_cast<std::size_t>(g)];
        for (auto i : std::views::iota(std::size_t{0}) | std::views::take_while([&](std::size_t i) {
                          return i < grp.size();
                      })) {
            if (grp[i] && !grp[i]->dbmName.empty()) {
                const auto h = hashName(grp[i]->dbmName);
                auto& bucket = hashTable_[h];
                // Prepend to match legacy LIFO hash-chain insertion order
                bucket.insert(bucket.begin(), HashNode{g, static_cast<Int32>(i)});
            }
        }
    }
}

GameBaseData*
DataBlockManager::createDataBlock(std::string_view name, Int32 group, GameBaseData** prevBlock) {
    if (prevBlock) {
        *prevBlock = nullptr;
    }
    if (group < 0 || group >= NumDataTypes) {
        return nullptr;
    }
    if (auto* existing = lookupDataBlock(name)) {
        if (prevBlock) {
            *prevBlock = existing;
        }
        return existing;
    }
    auto block = instantiateBlock(group);
    if (!block) {
        return nullptr;
    }
    auto& grp = dataBlocks_[static_cast<std::size_t>(group)];
    const Int32 index = static_cast<Int32>(grp.size());
    block->dbmName = std::string(name);
    block->dbmIndex = index;
    block->dbmDatFileIndex = index;
    block->dbmGroup = group;
    auto* raw = block.get();
    grp.push_back(std::move(block));

    const auto h = hashName(name);
    hashTable_[h].insert(hashTable_[h].begin(), HashNode{group, index});
    return raw;
}

bool DataBlockManager::createBlock(Int32 group, Int32 block, Int32 totalBlocks) {
    if (group < 0 || group >= NumDataTypes || totalBlocks < 0 || block < 0 ||
        block >= totalBlocks) {
        return false;
    }
    auto& grp = dataBlocks_[static_cast<std::size_t>(group)];
    if (grp.empty() && totalBlocks > 0) {
        grp.resize(static_cast<std::size_t>(totalBlocks));
    } else if (static_cast<std::size_t>(block) >= grp.size()) {
        grp.resize(static_cast<std::size_t>(std::max(totalBlocks, block + 1)));
    }
    auto inst = instantiateBlock(group);
    if (!inst) {
        return false;
    }
    inst->dbmIndex = block;
    inst->dbmDatFileIndex = block;
    inst->dbmGroup = group;
    grp[static_cast<std::size_t>(block)] = std::move(inst);
    return true;
}

bool DataBlockManager::preloadBlock(Int32 group, Int32 block) {
    if (group < 0 || group >= NumDataTypes) {
        return false;
    }
    auto& grp = dataBlocks_[static_cast<std::size_t>(group)];
    if (block < 0 || static_cast<std::size_t>(block) >= grp.size() ||
        !grp[static_cast<std::size_t>(block)]) {
        return false;
    }
    std::string err;
    if (!grp[static_cast<std::size_t>(block)]->preload(resources(), isServer_, err)) {
        lastError_ = err;
        return false;
    }
    return true;
}

bool DataBlockManager::serverPreload() {
    bool ok = true;
    for (auto g : std::views::iota(Int32{0}) | std::views::take_while([&](Int32 g) {
                      return g < NumDataTypes;
                  })) {
        auto& grp = dataBlocks_[static_cast<std::size_t>(g)];
        for (const auto& i : grp) {
            if (!i) {
                continue;
            }
            std::string err;
            if (!i->preload(resources(), true, err)) {
                lastError_ = err;
                if (auto* mgr = getManager(); mgr && mgr->console()) {
                    mgr->console()->print(err);
                }
                ok = false;
            }
        }
    }
    return ok;
}

std::shared_ptr<GameBaseData>
DataBlockManager::retainDataBlock(const GameBaseData* block) const noexcept {
    if (block && block->dbmGroup >= 0 && block->dbmGroup < NumDataTypes && block->dbmIndex >= 0) {
        const auto& group = dataBlocks_[static_cast<std::size_t>(block->dbmGroup)];
        const auto index = static_cast<std::size_t>(block->dbmIndex);
        if (index < group.size() && group[index].get() == block)
            return group[index];
    }
    return {};
}

void DataBlockManager::reset() {
    for (auto& grp : dataBlocks_) {
        grp.clear();
    }
    for (auto& bucket : hashTable_) {
        bucket.clear();
    }
    lastError_.clear();
}

Int32 DataBlockManager::getDataGroupSize(Int32 group) const noexcept {
    if (group < 0 || group >= NumDataTypes) {
        return 0;
    }
    return static_cast<Int32>(dataBlocks_[static_cast<std::size_t>(group)].size());
}

Int32 DataBlockManager::getDataGroupBitSize(Int32 group) const noexcept {
    if (group < 0 || group >= NumDataTypes) {
        return 0;
    }
    return countBits(static_cast<Int32>(dataBlocks_[static_cast<std::size_t>(group)].size()) + 1);
}

void DataBlockManager::writeBlockId(BitStream& stream, Int32 /*group*/, Int32 value) const {
    stream.writeInt(value + 1, 8);
}

Int32 DataBlockManager::readBlockId(BitStream& stream, Int32 /*group*/) const {
    return stream.readInt(8) - 1;
}

GameBaseData* DataBlockManager::lookupDataBlock(Int32 blockId, Int32 groupId) const noexcept {
    if (groupId < 0 || groupId >= NumDataTypes || blockId < 0) {
        return nullptr;
    }
    const auto& grp = dataBlocks_[static_cast<std::size_t>(groupId)];
    if (static_cast<std::size_t>(blockId) >= grp.size()) {
        return nullptr;
    }
    return grp[static_cast<std::size_t>(blockId)].get();
}

std::string_view DataBlockManager::lookupBlockName(Int32 blockId, Int32 groupId) const noexcept {
    if (const auto* blk = lookupDataBlock(blockId, groupId)) {
        return blk->dbmName;
    }
    return {};
}

GameBaseData* DataBlockManager::lookupDataBlock(std::string_view name,
                                                Int32 groupId,
                                                Int32* blockId) const noexcept {
    if (blockId) {
        *blockId = -1;
    }
    if (groupId < 0 || groupId >= NumDataTypes || name.empty()) {
        return nullptr;
    }
    const auto& grp = dataBlocks_[static_cast<std::size_t>(groupId)];
    for (auto i : std::views::iota(std::size_t{0}, grp.size())) {
        if (grp[i] && equalIgnoreCase(grp[i]->dbmName, name)) {
            if (blockId) {
                *blockId = static_cast<Int32>(i);
            }
            return grp[i].get();
        }
    }
    return nullptr;
}

GameBaseData* DataBlockManager::lookupDataBlock(std::string_view name) const noexcept {
    if (name.empty()) {
        return nullptr;
    }
    const auto h = hashName(name);
    for (const auto& node : hashTable_[h]) {
        if (auto* blk = lookupDataBlock(node.index, node.group)) {
            if (equalIgnoreCase(blk->dbmName, name)) {
                return blk;
            }
        }
    }
    return nullptr;
}

std::vector<std::unique_ptr<DataBlockEvent>> DataBlockManager::buildClientEvents() const {
    std::vector<std::unique_ptr<DataBlockEvent>> events;
    for (auto g : std::views::iota(Int32{0}) | std::views::take_while([&](Int32 g) {
                      return g < NumDataTypes;
                  })) {
        const auto& grp = dataBlocks_[static_cast<std::size_t>(g)];
        if (!grp.empty()) {
            const Int32 gsize = static_cast<Int32>(grp.size());
            for (auto j : std::views::iota(Int32{0}) | std::views::take_while([&](Int32 j) {
                              return j < gsize;
                          })) {
                auto evt = std::make_unique<DataBlockEvent>();
                evt->group = g;
                evt->totalBlocks = gsize;
                evt->block = j;
                if (grp[static_cast<std::size_t>(j)]) {
                    evt->data = grp[static_cast<std::size_t>(j)]->clone();
                }
                events.push_back(std::move(evt));
            }
        } else {
            auto evt = std::make_unique<DataBlockEvent>();
            evt->group = g;
            evt->totalBlocks = 0;
            evt->block = -1;
            events.push_back(std::move(evt));
        }
    }
    return events;
}

bool DataBlockManager::sendDataToClient(sim::SimObjectId clientManagerId) {
    auto* mgr = getManager();
    if (!mgr) {
        return false;
    }
    auto events = buildClientEvents();
    for (auto& evt : events) {
        evt->address.managerId = clientManagerId;
        if (!mgr->postEvent(std::move(evt), true)) {
            return false;
        }
    }
    return true;
}

bool DataBlockManager::sendDataToStream(sim::network::PacketStream& stream) const {
    auto events = buildClientEvents();
    for (auto& evt : events) {
        if (!stream.sendRemoteEvent(std::move(evt))) {
            return false;
        }
    }
    return true;
}

bool DataBlockManager::processEvent(const sim::SimEvent& event) {
    if (event.type != DataBlockEventType) {
        return sim::SimObject::processEvent(event);
    }
    const auto* evt = dynamic_cast<const DataBlockEvent*>(&event);
    if (!evt || evt->group < 0 || evt->group >= NumDataTypes || evt->totalBlocks < 0) {
        return false;
    }
    auto& grp = dataBlocks_[static_cast<std::size_t>(evt->group)];
    if (grp.empty() && evt->totalBlocks > 0) {
        grp.resize(static_cast<std::size_t>(evt->totalBlocks));
    }
    if (evt->block != -1 && evt->data) {
        if (evt->block < 0 || evt->block >= evt->totalBlocks) {
            return false;
        }
        if (static_cast<std::size_t>(evt->block) >= grp.size()) {
            grp.resize(static_cast<std::size_t>(evt->totalBlocks));
        }
        auto cloned = evt->data->clone();
        std::string err;
        if (!cloned->preload(resources(), false, err)) {
            lastError_ = err;
            if (getManager() && getManager()->console()) {
                getManager()->console()->print(std::format(
                    "Datablock group {} block {} preload failed: {}", evt->group, evt->block, err));
            }
            return false;
        }
        cloned->dbmIndex = evt->block;
        cloned->dbmDatFileIndex = evt->block;
        cloned->dbmGroup = evt->group;
        cloned->dbmName.clear();
        grp[static_cast<std::size_t>(evt->block)] = std::move(cloned);

        if (auto* mgr = getManager(); mgr && mgr->console() && evt->totalBlocks > 0) {
            const float pct = static_cast<float>(evt->group * evt->totalBlocks + evt->block) /
                              static_cast<float>(NumDataTypes * evt->totalBlocks);
            const std::array<std::string, 2> args{
                std::string(dataGroupDisplayName(evt->group)),
                std::format("{:.6g}", static_cast<double>(pct)),
            };
            (void)mgr->console()->execute("dataGotBlock", args);
        }
    }
    if (evt->block == (evt->totalBlocks - 1) && evt->group == (NumDataTypes - 1)) {
        if (onDataFinished) {
            onDataFinished();
        }
        if (auto* mgr = getManager(); mgr && mgr->console()) {
            (void)mgr->console()->execute("dataFinished");
        }
    }
    return true;
}

DataBlockManager::Snapshot DataBlockManager::captureSnapshot() const {
    Snapshot snap;
    snap.lastError = lastError_;
    for (auto g : std::views::iota(std::size_t{0}) | std::views::take_while([&](std::size_t g) {
                      return g < NumDataTypes;
                  })) {
        snap.groups[g].reserve(dataBlocks_[g].size());
        for (const auto& blk : dataBlocks_[g]) {
            snap.groups[g].push_back(blk ? blk->clone() : nullptr);
        }
    }
    return snap;
}

void DataBlockManager::restoreSnapshot(const Snapshot& snapshot) {
    lastError_ = snapshot.lastError;
    for (auto g : std::views::iota(std::size_t{0}) | std::views::take_while([&](std::size_t g) {
                      return g < NumDataTypes;
                  })) {
        dataBlocks_[g].clear();
        dataBlocks_[g].reserve(snapshot.groups[g].size());
        for (const auto& blk : snapshot.groups[g]) {
            dataBlocks_[g].push_back(blk ? blk->clone() : nullptr);
        }
    }
    rebuildLookupTable();
}

} // namespace opent1::game
