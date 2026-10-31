#pragma once
#include <opent1/game/shape_collision.hpp>

namespace opent1::game {

class PlayerCollisionImage final : public ShapeCollisionImage {
public:
    Box3F crouchBox{};

    PlayerCollisionImage() noexcept;
    ~PlayerCollisionImage() override = default;

    bool collide(const SimCollisionImage& image, CollisionSurfaceList& list) const override;
};

} // namespace opent1::game
