#include <opent1/game/proj_bullet.hpp>
#include <opent1/game/proj_grenade.hpp>
#include <opent1/game/proj_laser.hpp>
#include <opent1/game/proj_lightning.hpp>
#include <opent1/game/proj_repair.hpp>
#include <opent1/game/proj_rocket.hpp>
#include <opent1/game/proj_seek_missile.hpp>
#include <opent1/game/proj_target_laser.hpp>
#include <opent1/game/projectile_factory.hpp>

namespace opent1::game {

std::unique_ptr<Projectile> createProjectile(const ProjectileDataType& projectile) {
    if (projectile.type == -1 || projectile.dataType == -1) {
        return nullptr;
    }

    switch (projectile.type) {
        case DataBlockManager::BulletDataType:
            return std::make_unique<Bullet>(projectile.dataType);
        case DataBlockManager::GrenadeDataType:
            return std::make_unique<Grenade>(projectile.dataType);
        case DataBlockManager::RocketDataType:
            return std::make_unique<RocketDumb>(projectile.dataType);
        case DataBlockManager::LaserDataType:
            return std::make_unique<LaserProjectile>(projectile.dataType);
        case DataBlockManager::TargetLaserDataType:
            return std::make_unique<TargetLaser>(projectile.dataType);
        case DataBlockManager::SeekingMissileDataType:
            return std::make_unique<SeekingMissile>(projectile.dataType);
        case DataBlockManager::LightningDataType:
            return std::make_unique<Lightning>(projectile.dataType);
        case DataBlockManager::RepairEffectDataType:
            return std::make_unique<RepairEffect>(projectile.dataType);
        default:
            return nullptr;
    }
}

bool registerProjectileClasses(Persistent::ClassRegistry& registry) {
    const bool ok1 = registry.registerClass<Bullet>("Bullet", BulletPersTag);
    const bool ok2 = registry.registerClass<Grenade>("Grenade", GrenadePersTag);
    const bool ok3 = registry.registerClass<RocketDumb>("RocketDumb", RocketPersTag);
    const bool ok4 = registry.registerClass<LaserProjectile>("LaserProjectile", LaserPersTag);
    const bool ok5 = registry.registerClass<TargetLaser>("TargetLaser", TargetLaserPersTag);
    const bool ok6 =
        registry.registerClass<SeekingMissile>("SeekingMissile", SeekingMissilePersTag);
    const bool ok7 = registry.registerClass<Lightning>("Lightning", LightningPersTag);
    const bool ok8 = registry.registerClass<RepairEffect>("RepairEffect", RepairEffectPersTag);
    return ok1 && ok2 && ok3 && ok4 && ok5 && ok6 && ok7 && ok8;
}

bool registerProjectileNetwork(sim::network::Registry& registry) {
    const bool ok1 = registry.registerGhost<Bullet>(static_cast<UInt16>(BulletPersTag));
    const bool ok2 = registry.registerGhost<Grenade>(static_cast<UInt16>(GrenadePersTag));
    const bool ok3 = registry.registerGhost<RocketDumb>(static_cast<UInt16>(RocketPersTag));
    const bool ok4 = registry.registerGhost<LaserProjectile>(static_cast<UInt16>(LaserPersTag));
    const bool ok5 = registry.registerGhost<TargetLaser>(static_cast<UInt16>(TargetLaserPersTag));
    const bool ok6 =
        registry.registerGhost<SeekingMissile>(static_cast<UInt16>(SeekingMissilePersTag));
    const bool ok7 = registry.registerGhost<Lightning>(static_cast<UInt16>(LightningPersTag));
    const bool ok8 = registry.registerGhost<RepairEffect>(static_cast<UInt16>(RepairEffectPersTag));
    return ok1 && ok2 && ok3 && ok4 && ok5 && ok6 && ok7 && ok8;
}

} // namespace opent1::game
