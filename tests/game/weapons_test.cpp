#include <opent1/core/bit_stream.hpp>
#include <ranges>
#include <opent1/core/mem_stream.hpp>
#include <opent1/game/flier.hpp>
#include <opent1/game/mine.hpp>
#include <opent1/game/data_block_manager.hpp>
#include <opent1/game/debris.hpp>
#include <opent1/game/explosion.hpp>
#include <opent1/game/fear_player_psc.hpp>
#include <opent1/game/item.hpp>
#include <opent1/game/part_debris.hpp>
#include <opent1/game/player.hpp>
#include <opent1/game/player_manager.hpp>
#include <opent1/game/proj_attach.hpp>
#include <opent1/game/proj_bullet.hpp>
#include <opent1/game/proj_grenade.hpp>
#include <opent1/game/proj_laser.hpp>
#include <opent1/game/proj_lightning.hpp>
#include <opent1/game/proj_repair.hpp>
#include <opent1/game/proj_rocket.hpp>
#include <opent1/game/proj_seek_missile.hpp>
#include <opent1/game/proj_target_laser.hpp>
#include <opent1/game/projectile.hpp>
#include <opent1/game/projectile_factory.hpp>
#include <opent1/game/sensor.hpp>
#include <opent1/game/sensor_manager.hpp>
#include <opent1/game/smoke_puff.hpp>
#include <opent1/game/turret.hpp>
#include <opent1/game/vehicle.hpp>

#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace opent1;
using namespace opent1::game;

void fail(const char* expr, const char* file, int line) {
    std::fprintf(stderr, "FAIL: %s (%s:%d)\n", expr, file, line);
    std::exit(1);
}

#define CHECK(expr)                                                                                \
    do {                                                                                           \
        if (!(expr)) {                                                                             \
            fail(#expr, __FILE__, __LINE__);                                                       \
        }                                                                                          \
    } while (false)

bool nearEq(float a, float b, float tol = 1e-4f) {
    return std::fabs(a - b) <= tol;
}

bool nearPoint(const Point3F& a, const Point3F& b, float tol = 1e-4f) {
    return nearEq(a.x, b.x, tol) && nearEq(a.y, b.y, tol) && nearEq(a.z, b.z, tol);
}

bool nearBox(const Box3F& a, const Box3F& b, float tol = 1e-4f) {
    return nearPoint(a.fMin, b.fMin, tol) && nearPoint(a.fMax, b.fMax, tol);
}

template <typename T> T& createBlock(DataBlockManager& dbm, Int32 group, std::string_view name) {
    auto* ptr = static_cast<T*>(dbm.createDataBlock(name, group));
    CHECK(ptr != nullptr);
    return *ptr;
}

class BinaryFixtureReader {
public:
    explicit BinaryFixtureReader(const std::string& path) {
        std::ifstream in(path, std::ios::binary);
        CHECK(static_cast<bool>(in));
        in.seekg(0, std::ios::end);
        const auto len = in.tellg();
        CHECK(len > 0);
        data_.resize(static_cast<std::size_t>(len));
        in.seekg(0, std::ios::beg);
        in.read(reinterpret_cast<char*>(data_.data()), len);
        CHECK(static_cast<bool>(in));
    }

    void expectMagic(const char (&magic)[5], std::uint32_t expectedVersion) {
        CHECK(pos_ + 8 <= data_.size());
        CHECK(std::memcmp(data_.data() + pos_, magic, 4) == 0);
        pos_ += 4;
        CHECK(readU32() == expectedVersion);
    }

    std::uint32_t readU32() {
        CHECK(pos_ + 4 <= data_.size());
        std::uint32_t v = 0;
        std::memcpy(&v, data_.data() + pos_, 4);
        pos_ += 4;
        return v;
    }

    std::int32_t readI32() {
        return static_cast<std::int32_t>(readU32());
    }

    float readF32() {
        CHECK(pos_ + 4 <= data_.size());
        float v = 0.0f;
        std::memcpy(&v, data_.data() + pos_, 4);
        pos_ += 4;
        return v;
    }

    Point3F readPoint3F() {
        const float x = readF32();
        const float y = readF32();
        const float z = readF32();
        return Point3F{x, y, z};
    }

    Box3F readBox3F() {
        const Point3F mn = readPoint3F();
        const Point3F mx = readPoint3F();
        return Box3F{mn, mx};
    }

    std::vector<std::byte> readBytes() {
        const std::uint32_t len = readU32();
        CHECK(pos_ + len <= data_.size());
        std::vector<std::byte> out(len);
        if (len > 0) {
            std::memcpy(out.data(), data_.data() + pos_, len);
        }
        pos_ += len;
        return out;
    }

    struct PacketRecord {
        std::uint32_t bitCount{0};
        std::vector<std::byte> bytes;
    };

    PacketRecord readPacketRecord() {
        PacketRecord rec{};
        rec.bitCount = readU32();
        rec.bytes = readBytes();
        return rec;
    }

    [[nodiscard]] bool atEnd() const noexcept {
        return pos_ == data_.size();
    }

private:
    std::vector<std::uint8_t> data_;
    std::size_t pos_{0};
};

template <typename PackFn> BinaryFixtureReader::PacketRecord captureBitStream(PackFn&& fn) {
    std::vector<std::byte> buf(4096, std::byte{0});
    BitStream bs(std::span<std::byte>(buf.data(), buf.size()));
    fn(bs);
    BinaryFixtureReader::PacketRecord rec{};
    rec.bitCount = static_cast<std::uint32_t>(bs.getCurPos());
    const std::size_t byteCount = static_cast<std::size_t>((bs.getCurPos() + 7) >> 3);
    rec.bytes.assign(buf.begin(), buf.begin() + static_cast<std::ptrdiff_t>(byteCount));
    return rec;
}

template <typename WriteFn> std::vector<std::byte> captureMemStream(WriteFn&& fn) {
    std::vector<std::byte> buf(2048, std::byte{0});
    MemRWStream ms(std::span<std::byte>(buf.data(), buf.size()));
    fn(ms);
    return std::vector<std::byte>(buf.begin(),
                                  buf.begin() + static_cast<std::ptrdiff_t>(ms.getPosition()));
}

void checkPacketParity(const BinaryFixtureReader::PacketRecord& actual,
                       const BinaryFixtureReader::PacketRecord& expected,
                       const char* label = "") {
    if (actual.bitCount != expected.bitCount || actual.bytes != expected.bytes) {
        std::fprintf(
            stderr,
            "Packet mismatch [%s]: actual bits=%u (bytes=%zu) vs expected bits=%u (bytes=%zu)\n",
            label,
            actual.bitCount,
            actual.bytes.size(),
            expected.bitCount,
            expected.bytes.size());
        const std::size_t n = std::min(actual.bytes.size(), expected.bytes.size());
        for (auto i : std::views::iota(std::size_t{0}) | std::views::take_while([&](std::size_t i) {
                          return i < n;
                      })) {
            if (actual.bytes[i] != expected.bytes[i]) {
                std::fprintf(stderr,
                             "  first byte diff at index %zu: actual=0x%02X vs expected=0x%02X\n",
                             i,
                             static_cast<unsigned>(actual.bytes[i]),
                             static_cast<unsigned>(expected.bytes[i]));
                break;
            }
        }
    }
    CHECK(actual.bitCount == expected.bitCount);
    CHECK(actual.bytes == expected.bytes);
}

template <typename UnpackFn>
void verifyBitStreamTruncationRollback(const BinaryFixtureReader::PacketRecord& pkt,
                                       UnpackFn&& unpackFn) {
    if (pkt.bitCount == 0) {
        return;
    }
    const std::uint32_t step = (pkt.bitCount > 64u) ? 7u : 1u;
    for (std::uint32_t bits = 0; bits < pkt.bitCount; bits += step) {
        std::vector<std::byte> buf = pkt.bytes;
        BitStream bs(std::span<const std::byte>(buf.data(), buf.size()));
        bs.setLimits(static_cast<std::size_t>(bits), 0);
        CHECK(!unpackFn(bs));
        CHECK(bs.getCurPos() == 0);
    }
}

void testFixtureParity() {
    BinaryFixtureReader reader(OPENT1_GAME_WEAPONS_FIXTURE);
    reader.expectMagic("GWV1", 1);

    DataBlockManager dbm(true);
    auto& bulletData = createBlock<BulletData>(dbm, DataBlockManager::BulletDataType, "Bullet0");
    auto& grenadeData =
        createBlock<GrenadeData>(dbm, DataBlockManager::GrenadeDataType, "Grenade0");
    auto& rocketData = createBlock<RocketData>(dbm, DataBlockManager::RocketDataType, "Rocket0");
    auto& laserData = createBlock<LaserData>(dbm, DataBlockManager::LaserDataType, "Laser0");
    auto& tlaserData =
        createBlock<TargetLaserData>(dbm, DataBlockManager::TargetLaserDataType, "TLaser0");
    auto& seekData =
        createBlock<SeekingMissileData>(dbm, DataBlockManager::SeekingMissileDataType, "Seek0");
    auto& lightningData =
        createBlock<LightningData>(dbm, DataBlockManager::LightningDataType, "Lightning0");
    auto& repairData =
        createBlock<RepairEffectData>(dbm, DataBlockManager::RepairEffectDataType, "Repair0");
    auto& expData = createBlock<ExplosionData>(dbm, DataBlockManager::ExplosionDataType, "Exp0");
    auto& debrisData = createBlock<DebrisData>(dbm, DataBlockManager::DebrisDataType, "Debris0");
    auto& itemData = createBlock<ItemData>(dbm, DataBlockManager::ItemDataType, "Item0");
    auto& mineData = createBlock<MineData>(dbm, DataBlockManager::MineDataType, "Mine0");
    auto& sensorData = createBlock<SensorData>(dbm, DataBlockManager::SensorDataType, "Sensor0");
    auto& turretData = createBlock<TurretData>(dbm, DataBlockManager::TurretDataType, "Turret0");
    auto& vehicleData =
        createBlock<VehicleData>(dbm, DataBlockManager::VehicleDataType, "Vehicle0");
    auto& flierData = createBlock<FlierData>(dbm, DataBlockManager::FlierDataType, "Flier0");
    for (auto i : std::views::iota(0, 4)) {
        createBlock<SoundData>(dbm, DataBlockManager::SoundDataType, "Sound" + std::to_string(i));
    }

    // =========================================================================
    // Section 1: Datablock BitStream pack/unpack parity & truncation rollback
    // =========================================================================
    {
        bulletData.bulletHoleIndex = 2;
        bulletData.explosionTag = 0;
        bulletData.expRandCycle = 3;
        bulletData.collideWithOwner = true;
        bulletData.ownerGraceMS = 250;
        bulletData.collisionRadius = 0.25f;
        bulletData.mass = 0.5f;
        bulletData.damageClass = 1;
        bulletData.damageValue = 0.18f;
        bulletData.damageType = 2;
        bulletData.explosionRadius = 4.0f;
        bulletData.kickBackStrength = 120.0f;
        bulletData.aimDeflection = 0.0f;
        bulletData.muzzleVelocity = 350.0f;
        bulletData.terminalVelocity = 350.0f;
        bulletData.acceleration = 0.0f;
        bulletData.totalTime = 2.0f;
        bulletData.liveTime = 1.8f;
        bulletData.projSpecialTime = 0.75f;
        bulletData.projSpecialBool = false;
        bulletData.lightRange = 5.0f;
        bulletData.lightColor = ColorF{1.0f, 0.8f, 0.2f};
        bulletData.inheritedVelocityScale = 0.5f;
        bulletData.soundId = 1;
        bulletData.rotationPeriod = 1.5f;
        bulletData.tracerLength = 15.0f;

        const auto expBulletPkt = reader.readPacketRecord();
        const auto actBulletPkt = captureBitStream([&](BitStream& bs) {
            bulletData.pack(bs, &dbm);
        });
        checkPacketParity(actBulletPkt, expBulletPkt, "BulletData::pack");
        {
            BulletData unpacked{};
            BitStream rbs(
                std::span<const std::byte>(expBulletPkt.bytes.data(), expBulletPkt.bytes.size()));
            CHECK(unpacked.unpack(rbs, &dbm));
            CHECK(unpacked.bulletHoleIndex == 2);
            CHECK(nearEq(unpacked.tracerLength, 15.0f));
            CHECK(nearEq(unpacked.muzzleVelocity, 350.0f));
        }
        verifyBitStreamTruncationRollback(expBulletPkt, [&](BitStream& rbs) {
            BulletData d{};
            return d.unpack(rbs, &dbm);
        });

        grenadeData.muzzleVelocity = 40.0f;
        grenadeData.terminalVelocity = 40.0f;
        grenadeData.totalTime = 4.0f;
        grenadeData.liveTime = 2.5f;
        grenadeData.projSpecialTime = 0.2f;
        grenadeData.inheritedVelocityScale = 0.75f;
        grenadeData.elasticity = 0.45f;
        grenadeData.pSmokeName = "grenadeSmoke.dts";
        const auto expGrenadePkt = reader.readPacketRecord();
        const auto actGrenadePkt = captureBitStream([&](BitStream& bs) {
            grenadeData.pack(bs, &dbm);
        });
        checkPacketParity(actGrenadePkt, expGrenadePkt, "GrenadeData::pack");
        {
            GrenadeData unpacked{};
            BitStream rbs(
                std::span<const std::byte>(expGrenadePkt.bytes.data(), expGrenadePkt.bytes.size()));
            CHECK(unpacked.unpack(rbs, &dbm));
            CHECK(nearEq(unpacked.elasticity, 0.45f));
            CHECK(unpacked.pSmokeName == "grenadeSmoke.dts");
        }
        verifyBitStreamTruncationRollback(expGrenadePkt, [&](BitStream& rbs) {
            GrenadeData d{};
            return d.unpack(rbs, &dbm);
        });

        rocketData.muzzleVelocity = 55.0f;
        rocketData.terminalVelocity = 95.0f;
        rocketData.acceleration = 20.0f;
        rocketData.totalTime = 6.0f;
        rocketData.liveTime = 6.0f;
        rocketData.projSpecialTime = 1.0f;
        rocketData.trailType = RocketData::Swoosh;
        rocketData.trailString = "rocketTrail.bmp";
        rocketData.trailLength = 12.0f;
        rocketData.trailWidth = 0.8f;
        const auto expRocketPkt = reader.readPacketRecord();
        const auto actRocketPkt = captureBitStream([&](BitStream& bs) {
            rocketData.pack(bs, &dbm);
        });
        checkPacketParity(actRocketPkt, expRocketPkt, "RocketData::pack");
        {
            RocketData unpacked{};
            BitStream rbs(
                std::span<const std::byte>(expRocketPkt.bytes.data(), expRocketPkt.bytes.size()));
            CHECK(unpacked.unpack(rbs, &dbm));
            CHECK(unpacked.trailType == RocketData::Swoosh);
            CHECK(unpacked.trailString == "rocketTrail.bmp");
            CHECK(nearEq(unpacked.trailLength, 12.0f));
        }
        verifyBitStreamTruncationRollback(expRocketPkt, [&](BitStream& rbs) {
            RocketData d{};
            return d.unpack(rbs, &dbm);
        });

        laserData.totalTime = 0.5f;
        laserData.liveTime = 0.25f;
        laserData.damageValue = 0.6f;
        laserData.pBitmapName = "laserPulse.bmp";
        laserData.pHitShapeName = "laserHit.dts";
        laserData.detachFromShooter = true;
        const auto expLaserPkt = reader.readPacketRecord();
        const auto actLaserPkt = captureBitStream([&](BitStream& bs) {
            laserData.pack(bs, &dbm);
        });
        checkPacketParity(actLaserPkt, expLaserPkt, "LaserData::pack");
        {
            LaserData unpacked{};
            BitStream rbs(
                std::span<const std::byte>(expLaserPkt.bytes.data(), expLaserPkt.bytes.size()));
            CHECK(unpacked.unpack(rbs, &dbm));
            CHECK(unpacked.pBitmapName == "laserPulse.bmp");
            CHECK(unpacked.pHitShapeName == "laserHit.dts");
            CHECK(unpacked.detachFromShooter);
        }
        verifyBitStreamTruncationRollback(expLaserPkt, [&](BitStream& rbs) {
            LaserData d{};
            return d.unpack(rbs, &dbm);
        });

        seekData.muzzleVelocity = 60.0f;
        seekData.terminalVelocity = 80.0f;
        seekData.totalTime = 8.0f;
        seekData.liveTime = 8.0f;
        seekData.seekingTurningRadius = 12.0f;
        seekData.nonSeekingTurningRadius = 45.0f;
        seekData.proximityDist = 2.5f;
        const auto expSeekPkt = reader.readPacketRecord();
        const auto actSeekPkt = captureBitStream([&](BitStream& bs) {
            seekData.pack(bs, &dbm);
        });
        checkPacketParity(actSeekPkt, expSeekPkt, "SeekingMissileData::pack");
        {
            SeekingMissileData unpacked{};
            BitStream rbs(
                std::span<const std::byte>(expSeekPkt.bytes.data(), expSeekPkt.bytes.size()));
            CHECK(unpacked.unpack(rbs, &dbm));
            CHECK(nearEq(unpacked.seekingTurningRadius, 12.0f));
            CHECK(nearEq(unpacked.nonSeekingTurningRadius, 45.0f));
        }
        verifyBitStreamTruncationRollback(expSeekPkt, [&](BitStream& rbs) {
            SeekingMissileData d{};
            return d.unpack(rbs, &dbm);
        });

        lightningData.totalTime = 2.0f;
        lightningData.damageValue = 0.35f;
        lightningData.boltLength = 35.0f;
        lightningData.coneAngle = 15.0f;
        lightningData.damagePerSec = 25.0f;
        lightningData.energyDrainPerSec = 10.0f;
        lightningData.segmentDivisions = 4;
        lightningData.numSegments = 6;
        lightningData.beamWidth = 0.15f;
        lightningData.updateTime = 120;
        lightningData.skipPercent = 0.3f;
        lightningData.displaceBias = 0.4f;
        lightningData.pBitmapName = "lightning.bmp";
        const auto expLightningPkt = reader.readPacketRecord();
        const auto actLightningPkt = captureBitStream([&](BitStream& bs) {
            lightningData.pack(bs, &dbm);
        });
        checkPacketParity(actLightningPkt, expLightningPkt, "LightningData::pack");
        {
            LightningData unpacked{};
            BitStream rbs(std::span<const std::byte>(expLightningPkt.bytes.data(),
                                                     expLightningPkt.bytes.size()));
            CHECK(unpacked.unpack(rbs, &dbm));
            CHECK(unpacked.segmentDivisions == 4);
            CHECK(unpacked.numSegments == 6);
            CHECK(unpacked.pBitmapName == "lightning.bmp");
        }
        verifyBitStreamTruncationRollback(expLightningPkt, [&](BitStream& rbs) {
            LightningData d{};
            return d.unpack(rbs, &dbm);
        });

        repairData.totalTime = 3.0f;
        repairData.damageValue = 0.1f;
        repairData.boltLength = 15.0f;
        repairData.beamWidth = 0.12f;
        repairData.numSegments = 8;
        repairData.segmentDivisions = 3;
        repairData.displaceBias = 0.2f;
        repairData.pBitmapName = "repair.bmp";
        repairData.updateTime = 100;
        repairData.skipPercent = 0.2f;
        const auto expRepairPkt = reader.readPacketRecord();
        const auto actRepairPkt = captureBitStream([&](BitStream& bs) {
            repairData.pack(bs, &dbm);
        });
        checkPacketParity(actRepairPkt, expRepairPkt, "RepairEffectData::pack");
        {
            RepairEffectData unpacked{};
            BitStream rbs(
                std::span<const std::byte>(expRepairPkt.bytes.data(), expRepairPkt.bytes.size()));
            CHECK(unpacked.unpack(rbs, &dbm));
            CHECK(unpacked.numSegments == 8);
            CHECK(unpacked.pBitmapName == "repair.bmp");
        }
        verifyBitStreamTruncationRollback(expRepairPkt, [&](BitStream& rbs) {
            RepairEffectData d{};
            return d.unpack(rbs, &dbm);
        });

        expData.mapIcon = "";
        expData.description = "Explosion";
        expData.pShapeName = "exp.dts";
        expData.soundId = 1;
        expData.faceCamera = true;
        expData.randomSpin = false;
        expData.hasLight = true;
        expData.lightRange = 12.0f;
        expData.timeScale = 1.0f;
        expData.timeZero = 0.25f;
        expData.timeOne = 0.75f;
        expData.colors[0] = ColorF{1.0f, 0.9f, 0.5f};
        expData.colors[1] = ColorF{1.0f, 0.4f, 0.1f};
        expData.colors[2] = ColorF{0.2f, 0.1f, 0.05f};
        expData.radFactors[0] = 0.5f;
        expData.radFactors[1] = 1.0f;
        expData.radFactors[2] = 0.2f;
        expData.shiftPosition = true;
        const auto expExpPkt = reader.readPacketRecord();
        const auto actExpPkt = captureBitStream([&](BitStream& bs) {
            expData.pack(bs, &dbm);
        });
        checkPacketParity(actExpPkt, expExpPkt, "ExplosionData::pack");
        {
            ExplosionData unpacked{};
            BitStream rbs(
                std::span<const std::byte>(expExpPkt.bytes.data(), expExpPkt.bytes.size()));
            CHECK(unpacked.unpack(rbs, &dbm));
            CHECK(unpacked.pShapeName == "exp.dts");
            CHECK(unpacked.hasLight);
            CHECK(nearEq(unpacked.lightRange, 12.0f));
        }
        verifyBitStreamTruncationRollback(expExpPkt, [&](BitStream& rbs) {
            ExplosionData d{};
            return d.unpack(rbs, &dbm);
        });

        debrisData.mapIcon = "";
        debrisData.description = "Debris";
        debrisData.type = 1;
        debrisData.pShapeName = "debris.dts";
        debrisData.mass = 25.0f;
        debrisData.elasticity = 0.4f;
        debrisData.friction = 0.6f;
        debrisData.animationSequence = 2;
        debrisData.minTimeout = 2.0f;
        debrisData.maxTimeout = 4.5f;
        debrisData.explodeOnBounce = 0.3f;
        debrisData.damage = 10.0f;
        debrisData.damageThreshold = 50.0f;
        debrisData.spawnedDebrisMask = 3;
        debrisData.spawnedDebrisStrength = 25.0f;
        debrisData.spawnedDebrisRadius = 2.0f;
        debrisData.spawnedExplosionID = 0;
        debrisData.p = 0.5f;
        debrisData.explodeOnRest = true;
        debrisData.collisionDetail = 0;
        const auto expDebrisPkt = reader.readPacketRecord();
        const auto actDebrisPkt = captureBitStream([&](BitStream& bs) {
            debrisData.pack(bs, &dbm);
        });
        checkPacketParity(actDebrisPkt, expDebrisPkt, "DebrisData::pack");
        {
            DebrisData unpacked{};
            BitStream rbs(
                std::span<const std::byte>(expDebrisPkt.bytes.data(), expDebrisPkt.bytes.size()));
            CHECK(unpacked.unpack(rbs, &dbm));
            CHECK(unpacked.pShapeName == "debris.dts");
            CHECK(nearEq(unpacked.mass, 25.0f));
        }
        verifyBitStreamTruncationRollback(expDebrisPkt, [&](BitStream& rbs) {
            DebrisData d{};
            return d.unpack(rbs, &dbm);
        });

        itemData.mapIcon = "";
        itemData.shieldShapeName = "";
        itemData.fileName = "disclauncher.dts";
        itemData.description = "Disc Launcher";
        itemData.showInventory = true;
        itemData.lightType = Item::PulsingLight;
        itemData.lightRadius = 4.5f;
        itemData.lightTime = 1.25f;
        itemData.lightColor = Point3F{0.3f, 0.6f, 1.0f};
        itemData.hudIcon = "icon_disc";
        itemData.showWeaponBar = true;
        itemData.hiliteOnActive = true;
        const auto expItemPkt = reader.readPacketRecord();
        const auto actItemPkt = captureBitStream([&](BitStream& bs) {
            itemData.pack(bs, &dbm);
        });
        checkPacketParity(actItemPkt, expItemPkt, "ItemData::pack");
        {
            ItemData unpacked{};
            BitStream rbs(
                std::span<const std::byte>(expItemPkt.bytes.data(), expItemPkt.bytes.size()));
            CHECK(unpacked.unpack(rbs, &dbm));
            CHECK(unpacked.fileName == "disclauncher.dts");
            CHECK(unpacked.hudIcon == "icon_disc");
            CHECK(unpacked.lightType == Item::PulsingLight);
        }
        verifyBitStreamTruncationRollback(expItemPkt, [&](BitStream& rbs) {
            ItemData d{};
            return d.unpack(rbs, &dbm);
        });

        mineData.mapIcon = "";
        mineData.shieldShapeName = "";
        mineData.hudIcon = "icon_mine";
        mineData.fileName = "mine.dts";
        mineData.description = "Mine";
        mineData.damageType = 2;
        mineData.damageValue = 0.65f;
        mineData.explosionRadius = 8.0f;
        mineData.kickBackStrength = 150.0f;
        const auto expMinePkt = reader.readPacketRecord();
        const auto actMinePkt = captureBitStream([&](BitStream& bs) {
            mineData.pack(bs, &dbm);
        });
        checkPacketParity(actMinePkt, expMinePkt, "MineData::pack");
        {
            MineData unpacked{};
            BitStream rbs(
                std::span<const std::byte>(expMinePkt.bytes.data(), expMinePkt.bytes.size()));
            CHECK(unpacked.unpack(rbs, &dbm));
            CHECK(unpacked.fileName == "mine.dts");
            CHECK(unpacked.hudIcon == "icon_mine");
        }
        verifyBitStreamTruncationRollback(expMinePkt, [&](BitStream& rbs) {
            MineData d{};
            return d.unpack(rbs, &dbm);
        });

        sensorData.mapIcon = "";
        sensorData.shieldShapeName = "";
        sensorData.fileName = "pulse_sensor.dts";
        sensorData.description = "Pulse Sensor";
        sensorData.fCastLOS = true;
        sensorData.iRange = 200;
        sensorData.iDopplerVelocity = 5;
        sensorData.supression = false;
        sensorData.supressable = true;
        sensorData.pinger = true;
        const auto expSensorPkt = reader.readPacketRecord();
        const auto actSensorPkt = captureBitStream([&](BitStream& bs) {
            sensorData.pack(bs, &dbm);
        });
        checkPacketParity(actSensorPkt, expSensorPkt, "SensorData::pack");
        {
            SensorData unpacked{};
            BitStream rbs(
                std::span<const std::byte>(expSensorPkt.bytes.data(), expSensorPkt.bytes.size()));
            CHECK(unpacked.unpack(rbs, &dbm));
            CHECK(unpacked.iRange == 200);
            CHECK(unpacked.iDopplerVelocity == 5);
            CHECK(unpacked.pinger);
        }
        verifyBitStreamTruncationRollback(expSensorPkt, [&](BitStream& rbs) {
            SensorData d{};
            return d.unpack(rbs, &dbm);
        });

        turretData.mapIcon = "";
        turretData.shieldShapeName = "";
        turretData.fileName = "plasma_turret.dts";
        turretData.description = "Plasma Turret";
        turretData.fCastLOS = true;
        turretData.iRange = 150;
        turretData.iDopplerVelocity = 0;
        turretData.supression = false;
        turretData.gunRange = 120.0f;
        turretData.speed = 2.5f;
        turretData.speedModifier = 0.5f;
        turretData.projectile.type = DataBlockManager::RocketDataType;
        turretData.projectile.dataType = 0;
        turretData.reloadDelay = 0.75f;
        turretData.minGunEnergy = 10.0f;
        turretData.maxGunEnergy = 50.0f;
        turretData.FOV = 1.5f;
        turretData.fireSound = 1;
        turretData.isSustained = false;
        const auto expTurretPkt = reader.readPacketRecord();
        const auto actTurretPkt = captureBitStream([&](BitStream& bs) {
            turretData.pack(bs, &dbm);
        });
        checkPacketParity(actTurretPkt, expTurretPkt, "TurretData::pack");
        {
            TurretData unpacked{};
            BitStream rbs(
                std::span<const std::byte>(expTurretPkt.bytes.data(), expTurretPkt.bytes.size()));
            CHECK(unpacked.unpack(rbs, &dbm));
            CHECK(nearEq(unpacked.gunRange, 120.0f));
            CHECK(unpacked.projectile.type == DataBlockManager::RocketDataType);
            CHECK(unpacked.fireSound == 1);
        }
        verifyBitStreamTruncationRollback(expTurretPkt, [&](BitStream& rbs) {
            TurretData d{};
            return d.unpack(rbs, &dbm);
        });

        vehicleData.mapIcon = "";
        vehicleData.shieldShapeName = "";
        vehicleData.fileName = "scout.dts";
        vehicleData.description = "Scout";
        vehicleData.maxDamage = 2.5f;
        vehicleData.maxEnergy = 100.0f;
        vehicleData.mass = 150.0f;
        vehicleData.drag = 0.2f;
        vehicleData.density = 1.5f;
        vehicleData.minSpeed = 0.0f;
        vehicleData.maxSpeed = 35.0f;
        vehicleData.ramDamage = 1.5f;
        vehicleData.ramDamageType = 3;
        vehicleData.visibleDriver = true;
        vehicleData.driverPose = 2;
        vehicleData.idleSound = 1;
        vehicleData.moveSound = 2;
        const auto expVehPkt = reader.readPacketRecord();
        const auto actVehPkt = captureBitStream([&](BitStream& bs) {
            vehicleData.pack(bs, &dbm);
        });
        checkPacketParity(actVehPkt, expVehPkt, "VehicleData::pack");
        {
            VehicleData unpacked{};
            BitStream rbs(
                std::span<const std::byte>(expVehPkt.bytes.data(), expVehPkt.bytes.size()));
            CHECK(unpacked.unpack(rbs, &dbm));
            CHECK(nearEq(unpacked.maxSpeed, 35.0f));
            CHECK(unpacked.visibleDriver);
            CHECK(unpacked.driverPose == 2);
        }
        verifyBitStreamTruncationRollback(expVehPkt, [&](BitStream& rbs) {
            VehicleData d{};
            return d.unpack(rbs, &dbm);
        });

        flierData.mapIcon = "";
        flierData.shieldShapeName = "";
        flierData.fileName = "lflyer.dts";
        flierData.description = "Scout Flier";
        flierData.maxDamage = 2.0f;
        flierData.maxEnergy = 120.0f;
        flierData.mass = 150.0f;
        flierData.drag = 0.2f;
        flierData.density = 1.5f;
        flierData.minSpeed = 0.0f;
        flierData.maxSpeed = 45.0f;
        flierData.visibleDriver = true;
        flierData.driverPose = 2;
        flierData.maxBank = 0.6f;
        flierData.maxPitch = 0.8f;
        flierData.lift = 0.5f;
        flierData.maxAlt = 200.0f;
        flierData.maxVertical = 15.0f;
        flierData.accel = 18.0f;
        flierData.groundDamageScale = 0.5f;
        flierData.idleSound = 1;
        flierData.moveSound = 2;
        const auto expFlierPkt = reader.readPacketRecord();
        const auto actFlierPkt = captureBitStream([&](BitStream& bs) {
            flierData.pack(bs, &dbm);
        });
        checkPacketParity(actFlierPkt, expFlierPkt, "FlierData::pack");
        {
            FlierData unpacked{};
            BitStream rbs(
                std::span<const std::byte>(expFlierPkt.bytes.data(), expFlierPkt.bytes.size()));
            CHECK(unpacked.unpack(rbs, &dbm));
            CHECK(nearEq(unpacked.maxAlt, 200.0f));
            CHECK(nearEq(unpacked.accel, 18.0f));
        }
        verifyBitStreamTruncationRollback(expFlierPkt, [&](BitStream& rbs) {
            FlierData d{};
            return d.unpack(rbs, &dbm);
        });
    }

    // =========================================================================
    // Section 2: Projectile Factory, Ballistics, Homing, & Ghost packUpdate
    // =========================================================================
    {
        sim::network::GhostManager gm;
        TMat3F trans{};
        trans.identity();
        trans.p.set(10.0f, 20.0f, 30.0f);

        // 2a. Bullet
        ProjectileDataType bType{DataBlockManager::BulletDataType, 0};
        auto pProj = createProjectile(bType);
        CHECK(pProj != nullptr);
        pProj->bindDataBlockManager(&dbm);
        auto* pBullet = dynamic_cast<Bullet*>(pProj.get());
        CHECK(pBullet != nullptr);
        CHECK(pBullet->initResources(&bulletData));
        pBullet->initProjectile(trans, Point3F{4.0f, 8.0f, 0.0f}, ~sim::SimObjectId{0});
        CHECK(pBullet->onAdd());

        const Point3F expBulletPos = reader.readPoint3F();
        const Point3F expBulletVel = reader.readPoint3F();
        CHECK(nearPoint(pBullet->getLinearPosition(), expBulletPos));
        CHECK(nearPoint(pBullet->getLinearVelocity(), expBulletVel));

        const auto expBulletInitPkt = reader.readPacketRecord();
        const auto actBulletInitPkt = captureBitStream([&](BitStream& bs) {
            pBullet->packUpdate(gm, Projectile::InitialUpdate, bs);
        });
        checkPacketParity(actBulletInitPkt, expBulletInitPkt, "Bullet::packUpdate");
        {
            Bullet clientBullet;
            clientBullet.bindDataBlockManager(&dbm);
            BitStream rbs(std::span<const std::byte>(expBulletInitPkt.bytes.data(),
                                                     expBulletInitPkt.bytes.size()));
            CHECK(clientBullet.unpackUpdate(gm, rbs));
            CHECK(nearPoint(clientBullet.spawnPosition(), expBulletPos, 0.1f));
            CHECK(nearPoint(clientBullet.spawnVelocity(), expBulletVel, 1.0f));
        }
        verifyBitStreamTruncationRollback(expBulletInitPkt, [&](BitStream& rbs) {
            Bullet clientBullet;
            clientBullet.bindDataBlockManager(&dbm);
            return clientBullet.unpackUpdate(gm, rbs);
        });

        // 2b. Grenade
        ProjectileDataType gType{DataBlockManager::GrenadeDataType, 0};
        auto pGrenadeProj = createProjectile(gType);
        pGrenadeProj->bindDataBlockManager(&dbm);
        auto* pGrenade = dynamic_cast<Grenade*>(pGrenadeProj.get());
        CHECK(pGrenade != nullptr);
        CHECK(pGrenade->initResources(&grenadeData));

        Point3F aimVec{};
        bool aimInRange = false;
        const bool aimOk = grenadeData.getTargetVector(Point3F{0.0f, 0.0f, 10.0f},
                                                       Point3F{0.0f, 50.0f, 12.0f},
                                                       20.0f,
                                                       &aimVec,
                                                       &aimInRange,
                                                       false);
        const std::uint32_t expAimOk = reader.readU32();
        const std::uint32_t expAimInRange = reader.readU32();
        const Point3F expAimVec = reader.readPoint3F();
        CHECK((aimOk ? 1u : 0u) == expAimOk);
        CHECK((aimInRange ? 1u : 0u) == expAimInRange);
        CHECK(nearPoint(aimVec, expAimVec));

        pGrenade->initProjectile(trans, Point3F{2.0f, 4.0f, 0.0f}, ~sim::SimObjectId{0});
        CHECK(pGrenade->onAdd());
        const Point3F expGrenPos = reader.readPoint3F();
        const Point3F expGrenVel = reader.readPoint3F();
        CHECK(nearPoint(pGrenade->getLinearPosition(), expGrenPos));
        CHECK(nearPoint(pGrenade->getLinearVelocity(), expGrenVel));

        const auto expGrenInitPkt = reader.readPacketRecord();
        const auto actGrenInitPkt = captureBitStream([&](BitStream& bs) {
            pGrenade->packUpdate(gm, Projectile::InitialUpdate, bs);
        });
        checkPacketParity(actGrenInitPkt, expGrenInitPkt, "Grenade::packUpdate(Initial)");

        const auto expGrenBouncePkt = reader.readPacketRecord();
        const auto actGrenBouncePkt = captureBitStream([&](BitStream& bs) {
            pGrenade->packUpdate(gm, Grenade::FGCollisionSoundMask, bs);
        });
        checkPacketParity(actGrenBouncePkt, expGrenBouncePkt, "Grenade::packUpdate(Bounce)");
        {
            Grenade clientGren;
            clientGren.bindDataBlockManager(&dbm);
            BitStream rbs(std::span<const std::byte>(expGrenInitPkt.bytes.data(),
                                                     expGrenInitPkt.bytes.size()));
            CHECK(clientGren.unpackUpdate(gm, rbs));
            CHECK(nearPoint(clientGren.getLinearPosition(), expGrenPos));

            BitStream rbs2(std::span<const std::byte>(expGrenBouncePkt.bytes.data(),
                                                      expGrenBouncePkt.bytes.size()));
            CHECK(clientGren.unpackUpdate(gm, rbs2));
        }
        verifyBitStreamTruncationRollback(expGrenInitPkt, [&](BitStream& rbs) {
            Grenade clientGren;
            clientGren.bindDataBlockManager(&dbm);
            return clientGren.unpackUpdate(gm, rbs);
        });

        // 2c. RocketDumb
        ProjectileDataType rType{DataBlockManager::RocketDataType, 0};
        auto pRocketProj = createProjectile(rType);
        pRocketProj->bindDataBlockManager(&dbm);
        auto* pRocket = dynamic_cast<RocketDumb*>(pRocketProj.get());
        CHECK(pRocket != nullptr);
        CHECK(pRocket->initResources(&rocketData));
        const float expTermVel = reader.readF32();
        CHECK(nearEq(rocketData.getTerminalVelocity(), expTermVel));

        pRocket->initProjectile(trans, Point3F{0.0f, 10.0f, 0.0f}, ~sim::SimObjectId{0});
        CHECK(pRocket->onAdd());
        const Point3F expRocketVel = reader.readPoint3F();
        CHECK(nearPoint(pRocket->getLinearVelocity(), expRocketVel));

        const auto expRocketInitPkt = reader.readPacketRecord();
        const auto actRocketInitPkt = captureBitStream([&](BitStream& bs) {
            pRocket->packUpdate(gm, Projectile::InitialUpdate, bs);
        });
        checkPacketParity(actRocketInitPkt, expRocketInitPkt, "RocketDumb::packUpdate");
        {
            RocketDumb clientRocket;
            clientRocket.bindDataBlockManager(&dbm);
            BitStream rbs(std::span<const std::byte>(expRocketInitPkt.bytes.data(),
                                                     expRocketInitPkt.bytes.size()));
            CHECK(clientRocket.unpackUpdate(gm, rbs));
            CHECK(nearPoint(clientRocket.getLinearPosition(), trans.p));
        }
        verifyBitStreamTruncationRollback(expRocketInitPkt, [&](BitStream& rbs) {
            RocketDumb clientRocket;
            clientRocket.bindDataBlockManager(&dbm);
            return clientRocket.unpackUpdate(gm, rbs);
        });

        // 2d. LaserProjectile & TargetLaser
        ProjectileDataType lType{DataBlockManager::LaserDataType, 0};
        auto pLaserProj = createProjectile(lType);
        pLaserProj->bindDataBlockManager(&dbm);
        auto* pLaser = dynamic_cast<LaserProjectile*>(pLaserProj.get());
        CHECK(pLaser != nullptr);
        CHECK(pLaser->initResources(&laserData));
        pLaser->initProjectile(trans, Point3F{0.0f, 0.0f, 0.0f}, ~sim::SimObjectId{0});
        const auto expLaserInitPkt = reader.readPacketRecord();
        const auto actLaserInitPkt = captureBitStream([&](BitStream& bs) {
            pLaser->packUpdate(gm, Projectile::InitialUpdate, bs);
        });
        checkPacketParity(actLaserInitPkt, expLaserInitPkt, "LaserProjectile::packUpdate");
        verifyBitStreamTruncationRollback(expLaserInitPkt, [&](BitStream& rbs) {
            LaserProjectile clientLaser;
            clientLaser.bindDataBlockManager(&dbm);
            return clientLaser.unpackUpdate(gm, rbs);
        });

        ProjectileDataType tlType{DataBlockManager::TargetLaserDataType, 0};
        auto pTLaserProj = createProjectile(tlType);
        pTLaserProj->bindDataBlockManager(&dbm);
        auto* pTLaser = dynamic_cast<TargetLaser*>(pTLaserProj.get());
        CHECK(pTLaser != nullptr);
        CHECK(pTLaser->initResources(&tlaserData));
        pTLaser->initProjectile(trans, Point3F{0.0f, 0.0f, 0.0f}, ~sim::SimObjectId{0});
        const auto expTLaserInitPkt = reader.readPacketRecord();
        const auto actTLaserInitPkt = captureBitStream([&](BitStream& bs) {
            pTLaser->packUpdate(gm, Projectile::InitialUpdate, bs);
        });
        checkPacketParity(actTLaserInitPkt, expTLaserInitPkt, "TargetLaser::packUpdate(Initial)");

        const auto expTLaserUpdPkt = reader.readPacketRecord();
        const auto actTLaserUpdPkt = captureBitStream([&](BitStream& bs) {
            pTLaser->packUpdate(gm, TargetLaser::EndPointMask, bs);
        });
        checkPacketParity(actTLaserUpdPkt, expTLaserUpdPkt, "TargetLaser::packUpdate(EndPoint)");
        verifyBitStreamTruncationRollback(expTLaserInitPkt, [&](BitStream& rbs) {
            TargetLaser clientTLaser;
            clientTLaser.bindDataBlockManager(&dbm);
            return clientTLaser.unpackUpdate(gm, rbs);
        });

        // 2e. SeekingMissile
        ProjectileDataType sType{DataBlockManager::SeekingMissileDataType, 0};
        auto pSeekProj = createProjectile(sType);
        pSeekProj->bindDataBlockManager(&dbm);
        auto* pSeek = dynamic_cast<SeekingMissile*>(pSeekProj.get());
        CHECK(pSeek != nullptr);
        CHECK(pSeek->initResources(&seekData));
        pSeek->initProjectile(trans, Point3F{0.0f, 5.0f, 0.0f}, ~sim::SimObjectId{0});
        CHECK(pSeek->onAdd());
        const Point3F expSeekVel = reader.readPoint3F();
        CHECK(nearPoint(pSeek->getLinearVelocity(), expSeekVel));

        const auto expSeekInitPkt = reader.readPacketRecord();
        const auto actSeekInitPkt = captureBitStream([&](BitStream& bs) {
            pSeek->packUpdate(gm, Projectile::InitialUpdate, bs);
        });
        checkPacketParity(actSeekInitPkt, expSeekInitPkt, "SeekingMissile::packUpdate");
        verifyBitStreamTruncationRollback(expSeekInitPkt, [&](BitStream& rbs) {
            SeekingMissile clientSeek;
            clientSeek.bindDataBlockManager(&dbm);
            return clientSeek.unpackUpdate(gm, rbs);
        });

        // 2f. Lightning & RepairEffect
        ProjectileDataType ltType{DataBlockManager::LightningDataType, 0};
        auto pLightProj = createProjectile(ltType);
        pLightProj->bindDataBlockManager(&dbm);
        auto* pLight = dynamic_cast<Lightning*>(pLightProj.get());
        CHECK(pLight != nullptr);
        CHECK(pLight->initResources(&lightningData));
        pLight->initProjectile(trans, Point3F{0.0f, 0.0f, 0.0f}, ~sim::SimObjectId{0});
        const auto expLightInitPkt = reader.readPacketRecord();
        const auto actLightInitPkt = captureBitStream([&](BitStream& bs) {
            pLight->packUpdate(gm, Projectile::InitialUpdate, bs);
        });
        checkPacketParity(actLightInitPkt, expLightInitPkt, "Lightning::packUpdate(Initial)");

        const auto expLightTargPkt = reader.readPacketRecord();
        const auto actLightTargPkt = captureBitStream([&](BitStream& bs) {
            pLight->packUpdate(gm, Lightning::TargetMask, bs);
        });
        checkPacketParity(actLightTargPkt, expLightTargPkt, "Lightning::packUpdate(Target)");
        verifyBitStreamTruncationRollback(expLightInitPkt, [&](BitStream& rbs) {
            Lightning clientLight;
            clientLight.bindDataBlockManager(&dbm);
            return clientLight.unpackUpdate(gm, rbs);
        });

        ProjectileDataType rpType{DataBlockManager::RepairEffectDataType, 0};
        auto pRepairProj = createProjectile(rpType);
        pRepairProj->bindDataBlockManager(&dbm);
        auto* pRepair = dynamic_cast<RepairEffect*>(pRepairProj.get());
        CHECK(pRepair != nullptr);
        CHECK(pRepair->initResources(&repairData));
        pRepair->initProjectile(trans, Point3F{0.0f, 0.0f, 0.0f}, ~sim::SimObjectId{0});
        const auto expRepairInitPkt = reader.readPacketRecord();
        const auto actRepairInitPkt = captureBitStream([&](BitStream& bs) {
            pRepair->packUpdate(gm, Projectile::InitialUpdate, bs);
        });
        checkPacketParity(actRepairInitPkt, expRepairInitPkt, "RepairEffect::packUpdate(Initial)");

        const auto expRepairTargPkt = reader.readPacketRecord();
        const auto actRepairTargPkt = captureBitStream([&](BitStream& bs) {
            pRepair->packUpdate(gm, Lightning::TargetMask, bs);
        });
        checkPacketParity(actRepairTargPkt, expRepairTargPkt, "RepairEffect::packUpdate(Target)");

        // 2g. SwooshTrail & SimpleSwooshTrail
        SwooshTrail swoosh(10.0f, 8, 1.0f);
        swoosh.updateTransform(trans, 1.0f);
        TMat3F trans2 = trans;
        trans2.p.y += 5.0f;
        swoosh.updateTransform(trans2, 1.2f);
        Box3F sBox{};
        const bool hasSBox = swoosh.getBoundingBox(sBox);
        const std::uint32_t expHasSBox = reader.readU32();
        const Box3F expSBox = reader.readBox3F();
        CHECK((hasSBox ? 1u : 0u) == expHasSBox);
        CHECK(nearBox(sBox, expSBox));

        SimpleSwooshTrail simpleSwoosh(12.0f, 0.8f);
        simpleSwoosh.updateTransform(trans, 1.0f);
        simpleSwoosh.updateTransform(trans2, 1.2f);
        Box3F ssBox{};
        const bool hasSSBox = simpleSwoosh.getBoundingBox(ssBox);
        const std::uint32_t expHasSSBox = reader.readU32();
        const Box3F expSSBox = reader.readBox3F();
        CHECK((hasSSBox ? 1u : 0u) == expHasSSBox);
        CHECK(nearBox(ssBox, expSSBox));
    }

    // =========================================================================
    // Section 3: Explosion, Debris, & DebrisCloud
    // =========================================================================
    {
        sim::network::GhostManager gm;
        Explosion exp(0);
        exp.bindDataBlockManager(&dbm);
        CHECK(exp.initResources(&expData));
        exp.setPosition(Point3F{15.0f, 25.0f, 35.0f});
        exp.setAxis(Point3F{0.0f, 0.0f, 1.0f});

        const auto expExpPkt1 = reader.readPacketRecord();
        const auto actExpPkt1 = captureBitStream([&](BitStream& bs) {
            exp.packUpdate(gm, GameBase::InitialUpdate, bs);
        });
        checkPacketParity(actExpPkt1, expExpPkt1, "Explosion::packUpdate(axis)");
        verifyBitStreamTruncationRollback(expExpPkt1, [&](BitStream& rbs) {
            Explosion clientExp;
            clientExp.bindDataBlockManager(&dbm);
            return clientExp.unpackUpdate(gm, rbs);
        });

        Explosion expDefaultAxis(0);
        expDefaultAxis.bindDataBlockManager(&dbm);
        CHECK(expDefaultAxis.initResources(&expData));
        expDefaultAxis.setPosition(Point3F{15.0f, 25.0f, 35.0f});
        const auto expExpPkt2 = reader.readPacketRecord();
        const auto actExpPkt2 = captureBitStream([&](BitStream& bs) {
            expDefaultAxis.packUpdate(gm, GameBase::InitialUpdate, bs);
        });
        checkPacketParity(actExpPkt2, expExpPkt2, "Explosion::packUpdate(defaultAxis)");

        Debris deb("debris.dts", 0, 3.5f, true, 50.0f, 2, 0.5f);
        deb.bindDataBlockManager(&dbm);
        CHECK(deb.initResources(&debrisData));
        deb.setLinearVelocity(Point3F{1.0f, 2.0f, 3.0f});
        const auto expDebPkt1 = reader.readPacketRecord();
        const auto actDebPkt1 = captureBitStream([&](BitStream& bs) {
            deb.packUpdate(gm, 1u, bs);
        });
        checkPacketParity(actDebPkt1, expDebPkt1, "Debris::packUpdate(netInit)");
        verifyBitStreamTruncationRollback(expDebPkt1, [&](BitStream& rbs) {
            Debris clientDeb;
            clientDeb.bindDataBlockManager(&dbm);
            return clientDeb.unpackUpdate(gm, rbs);
        });

        const auto expDebPkt2 = reader.readPacketRecord();
        const auto actDebPkt2 = captureBitStream([&](BitStream& bs) {
            deb.packUpdate(gm, 2u, bs);
        });
        checkPacketParity(actDebPkt2, expDebPkt2, "Debris::packUpdate(netUpdate)");

        DebrisCloud cloud(100.0f,
                          0xFF,
                          Point3F{10.0f, 20.0f, 30.0f},
                          2.5f,
                          Point3F{1.0f, 2.0f, 3.0f},
                          25.0f,
                          Point3F{0.0f, 0.0f, 1.0f},
                          0.5f,
                          Point3F{5.0f, 0.0f, 10.0f},
                          4.0f);
        cloud.bindDataBlockManager(&dbm);
        const auto expCloudPkt = reader.readPacketRecord();
        const auto actCloudPkt = captureBitStream([&](BitStream& bs) {
            cloud.packUpdate(gm, 1u, bs);
        });
        checkPacketParity(actCloudPkt, expCloudPkt, "DebrisCloud::packUpdate");
        verifyBitStreamTruncationRollback(expCloudPkt, [&](BitStream& rbs) {
            DebrisCloud clientCloud;
            clientCloud.bindDataBlockManager(&dbm);
            return clientCloud.unpackUpdate(gm, rbs);
        });
    }

    // =========================================================================
    // Section 4: Item & Mine Simulation, Ghost packUpdate, & Persistence
    // =========================================================================
    {
        sim::network::GhostManager gm;
        Item item;
        item.bindDataBlockManager(&dbm);
        CHECK(item.initResources(&itemData));
        item.setPos(Point3F{40.0f, 50.0f, 60.0f});
        item.setLinearVelocity(Point3F{2.0f, -3.0f, 5.0f});
        item.pop();

        const std::uint32_t expRot = reader.readU32();
        const std::int32_t expCount = reader.readI32();
        CHECK((item.isRotating() ? 1u : 0u) == expRot);
        CHECK(item.getCount() == expCount);

        const auto expItemInitPkt = reader.readPacketRecord();
        const auto actItemInitPkt = captureBitStream([&](BitStream& bs) {
            item.packUpdate(gm, GameBase::InitialUpdate, bs);
        });
        checkPacketParity(actItemInitPkt, expItemInitPkt, "Item::packUpdate(Initial)");

        const auto expItemMovePkt = reader.readPacketRecord();
        const auto actItemMovePkt = captureBitStream([&](BitStream& bs) {
            item.packUpdate(gm, Item::PositionMask | Item::RotationMask | Item::VelocityMask, bs);
        });
        checkPacketParity(actItemMovePkt, expItemMovePkt, "Item::packUpdate(Move)");
        verifyBitStreamTruncationRollback(expItemMovePkt, [&](BitStream& rbs) {
            Item clientItem;
            clientItem.bindDataBlockManager(&dbm);
            return clientItem.unpackUpdate(gm, rbs);
        });

        // Signed and multi-turn mission yaws must survive unsigned quantization.
        for (const Flt32 yaw : {-2.34512f, -6.283185307f, 0.0f, 13.5f}) {
            const auto originalRot = item.getRot();
            item.setRot({0.0f, 0.0f, yaw});
            const auto packet = captureBitStream([&](BitStream& bs) {
                item.packUpdate(gm, Item::RotationMask, bs);
            });
            Item clientItem;
            clientItem.bindDataBlockManager(&dbm);
            BitStream input(std::span<const std::byte>(packet.bytes));
            CHECK(clientItem.unpackUpdate(gm, input));
            CHECK(clientItem.getRot().z >= 0.0f);
            CHECK(clientItem.getRot().z <= 6.283186f);
            CHECK(std::abs(std::remainder(clientItem.getRot().z - yaw, 6.283185307f)) < 0.026f);
            item.setRot(originalRot);
        }

        Persistent::ClassRegistry persistReg;
        const auto expItemPersist = reader.readBytes();
        const auto actItemPersist = captureMemStream([&](MemRWStream& ms) {
            Persistent::StoreContext sctx(persistReg);
            CHECK(item.write(ms, 1, 0, sctx) == Persistent::Error::Ok);
        });
        CHECK(actItemPersist == expItemPersist);
        for (auto cut : std::views::iota(std::size_t{0}, expItemPersist.size())) {
            std::span<const std::byte> sub(expItemPersist.data(), cut);
            MemRWStream rms(sub);
            Item readItem;
            Persistent::LoadContext lctx(persistReg);
            CHECK(readItem.read(rms, 1, 0, lctx) != Persistent::Error::Ok);
            CHECK(rms.getPosition() == 0);
        }

        Mine mine;
        mine.bindDataBlockManager(&dbm);
        CHECK(mine.initResources(&mineData));
        mine.setPos(Point3F{40.0f, 50.0f, 60.0f});
        const auto expMineInitPkt = reader.readPacketRecord();
        const auto actMineInitPkt = captureBitStream([&](BitStream& bs) {
            mine.packUpdate(gm, GameBase::InitialUpdate, bs);
        });
        checkPacketParity(actMineInitPkt, expMineInitPkt, "Mine::packUpdate(Initial)");

        const auto expMinePersist = reader.readBytes();
        const auto actMinePersist = captureMemStream([&](MemRWStream& ms) {
            Persistent::StoreContext sctx(persistReg);
            CHECK(mine.write(ms, 1, 0, sctx) == Persistent::Error::Ok);
        });
        CHECK(actMinePersist == expMinePersist);
    }

    // =========================================================================
    // Section 5: Sensor, SensorManager, & TargetNameEvent
    // =========================================================================
    {
        Persistent::ClassRegistry persistReg;
        sim::network::GhostManager gm;
        Sensor sensor;
        sensor.bindDataBlockManager(&dbm);
        CHECK(sensor.initResources(&sensorData));
        sensor.setPos(Point3F{100.0f, 200.0f, 50.0f});

        const auto expSensorInitPkt = reader.readPacketRecord();
        const auto actSensorInitPkt = captureBitStream([&](BitStream& bs) {
            sensor.packUpdate(gm, GameBase::InitialUpdate, bs);
        });
        checkPacketParity(actSensorInitPkt, expSensorInitPkt, "Sensor::packUpdate(Initial)");

        const auto expSensorPersist = reader.readBytes();
        const auto actSensorPersist = captureMemStream([&](MemRWStream& ms) {
            Persistent::StoreContext sctx(persistReg);
            CHECK(sensor.write(ms, 1, 0, sctx) == Persistent::Error::Ok);
        });
        CHECK(actSensorPersist == expSensorPersist);

        sim::SimManager dummyMgr;
        auto netReg = std::make_shared<sim::network::Registry>();
        sim::network::PacketStream dummyPs(netReg);
        TargetNameEvent tne;
        tne.targId = 5;
        tne.name = "Base Generator";
        const auto expTnePkt = reader.readPacketRecord();
        const auto actTnePkt = captureBitStream([&](BitStream& bs) {
            CHECK(tne.pack(dummyMgr, dummyPs, bs));
        });
        checkPacketParity(actTnePkt, expTnePkt, "TargetNameEvent::pack");
        verifyBitStreamTruncationRollback(expTnePkt, [&](BitStream& rbs) {
            TargetNameEvent unpacked;
            return unpacked.unpack(dummyMgr, dummyPs, rbs);
        });
    }

    // =========================================================================
    // Section 6: Turret Aiming, Ghost packUpdate, Control Packet, & Persistence
    // =========================================================================
    {
        Persistent::ClassRegistry persistReg;
        sim::network::GhostManager gm;
        Turret turret;
        turret.bindDataBlockManager(&dbm);
        CHECK(turret.initResources(&turretData));
        turret.setElevationLimits(-0.6f, 1.2f);
        turret.setTurretElevation((1.2f + (-0.6f)) * 0.5f);
        turret.setPos(Point3F{0.0f, 0.0f, 10.0f});
        turret.setTeam(0);

        const float expRot = reader.readF32();
        const float expEle = reader.readF32();
        const std::uint32_t expDz1 = reader.readU32();
        const std::uint32_t expDz2 = reader.readU32();
        CHECK(nearEq(turret.rotation(10.0f, 20.0f), expRot));
        CHECK(nearEq(turret.elevation(10.0f, 20.0f, 5.0f), expEle));
        CHECK((turret.inDeadZone(0.5f, 0.2f) ? 1u : 0u) == expDz1);
        CHECK((turret.inDeadZone(0.5f, -1.5f) ? 1u : 0u) == expDz2);

        const auto expTurretInitPkt = reader.readPacketRecord();
        const auto actTurretInitPkt = captureBitStream([&](BitStream& bs) {
            turret.packUpdate(gm, GameBase::InitialUpdate, bs);
        });
        checkPacketParity(actTurretInitPkt, expTurretInitPkt, "Turret::packUpdate(Initial)");

        const auto expTurretUpdPkt = reader.readPacketRecord();
        const auto actTurretUpdPkt = captureBitStream([&](BitStream& bs) {
            turret.packUpdate(gm,
                              Turret::TRotationMask | Turret::ElevationMask | Turret::ShootingMask |
                                  Turret::StateMask,
                              bs);
        });
        checkPacketParity(actTurretUpdPkt, expTurretUpdPkt, "Turret::packUpdate(Delta)");
        verifyBitStreamTruncationRollback(expTurretUpdPkt, [&](BitStream& rbs) {
            Turret clientTurret;
            clientTurret.bindDataBlockManager(&dbm);
            return clientTurret.unpackUpdate(gm, rbs);
        });

        const auto expTurretCtrlPkt = reader.readPacketRecord();
        const auto actTurretCtrlPkt = captureBitStream([&](BitStream& bs) {
            turret.writePacketData(bs);
        });
        checkPacketParity(actTurretCtrlPkt, expTurretCtrlPkt, "Turret::writePacketData");
        verifyBitStreamTruncationRollback(expTurretCtrlPkt, [&](BitStream& rbs) {
            Turret clientTurret;
            clientTurret.bindDataBlockManager(&dbm);
            return clientTurret.readPacketData(rbs);
        });

        const auto expTurretPersist = reader.readBytes();
        const auto actTurretPersist = captureMemStream([&](MemRWStream& ms) {
            Persistent::StoreContext sctx(persistReg);
            CHECK(turret.write(ms, 1, 0, sctx) == Persistent::Error::Ok);
        });
        CHECK(actTurretPersist == expTurretPersist);
    }

    // =========================================================================
    // Section 7: Vehicle & Flier Physics, Control Packet, Ghost packUpdate, & Persistence
    // =========================================================================
    {
        Persistent::ClassRegistry persistReg;
        sim::network::GhostManager gm;
        Vehicle veh;
        veh.bindDataBlockManager(&dbm);
        CHECK(veh.initResources(&vehicleData));
        veh.setRot(Point3F{0.1f, -0.05f, 1.2f});
        veh.setPos(Point3F{50.0f, 60.0f, 70.0f});

        const auto expVehFullPkt = reader.readPacketRecord();
        const auto actVehFullPkt = captureBitStream([&](BitStream& bs) {
            veh.packUpdate(gm, 0xFFFFFFFFu, bs);
        });
        checkPacketParity(actVehFullPkt, expVehFullPkt, "Vehicle::packUpdate(Full)");

        const auto expVehDeltaPkt = reader.readPacketRecord();
        const auto actVehDeltaPkt = captureBitStream([&](BitStream& bs) {
            veh.packUpdate(gm,
                           Vehicle::OrientationMask | Vehicle::StatusMask | Vehicle::ShootingMask |
                               Vehicle::DamageMask | Vehicle::SoundMask,
                           bs);
        });
        checkPacketParity(actVehDeltaPkt, expVehDeltaPkt, "Vehicle::packUpdate(Delta)");
        verifyBitStreamTruncationRollback(expVehDeltaPkt, [&](BitStream& rbs) {
            Vehicle clientVeh;
            clientVeh.bindDataBlockManager(&dbm);
            clientVeh.initResources(&vehicleData);
            return clientVeh.unpackUpdate(gm, rbs);
        });

        const auto expVehCtrlPkt = reader.readPacketRecord();
        const auto actVehCtrlPkt = captureBitStream([&](BitStream& bs) {
            veh.writePacketData(bs);
        });
        checkPacketParity(actVehCtrlPkt, expVehCtrlPkt, "Vehicle::writePacketData");
        verifyBitStreamTruncationRollback(expVehCtrlPkt, [&](BitStream& rbs) {
            Vehicle clientVeh;
            clientVeh.bindDataBlockManager(&dbm);
            clientVeh.initResources(&vehicleData);
            return clientVeh.readPacketData(rbs);
        });

        const auto expVehPersist = reader.readBytes();
        const auto actVehPersist = captureMemStream([&](MemRWStream& ms) {
            Persistent::StoreContext sctx(persistReg);
            CHECK(veh.write(ms, 1, 0, sctx) == Persistent::Error::Ok);
        });
        CHECK(actVehPersist == expVehPersist);

        Flier flier;
        flier.bindDataBlockManager(&dbm);
        CHECK(flier.initResources(&flierData));
        flier.setRot(Point3F{0.1f, -0.05f, 1.2f});
        flier.setPos(Point3F{50.0f, 60.0f, 70.0f});

        PlayerMove pm{};
        pm.forwardAction = 1.0f;
        pm.pitch = 0.05f;
        pm.turnRot = -0.04f;
        pm.jetting = true;
        flier.updateMove(&pm, 0.032f);
        flier.setPos(flier.getPos());
        const Point3F expFlierRot = reader.readPoint3F();
        CHECK(nearPoint(flier.getRot(), expFlierRot));

        const auto expFlierFullPkt = reader.readPacketRecord();
        const auto actFlierFullPkt = captureBitStream([&](BitStream& bs) {
            flier.packUpdate(gm, 0xFFFFFFFFu, bs);
        });
        checkPacketParity(actFlierFullPkt, expFlierFullPkt, "Flier::packUpdate(Full)");

        const auto expFlierCtrlPkt = reader.readPacketRecord();
        const auto actFlierCtrlPkt = captureBitStream([&](BitStream& bs) {
            flier.writePacketData(bs);
        });
        checkPacketParity(actFlierCtrlPkt, expFlierCtrlPkt, "Flier::writePacketData");
        verifyBitStreamTruncationRollback(expFlierCtrlPkt, [&](BitStream& rbs) {
            Flier clientFlier;
            clientFlier.bindDataBlockManager(&dbm);
            clientFlier.initResources(&flierData);
            return clientFlier.readPacketData(rbs);
        });

        const auto expFlierPersist = reader.readBytes();
        const auto actFlierPersist = captureMemStream([&](MemRWStream& ms) {
            Persistent::StoreContext sctx(persistReg);
            CHECK(flier.write(ms, 1, 0, sctx) == Persistent::Error::Ok);
        });
        CHECK(actFlierPersist == expFlierPersist);
    }

    CHECK(reader.atEnd());
}

void testSimulationLifecyclesAndIntegration() {
    sim::SimManager serverMgr;
    auto rootContainer = std::make_unique<sim::SimDefaultOpenContainer>();
    rootContainer->setId(sim::RootContainerId);
    auto* pRoot = rootContainer.get();
    CHECK(serverMgr.addObject(std::move(rootContainer)) != nullptr);

    auto dbmPtr = std::make_unique<DataBlockManager>(true);
    auto* dbm = dbmPtr.get();
    dbm->setId(DataBlockManagerId);
    CHECK(serverMgr.addObject(std::move(dbmPtr)) != nullptr);

    auto pmPtr = std::make_unique<PlayerManager>(true);
    auto* pm = pmPtr.get();
    pm->setId(PlayerManagerId);
    CHECK(serverMgr.addObject(std::move(pmPtr)) != nullptr);
    pm->teamAdded(-1);
    pm->teamAdded(0);
    pm->teamAdded(1);

    auto sensorMgrPtr = std::make_unique<SensorManager>();
    auto* sensorMgr = sensorMgrPtr.get();
    sensorMgr->setId(SensorManagerId);
    CHECK(serverMgr.addObject(std::move(sensorMgrPtr)) != nullptr);

    auto smokeMgrPtr = std::make_unique<SmokeManager>();
    auto* smokeMgr = smokeMgrPtr.get();
    smokeMgr->setId(SmokeManagerId);
    CHECK(serverMgr.addObject(std::move(smokeMgrPtr)) != nullptr);

    // Populate datablocks
    auto& expData = createBlock<ExplosionData>(*dbm, DataBlockManager::ExplosionDataType, "Exp0");
    expData.hasLight = true;
    expData.lightRange = 10.0f;
    expData.timeZero = 0.25f;
    expData.timeOne = 0.75f;
    expData.colors[0] = ColorF{1.0f, 1.0f, 1.0f};
    expData.colors[1] = ColorF{1.0f, 0.5f, 0.0f};
    expData.colors[2] = ColorF{0.0f, 0.0f, 0.0f};
    expData.radFactors[0] = 0.5f;
    expData.radFactors[1] = 1.0f;
    expData.radFactors[2] = 0.0f;

    auto& rocketData = createBlock<RocketData>(*dbm, DataBlockManager::RocketDataType, "Rocket0");
    rocketData.muzzleVelocity = 50.0f;
    rocketData.terminalVelocity = 100.0f;
    rocketData.acceleration = 25.0f;
    rocketData.totalTime = 5.0f;
    rocketData.liveTime = 4.0f;
    rocketData.projSpecialTime = 1.0f;
    rocketData.damageClass = 1;
    rocketData.damageType = 2;
    rocketData.damageValue = 0.5f;
    rocketData.explosionRadius = 10.0f;
    rocketData.kickBackStrength = 100.0f;

    auto& seekData =
        createBlock<SeekingMissileData>(*dbm, DataBlockManager::SeekingMissileDataType, "Seek0");
    seekData.muzzleVelocity = 60.0f;
    seekData.terminalVelocity = 60.0f;
    seekData.totalTime = 8.0f;
    seekData.liveTime = 8.0f;
    seekData.seekingTurningRadius = 10.0f;
    seekData.nonSeekingTurningRadius = 40.0f;
    seekData.proximityDist = 3.0f;
    seekData.damageClass = 1;
    seekData.damageValue = 0.6f;
    seekData.explosionRadius = 8.0f;

    auto& lightningData =
        createBlock<LightningData>(*dbm, DataBlockManager::LightningDataType, "Light0");
    lightningData.boltLength = 30.0f;
    lightningData.coneAngle = 30.0f;
    lightningData.damagePerSec = 20.0f;
    lightningData.energyDrainPerSec = 10.0f;
    lightningData.totalTime = 2.0f;

    auto& repairData =
        createBlock<RepairEffectData>(*dbm, DataBlockManager::RepairEffectDataType, "Repair0");
    repairData.boltLength = 20.0f;
    repairData.damageValue = 0.5f;

    auto& itemData = createBlock<ItemData>(*dbm, DataBlockManager::ItemDataType, "Item0");
    itemData.description = "Health Kit";
    itemData.mass = 1.0f;
    itemData.maxDamage = 1.0f;

    auto& mineData = createBlock<MineData>(*dbm, DataBlockManager::MineDataType, "Mine0");
    mineData.description = "Anti-Personnel Mine";
    mineData.maxDamage = 0.5f;
    mineData.damageLevel.disabled = 0.5f;
    mineData.damageLevel.destroyed = 1.0f;
    mineData.damageValue = 0.8f;
    mineData.explosionRadius = 10.0f;
    mineData.kickBackStrength = 120.0f;

    auto& sensorData = createBlock<SensorData>(*dbm, DataBlockManager::SensorDataType, "Sensor0");
    sensorData.iRange = 150;
    sensorData.iDopplerVelocity = 0;
    sensorData.fCastLOS = false;
    sensorData.supression = false;
    sensorData.supressable = true;
    sensorData.pinger = true;
    sensorData.maxDamage = 1.0f;
    sensorData.damageLevel.disabled = 0.5f;
    sensorData.damageLevel.destroyed = 1.0f;

    auto& supSensorData =
        createBlock<SensorData>(*dbm, DataBlockManager::SensorDataType, "SensorSup");
    supSensorData.iRange = 50;
    supSensorData.supression = true;
    supSensorData.supressable = false;
    supSensorData.pinger = false;
    supSensorData.maxDamage = 1.0f;
    supSensorData.damageLevel.disabled = 0.5f;
    supSensorData.damageLevel.destroyed = 1.0f;

    auto& turretData = createBlock<TurretData>(*dbm, DataBlockManager::TurretDataType, "Turret0");
    turretData.iRange = 100;
    turretData.gunRange = 80.0f;
    turretData.speed = 3.0f;
    turretData.speedModifier = 0.5f;
    turretData.projectile = ProjectileDataType{DataBlockManager::RocketDataType, 0};
    turretData.reloadDelay = 0.5f;
    turretData.minGunEnergy = 10.0f;
    turretData.maxGunEnergy = 20.0f;
    turretData.maxEnergy = 100.0f;
    turretData.maxDamage = 2.0f;
    turretData.damageLevel.disabled = 0.5f;
    turretData.damageLevel.destroyed = 1.0f;
    turretData.FOV = 0.0f;

    auto& flierData = createBlock<FlierData>(*dbm, DataBlockManager::FlierDataType, "Flier0");
    flierData.maxDamage = 3.0f;
    flierData.damageLevel.disabled = 0.5f;
    flierData.damageLevel.destroyed = 1.0f;
    flierData.maxEnergy = 100.0f;
    flierData.minSpeed = 0.0f;
    flierData.maxSpeed = 40.0f;
    flierData.accel = 20.0f;
    flierData.maxBank = 0.5f;
    flierData.maxPitch = 0.7f;
    flierData.maxAlt = 150.0f;
    flierData.maxVertical = 12.0f;
    flierData.lift = 0.4f;
    flierData.projectile = ProjectileDataType{DataBlockManager::RocketDataType, 0};

    // 1. SmokeManager puff lifecycle & removeAllInRadius
    smokeMgr->addPuff(
        "smoke.dts", 1.0f, Point3F{10.0f, 10.0f, 10.0f}, Point3F{0.0f, 0.0f, 2.0f}, 1000);
    smokeMgr->addPuff(
        "smoke.dts", 1.0f, Point3F{100.0f, 100.0f, 100.0f}, Point3F{0.0f, 0.0f, 2.0f}, 1000);
    CHECK(smokeMgr->activePuffCount() == 2);
    smokeMgr->clientProcess(1200);
    CHECK(smokeMgr->activePuffCount() == 2);
    smokeMgr->removeAllInRadius(Point3F{10.0f, 10.0f, 10.0f}, 5.0f);
    CHECK(smokeMgr->activePuffCount() == 1);
    smokeMgr->clientProcess(2500);
    CHECK(smokeMgr->activePuffCount() == 0);

    // 2. PartDebrisCloud lifecycle
    std::vector<PartDebrisPiece> synthPieces(2);
    synthPieces[0].bounds = Box3F{Point3F{-0.5f, -0.5f, -0.5f}, Point3F{0.5f, 0.5f, 0.5f}};
    synthPieces[0].rootDeltaTransform.identity();
    synthPieces[1].bounds = Box3F{Point3F{-0.5f, -0.5f, -0.5f}, Point3F{0.5f, 0.5f, 0.5f}};
    synthPieces[1].rootDeltaTransform.identity();
    TMat3F partTrans{};
    partTrans.identity();
    auto partCloud = std::make_unique<PartDebrisCloud>(nullptr,
                                                       partTrans,
                                                       Point3F{0.0f, 0.0f, 10.0f},
                                                       0,
                                                       Point3F{0.0f, 0.0f, 5.0f},
                                                       10.0f,
                                                       Point3F{0.0f, 0.0f, 1.0f},
                                                       2.0f);
    partCloud->bindDataBlockManager(dbm);
    partCloud->setSyntheticPieces(std::move(synthPieces));
    auto* pPartCloud = partCloud.get();
    CHECK(serverMgr.addObject(std::move(partCloud)) != nullptr);
    CHECK(pPartCloud->debrisList().size() == 2);

    // 3. Sensor & SensorManager radar visibility, suppression, and target tracking
    auto sensorObj = std::make_unique<Sensor>();
    sensorObj->bindDataBlockManager(dbm);
    sensorObj->setDatFileId(0);
    sensorObj->initResources(&sensorData);
    sensorObj->setPos(Point3F{0.0f, 0.0f, 0.0f});
    auto* pSensor = sensorObj.get();
    CHECK(serverMgr.addObject(std::move(sensorObj)) != nullptr);
    pSensor->setTeam(0);
    pSensor->setActive(true);
    pSensor->forcePowerState(true);

    auto targetPlayerObj = std::make_unique<Player>();
    auto* pTargetPlayer = targetPlayerObj.get();
    CHECK(serverMgr.addObject(std::move(targetPlayerObj)) != nullptr);
    pTargetPlayer->setTeam(1);
    pTargetPlayer->setPos(Point3F{0.0f, 40.0f, 0.0f});
    pTargetPlayer->setBoundingBox(Box3F{Point3F{-1.0f, 39.0f, -1.0f}, Point3F{1.0f, 41.0f, 1.0f}});

    CHECK(pSensor->detectItem(pTargetPlayer));
    sensorMgr->testObject(pTargetPlayer);
    CHECK(pTargetPlayer->getVisibleToTeam(0));
    CHECK(pTargetPlayer->getSensorPinged() == GameBase::PingStatusOn);

    // Add suppression sensor near target player
    auto supSensorObj = std::make_unique<Sensor>();
    supSensorObj->bindDataBlockManager(dbm);
    supSensorObj->setDatFileId(1);
    supSensorObj->initResources(&supSensorData);
    supSensorObj->setPos(Point3F{0.0f, 35.0f, 0.0f});
    auto* pSupSensor = supSensorObj.get();
    CHECK(serverMgr.addObject(std::move(supSensorObj)) != nullptr);
    pSupSensor->setTeam(1);
    pSupSensor->setActive(true);
    pSupSensor->forcePowerState(true);

    sensorMgr->testObject(pTargetPlayer);
    CHECK(pTargetPlayer->getSensorPinged() == GameBase::PingStatusSupressed);

    const Int32 key = sensorMgr->addTarget(pSensor, "Alpha Sensor");
    CHECK(key >= 0);
    sensorMgr->changeTargetName(key, "Alpha Radar");
    CHECK(sensorMgr->target(key).name == "Alpha Radar");
    sensorMgr->removeTarget(key);
    CHECK(sensorMgr->target(key).obj == nullptr);

    // 4. Mine arming, surface normal sinkIn, and radius damage
    auto mineObj = std::make_unique<Mine>();
    mineObj->bindDataBlockManager(dbm);
    mineObj->setDatFileId(0);
    mineObj->initResources(&mineData);
    mineObj->setPos(Point3F{0.0f, 40.0f, 0.0f});
    mineObj->setGroundNormal(Point3F{0.0f, 0.5f, 0.8660254f});
    auto* pMine = mineObj.get();
    CHECK(serverMgr.addObject(std::move(mineObj)) != nullptr);
    pMine->sinkIn(0.032f);
    CHECK(!pMine->getRot().isZero());

    // 5. SeekingMissile homing towards target player
    auto seekObj = std::make_unique<SeekingMissile>(0);
    seekObj->bindDataBlockManager(dbm);
    seekObj->initResources(&seekData);
    TMat3F seekTrans{};
    seekTrans.identity();
    seekTrans.p.set(10.0f, 0.0f, 0.0f);
    seekObj->initProjectile(seekTrans, Point3F{0.0f, 0.0f, 0.0f}, ~sim::SimObjectId{0});
    auto* pSeek = seekObj.get();
    CHECK(serverMgr.addObject(std::move(seekObj)) != nullptr);
    pSeek->setTarget(pTargetPlayer);
    pSeek->updateTrailPercentage();
    CHECK(pSeek->currentTurnPercentage() >= 0.0f);

    // 6. Turret target tracking & firing
    auto turretObj = std::make_unique<Turret>();
    turretObj->bindDataBlockManager(dbm);
    turretObj->setDatFileId(0);
    turretObj->initResources(&turretData);
    turretObj->setPos(Point3F{0.0f, 0.0f, 0.0f});
    auto* pTurret = turretObj.get();
    CHECK(serverMgr.addObject(std::move(turretObj)) != nullptr);
    pTurret->setTeam(0);
    pTurret->setActive(true);
    pTurret->forcePowerState(true);
    pTurret->setEnergy(100.0f);
    pTurret->setPrimaryTarget(pTargetPlayer);
    pTurret->serverProcess(1100);
    CHECK(pTurret->turretState() == Turret::EXTENDING ||
          pTurret->turretState() == Turret::EXTENDED);

    // 7. Flier mount, control, and shoot
    auto flierObj = std::make_unique<Flier>();
    flierObj->bindDataBlockManager(dbm);
    flierObj->setDatFileId(0);
    flierObj->initResources(&flierData);
    flierObj->setPos(Point3F{0.0f, -20.0f, 10.0f});
    auto* pFlier = flierObj.get();
    CHECK(serverMgr.addObject(std::move(flierObj)) != nullptr);
    CHECK(pFlier->canMount(pTargetPlayer));
    CHECK(!pFlier->canRide(pTargetPlayer)); // No authored passenger nodes in this synthetic shape.
    pFlier->changePassengerCount(1);
    pFlier->setPlayerControl(true);
    PlayerMove flyMove{};
    flyMove.forwardAction = 1.0f;
    flyMove.jetting = true;
    flyMove.trigger = true;
    std::array<PlayerMove, 1> moves{flyMove};
    pFlier->serverUpdateMove(moves);
    CHECK(pFlier->speed() > 0.0f);

    // Unregister objects in reverse order of registration
    CHECK(serverMgr.unregisterObject(*pFlier) != nullptr);
    CHECK(serverMgr.unregisterObject(*pTurret) != nullptr);
    CHECK(serverMgr.unregisterObject(*pSeek) != nullptr);
    CHECK(serverMgr.unregisterObject(*pMine) != nullptr);
    CHECK(serverMgr.unregisterObject(*pSupSensor) != nullptr);
    CHECK(serverMgr.unregisterObject(*pTargetPlayer) != nullptr);
    CHECK(serverMgr.unregisterObject(*pSensor) != nullptr);
    CHECK(serverMgr.unregisterObject(*pPartCloud) != nullptr);
    CHECK(serverMgr.unregisterObject(*smokeMgr) != nullptr);
    CHECK(serverMgr.unregisterObject(*sensorMgr) != nullptr);
    CHECK(serverMgr.unregisterObject(*pm) != nullptr);
    CHECK(serverMgr.unregisterObject(*dbm) != nullptr);
    CHECK(serverMgr.unregisterObject(*pRoot) != nullptr);
}

} // namespace

int opent1_test_game_weapons() {
    try {
        testFixtureParity();
        testSimulationLifecyclesAndIntegration();
    } catch (const std::exception& ex) {
        std::fprintf(stderr, "EXCEPTION: %s\n", ex.what());
        return 1;
    }
    std::printf(
        "All L7-B04 game weapons, projectiles, items, turrets, and vehicles tests passed.\n");
    return 0;
}

#include "test_registry.hpp"
OPENT1_REGISTER_TEST("game", "game_weapons", opent1_test_game_weapons);
