#include <opent1/game/moveable.hpp>
#include <opent1/game/sim_move_obj.hpp>
#include <algorithm>
#include <cmath>

namespace opent1::game {

namespace {
constexpr UInt32 kBlockedTimeoutMs = 500;

struct BitStreamRollbackGuard {
    BitStream& stream;
    Int32 savedBitPos;
    bool committed{false};

    explicit BitStreamRollbackGuard(BitStream& s) noexcept
        : stream(s), savedBitPos(s.getCurPos()) {}

    ~BitStreamRollbackGuard() noexcept {
        if (!committed) {
            stream.setCurPos(savedBitPos);
        }
    }
};
} // namespace

// -----------------------------------------------------------------------------
// Moveable
// -----------------------------------------------------------------------------

Moveable::Moveable() {
    setType(MoveableObjectType | SimShapeObjectType | SimDefaultObjectType | SimInteriorObjectType |
            StaticObjectType);
    setCollisionMask(SimPlayerObjectType | SimVehicleObjectType | ItemObjectType);
}

Moveable::~Moveable() = default;

world::SimPathManager* Moveable::resolvePathManager() const noexcept {
    if (boundPathMgr_) {
        return boundPathMgr_;
    }
    if (auto* mgr = getManager()) {
        return dynamic_cast<world::SimPathManager*>(mgr->findObject(world::PathManagerId));
    }
    return nullptr;
}

bool Moveable::onAdd() {
    if (!Parent::onAdd()) {
        return false;
    }
    if (!isGhost()) {
        addToSet(MoveableSetId);
        setPathId(findPathId());
        setWaypointPosition(0);
    }
    return true;
}

void Moveable::onRemove() {
    runSound_ = 0;
    Parent::onRemove();
}

void Moveable::onGroupAdd() {
    Parent::onGroupAdd();
    if (!isGhost()) {
        setPathId(findPathId());
        setWaypointPosition(0);
    }
}

bool Moveable::initResources(GameBaseData* inDat) {
    if (!Parent::initResources(inDat)) {
        return false;
    }
    moveableData_ = dynamic_cast<MoveableData*>(getDatPtr());
    return moveableData_ != nullptr;
}

Int32 Moveable::findPathId() const {
    auto* group = getGroup();
    if (!group) {
        return UndefinedPathId;
    }
    for (auto* obj : group->objects()) {
        if (!obj) {
            continue;
        }
        world::PathQuery query{};
        if (obj->processQuery(query) && query.id >= 0 && query.id < UndefinedPathId) {
            return query.id;
        }
    }
    return UndefinedPathId;
}

void Moveable::setPathId(Int32 pid) {
    if (pid < UndefinedPathId && pid >= 0) {
        if (pid != pathId_) {
            pathId_ = pid;
            setMaskBits(static_cast<UInt32>(PathMask) | static_cast<UInt32>(NoInterpMask));
        }
    } else if (pathId_ != UndefinedPathId) {
        pathId_ = UndefinedPathId;
        setMaskBits(PathMask);
    }
    waypointCount_ = 0;
}

Int32 Moveable::getWaypointCount() noexcept {
    if (waypointCount_ == 0 && pathId_ != UndefinedPathId) {
        if (auto* pm = resolvePathManager()) {
            if (const auto sample = pm->waypoint(pathId_, 0)) {
                waypointCount_ = sample->count;
            }
        }
    }
    return waypointCount_;
}

bool Moveable::buildMovementVector(Int32 index, bool backward) {
    auto* pm = resolvePathManager();
    if (!pm || pathId_ == UndefinedPathId || index == UndefinedWaypoint) {
        return false;
    }

    const auto sample = pm->waypoint(pathId_, index);
    if (!sample) {
        return false;
    }
    waypointCount_ = sample->count;
    startPos_ = sample->position;
    startRot_ = sample->rotation;
    lVector_ = sample->linear;
    aVector_ = sample->angular;

    const Flt32 len = lVector_.len();
    const Flt32 speed =
        (moveableData_ && moveableData_->speed > 0.0f) ? moveableData_->speed : 1.0f;
    pathTime_ = (len > 0.0f) ? (len / speed) : 1.0f;
    ivScale_ = (len > 0.0f) ? (speed / len) : 1.0f;

    const Flt32 scale = backward ? -ivScale_ : ivScale_;
    setLinearVelocity(lVector_ * scale);
    setAngularVelocity(aVector_ * scale);
    return true;
}

bool Moveable::updatePosition(bool force) {
    if (currentWaypoint_ == UndefinedWaypoint) {
        return false;
    }
    const TMat3F oldPos = getTransform();
    const Point3F rot = startRot_ + aVector_ * position_;
    const Point3F pos = startPos_ + lVector_ * position_;
    TMat3F mat(EulerF{rot.x, rot.y, rot.z}, pos);

    const bool doDisplace = moveableData_ ? moveableData_->displace : true;
    const bool blockFwd = moveableData_ ? moveableData_->blockForward : true;
    const bool blockBwd = moveableData_ ? moveableData_->blockBackward : true;

    if (!force && !isGhost() &&
        (doDisplace || (moveState_ == ForwardState && blockFwd) ||
         (moveState_ == BackwardState && blockBwd))) {
        if (doDisplace) {
            SimMovement::setPosition(mat, true);
            if (!displaceObjects(oldPos)) {
                SimMovement::setPosition(oldPos, true);
                return false;
            }
            rotation_ = rot;
        } else {
            MovementInfo info{};
            if (!testPosition(mat, info)) {
                for (const auto& coll : info.collisionList) {
                    if (auto* mgr = getManager()) {
                        if (auto* obj = coll.object.resolve(*mgr)) {
                            if (processBlocker(*obj)) {
                                return false;
                            }
                        }
                    }
                }
            }
            setRot(rot);
            Parent::setPos(pos);
        }
    } else {
        setRot(rot);
        Parent::setPos(pos);
    }

    if (force) {
        setMaskBits(NoInterpMask);
    }
    return true;
}

bool Moveable::processBlocker(sim::SimObject& obj) {
    const bool blockFwd = moveableData_ ? moveableData_->blockForward : true;
    const bool blockBwd = moveableData_ ? moveableData_->blockBackward : true;
    if ((moveState_ == ForwardState && blockFwd) || (moveState_ == BackwardState && blockBwd)) {
        if (auto* gb = dynamic_cast<GameBase*>(&obj)) {
            if (gb->getType() & getCollisionMask()) {
                blocked_ = gb;
                return true;
            }
        }
    }
    return false;
}

void Moveable::updateMoveState(State newState) {
    if (newState != moveState_) {
        moveState_ = newState;
        if (!isGhost()) {
            setMaskBits(StateMask);
        }
    }
}

bool Moveable::setWaypoint(Flt32 wp) {
    currentWaypoint_ = static_cast<Int32>(wp);
    position_ = 0.0f;
    setMaskBits(PositionMask);
    return true;
}

bool Moveable::setWaypointPosition(Int32 wp) {
    if (wp >= 0 && wp < getWaypointCount()) {
        currentWaypoint_ = wp;
        waypoint_ = wp;
        position_ = 0.0f;
        buildMovementVector(currentWaypoint_, false);
        updatePosition(true);
        updateMoveState(StopState);
        return true;
    }
    return false;
}

bool Moveable::moveToWaypoint(Int32 wp) {
    if (state_ != Enabled || pathId_ == UndefinedPathId || currentWaypoint_ == UndefinedWaypoint ||
        wp < 0 || wp >= getWaypointCount()) {
        return false;
    }
    setMaskBits(StateMask);
    waypoint_ = wp;
    if (wp > currentWaypoint_) {
        return moveForward();
    }
    if (wp < currentWaypoint_ || position_ != 0.0f) {
        return moveBackward();
    }
    return false;
}

bool Moveable::moveForward() {
    if (state_ != Enabled || pathId_ == UndefinedPathId ||
        currentWaypoint_ >= getWaypointCount() - 1) {
        return false;
    }
    updateMoveState(ForwardState);
    if (waypoint_ <= currentWaypoint_) {
        waypoint_ = getWaypointCount() - 1;
    }
    if (!isGhost() && position_ == 0.0f && currentWaypoint_ == 0) {
        if (auto* con = resolveConsole()) {
            const std::string cb = scriptName("onFirst");
            if (!cb.empty()) {
                std::array<std::string, 1> argv{scriptThis()};
                (void)con->execute(cb, argv);
            }
        }
    }
    buildMovementVector(currentWaypoint_, false);
    return true;
}

bool Moveable::moveBackward() {
    if (state_ != Enabled || pathId_ == UndefinedPathId ||
        (currentWaypoint_ == 0 && position_ == 0.0f)) {
        return false;
    }
    updateMoveState(BackwardState);
    if (waypoint_ > currentWaypoint_) {
        waypoint_ = 0;
    }
    if (position_ == 0.0f) {
        if (!isGhost() && currentWaypoint_ == getWaypointCount() - 1) {
            if (auto* con = resolveConsole()) {
                const std::string cb = scriptName("onLast");
                if (!cb.empty()) {
                    std::array<std::string, 1> argv{scriptThis()};
                    (void)con->execute(cb, argv);
                }
            }
        }
        --currentWaypoint_;
        position_ = 1.0f;
    }
    buildMovementVector(currentWaypoint_, true);
    return true;
}

void Moveable::stop() {
    if (moveState_ == StopState) {
        return;
    }
    updateMoveState(StopState);
    setLinearVelocity(Point3F{0.0f, 0.0f, 0.0f});
    setAngularVelocity(Point3F{0.0f, 0.0f, 0.0f});
}

std::string_view Moveable::getMoveStateName() const noexcept {
    switch (moveState_) {
        case Undefined:
            return "Undefined";
        case ForwardState:
            return "MoveForward";
        case BackwardState:
            return "MoveBackward";
        case StopState:
            return "Stop";
        default:
            return "";
    }
}

void Moveable::serverProcess(UInt32 curTime) {
    const Flt32 dt = (curTime > lastProcessTime_)
                         ? static_cast<Flt32>(curTime - lastProcessTime_) * 0.001f
                         : 0.0f;
    Parent::serverProcess(curTime);

    if (state_ != Enabled && moveState_ != StopState) {
        stop();
    }

    if (moveState_ == ForwardState || moveState_ == BackwardState) {
        const Flt32 savedPos = position_;
        const Int32 savedWaypoint = currentWaypoint_;
        bool hitWaypoint = false;
        bool stopMove = false;
        Flt32 stepDt = dt;

        if (moveState_ == ForwardState) {
            while (stepDt > 0.0f && ivScale_ > 0.0f) {
                const Flt32 step = stepDt * ivScale_;
                if (position_ + step >= 1.0f) {
                    stepDt -= (1.0f - position_) / ivScale_;
                    ++currentWaypoint_;
                    position_ = 0.0f;
                    hitWaypoint = true;
                    if (currentWaypoint_ >= waypoint_ ||
                        currentWaypoint_ >= getWaypointCount() - 1) {
                        stopMove = true;
                        break;
                    }
                    if (!buildMovementVector(currentWaypoint_, false)) {
                        stopMove = true;
                        break;
                    }
                } else {
                    position_ += step;
                    break;
                }
            }
        } else {
            while (stepDt > 0.0f && ivScale_ > 0.0f) {
                const Flt32 step = stepDt * ivScale_;
                if (position_ - step <= 0.0f) {
                    stepDt -= position_ / ivScale_;
                    position_ = 0.0f;
                    hitWaypoint = true;
                    if (currentWaypoint_ <= waypoint_ || currentWaypoint_ <= 0) {
                        stopMove = true;
                        break;
                    }
                    --currentWaypoint_;
                    position_ = 1.0f;
                    if (!buildMovementVector(currentWaypoint_, true)) {
                        stopMove = true;
                        break;
                    }
                } else {
                    position_ -= step;
                    break;
                }
            }
        }

        blocked_ = nullptr;
        if (!updatePosition(false)) {
            if (currentWaypoint_ != savedWaypoint) {
                buildMovementVector(savedWaypoint, moveState_ == BackwardState);
            }
            position_ = savedPos;
            currentWaypoint_ = savedWaypoint;
            if (blocked_ && curTime >= lastBlockedTime_ + kBlockedTimeoutMs) {
                lastBlockedTime_ = curTime;
                if (auto* con = resolveConsole()) {
                    const std::string cb = scriptName("onBlocker");
                    if (!cb.empty()) {
                        std::array<std::string, 2> argv{scriptThis(), blocked_->scriptThis()};
                        (void)con->execute(cb, argv);
                    }
                }
            }
        } else {
            if (stopMove) {
                stop();
            }
            if (hitWaypoint) {
                if (auto* con = resolveConsole()) {
                    if (currentWaypoint_ == 0) {
                        const std::string cb = scriptName("onFirst");
                        if (!cb.empty()) {
                            std::array<std::string, 1> argv{scriptThis()};
                            (void)con->execute(cb, argv);
                        }
                    } else if (currentWaypoint_ == getWaypointCount() - 1) {
                        const std::string cb = scriptName("onLast");
                        if (!cb.empty()) {
                            std::array<std::string, 1> argv{scriptThis()};
                            (void)con->execute(cb, argv);
                        }
                    }
                    const std::string wpCb = scriptName("onWaypoint");
                    if (!wpCb.empty()) {
                        std::array<std::string, 1> argv{scriptThis()};
                        (void)con->execute(wpCb, argv);
                    }
                }
            }
        }
    }
}

void Moveable::clientProcess(UInt32 curTime) {
    const Flt32 dt = (curTime > lastProcessTime_)
                         ? static_cast<Flt32>(curTime - lastProcessTime_) * 0.001f
                         : 0.0f;
    Parent::clientProcess(curTime);

    if (moveState_ == ForwardState || moveState_ == BackwardState) {
        Flt32 stepDt = dt;
        bool stopMove = false;
        if (moveState_ == ForwardState) {
            while (stepDt > 0.0f && ivScale_ > 0.0f) {
                const Flt32 step = stepDt * ivScale_;
                if (position_ + step >= 1.0f) {
                    stepDt -= (1.0f - position_) / ivScale_;
                    ++currentWaypoint_;
                    position_ = 0.0f;
                    if (currentWaypoint_ == waypoint_) {
                        stopMove = true;
                        break;
                    }
                    buildMovementVector(currentWaypoint_, false);
                } else {
                    position_ += step;
                    break;
                }
            }
        } else {
            while (stepDt > 0.0f && ivScale_ > 0.0f) {
                const Flt32 step = stepDt * ivScale_;
                if (position_ - step <= 0.0f) {
                    stepDt -= position_ / ivScale_;
                    position_ = 0.0f;
                    if (currentWaypoint_ == waypoint_) {
                        stopMove = true;
                        break;
                    }
                    --currentWaypoint_;
                    position_ = 1.0f;
                    buildMovementVector(currentWaypoint_, true);
                } else {
                    position_ -= step;
                    break;
                }
            }
        }
        updatePosition(false);
        if (stopMove) {
            stop();
        }
    }
}

bool Moveable::processEvent(const sim::SimEvent& event) {
    if (const auto* editEv = dynamic_cast<const sim::SimEditEvent*>(&event)) {
        if (editEv->editor == sim::MissionEditorId) {
            if (!editEv->state) {
                setPathId(findPathId());
                if (currentWaypoint_ >= getWaypointCount()) {
                    setWaypointPosition(0);
                } else {
                    buildMovementVector(currentWaypoint_, moveState_ == BackwardState);
                    updatePosition(true);
                }
                setMaskBits(static_cast<UInt32>(StateMask) | static_cast<UInt32>(NoInterpMask));
            }
            return true;
        }
    }
    return Parent::processEvent(event);
}

bool Moveable::packMoveableUpdate(UInt32 mask, BitStream& stream, UInt32 currentTime) const {
    const UInt32 pmask = mask & ~(PositionMask | RotationMask | VelocityMask);
    if (!packStaticBaseUpdate(pmask, stream, currentTime)) {
        return false;
    }

    UInt32 effMask = mask;
    if ((effMask & InitialUpdate) != 0) {
        packDatFile(stream);
        if (pathId_ == UndefinedPathId) {
            effMask &= ~(static_cast<UInt32>(PathMask) | static_cast<UInt32>(PositionMask) |
                         static_cast<UInt32>(StateMask));
        }
    }

    if (stream.writeFlag((effMask & PathMask) != 0)) {
        stream.writeInt(pathId_, PathIdBits);
    }
    if (stream.writeFlag((effMask & PositionMask) != 0)) {
        stream.writeInt(currentWaypoint_, WaypointBits);
        stream.write(position_);
    }
    if (stream.writeFlag((effMask & StateMask) != 0)) {
        stream.writeFlag(isBlocked());
        stream.writeInt(waypoint_, WaypointBits);
        stream.writeInt(static_cast<Int32>(moveState_), 2);
    }
    return stream.isValid();
}

bool Moveable::unpackMoveableUpdate(BitStream& stream, UInt32 currentTime) {
    BitStreamRollbackGuard streamGuard(stream);
    const auto savedTeam = teamId_;
    const auto savedCtrl = ctrlClientId_;
    const auto savedOwner = ownerClientId_;
    const auto savedLastOwner = lastOwnerClientId_;
    const auto savedShieldVec = shieldVec_;
    const auto savedZOffset = zOffset_;
    const auto savedThreads = scriptThread_;
    const auto savedFading = m_fading_;
    const auto savedFadeStarted = m_fadeStarted_;
    const auto savedDamage = damageLevel_;
    const auto savedStaticState = state_;
    const auto savedBlown = blowedUpGood_;
    const auto savedIsTarget = isTarget_;
    const auto savedUnpackedStaticInit = hasUnpackedInitial_;
    const Int32 savedDatFileId = datFileId_;
    const Int32 savedPathId = pathId_;
    const State savedMoveState = moveState_;
    const Int32 savedCurWp = currentWaypoint_;
    const Int32 savedWp = waypoint_;
    const Flt32 savedPos = position_;
    const bool savedBlocked = blockedFlag_;
    const bool savedInit = hasUnpackedInitialMoveable_;
    const TMat3F savedTransform = getTransform();
    const Point3F savedRot = rotation_;

    if (!unpackStaticBaseUpdate(stream, currentTime)) {
        return false;
    }

    const auto rollback = [&]() noexcept {
        teamId_ = savedTeam;
        ctrlClientId_ = savedCtrl;
        ownerClientId_ = savedOwner;
        lastOwnerClientId_ = savedLastOwner;
        shieldVec_ = savedShieldVec;
        zOffset_ = savedZOffset;
        scriptThread_ = savedThreads;
        m_fading_ = savedFading;
        m_fadeStarted_ = savedFadeStarted;
        damageLevel_ = savedDamage;
        state_ = savedStaticState;
        blowedUpGood_ = savedBlown;
        setIsTarget(savedIsTarget);
        hasUnpackedInitial_ = savedUnpackedStaticInit;
        datFileId_ = savedDatFileId;
        pathId_ = savedPathId;
        moveState_ = savedMoveState;
        currentWaypoint_ = savedCurWp;
        waypoint_ = savedWp;
        position_ = savedPos;
        blockedFlag_ = savedBlocked;
        hasUnpackedInitialMoveable_ = savedInit;
        SimMovement::setPosition(savedTransform, true);
        rotation_ = savedRot;
        return false;
    };

    if (!hasUnpackedInitialMoveable_) {
        if (!unpackDatFile(stream)) {
            return rollback();
        }
        loadDatFile();
    }
    if (stream.readFlag()) {
        pathId_ = stream.readInt(PathIdBits);
        if (!stream.isValid()) {
            return rollback();
        }
    }
    if (stream.readFlag()) {
        currentWaypoint_ = stream.readInt(WaypointBits);
        if (!stream.read(position_) || !stream.isValid()) {
            return rollback();
        }
    }
    if (stream.readFlag()) {
        blockedFlag_ = stream.readFlag();
        waypoint_ = stream.readInt(WaypointBits);
        const auto ms = static_cast<State>(stream.readInt(2));
        if (!stream.isValid()) {
            return rollback();
        }
        if (getManager() || boundPathMgr_) {
            buildMovementVector(currentWaypoint_, ms == BackwardState);
            updatePosition(true);
            updateMoveState(ms);
        } else {
            moveState_ = ms;
        }
    }

    if (!stream.isValid()) {
        return rollback();
    }

    hasUnpackedInitialMoveable_ = true;
    streamGuard.committed = true;
    return true;
}

UInt32
Moveable::packUpdate(sim::network::GhostManager& /*gm*/, UInt32 mask, BitStream& stream) const {
    packMoveableUpdate(mask, stream);
    return 0;
}

bool Moveable::unpackUpdate(sim::network::GhostManager& /*gm*/, BitStream& stream) {
    return unpackMoveableUpdate(stream);
}

Persistent::Error
Moveable::read(StreamIO& sio, Int32 version, Int32 user, Persistent::LoadContext& ctx) {
    return Parent::read(sio, version, user, ctx);
}

Persistent::Error
Moveable::write(StreamIO& sio, Int32 version, Int32 user, Persistent::StoreContext& ctx) const {
    return Parent::write(sio, version, user, ctx);
}

// -----------------------------------------------------------------------------
// MoveableBaseData & MoveableBase
// -----------------------------------------------------------------------------

void MoveableBaseData::pack(BitStream& stream, DataBlockManager* dbm) const {
    StaticBaseData::pack(stream, dbm);
    stream.write(iStartSoundTag);
    stream.write(iStopSoundTag);
    stream.write(iRunSoundTag);
    stream.write(rForwardDelay);
    stream.write(rBackwardDelay);
    stream.write(iLinearVelocity);
    stream.write(rCrushDamage);
    stream.write(fInvincible);
}

bool MoveableBaseData::unpack(BitStream& stream, DataBlockManager* dbm) {
    const auto backup = *this;
    if (!StaticBaseData::unpack(stream, dbm)) {
        *this = backup;
        return false;
    }
    if (!stream.read(iStartSoundTag) || !stream.read(iStopSoundTag) || !stream.read(iRunSoundTag) ||
        !stream.read(rForwardDelay) || !stream.read(rBackwardDelay) ||
        !stream.read(iLinearVelocity) || !stream.read(rCrushDamage) || !stream.read(fInvincible) ||
        !stream.isValid()) {
        *this = backup;
        return false;
    }
    return true;
}

MoveableBase::MoveableBase() {
    setType(SimShapeObjectType | SimDefaultObjectType | MoveableBaseObjectType);
    setCollisionMask(SimPlayerObjectType | SimVehicleObjectType | ItemObjectType);
    if (collisionImage_) {
        collisionImage_->moveable = true;
    }
    setNetFlags(Ghostable);
}

MoveableBase::~MoveableBase() = default;

world::SimPathManager* MoveableBase::resolvePathManager() const noexcept {
    if (boundPathMgr_) {
        return boundPathMgr_;
    }
    if (auto* mgr = getManager()) {
        return dynamic_cast<world::SimPathManager*>(mgr->findObject(world::PathManagerId));
    }
    return nullptr;
}

bool MoveableBase::onAdd() {
    if (!Parent::onAdd()) {
        return false;
    }
    return true;
}

void MoveableBase::onGroupAdd() {
    Parent::onGroupAdd();
    if (!isGhost()) {
        if (auto* group = getGroup()) {
            for (auto* obj : group->objects()) {
                if (!obj) {
                    continue;
                }
                world::PathQuery query{};
                if (obj->processQuery(query) && query.id >= 0) {
                    iPathID_ = query.id;
                    iLastWaypoint_ = query.count - 1;
                    break;
                }
            }
        }
    }
}

bool MoveableBase::initResources(GameBaseData* inDat) {
    if (!Parent::initResources(inDat)) {
        return false;
    }
    mbData_ = dynamic_cast<MoveableBaseData*>(getDatPtr());
    return true;
}

void MoveableBase::move(MoveState newState, Int32 waypoint) {
    if (Parent::state_ == Disabled || Parent::state_ == Destroyed || iPathID_ == -1) {
        return;
    }

    switch (newState) {
        case STATE_STOP:
            if (state_ != STATE_STOP) {
                state_ = STATE_STOP;
                fSoundStop_ = true;
                fSoundRun_ = false;
                setMaskBits(NETMASK_UPDATE | NETMASK_SOUND);
                setLinearVelocity(Point3F{0.0f, 0.0f, 0.0f});
                setAngularVelocity(Point3F{0.0f, 0.0f, 0.0f});
            }
            break;
        case STATE_WAYPOINT:
            state_ = STATE_WAYPOINT;
            iTargetWaypoint_ = waypoint;
            fSoundStart_ = true;
            fSoundRun_ = true;
            setMaskBits(NETMASK_UPDATE | NETMASK_SOUND);
            break;
        case STATE_FORWARD:
            state_ = STATE_FORWARD;
            fSoundStart_ = true;
            fSoundRun_ = true;
            setMaskBits(NETMASK_UPDATE | NETMASK_SOUND);
            break;
        case STATE_BACKWARD:
            state_ = STATE_BACKWARD;
            fSoundStart_ = true;
            fSoundRun_ = true;
            setMaskBits(NETMASK_UPDATE | NETMASK_SOUND);
            break;
        default:
            break;
    }
}

void MoveableBase::updateServerMoveState(Flt32 dt) {
    if (state_ == STATE_STOP || iPathID_ == -1) {
        return;
    }
    rTime_ += dt;
    if (rTotalTime_ > 0.0f && rTime_ >= rTotalTime_) {
        rTime_ = 0.0f;
        if (state_ == STATE_FORWARD ||
            (state_ == STATE_WAYPOINT && iTargetWaypoint_ > iWaypoint_)) {
            ++iWaypoint_;
            if (iWaypoint_ >= iLastWaypoint_) {
                iWaypoint_ = iLastWaypoint_;
                move(STATE_STOP);
            } else if (state_ == STATE_WAYPOINT && iWaypoint_ == iTargetWaypoint_) {
                move(STATE_STOP);
            }
        } else if (state_ == STATE_BACKWARD ||
                   (state_ == STATE_WAYPOINT && iTargetWaypoint_ < iWaypoint_)) {
            if (iWaypoint_ > 0) {
                --iWaypoint_;
            }
            if (iWaypoint_ == 0 || (state_ == STATE_WAYPOINT && iWaypoint_ == iTargetWaypoint_)) {
                move(STATE_STOP);
            }
        }
        setMaskBits(NETMASK_WAYPOINT);
    }
}

void MoveableBase::updateMovement(Flt32 dt) {
    Parent::updateMovement(dt);
}

void MoveableBase::serverProcess(UInt32 curTime) {
    const Flt32 dt = (curTime > lastProcessTime_)
                         ? static_cast<Flt32>(curTime - lastProcessTime_) * 0.001f
                         : 0.0f;
    Parent::serverProcess(curTime);
    updateServerMoveState(dt);
}

void MoveableBase::clientProcess(UInt32 curTime) {
    const Flt32 dt = (curTime > lastProcessTime_)
                         ? static_cast<Flt32>(curTime - lastProcessTime_) * 0.001f
                         : 0.0f;
    Parent::clientProcess(curTime);
    if (state_ != STATE_STOP) {
        rTime_ += dt;
    }
}

bool MoveableBase::processEvent(const sim::SimEvent& event) {
    return Parent::processEvent(event);
}

bool MoveableBase::processQuery(sim::SimQuery& query) {
    return Parent::processQuery(query);
}

bool MoveableBase::packMoveableBaseUpdate(UInt32 mask,
                                          BitStream& stream,
                                          UInt32 currentTime,
                                          bool forceDelta) const {
    if (!packStaticBaseUpdate(mask, stream, currentTime)) {
        return false;
    }

    if (!isGhost()) {
        if (!forceDelta && (mask & NETMASK_INIT) != 0) {
            const Point3F& pos = getPos();
            const Point3F& rot = getRot();
            stream.write(pos.x);
            stream.write(pos.y);
            stream.write(pos.z);
            stream.write(rot.x);
            stream.write(rot.y);
            stream.write(rot.z);
            stream.write(static_cast<Int8>(iWaypoint_));
            packDatFile(stream);
            position_ = pos;
            rotationVec_ = rot;
        } else {
            if (fPathUpdated_) {
                stream.writeFlag(true);
                stream.write(static_cast<Int8>(iPathID_));
                stream.write(static_cast<Int8>(iLastWaypoint_));
                fPathUpdated_ = false;
            } else {
                stream.writeFlag(false);
            }

            if ((mask & NETMASK_EDIT) != 0) {
                stream.writeFlag(true);
                const Point3F& pos = getPos();
                const Point3F& rot = getRot();
                stream.write(pos.x);
                stream.write(pos.y);
                stream.write(pos.z);
                stream.write(rot.x);
                stream.write(rot.y);
                stream.write(rot.z);
            } else {
                stream.writeFlag(false);
            }

            if ((mask & NETMASK_WAYPOINT) != 0) {
                stream.writeFlag(true);
                const UInt8 packed = static_cast<UInt8>((static_cast<UInt8>(state_) << 4) |
                                                        (static_cast<UInt8>(iWaypoint_) & 0x0Fu));
                stream.write(packed);
            } else {
                stream.writeFlag(false);
            }

            if ((mask & NETMASK_UPDATE) != 0) {
                stream.writeFlag(true);
                const Flt32 denom = (rTotalTime_ > 0.0f) ? rTotalTime_ : 1.0f;
                stream.writeFloat(std::clamp(rTime_ / denom, 0.0f, 1.0f), 10);
            } else {
                stream.writeFlag(false);
            }

            stream.writeFlag((mask & NETMASK_SOUND) != 0);
        }
    }

    return stream.isValid();
}

bool MoveableBase::unpackMoveableBaseUpdate(BitStream& stream, UInt32 currentTime) {
    BitStreamRollbackGuard streamGuard(stream);
    const auto savedTeam = teamId_;
    const auto savedCtrl = ctrlClientId_;
    const auto savedOwner = ownerClientId_;
    const auto savedLastOwner = lastOwnerClientId_;
    const auto savedShieldVec = shieldVec_;
    const auto savedZOffset = zOffset_;
    const auto savedThreads = scriptThread_;
    const auto savedFading = m_fading_;
    const auto savedFadeStarted = m_fadeStarted_;
    const auto savedDamage = damageLevel_;
    const auto savedStaticState = Parent::state_;
    const auto savedBlown = blowedUpGood_;
    const auto savedIsTarget = isTarget_;
    const auto savedUnpackedStaticInit = hasUnpackedInitial_;
    const Int32 savedPathId = iPathID_;
    const Int32 savedWaypoint = iWaypoint_;
    const Int32 savedLastWaypoint = iLastWaypoint_;
    const Flt32 savedTime = rTime_;
    const MoveState savedState = state_;
    const Int32 savedDatFileId = datFileId_;
    const TMat3F savedTransform = getTransform();
    const Point3F savedRot = rotation_;
    const Point3F savedPosVec = position_;
    const Point3F savedRotVec = rotationVec_;
    const bool savedStart = fSoundStart_;
    const bool savedStop = fSoundStop_;
    const bool savedRun = fSoundRun_;
    const bool savedInit = hasUnpackedInitialMoveableBase_;

    if (!unpackStaticBaseUpdate(stream, currentTime)) {
        return false;
    }

    const auto rollback = [&]() noexcept {
        teamId_ = savedTeam;
        ctrlClientId_ = savedCtrl;
        ownerClientId_ = savedOwner;
        lastOwnerClientId_ = savedLastOwner;
        shieldVec_ = savedShieldVec;
        zOffset_ = savedZOffset;
        scriptThread_ = savedThreads;
        m_fading_ = savedFading;
        m_fadeStarted_ = savedFadeStarted;
        damageLevel_ = savedDamage;
        Parent::state_ = savedStaticState;
        blowedUpGood_ = savedBlown;
        setIsTarget(savedIsTarget);
        hasUnpackedInitial_ = savedUnpackedStaticInit;
        iPathID_ = savedPathId;
        iWaypoint_ = savedWaypoint;
        iLastWaypoint_ = savedLastWaypoint;
        rTime_ = savedTime;
        state_ = savedState;
        datFileId_ = savedDatFileId;
        SimMovement::setPosition(savedTransform, true);
        rotation_ = savedRot;
        position_ = savedPosVec;
        rotationVec_ = savedRotVec;
        fSoundStart_ = savedStart;
        fSoundStop_ = savedStop;
        fSoundRun_ = savedRun;
        hasUnpackedInitialMoveableBase_ = savedInit;
        return false;
    };

    if (!hasUnpackedInitialMoveableBase_) {
        Point3F pos{};
        Point3F rot{};
        Int8 wp = 0;
        if (!stream.read(pos.x) || !stream.read(pos.y) || !stream.read(pos.z) ||
            !stream.read(rot.x) || !stream.read(rot.y) || !stream.read(rot.z) || !stream.read(wp) ||
            !unpackDatFile(stream) || !stream.isValid()) {
            return rollback();
        }
        iWaypoint_ = static_cast<Int32>(wp);
        position_ = pos;
        rotationVec_ = rot;
        loadDatFile();
        setPos(pos);
        setRot(rot);
        hasUnpackedInitialMoveableBase_ = true;
    } else {
        if (stream.readFlag()) {
            Int8 pid = -1;
            Int8 lastWp = 0;
            if (!stream.read(pid) || !stream.read(lastWp)) {
                return rollback();
            }
            iPathID_ = static_cast<Int32>(pid);
            iLastWaypoint_ = static_cast<Int32>(lastWp);
        }

        if (stream.readFlag()) {
            Point3F pos{};
            Point3F rot{};
            if (!stream.read(pos.x) || !stream.read(pos.y) || !stream.read(pos.z) ||
                !stream.read(rot.x) || !stream.read(rot.y) || !stream.read(rot.z)) {
                return rollback();
            }
            position_ = pos;
            rotationVec_ = rot;
            setPos(pos);
            setRot(rot);
        }

        if (stream.readFlag()) {
            UInt8 stNew = 0;
            if (!stream.read(stNew)) {
                return rollback();
            }
            const Int32 iNewWaypoint = static_cast<Int32>(stNew & 0x0Fu);
            const auto newState = static_cast<MoveState>((stNew >> 4) & 0x0Fu);
            const Int32 diff = iNewWaypoint - iWaypoint_;
            if (diff == 1 || diff == -1) {
                position_ += ldistance_;
                rotationVec_ += adistance_;
            } else if (auto* pm = resolvePathManager()) {
                if (const auto wpOpt = pm->waypoint(iPathID_, iNewWaypoint)) {
                    position_ = wpOpt->position;
                    rotationVec_ = wpOpt->rotation;
                }
            }
            rTime_ = 0.0f;
            state_ = newState;
            iWaypoint_ = iNewWaypoint;
        }

        if (stream.readFlag()) {
            const Flt32 frac = stream.readFloat(10);
            if (!stream.isValid()) {
                return rollback();
            }
            const Flt32 denom = (rTotalTime_ > 0.0f) ? rTotalTime_ : 1.0f;
            rTime_ = denom * frac;
        }

        fSoundStart_ = stream.readFlag();
        if (!stream.isValid()) {
            return rollback();
        }
    }

    if (!stream.isValid()) {
        return rollback();
    }

    streamGuard.committed = true;
    return true;
}

UInt32
MoveableBase::packUpdate(sim::network::GhostManager& /*gm*/, UInt32 mask, BitStream& stream) const {
    packMoveableBaseUpdate(mask, stream);
    return 0;
}

bool MoveableBase::unpackUpdate(sim::network::GhostManager& /*gm*/, BitStream& stream) {
    return unpackMoveableBaseUpdate(stream);
}

Persistent::Error
MoveableBase::read(StreamIO& sio, Int32 version, Int32 user, Persistent::LoadContext& ctx) {
    const Int32 startPos = sio.getPosition();
    const auto savedId = getId();
    const auto savedNetFlags = netFlags();
    const auto savedLocked = isLocked();
    const auto savedTeam = teamId_;
    const auto savedDatFileName = datFileName_;
    const auto savedDestroyable = destroyable_;
    const auto savedShapeRot = rotation_;
    const auto savedTransform = getTransform();
    const MoveState savedState = state_;
    const Flt32 savedTime = rTime_;
    const Int32 savedWaypoint = iWaypoint_;
    const Point3F savedPos = position_;
    const Point3F savedRot = rotationVec_;
    const Point3F savedLDist = ldistance_;
    const Point3F savedADist = adistance_;

    const auto rollback = [&]() noexcept {
        setId(savedId);
        setNetFlags(savedNetFlags);
        setLocked(savedLocked);
        teamId_ = savedTeam;
        datFileName_ = savedDatFileName;
        destroyable_ = savedDestroyable;
        rotation_ = savedShapeRot;
        SimMovement::setPosition(savedTransform, true);
        state_ = savedState;
        rTime_ = savedTime;
        iWaypoint_ = savedWaypoint;
        position_ = savedPos;
        rotationVec_ = savedRot;
        ldistance_ = savedLDist;
        adistance_ = savedADist;
        sio.setPosition(startPos);
        return Persistent::Error::ReadError;
    };

    if (Parent::read(sio, version, user, ctx) != Persistent::Error::Ok) {
        return rollback();
    }

    Int8 stVal = 0;
    Int8 wpVal = 0;
    if (!sio.read(stVal) || !sio.read(rTime_) || !sio.read(wpVal)) {
        return rollback();
    }
    state_ = static_cast<MoveState>(stVal);
    iWaypoint_ = static_cast<Int32>(wpVal);

    if (state_ == STATE_FORWARD || state_ == STATE_BACKWARD) {
        if (!sio.read(position_.x) || !sio.read(position_.y) || !sio.read(position_.z) ||
            !sio.read(rotationVec_.x) || !sio.read(rotationVec_.y) || !sio.read(rotationVec_.z) ||
            !sio.read(ldistance_.x) || !sio.read(ldistance_.y) || !sio.read(ldistance_.z) ||
            !sio.read(adistance_.x) || !sio.read(adistance_.y) || !sio.read(adistance_.z)) {
            return rollback();
        }
    }
    return sio.getStatus() == STRM_OK ? Persistent::Error::Ok : rollback();
}

Persistent::Error
MoveableBase::write(StreamIO& sio, Int32 version, Int32 user, Persistent::StoreContext& ctx) const {
    const Int32 startPos = sio.getPosition();
    const Int8 stVal = static_cast<Int8>(state_);
    const Int8 wpVal = static_cast<Int8>(iWaypoint_);

    if (Parent::write(sio, version, user, ctx) != Persistent::Error::Ok || !sio.write(stVal) ||
        !sio.write(rTime_) || !sio.write(wpVal)) {
        sio.setPosition(startPos);
        return Persistent::Error::WriteError;
    }

    if (state_ == STATE_FORWARD || state_ == STATE_BACKWARD) {
        if (!sio.write(position_.x) || !sio.write(position_.y) || !sio.write(position_.z) ||
            !sio.write(rotationVec_.x) || !sio.write(rotationVec_.y) ||
            !sio.write(rotationVec_.z) || !sio.write(ldistance_.x) || !sio.write(ldistance_.y) ||
            !sio.write(ldistance_.z) || !sio.write(adistance_.x) || !sio.write(adistance_.y) ||
            !sio.write(adistance_.z)) {
            sio.setPosition(startPos);
            return Persistent::Error::WriteError;
        }
    }
    return sio.getStatus() == STRM_OK ? Persistent::Error::Ok : Persistent::Error::WriteError;
}

bool registerBaseSimClasses(Persistent::ClassRegistry& registry) {
    return registry.registerClass<SimMoveObject>("SimMoveObject") &&
           registry.registerClass<StaticShape>(
               "StaticShape", makeFourCC('S', 'S', 'H', 'P'), StaticShapePersTag) &&
           registry.registerClass<Moveable>("Moveable", FearMoveablePersTag) &&
           registry.registerClass<MoveableBase>("MoveableBase", MoveableBasePersTag);
}

bool registerBaseSimNetwork(sim::network::Registry& registry) {
    return registry.registerGhost<StaticShape>(static_cast<UInt16>(StaticShapePersTag)) &&
           registry.registerGhost<Moveable>(static_cast<UInt16>(FearMoveablePersTag)) &&
           registry.registerGhost<MoveableBase>(static_cast<UInt16>(MoveableBasePersTag));
}

} // namespace opent1::game
