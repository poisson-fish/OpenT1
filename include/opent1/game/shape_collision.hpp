#pragma once
#include <opent1/game/f_base.hpp>
#include <opent1/math/collision.hpp>
#include <opent1/sim/sim_collision_movement.hpp>
#include <opent1/world/ts_collision.hpp>
#include <opent1/world/ts_shape_instance.hpp>
#include <opent1/world/world_collision_sim.hpp>
#include <memory>

namespace opent1::game {

class ShapeCollisionImage : public sim::SimCollisionImage {
public:
    enum CollisionLevel : Int32 {
        CollideDepth = 0,
        CollideFaces = 1,
    };

    Int32 collisionDetail{-1};
    CollisionLevel collisionLevel{CollideFaces};
    SphereF sphere{};
    Box3F bbox{};
    TMat3F rootDeltaTransform{};

    ShapeCollisionImage() noexcept;
    ~ShapeCollisionImage() override = default;

    void setShapeInstance(world::TsShapeInstance* instance,
                          Int32 detail = -1,
                          std::shared_ptr<const world::TsShapeInstance> snapshot = {});

    void clearShapeInstance() noexcept {
        shapeInst_ = nullptr;
    }

    [[nodiscard]] const world::TsShapeInstance* shapeInstance() const noexcept {
        return shapeInst_ ? shapeInst_ : retainedShape_.get();
    }

    [[nodiscard]] world::TsShapeInstance* mutableShapeInstance() const noexcept {
        return shapeInst_;
    }

    [[nodiscard]] std::shared_ptr<const world::TsShapeInstance>
    sharedShapeInstance() const noexcept {
        return retainedShape_;
    }

    void
    setCollisionSnapshots(std::shared_ptr<const world::TsShapeCollision> detailCollision,
                          std::shared_ptr<const world::TsShapeCollision> lineDetail0Collision = {});
    void syncCollisionSnapshots() const;

    [[nodiscard]] std::shared_ptr<const world::TsShapeCollision> detailCollision() const;
    [[nodiscard]] std::shared_ptr<const world::TsShapeCollision> lineCollision() const;

    [[nodiscard]] world::TsCollisionLevel tsCollisionLevel() const noexcept {
        return collisionLevel == CollideFaces ? world::TsCollisionLevel::Faces
                                              : world::TsCollisionLevel::Parts;
    }

    bool checkShape(const ShapeCollisionImage& other, CollisionSurfaceList& list) const;
    bool collide(const SimCollisionImage& image, CollisionSurfaceList& list) const override;
    [[nodiscard]] std::optional<sim::SimCollisionMaterial> getMaterial(Int32 slot) const override;
    [[nodiscard]] std::optional<sim::SimCollisionMaterial>
    getContactMaterial(const SimCollisionImage& other, Int32 slot) const override;

protected:
    explicit ShapeCollisionImage(Type imageType, Int32 imagePriority) noexcept;
    [[nodiscard]] TMat3F buildShapeTransform(const sim::SimCollisionImage& image,
                                             CollisionSurfaceList& list) const;

    world::TsShapeInstance* shapeInst_{nullptr};
    std::shared_ptr<const world::TsShapeInstance> retainedShape_;
    mutable std::shared_ptr<const world::TsShapeCollision> detailCollision_;
    mutable std::shared_ptr<const world::TsShapeCollision> lineCollision_;
};

} // namespace opent1::game
