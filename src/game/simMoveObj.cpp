#include <opent1/game/sim_move_obj.hpp>
#include <opent1/world/scene_objects.hpp>
#include <charconv>
#include <numbers>
#include <vector>

namespace opent1::game {

namespace {
constexpr Flt32 kMaxSpeed = 1.0f;
constexpr Flt32 kRadius = 0.5f;
constexpr Flt32 kDefaultFarPlane = 100000.0f;

bool parseFloatArg(std::string_view text, Flt32& outValue) noexcept {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) {
        text.remove_prefix(1);
    }
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t')) {
        text.remove_suffix(1);
    }
    if (text.empty()) {
        return false;
    }
    const auto [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), outValue);
    return ec == std::errc{} && ptr == text.data() + text.size();
}
} // namespace

SimMoveObject::SimMoveObject() : collisionImage_(std::make_shared<sim::SimCollisionSphereImage>()) {
    lastUpdate = 0.0f;
    movement.pos.set(0.0f, 0.0f, 0.0f);
    movement.vel.set(0.0f, 0.0f, 0.0f);
    rotation.pos.set(-0.65f, 0.0f, 1.81f);
    rotation.vel.set(0.0f, 0.0f, 0.0f);
    throttle_.set(0.0f, 0.0f, 0.0f);
    desiredThrottle_.set(0.0f, 0.0f, 0.0f);

    setCollisionMask(0xFFFFFFFFu);
    collisionImage_->radius = kRadius;
    collisionImage_->center.set(0.0f, 0.0f, 0.0f);
    setLocalBoundingBox(Box3F{{-kRadius * 0.5f, -kRadius * 0.5f, -kRadius * 0.5f},
                              {+kRadius * 0.5f, +kRadius * 0.5f, +kRadius * 0.5f}});
}

SimMoveObject::~SimMoveObject() = default;

bool SimMoveObject::processArguments(std::span<const std::string> arguments) {
    std::vector<std::string_view> views;
    views.reserve(arguments.size());
    for (const auto& arg : arguments) {
        views.emplace_back(arg);
    }
    return processArgumentsView(views);
}

bool SimMoveObject::processArgumentsView(std::span<const std::string_view> arguments) {
    if (arguments.empty()) {
        setPosition(Point3F{0.0f, 0.0f, 0.0f}, EulerF{0.0f, 0.0f, 0.0f});
        return true;
    }
    if (arguments.size() == 3) {
        Point3F pos{};
        if (!parseFloatArg(arguments[0], pos.x) || !parseFloatArg(arguments[1], pos.y) ||
            !parseFloatArg(arguments[2], pos.z)) {
            return false;
        }
        setPosition(pos, EulerF{0.0f, 0.0f, 0.0f});
        return true;
    }
    if (arguments.size() == 6) {
        Point3F pos{};
        EulerF rot{};
        if (!parseFloatArg(arguments[0], pos.x) || !parseFloatArg(arguments[1], pos.y) ||
            !parseFloatArg(arguments[2], pos.z) || !parseFloatArg(arguments[3], rot.x) ||
            !parseFloatArg(arguments[4], rot.y) || !parseFloatArg(arguments[5], rot.z)) {
            return false;
        }
        setPosition(pos, rot);
        return true;
    }
    return false;
}

bool SimMoveObject::onAdd() {
    if (!SimMovement::onAdd()) {
        return false;
    }
    if (auto* mgr = getManager()) {
        lastUpdate = mgr->getCurrentTime();
    }
    addToSet(sim::CameraSetId);
    addToSet(sim::FrameEndNotifySetId);

    if (auto* mgr = getManager()) {
        if (auto* root = dynamic_cast<sim::SimContainer*>(mgr->findObject(sim::RootContainerId))) {
            root->addObject(*this);
        }
    }
    return true;
}

void SimMoveObject::onRemove() {
    if (hasFocus_ && actionMap_) {
        if (auto* mgr = getManager()) {
            if (auto* handler =
                    dynamic_cast<sim::SimActionHandler*>(mgr->findObject(sim::ActionHandlerId))) {
                handler->pop(*this, actionMap_.get());
            }
        }
        hasFocus_ = false;
    }
    SimMovement::onRemove();
}

void SimMoveObject::setActionMap(std::string_view fileName) {
    auto* mgr = getManager();
    if (!mgr) {
        return;
    }
    auto loaded = sim::SimResource::loadByName<sim::SimActionMap>(*mgr, fileName);
    if (!loaded) {
        return;
    }
    setActionMapResource(std::make_shared<const sim::SimActionMap>(*loaded));
}

void SimMoveObject::setActionMapResource(std::shared_ptr<const sim::SimActionMap> map) {
    if (hasFocus_ && actionMap_) {
        if (auto* mgr = getManager()) {
            if (auto* handler =
                    dynamic_cast<sim::SimActionHandler*>(mgr->findObject(sim::ActionHandlerId))) {
                handler->pop(*this, actionMap_.get());
            }
        }
    }
    actionMap_ = std::move(map);
    if (hasFocus_ && actionMap_) {
        if (auto* mgr = getManager()) {
            if (auto* handler =
                    dynamic_cast<sim::SimActionHandler*>(mgr->findObject(sim::ActionHandlerId))) {
                handler->push(*this, actionMap_);
            }
        }
    }
}

void SimMoveObject::setPos(Point3F pos) {
    TMat3F mat = getTransform();
    mat.p = pos;
    if (!pos.isZero()) {
        mat.flags |= RMat3F::Matrix_HasTranslation;
    }
    SimMovement::setPosition(mat);
    movement.pos = pos;
}

void SimMoveObject::setPosition(Point3F pos, EulerF rot) {
    rotation.pos.set(rot.x, rot.y, rot.z);
    movement.pos = pos;
    TMat3F mat(rot, pos);
    SimMovement::setPosition(mat);
    movement.pos = getTransform().p;
}

void SimMoveObject::update(sim::SimTime dt) {
    if (dt < 0.033f) {
        dt = 0.033f;
    }
    movement.update(dt);
    rotation.update(dt);
    TMat3F mat(EulerF{rotation.pos.x, rotation.pos.y, rotation.pos.z}, movement.pos);

    SimMovement::setPosition(mat);
    movement.pos = getTransform().p;

    RMat3F mrot(EulerF{rotation.pos.x, rotation.pos.y, rotation.pos.z});
    Point3F vel = throttle_;
    vel *= kMaxSpeed;
    m_mul(vel, mrot, movement.vel);
    throttle_ = desiredThrottle_;
    if (auto* mgr = getManager()) {
        lastUpdate = mgr->getCurrentTime();
    }
}

bool SimMoveObject::stepMovement(sim::SimTime dt) {
    update(dt);
    return true;
}

Flt32 SimMoveObject::getFarPlane() const {
    if (auto* mgr = getManager()) {
        if (const auto* place =
                dynamic_cast<const world::SimPlace*>(mgr->findObject(TribesSkyId))) {
            if (place->farDistance() > 0.0f && place->farDistance() < 1.0e9f) {
                return place->farDistance() * 1.1f;
            }
        }
        if (const auto* terrain =
                dynamic_cast<const world::SimMissionTerrain*>(mgr->findObject(sim::TerrainId))) {
            return terrain->state().visibleDistance * 1.1f;
        }
    }
    return kDefaultFarPlane;
}

bool SimMoveObject::onSimTimerEvent(const sim::SimTimerEvent& event) {
    update(event.timerInterval);
    return true;
}

bool SimMoveObject::onSimFrameEndNotifyEvent(const SimFrameEndNotifyEvent& event) {
    const sim::SimTime dt =
        getManager() ? (getManager()->getCurrentTime() - lastUpdate) : event.elapsedTime;
    update(dt);
    return true;
}

bool SimMoveObject::onSimCollisionEvent(const sim::SimMovementCollisionEvent& /*event*/) {
    return true;
}

bool SimMoveObject::onSimGainFocusEvent(const SimGainFocusEvent& /*event*/) {
    if (!hasFocus_ && actionMap_) {
        if (auto* mgr = getManager()) {
            if (auto* handler =
                    dynamic_cast<sim::SimActionHandler*>(mgr->findObject(sim::ActionHandlerId))) {
                handler->push(*this, actionMap_);
            }
        }
    }
    hasFocus_ = true;
    return true;
}

bool SimMoveObject::onSimLoseFocusEvent(const SimLoseFocusEvent& /*event*/) {
    if (hasFocus_ && actionMap_) {
        if (auto* mgr = getManager()) {
            if (auto* handler =
                    dynamic_cast<sim::SimActionHandler*>(mgr->findObject(sim::ActionHandlerId))) {
                handler->pop(*this, actionMap_.get());
            }
        }
    }
    hasFocus_ = false;
    return true;
}

bool SimMoveObject::onSimObjectTransformEvent(const sim::SimObjectTransformEvent& event) {
    setPos(event.transform.p);
    return true;
}

bool SimMoveObject::onSimActionEvent(const sim::SimActionEvent& event) {
    switch (event.action) {
        case IDACTION_SETSPEED:
            desiredThrottle_.y = event.fValue;
            break;
        case IDACTION_MOVEFORWARD:
            desiredThrottle_.y = event.fValue;
            desiredThrottle_.x = 0.0f;
            desiredThrottle_.z = 0.0f;
            break;
        case IDACTION_MOVEBACK:
            desiredThrottle_.y = -event.fValue;
            desiredThrottle_.x = 0.0f;
            desiredThrottle_.z = 0.0f;
            break;
        case IDACTION_MOVELEFT:
            desiredThrottle_.x = -event.fValue;
            desiredThrottle_.y = 0.0f;
            desiredThrottle_.z = 0.0f;
            break;
        case IDACTION_MOVERIGHT:
            desiredThrottle_.x = event.fValue;
            desiredThrottle_.y = 0.0f;
            desiredThrottle_.z = 0.0f;
            break;
        case IDACTION_MOVEUP:
            desiredThrottle_.z = event.fValue;
            desiredThrottle_.y = 0.0f;
            desiredThrottle_.x = 0.0f;
            break;
        case IDACTION_MOVEDOWN:
            desiredThrottle_.z = -event.fValue;
            desiredThrottle_.y = 0.0f;
            desiredThrottle_.x = 0.0f;
            break;
        case IDACTION_ROLL:
            rotation.vel.y = event.fValue;
            break;
        case IDACTION_PITCH:
            rotation.vel.x = event.fValue;
            break;
        case IDACTION_YAW:
            rotation.vel.z = event.fValue;
            break;
        default:
            break;
    }
    return true;
}

bool SimMoveObject::processEvent(const sim::SimEvent& event) {
    if (const auto* timerEv = dynamic_cast<const sim::SimTimerEvent*>(&event)) {
        return onSimTimerEvent(*timerEv);
    }
    if (const auto* frameEndEv = dynamic_cast<const SimFrameEndNotifyEvent*>(&event)) {
        return onSimFrameEndNotifyEvent(*frameEndEv);
    }
    if (const auto* collEv = dynamic_cast<const sim::SimMovementCollisionEvent*>(&event)) {
        return onSimCollisionEvent(*collEv);
    }
    if (const auto* gainEv = dynamic_cast<const SimGainFocusEvent*>(&event)) {
        return onSimGainFocusEvent(*gainEv);
    }
    if (const auto* loseEv = dynamic_cast<const SimLoseFocusEvent*>(&event)) {
        return onSimLoseFocusEvent(*loseEv);
    }
    if (const auto* xformEv = dynamic_cast<const sim::SimObjectTransformEvent*>(&event)) {
        return onSimObjectTransformEvent(*xformEv);
    }
    if (const auto* actionEv = dynamic_cast<const sim::SimActionEvent*>(&event)) {
        return onSimActionEvent(*actionEv);
    }
    return SimMovement::processEvent(event);
}

bool SimMoveObject::onSimCameraQuery(sim::SimCameraQuery& query) const {
    query.fov = 90.0f * std::numbers::pi_v<Flt32> / 360.0f;
    query.nearPlane = 0.1f;
    query.farPlane = getFarPlane();
    query.transform = getTransform();
    return true;
}

bool SimMoveObject::onSimCollisionImageQuery(sim::SimCollisionImageQuery& query) const {
    collisionImage_->transform = getTransform();
    query.images.push_back(collisionImage_);
    return true;
}

bool SimMoveObject::onSimObjectTransformQuery(sim::SimObjectTransformQuery& query) const {
    query.transform = getTransform();
    return true;
}

bool SimMoveObject::processQuery(sim::SimQuery& query) {
    if (auto* camQ = dynamic_cast<sim::SimCameraQuery*>(&query)) {
        return onSimCameraQuery(*camQ);
    }
    if (auto* collQ = dynamic_cast<sim::SimCollisionImageQuery*>(&query)) {
        return onSimCollisionImageQuery(*collQ);
    }
    if (auto* xformQ = dynamic_cast<sim::SimObjectTransformQuery*>(&query)) {
        return onSimObjectTransformQuery(*xformQ);
    }
    return SimMovement::processQuery(query);
}

} // namespace opent1::game
