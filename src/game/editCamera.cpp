#include <opent1/game/edit_camera.hpp>
#include <opent1/game/sky.hpp>
#include <opent1/game/string_table.hpp>
#include <opent1/world/world_mission.hpp>

#include <algorithm>
#include <charconv>

namespace opent1::game {

EditCamera::EditCamera() {
    setType(SimCameraObjectType);
    setCollisionMask(0);
    movement.pos.set(0.0f, 0.0f, 0.0f);
    movement.vel.set(0.0f, 0.0f, 0.0f);
    rotation.pos.set(-0.65f, 0.0f, 1.81f);
    rotation.vel.set(0.0f, 0.0f, 0.0f);
    collisionImage_ = std::make_shared<sim::SimCollisionSphereImage>();
    collisionImage_->center.set(0.0f, 0.0f, 0.0f);
    collisionImage_->radius = 0.6f;

    TMat3F mat(EulerF{rotation.pos.x, rotation.pos.y, rotation.pos.z}, movement.pos);
    Parent::setPosition(mat, true);
}

EditCamera::~EditCamera() = default;

void EditCamera::setActionMap(std::string_view fileName) {
    if (auto* mgr = getManager()) {
        auto map = loadActionMap ? loadActionMap(fileName) : nullptr;
        if (!map) {
            auto mapRes = sim::SimResource::loadByName<sim::SimActionMap>(*mgr, fileName);
            if (mapRes)
                map = std::make_shared<sim::SimActionMap>(*mapRes);
        }
        if (map) {
            if (actionMap_) {
                if (auto* ah =
                        dynamic_cast<sim::SimActionHandler*>(mgr->findObject(SimActionHandlerId))) {
                    ah->pop(*this, actionMap_.get());
                }
            }
            actionMap_ = std::move(map);
            if (hasFocus_) {
                if (auto* ah =
                        dynamic_cast<sim::SimActionHandler*>(mgr->findObject(SimActionHandlerId))) {
                    ah->push(*this, actionMap_);
                }
            }
        }
    }
}

bool EditCamera::processArguments(std::span<const std::string> args) {
    if (args.size() >= 1) {
        setActionMap(args[0]);
    }
    return true;
}

bool EditCamera::onAdd() {
    if (!Parent::onAdd()) {
        return false;
    }
    if (auto* mgr = getManager()) {
        lastUpdate = mgr->getCurrentTime();
        addToSet(SimCameraSetId);
        addToSet(SimFrameEndNotifySetId);
        if (auto* root = dynamic_cast<sim::SimContainer*>(mgr->findObject(SimRootContainerId))) {
            root->addObject(*this);
        }
    }
    setPosition(movement.pos, EulerF{rotation.pos.x, rotation.pos.y, rotation.pos.z});
    return true;
}

void EditCamera::onRemove() {
    if (auto* cont = getContainer()) {
        cont->removeObject(*this);
    }
    if (auto* mgr = getManager()) {
        removeFromSet(SimCameraSetId);
        removeFromSet(SimFrameEndNotifySetId);
        if (actionMap_) {
            if (auto* ah =
                    dynamic_cast<sim::SimActionHandler*>(mgr->findObject(SimActionHandlerId))) {
                ah->pop(*this, actionMap_.get());
            }
        }
    }
    Parent::onRemove();
}

void EditCamera::setPos(Point3F pos) {
    TMat3F mat = getTransform();
    mat.p = pos;
    Parent::setPosition(mat, true);
    movement.pos = pos;
}

void EditCamera::setPosition(Point3F pos, EulerF rot) {
    movement.pos = pos;
    rotation.pos.set(rot.x, rot.y, rot.z);
    TMat3F mat(rot, pos);
    Parent::setPosition(mat, true);
}

void EditCamera::update(SimTime dt) {
    if (dt < static_cast<SimTime>(0.033)) {
        dt = static_cast<SimTime>(0.033);
    }

    movement.update(dt);
    rotation.update(dt);
    TMat3F mat;
    mat.set(EulerF{rotation.pos.x, rotation.pos.y, rotation.pos.z}, movement.pos);

    Parent::setPosition(mat, true);
    movement.pos = getTransform().p;

    RMat3F mrot;
    mrot.set(EulerF{rotation.pos.x, rotation.pos.y, rotation.pos.z});
    const Point3F vel = throttle_;
    m_mul(vel, mrot, movement.vel);
    if (auto* mgr = getManager()) {
        lastUpdate = mgr->getCurrentTime();
    }
}

Flt32 EditCamera::getFarPlane() const {
    if (auto* mgr = getManager()) {
        if (const auto* sky = dynamic_cast<const Sky*>(mgr->findObject(TribesSkyId))) {
            return sky->distance() * 1.1f;
        }
        if (const auto* terr =
                dynamic_cast<const world::SimMissionTerrain*>(mgr->findObject(SimTerrainId))) {
            return terr->state().visibleDistance * 1.1f;
        }
    }
    return 65536.0f;
}

bool EditCamera::onSimTimerEvent(const sim::SimTimerEvent& event) {
    update(event.timerInterval);
    return true;
}

bool EditCamera::onSimFrameEndNotifyEvent(const SimFrameEndNotifyEvent& event) {
    if (auto* mgr = getManager()) {
        const SimTime now = mgr->getCurrentTime();
        const SimTime dt = (now > lastUpdate) ? (now - lastUpdate) : event.elapsedTime;
        lastUpdate = now;
        update(dt);
    } else {
        update(event.elapsedTime);
    }
    return true;
}

bool EditCamera::onSimCollisionEvent(const sim::SimMovementCollisionEvent& /*event*/) {
    return true;
}

bool EditCamera::onSimGainFocusEvent(const SimGainFocusEvent& /*event*/) {
    if (!hasFocus_) {
        if (auto* mgr = getManager()) {
            addToSet(SimFrameEndNotifySetId);
            if (actionMap_) {
                if (auto* ah =
                        dynamic_cast<sim::SimActionHandler*>(mgr->findObject(SimActionHandlerId))) {
                    ah->push(*this, actionMap_);
                }
            }
        }
        hasFocus_ = true;
    }
    return true;
}

bool EditCamera::onSimLoseFocusEvent(const SimLoseFocusEvent& /*event*/) {
    if (hasFocus_) {
        if (auto* mgr = getManager()) {
            removeFromSet(SimFrameEndNotifySetId);
            if (actionMap_) {
                if (auto* ah =
                        dynamic_cast<sim::SimActionHandler*>(mgr->findObject(SimActionHandlerId))) {
                    ah->pop(*this, actionMap_.get());
                }
            }
        }
        hasFocus_ = false;
    }
    return true;
}

bool EditCamera::onSimObjectTransformEvent(const sim::SimObjectTransformEvent& event) {
    EulerF rot{};
    event.transform.angles(rot);
    setPosition(event.transform.p, rot);
    return true;
}

bool EditCamera::onSimActionEvent(const sim::SimActionEvent& event) {
    Flt32 moveSpd = 2.0f;
    if (auto* mgr = getManager()) {
        if (auto* con = mgr->console()) {
            const auto msVar = con->getVariable("$ME::CameraMoveSpeed");
            if (!msVar.empty()) {
                std::from_chars(msVar.data(), msVar.data() + msVar.size(), moveSpd);
            }
        }
    }

    constexpr Flt32 kMaxPitch = 1.3962f;
    const Flt32 val = event.fValue;
    switch (event.action) {
        case IDACTION_MOVEFORWARD:
            desiredThrottle_.y = val;
            throttle_.y = val * moveSpd;
            return true;
        case IDACTION_MOVEBACK:
            desiredThrottle_.y = -val;
            throttle_.y = -val * moveSpd;
            return true;
        case IDACTION_MOVELEFT:
            desiredThrottle_.x = -val;
            throttle_.x = -val * moveSpd;
            return true;
        case IDACTION_MOVERIGHT:
            desiredThrottle_.x = val;
            throttle_.x = val * moveSpd;
            return true;
        case IDACTION_MOVEUP:
            desiredThrottle_.z = val;
            throttle_.z = val * moveSpd;
            return true;
        case IDACTION_MOVEDOWN:
            desiredThrottle_.z = -val;
            throttle_.z = -val * moveSpd;
            return true;
        case IDACTION_PITCH:
            rotation.pos.x += val;
            if (rotation.pos.x < -kMaxPitch) {
                rotation.pos.x = -kMaxPitch;
            }
            if (rotation.pos.x > kMaxPitch) {
                rotation.pos.x = kMaxPitch;
            }
            return true;
        case IDACTION_ROLL:
            rotation.pos.y += val;
            return true;
        case IDACTION_YAW:
            rotation.pos.z += val;
            return true;
        case IDACTION_ME_MOD1:
            if (auto* mgr = getManager()) {
                if (auto* con = mgr->console()) {
                    con->setVariable("$ME::Mod1", (val != 0.0f) ? "True" : "False");
                }
            }
            return true;
        case IDACTION_ME_MOD2:
            if (auto* mgr = getManager()) {
                if (auto* con = mgr->console()) {
                    con->setVariable("$ME::Mod2", (val != 0.0f) ? "True" : "False");
                }
            }
            return true;
        case IDACTION_ME_MOD3:
            if (auto* mgr = getManager()) {
                if (auto* con = mgr->console()) {
                    con->setVariable("$ME::Mod3", (val != 0.0f) ? "True" : "False");
                }
            }
            return true;
        default:
            return false;
    }
}

bool EditCamera::onSimCameraQuery(sim::SimCameraQuery& query) {
    query.transform = getTransform();
    query.nearPlane = 0.2f;
    query.farPlane = getFarPlane();
    query.fov = 0.6f;
    query.perspective = true;
    return true;
}

bool EditCamera::onSimObjectTransformQuery(sim::SimObjectTransformQuery& query) {
    query.transform = getTransform();
    return true;
}

bool EditCamera::onSimCollisionImageQuery(sim::SimCollisionImageQuery& query) {
    if (collisionImage_) {
        collisionImage_->transform = getTransform();
        query.images.push_back(collisionImage_);
        return true;
    }
    return false;
}

bool EditCamera::processEvent(const sim::SimEvent& event) {
    if (const auto* timerEv = dynamic_cast<const sim::SimTimerEvent*>(&event)) {
        return onSimTimerEvent(*timerEv);
    }
    if (const auto* frameEv = dynamic_cast<const SimFrameEndNotifyEvent*>(&event)) {
        return onSimFrameEndNotifyEvent(*frameEv);
    }
    if (const auto* colEv = dynamic_cast<const sim::SimMovementCollisionEvent*>(&event)) {
        return onSimCollisionEvent(*colEv);
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
    if (const auto* actEv = dynamic_cast<const sim::SimActionEvent*>(&event)) {
        return onSimActionEvent(*actEv);
    }
    return Parent::processEvent(event);
}

bool EditCamera::processQuery(sim::SimQuery& query) {
    if (auto* cq = dynamic_cast<sim::SimCameraQuery*>(&query)) {
        return onSimCameraQuery(*cq);
    }
    if (auto* xq = dynamic_cast<sim::SimObjectTransformQuery*>(&query)) {
        return onSimObjectTransformQuery(*xq);
    }
    if (auto* ciq = dynamic_cast<sim::SimCollisionImageQuery*>(&query)) {
        return onSimCollisionImageQuery(*ciq);
    }
    return Parent::processQuery(query);
}

} // namespace opent1::game
