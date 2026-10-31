#pragma once
#include <opent1/world/terrain_collision.hpp>
#include <opent1/world/terrain_collision_snapshot.hpp>
#include <opent1/world/interior_collision.hpp>
#include <opent1/sim/sim_collision_movement.hpp>

namespace opent1::world {
class SimTerrainCollisionImage : public sim::SimCollisionImage {
public:
    explicit SimTerrainCollisionImage(std::shared_ptr<const TerrainCollision>,
                                      std::span<const sim::SimCollisionMaterial> materials = {});
    explicit SimTerrainCollisionImage(std::shared_ptr<const TerrainCollisionSnapshot>,
                                      std::span<const sim::SimCollisionMaterial> materials = {});
    bool editor{}; // line queries only: include holes
    bool collide(const SimCollisionImage&, CollisionSurfaceList&) const override;
    [[nodiscard]] std::optional<sim::SimCollisionMaterial> getMaterial(Int32) const override;

    [[nodiscard]] const TerrainCollision& geometry() const {
        if (!geometry_) {
            throw std::logic_error("native terrain image uses nativeGeometry()");
        }
        return *geometry_;
    }

    [[nodiscard]] const TerrainCollisionSnapshot* nativeGeometry() const {
        return native_.get();
    }

private:
    std::shared_ptr<const TerrainCollision> geometry_;
    std::shared_ptr<const TerrainCollisionSnapshot> native_;
    std::vector<sim::SimCollisionMaterial> materials_;
};

class SimInteriorCollisionImage : public sim::SimCollisionImage {
public:
    explicit SimInteriorCollisionImage(std::shared_ptr<const InteriorCollision>,
                                       std::span<const sim::SimCollisionMaterial> materials = {});
    bool collide(const SimCollisionImage&, CollisionSurfaceList&) const override;
    [[nodiscard]] std::optional<sim::SimCollisionMaterial> getMaterial(Int32) const override;

    [[nodiscard]] const InteriorCollision& geometry() const {
        return *geometry_;
    }

private:
    std::shared_ptr<const InteriorCollision> geometry_;
    std::vector<sim::SimCollisionMaterial> materials_;
};

// Static spatial provider. Each query returns an independent image snapshot;
// retained images survive removal. Interior callers supply authored local bounds.
class SimWorldCollider : public sim::SimContainer {
public:
    SimWorldCollider(SimTerrainCollisionImage, const TMat3F& placement = {});
    SimWorldCollider(SimInteriorCollisionImage,
                     const Box3F& localBounds,
                     const TMat3F& placement = {});
    bool onAdd() override;
    bool processQuery(sim::SimQuery&) override;

private:
    std::shared_ptr<const sim::SimCollisionImage> image_;
};
class TerrainResourceStream;

// Shares one bounded stream with a SimTerrainSurface. Missing query bounds or a
// failed load rejects publication; existing images remain independent snapshots.
class SimTerrainStreamCollider : public sim::SimContainer {
public:
    SimTerrainStreamCollider(std::shared_ptr<TerrainResourceStream>,
                             const TMat3F& placement = {},
                             std::span<const sim::SimCollisionMaterial> materials = {});
    bool onAdd() override;
    bool processQuery(sim::SimQuery&) override;

private:
    std::shared_ptr<TerrainResourceStream> stream_;
    TMat3F placement_;
    std::vector<sim::SimCollisionMaterial> materials_;
};
} // namespace opent1::world
