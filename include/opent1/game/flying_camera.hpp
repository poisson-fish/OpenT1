#pragma once

#include <opent1/game/game_base.hpp>
#include <opent1/game/input_handler.hpp>
#include <opent1/game/sim_move_obj.hpp>
#include <opent1/sim/sim_collision_movement.hpp>
#include <opent1/sim/sim_editor_events.hpp>
#include <opent1/sim/sim_input_action.hpp>

#include <memory>
#include <span>
#include <string>
#include <string_view>

namespace opent1::game {

class FlyingCamera : public sim::SimMovement {
    using Parent = sim::SimMovement;

public:
    struct INPUT {
        struct AxisGroup {
            InputHandler x{};
            InputHandler y{};
            InputHandler z{};
        };

        AxisGroup linear{};
        AxisGroup angular{};

        void reset() noexcept {
            linear.x.reset();
            linear.y.reset();
            linear.z.reset();
            angular.x.reset();
            angular.y.reset();
            angular.z.reset();
        }
    };

    FlyingCamera();
    ~FlyingCamera() override;

    static void init(sim::SimManager* mgr);

    void reset();
    void activate(bool fFreeFly);
    void deactivate();
    void attachToGUI();
    void attachToObject(GameBase* obj);
    void nextObservable(Int32 iDirection);
    void setFreeFlyMode(bool fFreeFly);

    [[nodiscard]] Point3F findLOSPosition(const Point3F& pos, const Point3F& vec, Flt32 dist) const;
    [[nodiscard]] Flt32 getFarPlane() const;

    bool handleAction(Int32 iAction, Flt32 rValue, Int32 iDevice);
    void stepFrame(SimTime dt);

    [[nodiscard]] std::string_view guiFileName() const noexcept {
        return szGUIFileName_;
    }

    [[nodiscard]] std::string_view guiObjName() const noexcept {
        return szGUIObjName_;
    }

    [[nodiscard]] std::string_view saeName() const noexcept {
        return szSAEName_;
    }

    [[nodiscard]] bool isFreeFly() const noexcept {
        return fFreeFly_;
    }

    [[nodiscard]] bool isActivated() const noexcept {
        return fActivated_;
    }

    [[nodiscard]] bool isFocused() const noexcept {
        return fFocused_;
    }

    [[nodiscard]] Int32 heading() const noexcept {
        return iHeading_;
    }

    [[nodiscard]] Flt32 distance() const noexcept {
        return rDistance_;
    }

    [[nodiscard]] Flt32 pitch() const noexcept {
        return rPitch_;
    }

    [[nodiscard]] Flt32 speedScale() const noexcept {
        return rSpeedScale_;
    }

    [[nodiscard]] const Point3F& pos() const noexcept {
        return ptPos_;
    }

    [[nodiscard]] const EulerF& rot() const noexcept {
        return euRot_;
    }

    [[nodiscard]] const Point3F& maxLinearSpeed() const noexcept {
        return maxLinearSpeed_;
    }

    [[nodiscard]] const Point3F& maxAngularSpeed() const noexcept {
        return maxAngularSpeed_;
    }

    [[nodiscard]] GameBase* attachedObject() const noexcept {
        return pAttachedObj_;
    }

    [[nodiscard]] INPUT& input() noexcept {
        return input_;
    }

    [[nodiscard]] const INPUT& input() const noexcept {
        return input_;
    }

    bool processArguments(std::span<const std::string> args) override;
    bool onAdd() override;
    void onRemove() override;
    void onDeleteNotify(sim::SimObject& object) override;
    bool processEvent(const sim::SimEvent& event) override;
    bool processQuery(sim::SimQuery& query) override;

private:
    bool onSimGainFocusEvent(const SimGainFocusEvent& event);
    bool onSimLoseFocusEvent(const SimLoseFocusEvent& event);
    bool onSimObjectTransformEvent(const sim::SimObjectTransformEvent& event);
    bool onSimFrameEndNotifyEvent(const SimFrameEndNotifyEvent& event);
    bool onSimCameraMountEvent(const sim::SimCameraMountEvent& event);
    bool onSimCameraUnmountEvent(const sim::SimCameraUnmountEvent& event);
    bool onSimCameraQuery(sim::SimCameraQuery& query);
    bool onSimCollisionImageQuery(sim::SimCollisionImageQuery& query);

    std::string szGUIFileName_{};
    std::string szGUIObjName_{};
    std::string szSAEName_{"flycam.sae"};
    bool fFreeFly_{true};
    bool fActivated_{false};
    bool fFocused_{false};
    Int32 iHeading_{1};
    Flt32 rDistance_{26.0f};
    Flt32 rMaxDistance_{50.0f};
    Flt32 rMinDistance_{2.0f};
    Flt32 rPitch_{0.0f};
    Point3F ptPos_{0.0f, 0.0f, 0.0f};
    EulerF euRot_{0.0f, 0.0f, 0.0f};
    Point3F maxLinearSpeed_{50.0f, 50.0f, 50.0f};
    Point3F maxAngularSpeed_{
        static_cast<Flt32>(kRealDPi), static_cast<Flt32>(kRealDPi), static_cast<Flt32>(kRealDPi)};
    GameBase* pAttachedObj_{nullptr};
    INPUT input_{};
    Flt32 rSpeedScale_{1.0f};
    std::shared_ptr<const sim::SimActionMap> hActionMap_{};
    std::shared_ptr<sim::SimCollisionSphereImage> collisionImage_{};
};

} // namespace opent1::game
