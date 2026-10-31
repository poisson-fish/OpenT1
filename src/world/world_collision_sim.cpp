#include <opent1/world/world_collision_sim.hpp>
#include <opent1/world/world_surfaces.hpp>
#include "collision_geometry.hpp"

namespace opent1::world {
namespace {
void validateMaterials(std::span<const sim::SimCollisionMaterial> materials) {
    if (materials.size() > 65536 || std::ranges::any_of(materials, [](const auto& m) {
            return !std::isfinite(m.friction) || !std::isfinite(m.elasticity) || m.friction < 0 ||
                   m.elasticity < 0 || m.friction > 1e12f || m.elasticity > 1e12f;
        })) {
        throw std::invalid_argument("invalid world collision materials");
    }
}

std::optional<sim::SimCollisionMaterial>
material(std::span<const sim::SimCollisionMaterial> materials, Int32 index) {
    if (index == -1 || materials.empty()) {
        return {};
    }
    if (index < 0 || std::size_t(index) >= materials.size()) {
        throw std::invalid_argument("invalid world collision material slot");
    }
    return materials[std::size_t(index)];
}

template <class Geometry>
bool collide(const Geometry& geometry,
             const sim::SimCollisionImage& image,
             const TMat3F& toThis,
             CollisionSurfaceList& list,
             bool editor = false) {
    using namespace sim;
    const auto point = [&](Point3F p) {
        return collision_detail::point(p, toThis);
    };
    if (const auto* sphere = dynamic_cast<const SimCollisionSphereImage*>(&image)) {
        return geometry.sphere(point(sphere->center), sphere->radius, list);
    }
    if (const auto* line = dynamic_cast<const SimCollisionLineImage*>(&image)) {
        if constexpr (std::is_same_v<Geometry, TerrainCollision> ||
                      std::is_same_v<Geometry, TerrainCollisionSnapshot>) {
            return geometry.line(point(line->start), point(line->end), list, editor);
        } else {
            return geometry.line(point(line->start), point(line->end), list);
        }
    }
    if (const auto* tube = dynamic_cast<const SimCollisionTubeImage*>(&image)) {
        return geometry.tube(point(tube->start), point(tube->end), tube->radius, list);
    }
    if (const auto* box = dynamic_cast<const SimCollisionBoxImage*>(&image)) {
        return geometry.box(box->box, toThis, list);
    }
    return false;
}
}

SimTerrainCollisionImage::SimTerrainCollisionImage(
    std::shared_ptr<const TerrainCollision> geometry,
    std::span<const sim::SimCollisionMaterial> materials)
    : SimCollisionImage(Terrain, Terrain), geometry_(std::move(geometry)) {
    if (!geometry_) {
        throw std::invalid_argument("missing terrain collision geometry");
    }
    validateMaterials(materials);
    materials_.assign(materials.begin(), materials.end());
}

bool SimTerrainCollisionImage::collide(const SimCollisionImage& image,
                                       CollisionSurfaceList& list) const {
    if (native_) {
        return world::collide(*native_, image, buildImageTransform(image), list, editor);
    }
    return world::collide(*geometry_, image, buildImageTransform(image), list, editor);
}

SimTerrainCollisionImage::SimTerrainCollisionImage(
    std::shared_ptr<const TerrainCollisionSnapshot> geometry,
    std::span<const sim::SimCollisionMaterial> materials)
    : SimCollisionImage(Terrain, Terrain), native_(std::move(geometry)) {
    if (!native_) {
        throw std::invalid_argument("missing native terrain collision geometry");
    }
    validateMaterials(materials);
    materials_.assign(materials.begin(), materials.end());
}

std::optional<sim::SimCollisionMaterial> SimTerrainCollisionImage::getMaterial(Int32 slot) const {
    return material(materials_, slot);
}

SimInteriorCollisionImage::SimInteriorCollisionImage(
    std::shared_ptr<const InteriorCollision> geometry,
    std::span<const sim::SimCollisionMaterial> materials)
    : SimCollisionImage(Interior, Interior), geometry_(std::move(geometry)) {
    if (!geometry_) {
        throw std::invalid_argument("missing interior collision geometry");
    }
    validateMaterials(materials);
    materials_.assign(materials.begin(), materials.end());
}

bool SimInteriorCollisionImage::collide(const SimCollisionImage& image,
                                        CollisionSurfaceList& list) const {
    return world::collide(*geometry_, image, buildImageTransform(image), list);
}

std::optional<sim::SimCollisionMaterial> SimInteriorCollisionImage::getMaterial(Int32 slot) const {
    return material(materials_, slot);
}

SimWorldCollider::SimWorldCollider(SimTerrainCollisionImage image, const TMat3F& placement) {
    if (!collision_detail::rigid(placement)) {
        throw std::invalid_argument("invalid terrain placement");
    }
    image.transform = placement;
    Box3F bounds;
    m_mul(image.nativeGeometry() ? image.nativeGeometry()->bounds() : image.geometry().bounds(),
          placement,
          bounds);
    if (!setBoundingBox(bounds)) {
        throw std::invalid_argument("invalid terrain bounds");
    }
    setType(sim::TerrainObjectType);
    image_ = std::make_shared<const SimTerrainCollisionImage>(std::move(image));
}

SimWorldCollider::SimWorldCollider(SimInteriorCollisionImage image,
                                   const Box3F& localBounds,
                                   const TMat3F& placement) {
    if (!collision_detail::valid(localBounds) || !collision_detail::rigid(placement)) {
        throw std::invalid_argument("invalid interior placement");
    }
    image.transform = placement;
    Box3F bounds;
    m_mul(localBounds, placement, bounds);
    if (!setBoundingBox(bounds)) {
        throw std::invalid_argument("invalid interior bounds");
    }
    setType(sim::InteriorObjectType);
    image_ = std::make_shared<const SimInteriorCollisionImage>(std::move(image));
}

bool SimWorldCollider::onAdd() {
    if (!SimContainer::onAdd()) {
        return false;
    }
    auto* root = dynamic_cast<sim::SimContainer*>(getManager()->findObject(sim::RootContainerId));
    return !root || root->addObject(*this);
}

bool SimWorldCollider::processQuery(sim::SimQuery& query) {
    auto* collision = dynamic_cast<sim::SimCollisionImageQuery*>(&query);
    if (!collision) {
        return SimContainer::processQuery(query);
    }
    if (const auto* terrain = dynamic_cast<const SimTerrainCollisionImage*>(image_.get())) {
        auto snapshot = std::make_shared<SimTerrainCollisionImage>(*terrain);
        snapshot->editor = collision->detail == sim::SimCollisionImageQuery::Editor;
        collision->images.push_back(std::move(snapshot));
    } else {
        collision->images.push_back(std::make_shared<SimInteriorCollisionImage>(
            *static_cast<const SimInteriorCollisionImage*>(image_.get())));
    }
    return true;
}

SimTerrainStreamCollider::SimTerrainStreamCollider(
    std::shared_ptr<TerrainResourceStream> stream,
    const TMat3F& placement,
    std::span<const sim::SimCollisionMaterial> materials)
    : stream_(std::move(stream)), placement_(placement) {
    if (!stream_ || !collision_detail::rigid(placement_)) {
        throw std::invalid_argument("invalid streamed terrain collider");
    }
    validateMaterials(materials);
    materials_.assign(materials.begin(), materials.end());
    const auto& grid = stream_->grid();
    const auto size = Flt32(1U << (grid.scale + grid.detailCount - 1));
    // Authored scratch bounds are unusable. Horizontal extent is topology;
    // vertical admission is deliberately conservative until DTBs are loaded.
    Box3F bounds;
    m_mul(Box3F{{0, 0, -1e8f}, {grid.size.x * size, grid.size.y * size, 1e8f}}, placement_, bounds);
    if (!setBoundingBox(bounds)) {
        throw std::invalid_argument("invalid streamed terrain bounds");
    }
    setType(sim::TerrainObjectType);
}

bool SimTerrainStreamCollider::onAdd() {
    if (!SimContainer::onAdd()) {
        return false;
    }
    auto* root = dynamic_cast<sim::SimContainer*>(getManager()->findObject(sim::RootContainerId));
    return !root || root->addObject(*this);
}

bool SimTerrainStreamCollider::processQuery(sim::SimQuery& query) {
    auto* collision = dynamic_cast<sim::SimCollisionImageQuery*>(&query);
    if (!collision) {
        return SimContainer::processQuery(query);
    }
    if (!collision->bounds || !collision_detail::valid(*collision->bounds)) {
        throw std::invalid_argument("streamed collision requires query bounds");
    }
    auto inverse = placement_;
    inverse.inverse();
    Box3F local;
    m_mul(*collision->bounds, inverse, local);
    auto geometry =
        stream_->prepareCollision({local.fMin.x, local.fMin.y, local.fMax.x, local.fMax.y});
    if (!geometry) {
        throw std::invalid_argument("terrain collision working set failed");
    }
    auto image = std::make_shared<SimTerrainCollisionImage>(std::move(geometry), materials_);
    image->transform = placement_;
    image->editor = collision->detail == sim::SimCollisionImageQuery::Editor;
    collision->images.push_back(std::move(image));
    return true;
}
} // namespace opent1::world
