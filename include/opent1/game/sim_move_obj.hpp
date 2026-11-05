#pragma once
#include <opent1/core/res_manager.hpp>
#include <opent1/game/f_base.hpp>
#include <opent1/math/euler.hpp>
#include <opent1/math/point.hpp>
#include <opent1/sim/sim_collision_movement.hpp>
#include <opent1/sim/sim_editor_events.hpp>
#include <opent1/sim/sim_environment_camera.hpp>
#include <opent1/sim/sim_input_action.hpp>
#include <opent1/sim/sim_world_game.hpp>
#include <memory>
#include <span>
#include <string>
#include <string_view>

namespace opent1::game {

// Fear Action IDs from fear.strings.h
inline constexpr Int32 IDACTION_FIRE1 = 220001;
inline constexpr Int32 IDACTION_ROLL = 220002;
inline constexpr Int32 IDACTION_PITCH = 220003;
inline constexpr Int32 IDACTION_YAW = 220004;
inline constexpr Int32 IDACTION_STOP = 220005;
inline constexpr Int32 IDACTION_SETSPEED = 220006;
inline constexpr Int32 IDACTION_MOVELEFT = 220007;
inline constexpr Int32 IDACTION_MOVERIGHT = 220008;
inline constexpr Int32 IDACTION_MOVEBACK = 220009;
inline constexpr Int32 IDACTION_MOVEFORWARD = 220010;
inline constexpr Int32 IDACTION_MOVEUP = 220011;
inline constexpr Int32 IDACTION_MOVEDOWN = 220012;

// Legacy Darkstar focus / frame-end event types (simEvDcl.h)
inline constexpr Int32 SimLoseFocusEventType = sim::EventTypeFirst + 2;
inline constexpr Int32 SimGainFocusEventType = sim::EventTypeFirst + 3;
inline constexpr Int32 SimFrameEndNotifyEventType = sim::EventTypeFirst + 6;

using SimLoseFocusEvent = sim::SimLoseFocusEvent;
using SimGainFocusEvent = sim::SimGainFocusEvent;

struct SimFrameEndNotifyEvent : sim::SimEvent {
    SimFrameEndNotifyEvent() {
        type = SimFrameEndNotifyEventType;
    }

    sim::SimTime elapsedTime{0.0f};
};

struct MoveLinear {
    Point3F pos{0.0f, 0.0f, 0.0f};
    Point3F vel{0.0f, 0.0f, 0.0f};

    void update(sim::SimTime dt) noexcept {
        pos.x += dt * vel.x;
        pos.y += dt * vel.y;
        pos.z += dt * vel.z;
    }
};

class SimMoveObject final : public sim::SimMovement {
public:
    MoveLinear movement{};
    MoveLinear rotation{};
    sim::SimTime lastUpdate{0.0f};

    SimMoveObject();
    ~SimMoveObject() override;

    bool processArguments(std::span<const std::string> arguments) override;
    bool processArgumentsView(std::span<const std::string_view> arguments);
    bool onAdd() override;
    void onRemove() override;

    bool processEvent(const sim::SimEvent& event) override;
    bool processQuery(sim::SimQuery& query) override;

    void setActionMap(std::string_view fileName);
    void setActionMapResource(std::shared_ptr<const sim::SimActionMap> map);

    [[nodiscard]] const Point3F& getPos() const noexcept {
        return getTransform().p;
    }

    void setPos(Point3F pos);
    void setPosition(Point3F pos, EulerF rot);
    using sim::SimMovement::setPosition;
    void update(sim::SimTime dt);
    bool stepMovement(sim::SimTime dt);

    [[nodiscard]] Flt32 getFarPlane() const;

    [[nodiscard]] Point3F throttle() const noexcept {
        return throttle_;
    }

    [[nodiscard]] Point3F desiredThrottle() const noexcept {
        return desiredThrottle_;
    }

    [[nodiscard]] bool hasFocus() const noexcept {
        return hasFocus_;
    }

    [[nodiscard]] const std::shared_ptr<sim::SimCollisionSphereImage>&
    collisionImage() const noexcept {
        return collisionImage_;
    }

private:
    bool onSimTimerEvent(const sim::SimTimerEvent& event);
    bool onSimFrameEndNotifyEvent(const SimFrameEndNotifyEvent& event);
    bool onSimCollisionEvent(const sim::SimMovementCollisionEvent& event);
    bool onSimGainFocusEvent(const SimGainFocusEvent& event);
    bool onSimLoseFocusEvent(const SimLoseFocusEvent& event);
    bool onSimObjectTransformEvent(const sim::SimObjectTransformEvent& event);
    bool onSimActionEvent(const sim::SimActionEvent& event);

    bool onSimCameraQuery(sim::SimCameraQuery& query) const;
    bool onSimCollisionImageQuery(sim::SimCollisionImageQuery& query) const;
    bool onSimObjectTransformQuery(sim::SimObjectTransformQuery& query) const;

    Point3F throttle_{0.0f, 0.0f, 0.0f};
    Point3F desiredThrottle_{0.0f, 0.0f, 0.0f};
    std::shared_ptr<sim::SimCollisionSphereImage> collisionImage_;
    std::shared_ptr<const sim::SimActionMap> actionMap_;
    bool hasFocus_{false};
};

} // namespace opent1::game
