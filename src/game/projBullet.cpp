#include <opent1/game/player.hpp>
#include <opent1/game/proj_bullet.hpp>
#include <opent1/world/world_scene.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <numbers>

namespace opent1::game {

namespace {
constexpr Flt32 kTwoPi = static_cast<Flt32>(2.0 * std::numbers::pi);
Random sg_bulletRandom{};

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

Bullet::Bullet(Int32 in_datFileId)
    : Projectile(in_datFileId),
      m_sphereCollImage_(std::make_shared<sim::SimCollisionSphereImage>()) {
    setNetFlags(Ghostable);
    m_sphereCollImage_->transform.identity();
}

Bullet::Bullet() : Bullet(-1) {}

bool Bullet::initResources(GameBaseData* io_pDat) {
    if (!Parent::initResources(io_pDat)) {
        return false;
    }
    m_pBulletData_ = dynamic_cast<BulletData*>(io_pDat);
    return m_pBulletData_ != nullptr;
}

void Bullet::setSpawnState(const Point3F& pos, const Point3F& vel, UInt32 spawnTimeMs) noexcept {
    m_spawnPosition_ = pos;
    m_spawnVelocity_ = vel;
    m_spawnVelocityLen_ = vel.len();
    m_spawnTime_ = spawnTimeMs;
    setLinearVelocity(vel);
    TMat3F trans = getTransform();
    trans.p = pos;
    trans.flags |= RMat3F::Matrix_HasTranslation;
    setPosition(trans, true);
}

void Bullet::quantizeServerVelocity() noexcept {
    Point3F velocity = getLinearVelocity();
    Flt32 velLen = velocity.len();
    if (velLen <= 0.0f) {
        m_spawnVelocity_.set(0.0f, 0.0f, 0.0f);
        m_spawnVelocityLen_ = 0.0f;
        m_spawnPosition_ = getLinearPosition();
        return;
    }
    velocity /= velLen;

    constexpr UInt32 kMaxQuant = (1u << csm_normalBits) - 1u;
    const auto intx =
        static_cast<UInt32>(((velocity.x + 1.0f) * 0.5f) * static_cast<Flt32>(kMaxQuant));
    const auto inty =
        static_cast<UInt32>(((velocity.y + 1.0f) * 0.5f) * static_cast<Flt32>(kMaxQuant));
    const bool zvel = velocity.z < 0.0f;

    const Flt32 floatx = (static_cast<Flt32>(intx * 2u) / static_cast<Flt32>(kMaxQuant)) - 1.0f;
    const Flt32 floaty = (static_cast<Flt32>(inty * 2u) / static_cast<Flt32>(kMaxQuant)) - 1.0f;
    Flt32 floatz = 0.0f;
    const Flt32 zSquared = 1.0f - (floatx * floatx) - (floaty * floaty);
    if (zSquared >= 0.0f) {
        floatz = std::sqrt(zSquared);
        if (zvel) {
            floatz = -floatz;
        }
    }

    const auto writeVel = static_cast<UInt32>(velLen * 16.0f);
    velLen = static_cast<Flt32>(writeVel) / 16.0f;

    velocity.x = floatx;
    velocity.y = floaty;
    velocity.z = floatz;
    velocity *= velLen;
    setLinearVelocity(velocity);

    m_spawnVelocity_ = getLinearVelocity();
    m_spawnVelocityLen_ = m_spawnVelocity_.len();
    m_spawnPosition_ = getLinearPosition();
}

bool Bullet::applyBaseDamage(sim::SimCollisionInfo& io_rCollision, sim::SimObject* io_pHit) {
    if (isGhost() || !io_pHit || !m_projectileData_) {
        return false;
    }
    if ((io_pHit->getType() & csm_damageMask) != 0) {
        if (auto* pShape = dynamic_cast<ShapeBase*>(io_pHit)) {
            Point3F pos{};
            if (!io_rCollision.surfaces.surfaces.empty()) {
                m_mul(io_rCollision.surfaces.surfaces[0].position,
                      io_rCollision.surfaces.tWorld,
                      pos);
            } else {
                pos = getLinearPosition();
            }

            Flt32 value = m_projectileData_->damageValue;
            const UInt32 nowMs = currentTimeMs(getManager(), m_lastUpdated_);
            if (nowMs > m_liveTime_ && m_totalTime_ > m_liveTime_) {
                value *= 1.0f - (static_cast<Flt32>(nowMs - m_liveTime_) /
                                 static_cast<Flt32>(m_totalTime_ - m_liveTime_));
            }

            pShape->applyDamage(m_projectileData_->damageType,
                                value,
                                pos,
                                getLinearVelocity(),
                                Point3F{0.0f, 0.0f, 0.0f},
                                static_cast<sim::SimObjectId>(getDamageId()));
            return true;
        }
    }
    return false;
}

bool Bullet::onAdd() {
    if (!Parent::onAdd() || !m_pBulletData_) {
        return false;
    }

    const UInt32 nowMs = currentTimeMs(getManager(), m_lastUpdated_);
    if (isGhost()) {
        if (!m_projectileData_->projSpecialBool) {
            m_overrideRender_ = true;
        }
        m_renderFacingCamera_ = true;

        m_spawnTime_ = nowMs;
        m_lastUpdated_ = nowMs;
        m_startInterp_ = nowMs;
        m_endInterp_ = nowMs + 64u;
        m_interpFrom_ = getLinearPosition();
        predictForward(nowMs, nowMs + 96u, m_interpTo_);

        if (sg_bulletRandom.getFloat() < m_pBulletData_->projSpecialTime &&
            !m_pBulletData_->projSpecialBool) {
            m_isTracer_ = true;
            m_overrideRender_ = false;
        }
    } else {
        quantizeServerVelocity();
        m_spawnTime_ = nowMs;
    }

    setSearchBoundaries();
    return true;
}

Box3F Bullet::buildContainerBox(const TMat3F& in_rMat) const {
    return Box3F{in_rMat.p, in_rMat.p};
}

void Bullet::writeInitialPacket(sim::network::GhostManager* gm, BitStream& pStream) const {
    Parent::writeInitialPacket(gm, pStream);

    const Point3F position = getLinearPosition();
    const Flt32 time = (m_spawnVelocityLen_ > 0.0f)
                           ? ((position - m_spawnPosition_).len() / m_spawnVelocityLen_)
                           : 0.0f;
    const auto timeElapsed = static_cast<UInt32>(time * 1000.0f);

    writePod(pStream, m_spawnPosition_);
    pStream.writeInt(static_cast<Int32>(timeElapsed), 15);

    Point3F normalVel = m_spawnVelocity_;
    if (m_spawnVelocityLen_ > 0.0f) {
        normalVel /= m_spawnVelocityLen_;
    } else {
        normalVel.set(0.0f, 1.0f, 0.0f);
    }
    pStream.writeNormalVector(NormalVector3F{normalVel.x, normalVel.y, normalVel.z},
                              csm_normalBits);

    const auto writeVel = static_cast<UInt32>(m_spawnVelocityLen_ * 16.0f);
    pStream.writeInt(static_cast<Int32>(writeVel), 14);
}

bool Bullet::readInitialPacket(sim::network::GhostManager* io_pGM, BitStream& pStream) {
    if (!Parent::readInitialPacket(io_pGM, pStream)) {
        return false;
    }

    Point3F initialPosition{};
    if (!readPod(pStream, initialPosition)) {
        return false;
    }
    m_spawnPosition_ = initialPosition;

    const auto elapsed = static_cast<UInt32>(pStream.readInt(15));
    NormalVector3F normalVel{};
    pStream.readNormalVector(normalVel, csm_normalBits);
    if (!pStream.isValid()) {
        return false;
    }
    m_spawnVelocity_.set(normalVel.x, normalVel.y, normalVel.z);

    m_spawnVelocityLen_ = static_cast<Flt32>(pStream.readInt(14)) / 16.0f;
    if (!pStream.isValid()) {
        return false;
    }
    m_spawnVelocity_ *= m_spawnVelocityLen_;
    setLinearVelocity(m_spawnVelocity_);

    initialPosition += getLinearVelocity() * (static_cast<Flt32>(elapsed) * 0.001f);

    if (m_pShooter_) {
        TMat3F muzzleTrans{};
        if (m_pShooter_->getMuzzleTransform(0, &muzzleTrans)) {
            if (dynamic_cast<Player*>(m_pShooter_) != nullptr) {
                TMat3F total{};
                m_mul(muzzleTrans, m_pShooter_->getTransform(), total);
                muzzleTrans = total;
            }
            setPosition(muzzleTrans, true);
            return true;
        }
    }

    TMat3F transform = getTransform();
    transform.p = initialPosition;
    transform.flags |= RMat3F::Matrix_HasTranslation;
    setPosition(transform, true);
    return true;
}

UInt32 Bullet::packUpdate(sim::network::GhostManager& gm, UInt32 mask, BitStream& stream) const {
    UInt32 parentPunt = Parent::packUpdate(gm, mask, stream);

    if ((mask & Projectile::InitialUpdate) != 0) {
        stream.writeFlag(true);
        writeInitialPacket(&gm, stream);
        if (m_hasExploded_) {
            parentPunt |= (mask & FPExplosionMask);
        }
    } else {
        stream.writeFlag(false);
    }
    return parentPunt;
}

bool Bullet::unpackUpdate(sim::network::GhostManager& gm, BitStream& stream) {
    BitStreamRollbackGuard guard(stream);

    const auto savedDatId = datFileId_;
    const auto savedOwnerId = ownerId_;
    const auto savedShooter = m_pShooter_;
    const auto savedShooterGhost = m_explicitShooterGhostIndex_;
    const auto savedSpawnPos = m_spawnPosition_;
    const auto savedSpawnVel = m_spawnVelocity_;
    const auto savedSpawnVelLen = m_spawnVelocityLen_;
    const auto savedTrans = getTransform();
    const auto savedLinVel = getLinearVelocity();
    const auto savedExpPos = m_explosionPosition_;
    const auto savedExpAxis = m_explosionAxis_;
    const auto savedExpId = m_explosionId_;
    const auto savedHasExp = m_hasExploded_;
    const auto savedOverride = m_overrideRender_;
    const auto savedPending = m_pendingImpactEffect_;

    const auto rollback = [&]() noexcept {
        datFileId_ = savedDatId;
        ownerId_ = savedOwnerId;
        m_pShooter_ = savedShooter;
        m_explicitShooterGhostIndex_ = savedShooterGhost;
        m_spawnPosition_ = savedSpawnPos;
        m_spawnVelocity_ = savedSpawnVel;
        m_spawnVelocityLen_ = savedSpawnVelLen;
        SimMovement::setPosition(savedTrans, true);
        setLinearVelocity(savedLinVel);
        m_explosionPosition_ = savedExpPos;
        m_explosionAxis_ = savedExpAxis;
        m_explosionId_ = savedExpId;
        m_hasExploded_ = savedHasExp;
        m_overrideRender_ = savedOverride;
        m_pendingImpactEffect_ = savedPending;
        return false;
    };

    if (!Parent::unpackUpdate(gm, stream)) {
        return rollback();
    }

    const bool isInitial = stream.readFlag();
    if (!stream.isValid()) {
        return rollback();
    }
    if (isInitial) {
        if (!readInitialPacket(&gm, stream)) {
            return rollback();
        }
    }

    if (!stream.isValid()) {
        return rollback();
    }
    commitImpactEffect(false);
    guard.committed = true;
    return true;
}

void Bullet::clientProcess(UInt32 in_currTime) {
    // A new ghost can be stamped ahead of this frame's millisecond clock.
    // Never let unsigned elapsed time wrap into a multi-day movement step.
    if (in_currTime < m_lastUpdated_) {
        return;
    }
    if (m_hasExploded_ || in_currTime > m_totalTime_) {
        stopProjectileSound();
        return;
    }
    if (m_translucentShape_ && m_translucentThread_ && in_currTime > m_lastUpdated_) {
        const Flt32 deltaT = static_cast<Flt32>(in_currTime - m_lastUpdated_) / 1000.0f;
        m_translucentShape_->advanceThread(*m_translucentThread_, static_cast<Flt64>(deltaT));
    }
    if (m_pBulletData_ && m_pBulletData_->rotationPeriod != 0.0f) {
        const auto periodMs =
            std::max<UInt32>(1u, static_cast<UInt32>(m_pBulletData_->rotationPeriod * 1000.0f));
        const UInt32 rotation = (in_currTime - m_spawnTime_) % periodMs;
        const Flt32 fRotation = static_cast<Flt32>(rotation) / static_cast<Flt32>(periodMs);
        m_axisSpin_ = fRotation * kTwoPi;
    }

    const UInt32 slice = in_currTime - m_lastUpdated_;
    if (slice == 0) {
        return;
    }

    Point3F movePosition{};
    if (in_currTime <= m_endInterp_ && m_endInterp_ > m_startInterp_) {
        if (m_resetInterpFrom_) {
            if (m_pShooter_) {
                TMat3F muzzleTrans{};
                if (m_pShooter_->getMuzzleTransform(0, &muzzleTrans)) {
                    if (dynamic_cast<Player*>(m_pShooter_) != nullptr) {
                        TMat3F total{};
                        m_mul(muzzleTrans, m_pShooter_->getTransform(), total);
                        muzzleTrans = total;
                    }
                    m_interpFrom_ = muzzleTrans.p;
                }
            }
            m_resetInterpFrom_ = false;
        }
        const Flt32 factor = static_cast<Flt32>(in_currTime - m_startInterp_) /
                             static_cast<Flt32>(m_endInterp_ - m_startInterp_);
        movePosition.interpolate(m_interpFrom_, m_interpTo_, factor);
    } else {
        if (m_resetInterpFrom_) {
            if (m_pShooter_) {
                TMat3F muzzleTrans{};
                if (m_pShooter_->getMuzzleTransform(0, &muzzleTrans)) {
                    if (dynamic_cast<Player*>(m_pShooter_) != nullptr) {
                        TMat3F total{};
                        m_mul(muzzleTrans, m_pShooter_->getTransform(), total);
                        muzzleTrans = total;
                    }
                    TMat3F reallyFinal = getTransform();
                    reallyFinal.p = muzzleTrans.p;
                    reallyFinal.flags |= RMat3F::Matrix_HasTranslation;
                    setPosition(reallyFinal, true);
                }
            }
            m_resetInterpFrom_ = false;
        }
        movePosition =
            getLinearPosition() + getLinearVelocity() * (static_cast<Flt32>(slice) * 0.001f);
    }

    sim::SimCollisionInfo collisionInfo{};
    sim::SimContainerQuery collisionQuery{};
    collisionQuery.type = ~UInt32{0};
    // The initial long-range prediction is only a fallback. Terrain/interior
    // ghosts can arrive after the bullet, so sweep the actual travelled segment
    // against them as well as the dynamic objects on every frame.
    collisionQuery.mask = csm_collisionMask;
    collisionQuery.detail = sim::SimCollisionImageQuery::Normal;
    collisionQuery.box.fMin = getTransform().p;
    collisionQuery.box.fMax = movePosition;

    if (!m_projectileData_ || !m_projectileData_->collideWithOwner || m_ownerGraceMS_ > 0) {
        collisionQuery.id = getOwnerId();
    } else {
        collisionQuery.id = static_cast<sim::SimObjectId>(-1);
    }
    decayGraceTime(in_currTime - m_lastUpdated_);

    sim::SimContainer* root =
        getManager()
            ? dynamic_cast<sim::SimContainer*>(getManager()->findObject(sim::RootContainerId))
            : nullptr;
    if (root && root->findLOS(collisionQuery, collisionInfo, sim::SimCollisionImageQuery::High)) {
        sim::SimObject* hitObj = collisionInfo.object.resolve(*getManager());
        if (isProjStatic(hitObj)) {
            collisionInfo.transform(collisionInfo.surfaces.tWorld);
            auto pExplosion = std::make_unique<Explosion>(getExplosionTag(collisionInfo));
            pExplosion->bindDataBlockManager(resolveDataBlockManager());
            if (!collisionInfo.surfaces.surfaces.empty()) {
                pExplosion->setPosition(collisionInfo.surfaces.surfaces[0].position);
                pExplosion->setAxis(collisionInfo.surfaces.surfaces[0].normal);
            }
            getManager()->addObject(std::move(pExplosion));
            m_hasExploded_ = true;
            m_overrideRender_ = true;
            stopProjectileSound();
        }
    } else {
        if (in_currTime > m_finalTime_) {
            if (getManager()) {
                auto pExplosion = std::make_unique<Explosion>(getExplosionTag(storedInfo_));
                pExplosion->bindDataBlockManager(resolveDataBlockManager());
                pExplosion->setPosition(m_finalPosition_);
                pExplosion->setAxis(m_finalNormal_);
                getManager()->addObject(std::move(pExplosion));
            }
            m_hasExploded_ = true;
            m_overrideRender_ = true;
            stopProjectileSound();
            return;
        }

        TMat3F finalTransform = getTransform();
        finalTransform.p = movePosition;
        finalTransform.flags |= RMat3F::Matrix_HasTranslation;
        setPosition(finalTransform, true);
        m_lastUpdated_ = in_currTime;
    }

    if (!m_overrideRender_) {
        updateProjectileSound(getTransform(), getLinearVelocity());
    }
}

void Bullet::predictForward(UInt32 in_currTime,
                            UInt32 in_predictTime,
                            Point3F& out_rPosition) const noexcept {
    const Point3F startPos = getLinearPosition();
    const Flt32 timeFloat = static_cast<Flt32>(in_predictTime - in_currTime) * 0.001f;
    out_rPosition = startPos + (m_spawnVelocity_ * timeFloat);
}

Int32 Bullet::getBulletHoleIndex(const sim::SimCollisionInfo& io_rCollision) const noexcept {
    if (!getManager() || !m_pBulletData_) {
        return -1;
    }
    const auto* obj = io_rCollision.object.resolve(*getManager());
    if (!obj) {
        return -1;
    }
    if ((obj->getType() & SimInteriorObjectType) != 0 && m_pBulletData_->bulletHoleIndex != -1) {
        return 0;
    }
    return -1;
}

void Bullet::spawnImpactEffect(sim::SimCollisionInfo& io_rCollision) {
    if (isGhost()) {
        return;
    }
    m_explosionId_ = getExplosionTag(io_rCollision);
    if (!io_rCollision.surfaces.surfaces.empty()) {
        m_explosionPosition_ = io_rCollision.surfaces.surfaces[0].position;
        m_explosionAxis_ = io_rCollision.surfaces.surfaces[0].normal;
    } else {
        m_explosionPosition_ = getLinearPosition();
        m_explosionAxis_.set(0.0f, 0.0f, 1.0f);
    }
    m_hasExploded_ = true;

    // A client's prediction may not yet contain the terrain/interior that the
    // server hit. Always replicate the impact; readExplosion suppresses a
    // duplicate effect when the client has already predicted it.
    setMaskBits(FPExplosionMask);
}

bool Bullet::readExplosion(BitStream& io_pStream) {
    const Int32 ms = io_pStream.readInt(15);
    NormalVector3F axis{};
    io_pStream.readNormalVector(axis, 6);
    if (!io_pStream.isValid()) {
        return false;
    }
    Int32 expBits = 6;
    if (auto* dbm = resolveDataBlockManager()) {
        expBits = dbm->getDataGroupBitSize(DataBlockManager::ExplosionDataType);
    }
    const Int32 expId = io_pStream.readInt(expBits);
    if (!io_pStream.isValid()) {
        return false;
    }

    m_explosionPosition_ =
        m_spawnPosition_ + (m_spawnVelocity_ * (static_cast<Flt32>(ms) / 1000.0f));
    m_explosionAxis_.set(axis.x, axis.y, axis.z);
    m_explosionId_ = expId;

    // Derived fields still follow this base payload. Emit only after the
    // complete update succeeds, so a retry cannot duplicate sound/effects.
    m_pendingImpactEffect_ = !m_hasExploded_;
    return true;
}

void Bullet::writeExplosion(BitStream& io_pStream) const {
    const Flt32 len1 = (m_explosionPosition_ - m_spawnPosition_).len();
    const Flt32 len2 = (m_spawnVelocityLen_ > 0.0f) ? m_spawnVelocityLen_ : 1.0f;
    const auto expTime = static_cast<UInt32>((len1 / len2) * 1000.0f);

    io_pStream.writeInt(static_cast<Int32>(expTime), 15);
    io_pStream.writeNormalVector(
        NormalVector3F{m_explosionAxis_.x, m_explosionAxis_.y, m_explosionAxis_.z}, 6);
    Int32 expBits = 6;
    if (auto* dbm = resolveDataBlockManager()) {
        expBits = dbm->getDataGroupBitSize(DataBlockManager::ExplosionDataType);
    }
    io_pStream.writeInt(m_explosionId_, expBits);
}

std::shared_ptr<sim::SimCollisionImage> Bullet::getSelfImage() {
    m_sphereCollImage_->transform = getTransform();
    m_sphereCollImage_->center.set(0.0f, 0.0f, 0.0f);
    m_sphereCollImage_->radius = m_projectileData_ ? m_projectileData_->collisionRadius : 0.0f;
    return m_sphereCollImage_;
}

bool Bullet::onSimLightQuery(sim::SimLightQuery& query) const {
    if (m_overrideRender_ || !m_projectileData_ || m_projectileData_->lightRange == 0.0f) {
        return false;
    }
    const UInt32 nowMs = currentTimeMs(getManager(), m_lastUpdated_);
    if (nowMs >= m_liveTime_ && m_totalTime_ > m_liveTime_) {
        Flt32 value = 1.0f - (static_cast<Flt32>(nowMs - m_liveTime_) /
                              static_cast<Flt32>(m_totalTime_ - m_liveTime_));
        value = std::clamp(value, 0.0f, 1.0f);
        m_light_.intensity = ColorF{m_projectileData_->lightColor.red * value,
                                    m_projectileData_->lightColor.green * value,
                                    m_projectileData_->lightColor.blue * value};
    } else {
        m_light_.intensity = m_projectileData_->lightColor;
    }
    m_light_.type = sim::SimLight::Type::Point;
    m_light_.range = m_projectileData_->lightRange;
    m_light_.position = getLinearPosition();
    query.lights.push_back(m_light_);
    return true;
}

bool Bullet::onWorldSurfaceQuery(world::WorldSurfaceQuery& query) {
    const UInt32 nowMs = currentTimeMs(getManager(), m_lastUpdated_);
    if (!m_isTracer_ || m_overrideRender_ || m_hasExploded_ || m_waitingForDelete_ ||
        !m_pBulletData_ || !query.camera || nowMs < m_spawnTime_ || nowMs >= m_totalTime_ ||
        m_spawnVelocityLen_ <= 0.0f || m_pBulletData_->tracerLength <= 0.0f) {
        return false;
    }

    const Flt32 trailSeconds = m_pBulletData_->tracerLength / m_spawnVelocityLen_;
    const Flt32 ageSeconds = static_cast<Flt32>(nowMs - m_spawnTime_) * 0.001f;
    // Start the streak beyond the muzzle, then grow it to the authored length.
    const Flt32 visibleSeconds = std::min(trailSeconds, ageSeconds - trailSeconds * 0.5f);
    if (visibleSeconds <= 0.0f) {
        return false;
    }
    const Point3F end = getLinearPosition();
    const Point3F start = end - getLinearVelocity() * visibleSeconds;

    const auto& camera = *query.camera;
    TMat3F cameraToWorld = camera.worldToCamera;
    cameraToWorld.inverse();
    Point3F forward;
    cameraToWorld.getRow(1, forward);
    Point3F side;
    m_cross(end - start, forward, side);
    if (side.len() <= 0.00001f) {
        cameraToWorld.getRow(0, side);
    }
    side.normalize();
    Point3F cameraEnd;
    m_mul(end, camera.worldToCamera, cameraEnd);
    // A one-pixel tip remains visible as the high-speed tracer moves away.
    const Flt32 halfWidth =
        0.5f * (camera.perspective ? std::max(camera.nearDistance, cameraEnd.y) : 1.0f) /
        std::max(1.0f, camera.focal.x);
    side *= halfWidth;

    if (!m_tracerTexture_) {
        auto bitmap = gfx::Bitmap::create(1, 1);
        if (!bitmap) {
            return false;
        }
        bitmap->paletteIndex = 1976;
        bitmap->attributes |= gfx::BitmapTranslucent;
        bitmap->pixels[0] = std::byte{250};
        ResourceManager resources;
        if (!resources.add("bullet-tracer.bmp", std::move(bitmap))) {
            return false;
        }
        m_tracerTexture_ = resources.load<gfx::Bitmap>("bullet-tracer.bmp");
    }

    auto mesh = std::make_shared<world::WorldSurfaceMesh>();
    mesh->materials.list.details = mesh->materials.list.materialsPerDetail = 1;
    world::TsMaterial material;
    material.flags = 0x2103; // Unlit indexed translucency, matching the legacy tracer fill.
    mesh->materials.list.materials.push_back(material);
    mesh->materials.textures.push_back(m_tracerTexture_);
    world::StaticSurfaceTriangle triangle;
    triangle.vertices[0].position = start;
    triangle.vertices[1].position = end - side;
    triangle.vertices[2].position = end + side;
    for (auto& vertex : triangle.vertices) {
        vertex.texture = {0.5f, 0.5f};
    }
    mesh->triangles.push_back(triangle);
    std::swap(triangle.vertices[1], triangle.vertices[2]);
    mesh->triangles.push_back(triangle);
    mesh->bounds = {start, start};
    for (const auto& vertex : triangle.vertices) {
        mesh->bounds.fMin.setMin(vertex.position);
        mesh->bounds.fMax.setMax(vertex.position);
    }
    query.image = world::WorldSurfaceInstance{std::move(mesh), TMat3F{}};
    return true;
}

bool Bullet::processQuery(sim::SimQuery& query) {
    if (auto* lq = dynamic_cast<sim::SimLightQuery*>(&query)) {
        return onSimLightQuery(*lq);
    }
    if (auto* surface = dynamic_cast<world::WorldSurfaceQuery*>(&query)) {
        return onWorldSurfaceQuery(*surface);
    }
    if (auto* shape = dynamic_cast<world::WorldShapeQuery*>(&query)) {
        if (m_isTracer_ || currentTimeMs(getManager(), m_lastUpdated_) >= m_totalTime_) {
            return false;
        }
        const bool rendered = Parent::processQuery(query);
        // A ghost may render before its first clientProcess. Legacy samples the
        // current muzzle here too, so movement between packet receipt and render
        // cannot leave a new blaster bolt hanging above or behind the gun.
        if (rendered && shape->image && m_resetInterpFrom_ && m_pShooter_) {
            TMat3F muzzle;
            if (m_pShooter_->getMuzzleTransform(0, &muzzle)) {
                if (dynamic_cast<Player*>(m_pShooter_)) {
                    TMat3F world;
                    m_mul(muzzle, m_pShooter_->getTransform(), world);
                    muzzle = world;
                }
                shape->image->shapeToWorld.p += muzzle.p - getLinearPosition();
            }
        }
        return rendered;
    }
    return Parent::processQuery(query);
}

void Bullet::setSearchBoundaries() {
    const UInt32 nowMs = currentTimeMs(getManager(), m_lastUpdated_);
    const Flt32 totalTimeSec = m_projectileData_ ? m_projectileData_->totalTime : 10.0f;

    sim::SimContainer* pRoot =
        getManager()
            ? dynamic_cast<sim::SimContainer*>(getManager()->findObject(sim::RootContainerId))
            : nullptr;
    if (pRoot) {
        sim::SimContainerQuery query{};
        query.id = m_pShooter_ ? m_pShooter_->getId() : static_cast<sim::SimObjectId>(-1);
        query.type = ~UInt32{0};
        query.mask = SimInteriorObjectType | SimTerrainObjectType;
        query.detail = sim::SimCollisionImageQuery::Normal;
        query.box.fMin = getLinearPosition();
        query.box.fMax = getLinearPosition() + (getLinearVelocity() * totalTimeSec);

        if (pRoot->findLOS(query, storedInfo_, sim::SimCollisionImageQuery::High) &&
            !storedInfo_.surfaces.surfaces.empty()) {
            m_mul(storedInfo_.surfaces.surfaces[0].position,
                  storedInfo_.surfaces.tWorld,
                  m_finalPosition_);
            m_mul(storedInfo_.surfaces.surfaces[0].normal,
                  static_cast<const RMat3F&>(storedInfo_.surfaces.tWorld),
                  m_finalNormal_);
            m_finalPosition_ += m_finalNormal_ * 0.10f;
            m_finalObject_ = storedInfo_.object.resolve(*getManager());

            const Point3F travel = m_finalPosition_ - getLinearPosition();
            const Flt32 spd = getLinearVelocity().len();
            const Flt32 time = (spd > 0.0f) ? (travel.len() / spd) : 0.0f;
            m_finalTime_ = nowMs + static_cast<UInt32>(time * 1000.0f);
            return;
        }
    }

    m_finalTime_ = nowMs + static_cast<UInt32>(totalTimeSec * 1000.0f) + 10000u;
    m_finalPosition_.set(0.0f, 0.0f, 0.0f);
    m_finalNormal_.set(0.0f, 0.0f, 1.0f);
    m_finalObject_ = nullptr;
}

void Bullet::serverProcess(UInt32 in_currTime) {
    if (in_currTime < m_lastUpdated_) {
        return;
    }
    if (m_waitingForDelete_) {
        if (in_currTime >= m_deleteTime_) {
            deleteObject();
        }
        return;
    }

    if (m_projectileIsLive_ && in_currTime > m_liveTime_) {
        killProjectile();
    } else if (in_currTime > m_totalTime_) {
        destroyProjectile();
        m_waitingForDelete_ = true;
        m_deleteTime_ = in_currTime + 500;
        return;
    }

    UInt32 slice = in_currTime - m_lastUpdated_;
    if (slice == 0) {
        return;
    }
    m_lastUpdated_ = in_currTime;

    if (in_currTime > m_finalTime_) {
        const UInt32 over = in_currTime - m_finalTime_;
        slice = (slice > over) ? (slice - over) : 0;
    }

    sim::SimCollisionInfo info{};
    sim::SimContainerQuery query{};
    query.id = static_cast<sim::SimObjectId>(-1);
    query.type = ~UInt32{0};
    query.mask = csm_collisionMask;
    query.detail = sim::SimCollisionImageQuery::Normal;
    query.box.fMin = getLinearPosition();
    query.box.fMax =
        getLinearPosition() + (getLinearVelocity() * (static_cast<Flt32>(slice) * 0.001f));

    if (!m_projectileData_ || !m_projectileData_->collideWithOwner || m_ownerGraceMS_ > 0) {
        query.id = getOwnerId();
    }
    decayGraceTime(slice);

    sim::SimContainer* pRoot =
        getManager()
            ? dynamic_cast<sim::SimContainer*>(getManager()->findObject(sim::RootContainerId))
            : nullptr;
    if (pRoot && pRoot->findLOS(query, info, sim::SimCollisionImageQuery::High)) {
        TMat3F finalTransform = getTransform();
        if (!info.surfaces.surfaces.empty()) {
            m_mul(info.surfaces.surfaces[0].position, info.surfaces.tWorld, finalTransform.p);
        }
        setPosition(finalTransform, true);

        sim::SimObject* hitObj = info.object.resolve(*getManager());
        if (m_projectileData_) {
            if (m_projectileData_->damageClass == 0) {
                applyBaseDamage(info, hitObj);
                if (!info.surfaces.surfaces.empty()) {
                    info.surfaces.surfaces[0].transform(info.surfaces.tWorld);
                }
                spawnImpactEffect(info);
            } else if (m_projectileData_->damageClass == 1) {
                applyRadiusDamage(getLinearPosition());
                spawnImpactEffect(getLinearPosition());
            }
        }
        m_waitingForDelete_ = true;
        m_deleteTime_ = in_currTime + 500;
    } else {
        if (in_currTime >= m_finalTime_) {
            if (m_finalObject_ && m_projectileData_) {
                TMat3F finalTransform = getTransform();
                finalTransform.p = m_finalPosition_;
                finalTransform.flags |= RMat3F::Matrix_HasTranslation;
                setPosition(finalTransform, true);
                if (m_projectileData_->damageClass == 0) {
                    applyBaseDamage(storedInfo_, m_finalObject_);
                    if (!storedInfo_.surfaces.surfaces.empty()) {
                        storedInfo_.surfaces.surfaces[0].transform(storedInfo_.surfaces.tWorld);
                    }
                    spawnImpactEffect(storedInfo_);
                } else if (m_projectileData_->damageClass == 1) {
                    applyRadiusDamage(m_finalPosition_);
                    spawnImpactEffect(m_finalPosition_);
                }
            }
            m_waitingForDelete_ = true;
            m_deleteTime_ = in_currTime + 500;
            return;
        }
        TMat3F finalTransform = getTransform();
        finalTransform.p = query.box.fMax;
        finalTransform.flags |= RMat3F::Matrix_HasTranslation;
        setPosition(finalTransform, true);
    }
}

} // namespace opent1::game
