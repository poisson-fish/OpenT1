#pragma once
#include <opent1/sim/sim_collision_movement.hpp>
#include <opent1/sim/sim_environment_camera.hpp>

namespace opent1::sim {
inline constexpr SimObjectId MissionEditorId = 30;

struct SimObjectTransformEvent : SimEvent {
    SimObjectTransformEvent() {
        type = EventTypeFirst + 5;
    }

    TMat3F transform;
};

struct SimEditEvent : SimEvent {
    SimEditEvent() {
        type = EventTypeFirst + 25;
    }

    SimObjectId editor{MissionEditorId};
    bool state{};
};

struct SimTriggerEvent : SimEvent {
    SimTriggerEvent() {
        type = EventTypeFirst + 32;
    }

    bool activate{true};
    float value{1};
};

// Native camera query, expressed without a borrowed TS camera. fov is the
// horizontal half-angle; orthographic bounds use X right and Z up.
struct SimCameraQuery : SimQuery {
    SimCameraQuery() {
        type = EventTypeFirst;
    }

    TMat3F transform;
    float nearPlane{1}, farPlane{1.e7f}, fov{.6f};
    Point2F upperLeft{-320, 240}, lowerRight{320, -240};
    bool perspective{true};
};

// Generation-addressed native camera lifecycle payloads. A previous camera or
// data object can be removed by a callback without leaving a borrowed pointer.
struct SimCameraMountEvent : SimEvent {
    SimCameraMountEvent() {
        type = EventTypeFirst + 20;
    }

    SimAddress prevCamera, data;
};

struct SimCameraUnmountEvent : SimEvent {
    SimCameraUnmountEvent() {
        type = EventTypeFirst + 21;
    }
};

struct SimCameraRemountEvent : SimEvent {
    SimCameraRemountEvent() {
        type = EventTypeFirst + 38;
    }

    SimAddress data;
};

struct SimObjectTransformQuery : SimImageTransformQuery {
    SimObjectTransformQuery() {
        type = EventTypeFirst + 1;
    }
};
} // namespace opent1::sim
