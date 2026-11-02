#include <opent1/game/shape_base.hpp>
#include <opent1/core/text.hpp>
#include <opent1/world/scene_objects.hpp>
#include <opent1/game/sound.hpp>
#include <opent1/game/player_manager.hpp>
#include <algorithm>
#include <bit>
#include <cctype>
#include <charconv>
#include <cmath>
#include <sstream>
#include <unordered_map>

namespace opent1::game {

namespace {
bool parsePoint3Field(std::string_view text, Point3F& out) noexcept {
    std::istringstream iss{std::string(text)};
    Flt32 x = 0.0f;
    Flt32 y = 0.0f;
    Flt32 z = 0.0f;
    if (!(iss >> x >> y >> z)) {
        return false;
    }
    out.set(x, y, z);
    return true;
}

TMat3F ensureRigidTransform(const TMat3F& inMat, bool preserveReflection = false) noexcept {
    TMat3F out = inMat;
    if (!std::isfinite(out.p.x) || !std::isfinite(out.p.y) || !std::isfinite(out.p.z)) {
        out.p.set(0.0f, 0.0f, 0.0f);
    }
    Point3F r0{out.m[0][0], out.m[0][1], out.m[0][2]};
    Point3F r1{out.m[1][0], out.m[1][1], out.m[1][2]};
    if (!std::isfinite(r0.len()) || r0.len() < 1e-5f) {
        r0.set(1.0f, 0.0f, 0.0f);
    } else {
        r0.normalize();
    }
    r1 -= r0 * m_dot(r1, r0);
    if (!std::isfinite(r1.len()) || r1.len() < 1e-5f) {
        r1 = std::fabs(r0.y) < 0.9f ? Point3F{0.0f, 1.0f, 0.0f} : Point3F{0.0f, 0.0f, 1.0f};
        r1 -= r0 * m_dot(r1, r0);
    }
    r1.normalize();
    Point3F r2{};
    m_cross(r0, r1, r2);
    r2.normalize();
    if (preserveReflection && inMat.determinant() < 0) {
        r2.neg();
    }
    out.setRow(0, r0);
    out.setRow(1, r1);
    out.setRow(2, r2);
    out.flags = RMat3F::Matrix_HasRotation | RMat3F::Matrix_HasTranslation;
    return out;
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

ShapeBase::ShapeBase() {
    setType(SimShapeObjectType | SimDefaultObjectType);
    image_.pOwner = this;
    for (auto& st : scriptThread_) {
        st.threadIndex.reset();
        st.sound = 0;
        st.state = SThread::Play;
        st.sequence = -1;
        st.atEnd = false;
        st.forward = true;
    }
}

ShapeBase::~ShapeBase() {
    image_.clearShapeInstance();
    shieldImage_.clearShapeInstance();
}

bool ShapeBase::onAdd() {
    if (const auto rotStr = getFieldValue("rotation"); rotStr && !rotStr->empty()) {
        Point3F rot{};
        if (parsePoint3Field(*rotStr, rot)) {
            setRot(rot);
        }
    }
    if (const auto posStr = getFieldValue("position"); posStr && !posStr->empty()) {
        Point3F pos{};
        if (parsePoint3Field(*posStr, pos)) {
            setPos(pos);
        }
    }
    if (const auto destrStr = getFieldValue("destroyable"); destrStr && !destrStr->empty()) {
        if (*destrStr == "0" || equalIgnoreCase(*destrStr, "false")) {
            destroyable_ = false;
        } else if (*destrStr == "1" || equalIgnoreCase(*destrStr, "true")) {
            destroyable_ = true;
        }
    }
    if (!Parent::onAdd()) {
        return false;
    }
    (void)addToSet(sim::RenderSetId);
    return true;
}

void ShapeBase::onRemove() {
    for (auto& st : scriptThread_) {
        stopSequenceSound(st);
    }
    Parent::onRemove();
}

void ShapeBase::setShapeInstance(std::shared_ptr<world::TsShapeInstance> inst) {
    shapeInstance_ = std::move(inst);
    cachedWorldResources_.reset();
    image_.setShapeInstance(shapeInstance_.get(), shapeInstance_);
    if (shapeInstance_ && shapeInstance_->shape()) {
        movementStepSize_ = shapeInstance_->shape()->radius * 0.45f;
    }
}

const world::TsShape* ShapeBase::getTSShape() const noexcept {
    if (shapeInstance_ && shapeInstance_->shape()) {
        return shapeInstance_->shape().get();
    }
    if (const auto* sbd = dynamic_cast<const ShapeBaseData*>(getDatPtr())) {
        if (sbd->shape) {
            return sbd->shape.get();
        }
    }
    return nullptr;
}

Flt32 ShapeBase::getRadius() const noexcept {
    if (const auto* shape = getTSShape()) {
        return shape->radius;
    }
    return 0.0f;
}

Point3F ShapeBase::getCenter() const noexcept {
    if (const auto* shape = getTSShape()) {
        return shape->center;
    }
    return Point3F{0.0f, 0.0f, 0.0f};
}

const Point3F& ShapeBase::getAnimTransform() const noexcept {
    if (shapeInstance_) {
        return shapeInstance_->rootDelta().p;
    }
    return rootDeltaTransform_.p;
}

void ShapeBase::clearAnimTransform() noexcept {
    rootDeltaTransform_.identity();
    if (shapeInstance_) {
        shapeInstance_->resetRootMotion();
    }
}

const TMat3F& ShapeBase::rootDeltaTransform() const noexcept {
    if (shapeInstance_) {
        return shapeInstance_->rootDelta();
    }
    return rootDeltaTransform_;
}

bool ShapeBase::initResources(GameBaseData* dat) {
    if (!Parent::initResources(dat)) {
        return false;
    }

    setRot(rotation_);
    setPos(getLinearPosition());

    shapeData_ = dynamic_cast<ShapeBaseData*>(getDatPtr());
    shieldShapeInstance_.reset();
    shieldImage_.clearShapeInstance();
    shieldThreadIndex_.reset();
    cachedWorldResources_.reset();
    cachedShieldResources_.reset();

    if (shapeData_) {
        energy_ = shapeData_->maxEnergy;
        ResourceManager* rm = nullptr;
        if (getManager()) {
            if (auto* sr = sim::SimResource::find(*getManager())) {
                rm = &sr->get();
            }
        }
        if (!rm) {
            if (auto* dbm = resolveDataBlockManager(); dbm && dbm->resources()) {
                rm = dbm->resources();
            }
        }
        if (!shapeData_->shape && !shapeData_->fileName.empty()) {
            if (rm) {
                std::string fname = shapeData_->fileName;
                shapeData_->shape = rm->load<world::TsShape>(fname);
                if (!shapeData_->shape && !fname.ends_with(".dts") && !fname.ends_with(".DTS")) {
                    shapeData_->shape = rm->load<world::TsShape>(fname + ".dts");
                }
            }
        }
        if (isGhost() && !shapeData_->shieldShape && !shapeData_->shieldShapeName.empty() && rm) {
            std::string name = shapeData_->shieldShapeName;
            if (!name.ends_with(".dts") && !name.ends_with(".DTS")) {
                name += ".dts";
            }
            shapeData_->shieldShape = rm->load<world::TsShape>(name);
        }
        if (isGhost() && shapeData_->shieldShape) {
            shieldShapeInstance_ =
                std::make_shared<world::TsShapeInstance>(shapeData_->shieldShape);
            shieldImage_.setShapeInstance(shieldShapeInstance_.get(), shieldShapeInstance_);
            for (std::size_t i = 0; i < shapeData_->shieldShape->sequences.size(); ++i) {
                const auto nameIdx = shapeData_->shieldShape->sequences[i].name;
                if (nameIdx >= 0 &&
                    static_cast<std::size_t>(nameIdx) < shapeData_->shieldShape->names.size() &&
                    equalIgnoreCase(
                        shapeData_->shieldShape->names[static_cast<std::size_t>(nameIdx)],
                        "Sequence01")) {
                    shieldThreadIndex_ =
                        shieldShapeInstance_->addThread(static_cast<Int32>(i), 1.0f);
                    if (shieldThreadIndex_) {
                        shieldShapeInstance_->setTimeScale(*shieldThreadIndex_, 1.0f);
                    }
                    break;
                }
            }
        }
        if (shapeData_->shape) {
            shapeInstance_ = std::make_shared<world::TsShapeInstance>(shapeData_->shape);
            image_.setShapeInstance(shapeInstance_.get(), shapeInstance_);
            movementStepSize_ = shapeData_->shape->radius * 0.45f;
        }
    }

    if (isGhost()) {
        reSkin();
    }

    if (auto* mgr = getManager()) {
        if (auto* root = dynamic_cast<sim::SimContainer*>(mgr->findObject(sim::RootContainerId))) {
            root->addObject(*this);
            image_.root = root;
        }
    }

    for (Int32 i = 0; i < MaxScriptThreads; ++i) {
        auto& st = scriptThread_[static_cast<std::size_t>(i)];
        st.threadIndex.reset();
        if (st.sequence != -1) {
            setSequence(i, st.sequence, false);
        }
    }
    return true;
}

void ShapeBase::setPos(const Point3F& pos) {
    TMat3F mat(EulerF{rotation_.x, rotation_.y, rotation_.z}, pos);
    SimMovement::setPosition(mat, true);
    setMaskBits(PositionMask);
    clearRestFlag();
}

void ShapeBase::setRot(const Point3F& rot) {
    rotation_ = rot;
    RMat3F rmat(EulerF{rotation_.x, rotation_.y, rotation_.z});
    SimMovement::setRotation(rmat, true);
    setMaskBits(RotationMask);
}

void ShapeBase::updateMovement(Flt32 slice) {
    setTimeSlice(slice);
    TMat3F tmat = SimMovement::updateMovement();
    RMat3F rmat(EulerF{rotation_.x, rotation_.y, rotation_.z});
    static_cast<RMat3F&>(tmat) = rmat;
    tmat.flags |= TMat3F::Matrix_HasTranslation;

    if (!SimMovement::stepPosition(tmat, movementStepSize_ * 0.3f, movementStepSize_)) {
        setRotation(rmat, true);
    }
}

std::optional<UInt32> ShapeBase::createThread(Int32 seqNum) {
    if (!shapeInstance_ || !shapeInstance_->shape() || shapeInstance_->shape()->sequences.empty()) {
        return std::nullopt;
    }
    auto idx = shapeInstance_->addThread(seqNum, 0.0f);
    if (idx) {
        shapeInstance_->setTimeScale(*idx, 1.0f);
    }
    return idx;
}

bool ShapeBase::setSequence(Int32 slot, Int32 seq, bool reset) {
    if (slot < 0 || slot >= MaxScriptThreads || seq < 0) {
        return false;
    }
    auto& st = scriptThread_[static_cast<std::size_t>(slot)];
    if (st.threadIndex && st.sequence == seq && st.state == SThread::Play) {
        return true;
    }

    if (seq < MaxSequenceIndex) {
        setMaskBits(ThreadMask | (ThreadNMask << slot));
        st.sequence = seq;
        if (reset) {
            st.state = SThread::Play;
            st.atEnd = false;
            st.forward = true;
        }
        if (shapeInstance_) {
            if (!st.threadIndex) {
                st.threadIndex = shapeInstance_->addThread(seq, 0.0f);
            } else {
                shapeInstance_->setSequence(*st.threadIndex, seq, 0.0f);
            }
            stopSequenceSound(st);
            updateSequence(st);
        }
        return true;
    }
    return false;
}

void ShapeBase::stopSequenceSound(SThread& st) {
    if (st.sound && getManager()) {
        if (auto* audio =
                dynamic_cast<SoundPlayback*>(getManager()->findObject("GameSoundPlayback"));
            audio && audio->stop) {
            audio->stop(st.sound);
        }
    }
    st.sound = 0;
}

void ShapeBase::startSequenceSound(SThread& st) {
    if (!isGhost() || st.sound || !st.threadIndex || !shapeData_) {
        return;
    }
    const auto* shape = getTSShape();
    if (!shape || st.sequence < 0 || std::size_t(st.sequence) >= shape->sequences.size()) {
        return;
    }
    const auto name = shape->sequences[std::size_t(st.sequence)].name;
    if (name < 0 || std::size_t(name) >= shape->names.size()) {
        return;
    }
    for (const auto& entry : shapeData_->sequenceSound) {
        if (entry.sound >= 0 && equalIgnoreCase(entry.sequence, shape->names[std::size_t(name)])) {
            st.sound = TSFX::PlayAt(UInt32(entry.sound), getTransform(), {}, getManager());
            break;
        }
    }
}

void ShapeBase::updateSequence(SThread& st) {
    switch (st.state) {
        case SThread::Stop:
            if (shapeInstance_ && st.threadIndex) {
                shapeInstance_->setTimeScale(*st.threadIndex, 1.0f);
                shapeInstance_->setSequence(*st.threadIndex, st.sequence, 0.0f);
                shapeInstance_->setTimeScale(*st.threadIndex, 0.0f);
            }
            stopSequenceSound(st);
            break;
        case SThread::Pause:
            if (shapeInstance_ && st.threadIndex) {
                shapeInstance_->setTimeScale(*st.threadIndex, 0.0f);
            }
            stopSequenceSound(st);
            break;
        case SThread::Play:
            if (st.atEnd) {
                if (shapeInstance_ && st.threadIndex) {
                    shapeInstance_->setTimeScale(*st.threadIndex, 1.0f);
                    shapeInstance_->setSequence(
                        *st.threadIndex, st.sequence, st.forward ? 1.0f : 0.0f);
                    shapeInstance_->setTimeScale(*st.threadIndex, 0.0f);
                }
                stopSequenceSound(st);
            } else if (shapeInstance_ && st.threadIndex) {
                shapeInstance_->setTimeScale(*st.threadIndex, st.forward ? 1.0f : -1.0f);
                startSequenceSound(st);
            }
            break;
        default:
            break;
    }
}

bool ShapeBase::stopSequence(Int32 slot) {
    if (slot < 0 || slot >= MaxScriptThreads) {
        return false;
    }
    auto& st = scriptThread_[static_cast<std::size_t>(slot)];
    if (st.sequence != -1 && st.state != SThread::Stop) {
        setMaskBits(ThreadMask | (ThreadNMask << slot));
        st.state = SThread::Stop;
        updateSequence(st);
        return true;
    }
    return false;
}

bool ShapeBase::pauseSequence(Int32 slot) {
    if (slot < 0 || slot >= MaxScriptThreads) {
        return false;
    }
    auto& st = scriptThread_[static_cast<std::size_t>(slot)];
    if (st.sequence != -1 && st.state != SThread::Pause) {
        setMaskBits(ThreadMask | (ThreadNMask << slot));
        st.state = SThread::Pause;
        updateSequence(st);
        return true;
    }
    return false;
}

bool ShapeBase::playSequence(Int32 slot) {
    if (slot < 0 || slot >= MaxScriptThreads) {
        return false;
    }
    auto& st = scriptThread_[static_cast<std::size_t>(slot)];
    if (st.sequence != -1 && st.state != SThread::Play) {
        setMaskBits(ThreadMask | (ThreadNMask << slot));
        st.state = SThread::Play;
        updateSequence(st);
        return true;
    }
    return false;
}

bool ShapeBase::setSequenceDir(Int32 slot, bool forward) {
    if (slot < 0 || slot >= MaxScriptThreads) {
        return false;
    }
    auto& st = scriptThread_[static_cast<std::size_t>(slot)];
    if (st.sequence != -1) {
        if (st.forward != forward) {
            setMaskBits(ThreadMask | (ThreadNMask << slot));
            st.forward = forward;
            st.atEnd = false;
            updateSequence(st);
        }
        return true;
    }
    return false;
}

void ShapeBase::updateScriptThreads(SimTime t) {
    for (Int32 i = 0; i < MaxScriptThreads; ++i) {
        auto& st = scriptThread_[static_cast<std::size_t>(i)];
        if (!shapeInstance_ || !st.threadIndex) {
            continue;
        }
        const auto threads = shapeInstance_->threads();
        if (*st.threadIndex >= threads.size()) {
            continue;
        }
        const auto& tsThread = threads[*st.threadIndex];
        if (st.state == SThread::Play && !st.atEnd) {
            startSequenceSound(st);
        }
        const auto* shape = getTSShape();
        bool cyclic = false;
        if (shape && tsThread.sequence >= 0 &&
            static_cast<std::size_t>(tsThread.sequence) < shape->sequences.size()) {
            cyclic = shape->sequences[static_cast<std::size_t>(tsThread.sequence)].cyclic;
        }

        if (!cyclic && !st.atEnd &&
            (st.forward ? tsThread.position >= 1.0f : tsThread.position <= 0.0f)) {
            st.atEnd = true;
            updateSequence(st);
            if (!isGhost()) {
                if (auto* con = resolveConsole()) {
                    const std::string cb = scriptName("onEndSequence");
                    if (!cb.empty()) {
                        std::array<std::string, 2> argv{scriptThis(), std::to_string(i)};
                        (void)con->execute(cb, argv);
                    }
                }
            }
        }
        shapeInstance_->advanceThread(*st.threadIndex, static_cast<Flt64>(t));
    }
}

void ShapeBase::setEnergy(Flt32 ee) {
    const Flt32 maxEnergy = shapeData_ ? shapeData_->maxEnergy : 100.0f;
    if (ee < 0.0f) {
        energy_ = 0.0f;
    } else if (ee > maxEnergy) {
        energy_ = maxEnergy;
    } else {
        energy_ = ee;
    }
}

void ShapeBase::setRechargeRate(Flt32 rate) {
    rechargeRate_ = rate;
    setMaskBits(InfoMask);
}

void ShapeBase::updateEnergy(SimTime t) {
    const Flt32 maxEnergy = shapeData_ ? shapeData_->maxEnergy : 100.0f;
    energy_ += rechargeRate_ * t;
    if (energy_ > maxEnergy) {
        energy_ = maxEnergy;
    } else if (energy_ < 0.0f) {
        energy_ = 0.0f;
    }
}

Flt32 ShapeBase::getEnergyLevel() const noexcept {
    const Flt32 maxEnergy =
        (shapeData_ && shapeData_->maxEnergy > 0.0f) ? shapeData_->maxEnergy : 100.0f;
    Flt32 returnVal = 1.0f - (energy_ / maxEnergy);
    if (returnVal > 1.0f) {
        returnVal = 1.0f;
    }
    if (returnVal < 0.0f) {
        returnVal = 0.0f;
    }
    return returnVal;
}

void ShapeBase::setDamageLevel(Flt32 level) {
    const Flt32 maxDamage = shapeData_ ? shapeData_->maxDamage : 1.0f;
    if (level < 0.0f) {
        level = 0.0f;
    } else if (level >= maxDamage) {
        level = maxDamage;
    }
    if (level != damageLevel_) {
        damageLevel_ = level;
        setMaskBits(DamageMask);
    }
}

bool ShapeBase::repairDamage(Flt32 amount) {
    if (damageLevel_ != 0.0f && amount > 0.0f) {
        repairReserve_ += amount;
    }
    return true;
}

void ShapeBase::updateDamage(SimTime t) {
    const Flt32 repairRate = shapeData_ ? shapeData_->repairRate : 0.1f;
    if (repairReserve_ != 0.0f) {
        const Flt32 rate = repairRate * t;
        if (repairReserve_ < rate) {
            damageLevel_ -= repairReserve_;
            repairReserve_ = 0.0f;
        } else {
            damageLevel_ -= rate;
            repairReserve_ -= rate;
        }
        if (damageLevel_ < 0.0f) {
            damageLevel_ = 0.0f;
            repairReserve_ = 0.0f;
        }
        setMaskBits(DamageMask);
    }
    if (damageLevel_ > 0.0f && autoRepairRate_ > 0.0f) {
        damageLevel_ -= autoRepairRate_ * 0.032f;
        if (damageLevel_ < 0.0f) {
            damageLevel_ = 0.0f;
        }
        setMaskBits(DamageMask);
    }
}

void ShapeBase::applyDamage(Int32 type,
                            Flt32 value,
                            const Point3F& pos,
                            const Point3F& vec,
                            const Point3F& mom,
                            sim::SimObjectId objectId) {
    if (!isGhost()) {
        if (auto* con = resolveConsole()) {
            const std::string cb = scriptName("onDamage");
            if (!cb.empty()) {
                std::array<std::string, 7> argv{scriptThis(),
                                                std::to_string(type),
                                                std::format("{:g}", value),
                                                std::format("{:g} {:g} {:g}", pos.x, pos.y, pos.z),
                                                std::format("{:g} {:g} {:g}", vec.x, vec.y, vec.z),
                                                std::format("{:g} {:g} {:g}", mom.x, mom.y, mom.z),
                                                std::to_string(static_cast<Int32>(objectId))};
                (void)con->execute(cb, argv);
            }
        }
    }
}

bool ShapeBase::getCommandStatus(CommandStatus* status) const {
    if (!status) {
        return false;
    }
    const Flt32 maxDamage =
        (shapeData_ && shapeData_->maxDamage > 0.0f) ? shapeData_->maxDamage : 1.0f;
    const Flt32 damage = damageLevel_ / maxDamage;
    status->damageLevel = (damage > 0.7) ? 2 : (damage > 0.3) ? 1 : 0;
    status->active = true;
    return true;
}

Int32 ShapeBase::getDamageSkinLevel(Int32 detailLevel) const noexcept {
    if (detailLevel == 1 && getDamageLevel() > 0.40) {
        return 2;
    }
    if (detailLevel == 2) {
        if (getDamageLevel() > 0.75) {
            return 3;
        }
        if (getDamageLevel() > 0.50) {
            return 2;
        }
        if (getDamageLevel() > 0.25) {
            return 1;
        }
    }
    return 0;
}

std::string_view ShapeBase::getSkinBase() const noexcept {
    if (const auto* players = PlayerManager::get(getManager())) {
        if (const auto* owner = players->findBaseRep(getLastOwnerClient())) {
            return owner->skinBase;
        }
        return players->getTeamSkinBase(getTeam());
    }
    return "base";
}

void ShapeBase::reSkin(bool force) {
    const std::string_view newSkinBase = getSkinBase();
    const Int32 dlevel = getDamageSkinLevel(prefDamageSkinDetail);
    if (!force && dlevel == curSkinDamage_ && equalIgnoreCase(curSkin_, newSkinBase)) {
        return;
    }
    curSkinDamage_ = dlevel;
    curSkin_.assign(newSkinBase);
    cachedWorldResources_.reset();
}

std::optional<world::TsMaterialResources>
ShapeBase::damageMaterials(world::TsMaterialResources materials) const {
    if (curSkinDamage_ == 0 || !shapeData_ || shapeData_->damageSkinId < 0) {
        return materials;
    }
    auto* dbm = resolveDataBlockManager();
    if (!dbm || !dbm->resources()) {
        return {};
    }
    const auto* skin = dynamic_cast<DamageSkinData*>(
        dbm->lookupDataBlock(shapeData_->damageSkinId, DataBlockManager::DamageSkinDataType));
    if (!skin) {
        return materials;
    }
    return world::TsMaterialResources::withDamageSkin(
        *dbm->resources(), std::move(materials), curSkinDamage_, skin->bmpName);
}

void ShapeBase::setShieldVec(Point3F hv, Flt32 zoff) {
    shieldVec_.x = -hv.x;
    shieldVec_.y = -hv.y;
    shieldVec_.z = -hv.z;
    shieldVec_.normalize();
    zOffset_ = zoff;
    setMaskBits(ShieldMask);
}

void ShapeBase::setShieldThread(Flt32 pos) {
    if (shieldShapeInstance_ && shieldThreadIndex_) {
        const auto threads = shieldShapeInstance_->threads();
        if (*shieldThreadIndex_ < threads.size()) {
            shieldShapeInstance_->setSequence(
                *shieldThreadIndex_, threads[*shieldThreadIndex_].sequence, pos);
        }
    }

    Point3F up{};
    Point3F r0{};
    Point3F r2{};
    shieldVec_.normalize();
    if (shieldVec_.x == 0.0f && shieldVec_.y == 0.0f) {
        up.set(1.0f, 0.0f, 0.0f);
    } else {
        up.set(0.0f, 0.0f, 1.0f);
    }
    m_cross(shieldVec_, up, r0);
    r0.normalize();
    m_cross(shieldVec_, r0, r2);
    r2.normalize();

    shieldImage_.transform.identity();
    shieldImage_.transform.setRow(0, r0);
    shieldImage_.transform.setRow(1, shieldVec_);
    shieldImage_.transform.setRow(2, r2);
    shieldImage_.transform.p = getBoxCenter();
    shieldImage_.transform.p.z += zOffset_;

    const Box3F& box = getBoundingBox();
    const Flt32 ll = (std::max(box.len_x(), box.len_y()) * 0.5f) * 0.70f;
    shieldImage_.transform.p += shieldVec_ * ll;
}

void ShapeBase::updateShieldThread(SimTime t) {
    if (shieldShapeInstance_ && shieldThreadIndex_) {
        shieldShapeInstance_->advanceThread(*shieldThreadIndex_, static_cast<Flt64>(t));
    }
}

void ShapeBase::blowUp() {
    if (shapeData_ && shapeData_->debrisId != -1 && shapeInstance_ && shapeInstance_->shape()) {
        bool foundDebrisDetail = false;
        const auto& shape = *shapeInstance_->shape();
        for (const auto& det : shape.details) {
            if (det.root >= 0 && static_cast<std::size_t>(det.root) < shape.nodes.size()) {
                const Int32 nameIdx = shape.nodes[static_cast<std::size_t>(det.root)].name;
                if (nameIdx >= 0 && static_cast<std::size_t>(nameIdx) < shape.names.size() &&
                    equalIgnoreCase(shape.names[static_cast<std::size_t>(nameIdx)], "debris0")) {
                    foundDebrisDetail = true;
                    break;
                }
            }
        }
        if (!foundDebrisDetail) {
            blowedUpGood_ = true;
        }
    } else {
        blowedUpGood_ = true;
    }
}

void ShapeBase::unBlowUp() {
    blowedUpGood_ = false;
}

void ShapeBase::startFadeOut(UInt32 explicitCurrentTime) {
    const UInt32 now =
        explicitCurrentTime != 0
            ? explicitCurrentTime
            : (getManager() ? static_cast<UInt32>(getManager()->getCurrentTime() * 1000.0f) : 0u);
    if (!isGhost()) {
        m_fading_ = FadingOut;
        m_fadeStarted_ = now;
        setMaskBits(SBFadeMask);
    } else {
        if (m_fading_ == NotFading) {
            m_fadeStarted_ = now;
        } else if (m_fading_ == FadingIn) {
            UInt32 timeDiff = now >= m_fadeStarted_ ? (now - m_fadeStarted_) : 0u;
            if (timeDiff > csm_fadeLength) {
                m_fadeStarted_ = now;
            } else {
                timeDiff = csm_fadeLength - timeDiff;
                m_fadeStarted_ = now >= timeDiff ? (now - timeDiff) : 0u;
            }
        }
        m_fading_ = FadingOut;
    }
}

void ShapeBase::startFadeIn(UInt32 explicitCurrentTime) {
    const UInt32 now =
        explicitCurrentTime != 0
            ? explicitCurrentTime
            : (getManager() ? static_cast<UInt32>(getManager()->getCurrentTime() * 1000.0f) : 0u);
    if (!isGhost()) {
        m_fading_ = FadingIn;
        m_fadeStarted_ = now;
        setMaskBits(SBFadeMask);
    } else {
        if (m_fading_ == FadingOut) {
            UInt32 timeDiff = now >= m_fadeStarted_ ? (now - m_fadeStarted_) : 0u;
            if (timeDiff > csm_fadeLength) {
                m_fadeStarted_ = now;
            } else {
                timeDiff = csm_fadeLength - timeDiff;
                m_fadeStarted_ = now >= timeDiff ? (now - timeDiff) : 0u;
            }
        } else if (m_fading_ == NotFading) {
            m_fadeStarted_ = now;
        }
        m_fading_ = FadingIn;
    }
}

Flt32 ShapeBase::computeFadeFactor(UInt32 currentTime) noexcept {
    if (m_fading_ == NotFading) {
        image_.alphaLevel = 1.0f;
        return 0.0f;
    }
    const UInt32 elapsed = currentTime >= m_fadeStarted_ ? (currentTime - m_fadeStarted_) : 0u;
    Flt32 factor = 0.0f;
    if (m_fading_ == FadingOut) {
        factor = static_cast<Flt32>(elapsed) / static_cast<Flt32>(csm_fadeLength);
        if (factor > 1.0f) {
            factor = 1.0f;
        }
    } else {
        factor = 1.0f - static_cast<Flt32>(elapsed) / static_cast<Flt32>(csm_fadeLength);
        if (factor < 0.0f) {
            factor = 0.0f;
            m_fading_ = NotFading;
        }
    }
    image_.alphaLevel = 1.0f - factor;
    return factor;
}

void ShapeBase::updateSunRollAv() noexcept {
    if (m_attenuateSun_ || m_ticks_ != 0) {
        m_attenuationColor_ *= 0.5f;
        m_attenuationColor_ += m_rollColor_ * 0.5f;
        m_attenuationFactor_ *= 0.5f;
        m_attenuationFactor_ += m_rollFactor_ * 0.5f;
        if (m_ticks_ != 0) {
            --m_ticks_;
        }
    }
}

void ShapeBase::updateSunOverride(bool overrideSun,
                                  Flt32 sunAttenuation,
                                  const ColorF& positionalColor) noexcept {
    if (overrideSun) {
        if (!m_attenuateSun_ && m_ticks_ == 0) {
            m_attenuationColor_ = positionalColor;
        }
        m_ticks_ = 0;
        m_rollFactor_ = sunAttenuation;
        m_rollColor_ = positionalColor;
    } else {
        if (m_attenuateSun_) {
            m_ticks_ = 9;
            m_rollFactor_ = 0.0f;
        } else if (m_ticks_ > 0) {
            --m_ticks_;
        }
    }
    m_attenuateSun_ = overrideSun;
    updateSunRollAv();

    if (m_attenuateSun_ || m_ticks_ > 0) {
        image_.updateSunOverride(m_attenuateSun_, m_attenuationFactor_, m_attenuationColor_);
    } else {
        image_.updateSunOverride(false, 0.0f, ColorF{0.0f, 0.0f, 0.0f});
    }
}

Flt32 ShapeBase::coverage(Point3F eye) const {
    auto* mgr = getManager();
    if (!mgr) {
        return 1.0f;
    }
    auto* root = dynamic_cast<sim::SimContainer*>(mgr->findObject(sim::RootContainerId));
    if (!root) {
        return 1.0f;
    }

    sim::SimContainerQuery query{};
    query.id = getId();
    query.type = AllObjectTypes;
    query.mask = AllObjectTypes;
    query.box.fMin = eye;
    query.box.fMax = getBoxCenter();

    sim::SimCollisionInfo info{};
    return root->findLOS(query, info) ? 0.0f : 1.0f;
}

TMat3F ShapeBase::getLOSTransform() const noexcept {
    TMat3F mat = getTransform();
    mat.p = getBoxCenter();
    return mat;
}

sim::SimObject* ShapeBase::findLOSObject(
    const TMat3F& mat, Flt32 range, UInt32 mask, Point3F* pos, Point3F* normal) const {
    if (pos) {
        pos->set(0.0f, 0.0f, 0.0f);
    }
    if (normal) {
        normal->set(0.0f, 0.0f, 1.0f);
    }
    auto* mgr = getManager();
    if (!mgr) {
        return nullptr;
    }
    auto* root = dynamic_cast<sim::SimContainer*>(mgr->findObject(sim::RootContainerId));
    if (!root) {
        return nullptr;
    }

    sim::SimContainerQuery query{};
    query.id = getId();
    query.type = AllObjectTypes;
    query.mask = mask;
    query.box.fMin = mat.p;
    const Point3F vec{0.0f, range, 0.0f};
    m_mul(vec, mat, query.box.fMax);

    sim::SimCollisionInfo info{};
    if (root->findLOS(query, info) && !info.surfaces.surfaces.empty()) {
        if (pos) {
            m_mul(info.surfaces.surfaces[0].position, info.surfaces.tWorld, *pos);
        }
        if (normal) {
            m_mul(info.surfaces.surfaces[0].normal,
                  static_cast<const RMat3F&>(info.surfaces.tWorld),
                  *normal);
        }
        return info.object.resolve(*mgr);
    }
    return nullptr;
}

bool ShapeBase::aimedTransform(TMat3F* eyeMat, Point3F startPoint) const {
    if (!eyeMat) {
        return false;
    }
    Point3F p{};
    Point3F n{};
    if (findLOSObject(*eyeMat, 1200.0f, AllObjectTypes, &p, &n)) {
        Point3F forward = p - startPoint;
        if (forward.len() >= 2.0f) {
            forward.normalize();
            Point3F right{};
            m_cross(Point3F{0.0f, 0.0f, 1.0f}, forward, right);
            right.normalize();
            Point3F up{};
            m_cross(right, forward, up);
            up.normalize();
            eyeMat->setRow(0, right);
            eyeMat->setRow(1, forward);
            eyeMat->setRow(2, up);
            return true;
        }
    }
    return false;
}

bool ShapeBase::getAimedMuzzleTransform(Int32 slot, TMat3F* mat) const {
    if (!mat) {
        return false;
    }
    TMat3F temp{};
    if (!getMuzzleTransform(slot, &temp)) {
        return false;
    }
    m_mul(temp, getTransform(), *mat);
    const Point3F start = mat->p;
    temp = getEyeTransform();
    if (aimedTransform(&temp, start)) {
        *mat = temp;
        mat->p = start;
    }
    return true;
}

bool ShapeBase::getLOSColor(const Point3F& shootPoint,
                            const Point3F& vec,
                            Flt32 dist,
                            ColorF& outColor,
                            Flt32& outFactor,
                            sim::SimContainer*& outContainer) const {
    outContainer = nullptr;
    outFactor = 0.0f;

    auto* mgr = getManager();
    if (!mgr || dist <= 0.0f) {
        return false;
    }
    auto* root = dynamic_cast<sim::SimContainer*>(mgr->findObject(sim::RootContainerId));
    if (!root) {
        return false;
    }

    sim::SimContainerQuery query{};
    query.id = getId();
    query.mask = SimInteriorObjectType | SimTerrainObjectType;
    query.box.fMin = shootPoint;
    query.box.fMax = shootPoint + vec * dist;

    sim::SimCollisionInfo cInfo{};
    if (!root->findLOS(query, cInfo, sim::SimCollisionImageQuery::High) ||
        cInfo.surfaces.surfaces.empty()) {
        return false;
    }

    auto* collObject = cInfo.object.resolve(*mgr);
    if (!collObject) {
        return false;
    }

    Point3F worldPos{};
    m_mul(cInfo.surfaces.surfaces[0].position, cInfo.surfaces.tWorld, worldPos);
    const Flt32 hitDist = std::fabs(m_dot(shootPoint - worldPos, vec));
    if (hitDist >= dist) {
        outFactor = 0.0f;
        return false;
    }
    outFactor = hitDist < 0.0f ? 1.0f : (dist - hitDist) / dist;
    outContainer = dynamic_cast<sim::SimContainer*>(collObject);
    if (outFactor <= 0.0f || !outContainer) {
        outContainer = nullptr;
        outFactor = 0.0f;
        return false;
    }

    std::optional<UInt16> sampled;
    if (const auto* terrain = dynamic_cast<const world::SimMissionTerrain*>(collObject)) {
        sampled = terrain->sampleLighting(worldPos);
    } else {
        world::WorldSurfaceQuery surface;
        if (collObject->processQuery(surface) && surface.image && surface.image->mesh) {
            Point3F local;
            m_mulInverse(worldPos, surface.image->placement, local);
            sampled = surface.image->mesh->sampleLighting(local);
        }
    }
    if (!sampled) {
        outContainer = nullptr;
        outFactor = 0;
        return false;
    }
    outColor.set(Flt32((*sampled >> 8) & 15) / 15,
                 Flt32((*sampled >> 4) & 15) / 15,
                 Flt32(*sampled & 15) / 15);
    return true;
}

bool ShapeBase::getPositionalColor(ColorF& outColor, Flt32& outSunAttenuation) const {
    outColor.set(0, 0, 0);
    outSunAttenuation = 0;
    if (g_lowDetailObjectLighting) {
        Point3F pPosition = getBoxCenter();
        Point3F vector{};
        getTransform().getRow(2, vector);
        vector.neg();

        const Flt32 dotX = m_dot(Point3F{getBoundingBox().len_x() * 0.5f, 0.0f, 0.0f}, vector);
        const Flt32 dotY = m_dot(Point3F{0.0f, getBoundingBox().len_y() * 0.5f, 0.0f}, vector);
        const Flt32 dotZ = m_dot(Point3F{0.0f, 0.0f, getBoundingBox().len_z() * 0.5f}, vector);

        Flt32 useDot = dotX;
        if (std::fabs(dotY) > std::fabs(useDot)) {
            useDot = dotY;
        }
        if (std::fabs(dotZ) > std::fabs(useDot)) {
            useDot = dotZ;
        }
        pPosition -= vector * std::fabs(useDot);

        sim::SimContainer* pContainer = nullptr;
        return getLOSColor(
            pPosition, vector, 20.0f + std::fabs(useDot), outColor, outSunAttenuation, pContainer);
    }
    return false;
}

bool ShapeBase::onSimObjectTransformEvent(const sim::SimObjectTransformEvent& event) {
    EulerF rot{};
    event.transform.angles(rot);
    setRot(Point3F{rot.x, rot.y, rot.z});
    setPos(event.transform.p);
    return true;
}

bool ShapeBase::onSimObjectTransformQuery(sim::SimObjectTransformQuery& query) const {
    query.transform = getTransform();
    return true;
}

bool ShapeBase::onSimImageTransformQuery(sim::SimImageTransformQuery& query) const {
    query.transform = getTransform();
    return true;
}

bool ShapeBase::onWorldShapeQuery(world::WorldShapeQuery& query) const {
    // A shield hit is a separate, transient image. Legacy emits it while
    // Sequence01 is before its endpoint, even when the body is hidden.
    if (shieldShapeInstance_ && shieldThreadIndex_ &&
        shieldShapeInstance_->threads()[*shieldThreadIndex_].position < 1.0f) {
        if (!cachedShieldResources_) {
            world::TsShapeResources resources;
            resources.shape = shieldShapeInstance_->shape();
            if (resources.shape->materials) {
                auto* dbm = resolveDataBlockManager();
                if (!dbm || !dbm->resources()) {
                    return false;
                }
                auto materials = world::TsMaterialResources::load(*dbm->resources(),
                                                                  *resources.shape->materials);
                if (!materials) {
                    return false;
                }
                resources.materials = std::move(*materials);
            }
            cachedShieldResources_ = std::move(resources);
        }
        const auto pose = shieldShapeInstance_->pose();
        if (!pose) {
            return false;
        }
        world::WorldShapeImage shield;
        shield.resources = *cachedShieldResources_;
        shield.pose = *pose;
        // TS poses keep node zero separate. Legacy ShapeInstance::render
        // applies its default root transform before the hit placement.
        const auto& shape = *shield.resources.shape;
        const auto& root = shape.transforms[shape.nodes[0].defaultTransform];
        TMat3F transform;
        m_mul(TMat3F(root.rotation, root.translation), shieldImage_.transform, transform);
        // The hit basis is deliberately reflected, as in the native effect.
        shield.shapeToWorld = ensureRigidTransform(transform, true);
        query.attachments.push_back(std::move(shield));
    }
    if (blowedUpGood_ || !shapeInstance_ || !shapeInstance_->shape()) {
        return !query.attachments.empty();
    }

    if (!cachedWorldResources_) {
        world::TsShapeResources resources;
        resources.shape = shapeInstance_->shape();
        if (resources.shape->materials) {
            auto* dbm = resolveDataBlockManager();
            if (!dbm || !dbm->resources()) {
                return false;
            }
            auto materials = world::TsMaterialResources::loadSkin(
                *dbm->resources(),
                *resources.shape->materials,
                getSkinBase(),
                shapeData_ ? shapeData_->fileName : "");
            if (!materials) {
                return false;
            }
            materials = damageMaterials(std::move(*materials));
            if (!materials) {
                return false;
            }
            resources.materials = std::move(*materials);
        }
        cachedWorldResources_ = std::move(resources);
    }
    const auto poseOpt = shapeInstance_->pose();
    if (!poseOpt) {
        return false;
    }

    world::WorldShapeImage img{};
    img.resources = *cachedWorldResources_;
    img.pose = *poseOpt;
    img.shapeToWorld = ensureRigidTransform(getTransform());
    img.detail = 0;
    img.automaticDetail = true;
    img.haze = query.cameraPosition && getContainer() ? getHaze(*query.cameraPosition)
                                                      : std::clamp(image_.hazeValue, 0.0f, 1.0f);
    img.opacity = std::clamp(image_.alphaLevel, 0.f, 1.f);
    img.texturePerspective = shapeData_ && shapeData_->isPerspective;
    img.castShadow =
        shapeData_ && (shapeData_->shadowDetailMask & prefShadowDetailMask) && img.haze <= .2f;
    if (image_.m_attenuateSun) {
        img.sunOverride = world::WorldShapeImage::SunOverride{image_.m_attenuationFactor,
                                                              image_.m_attenuationColor};
    }
    query.image = std::move(img);
    return true;
}

bool ShapeBase::processEvent(const sim::SimEvent& event) {
    if (const auto* sound = dynamic_cast<const SoundEvent*>(&event)) {
        if (sound->channel < 0 || sound->channel >= MaxSoundChannels) {
            return false;
        }
        auto& handle = soundChannel_[static_cast<std::size_t>(sound->channel)];
        auto* playback =
            getManager()
                ? dynamic_cast<SoundPlayback*>(getManager()->findObject("GameSoundPlayback"))
                : nullptr;
        if (handle && playback && playback->stop) {
            playback->stop(handle);
        }
        handle = TSFX::PlayAt(
            static_cast<UInt32>(sound->soundId), getTransform(), getLinearVelocity(), getManager());
        return true;
    }
    if (const auto* xformEv = dynamic_cast<const sim::SimObjectTransformEvent*>(&event)) {
        return onSimObjectTransformEvent(*xformEv);
    }
    return Parent::processEvent(event);
}

bool ShapeBase::processQuery(sim::SimQuery& query) {
    if (auto* objXformQ = dynamic_cast<sim::SimObjectTransformQuery*>(&query)) {
        return onSimObjectTransformQuery(*objXformQ);
    }
    if (auto* imgXformQ = dynamic_cast<sim::SimImageTransformQuery*>(&query)) {
        return onSimImageTransformQuery(*imgXformQ);
    }
    if (auto* shapeQ = dynamic_cast<world::WorldShapeQuery*>(&query)) {
        reSkin();
        const auto now = getManager() ? UInt32(getManager()->getCurrentTime() * 1000.f) : 0U;
        (void)computeFadeFactor(now);
        if (!m_lastSunUpdated_ || now - m_lastSunUpdated_ >= 32) {
            ColorF color{};
            Flt32 factor{};
            const auto overrideSun = getPositionalColor(color, factor);
            updateSunOverride(overrideSun, factor, color);
            m_lastSunUpdated_ = now;
        }
        return onWorldShapeQuery(*shapeQ);
    }
    return Parent::processQuery(query);
}

bool ShapeBase::packShapeBaseUpdate(UInt32 mask, BitStream& stream, UInt32 currentTime) const {
    if (!packGameBaseUpdate(mask, stream)) {
        return false;
    }

    if ((mask & InitialUpdate) != 0) {
        mask &= ~ShieldMask;
        mask &= ~ThreadMask;
        for (Int32 i = 0; i < MaxScriptThreads; ++i) {
            if (scriptThread_[static_cast<std::size_t>(i)].sequence != -1) {
                mask |= ThreadMask;
                break;
            }
        }
    }

    if (stream.writeFlag((mask & ShieldMask) != 0)) {
        const NormalVector3F nv{shieldVec_.x, shieldVec_.y, shieldVec_.z};
        stream.writeNormalVector(nv, 7);
        Flt32 zOff = zOffset_;
        if (zOff > 5.0f) {
            zOff = 5.0f;
        }
        if (zOff < -5.0f) {
            zOff = -5.0f;
        }
        zOff = (zOff + 5.0f) / 10.0f;
        stream.writeFloat(zOff, 8);
    }

    if (stream.writeFlag((mask & ThreadMask) != 0)) {
        for (Int32 i = 0; i < MaxScriptThreads; ++i) {
            const auto& st = scriptThread_[static_cast<std::size_t>(i)];
            if (stream.writeFlag(st.sequence != -1 && (mask & (ThreadNMask << i)) != 0)) {
                stream.writeInt(st.sequence, ThreadSequenceBits);
                stream.writeInt(st.state, 2);
                stream.writeFlag(st.forward);
                stream.writeFlag(st.atEnd);
            }
        }
    }

    const UInt32 now =
        currentTime != 0
            ? currentTime
            : (getManager() ? static_cast<UInt32>(getManager()->getCurrentTime() * 1000.0f) : 0u);
    if (m_fading_ == FadingIn) {
        if (m_fadeStarted_ + csm_fadeLength <= now) {
            m_fading_ = NotFading;
        }
    }
    if (stream.writeFlag(m_fading_ != NotFading)) {
        stream.writeFlag(m_fading_ == FadingIn);
    }

    return stream.isValid();
}

bool ShapeBase::unpackShapeBaseUpdate(BitStream& stream, UInt32 currentTime) {
    BitStreamRollbackGuard streamGuard(stream);
    const Int32 savedTeam = teamId_;
    const Int32 savedCtrl = ctrlClientId_;
    const Int32 savedOwner = ownerClientId_;
    const Int32 savedLastOwner = lastOwnerClientId_;
    const Point3F savedShieldVec = shieldVec_;
    const Flt32 savedZOffset = zOffset_;
    const auto savedThreads = scriptThread_;
    const FadeState savedFading = m_fading_;
    const UInt32 savedFadeStarted = m_fadeStarted_;

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
        return false;
    };

    if (!unpackGameBaseUpdate(stream)) {
        return rollback();
    }

    if (stream.readFlag()) {
        NormalVector3F nv{};
        stream.readNormalVector(nv, 7);
        const Flt32 zOff = (stream.readFloat(8) * 10.0f) - 5.0f;
        if (!stream.isValid()) {
            return rollback();
        }
        shieldVec_.set(nv.x, nv.y, nv.z);
        zOffset_ = zOff;
        setShieldThread(0.0f);
    }

    if (stream.readFlag()) {
        for (Int32 i = 0; i < MaxScriptThreads; ++i) {
            if (stream.readFlag()) {
                auto& st = scriptThread_[static_cast<std::size_t>(i)];
                const Int32 seq = stream.readInt(ThreadSequenceBits);
                const Int32 stState = stream.readInt(2);
                const bool fwd = stream.readFlag();
                const bool endFlag = stream.readFlag();
                if (!stream.isValid()) {
                    return rollback();
                }
                st.state = stState;
                st.forward = fwd;
                st.atEnd = endFlag;
                if (st.sequence != seq) {
                    setSequence(i, seq, false);
                } else {
                    updateSequence(st);
                }
            }
        }
    }

    if (stream.readFlag()) {
        const bool isFadeIn = stream.readFlag();
        if (!stream.isValid()) {
            return rollback();
        }
        if (!isFadeIn) {
            startFadeOut(currentTime);
        } else {
            startFadeIn(currentTime);
        }
    } else {
        if (!stream.isValid()) {
            return rollback();
        }
        m_fading_ = NotFading;
    }

    if (!stream.isValid()) {
        return rollback();
    }

    streamGuard.committed = true;
    return true;
}

UInt32
ShapeBase::packUpdate(sim::network::GhostManager& /*gm*/, UInt32 mask, BitStream& stream) const {
    packShapeBaseUpdate(mask, stream);
    return 0;
}

bool ShapeBase::unpackUpdate(sim::network::GhostManager& /*gm*/, BitStream& stream) {
    return unpackShapeBaseUpdate(stream);
}

Persistent::Error
ShapeBase::read(StreamIO& sio, Int32 /*version*/, Int32 /*user*/, Persistent::LoadContext& ctx) {
    const Int32 startPos = sio.getPosition();
    const auto savedId = getId();
    const auto savedNetFlags = netFlags();
    const auto savedLocked = isLocked();
    const auto savedTeam = teamId_;
    const auto savedDatFileName = datFileName_;
    const auto savedDestroyable = destroyable_;
    const auto savedRot = rotation_;
    const auto savedTransform = getTransform();

    const auto rollback = [&]() noexcept {
        setId(savedId);
        setNetFlags(savedNetFlags);
        setLocked(savedLocked);
        teamId_ = savedTeam;
        datFileName_ = savedDatFileName;
        destroyable_ = savedDestroyable;
        rotation_ = savedRot;
        SimMovement::setPosition(savedTransform, true);
        sio.setPosition(startPos);
        return Persistent::Error::ReadError;
    };

    Int32 fileVersion = 0;
    if (!sio.read(fileVersion)) {
        return rollback();
    }
    if (Parent::read(sio, 0, 0, ctx) != Persistent::Error::Ok) {
        return rollback();
    }
    if (fileVersion > 0) {
        if (!sio.read(destroyable_)) {
            return rollback();
        }
    }
    Point3F pos{};
    Point3F rot{};
    if (!sio.read(pos.x) || !sio.read(pos.y) || !sio.read(pos.z) || !sio.read(rot.x) ||
        !sio.read(rot.y) || !sio.read(rot.z)) {
        return rollback();
    }
    setRot(rot);
    setPos(pos);

    return sio.getStatus() == STRM_OK ? Persistent::Error::Ok : rollback();
}

Persistent::Error ShapeBase::write(StreamIO& sio,
                                   Int32 /*version*/,
                                   Int32 /*user*/,
                                   Persistent::StoreContext& ctx) const {
    const Int32 startPos = sio.getPosition();
    const Int32 fileVersion = 1;
    const Point3F& pos = getPos();
    const Point3F& rot = getRot();

    if (!sio.write(fileVersion) || Parent::write(sio, 0, 0, ctx) != Persistent::Error::Ok ||
        !sio.write(destroyable_) || !sio.write(pos.x) || !sio.write(pos.y) || !sio.write(pos.z) ||
        !sio.write(rot.x) || !sio.write(rot.y) || !sio.write(rot.z)) {
        sio.setPosition(startPos);
        return Persistent::Error::WriteError;
    }
    return sio.getStatus() == STRM_OK ? Persistent::Error::Ok : Persistent::Error::WriteError;
}

} // namespace opent1::game
