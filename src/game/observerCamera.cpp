#include <opent1/game/observer_camera.hpp>
#include <opent1/game/player_manager.hpp>
#include <opent1/game/string_table.hpp>

#include <algorithm>
#include <cmath>
#include <format>

namespace opent1::game {

ObserverCamera::ObserverCamera(Int32 in_clientId) : clientId_(in_clientId) {
    setType(SimCameraObjectType);
    setCollisionMask(0);
    setNetFlags(Ghostable);
}

ObserverCamera::~ObserverCamera() = default;

void ObserverCamera::invokeObserverCallback(std::string_view fn) {
    callbackHistory_.push_back(ScriptCallbackRecord{std::string(fn), clientId_});
    if (auto* con = resolveConsole()) {
        if (con->isFunction(fn)) {
            const std::string cidStr = std::format("{}", clientId_);
            const std::array<std::string, 1> argv{cidStr};
            (void)con->execute(fn, argv);
        }
    }
}

void ObserverCamera::setFlyMode(bool canMove,
                                bool canRotate,
                                const Point3F& pos,
                                const EulerF& rot) {
    if (orbitObject_) {
        clearNotify(*orbitObject_);
        orbitObject_ = nullptr;
    }
    curOrbitPoint_ = pos;
    rotation_ = rot;
    setPosition(TMat3F(rot, pos), true);
    setLinearVelocity(Point3F{0.0f, 0.0f, 0.0f});
    if (canMove && canRotate) {
        mode_ = FlyMode;
    } else if (canRotate) {
        mode_ = FreeRotateMode;
    } else {
        mode_ = StationaryMode;
    }
    damageFlash_ = 0.0f;
    damageLevel_ = 0.0f;
    energyLevel_ = 0.0f;
}

void ObserverCamera::setOrbitMode(GameBase* obj,
                                  const Point3F& pos,
                                  const EulerF& rot,
                                  Flt32 minDist,
                                  Flt32 maxDist,
                                  Flt32 curDist) {
    if (obj != orbitObject_) {
        if (orbitObject_) {
            clearNotify(*orbitObject_);
        }
        orbitObject_ = obj;
        if (orbitObject_) {
            deleteNotify(*orbitObject_);
        }
    }
    if (orbitObject_) {
        mode_ = OrbitObjectMode;
        curOrbitPoint_ = orbitObject_->getBoxCenter();
    } else {
        mode_ = OrbitPointMode;
        curOrbitPoint_ = pos;
        damageFlash_ = 0.0f;
        damageLevel_ = 0.0f;
        energyLevel_ = 0.0f;
    }
    rotation_ = rot;
    minOrbitDist_ = minDist;
    maxOrbitDist_ = maxDist;
    orbitDist_ = curDist;
}

void ObserverCamera::onDeleteNotify(sim::SimObject& object) {
    if (&object == orbitObject_) {
        orbitObject_ = nullptr;
        mode_ = OrbitPointMode;
    }
    Parent::onDeleteNotify(object);
}

void ObserverCamera::update(UInt32 /*startTime*/, UInt32 tickEnd, const PlayerMove* pm) {
    const bool endMove = (tickEnd == 32U);
    const Flt32 dt = static_cast<Flt32>(static_cast<RealD>(tickEnd) * 0.001);
    const Flt32 endFrac = static_cast<Flt32>(tickEnd) / 32.0f;
    const Flt32 twoPi = static_cast<Flt32>(2.0 * kRealDPi);
    Flt32 orbitDist = orbitDist_;

    EulerF rot = rotation_;
    TMat3F tmat(rotation_, curOrbitPoint_);
    Point3F vel{0.0f, 0.0f, 0.0f};

    const Flt32 rMoveFore =
        pm ? (pm->forwardAction - pm->backwardAction) * (pm->jetting ? 30.0f : 10.0f) : 0.0f;
    const Flt32 rMoveSide =
        pm ? (pm->rightAction - pm->leftAction) * (pm->jetting ? 30.0f : 10.0f) : 0.0f;

    switch (mode_) {
        case FlyMode:
            m_mul(Point3F(rMoveSide, rMoveFore, 0.0f), static_cast<const RMat3F&>(tmat), vel);
            vel *= dt;
            tmat.p += vel;
            break;
        case OrbitObjectMode:
        case OrbitPointMode:
            orbitDist -= rMoveFore * dt;
            if (orbitDist < minOrbitDist_) {
                orbitDist = minOrbitDist_;
            } else if (orbitDist > maxOrbitDist_) {
                orbitDist = maxOrbitDist_;
            }
            break;
        default:
            break;
    }

    if (mode_ != StationaryMode && pm != nullptr) {
        rot.z += pm->turnRot * endFrac;
        rot.x += pm->pitch * endFrac;

        if (rot.z < 0.0f) {
            rot.z += twoPi;
        } else if (rot.z > twoPi) {
            rot.z -= twoPi;
        }
        if (rot.x > MaxPitch) {
            rot.x = MaxPitch;
        } else if (rot.x < -MaxPitch) {
            rot.x = -MaxPitch;
        }
    }

    if (mode_ == OrbitObjectMode || mode_ == OrbitPointMode) {
        if (mode_ == OrbitObjectMode && orbitObject_ != nullptr && orbitDist_ < 0.0f) {
            if (orbitDist_ == -1.0f) {
                setPosition(orbitObject_->getEyeTransform(), true);
            } else {
                TMat3F transform{};
                orbitObject_->getCameraTransform(-orbitDist_, &transform);
                setPosition(transform, true);
            }
        } else {
            if (mode_ == OrbitObjectMode && orbitObject_ != nullptr) {
                curOrbitPoint_ = orbitObject_->getBoxCenter();
                damageFlash_ = orbitObject_->getDamageFlash();
                damageLevel_ = orbitObject_->getDamageLevel();
                energyLevel_ = orbitObject_->getEnergyLevel();
            }
            TMat3F transform(rot, curOrbitPoint_);
            validateEyePoint(&transform, orbitDist);
            setPosition(transform, true);
        }
    } else {
        setPosition(TMat3F(rot, tmat.p), true);
    }

    if (endMove) {
        rotation_ = rot;
        if (mode_ != OrbitObjectMode && mode_ != OrbitPointMode) {
            curOrbitPoint_ = tmat.p;
        }
        orbitDist_ = orbitDist;
    }
}

void ObserverCamera::serverUpdateMove(std::span<const PlayerMove> moves) {
    for (const PlayerMove& pm : moves) {
        if (pm.trigger && !pmLast_.trigger) {
            invokeObserverCallback("Observer::triggerDown");
        } else if (!pm.trigger && pmLast_.trigger) {
            invokeObserverCallback("Observer::triggerUp");
        }
        if (pm.jumpAction) {
            invokeObserverCallback("Observer::jump");
        }
        update(0, 32, &pmLast_);
        pmLast_ = pm;
    }
}

void ObserverCamera::clientProcess(UInt32 curTime) {
    if (curTime < lastProcessTime_) {
        lastProcessTime_ = curTime;
    }
    auto* psc =
        getManager() ? dynamic_cast<PlayerPSC*>(getManager()->findObject(PlayerPSCId)) : nullptr;
    while (lastProcessTime_ < curTime) {
        const UInt32 endTick = std::min((lastProcessTime_ + 32) & ~UInt32{31}, curTime);
        if (const auto* move = psc && psc->getControlObject() == this
                                   ? psc->getClientMove(lastProcessTime_)
                                   : nullptr) {
            update(lastProcessTime_ & 31, ((endTick - 1) & 31) + 1, move);
        }
        lastProcessTime_ = endTick;
    }
}

const TMat3F& ObserverCamera::getEyeTransform() const {
    eyeTransformCache_ = getTransform();
    return eyeTransformCache_;
}

void ObserverCamera::getCameraTransform(Flt32 /*camDist*/, TMat3F* transform) const {
    if (transform) {
        *transform = getEyeTransform();
    }
}

Flt32 ObserverCamera::getDamageFlash() const noexcept {
    return damageFlash_;
}

Flt32 ObserverCamera::getEnergyLevel() const noexcept {
    return energyLevel_;
}

Flt32 ObserverCamera::getDamageLevel() const noexcept {
    return damageLevel_;
}

void ObserverCamera::setPlayerControl(bool /*ctrl*/) {}

void ObserverCamera::buildScopeAndCameraInfo(sim::network::GhostManager& cr,
                                             sim::SimNetObject::CameraInfo& camInfo) {
    if (mode_ == OrbitObjectMode && orbitObject_ && orbitObject_ != this) {
        orbitObject_->buildScopeAndCameraInfo(cr, camInfo);
    } else {
        Parent::buildScopeAndCameraInfo(cr, camInfo);
    }
}

void ObserverCamera::writePacketData(BitStream& bstream) const {
    writePacketData(bstream, nullptr);
}

void ObserverCamera::writePacketData(BitStream& bstream,
                                     const sim::network::GhostManager* ghosts) const {
    bstream.writeBytes(std::as_bytes(std::span(&rotation_, 1)));
    Int32 outMode = mode_;
    const Int32 ghostIndex =
        ghosts && orbitObject_ ? ghosts->getGhostIndex(*orbitObject_) : explicitGhostIndex_;
    if (outMode == OrbitObjectMode && (ghostIndex < 0 || ghostIndex == sim::InvalidGhostIndex)) {
        outMode = OrbitPointMode;
    }
    bstream.writeInt(outMode, 3);
    switch (outMode) {
        case StationaryMode:
        case FreeRotateMode:
        case FlyMode:
            bstream.writeBytes(std::as_bytes(std::span(&curOrbitPoint_, 1)));
            break;
        case OrbitPointMode:
            bstream.writeBytes(std::as_bytes(std::span(&curOrbitPoint_, 1)));
            bstream.write(minOrbitDist_);
            bstream.write(maxOrbitDist_);
            bstream.write(orbitDist_);
            break;
        case OrbitObjectMode:
            bstream.writeInt(ghostIndex, 10);
            bstream.write(minOrbitDist_);
            bstream.write(maxOrbitDist_);
            bstream.write(orbitDist_);
            break;
        default:
            break;
    }
}

bool ObserverCamera::readPacketData(BitStream& bstream) {
    return readPacketData(bstream, nullptr);
}

bool ObserverCamera::readPacketData(BitStream& bstream, sim::network::GhostManager* ghosts) {
    const auto savedPos = bstream.getCurPos();
    EulerF rot{};
    if (!bstream.readBytes(std::as_writable_bytes(std::span(&rot, 1)))) {
        bstream.setCurPos(savedPos);
        return false;
    }
    const Int32 newMode = bstream.readInt(3);
    if (!bstream.isValid()) {
        bstream.setCurPos(savedPos);
        return false;
    }

    switch (newMode) {
        case StationaryMode:
        case FreeRotateMode:
        case FlyMode: {
            Point3F pos{};
            if (!bstream.readBytes(std::as_writable_bytes(std::span(&pos, 1))) ||
                !bstream.isValid()) {
                bstream.setCurPos(savedPos);
                return false;
            }
            if (orbitObject_) {
                clearNotify(*orbitObject_);
                orbitObject_ = nullptr;
            }
            rotation_ = rot;
            mode_ = newMode;
            curOrbitPoint_ = pos;
            setPosition(TMat3F(rot, pos), true);
            break;
        }
        case OrbitPointMode: {
            Point3F orbitPt{};
            Flt32 minD = 0.0f;
            Flt32 maxD = 0.0f;
            Flt32 curD = 0.0f;
            if (!bstream.readBytes(std::as_writable_bytes(std::span(&orbitPt, 1))) ||
                !bstream.read(minD) || !bstream.read(maxD) || !bstream.read(curD) ||
                !bstream.isValid()) {
                bstream.setCurPos(savedPos);
                return false;
            }
            if (orbitObject_) {
                clearNotify(*orbitObject_);
                orbitObject_ = nullptr;
            }
            rotation_ = rot;
            mode_ = newMode;
            curOrbitPoint_ = orbitPt;
            minOrbitDist_ = minD;
            maxOrbitDist_ = maxD;
            orbitDist_ = curD;
            break;
        }
        case OrbitObjectMode: {
            const Int32 ghostId = bstream.readInt(10);
            Flt32 minD = 0.0f;
            Flt32 maxD = 0.0f;
            Flt32 curD = 0.0f;
            if (!bstream.read(minD) || !bstream.read(maxD) || !bstream.read(curD) ||
                !bstream.isValid()) {
                bstream.setCurPos(savedPos);
                return false;
            }
            explicitGhostIndex_ = ghostId;
            auto* target =
                ghosts ? dynamic_cast<GameBase*>(ghosts->resolveGhost(static_cast<UInt16>(ghostId)))
                       : nullptr;
            setOrbitMode(target, curOrbitPoint_, rot, minD, maxD, curD);
            break;
        }
        default:
            bstream.setCurPos(savedPos);
            return false;
    }
    return true;
}

UInt32
ObserverCamera::packUpdate(sim::network::GhostManager& gm, UInt32 mask, BitStream& stream) const {
    return Parent::packUpdate(gm, mask, stream);
}

bool ObserverCamera::unpackUpdate(sim::network::GhostManager& gm, BitStream& stream) {
    return Parent::unpackUpdate(gm, stream);
}

bool ObserverCamera::onAdd() {
    if (!Parent::onAdd()) {
        return false;
    }
    if (auto* mgr = getManager()) {
        addToSet(ObserverSetId);
        if (auto* root = dynamic_cast<sim::SimContainer*>(mgr->findObject(SimRootContainerId))) {
            root->addObject(*this);
        }
    }
    return true;
}

void ObserverCamera::onRemove() {
    if (orbitObject_) {
        clearNotify(*orbitObject_);
        orbitObject_ = nullptr;
    }
    if (auto* cont = getContainer()) {
        cont->removeObject(*this);
    }
    if (getManager() != nullptr) {
        removeFromSet(ObserverSetId);
    }
    Parent::onRemove();
}

bool ObserverCamera::processEvent(const sim::SimEvent& event) {
    return Parent::processEvent(event);
}

bool ObserverCamera::processQuery(sim::SimQuery& query) {
    return Parent::processQuery(query);
}

} // namespace opent1::game
