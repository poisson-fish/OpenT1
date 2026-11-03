#include <opent1/game/player.hpp>
#include <ranges>
#include <opent1/game/proj_grenade.hpp>
#include <opent1/game/smoke_puff.hpp>
#include <opent1/game/sound.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>

namespace opent1::game {

namespace {

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

UInt32 currentTimeMs(const sim::SimManager* mgr, UInt32 fallback = 0) noexcept {
    if (!mgr) {
        return fallback;
    }
    return static_cast<UInt32>(mgr->getCurrentTime() * 1000.0f);
}

} // namespace

Grenade::Grenade(Int32 in_datFileId)
    : Projectile(in_datFileId),
      m_sphereCollImage_(std::make_shared<sim::SimCollisionSphereImage>()) {
    setMovementFlags(movementFlags() | UseElasticity | UseDrag | UseCurrent);
    m_faceForward_ = false;
    setForceMask(sim::MovementGravityForce);
    setNetFlags(Ghostable);
    m_sphereCollImage_->transform.identity();
}

Grenade::Grenade() : Grenade(-1) {}

bool Grenade::initResources(GameBaseData* io_pDat) {
    if (!Parent::initResources(io_pDat)) {
        return false;
    }
    m_pGrenadeData_ = dynamic_cast<GrenadeData*>(io_pDat);
    if (!m_pGrenadeData_) {
        return false;
    }
    setElasticity(m_pGrenadeData_->elasticity);
    setFriction(1.0f);
    setDrag(1.0f);
    return true;
}

void Grenade::loadResources(bool in_isGhost) {
    Parent::loadResources(in_isGhost);
    if (in_isGhost && m_pGrenadeData_) {
        setElasticity(m_pGrenadeData_->elasticity);
        setFriction(1.0f);
    }
}

void Grenade::killProjectile() {
    Parent::killProjectile();
    applyRadiusDamage(getLinearPosition());
    spawnImpactEffect(getLinearPosition());
    m_waitingForDelete_ = true;
    m_deleteTime_ = currentTimeMs(getManager(), m_lastUpdated_) + 500u;
    setMaskBits(FPExplosionMask);
}

void Grenade::destroyProjectile() {
    killProjectile();
}

bool Grenade::onAdd() {
    if (!Parent::onAdd() || !m_pGrenadeData_) {
        return false;
    }

    m_sphereCollImage_->center = Point3F{0.0f, 0.0f, 0.0f};
    m_sphereCollImage_->radius = m_projectileData_ ? m_projectileData_->collisionRadius : 0.1f;

    const UInt32 nowMs = currentTimeMs(getManager(), m_lastUpdated_);
    m_lastPuff_ = nowMs;
    m_puffInterval_ =
        100u + static_cast<UInt32>(std::max(0.0f, m_pGrenadeData_->projSpecialTime) * 1000.0f);

    if (isGhost()) {
        setMovementFlags(movementFlags() | UseAngularVelocity);
        setAngularVelocity(Point3F{6.0f, 2.0f, 4.0f});
        m_lastGhosted_ = nowMs;
        m_startInterp_ = nowMs;
        m_endInterp_ = nowMs + 64u;
        m_interpFrom_ = getLinearPosition();
        m_faceForward_ = false;

    } else {
        m_lastGhosted_ = nowMs - 2500u;
    }

    return true;
}

void Grenade::buildContainerBox(const TMat3F& in_rMat, Box3F* out_pBox) {
    const Flt32 rad = m_projectileData_ ? m_projectileData_->collisionRadius : 0.1f;
    out_pBox->fMin = in_rMat.p - Point3F{rad, rad, rad};
    out_pBox->fMax = in_rMat.p + Point3F{rad, rad, rad};
}

Box3F Grenade::buildContainerBox(const TMat3F& in_rMat) const {
    const Flt32 rad = m_projectileData_ ? m_projectileData_->collisionRadius : 0.1f;
    return Box3F{in_rMat.p - Point3F{rad, rad, rad}, in_rMat.p + Point3F{rad, rad, rad}};
}

std::shared_ptr<sim::SimCollisionImage> Grenade::getSelfImage() {
    m_sphereCollImage_->transform = getTransform();
    m_sphereCollImage_->center = Point3F{0.0f, 0.0f, 0.0f};
    m_sphereCollImage_->radius = m_projectileData_ ? m_projectileData_->collisionRadius : 0.1f;
    return m_sphereCollImage_;
}

void Grenade::writeInitialPacket(sim::network::GhostManager* gm, BitStream& pStream) const {
    Parent::writeInitialPacket(gm, pStream);
    const Point3F pos = getLinearPosition();
    const Point3F vel = getLinearVelocity();
    writePod(pStream, pos);
    writePod(pStream, vel);
}

bool Grenade::readInitialPacket(sim::network::GhostManager* gm, BitStream& pStream) {
    if (!Parent::readInitialPacket(gm, pStream)) {
        return false;
    }

    Point3F position{};
    Point3F velocity{};
    if (!readPod(pStream, position) || !readPod(pStream, velocity)) {
        return false;
    }

    setLinearVelocity(velocity);
    TMat3F transform = getTransform();
    transform.p = position;
    transform.flags |= RMat3F::Matrix_HasTranslation;
    setPosition(transform, true);
    return true;
}

UInt32 Grenade::packUpdate(sim::network::GhostManager& gm, UInt32 mask, BitStream& stream) const {
    UInt32 parentPunt = Parent::packUpdate(gm, mask, stream);

    if ((mask & Projectile::InitialUpdate) != 0) {
        stream.writeFlag(true);
        writeInitialPacket(&gm, stream);
        if (m_hasExploded_) {
            parentPunt |= FPExplosionMask;
        }
        return parentPunt;
    }
    stream.writeFlag(false);

    if (stream.writeFlag((mask & Projectile::FPPositionMask) != 0)) {
        const Point3F pos = getLinearPosition();
        writePod(stream, pos);
    }
    if (stream.writeFlag((mask & Projectile::FPVelocityMask) != 0)) {
        const Point3F vel = getLinearVelocity();
        writePod(stream, vel);
    } else {
        Point3F vel = getLinearVelocity();
        if (!vel.isZero()) {
            vel.normalize();
        }
        stream.writeNormalVector(NormalVector3F{vel.x, vel.y, vel.z}, 9);
    }
    if (stream.writeFlag((mask & FGCollisionSoundMask) != 0)) {
        stream.writeInt(m_lastSurfaceType_, 4);
    }

    return parentPunt;
}

bool Grenade::unpackUpdate(sim::network::GhostManager& gm, BitStream& stream) {
    BitStreamRollbackGuard guard(stream);

    const auto savedDatId = datFileId_;
    const auto savedOwnerId = ownerId_;
    const auto savedShooter = m_pShooter_;
    const auto savedShooterGhost = m_explicitShooterGhostIndex_;
    const auto savedTrans = getTransform();
    const auto savedLinVel = getLinearVelocity();
    const auto savedExpPos = m_explosionPosition_;
    const auto savedExpAxis = m_explosionAxis_;
    const auto savedExpId = m_explosionId_;
    const auto savedHasExp = m_hasExploded_;
    const auto savedOverride = m_overrideRender_;
    const auto savedStartInterp = m_startInterp_;
    const auto savedEndInterp = m_endInterp_;
    const auto savedInterpFrom = m_interpFrom_;
    const auto savedInterpTo = m_interpTo_;
    const auto savedSurfaceType = m_lastSurfaceType_;
    const bool savedPendingExplosion = m_pendingExplosion_;

    const auto rollback = [&]() noexcept {
        datFileId_ = savedDatId;
        ownerId_ = savedOwnerId;
        m_pShooter_ = savedShooter;
        m_explicitShooterGhostIndex_ = savedShooterGhost;
        SimMovement::setPosition(savedTrans, true);
        setLinearVelocity(savedLinVel);
        m_explosionPosition_ = savedExpPos;
        m_explosionAxis_ = savedExpAxis;
        m_explosionId_ = savedExpId;
        m_hasExploded_ = savedHasExp;
        m_overrideRender_ = savedOverride;
        m_startInterp_ = savedStartInterp;
        m_endInterp_ = savedEndInterp;
        m_interpFrom_ = savedInterpFrom;
        m_interpTo_ = savedInterpTo;
        m_lastSurfaceType_ = savedSurfaceType;
        m_pendingExplosion_ = savedPendingExplosion;
        return false;
    };

    if (!Parent::unpackUpdate(gm, stream)) {
        return rollback();
    }

    if (stream.readFlag()) {
        if (!readInitialPacket(&gm, stream)) {
            return rollback();
        }
        guard.committed = true;
        return true;
    }

    m_interpFrom_ = getLinearPosition();
    bool gotPosOrVel = false;

    if (stream.readFlag()) {
        Point3F position{};
        if (!readPod(stream, position)) {
            return rollback();
        }
        TMat3F transform = getTransform();
        transform.p = position;
        transform.flags |= RMat3F::Matrix_HasTranslation;
        setPosition(transform, true);
        gotPosOrVel = true;
    }

    if (stream.readFlag()) {
        Point3F velocity{};
        if (!readPod(stream, velocity)) {
            return rollback();
        }
        setLinearVelocity(velocity);
        gotPosOrVel = true;
    } else {
        NormalVector3F velDir{};
        stream.readNormalVector(velDir, 9);
        if (!stream.isValid()) {
            return rollback();
        }
        Point3F velocity{velDir.x, velDir.y, velDir.z};
        const Flt32 curSpeed = getLinearVelocity().len();
        velocity *= curSpeed;
        setLinearVelocity(velocity);
    }

    const bool bounceSound = stream.readFlag();
    if (bounceSound) {
        m_lastSurfaceType_ = stream.readInt(4);
        if (!stream.isValid()) {
            return rollback();
        }
    }

    if (gotPosOrVel) {
        const UInt32 nowMs = currentTimeMs(getManager(), m_lastUpdated_);
        m_startInterp_ = nowMs;
        m_endInterp_ = nowMs + 64u;
        m_lastUpdated_ = nowMs;

        Point3F newVel{};
        predictForward(nowMs, m_endInterp_, m_interpTo_, newVel);
        setLinearVelocity(newVel);
    }

    if (!stream.isValid()) {
        return rollback();
    }
    if (bounceSound) {
        playBounceSound(m_lastSurfaceType_);
    }
    if (m_pendingExplosion_) {
        createExplosionEffect();
    }
    guard.committed = true;
    return true;
}

void Grenade::predictForward(UInt32 in_currTime,
                             UInt32 in_predictTime,
                             Point3F& out_rPosition,
                             Point3F& out_rVelocity) {
    const TMat3F oldPosition = getTransform();
    const Point3F oldVelocity = getLinearVelocity();
    const UInt32 oldUpdated = m_lastUpdated_;
    const Int32 oldGrace = m_ownerGraceMS_;
    const Flt32 oldTimeSlice = getTimeSlice();

    m_lastUpdated_ = in_currTime;
    advanceGrenade(in_predictTime, false);

    out_rPosition = getLinearPosition();
    out_rVelocity = getLinearVelocity();

    m_lastUpdated_ = oldUpdated;
    m_ownerGraceMS_ = oldGrace;
    setTimeSlice(oldTimeSlice);
    setLinearVelocity(oldVelocity);
    setPosition(oldPosition, true);
}

void Grenade::playBounceSound(Int32 surfaceType) {
    // Native TS surface types: concrete 1, metal 3, marble 7, stone 12.
    const UInt32 tag = surfaceType == 3                                              ? 450102u
                       : (surfaceType == 1 || surfaceType == 7 || surfaceType == 12) ? 450101u
                                                                                     : 450100u;
    (void)TSFX::PlayTaggedAt(tag, getTransform(), getManager());
}

bool Grenade::advanceGrenade(UInt32 currTime, bool collisions) {
    auto* root =
        getManager()
            ? dynamic_cast<sim::SimContainer*>(getManager()->findObject(sim::RootContainerId))
            : nullptr;
    bool hitSomething = false;
    const Flt32 savedSlice = getTimeSlice();
    while (m_lastUpdated_ < currTime) {
        const UInt32 slice = std::min<UInt32>(33u, currTime - m_lastUpdated_);
        setTimeSlice(static_cast<Flt32>(slice) * 0.001f);
        TMat3F target = updateMovement();
        Flt32 remaining = static_cast<Flt32>(slice) * 0.001f;
        for ([[maybe_unused]] auto bounce : std::views::iota(Int32{0}, Int32{8})) {
            sim::SimCollisionInfo collision{};
            sim::SimContainerQuery query{};
            query.type = ~UInt32{0};
            query.mask = csm_collisionMask;
            query.detail = -1;
            query.id = (!m_projectileData_->collideWithOwner || m_ownerGraceMS_ > 0)
                           ? getOwnerId()
                           : static_cast<sim::SimObjectId>(-1);
            query.box = Box3F{getLinearPosition(), target.p};
            if (!collisions || !root ||
                !root->findLOS(query, collision, sim::SimCollisionImageQuery::High) ||
                collision.surfaces.surfaces.empty()) {
                setPosition(target, true);
                break;
            }
            collision.transform(collision.surfaces.tWorld);
            const auto& surface = collision.surfaces.surfaces.front();
            Point3F velocity = getLinearVelocity();
            const Flt32 dot = m_dot(surface.normal, velocity);
            if (dot >= 0.0f) {
                setPosition(target, true);
                break;
            }
            const Flt32 speed = velocity.len();
            const Flt32 elapsed =
                speed > 0.0f ? (surface.position - getLinearPosition()).len() / speed : remaining;
            remaining = std::max(0.0f, remaining - elapsed);
            // Legacy grenade reflection scales the entire reflected velocity,
            // including its tangential component, by the surface elasticity.
            velocity += surface.normal * (2.0f * (-dot + 0.01f));
            Flt32 elasticity = coefficients().elasticity;
            Int32 surfaceType = 0;
            if (collision.image) {
                if (const auto material = collision.image->getMaterial(surface.material)) {
                    elasticity *= material->elasticity;
                    surfaceType = std::max(0, material->surfaceType);
                }
            }
            setLinearVelocity(velocity * elasticity);
            if (!isGhost() &&
                getLinearVelocity().len() > m_projectileData_->muzzleVelocity * .15f) {
                m_lastSurfaceType_ = surfaceType;
                setMaskBits(FGCollisionSoundMask);
            }
            target.p = surface.position;
            target.flags |= RMat3F::Matrix_HasTranslation;
            setPosition(target, true);
            hitSomething = true;
            m_hasBounced_ = true;
            if (currTime > m_liveTime_) {
                break;
            }
            if (remaining <= 0.0f) {
                break;
            }
            target.p += getLinearVelocity() * remaining;
        }
        decayGraceTime(slice);
        m_lastUpdated_ += slice;
        if (hitSomething && currTime > m_liveTime_) {
            break;
        }
    }
    setTimeSlice(savedSlice);
    return hitSomething;
}

void Grenade::clientProcess(UInt32 in_currTime) {
    if (in_currTime <= m_lastUpdated_ || m_hasExploded_) {
        return;
    }
    if (m_translucentShape_ && m_translucentThread_) {
        m_translucentShape_->advanceThread(
            *m_translucentThread_, static_cast<Flt64>(in_currTime - m_lastUpdated_) * 0.001);
    }
    if (advanceGrenade(in_currTime, true) && in_currTime > m_liveTime_) {
        createExplosionEffect();
        return;
    }
    if (getManager() && m_pGrenadeData_ && !m_pGrenadeData_->pSmokeName.empty() &&
        in_currTime > m_lastPuff_ + m_puffInterval_) {
        if (auto* smoke = SmokeManager::get(getManager())) {
            smoke->addPuff(
                m_pGrenadeData_->pSmokeName, 1.0f, getLinearPosition(), Point3F{0.0f, 0.0f, 1.0f});
        }
        m_lastPuff_ = in_currTime;
        const Flt32 muzzle = m_projectileData_->muzzleVelocity;
        const Flt32 factor =
            muzzle > 0.0f ? std::max(0.0f, (muzzle - getLinearVelocity().len()) / muzzle) : 0.0f;
        m_puffInterval_ = std::max<UInt32>(1u,
                                           static_cast<UInt32>(m_projectileData_->projSpecialTime *
                                                               (1.0f + 2.0f * factor) * 1000.0f));
    }
    updateProjectileSound(getTransform(), getLinearVelocity());
}

void Grenade::serverProcess(UInt32 in_currTime) {
    if (in_currTime < m_lastUpdated_) {
        return;
    }
    if (m_waitingForDelete_) {
        if (in_currTime >= m_deleteTime_) {
            deleteObject();
        }
        return;
    }
    if (in_currTime > m_totalTime_) {
        destroyProjectile();
        return;
    }
    const bool collided = advanceGrenade(in_currTime, true);
    if (collided || in_currTime - m_lastGhosted_ > 500u) {
        m_lastGhosted_ = in_currTime;
        setMaskBits(FPPositionMask | (collided ? FPVelocityMask : 0u));
    }
    // liveTime is an arming delay, not an airburst fuse.
    if (collided && in_currTime > m_liveTime_) {
        destroyProjectile();
        m_deleteTime_ = in_currTime + 500u;
    }
}

bool Grenade::readExplosion(BitStream& io_pStream) {
    if (!io_pStream.isValid()) {
        return false;
    }
    // The base packet precedes this class's authoritative position update.
    // Defer the effect until that position has been decoded successfully.
    m_pendingExplosion_ = true;
    return true;
}

void Grenade::createExplosionEffect() {
    m_pendingExplosion_ = false;
    m_explosionPosition_ = getLinearPosition();
    m_explosionAxis_.set(0.0f, 0.0f, 1.0f);
    m_explosionId_ = getExplosionTag();

    if (!m_hasExploded_) {
        m_hasExploded_ = true;
        if (getManager()) {
            if (auto* sm = SmokeManager::get(getManager())) {
                sm->removeAllInRadius(getLinearPosition(),
                                      m_projectileData_ ? m_projectileData_->explosionRadius
                                                        : 15.0f);
            }
            auto pExplosion = std::make_unique<Explosion>(m_explosionId_);
            pExplosion->bindDataBlockManager(resolveDataBlockManager());
            pExplosion->setPosition(getLinearPosition());
            getManager()->addObject(std::move(pExplosion));
        }
        m_overrideRender_ = true;
        stopProjectileSound();
    }
}

void Grenade::writeExplosion(BitStream& /*io_pStream*/) const {}

} // namespace opent1::game
