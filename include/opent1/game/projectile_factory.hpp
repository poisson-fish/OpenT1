#pragma once

#include <opent1/game/data_block_manager.hpp>
#include <opent1/game/projectile.hpp>

#include <memory>

namespace opent1::game {

[[nodiscard]] std::unique_ptr<Projectile> createProjectile(const ProjectileDataType& projectile);

bool registerProjectileClasses(Persistent::ClassRegistry& registry);
bool registerProjectileNetwork(sim::network::Registry& registry);

} // namespace opent1::game
