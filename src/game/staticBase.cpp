#include <opent1/game/static_base.hpp>
#include <opent1/game/sound.hpp>
#include <algorithm>
#include <cctype>
#include <cmath>

namespace opent1::game {

namespace {
bool equalsIgnoreCase(std::string_view a, std::string_view b) noexcept {
    if (a.size() != b.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) !=
            std::tolower(static_cast<unsigned char>(b[i]))) {
            return false;
        }
    }
    return true;
}

Int32 findSequenceIndex(const world::TsShape& shape, std::string_view seqName) noexcept {
    for (std::size_t i = 0; i < shape.sequences.size(); ++i) {
        const Int32 nameIdx = shape.sequences[i].name;
        if (nameIdx >= 0 && static_cast<std::size_t>(nameIdx) < shape.names.size() &&
            equalsIgnoreCase(shape.names[static_cast<std::size_t>(nameIdx)], seqName)) {
            return static_cast<Int32>(i);
        }
    }
    return -1;
}

Int32 findDetailIndex(const world::TsShape& shape, std::string_view detailName) noexcept {
    for (std::size_t i = 0; i < shape.details.size(); ++i) {
        const Int32 rootNode = shape.details[i].root;
        if (rootNode >= 0 && static_cast<std::size_t>(rootNode) < shape.nodes.size()) {
            const Int32 nameIdx = shape.nodes[static_cast<std::size_t>(rootNode)].name;
            if (nameIdx >= 0 && static_cast<std::size_t>(nameIdx) < shape.names.size() &&
                equalsIgnoreCase(shape.names[static_cast<std::size_t>(nameIdx)], detailName)) {
                return static_cast<Int32>(i);
            }
        }
    }
    return -1;
}

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
// StaticBase
// -----------------------------------------------------------------------------

StaticBase::StaticBase() : collisionImage_(std::make_shared<ShapeCollisionImage>()) {
    setType(StaticObjectType | SimShapeObjectType | SimDefaultObjectType);
    setCollisionMask(0);
    collisionImage_->moveable = false;
    setNetFlags(Ghostable);
}

StaticBase::~StaticBase() {
    if (collisionImage_) {
        collisionImage_->syncCollisionSnapshots();
        collisionImage_->clearShapeInstance();
    }
}

bool StaticBase::onAdd() {
    if (!Parent::onAdd()) {
        return false;
    }
    return true;
}

void StaticBase::onRemove() {
    setIsTarget(false);
    stopAmbientSound();
    Parent::onRemove();
}

void StaticBase::onCollisionNotify(GameBase* object) {
    if (!isGhost() && object) {
        scriptOnCollision(object);
    }
}

void StaticBase::initCollisionInfo() {
    outerCollisionImage_.reset();
    collisionImage_->clearShapeInstance();
    collisionImage_->collisionDetail = -1;

    const auto* shape = getTSShape();
    Box3F box{{-0.5f, -0.5f, -0.5f}, {0.5f, 0.5f, 0.5f}};
    if (shape) {
        collisionImage_->sphere.center = shape->center;
        collisionImage_->sphere.radius = shape->radius;
        collisionImage_->bbox = shape->bounds;
        collisionImage_->collisionDetail = findDetailIndex(*shape, "collision");
        if (collisionImage_->collisionDetail == -1 && !shape->details.empty()) {
            collisionImage_->collisionDetail = 0;
        }
        collisionImage_->collisionLevel = ShapeCollisionImage::CollideFaces;
        collisionImage_->setShapeInstance(
            shapeInstance_.get(), collisionImage_->collisionDetail, shapeInstance_);
        box = shape->bounds;
    }

    if (staticData_ && staticData_->outerRadius > 0.0f && !isGhost()) {
        outerCollisionImage_ = std::make_shared<sim::SimCollisionSphereImage>();
        outerCollisionImage_->center = shape ? shape->center : Point3F{0.0f, 0.0f, 0.0f};
        outerCollisionImage_->radius = staticData_->outerRadius;
        outerCollisionImage_->trigger = true;
        outerCollisionImage_->moveable = false;

        const Point3F r{
            staticData_->outerRadius, staticData_->outerRadius, staticData_->outerRadius};
        box.fMin.setMin(outerCollisionImage_->center - r);
        box.fMax.setMax(outerCollisionImage_->center + r);
    }

    setMovementFlags(movementFlags() | RotateBoundingBox);
    setLocalBoundingBox(box);
}

bool StaticBase::initResources(GameBaseData* inDat) {
    if (!Parent::initResources(inDat)) {
        return false;
    }
    staticData_ = dynamic_cast<StaticBaseData*>(getDatPtr());
    initCollisionInfo();

    ambientThreadIndex_.reset();
    damageThreadIndex_.reset();

    if (const auto* shape = getTSShape()) {
        const Int32 ambSeq = findSequenceIndex(*shape, "ambient");
        if (ambSeq != -1) {
            ambientThreadIndex_ = createThread(ambSeq);
        }
        const Int32 dmgSeq = findSequenceIndex(*shape, "visibility");
        if (dmgSeq != -1) {
            damageThreadIndex_ = createThread(dmgSeq);
        }
    }

    if (staticData_ && staticData_->disableCollision) {
        disableCollision();
    }

    if (isGhost()) {
        setState(state_, true);
    } else {
        state_ = None;
        updateState();
    }
    return true;
}

void StaticBase::setPos(const Point3F& pos) {
    Parent::setPos(pos);
    setMaskBits(NoInterpMask);
}

void StaticBase::setRot(const Point3F& rot) {
    Parent::setRot(rot);
    SimMovement::setRotation(RMat3F{EulerF{rot.x, rot.y, rot.z}}, true);
}

void StaticBase::setState(Int32 ns, bool force) {
    if (ns == state_ && !force) {
        return;
    }
    const Int32 prevState = state_;
    state_ = ns;
    // setActive() only accepts changes while enabled. Damage transitions must
    // clear this directly, before the script observes the new state (legacy).
    if (state_ == Disabled || state_ == Destroyed) {
        active_ = false;
    }

    // Legacy StaticBase holds the authored visibility sequence at its intact
    // endpoint for Enabled/Disabled and its hulk endpoint for Destroyed. It is
    // a state selector, not an animation to advance while disabled.
    if (shapeInstance_ && damageThreadIndex_ && state_ != None) {
        const auto sequence = shapeInstance_->threads()[*damageThreadIndex_].sequence;
        shapeInstance_->setSequence(
            *damageThreadIndex_, sequence, state_ == Destroyed ? 1.0f : 0.0f);
        shapeInstance_->setTimeScale(*damageThreadIndex_, 0.0f);
        // Apply this state once. Reapplying a frozen visibility track every
        // frame overwrites the station's animated power/use IFL materials.
        shapeInstance_->advanceThread(*damageThreadIndex_, 0.0);
    }

    switch (state_) {
        case Enabled: {
            unBlowUp();
            if (shapeInstance_) {
                if (ambientThreadIndex_) {
                    shapeInstance_->setTimeScale(*ambientThreadIndex_, 1.0f);
                }
            }
            break;
        }
        case Disabled: {
            unBlowUp();
            if (shapeInstance_) {
                if (ambientThreadIndex_) {
                    shapeInstance_->setTimeScale(*ambientThreadIndex_, 0.0f);
                }
            }
            break;
        }
        case Destroyed: {
            if (shapeInstance_) {
                if (ambientThreadIndex_) {
                    shapeInstance_->setTimeScale(*ambientThreadIndex_, 0.0f);
                }
            }
            // The server owns health; ghosts own the explosion/hulk display.
            // Initial ghosts already contain the authored destroyed pose and
            // must not replay the destruction effect when first discovered.
            if (isGhost()) {
                if (prevState != None) {
                    blowUp();
                }
            } else {
                damageLevel_ = staticData_ ? staticData_->maxDamage : 1.0f;
            }
            break;
        }
        default:
            break;
    }
    setMaskBits(DamageMask);
    updateAmbientSound();
    if (!isGhost() && !force) {
        if (auto* con = resolveConsole()) {
            const std::string_view scriptEv =
                (state_ == Enabled)
                    ? "onEnabled"
                    : (state_ == Disabled ? "onDisabled"
                                          : (state_ == Destroyed ? "onDestroyed" : "onNone"));
            const std::string cb = scriptName(scriptEv);
            if (!cb.empty()) {
                std::array<std::string, 1> argv{scriptThis()};
                (void)con->execute(cb, argv);
            }
        }
    }
}

bool StaticBase::updateState() {
    const Flt32 maxDmg =
        (staticData_ && staticData_->maxDamage > 0.0f) ? staticData_->maxDamage : 1.0f;
    const Flt32 disabledThresh = maxDmg * (staticData_ ? staticData_->damageLevel.disabled : 0.5f);
    const Flt32 destroyedThresh =
        maxDmg * (staticData_ ? staticData_->damageLevel.destroyed : 0.75f);
    if (damageLevel_ >= destroyedThresh) {
        if (damageLevel_ >= maxDmg) {
            damageLevel_ = maxDmg;
        }
        setState(Destroyed);
    } else if (damageLevel_ >= disabledThresh) {
        setState(Disabled);
    } else {
        setState(Enabled);
    }
    return true;
}

Int32 StaticBase::getDamageSkinLevel(Int32 detailLevel) const noexcept {
    if (!staticData_) {
        return 0;
    }
    const Flt32 dmg = getDamageLevel();
    if (detailLevel == 1 && dmg > staticData_->damageLevel.disabled) {
        return 2;
    }
    if (detailLevel == 2) {
        if (dmg > staticData_->damageLevel.destroyed) {
            return 3;
        }
        if (dmg > staticData_->damageLevel.disabled) {
            return 2;
        }
        if (dmg > staticData_->damageLevel.disabled * 0.5f) {
            return 1;
        }
    }
    return 0;
}

std::string_view StaticBase::getDamageState() const noexcept {
    if (state_ == Enabled) {
        return "Enabled";
    }
    if (state_ == Disabled) {
        return "Disabled";
    }
    return "Destroyed";
}

bool StaticBase::getCommandStatus(CommandStatus* status) const {
    if (!status) {
        return false;
    }
    status->damageLevel = state_ - 1;
    status->active = active_;
    return true;
}

bool StaticBase::setActive(bool active) {
    if (state_ == Enabled && active != active_) {
        active_ = active;
        setMaskBits(InfoMask);
        if (active_) {
            onActivate();
        } else {
            onDeactivate();
        }
    }
    return active_;
}

void StaticBase::onActivate() {
    if (!isGhost()) {
        if (auto* con = resolveConsole()) {
            const std::string cb = scriptName("onActivate");
            if (!cb.empty()) {
                std::array<std::string, 1> argv{scriptThis()};
                (void)con->execute(cb, argv);
            }
        }
    }
}

void StaticBase::onDeactivate() {
    if (!isGhost()) {
        if (auto* con = resolveConsole()) {
            const std::string cb = scriptName("onDeactivate");
            if (!cb.empty()) {
                std::array<std::string, 1> argv{scriptThis()};
                (void)con->execute(cb, argv);
            }
        }
    }
}

void StaticBase::onPowerStateChange(GameBase* generator) {
    if (!isGhost() && state_ == Enabled) {
        if (auto* con = resolveConsole()) {
            const std::string cb = scriptName("onPower");
            if (!cb.empty()) {
                std::array<std::string, 3> argv{scriptThis(),
                                                isPowered() ? "True" : "False",
                                                generator ? generator->scriptThis() : "-1"};
                (void)con->execute(cb, argv);
            }
        }
        setMaskBits(InfoMask);
    }
}

void StaticBase::stopAmbientSound() {
    if (ambientSound_ && getManager()) {
        if (auto* audio =
                dynamic_cast<SoundPlayback*>(getManager()->findObject("GameSoundPlayback"));
            audio && audio->stop) {
            audio->stop(ambientSound_);
        }
    }
    ambientSound_ = 0;
}

void StaticBase::updateAmbientSound() {
    if (state_ != Enabled) {
        stopAmbientSound();
        return;
    }
    if (!isGhost() || !staticData_ || staticData_->sfxAmbient < 0) {
        return;
    }
    if (!ambientSound_) {
        ambientSound_ =
            TSFX::PlayAt(UInt32(staticData_->sfxAmbient), getTransform(), {}, getManager());
    } else if (getManager()) {
        if (auto* audio =
                dynamic_cast<SoundPlayback*>(getManager()->findObject("GameSoundPlayback"));
            audio && audio->move) {
            audio->move(ambientSound_, getTransform(), {});
        }
    }
}

void StaticBase::setDamageLevel(Flt32 level) {
    Parent::setDamageLevel(level);
    updateState();
}

void StaticBase::serverProcess(UInt32 curTime) {
    const Flt32 dt = (curTime > lastProcessTime_)
                         ? static_cast<Flt32>(curTime - lastProcessTime_) * 0.001f
                         : 0.0f;
    lastProcessTime_ = curTime;

    updateEnergy(dt);
    updateDamage(dt);
    updateScriptThreads(dt);
    updateState();
    updateTimers();
}

void StaticBase::clientProcess(UInt32 curTime) {
    const Flt32 dt = (curTime > lastProcessTime_)
                         ? static_cast<Flt32>(curTime - lastProcessTime_) * 0.001f
                         : 0.0f;
    lastProcessTime_ = curTime;

    updateEnergy(dt);
    updateAmbientSound();
    updateScriptThreads(dt);
    updateShieldThread(dt);

    if (shapeInstance_) {
        if (ambientThreadIndex_) {
            shapeInstance_->advanceThread(*ambientThreadIndex_, static_cast<Flt64>(dt));
        }
    }
}

void StaticBase::setIsTarget(bool isTarget) {
    if (getManager()) {
        if (!isTarget) {
            if (isTarget_) {
                removeFromSet(TargetableSetId);
            }
        } else {
            if (!isTarget_) {
                addToSet(TargetableSetId);
            }
        }
    }
    setMaskBits(InfoMask);
    isTarget_ = isTarget;
}

bool StaticBase::getTarget(Point3F* outTarget, Int32* outTeam) const {
    if (!isTarget_ || state_ != Enabled) {
        return false;
    }
    if (outTarget) {
        *outTarget = getBoxCenter();
    }
    if (outTeam) {
        *outTeam = getTeam();
    }
    return true;
}

void StaticBase::disableCollision() {
    ++disableCollisionCount_;
}

void StaticBase::enableCollision() {
    if (disableCollisionCount_ > 0) {
        --disableCollisionCount_;
    }
}

std::shared_ptr<sim::SimCollisionImage> StaticBase::getSelfImage() {
    if (disableCollisionCount_ > 0 || !collisionImage_ || collisionImage_->collisionDetail == -1) {
        return nullptr;
    }
    collisionImage_->transform = getTransform();
    return collisionImage_;
}

bool StaticBase::onSimCollisionImageQuery(sim::SimCollisionImageQuery& query) const {
    if (disableCollisionCount_ == 0 && collisionImage_ && collisionImage_->collisionDetail != -1) {
        collisionImage_->transform = getTransform();
        query.images.push_back(collisionImage_);
        if (outerCollisionImage_ && query.detail != sim::SimCollisionImageQuery::Editor) {
            outerCollisionImage_->transform = getTransform();
            query.images.push_back(outerCollisionImage_);
        }
        return true;
    }
    return false;
}

bool StaticBase::onSimEditEvent(const sim::SimEditEvent& event) {
    if (event.editor == sim::MissionEditorId) {
        if (!event.state) {
            setMaskBits(PositionMask | RotationMask | NoInterpMask);
        }
        return true;
    }
    return false;
}

bool StaticBase::processEvent(const sim::SimEvent& event) {
    if (const auto* editEv = dynamic_cast<const sim::SimEditEvent*>(&event)) {
        return onSimEditEvent(*editEv);
    }
    return Parent::processEvent(event);
}

bool StaticBase::processQuery(sim::SimQuery& query) {
    if (auto* collQ = dynamic_cast<sim::SimCollisionImageQuery*>(&query)) {
        return onSimCollisionImageQuery(*collQ);
    }
    return Parent::processQuery(query);
}

bool StaticBase::packStaticBaseUpdate(UInt32 mask, BitStream& stream, UInt32 currentTime) const {
    if (!packShapeBaseUpdate(mask, stream, currentTime)) {
        return false;
    }

    if (stream.writeFlag((mask & (PositionMask | RotationMask)) != 0)) {
        const Point3F& pos = getPos();
        const Point3F& rot = getRot();
        stream.write(pos.x);
        stream.write(pos.y);
        stream.write(pos.z);
        stream.write(rot.x);
        stream.write(rot.y);
        stream.write(rot.z);
    }

    if (stream.writeFlag((mask & DamageMask) != 0)) {
        if (!stream.writeFlag(state_ == Enabled)) {
            stream.writeFlag(state_ == Disabled);
        }
        const Flt32 maxDamage =
            (staticData_ && staticData_->maxDamage > 0.0f) ? staticData_->maxDamage : 1.0f;
        const Flt32 percentage = std::clamp(damageLevel_ / maxDamage, 0.0f, 1.0f);
        stream.writeFloat(percentage, DamageLevelBits);
    }

    if (stream.writeFlag((mask & InfoMask) != 0)) {
        stream.writeFlag(isTarget_);
    }

    return stream.isValid();
}

bool StaticBase::unpackStaticBaseUpdate(BitStream& stream, UInt32 currentTime) {
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
    const auto savedTransform = getTransform();
    const auto savedRot = rotation_;
    const auto savedDamage = damageLevel_;
    const auto savedState = state_;
    const auto savedBlown = blowedUpGood_;
    const auto savedIsTarget = isTarget_;
    const auto savedUnpackedInitial = hasUnpackedInitial_;

    if (!unpackShapeBaseUpdate(stream, currentTime)) {
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
        SimMovement::setPosition(savedTransform, true);
        rotation_ = savedRot;
        damageLevel_ = savedDamage;
        state_ = savedState;
        blowedUpGood_ = savedBlown;
        setIsTarget(savedIsTarget);
        hasUnpackedInitial_ = savedUnpackedInitial;
        return false;
    };

    if (stream.readFlag()) {
        Point3F pos{};
        Point3F rot{};
        if (!stream.read(pos.x) || !stream.read(pos.y) || !stream.read(pos.z) ||
            !stream.read(rot.x) || !stream.read(rot.y) || !stream.read(rot.z)) {
            return rollback();
        }
        if (!stream.isValid()) {
            return rollback();
        }
        setRot(rot);
        setPos(pos);
    }

    if (stream.readFlag()) {
        DamageState newState = Destroyed;
        if (stream.readFlag()) {
            newState = Enabled;
        } else if (stream.readFlag()) {
            newState = Disabled;
        } else {
            newState = Destroyed;
        }
        damageLevel_ = stream.readFloat(DamageLevelBits);
        if (!stream.isValid()) {
            return rollback();
        }
        reSkin();
        setState(newState, false);
    }

    if (stream.readFlag()) {
        const bool targetFlag = stream.readFlag();
        if (!stream.isValid()) {
            return rollback();
        }
        setIsTarget(targetFlag);
    }

    if (!stream.isValid()) {
        return rollback();
    }

    hasUnpackedInitial_ = true;
    streamGuard.committed = true;
    return true;
}

UInt32
StaticBase::packUpdate(sim::network::GhostManager& /*gm*/, UInt32 mask, BitStream& stream) const {
    packStaticBaseUpdate(mask, stream);
    return 0;
}

bool StaticBase::unpackUpdate(sim::network::GhostManager& /*gm*/, BitStream& stream) {
    return unpackStaticBaseUpdate(stream);
}

Persistent::Error
StaticBase::read(StreamIO& sio, Int32 /*version*/, Int32 /*user*/, Persistent::LoadContext& ctx) {
    const Int32 startPos = sio.getPosition();

    Int32 fileVersion = 0;
    if (!sio.read(fileVersion)) {
        sio.setPosition(startPos);
        return Persistent::Error::ReadError;
    }
    if (Parent::read(sio, 0, 0, ctx) != Persistent::Error::Ok) {
        sio.setPosition(startPos);
        return Persistent::Error::ReadError;
    }
    return sio.getStatus() == STRM_OK ? Persistent::Error::Ok : Persistent::Error::ReadError;
}

Persistent::Error StaticBase::write(StreamIO& sio,
                                    Int32 /*version*/,
                                    Int32 /*user*/,
                                    Persistent::StoreContext& ctx) const {
    const Int32 startPos = sio.getPosition();
    const Int32 fileVersion = 0;
    if (!sio.write(fileVersion) || Parent::write(sio, 0, 0, ctx) != Persistent::Error::Ok) {
        sio.setPosition(startPos);
        return Persistent::Error::WriteError;
    }
    return sio.getStatus() == STRM_OK ? Persistent::Error::Ok : Persistent::Error::WriteError;
}

// -----------------------------------------------------------------------------
// StaticShape
// -----------------------------------------------------------------------------

StaticShape::StaticShape() = default;

bool StaticShape::packStaticShapeUpdate(UInt32 mask, BitStream& stream, UInt32 currentTime) const {
    if (!packStaticBaseUpdate(mask, stream, currentTime)) {
        return false;
    }
    if (stream.writeFlag((mask & InfoMask) != 0)) {
        packDatFile(stream);
        if (stream.writeFlag(sensorKey != -1)) {
            stream.writeInt(sensorKey, 7);
        }
    }
    return stream.isValid();
}

bool StaticShape::unpackStaticShapeUpdate(BitStream& stream, UInt32 currentTime) {
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
    const auto savedTransform = getTransform();
    const auto savedRot = rotation_;
    const auto savedDamage = damageLevel_;
    const auto savedState = state_;
    const auto savedBlown = blowedUpGood_;
    const auto savedIsTarget = isTarget_;
    const auto savedUnpackedInitial = hasUnpackedInitial_;
    const Int32 savedDatFileId = datFileId_;
    const Int32 savedSensorKey = sensorKey;

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
        SimMovement::setPosition(savedTransform, true);
        rotation_ = savedRot;
        damageLevel_ = savedDamage;
        state_ = savedState;
        blowedUpGood_ = savedBlown;
        setIsTarget(savedIsTarget);
        hasUnpackedInitial_ = savedUnpackedInitial;
        datFileId_ = savedDatFileId;
        sensorKey = savedSensorKey;
        return false;
    };

    if (stream.readFlag()) {
        if (!unpackDatFile(stream)) {
            return rollback();
        }
        loadDatFile();
        if (stream.readFlag()) {
            sensorKey = stream.readInt(7);
            if (!stream.isValid()) {
                return rollback();
            }
            if (getManager()) {
                addToSet(SensorVisibleSetId);
            }
        }
    }

    if (!stream.isValid()) {
        return rollback();
    }

    streamGuard.committed = true;
    return true;
}

UInt32
StaticShape::packUpdate(sim::network::GhostManager& /*gm*/, UInt32 mask, BitStream& stream) const {
    packStaticShapeUpdate(mask, stream);
    return 0;
}

bool StaticShape::unpackUpdate(sim::network::GhostManager& /*gm*/, BitStream& stream) {
    return unpackStaticShapeUpdate(stream);
}

Persistent::Error
StaticShape::read(StreamIO& sio, Int32 version, Int32 user, Persistent::LoadContext& ctx) {
    const Int32 startPos = sio.getPosition();
    Int32 fileVersion = 0;
    if (!sio.read(fileVersion) || Parent::read(sio, version, user, ctx) != Persistent::Error::Ok) {
        sio.setPosition(startPos);
        return Persistent::Error::ReadError;
    }
    return sio.getStatus() == STRM_OK ? Persistent::Error::Ok : Persistent::Error::ReadError;
}

Persistent::Error
StaticShape::write(StreamIO& sio, Int32 version, Int32 user, Persistent::StoreContext& ctx) const {
    const Int32 startPos = sio.getPosition();
    const Int32 fileVersion = 0;
    if (!sio.write(fileVersion) ||
        Parent::write(sio, version, user, ctx) != Persistent::Error::Ok) {
        sio.setPosition(startPos);
        return Persistent::Error::WriteError;
    }
    return sio.getStatus() == STRM_OK ? Persistent::Error::Ok : Persistent::Error::WriteError;
}

} // namespace opent1::game
