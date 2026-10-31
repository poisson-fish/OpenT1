#pragma once
#include <opent1/world/ts_collision.hpp>
#include <opent1/sim/sim_collision_movement.hpp>

namespace opent1::world {
// Owns the posed geometry; safe after provider, instance and resource removal.
class SimTsCollisionImage : public sim::SimCollisionImage {
public:
    explicit SimTsCollisionImage(std::shared_ptr<const TsShapeCollision> geometry,
                                 TsCollisionLevel level = TsCollisionLevel::Faces,
                                 std::span<const TsMaterial> materials = {});
    bool collide(const SimCollisionImage&, CollisionSurfaceList&) const override;
    [[nodiscard]] std::optional<sim::SimCollisionMaterial> getMaterial(Int32) const override;
    [[nodiscard]] std::optional<sim::SimCollisionMaterial>
    getContactMaterial(const SimCollisionImage&, Int32) const override;

private:
    std::shared_ptr<const TsShapeCollision> geometry_;
    TsCollisionLevel level_;
    std::vector<std::optional<sim::SimCollisionMaterial>> materials_;
};
} // namespace opent1::world
