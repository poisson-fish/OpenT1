#include <opent1/world/ts_collision_sim.hpp>
#include <opent1/world/world_collision_sim.hpp>
#include <stdexcept>
#include <cmath>

namespace opent1::world {
SimTsCollisionImage::SimTsCollisionImage(std::shared_ptr<const TsShapeCollision> geometry,
                                         TsCollisionLevel level,
                                         std::span<const TsMaterial> materials)
    : SimCollisionImage(TSShape, TSShape), geometry_(std::move(geometry)), level_(level) {
    if (!geometry_ || materials.size() > 65536 ||
        (level != TsCollisionLevel::Parts && level != TsCollisionLevel::Faces)) {
        throw std::invalid_argument("invalid TS collision image");
    }
    moveable = true;
    for (const auto& material : materials) {
        if (material.useDefaultProperties) {
            materials_.push_back({});
            continue;
        }
        if (!std::isfinite(material.friction) || !std::isfinite(material.elasticity) ||
            material.friction < 0 || material.elasticity < 0 || material.friction > 1e12f ||
            material.elasticity > 1e12f) {
            throw std::invalid_argument("invalid TS collision material");
        }
        materials_.push_back(sim::SimCollisionMaterial{
            material.friction, material.elasticity, material.surfaceType});
    }
}

std::optional<sim::SimCollisionMaterial> SimTsCollisionImage::getMaterial(Int32 slot) const {
    if (slot == -1 || level_ == TsCollisionLevel::Parts || materials_.empty()) {
        return {};
    }
    if (slot < 0 || std::size_t(slot) >= materials_.size()) {
        throw std::invalid_argument("invalid TS material slot");
    }
    return materials_[std::size_t(slot)];
}

std::optional<sim::SimCollisionMaterial>
SimTsCollisionImage::getContactMaterial(const SimCollisionImage& other, Int32 slot) const {
    if (dynamic_cast<const SimTerrainCollisionImage*>(&other) ||
        dynamic_cast<const SimInteriorCollisionImage*>(&other)) {
        return other.getMaterial(slot);
    }
    return getMaterial(slot);
}

bool SimTsCollisionImage::collide(const SimCollisionImage& image,
                                  CollisionSurfaceList& list) const {
    if (!geometry_) {
        throw std::invalid_argument("missing TS collision snapshot");
    }
    const auto toThis = buildImageTransform(image);
    const auto point = [&](Point3F p) {
        Point3F result;
        m_mul(p, toThis, result);
        return result;
    };
    if (const auto* sphere = dynamic_cast<const sim::SimCollisionSphereImage*>(&image)) {
        return geometry_->sphere(point(sphere->center), sphere->radius, list, level_);
    }
    if (const auto* line = dynamic_cast<const sim::SimCollisionLineImage*>(&image)) {
        return geometry_->line(point(line->start), point(line->end), list, level_);
    }
    if (const auto* tube = dynamic_cast<const sim::SimCollisionTubeImage*>(&image)) {
        return geometry_->tube(point(tube->start), point(tube->end), tube->radius, list, level_);
    }
    if (const auto* box = dynamic_cast<const sim::SimCollisionBoxImage*>(&image)) {
        return geometry_->box(box->box, toThis, list);
    }
    if (const auto* shape = dynamic_cast<const SimTsCollisionImage*>(&image)) {
        if (!shape->geometry_) {
            throw std::invalid_argument("missing TS peer snapshot");
        }
        return geometry_->shape(*shape->geometry_, toThis, list);
    }
    if (const auto* terrain = dynamic_cast<const SimTerrainCollisionImage*>(&image);
        terrain && terrain->nativeGeometry()) {
        return terrain->nativeGeometry()->shape(
            geometry_->authoredCenter(), geometry_->authoredBounds().fMin.z, toThis, list);
    }
    auto toWorldGeometry = toThis;
    toWorldGeometry.inverse();
    CollisionSurfaceList result;
    bool hit = false;
    if (const auto* terrain = dynamic_cast<const SimTerrainCollisionImage*>(&image)) {
        if (!geometry_->bounds()) {
            return false;
        }
        hit = terrain->geometry().box(*geometry_->bounds(), toWorldGeometry, result);
    } else if (const auto* interior = dynamic_cast<const SimInteriorCollisionImage*>(&image)) {
        hit = interior->geometry().box(geometry_->authoredBounds(), toWorldGeometry, result);
    }
    if (!hit) {
        return false;
    }
    result.transform(toThis);
    // Native ITR support leaves stepVector in interior space.
    if (!dynamic_cast<const SimInteriorCollisionImage*>(&image)) {
        m_mul(result.stepVector, static_cast<const RMat3F&>(toThis), result.stepVector);
    }
    list.surfaces = std::move(result.surfaces);
    list.contained = result.contained;
    list.stepVector = result.stepVector;
    return true;
}
} // namespace opent1::world
