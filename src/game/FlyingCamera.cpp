#include <opent1/game/flying_camera.hpp>
#include <opent1/game/player.hpp>
#include <opent1/game/sky.hpp>
#include <opent1/game/string_table.hpp>
#include <opent1/world/world_mission.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>

namespace opent1::game {

FlyingCamera::FlyingCamera() {
    setType(SimCameraObjectType);
    setCollisionMask(SimTerrainObjectType | SimInteriorObjectType | StaticObjectType);
    setLocalBoundingBox({{0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.0f}});
    collisionImage_ = std::make_shared<sim::SimCollisionSphereImage>();
    collisionImage_->center.set(0.0f, 0.0f, 0.0f);
    collisionImage_->radius = 1.5f;
    reset();
}

FlyingCamera::~FlyingCamera() = default;

void FlyingCamera::init(sim::SimManager* mgr) {
    if (!mgr) {
        return;
    }
    if (!mgr->findObject("flyingCamera")) {
        auto cam = std::make_unique<FlyingCamera>();
        const std::array<std::string, 1> args{"play.gui"};
        cam->processArguments(args);
        auto* raw = cam.get();
        if (mgr->addObject(std::move(cam))) {
            raw->assignName("flyingCamera");
            raw->addToSet(SimCameraMountSetId);
        }
    }
}

void FlyingCamera::reset() {
    input_.reset();
    rDistance_ = (rMaxDistance_ + rMinDistance_) * 0.5f;
    rPitch_ = 0.0f;
    ptPos_.set(0.0f, 0.0f, 0.0f);
    euRot_.set(0.0f, 0.0f, 0.0f);
    if (pAttachedObj_) {
        clearNotify(*pAttachedObj_);
        pAttachedObj_ = nullptr;
    }
    fFreeFly_ = true;
    rSpeedScale_ = 1.0f;
}

bool FlyingCamera::processArguments(std::span<const std::string> args) {
    if (args.empty() || args.size() > 2) {
        return false;
    }
    szGUIFileName_ = args[0];

    std::string_view src = args[0];
    if (const auto slashPos = src.find_last_of("\\/"); slashPos != std::string_view::npos) {
        src.remove_prefix(slashPos + 1);
    }
    szGUIObjName_.clear();
    bool sawDot = false;
    bool nextUpper = false;
    for (const char ch : src) {
        if (!sawDot && ch == '.') {
            sawDot = true;
            nextUpper = true;
            continue;
        }
        if (nextUpper) {
            szGUIObjName_.push_back(
                static_cast<char>(std::toupper(static_cast<unsigned char>(ch))));
            nextUpper = false;
        } else {
            szGUIObjName_.push_back(
                static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
        }
    }

    if (args.size() >= 2 && !args[1].empty()) {
        szSAEName_ = args[1];
    } else {
        szSAEName_ = "flycam.sae";
    }
    if (auto* mgr = getManager()) {
        auto mapRes = sim::SimResource::loadByName<sim::SimActionMap>(*mgr, szSAEName_);
        if (mapRes) {
            hActionMap_ = std::make_shared<sim::SimActionMap>(*mapRes);
        }
    }
    return true;
}

bool FlyingCamera::onAdd() {
    if (!Parent::onAdd()) {
        return false;
    }
    if (auto* mgr = getManager()) {
        if (!hActionMap_ && !szSAEName_.empty()) {
            auto mapRes = sim::SimResource::loadByName<sim::SimActionMap>(*mgr, szSAEName_);
            if (mapRes) {
                hActionMap_ = std::make_shared<sim::SimActionMap>(*mapRes);
            }
        }
        if (auto* root = dynamic_cast<sim::SimContainer*>(mgr->findObject(SimRootContainerId))) {
            root->addObject(*this);
        }
    }
    return true;
}

void FlyingCamera::onRemove() {
    if (pAttachedObj_) {
        clearNotify(*pAttachedObj_);
        pAttachedObj_ = nullptr;
    }
    if (auto* cont = getContainer()) {
        cont->removeObject(*this);
    }
    Parent::onRemove();
}

void FlyingCamera::onDeleteNotify(sim::SimObject& object) {
    if (&object == pAttachedObj_) {
        pAttachedObj_ = nullptr;
        nextObservable(1);
    }
    Parent::onDeleteNotify(object);
}

void FlyingCamera::attachToGUI() {
    fActivated_ = true;
}

void FlyingCamera::activate(bool fFreeFly) {
    if (!fActivated_) {
        reset();
        fFreeFly_ = fFreeFly;
        if (!fFreeFly_) {
            nextObservable(1);
        }
        attachToGUI();
        if (getManager() != nullptr) {
            addToSet(SimFrameEndNotifySetId);
        }
        fFocused_ = true;
    }
}

void FlyingCamera::deactivate() {
    if (fActivated_) {
        if (getManager() != nullptr) {
            removeFromSet(SimFrameEndNotifySetId);
        }
        reset();
        fActivated_ = false;
        fFocused_ = false;
    }
}

void FlyingCamera::attachToObject(GameBase* obj) {
    if (pAttachedObj_) {
        clearNotify(*pAttachedObj_);
    }
    pAttachedObj_ = obj;
    if (pAttachedObj_) {
        deleteNotify(*pAttachedObj_);
        fFreeFly_ = false;
    }
}

void FlyingCamera::nextObservable(Int32 iDirection) {
    auto* mgr = getManager();
    if (!mgr) {
        return;
    }
    auto* set = dynamic_cast<sim::SimSet*>(mgr->findObject(PlayerSetId));
    if (!set) {
        set = dynamic_cast<sim::SimSet*>(mgr->findObject("Order/Players"));
    }
    if (!set || set->objects().empty()) {
        setFreeFlyMode(true);
        return;
    }

    const auto& objs = set->objects();
    const Int32 n = static_cast<Int32>(objs.size());
    Int32 startIdx = 0;
    for (Int32 i = 0; i < n; ++i) {
        if (objs[static_cast<std::size_t>(i)] == pAttachedObj_) {
            startIdx = i;
            break;
        }
    }
    for (Int32 step = 1; step <= n; ++step) {
        const Int32 idx = ((startIdx + step * (iDirection >= 0 ? 1 : -1)) % n + n) % n;
        if (auto* gb = dynamic_cast<GameBase*>(objs[static_cast<std::size_t>(idx)])) {
            attachToObject(gb);
            return;
        }
    }
    setFreeFlyMode(true);
}

void FlyingCamera::setFreeFlyMode(bool fFreeFly) {
    if (fFreeFly != fFreeFly_) {
        fFreeFly_ = fFreeFly;
        input_.reset();
        if (fFreeFly_) {
            if (pAttachedObj_) {
                clearNotify(*pAttachedObj_);
                pAttachedObj_ = nullptr;
            }
            const TMat3F& mat = getTransform();
            ptPos_ = mat.p;
            mat.angles(euRot_);
            euRot_.y = 0.0f;
        } else {
            rPitch_ = 0.0f;
            rDistance_ = (rMaxDistance_ + rMinDistance_) * 0.5f;
            if (!pAttachedObj_) {
                nextObservable(1);
            }
        }
    }
}

Point3F FlyingCamera::findLOSPosition(const Point3F& pos, const Point3F& vec, Flt32 dist) const {
    Point3F unitVec = vec;
    if (unitVec.lenSq() > 1e-12f) {
        unitVec.normalize();
    }
    const Point3F targetPos = pos + unitVec * dist;
    if (auto* mgr = getManager()) {
        if (auto* root = dynamic_cast<sim::SimContainer*>(mgr->findObject(SimRootContainerId))) {
            sim::SimContainerQuery cq{};
            cq.id = static_cast<sim::SimObjectId>(-1);
            cq.type = static_cast<UInt32>(-1);
            cq.mask = SimTerrainObjectType | SimInteriorObjectType | StaticObjectType;
            cq.box.fMin = pos;
            cq.box.fMax = targetPos;
            sim::SimCollisionInfo info{};
            if (root->findLOS(cq, info) && !info.surfaces.surfaces.empty()) {
                Point3F hitWorld{};
                m_mul(info.surfaces.surfaces[0].position, info.surfaces.tWorld, hitWorld);
                Point3F delta = hitWorld - pos;
                const Flt32 hitDist = std::max(0.0f, delta.len() - 1.0f);
                return pos + unitVec * hitDist;
            }
        }
    }
    return targetPos;
}

Flt32 FlyingCamera::getFarPlane() const {
    if (auto* mgr = getManager()) {
        if (const auto* sky = dynamic_cast<const Sky*>(mgr->findObject(TribesSkyId))) {
            return sky->distance() * 1.1f;
        }
        if (const auto* terr =
                dynamic_cast<const world::SimMissionTerrain*>(mgr->findObject(SimTerrainId))) {
            return terr->state().visibleDistance * 1.1f;
        }
    }
    return 10000.0f;
}

bool FlyingCamera::handleAction(Int32 iAction, Flt32 rValue, Int32 iDevice) {
    switch (iAction) {
        case IDACTION_MOVEUP:
            input_.linear.z.sample(rValue, iDevice);
            return true;
        case IDACTION_MOVEDOWN:
            input_.linear.z.sample(-rValue, iDevice);
            return true;
        case IDACTION_MOVELEFT:
            input_.linear.x.sample(-rValue, iDevice);
            return true;
        case IDACTION_MOVERIGHT:
            input_.linear.x.sample(rValue, iDevice);
            return true;
        case IDACTION_MOVEFORWARD:
            input_.linear.y.sample(rValue, iDevice);
            return true;
        case IDACTION_MOVEBACK:
            input_.linear.y.sample(-rValue, iDevice);
            return true;
        case IDACTION_PITCH:
            input_.angular.x.sample(rValue, iDevice);
            return true;
        case IDACTION_YAW:
            input_.angular.z.sample(rValue, iDevice);
            return true;
        case IDACTION_ROLL:
            input_.angular.y.sample(rValue, iDevice);
            return true;
        case IDACTION_SETSPEED:
            rSpeedScale_ = std::clamp(rValue, 0.05f, 10.0f);
            return true;
        case IDACTION_STOP:
            input_.reset();
            return true;
        case IDACTION_VIEW:
            if (!fFreeFly_) {
                iHeading_ = (iHeading_ + 1) % 4;
            }
            return true;
        case IDACTION_OBSERVER_NEXT:
            nextObservable(1);
            return true;
        case IDACTION_OBSERVER_PREV:
            nextObservable(-1);
            return true;
        case IDACTION_OBSERVER_TOGGLE:
            setFreeFlyMode(!fFreeFly_);
            return true;
        default:
            return false;
    }
}

void FlyingCamera::stepFrame(SimTime dt) {
    if (!fFocused_) {
        return;
    }

    const Flt32 lx = input_.linear.x.getInput(dt) * rSpeedScale_;
    const Flt32 ly = input_.linear.y.getInput(dt) * rSpeedScale_;
    const Flt32 lz = input_.linear.z.getInput(dt) * rSpeedScale_;
    const Flt32 ax = input_.angular.x.getInput(dt);
    const Flt32 ay = input_.angular.y.getInput(dt);
    const Flt32 az = input_.angular.z.getInput(dt);

    if (fFreeFly_ || !pAttachedObj_) {
        euRot_.x = std::clamp(euRot_.x + ax * dt,
                              static_cast<Flt32>(-kRealDPi * 0.5 + 0.01),
                              static_cast<Flt32>(kRealDPi * 0.5 - 0.01));
        euRot_.y += ay * dt;
        euRot_.z += az * dt;

        RMat3F rmat(euRot_);
        Point3F localDelta(lx * dt, ly * dt, lz * dt);
        Point3F worldDelta{};
        m_mul(localDelta, rmat, worldDelta);
        ptPos_ += worldDelta;

        TMat3F newMat(euRot_, ptPos_);
        setPosition(newMat, true);
    } else {
        rDistance_ = std::clamp(rDistance_ - ly * dt, rMinDistance_, rMaxDistance_);
        rPitch_ = std::clamp(rPitch_ + ax * dt, 0.0f, static_cast<Flt32>(kRealDPi * 0.5 - 0.05));

        const TMat3F& objMat = pAttachedObj_->getTransform();
        EulerF objAngles{};
        objMat.angles(objAngles);

        const Flt32 headingAngle =
            objAngles.z + static_cast<Flt32>(iHeading_) * static_cast<Flt32>(kRealDPi * 0.5);
        const EulerF camAngles(-rPitch_, 0.0f, headingAngle);
        const RMat3F camRot(camAngles);

        Point3F backVec(0.0f, -1.0f, 0.0f);
        Point3F worldBack{};
        m_mul(backVec, camRot, worldBack);

        ptPos_ = findLOSPosition(objMat.p, worldBack, rDistance_);
        euRot_ = camAngles;
        TMat3F newMat(euRot_, ptPos_);
        setPosition(newMat, true);
    }
}

bool FlyingCamera::onSimGainFocusEvent(const SimGainFocusEvent& /*event*/) {
    if (!fFocused_) {
        if (auto* mgr = getManager()) {
            addToSet(SimFrameEndNotifySetId);
            if (hActionMap_) {
                if (auto* ah =
                        dynamic_cast<sim::SimActionHandler*>(mgr->findObject(SimActionHandlerId))) {
                    ah->push(*this, hActionMap_);
                }
            }
        }
        fFocused_ = true;
    }
    return true;
}

bool FlyingCamera::onSimLoseFocusEvent(const SimLoseFocusEvent& /*event*/) {
    if (fFocused_) {
        if (auto* mgr = getManager()) {
            removeFromSet(SimFrameEndNotifySetId);
            if (hActionMap_) {
                if (auto* ah =
                        dynamic_cast<sim::SimActionHandler*>(mgr->findObject(SimActionHandlerId))) {
                    ah->pop(*this, hActionMap_.get());
                }
            }
        }
        fFocused_ = false;
    }
    return true;
}

bool FlyingCamera::onSimObjectTransformEvent(const sim::SimObjectTransformEvent& event) {
    ptPos_ = event.transform.p;
    event.transform.angles(euRot_);
    setPosition(event.transform, true);
    return true;
}

bool FlyingCamera::onSimFrameEndNotifyEvent(const SimFrameEndNotifyEvent& event) {
    stepFrame(event.elapsedTime > 0.0f ? event.elapsedTime : (1.0f / 30.0f));
    return true;
}

bool FlyingCamera::onSimCameraMountEvent(const sim::SimCameraMountEvent& /*event*/) {
    onSimGainFocusEvent(SimGainFocusEvent{});
    return true;
}

bool FlyingCamera::onSimCameraUnmountEvent(const sim::SimCameraUnmountEvent& /*event*/) {
    onSimLoseFocusEvent(SimLoseFocusEvent{});
    return true;
}

bool FlyingCamera::onSimCameraQuery(sim::SimCameraQuery& query) {
    query.transform = getTransform();
    query.nearPlane = 0.2f;
    query.farPlane = getFarPlane();
    query.fov = 0.6f;
    query.perspective = true;
    return true;
}

bool FlyingCamera::onSimCollisionImageQuery(sim::SimCollisionImageQuery& query) {
    if (collisionImage_) {
        collisionImage_->transform = getTransform();
        query.images.push_back(collisionImage_);
        return true;
    }
    return false;
}

bool FlyingCamera::processEvent(const sim::SimEvent& event) {
    if (const auto* actEv = dynamic_cast<const sim::SimActionEvent*>(&event)) {
        return handleAction(actEv->action, actEv->fValue, actEv->device);
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
    if (const auto* frameEv = dynamic_cast<const SimFrameEndNotifyEvent*>(&event)) {
        return onSimFrameEndNotifyEvent(*frameEv);
    }
    if (const auto* mountEv = dynamic_cast<const sim::SimCameraMountEvent*>(&event)) {
        return onSimCameraMountEvent(*mountEv);
    }
    if (const auto* unmountEv = dynamic_cast<const sim::SimCameraUnmountEvent*>(&event)) {
        return onSimCameraUnmountEvent(*unmountEv);
    }
    return Parent::processEvent(event);
}

bool FlyingCamera::processQuery(sim::SimQuery& query) {
    if (auto* cq = dynamic_cast<sim::SimCameraQuery*>(&query)) {
        return onSimCameraQuery(*cq);
    }
    if (auto* ciq = dynamic_cast<sim::SimCollisionImageQuery*>(&query)) {
        return onSimCollisionImageQuery(*ciq);
    }
    return Parent::processQuery(query);
}

} // namespace opent1::game
