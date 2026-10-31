#pragma once

#include <opent1/game/sim_move_obj.hpp>

#include <memory>
#include <span>
#include <string>
#include <string_view>

namespace opent1::game {

class EditCamera : public sim::SimMovement {
    using Parent = sim::SimMovement;

public:
    static inline constexpr Flt32 MIN_INTERVAL = 0.033f;

    MoveLinear movement{};
    MoveLinear rotation{};
    SimTime lastUpdate{0.0f};

    EditCamera();
    ~EditCamera() override;

    void setActionMap(std::string_view fileName);

    void setActionMapResource(std::shared_ptr<const sim::SimActionMap> map) noexcept {
        actionMap_ = std::move(map);
    }

    [[nodiscard]] const Point3F& getPos() const noexcept {
        return getTransform().p;
    }

    void setPos(Point3F pos);
    void setPosition(Point3F pos, EulerF rot);
    using Parent::setPosition;

    void update(SimTime dt);
    [[nodiscard]] Flt32 getFarPlane() const;

    [[nodiscard]] const Point3F& throttle() const noexcept {
        return throttle_;
    }

    [[nodiscard]] const Point3F& desiredThrottle() const noexcept {
        return desiredThrottle_;
    }

    [[nodiscard]] bool hasFocus() const noexcept {
        return hasFocus_;
    }

    bool processArguments(std::span<const std::string> args) override;
    bool onAdd() override;
    void onRemove() override;
    bool processEvent(const sim::SimEvent& event) override;
    bool processQuery(sim::SimQuery& query) override;

private:
    bool onSimTimerEvent(const sim::SimTimerEvent& event);
    bool onSimFrameEndNotifyEvent(const SimFrameEndNotifyEvent& event);
    bool onSimCollisionEvent(const sim::SimMovementCollisionEvent& event);
    bool onSimGainFocusEvent(const SimGainFocusEvent& event);
    bool onSimLoseFocusEvent(const SimLoseFocusEvent& event);
    bool onSimObjectTransformEvent(const sim::SimObjectTransformEvent& event);
    bool onSimActionEvent(const sim::SimActionEvent& event);
    bool onSimCameraQuery(sim::SimCameraQuery& query);
    bool onSimObjectTransformQuery(sim::SimObjectTransformQuery& query);
    bool onSimCollisionImageQuery(sim::SimCollisionImageQuery& query);

    Point3F throttle_{0.0f, 0.0f, 0.0f};
    Point3F desiredThrottle_{0.0f, 0.0f, 0.0f};
    bool hasFocus_{true};
    Flt32 moveSpeed_{0.5f};
    Flt32 rotateSpeed_{0.2f};
    std::shared_ptr<const sim::SimActionMap> actionMap_{};
    std::shared_ptr<sim::SimCollisionSphereImage> collisionImage_{};
};

} // namespace opent1::game
