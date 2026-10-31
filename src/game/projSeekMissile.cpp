#include <opent1/game/flier.hpp>
#include <opent1/game/player.hpp>
#include <opent1/game/proj_seek_missile.hpp>
#include <opent1/game/smoke_puff.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <numbers>

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

SeekingMissile::SeekingMissile(Int32 in_datFileId) : Projectile(in_datFileId) {
    setForceMask(0);
    setNetFlags(Ghostable);
}

SeekingMissile::SeekingMissile() : SeekingMissile(-1) {}

bool SeekingMissile::initResources(GameBaseData* io_pDat) {
    if (!Parent::initResources(io_pDat)) {
        return false;
    }
    m_pMissileData_ = dynamic_cast<SeekingMissileData*>(io_pDat);
    if (!m_pMissileData_) {
        return false;
    }
    setDrag(1.0f);
    return true;
}

void SeekingMissile::loadResources(bool in_isGhost) {
    Parent::loadResources(in_isGhost);
    if (in_isGhost) {
        setDrag(1.0f);
    }
}

void SeekingMissile::killProjectile() {
    Parent::killProjectile();
    setForceMask(sim::MovementGravityForce);
}

void SeekingMissile::destroyProjectile() {
    m_hasExploded_ = true;
    m_timeoutExplosion_ = true;
    setMaskBits(FPExplosionMask);
}

Int32 SeekingMissile::targetGhostIndex(const sim::network::GhostManager* gm) const noexcept {
    if (m_explicitTargetGhostIndex_ != -1) {
        return m_explicitTargetGhostIndex_;
    }
    if (m_pTarget_ && gm) {
        const auto index = gm->getGhostIndex(*m_pTarget_);
        return index == sim::InvalidGhostIndex ? -1 : static_cast<Int32>(index);
    }
    return -1;
}

bool SeekingMissile::onAdd() {
    if (!Parent::onAdd() || !m_pMissileData_) {
        return false;
    }

    setMass(m_projectileData_->mass);

    const UInt32 nowMs = currentTimeMs(getManager(), m_lastUpdated_);
    if (isGhost()) {
        m_renderFacingCamera_ = true;
        m_lastGhosted_ = nowMs;
        m_startInterp_ = nowMs;
        m_endInterp_ = nowMs + 64u;
        m_interpFrom_ = getLinearPosition();

        Point3F newVel{};
        predictForward(nowMs, nowMs + 64u, m_interpTo_, newVel);
        setLinearVelocity(newVel);
    } else {
        m_lastGhosted_ = nowMs;
    }

    m_lastPuff_ = nowMs;
    m_lastPuffPosition_ = getLinearPosition();
    m_lastTurn_ = nowMs;
    updateTrailPercentage();
    return true;
}

void SeekingMissile::buildContainerBox(const TMat3F& in_rMat, Box3F* out_pBox) {
    out_pBox->fMin = in_rMat.p;
    out_pBox->fMax = in_rMat.p;
}

Box3F SeekingMissile::buildContainerBox(const TMat3F& in_rMat) const {
    return Box3F{in_rMat.p, in_rMat.p};
}

void SeekingMissile::writeInitialPacket(sim::network::GhostManager* gm, BitStream& pStream) const {
    Parent::writeInitialPacket(gm, pStream);
    const Point3F pos = getLinearPosition();
    const Point3F vel = getLinearVelocity();
    writePod(pStream, pos);
    writePod(pStream, vel);
}

bool SeekingMissile::readInitialPacket(sim::network::GhostManager* io_pGM, BitStream& pStream) {
    if (!Parent::readInitialPacket(io_pGM, pStream)) {
        return false;
    }

    Point3F position{};
    Point3F velocity{};
    if (!readPod(pStream, position) || !readPod(pStream, velocity)) {
        return false;
    }
    setLinearVelocity(velocity * 0.5f);

    TMat3F transform = getTransform();
    transform.p = position;
    transform.flags |= RMat3F::Matrix_HasTranslation;
    setPosition(transform, true);
    return true;
}

UInt32
SeekingMissile::packUpdate(sim::network::GhostManager& gm, UInt32 mask, BitStream& stream) const {
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

    if ((mask & Projectile::FPPositionMask) != 0) {
        stream.writeFlag(true);
        const Point3F pos = getLinearPosition();
        writePod(stream, pos);
    } else {
        stream.writeFlag(false);
    }

    return parentPunt;
}

bool SeekingMissile::unpackUpdate(sim::network::GhostManager& gm, BitStream& stream) {
    BitStreamRollbackGuard guard(stream);

    const auto savedDatId = datFileId_;
    const auto savedOwnerId = ownerId_;
    const auto savedShooter = m_pShooter_;
    const auto savedShooterGhost = m_explicitShooterGhostIndex_;
    const auto savedTarget = m_pTarget_;
    const auto savedTargetGhost = m_explicitTargetGhostIndex_;
    const auto savedTrans = getTransform();
    const auto savedLinVel = getLinearVelocity();
    const auto savedExpPos = m_explosionPosition_;
    const auto savedExpAxis = m_explosionAxis_;
    const auto savedExpId = m_explosionId_;
    const auto savedHasExp = m_hasExploded_;
    const auto savedOverride = m_overrideRender_;
    const auto savedPending = m_pendingImpactEffect_;
    const auto savedStartInterp = m_startInterp_;
    const auto savedEndInterp = m_endInterp_;
    const auto savedInterpFrom = m_interpFrom_;
    const auto savedInterpTo = m_interpTo_;

    const auto rollback = [&]() noexcept {
        datFileId_ = savedDatId;
        ownerId_ = savedOwnerId;
        m_pShooter_ = savedShooter;
        m_explicitShooterGhostIndex_ = savedShooterGhost;
        m_pTarget_ = savedTarget;
        m_explicitTargetGhostIndex_ = savedTargetGhost;
        SimMovement::setPosition(savedTrans, true);
        setLinearVelocity(savedLinVel);
        m_explosionPosition_ = savedExpPos;
        m_explosionAxis_ = savedExpAxis;
        m_explosionId_ = savedExpId;
        m_hasExploded_ = savedHasExp;
        m_overrideRender_ = savedOverride;
        m_pendingImpactEffect_ = savedPending;
        m_startInterp_ = savedStartInterp;
        m_endInterp_ = savedEndInterp;
        m_interpFrom_ = savedInterpFrom;
        m_interpTo_ = savedInterpTo;
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

    if (stream.readFlag()) {
        if (!stream.isValid()) {
            return rollback();
        }
        m_interpFrom_ = getLinearPosition();

        Point3F position{};
        if (!readPod(stream, position)) {
            return rollback();
        }

        TMat3F transform = getTransform();
        transform.p = position;
        transform.flags |= RMat3F::Matrix_HasTranslation;
        setPosition(transform, true);

        const UInt32 nowMs = currentTimeMs(getManager(), m_lastUpdated_);
        m_startInterp_ = nowMs;
        m_endInterp_ = nowMs + 64u;
        m_lastUpdated_ = nowMs;

        Point3F newVel{};
        predictForward(nowMs, m_endInterp_, m_interpTo_, newVel);
    }

    if (!stream.isValid()) {
        return rollback();
    }
    commitImpactEffect(true);
    guard.committed = true;
    return true;
}

void SeekingMissile::predictForward(UInt32 in_currTime,
                                    UInt32 in_predictTime,
                                    Point3F& out_rPosition,
                                    Point3F& out_rVelocity) {
    const TMat3F oldPosition = getTransform();
    const Point3F oldVelocity = getLinearVelocity();
    const UInt32 oldUpdated = m_lastUpdated_;
    const Int32 oldGrace = m_ownerGraceMS_;
    const Flt32 oldTimeSlice = getTimeSlice();

    sim::SimCollisionInfo dummyCollision{};
    sim::SimObject* pDummyObject = nullptr;
    m_lastUpdated_ = in_currTime;
    moveProjectile(in_predictTime, dummyCollision, pDummyObject, false, true);

    out_rPosition = getLinearPosition();
    out_rVelocity = getLinearVelocity();

    m_lastUpdated_ = oldUpdated;
    m_ownerGraceMS_ = oldGrace;
    setTimeSlice(oldTimeSlice);
    setLinearVelocity(oldVelocity);
    setPosition(oldPosition, true);
}

void SeekingMissile::clientProcess(UInt32 in_currTime) {
    // A new ghost can be stamped ahead of this frame's millisecond clock.
    // Never let unsigned elapsed time wrap into a multi-day movement step.
    if (in_currTime < m_lastUpdated_) {
        return;
    }
    if (m_hasExploded_) {
        return;
    }

    if (m_translucentShape_ && m_translucentThread_ && in_currTime > m_lastUpdated_) {
        const Flt32 deltaT = static_cast<Flt32>(in_currTime - m_lastUpdated_) / 1000.0f;
        m_translucentShape_->advanceThread(*m_translucentThread_, static_cast<Flt64>(deltaT));
    }

    Point3F movePosition{};
    if (in_currTime <= m_endInterp_ && m_endInterp_ > m_startInterp_) {
        const Flt32 factor = static_cast<Flt32>(in_currTime - m_startInterp_) /
                             static_cast<Flt32>(m_endInterp_ - m_startInterp_);
        movePosition.interpolate(m_interpFrom_, m_interpTo_, factor);

        decayGraceTime(in_currTime - m_lastUpdated_);
        m_lastUpdated_ = in_currTime;

        TMat3F finalPosition = getTransform();
        finalPosition.p = movePosition;
        finalPosition.flags |= RMat3F::Matrix_HasTranslation;
        setPosition(finalPosition, true);
    } else {
        m_startInterp_ = 0;
        m_endInterp_ = 0;

        sim::SimCollisionInfo collision{};
        sim::SimObject* pObject = nullptr;
        if (moveProjectile(in_currTime, collision, pObject, true, true)) {
            if (isProjStatic(pObject)) {
                collision.transform(collision.surfaces.tWorld);
                if (getManager()) {
                    auto pExplosion = std::make_unique<Explosion>(getExplosionTag(collision));
                    pExplosion->bindDataBlockManager(resolveDataBlockManager());
                    if (!collision.surfaces.surfaces.empty()) {
                        pExplosion->setPosition(collision.surfaces.surfaces[0].position);
                        pExplosion->setAxis(collision.surfaces.surfaces[0].normal);
                    }
                    getManager()->addObject(std::move(pExplosion));
                }
                m_hasExploded_ = true;
                m_overrideRender_ = true;
                stopProjectileSound();
                return;
            }
        }
    }

    if (m_pMissileData_ && getManager() && m_pMissileData_->projSpecialTime > 0.0f) {
        const Point3F delta = getLinearPosition() - m_lastPuffPosition_;
        const Flt32 length = delta.len();
        const Int32 count = static_cast<Int32>(length / m_pMissileData_->projSpecialTime);
        if (count > 0) {
            if (auto* smoke = SmokeManager::get(getManager())) {
                const Point3F step = delta * (m_pMissileData_->projSpecialTime / length);
                for (Int32 i = 0; i < count; ++i) {
                    m_lastPuffPosition_ += step;
                    if (m_pMissileData_->smokeShape) {
                        smoke->addPuff(m_pMissileData_->smokeShape, 0.4f, m_lastPuffPosition_, {});
                    } else {
                        smoke->addPuff("rSmoke.dts", 0.4f, m_lastPuffPosition_, {});
                    }
                }
            }
        }
    }
    updateProjectileSound(getTransform(), getLinearVelocity());
}

void SeekingMissile::serverProcess(UInt32 in_currTime) {
    if (in_currTime < m_lastUpdated_) {
        return;
    }
    if (m_waitingForDelete_) {
        if (in_currTime >= m_deleteTime_) {
            deleteObject();
        }
        return;
    }
    if (!m_projectileData_ || !m_pMissileData_) {
        return;
    }
    if (in_currTime - m_lastTurn_ > 150u) {
        updateTrailPercentage();
        m_lastTurn_ = in_currTime;
    }
    const UInt32 slice = in_currTime - m_lastUpdated_;
    if (slice == 0) {
        return;
    }

    if (m_pTarget_ && !m_lockLost_) {
        const Point3F direct = m_pTarget_->getBoxCenter() - getLinearPosition();
        Point3F direction = direct;
        if (m_projectileData_->muzzleVelocity > 0.0f) {
            direction += m_pTarget_->getLinearVelocity() *
                         (direct.len() / m_projectileData_->muzzleVelocity);
        }
        bool visible = true;
        if (auto* root = getManager() ? dynamic_cast<sim::SimContainer*>(
                                            getManager()->findObject(sim::RootContainerId))
                                      : nullptr) {
            sim::SimContainerQuery query{};
            query.id = getId();
            query.type = ~UInt32{0};
            query.mask = csm_collisionMask & ~SimPlayerObjectType;
            query.detail = -1;
            query.box = Box3F{getLinearPosition(), getLinearPosition() + direction};
            sim::SimCollisionInfo obstacle{};
            if (root->findLOS(query, obstacle, sim::SimCollisionImageQuery::High) &&
                obstacle.object.resolve(*getManager()) != m_pTarget_) {
                direction = direct;
                query.box.fMax = getLinearPosition() + direction;
                visible = !root->findLOS(query, obstacle, sim::SimCollisionImageQuery::High) ||
                          obstacle.object.resolve(*getManager()) == m_pTarget_;
            }
        }
        Point3F velocity = getLinearVelocity();
        const Flt32 speed = velocity.len();
        if (speed > 0.0f && !direction.isZero()) {
            velocity /= speed;
            direction.normalize();
            const Flt32 dot = std::clamp(m_dot(velocity, direction), -1.0f, 1.0f);
            if (dot < 0.0f || !visible) {
                m_lockLost_ = true;
                killProjectile();
            } else {
                const Flt32 angle = m_angularVelocity_ * static_cast<Flt32>(slice) * 0.001f;
                if (std::cos(angle) > dot) {
                    Point3F normal;
                    m_cross(direction, velocity, normal);
                    if (!normal.isZero()) {
                        normal.normalize();
                        Point3F turned;
                        m_mul(velocity, RMat3F{AngAxisF{normal, angle}}, turned);
                        setLinearVelocity(turned * m_projectileData_->muzzleVelocity);
                    }
                } else {
                    setLinearVelocity(direction * speed);
                }
            }
        }
    }

    const bool nearTarget =
        m_pTarget_ &&
        (m_pTarget_->getBoxCenter() - getLinearPosition()).len() <= m_pMissileData_->proximityDist;
    sim::SimCollisionInfo collision{};
    sim::SimObject* object = nullptr;
    const bool hit = !nearTarget && moveProjectile(in_currTime, collision, object, true, true);
    if (hit || nearTarget || in_currTime > m_liveTime_ || in_currTime > m_totalTime_) {
        Point3F position = getLinearPosition();
        if (hit && !collision.surfaces.surfaces.empty()) {
            collision.transform(collision.surfaces.tWorld);
            position = collision.surfaces.surfaces.front().position;
        }
        applyRadiusDamage(position);
        if (hit && !collision.surfaces.surfaces.empty()) {
            spawnImpactEffect(collision);
        } else {
            spawnImpactEffect(position);
        }
        TMat3F transform = getTransform();
        transform.p = position;
        transform.flags |= RMat3F::Matrix_HasTranslation;
        setPosition(transform, true);
        m_waitingForDelete_ = true;
        m_deleteTime_ = in_currTime + 500u;
    }
    setMaskBits(FPPositionMask | FPVelocityMask);
}

void SeekingMissile::spawnImpactEffect(sim::SimCollisionInfo& io_rCollision) {
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

    setMaskBits(FPExplosionMask);
}

bool SeekingMissile::readExplosion(BitStream& io_pStream) {
    Point3F position{};
    Point3F axis{};
    Int32 id = -1;

    if (io_pStream.readFlag()) {
        if (!io_pStream.isValid()) {
            return false;
        }
        position = getLinearPosition();
        axis = Point3F{0.0f, 1.0f, 0.0f};
        id = getExplosionTag();
    } else {
        if (!readPod(io_pStream, position)) {
            return false;
        }
        NormalVector3F normAxis{};
        io_pStream.readNormalVector(normAxis, 5);
        const auto* dbm = resolveDataBlockManager();
        const Int32 expBits =
            dbm ? dbm->getDataGroupBitSize(DataBlockManager::ExplosionDataType) : 6;
        id = io_pStream.readInt(expBits);
        if (!io_pStream.isValid()) {
            return false;
        }
        axis.set(normAxis.x, normAxis.y, normAxis.z);
    }

    m_explosionPosition_ = position;
    m_explosionAxis_ = axis;
    m_explosionId_ = id;

    TMat3F newMat = getTransform();
    newMat.p = position;
    newMat.flags |= RMat3F::Matrix_HasTranslation;
    setPosition(newMat, true);

    m_pendingImpactEffect_ = !m_hasExploded_;
    return true;
}

void SeekingMissile::writeExplosion(BitStream& io_pStream) const {
    if (m_timeoutExplosion_) {
        io_pStream.writeFlag(true);
    } else {
        io_pStream.writeFlag(false);
        writePod(io_pStream, m_explosionPosition_);
        io_pStream.writeNormalVector(
            NormalVector3F{m_explosionAxis_.x, m_explosionAxis_.y, m_explosionAxis_.z}, 5);
        const auto* dbm = resolveDataBlockManager();
        const Int32 expBits =
            dbm ? dbm->getDataGroupBitSize(DataBlockManager::ExplosionDataType) : 6;
        io_pStream.writeInt(m_explosionId_, expBits);
    }
}

bool SeekingMissile::isTargetable() const noexcept {
    return true;
}

void SeekingMissile::setTarget(ShapeBase* io_pTarget) {
    if (m_pTarget_) {
        clearNotify(*m_pTarget_);
    }
    m_pTarget_ = io_pTarget;
    if (m_pTarget_) {
        deleteNotify(*m_pTarget_);
    }
}

Flt32 SeekingMissile::getTargetRange() const noexcept {
    return 1000.0f;
}

void SeekingMissile::onDeleteNotify(sim::SimObject& io_pDelete) {
    if (&io_pDelete == m_pTarget_) {
        m_pTarget_ = nullptr;
    }
    Parent::onDeleteNotify(io_pDelete);
}

void SeekingMissile::updateTrailPercentage() {
    m_currTrailPercentage_ = 1.0f;
    if (m_pTarget_) {
        if (auto* console = resolveConsole()) {
            const std::string callback = scriptName("updateTargetPercentage");
            if (!callback.empty()) {
                const std::array<std::string, 1> arguments{std::to_string(m_pTarget_->getId())};
                m_currTrailPercentage_ = static_cast<Flt32>(
                    script::toNumber(console->execute(callback, arguments).value));
            }
        }
    }
    if (!m_pMissileData_) {
        return;
    }
    const Flt32 radius =
        m_pMissileData_->nonSeekingTurningRadius +
        (m_pMissileData_->seekingTurningRadius - m_pMissileData_->nonSeekingTurningRadius) *
            m_currTrailPercentage_;
    m_angularVelocity_ = radius > 0.0f ? m_pMissileData_->muzzleVelocity /
                                             (2.0f * std::numbers::pi_v<Flt32> * radius)
                                       : 0.0f;
}
} // namespace opent1::game
