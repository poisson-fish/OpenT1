#include <opent1/game/debris.hpp>
#include <opent1/world/scene_objects.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <numbers>

namespace opent1::game {

namespace {
constexpr Flt32 kPi = static_cast<Flt32>(std::numbers::pi);
constexpr Flt32 kTwoPi = static_cast<Flt32>(2.0 * std::numbers::pi);
constexpr UInt32 kNormalizeMask = 0x1Fu;
constexpr std::size_t kNumRandomRotations = 32;

Random g_dbRand{};
bool g_rotationsInitialized = false;
std::array<RMat3F, kNumRandomRotations> g_randomRotations{};

void ensureRandomRotations() noexcept {
    if (g_rotationsInitialized) {
        return;
    }
    Random rotRand{};
    for (std::size_t i = 0; i < kNumRandomRotations; ++i) {
        const EulerF e{
            rotRand.getFloat(kTwoPi), rotRand.getFloat(kTwoPi), rotRand.getFloat(kTwoPi)};
        g_randomRotations[i].set(e);
    }
    g_rotationsInitialized = true;
}

template <class T> void writePod(BitStream& s, const T& val) {
    static_assert(std::is_trivially_copyable_v<T>);
    std::array<std::byte, sizeof(T)> bytes{};
    std::memcpy(bytes.data(), &val, sizeof(T));
    s.writeBytes(bytes);
}

template <class T> bool readPod(BitStream& s, T& val) {
    static_assert(std::is_trivially_copyable_v<T>);
    std::array<std::byte, sizeof(T)> bytes{};
    if (!s.readBytes(bytes) || !s.isValid()) {
        return false;
    }
    std::memcpy(&val, bytes.data(), sizeof(T));
    return true;
}

bool isEqualFloat(Flt32 a, Flt32 b, Flt32 tol = 1e-5f) noexcept {
    return std::fabs(a - b) <= tol;
}

bool readUnitVectorSmall(BitStream& s, Point3F& v) noexcept {
    UInt8 vIndex = 0;
    if (!readPod(s, vIndex)) {
        return false;
    }
    const Flt32 angle1 = static_cast<Flt32>(vIndex % 16u) * (kTwoPi / 16.0f);
    const Flt32 angle2 = static_cast<Flt32>(vIndex >> 4u) * (kPi / 16.0f) - 0.5f * kPi;
    v.x = std::cos(angle1) * std::cos(angle2);
    v.y = std::sin(angle1) * std::cos(angle2);
    v.z = std::sin(angle2);
    return true;
}

void writeUnitVectorSmall(BitStream& s, const Point3F& v) {
    Flt32 angle1 = 0.0f;
    Flt32 angle2 = 0.0f;
    if (!isEqualFloat(v.x, 0.0f)) {
        angle1 = m_atan(v.x, v.y);
    } else if (v.y > 0.0f) {
        angle1 = kPi * 0.5f;
    } else {
        angle1 = 1.5f * kPi;
    }

    const Flt32 L = std::sqrt(v.x * v.x + v.y * v.y);
    if (L != 0.0f) {
        angle2 = m_atan(L, v.z);
    } else if (v.z > 0.0f) {
        angle2 = 0.5f * kPi;
    } else {
        angle2 = -0.5f * kPi;
    }

    if (angle1 < 0.0f) {
        angle1 += kTwoPi;
    }
    angle2 += 0.5f * kPi;

    Int32 a2I = static_cast<Int32>(16.0f * angle2 / kPi);
    if (a2I > 15) {
        a2I = 15;
    }
    const UInt8 vIndex = static_cast<UInt8>(static_cast<UInt8>(16.0f * angle1 / kTwoPi) +
                                            (static_cast<UInt8>(a2I) << 4u));
    writePod(s, vIndex);
}

bool readVectorSmall(BitStream& s, Point3F& v, Flt32 precision) noexcept {
    UInt8 absV = 0;
    if (!readPod(s, absV)) {
        return false;
    }
    if (absV == 0) {
        v.set(0.0f, 0.0f, 0.0f);
        return true;
    }
    if (!readUnitVectorSmall(s, v)) {
        return false;
    }
    v *= precision * static_cast<Flt32>(absV);
    return true;
}

void writeVectorSmall(BitStream& s, const Point3F& v, Flt32 precision) {
    const Flt32 absV = v.len();
    UInt8 mag = 0;
    if (absV > 255.0f * precision) {
        mag = 255;
    } else {
        mag = static_cast<UInt8>(absV / precision);
    }
    writePod(s, mag);
    if (mag != 0) {
        Point3F tmpV = v;
        tmpV *= 1.0f / absV;
        writeUnitVectorSmall(s, tmpV);
    }
}

bool readFloatSmall(BitStream& s, Flt32& f, Flt32 precision) noexcept {
    UInt8 num = 0;
    if (!readPod(s, num)) {
        return false;
    }
    f = precision * static_cast<Flt32>(num);
    return true;
}

void writeFloatSmall(BitStream& s, Flt32 f, Flt32 precision) {
    UInt8 num = 0;
    if (f > precision * 255.0f) {
        num = 255;
    } else if (f <= 0.0f) {
        num = 0;
    } else {
        num = static_cast<UInt8>(f / precision);
    }
    writePod(s, num);
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
// DebrisTable
// -----------------------------------------------------------------------------

Flt32 DebrisTable::getShortList(const DataBlockManager& dbm,
                                Flt32 mass,
                                Int32 typeMask,
                                ShortList& sl) {
    sl.clear();
    Flt32 p = 0.0f;
    const Int32 count = dbm.getDataGroupSize(DataBlockManager::DebrisDataType);
    for (Int32 i = 0; i < count; ++i) {
        const auto* pData = dynamic_cast<const DebrisData*>(
            dbm.lookupDataBlock(i, DataBlockManager::DebrisDataType));
        if (!pData) {
            continue;
        }
        if (pData->mass > mass) {
            break;
        }
        if ((pData->type & typeMask) != 0) {
            sl.push_back(ShortListEntry{pData->mass, i, pData->p});
            p += pData->p;
        }
    }
    return p;
}

// -----------------------------------------------------------------------------
// Debris
// -----------------------------------------------------------------------------

void Debris::resetRandomSeed(UInt32 seed) noexcept {
    g_dbRand.setSeed(seed);
    g_rotationsInitialized = false;
    ensureRandomRotations();
}

Random& Debris::sharedRandom() noexcept {
    return g_dbRand;
}

Debris::Debris() : collisionImage_(std::make_shared<ShapeCollisionImage>()) {
    setType(SimDebrisObjectType);
    setNetFlags(0);
    setMovementFlags(UseAngularVelocity | UseDrag | UseCurrent | RotateBoundingBox);
    setForceMask(sim::MovementGravityForce | sim::MovementBuoyancyForce);
}

Debris::Debris(Int32 index, Flt32 spawnTO, bool ghost) : Debris() {
    datFileId_ = index;
    spawnTimeout_ = spawnTO;
    ghostMe_ = ghost;
    if (ghostMe_) {
        setNetFlags(Ghostable);
    }
}

Debris::Debris(std::string_view shapeFile,
               Int32 index,
               Flt32 spawnTO,
               bool ghost,
               Flt32 mass,
               Int32 animSeq,
               Flt32 restartTime)
    : Debris(index, spawnTO, ghost) {
    shapeName_.assign(shapeFile);
    debrisMass_ = mass;
    animationSequence_ = animSeq;
    animRestartTime_ = restartTime;
}

Debris::Debris(const Debris& other)
    : Debris(other.shapeName_,
             other.datFileId_,
             other.spawnTimeout_,
             false,
             other.debrisMass_,
             other.animationSequence_,
             other.animRestartTime_) {
    SimMovement::setPosition(other.getTransform(), true);
    setLinearVelocity(other.getLinearVelocity());
    setAngularVelocity(other.getAngularVelocity());
    boundDbm_ = other.boundDbm_;
    boundConsole_ = other.boundConsole_;
}

Debris::~Debris() {
    image_.clearShapeInstance();
    if (collisionImage_) {
        collisionImage_->clearShapeInstance();
    }
}

bool Debris::initResources(GameBaseData* in_data) {
    if (!Parent::initResources(in_data)) {
        return false;
    }
    m_debrisData_ = dynamic_cast<DebrisData*>(in_data);
    return m_debrisData_ != nullptr;
}

bool Debris::onAdd() {
    if (!Parent::onAdd()) {
        return false;
    }
    if (datFileId_ != -1) {
        finishAddToManager();
    }
    return true;
}

void Debris::finishAddToManager() {
    if (!m_debrisData_ && datFileId_ != -1) {
        loadDatFile();
    }
    if (!m_debrisData_) {
        return;
    }

    if (shapeName_.empty()) {
        shapeName_ = m_debrisData_->pShapeName;
    }
    if (m_debrisData_->shape) {
        shape_ = m_debrisData_->shape;
    } else if (!shapeName_.empty() && getManager()) {
        if (auto* sr = sim::SimResource::find(*getManager())) {
            shape_ = sr->get().load<world::TsShape>(shapeName_);
        }
    }

    if (shape_) {
        shapeInstance_ = std::make_shared<world::TsShapeInstance>(shape_);
        image_.setShapeInstance(shapeInstance_.get(), shapeInstance_);
        const Int32 seq =
            (animationSequence_ >= 0) ? animationSequence_ : m_debrisData_->animationSequence;
        if (seq >= 0 && static_cast<std::size_t>(seq) < shape_->sequences.size()) {
            animThread_ = shapeInstance_->addThread(seq, 0.0f);
            if (animRestartTime_ > 0.0f) {
                reanimateTime_ = animRestartTime_;
            } else if (m_debrisData_->animRestartTime > 0.0f) {
                reanimateTime_ = m_debrisData_->animRestartTime;
            }
        }
        radii_ = (shape_->bounds.fMax - shape_->bounds.fMin) * 0.5f;
        collisionImage_->bbox = shape_->bounds;
        collisionImage_->sphere.center = shape_->center;
        collisionImage_->sphere.radius = shape_->radius;
        collisionImage_->setShapeInstance(shapeInstance_.get(), 0, shapeInstance_);
        setLocalBoundingBox(shape_->bounds);
    } else {
        radii_.set(0.5f, 0.5f, 0.5f);
        const Box3F defaultBox{Point3F{-0.5f, -0.5f, -0.5f}, Point3F{0.5f, 0.5f, 0.5f}};
        collisionImage_->bbox = defaultBox;
        collisionImage_->sphere.center.set(0.0f, 0.0f, 0.0f);
        collisionImage_->sphere.radius = 0.866f;
        setLocalBoundingBox(defaultBox);
    }

    const Flt32 massVal = (debrisMass_ > 0.0f) ? debrisMass_ : m_debrisData_->mass;
    debrisMass_ = massVal;
    setMass(std::max(0.01f, massVal));
    elastic_ = m_debrisData_->elasticity;
    setElasticity(elastic_);
    setFriction(m_debrisData_->friction);
    setCollisionMask(static_cast<UInt32>(m_debrisData_->collisionMask));
    damage_ = m_debrisData_->damage;
    spawnImpulse_ = m_debrisData_->spawnedDebrisStrength;
    spawnedExplosionId_ = m_debrisData_->spawnedExplosionID;

    debrisTime_ = getManager() ? getManager()->getCurrentTime() : 0.0f;
    lastUpdateTime_ = debrisTime_;
    timeout_ =
        debrisTime_ + g_dbRand.getFloat(m_debrisData_->minTimeout, m_debrisData_->maxTimeout);

    if (!inCloud_ && getManager()) {
        if (auto* root =
                dynamic_cast<sim::SimContainer*>(getManager()->findObject(sim::RootContainerId))) {
            root->addObject(*this);
        }
        addToSet(sim::TimerSetId);
    }
    if (ghostMe_) {
        setMaskBits(netInit);
    }
}

void Debris::onRemove() {
    if (auto* cnt = getContainer()) {
        cnt->removeObject(*this);
    }
    Parent::onRemove();
}

bool Debris::onSimTimerEvent(const sim::SimTimerEvent& event) {
    if (!m_debrisData_) {
        return false;
    }
    const SimTime curTime =
        getManager() ? getManager()->getCurrentTime() : (lastUpdateTime_ + event.timerInterval);

    if (expire_ || damage_ < 0.0f ||
        (hitThisTime_ && g_dbRand.getFloat() < m_debrisData_->explodeOnBounce) ||
        (isAtRest() && m_debrisData_->explodeOnRest)) {
        onSimMessageEvent(sim::SimMessageEvent{});
        return true;
    }
    hitThisTime_ = false;
    if (isAtRest()) {
        if (curTime > timeout_) {
            expire_ = true;
        }
        return true;
    }

    lastUpdateTime_ = curTime;
    const SimTime delta = event.timerInterval;

    if (shapeInstance_ && animThread_) {
        if (reanimateTime_ >= 0.0f && curTime > reanimateTime_) {
            shapeInstance_->setSequence(*animThread_, 0, 0.0f);
            reanimateTime_ = curTime + ((animRestartTime_ > 0.0f) ? animRestartTime_
                                                                  : m_debrisData_->animRestartTime);
        } else {
            shapeInstance_->advanceThread(*animThread_, static_cast<Flt64>(delta));
        }
    }

    setTimeSlice(delta);
    underSurfaceThisTime_ = false;

    TMat3F mat = updateMovement();

    Point3F lVel = getLinearVelocity();
    if (potentialEnergyDeficit_ > 0.0f) {
        if (0.5f * lVel.z > potentialEnergyDeficit_) {
            lVel.z -= potentialEnergyDeficit_;
            potentialEnergyDeficit_ = 0.0f;
        } else if (0.5f * lVel.z < -potentialEnergyDeficit_) {
            lVel.z += potentialEnergyDeficit_;
            potentialEnergyDeficit_ = 0.0f;
        } else {
            potentialEnergyDeficit_ -= 0.5f * std::fabs(lVel.z);
            lVel.z *= 0.5f;
        }
        setLinearVelocity(lVel);
    }

    if ((timerCounter_++ & kNormalizeMask) != 0) {
        mat.normalize();
    }

    setPosition(mat);

    lVel = getLinearVelocity();
    const Point3F aVel = getAngularVelocity();

    avgLV_ *= 0.9f;
    avgLV_.x += 0.1f * lVel.x;
    avgLV_.y += 0.1f * lVel.y;
    avgLV_.z += 0.1f * lVel.z;
    avgLV_.z *= 0.25f;

    avgAV_ *= 0.9f;
    avgAV_.x += 0.1f * aVel.x;
    avgAV_.y += 0.1f * aVel.y;
    avgAV_.z += 0.1f * aVel.z;

    if (curTime > debrisTime_ + 1.0f && hitThisTime_ && m_dot(avgLV_, avgLV_) < 0.1f &&
        m_dot(avgAV_, avgAV_) < 0.1f) {
        setLinearVelocity(Point3F{0.0f, 0.0f, 0.0f});
        setAngularVelocity(Point3F{0.0f, 0.0f, 0.0f});
        setMovementFlags(movementFlags() | AtRest);
    }

    if (curTime > timeout_) {
        expire_ = true;
    }
    if (ghostMe_) {
        setMaskBits(netUpdate);
    }
    underSurfaceLastTime_ = underSurfaceThisTime_;
    return true;
}

bool Debris::onSimMessageEvent(const sim::SimMessageEvent&) {
    const SimTime now = getManager() ? getManager()->getCurrentTime() : lastUpdateTime_;
    const Flt32 elapsed = now - debrisTime_;
    spawnTimeout_ -= elapsed;
    expire_ = true;

    if (spawnTimeout_ > 0.0f && m_debrisData_ && getManager()) {
        if (spawnedExplosionId_ != -1) {
            auto exp = std::make_unique<Explosion>(spawnedExplosionId_);
            exp->bindDataBlockManager(resolveDataBlockManager());
            exp->setPosition(getTransform().p);
            getManager()->addObject(std::move(exp));
        }
    }
    if (getManager() && !inCloud_) {
        deleteObject();
    }
    return true;
}

bool Debris::bounceSphere(const Point3F& normal) {
    Point3F lVel = getLinearVelocity();
    const Flt32 dot = m_dot(normal, lVel);
    if (dot > 0.0f) {
        return false;
    }

    Point3F lv = normal;
    lv *= dot;
    lVel -= lv;

    Point3F fv = lVel;
    const Flt32 f = -dot * coefficients().friction;
    const Flt32 len = fv.len();
    if (len > f && len > 0.0f) {
        fv *= f / len;
    }
    lVel -= fv;

    lv *= elastic_;
    lVel -= lv;
    setLinearVelocity(lVel);

    Point3F aVel = getAngularVelocity();
    const Flt32 avgAVel = 1.5f * (std::fabs(aVel.x) + std::fabs(aVel.y) + std::fabs(aVel.z)) / 3.0f;
    aVel.x = g_dbRand.getFloat(-avgAVel, avgAVel);
    aVel.y = g_dbRand.getFloat(-avgAVel, avgAVel);
    aVel.z = g_dbRand.getFloat(-avgAVel, avgAVel);
    setAngularVelocity(aVel);
    return true;
}

bool Debris::bounce(TMat3F& objectToWorld, Point3F& normal, Flt32 k) {
    k -= sinkDepth_;

    Point3F boxCenter{};
    m_mul(collisionImage_->sphere.center, objectToWorld, boxCenter);

    const Flt32 dist = std::fabs(k - m_dot(normal, boxCenter));
    if (dist > collisionImage_->sphere.radius) {
        return false;
    }

    if (m_debrisData_ && m_debrisData_->collisionDetail == SphereCollisions) {
        return bounceSphere(normal);
    }

    Point3F v1{};
    Point3F v2{};
    Point3F v3{};
    objectToWorld.getRow(0, v1);
    objectToWorld.getRow(1, v2);
    objectToWorld.getRow(2, v3);

    const Flt32 penetration = std::fabs(m_dot(normal, v1) * radii_.x) +
                              std::fabs(m_dot(normal, v2) * radii_.y) +
                              std::fabs(m_dot(normal, v3) * radii_.z) - dist;
    if (penetration < 0.0f) {
        return false;
    }

    if (isAtRest()) {
        setLinearVelocity(Point3F{0.0f, 0.0f, 0.0f});
        setAngularVelocity(Point3F{0.0f, 0.0f, 0.0f});
        objectToWorld = getTransform();
        return true;
    }

    Point3F pushPoint{(m_dot(v1, normal) < 0.0f) ? radii_.x : -radii_.x,
                      (m_dot(v2, normal) < 0.0f) ? radii_.y : -radii_.y,
                      (m_dot(v3, normal) < 0.0f) ? radii_.z : -radii_.z};

    const Flt32 dmgThresh = m_debrisData_ ? m_debrisData_->damageThreshold : 1.0f;
    const Flt32 damageTaken = m_dot(getLinearVelocity(), normal) - dmgThresh;
    if (damageTaken > 0.0f) {
        damage_ -= damageTaken;
    }

    Point3F worldPushPoint{};
    m_mul(pushPoint, objectToWorld, worldPushPoint);
    Parent::collide(worldPushPoint, normal);

    worldPushPoint -= objectToWorld.p;
    Point3F pointVel = getVelocity(worldPushPoint);
    const Flt32 pdot = m_dot(pointVel, normal);
    pointVel -= normal * pdot;
    pointVel *= -coefficients().friction * coefficients().mass;
    applyImpulse(worldPushPoint, pointVel);

    if (penetration > 0.0f && normal.z > 0.5f) {
        if (timerCounter_ == 0) {
            sinkDepth_ += penetration;
        } else if (underSurfaceLastTime_) {
            Point3F putBack = normal * penetration;
            objectToWorld.p += putBack;
            const Point3F lv = getLinearVelocity();
            const Point3F gravity =
                getForce(sim::MovementGravityForce).value_or(Point3F{0.0f, 0.0f, -9.8f});
            const Flt32 lzSq = lv.z * lv.z + putBack.z * gravity.z;
            potentialEnergyDeficit_ += std::sqrt(std::fabs(lzSq));
        }
        underSurfaceThisTime_ = true;
    }
    return true;
}

bool Debris::bounce(sim::SimMovement::MovementInfo& minfo) {
    bool hit = false;
    for (auto& ci : minfo.collisionList) {
        if (auto* mgr = getManager()) {
            if (auto* obj = ci.object.resolve(*mgr)) {
                if (m_debrisData_ &&
                    (obj->getType() & static_cast<UInt32>(m_debrisData_->knockMask)) != 0) {
                    if (auto* sm = dynamic_cast<sim::SimMovement*>(obj)) {
                        const Point3F& addVel = sm->getLinearVelocity();
                        const Flt32 velDot = m_dot(addVel, getLinearVelocity());
                        setLinearVelocity(addVel * 1.25f);
                        if (velDot > 0.75f * m_dot(addVel, addVel)) {
                            return false;
                        }
                    }
                    expire_ = true;
                    return false;
                }
            }
        }
        for (const auto& surface : ci.surfaces.surfaces) {
            if (surface.distance < 0.0f) {
                continue;
            }
            Point3F normal{};
            m_mul(surface.normal, static_cast<const RMat3F&>(ci.surfaces.tWorld), normal);
            Point3F hitPoint{};
            m_mul(surface.position, ci.surfaces.tWorld, hitPoint);
            if (bounce(minfo.target, normal, m_dot(normal, hitPoint))) {
                hit = true;
            }
        }
    }
    return hit;
}

bool Debris::processCollision(MovementInfo& moveInfo) {
    if (bounce(moveInfo)) {
        hitThisTime_ = true;
    }
    return false;
}

std::shared_ptr<sim::SimCollisionImage> Debris::getSelfImage() {
    collisionImage_->transform = getTransform();
    return collisionImage_;
}

bool Debris::processEvent(const sim::SimEvent& event) {
    if (const auto* te = dynamic_cast<const sim::SimTimerEvent*>(&event)) {
        return onSimTimerEvent(*te);
    }
    if (const auto* me = dynamic_cast<const sim::SimMessageEvent*>(&event)) {
        return onSimMessageEvent(*me);
    }
    return Parent::processEvent(event);
}

bool Debris::processQuery(sim::SimQuery& query) {
    if (auto* ciq = dynamic_cast<sim::SimCollisionImageQuery*>(&query)) {
        collisionImage_->transform = getTransform();
        ciq->images.push_back(collisionImage_);
        return true;
    }
    if (auto* shapeQ = dynamic_cast<world::WorldShapeQuery*>(&query)) {
        if (!shapeInstance_) {
            return false;
        }
        if (const auto poseOpt = shapeInstance_->pose()) {
            world::WorldShapeImage img{};
            img.pose = *poseOpt;
            img.shapeToWorld = getTransform();
            img.detail = 0;
            img.automaticDetail = true;
            shapeQ->image = std::move(img);
            return true;
        }
        return false;
    }
    return Parent::processQuery(query);
}

bool Debris::packDebrisUpdate(UInt32 mask, BitStream& stream) const {
    if (!packGameBaseUpdate(mask, stream)) {
        return false;
    }
    writePod(stream, getTransform());
    writePod(stream, getLinearVelocity());
    writePod(stream, getAngularVelocity());

    if ((mask & netInit) != 0) {
        stream.writeFlag(true);
    } else {
        stream.writeFlag(false);
        return stream.isValid();
    }

    packDatFile(stream);
    writePod(stream, spawnTimeout_);
    stream.writeString(shapeName_);

    if (debrisMass_ > 0.0f) {
        stream.writeFlag(true);
        const auto massI = static_cast<UInt8>(debrisMass_ / 50.0f);
        writePod(stream, massI);
    } else {
        stream.writeFlag(false);
    }

    if (animationSequence_ > 0) {
        stream.writeFlag(true);
        const auto seqBits = static_cast<UInt8>(animationSequence_ & 0xF);
        stream.writeBits(
            4, std::span<const std::byte>(reinterpret_cast<const std::byte*>(&seqBits), 1));
    } else {
        stream.writeFlag(false);
    }

    if (animRestartTime_ > 0.0f) {
        stream.writeFlag(true);
        const auto atimeI = static_cast<UInt8>(animRestartTime_ * 10.0f);
        writePod(stream, atimeI);
    } else {
        stream.writeFlag(false);
    }

    return stream.isValid();
}

bool Debris::unpackDebrisUpdate(BitStream& stream) {
    BitStreamRollbackGuard guard(stream);

    const auto savedTeam = teamId_;
    const auto savedCtrl = ctrlClientId_;
    const auto savedOwner = ownerClientId_;
    const auto savedLastOwner = lastOwnerClientId_;
    const TMat3F savedTrans = getTransform();
    const Point3F savedVel = getLinearVelocity();
    const Point3F savedAv = getAngularVelocity();
    const Int32 savedDatId = datFileId_;
    const Flt32 savedSpawnTO = spawnTimeout_;
    const std::string savedShapeName = shapeName_;
    const Flt32 savedMass = debrisMass_;
    const Int32 savedSeq = animationSequence_;
    const Flt32 savedRestart = animRestartTime_;
    const bool savedGhostMe = ghostMe_;

    const auto rollback = [&]() noexcept {
        teamId_ = savedTeam;
        ctrlClientId_ = savedCtrl;
        ownerClientId_ = savedOwner;
        lastOwnerClientId_ = savedLastOwner;
        SimMovement::setPosition(savedTrans, true);
        setLinearVelocity(savedVel);
        setAngularVelocity(savedAv);
        datFileId_ = savedDatId;
        spawnTimeout_ = savedSpawnTO;
        shapeName_ = savedShapeName;
        debrisMass_ = savedMass;
        animationSequence_ = savedSeq;
        animRestartTime_ = savedRestart;
        ghostMe_ = savedGhostMe;
        return false;
    };

    if (!unpackGameBaseUpdate(stream)) {
        return rollback();
    }

    TMat3F trans{};
    Point3F vel{};
    Point3F av{};
    if (!readPod(stream, trans) || !readPod(stream, vel) || !readPod(stream, av)) {
        return rollback();
    }

    const bool isInitial = stream.readFlag();
    if (!stream.isValid()) {
        return rollback();
    }

    if (!isInitial) {
        SimMovement::setPosition(trans, true);
        setLinearVelocity(vel);
        setAngularVelocity(av);
        guard.committed = true;
        return true;
    }

    if (!unpackDatFile(stream)) {
        return rollback();
    }

    Flt32 newSpawnTO = 0.0f;
    std::string newShapeName;
    if (!readPod(stream, newSpawnTO) || !stream.readString(newShapeName)) {
        return rollback();
    }

    Flt32 newMass = -1.0f;
    if (stream.readFlag()) {
        UInt8 massI = 0;
        if (!readPod(stream, massI)) {
            return rollback();
        }
        newMass = static_cast<Flt32>(massI) * 50.0f;
    }

    Int32 newSeq = animationSequence_;
    if (stream.readFlag()) {
        UInt8 seqByte = 0;
        if (!stream.readBits(4, std::span<std::byte>(reinterpret_cast<std::byte*>(&seqByte), 1))) {
            return rollback();
        }
        newSeq = static_cast<Int32>(seqByte & 0xFu);
    }

    Flt32 newRestart = animRestartTime_;
    if (stream.readFlag()) {
        UInt8 atimeI = 0;
        if (!readPod(stream, atimeI)) {
            return rollback();
        }
        newRestart = static_cast<Flt32>(atimeI) / 10.0f;
    }

    if (!stream.isValid()) {
        return rollback();
    }

    SimMovement::setPosition(trans, true);
    setLinearVelocity(vel);
    setAngularVelocity(av);
    spawnTimeout_ = newSpawnTO;
    shapeName_ = std::move(newShapeName);
    debrisMass_ = newMass;
    animationSequence_ = newSeq;
    animRestartTime_ = newRestart;
    ghostMe_ = false;

    if (getManager()) {
        finishAddToManager();
    }
    guard.committed = true;
    return true;
}

UInt32
Debris::packUpdate(sim::network::GhostManager& /*gm*/, UInt32 mask, BitStream& stream) const {
    packDebrisUpdate(mask, stream);
    return 0;
}

bool Debris::unpackUpdate(sim::network::GhostManager& /*gm*/, BitStream& stream) {
    return unpackDebrisUpdate(stream);
}

// -----------------------------------------------------------------------------
// DebrisCloud
// -----------------------------------------------------------------------------

DebrisCloud::DebrisCloud() {
    setNetFlags(0);
    setContainerMask(0);
}

DebrisCloud::DebrisCloud(Flt32 kg,
                         Int32 typeMask,
                         const Point3F& p,
                         Flt32 r,
                         const Point3F& v,
                         Flt32 outJ,
                         const Point3F& ax,
                         Flt32 angleR,
                         const Point3F& inJ,
                         Flt32 spawnTO)
    : mass_(kg), debrisMask_(typeMask), pos_(p), radius_(r), vel_(v), outwardImpulse_(outJ),
      axis_(ax), angleRange_(angleR), incomingImpulse_(inJ), spawnTimeout_(spawnTO),
      dataReceived_(true) {
    setNetFlags(0);
    setContainerMask(0);
}

DebrisCloud::DebrisCloud(const DebrisCloud& other)
    : boundDbm_(other.boundDbm_), mass_(other.mass_), debrisMask_(other.debrisMask_),
      pos_(other.pos_), radius_(other.radius_), vel_(other.vel_),
      outwardImpulse_(other.outwardImpulse_), axis_(other.axis_), angleRange_(other.angleRange_),
      incomingImpulse_(other.incomingImpulse_), spawnTimeout_(other.spawnTimeout_),
      soundId_(other.soundId_), spawnedFrom_(nullptr), dataReceived_(true) {
    setNetFlags(0);
    setContainerMask(0);
}

DebrisCloud::~DebrisCloud() = default;

DataBlockManager* DebrisCloud::resolveDbm() const noexcept {
    if (boundDbm_) {
        return boundDbm_;
    }
    if (auto* mgr = getManager()) {
        return dynamic_cast<DataBlockManager*>(mgr->findObject(DataBlockManagerId));
    }
    return nullptr;
}

void DebrisCloud::addManagedDebris(std::unique_ptr<Debris> debris) {
    if (!debris) {
        return;
    }
    DebrisCloud* targetCloud = spawnedFrom_ ? spawnedFrom_ : this;
    debris->setInCloud(true, targetCloud);
    Debris* rawPtr = debris.get();
    targetCloud->setContainerMask(0xFFFFFFFFu);
    targetCloud->addObject(*rawPtr);
    targetCloud->setContainerMask(0);
    if (getManager()) {
        getManager()->addObject(std::move(debris));
    } else {
        targetCloud->ownedDebris_.push_back(std::move(debris));
    }
    targetCloud->debrisList_.push_back(rawPtr);
}

void DebrisCloud::getAxes(const Point3F& axis,
                          std::span<Point3F, 3> axes,
                          Flt32& angleRange) noexcept {
    Point3F& x = axes[0];
    Point3F& y = axes[1];
    Point3F& z = axes[2];

    const Flt32 len2 = m_dot(axis, axis);
    if (!isEqualFloat(len2, 0.0f)) {
        z = axis;
        if (!isEqualFloat(len2, 1.0f)) {
            z *= 1.0f / std::sqrt(len2);
        }
        if (isEqualFloat(std::fabs(z.z), 1.0f)) {
            x.set(1.0f, 0.0f, 0.0f);
            y.set(0.0f, 1.0f, 0.0f);
        } else {
            x.x = z.y;
            x.y = -z.x;
            x.z = 0.0f;
            const Flt32 tmp = 1.0f / (1.0f - z.z);
            x.x *= tmp;
            x.y *= tmp;
            m_cross(x, z, y);
        }
        x.normalize();
        y.normalize();
    } else {
        x.set(1.0f, 0.0f, 0.0f);
        y.set(0.0f, 1.0f, 0.0f);
        z.set(0.0f, 0.0f, 1.0f);
        angleRange = kPi;
    }
}

void DebrisCloud::toss(Debris& debris,
                       const Point3F& pos,
                       Flt32 radius,
                       const Point3F& vel,
                       Flt32 outwardImpulse,
                       std::span<const Point3F, 3> axes,
                       Flt32 angleRange,
                       const Point3F& incomingImpulse,
                       sim::SimManager* /*mgr*/) {
    ensureRandomRotations();
    const Point3F& x = axes[0];
    const Point3F& y = axes[1];
    const Point3F& z = axes[2];

    const Flt32 rot1 = g_dbRand.getFloat(kTwoPi);
    const Flt32 rot2 = g_dbRand.getFloat(-angleRange, angleRange);

    const Flt32 s2 = std::sin(rot2);
    const Flt32 c1s2 = std::cos(rot1) * s2;
    const Flt32 s1s2 = std::sin(rot1) * s2;
    const Flt32 c2 = std::cos(rot2);

    Point3F vec{x.x * c1s2 + y.x * s1s2 + z.x * c2,
                x.y * c1s2 + y.y * s1s2 + z.y * c2,
                x.z * c1s2 + y.z * s1s2 + z.z * c2};

    Point3F loc = vec * radius + pos;
    TMat3F tmat{};
    const Int32 rotIdx = g_dbRand.getInt(0, static_cast<Int32>(kNumRandomRotations) - 1);
    static_cast<RMat3F&>(tmat) = g_randomRotations[static_cast<std::size_t>(rotIdx)];
    tmat.p = loc;

    debris.setPosition(tmat, true);
    debris.setLinearVelocity(vel);

    Flt32 amag = outwardImpulse * 0.015f;
    if (amag > 6.0f) {
        amag = 6.0f;
    }
    const Point3F avel{g_dbRand.getFloat(amag) - amag * 0.5f,
                       g_dbRand.getFloat(amag) - amag * 0.5f,
                       g_dbRand.getFloat(amag) - amag * 0.5f};
    debris.setAngularVelocity(avel);

    vec *= outwardImpulse;
    debris.applyImpulse(vec);
    debris.applyImpulse(incomingImpulse);
}

bool DebrisCloud::onAdd() {
    if (!Parent::onAdd()) {
        return false;
    }
    setNetFlags(0);
    if (dataReceived_) {
        finishAddToManager();
    }
    return true;
}

void DebrisCloud::finishAddToManager() {
    ensureRandomRotations();
    if (!spawnedFrom_ && getManager()) {
        if (auto* root =
                dynamic_cast<sim::SimContainer*>(getManager()->findObject(sim::RootContainerId))) {
            setContainerMask(0);
            root->addObject(*this);
            setBoundingBox(Box3F{pos_, pos_});
        }
        addToSet(sim::TimerSetId);
    }

    auto* dbm = resolveDbm();
    if (!dbm) {
        return;
    }

    mass_ *= detailScale;
    DebrisTable::ShortList sl;
    Flt32 p = DebrisTable::getShortList(*dbm, mass_, debrisMask_, sl);
    getAxes(axis_, axes_, angleRange_);

    Int32 numLeft = static_cast<Int32>(sl.size());
    while (numLeft > 0) {
        Flt32 choose = g_dbRand.getFloat(0.0f, p);
        Int32 i = numLeft - 1;
        while (i > 0 && choose > sl[static_cast<std::size_t>(i)].p) {
            choose -= sl[static_cast<std::size_t>(i--)].p;
        }
        auto debris =
            std::make_unique<Debris>(sl[static_cast<std::size_t>(i)].index, spawnTimeout_);
        debris->bindDataBlockManager(dbm);
        debris->setInCloud(true, spawnedFrom_ ? spawnedFrom_ : this);

        TMat3F initTrans = debris->getTransform();
        initTrans.p = pos_;
        debris->setPosition(initTrans, true);
        debris->finishAddToManager();

        toss(*debris,
             pos_,
             radius_,
             vel_,
             outwardImpulse_,
             axes_,
             angleRange_,
             incomingImpulse_,
             getManager());

        Debris* rawPtr = debris.get();
        if (getManager()) {
            getManager()->addObject(std::move(debris));
        } else {
            ownedDebris_.push_back(std::move(debris));
        }
        debrisList_.push_back(rawPtr);

        mass_ -= sl[static_cast<std::size_t>(i)].mass;
        while (numLeft > 0 && sl[static_cast<std::size_t>(numLeft - 1)].mass > mass_) {
            p -= sl[static_cast<std::size_t>(--numLeft)].p;
        }
    }
    setContainerMask(0);
}

bool DebrisCloud::onSimTimerEvent(const sim::SimTimerEvent& event) {
    Box3F cloudBox{pos_, pos_};
    bool any = false;
    for (auto it = debrisList_.begin(); it != debrisList_.end();) {
        Debris* d = *it;
        if (!d || d->isDeleted() || d->isExpired()) {
            it = debrisList_.erase(it);
            continue;
        }
        d->onSimTimerEvent(event);
        if (d->isExpired()) {
            it = debrisList_.erase(it);
            continue;
        }
        if (!any) {
            cloudBox = d->getBoundingBox();
            any = true;
        } else {
            cloudBox.fMin.setMin(d->getBoundingBox().fMin);
            cloudBox.fMax.setMax(d->getBoundingBox().fMax);
        }
        ++it;
    }
    setBoundingBox(cloudBox);
    if (debrisList_.empty() && getManager()) {
        deleteObject();
    }
    return true;
}

bool DebrisCloud::onSimMessageEvent(const sim::SimMessageEvent&) {
    if (getManager()) {
        deleteObject();
    }
    return true;
}

bool DebrisCloud::processEvent(const sim::SimEvent& event) {
    if (const auto* te = dynamic_cast<const sim::SimTimerEvent*>(&event)) {
        return onSimTimerEvent(*te);
    }
    if (const auto* me = dynamic_cast<const sim::SimMessageEvent*>(&event)) {
        return onSimMessageEvent(*me);
    }
    return Parent::processEvent(event);
}

bool DebrisCloud::packCloudUpdate(BitStream& stream) const {
    writeFloatSmall(stream, mass_, 50.0f);
    writePod(stream, debrisMask_);
    writePod(stream, pos_);
    writeFloatSmall(stream, radius_, 0.5f);
    writeVectorSmall(stream, vel_, 0.1f);
    writeFloatSmall(stream, outwardImpulse_, 50.0f);

    const Flt32 aLenSq = m_dot(axis_, axis_);
    UInt8 angleI = 255;
    if (isEqualFloat(aLenSq, 0.0f) || angleRange_ > kPi - 0.1f) {
        angleI = 255;
        writePod(stream, angleI);
    } else {
        angleI = static_cast<UInt8>(angleRange_ / 0.0125f);
        writePod(stream, angleI);
        const Flt32 aLen = std::sqrt(aLenSq);
        Point3F unitAxis = axis_ * (1.0f / aLen);
        writeUnitVectorSmall(stream, unitAxis);
    }

    writeVectorSmall(stream, incomingImpulse_, 100.0f);
    writeFloatSmall(stream, spawnTimeout_, 1.0f);
    if (stream.writeFlag(soundId_ > 0)) {
        stream.writeInt(soundId_, 20);
    }
    return stream.isValid();
}

bool DebrisCloud::unpackCloudUpdate(BitStream& stream) {
    BitStreamRollbackGuard guard(stream);

    const Flt32 savedMass = mass_;
    const Int32 savedMask = debrisMask_;
    const Point3F savedPos = pos_;
    const Flt32 savedRadius = radius_;
    const Point3F savedVel = vel_;
    const Flt32 savedOutJ = outwardImpulse_;
    const Point3F savedAxis = axis_;
    const Flt32 savedAngleRange = angleRange_;
    const Point3F savedInJ = incomingImpulse_;
    const Flt32 savedSpawnTO = spawnTimeout_;
    const Int32 savedSoundId = soundId_;
    const bool savedDataReceived = dataReceived_;

    const auto rollback = [&]() noexcept {
        mass_ = savedMass;
        debrisMask_ = savedMask;
        pos_ = savedPos;
        radius_ = savedRadius;
        vel_ = savedVel;
        outwardImpulse_ = savedOutJ;
        axis_ = savedAxis;
        angleRange_ = savedAngleRange;
        incomingImpulse_ = savedInJ;
        spawnTimeout_ = savedSpawnTO;
        soundId_ = savedSoundId;
        dataReceived_ = savedDataReceived;
        return false;
    };

    if (!readFloatSmall(stream, mass_, 50.0f) || !readPod(stream, debrisMask_) ||
        !readPod(stream, pos_) || !readFloatSmall(stream, radius_, 0.5f) ||
        !readVectorSmall(stream, vel_, 10.0f) || !readFloatSmall(stream, outwardImpulse_, 50.0f)) {
        return rollback();
    }

    UInt8 angleI = 0;
    if (!readPod(stream, angleI)) {
        return rollback();
    }
    if (angleI == 255) {
        axis_.set(0.0f, 0.0f, 0.0f);
        angleRange_ = kPi;
    } else {
        angleRange_ = 0.0125f * static_cast<Flt32>(angleI);
        if (!readUnitVectorSmall(stream, axis_)) {
            return rollback();
        }
    }

    if (!readVectorSmall(stream, incomingImpulse_, 100.0f) ||
        !readFloatSmall(stream, spawnTimeout_, 1.0f)) {
        return rollback();
    }

    if (stream.readFlag()) {
        soundId_ = stream.readInt(20);
    } else {
        soundId_ = 0;
    }

    if (!stream.isValid()) {
        return rollback();
    }

    dataReceived_ = true;
    if (getManager()) {
        finishAddToManager();
    }
    guard.committed = true;
    return true;
}

UInt32 DebrisCloud::packUpdate(sim::network::GhostManager& /*gm*/,
                               UInt32 /*mask*/,
                               BitStream& stream) const {
    packCloudUpdate(stream);
    return 0;
}

bool DebrisCloud::unpackUpdate(sim::network::GhostManager& /*gm*/, BitStream& stream) {
    return unpackCloudUpdate(stream);
}

bool registerDebrisClasses(Persistent::ClassRegistry& registry) {
    const bool ok1 = registry.registerClass<Debris>("Debris", DebrisPersTag);
    const bool ok2 = registry.registerClass<DebrisCloud>("DebrisCloud", DebrisCloudPersTag);
    return ok1 && ok2;
}

bool registerDebrisNetwork(sim::network::Registry& registry) {
    const bool ok1 = registry.registerGhost<Debris>(static_cast<UInt16>(DebrisPersTag));
    const bool ok2 = registry.registerGhost<DebrisCloud>(static_cast<UInt16>(DebrisCloudPersTag));
    return ok1 && ok2;
}

} // namespace opent1::game
