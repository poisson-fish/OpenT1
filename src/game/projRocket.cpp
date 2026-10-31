#include <opent1/game/player.hpp>
#include <opent1/game/proj_rocket.hpp>
#include <opent1/game/smoke_puff.hpp>
#include <opent1/world/scene_objects.hpp>
#include "projectile_beam.hpp"

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

class RocketTrailImage final : public sim::SimContainer {
public:
    RocketTrailImage(sim::SimAddress owner, std::shared_ptr<SimpleSwooshTrail> trail)
        : owner_(owner), trail_(std::move(trail)) {}

    bool onAdd() override {
        if (!sim::SimContainer::onAdd()) {
            return false;
        }
        addToSet(sim::RenderSetId);
        if (auto* root =
                dynamic_cast<sim::SimContainer*>(getManager()->findObject(sim::RootContainerId))) {
            root->addObject(*this);
        }
        return true;
    }

    bool processQuery(sim::SimQuery& query) override {
        auto* surface = dynamic_cast<world::WorldSurfaceQuery*>(&query);
        const auto* owner =
            getManager() ? dynamic_cast<const RocketDumb*>(owner_.resolve(*getManager())) : nullptr;
        if (!surface || !owner || owner->hasExploded() || owner->isWaitingForDelete() ||
            !trail_->isRendering() || !trail_->bitmap()) {
            return false;
        }
        auto mesh = projectileBeamMesh(trail_->bitmap());
        const auto points = trail_->quadVertices();
        const auto texture = trail_->textureCoords();
        mesh->bounds = Box3F{points[0], points[0]};
        for (const auto& point : points) {
            mesh->bounds.fMin.setMin(point);
            mesh->bounds.fMax.setMax(point);
        }
        constexpr std::array<std::array<std::size_t, 3>, 4> faces{
            {{0, 1, 2}, {0, 2, 3}, {2, 1, 0}, {3, 2, 0}}};
        for (const auto& face : faces) {
            world::StaticSurfaceTriangle triangle;
            for (std::size_t i = 0; i < face.size(); ++i) {
                triangle.vertices[i].position = points[face[i]];
                triangle.vertices[i].texture = texture[face[i]];
            }
            mesh->triangles.push_back(triangle);
        }
        surface->image = world::WorldSurfaceInstance{std::move(mesh), TMat3F{}};
        return true;
    }

private:
    sim::SimAddress owner_;
    std::shared_ptr<SimpleSwooshTrail> trail_;
};

} // namespace

RocketDumb::RocketDumb(Int32 in_datFileId) : Projectile(in_datFileId) {
    // In the supplied legacy source the rocket motor is stored but not enabled.
    setForceMask(0);
    setNetFlags(Ghostable);
}

RocketDumb::RocketDumb() : RocketDumb(-1) {}

bool RocketDumb::initResources(GameBaseData* io_pDat) {
    if (!Parent::initResources(io_pDat)) {
        return false;
    }
    m_pRocketData_ = dynamic_cast<RocketData*>(io_pDat);
    if (!m_pRocketData_) {
        return false;
    }
    setDrag(1.0f);
    return true;
}

void RocketDumb::loadResources(bool in_isGhost) {
    Parent::loadResources(in_isGhost);
    if (in_isGhost) {
        setDrag(1.0f);
    }
}

void RocketDumb::setSpawnState(const Point3F& pos, const Point3F& vel) noexcept {
    m_spawnPosition_ = pos;
    m_spawnVelocity_ = vel;
    m_spawnVelocityLen_ = vel.len();
    setLinearVelocity(vel);
    TMat3F trans = getTransform();
    trans.p = pos;
    trans.flags |= RMat3F::Matrix_HasTranslation;
    setPosition(trans, true);
}

void RocketDumb::quantizeServerVelocity() noexcept {
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

void RocketDumb::killProjectile() {
    Parent::killProjectile();
}

void RocketDumb::destroyProjectile() {
    m_hasExploded_ = true;
    m_timeoutExplosion_ = true;
    setMaskBits(FPExplosionMask);
}

bool RocketDumb::onAdd() {
    if (!Parent::onAdd() || !m_pRocketData_) {
        return false;
    }

    setMass(m_projectileData_->mass);
    m_accelVector_ = getLinearVelocity();
    if (!m_accelVector_.isZero()) {
        m_accelVector_.normalize();
        m_accelVector_ *= m_pRocketData_->acceleration;
    }
    addForce(TribesRocketForce, m_accelVector_);

    const UInt32 nowMs = currentTimeMs(getManager(), m_lastUpdated_);
    if (isGhost()) {
        // Legacy reconstructs the client's terminal vector from the launch packet.
        if (m_projectileData_->muzzleVelocity > 0.0f) {
            m_instTerminalVelocity_ = getLinearVelocity() * (m_projectileData_->terminalVelocity /
                                                             m_projectileData_->muzzleVelocity);
        }
        m_spawnTime_ = nowMs;
        m_lastUpdated_ = nowMs;
        m_startInterp_ = nowMs;
        m_endInterp_ = nowMs + 64u;
        m_interpFrom_ = getLinearPosition();

        Point3F newVel{};
        predictForward(nowMs, nowMs + 96u, m_interpTo_, newVel);
        setLinearVelocity(newVel);

        if (m_pRocketData_->trailType == RocketData::Swoosh) {
            m_pTrail_ = std::make_shared<SimpleSwooshTrail>(m_pRocketData_->trailLength,
                                                            m_pRocketData_->trailWidth);
            m_pTrail_->loadResources("trailAdd2.bmp", getManager());
            if (auto* dbm = resolveDataBlockManager(); dbm && dbm->resources()) {
                if (auto bitmap = dbm->resources()->load<gfx::Bitmap>("trailAdd2.bmp")) {
                    m_pTrail_->setResources(bitmap);
                }
            }
            if (getManager()) {
                auto trailObject = std::make_unique<RocketTrailImage>(address(), m_pTrail_);
                trailObject->setBoundingBox(getBoundingBox());
                if (auto* added = getManager()->addObject(std::move(trailObject))) {
                    m_trailObjectId_ = added->getId();
                }
            }
            updateTrail();
        }
    } else {
        m_lastGhosted_ = nowMs;
        m_spawnPosition_ = getLinearPosition();
        m_spawnVelocity_ = getLinearVelocity();
        m_spawnVelocityLen_ = m_spawnVelocity_.len();
    }

    m_lastPuffPosition_ = getLinearPosition();
    return true;
}

void RocketDumb::onRemove() {
    if (getManager() && m_trailObjectId_) {
        if (auto* object = getManager()->findObject(m_trailObjectId_)) {
            object->deleteObject();
        }
    }
    m_trailObjectId_ = 0;
    m_pTrail_.reset();
    Parent::onRemove();
}

void RocketDumb::updateTrail() {
    if (!m_pTrail_) {
        return;
    }
    TMat3F transform = getTransform();
    if (!getLinearVelocity().isZero()) {
        if (const auto facing = world::faceDirection(getLinearVelocity())) {
            transform = *facing;
            transform.p = getLinearPosition();
            transform.flags |= RMat3F::Matrix_HasTranslation;
        }
    }
    m_pTrail_->updateTransform(transform, static_cast<Flt32>(m_lastUpdated_) * 0.001f);
    if (getManager() && m_trailObjectId_) {
        if (auto* object =
                dynamic_cast<sim::SimContainer*>(getManager()->findObject(m_trailObjectId_))) {
            Box3F bounds{m_pTrail_->centerPoint(), m_pTrail_->centerPoint()};
            for (const auto& point : m_pTrail_->quadVertices()) {
                bounds.fMin.setMin(point);
                bounds.fMax.setMax(point);
            }
            object->setBoundingBox(bounds);
        }
    }
}

void RocketDumb::buildContainerBox(const TMat3F& in_rMat, Box3F* out_pBox) {
    out_pBox->fMin = in_rMat.p;
    out_pBox->fMax = in_rMat.p;
    if (m_projectileData_) {
        const Point3F rad{m_projectileData_->collisionRadius,
                          m_projectileData_->collisionRadius,
                          m_projectileData_->collisionRadius};
        out_pBox->fMin -= rad;
        out_pBox->fMax += rad;
    }
}

Box3F RocketDumb::buildContainerBox(const TMat3F& in_rMat) const {
    Box3F box{in_rMat.p, in_rMat.p};
    if (m_projectileData_) {
        const Point3F rad{m_projectileData_->collisionRadius,
                          m_projectileData_->collisionRadius,
                          m_projectileData_->collisionRadius};
        box.fMin -= rad;
        box.fMax += rad;
    }
    return box;
}

void RocketDumb::writeInitialPacket(sim::network::GhostManager* gm, BitStream& pStream) const {
    Parent::writeInitialPacket(gm, pStream);
    const Point3F position = getLinearPosition();
    const Point3F velocity = getLinearVelocity();
    writePod(pStream, position);
    writePod(pStream, velocity);
}

bool RocketDumb::readInitialPacket(sim::network::GhostManager* gm, BitStream& pStream) {
    if (!Parent::readInitialPacket(gm, pStream)) {
        return false;
    }

    Point3F position{};
    Point3F velocity{};
    if (!readPod(pStream, position) || !readPod(pStream, velocity)) {
        return false;
    }

    m_spawnPosition_ = position;
    m_spawnVelocity_ = velocity;
    m_spawnVelocityLen_ = velocity.len();
    setLinearVelocity(velocity);

    TMat3F transform = getTransform();
    transform.p = position;
    transform.flags |= RMat3F::Matrix_HasTranslation;
    setPosition(transform, true);
    return true;
}

UInt32
RocketDumb::packUpdate(sim::network::GhostManager& gm, UInt32 mask, BitStream& stream) const {
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
        const Point3F position = getLinearPosition();
        writePod(stream, position);
    } else {
        stream.writeFlag(false);
    }
    stream.writeFlag(m_projectileIsLive_);
    return parentPunt;
}

bool RocketDumb::unpackUpdate(sim::network::GhostManager& gm, BitStream& stream) {
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
    const auto savedIsLive = m_projectileIsLive_;
    const auto savedRocketForce = getForce(TribesRocketForce);

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
        m_projectileIsLive_ = savedIsLive;
        setForceMask(savedIsLive ? 0u : static_cast<UInt32>(sim::MovementGravityForce));
        if (savedRocketForce.has_value()) {
            addForce(TribesRocketForce, *savedRocketForce, true);
        } else {
            removeForce(TribesRocketForce);
        }
        return false;
    };

    if (!Parent::unpackUpdate(gm, stream)) {
        return rollback();
    }

    if (stream.readFlag()) {
        if (!readInitialPacket(&gm, stream)) {
            return rollback();
        }
    } else {
        if (stream.readFlag()) {
            Point3F position{};
            if (!readPod(stream, position)) {
                return rollback();
            }
            TMat3F transform = getTransform();
            transform.p = position;
            transform.flags |= RMat3F::Matrix_HasTranslation;
            setPosition(transform, true);
        }
        m_projectileIsLive_ = stream.readFlag();
        if (!stream.isValid()) {
            return rollback();
        }
        if (!m_projectileIsLive_) {
            setForceMask(sim::MovementGravityForce);
            if (getForce(TribesRocketForce).has_value()) {
                removeForce(TribesRocketForce);
            }
        } else {
            setForceMask(0);
            addForce(TribesRocketForce, m_accelVector_);
        }
    }

    if (!stream.isValid()) {
        return rollback();
    }
    commitImpactEffect(true);
    guard.committed = true;
    return true;
}

void RocketDumb::updateMotor() {
    if (!m_projectileIsLive_) {
        return;
    }
    const Point3F velocity = getLinearVelocity();
    if (std::fabs(velocity.x) >= std::fabs(m_instTerminalVelocity_.x) &&
        std::fabs(velocity.y) >= std::fabs(m_instTerminalVelocity_.y) &&
        std::fabs(velocity.z) >= std::fabs(m_instTerminalVelocity_.z)) {
        removeForce(TribesRocketForce);
        setLinearVelocity(m_instTerminalVelocity_);
    } else {
        addForce(TribesRocketForce, m_accelVector_);
    }
}

void RocketDumb::predictForward(UInt32 in_currTime,
                                UInt32 in_predictTime,
                                Point3F& out_rPosition,
                                Point3F& out_rVelocity) {
    const TMat3F savedTransform = getTransform();
    const Point3F savedVelocity = getLinearVelocity();
    const Flt32 savedSlice = getTimeSlice();
    const auto savedForce = getForce(TribesRocketForce);
    out_rPosition = savedTransform.p;
    out_rVelocity = savedVelocity;
    if (in_predictTime > in_currTime) {
        updateMotor();
        setTimeSlice(static_cast<Flt32>(in_predictTime - in_currTime) * 0.001f);
        out_rPosition = updateMovement().p;
        out_rVelocity = getLinearVelocity();
    }
    setTimeSlice(savedSlice);
    setLinearVelocity(savedVelocity);
    if (savedForce) {
        addForce(TribesRocketForce, *savedForce, true);
    } else {
        removeForce(TribesRocketForce);
    }
}

void RocketDumb::clientProcess(UInt32 in_currTime) {
    if (in_currTime <= m_lastUpdated_ || m_hasExploded_) {
        return;
    }
    if (m_translucentShape_ && m_translucentThread_) {
        m_translucentShape_->advanceThread(
            *m_translucentThread_, static_cast<Flt64>(in_currTime - m_lastUpdated_) * 0.001);
    }

    sim::SimCollisionInfo collision{};
    bool collided = false;
    if (in_currTime <= m_endInterp_ && m_endInterp_ > m_startInterp_) {
        const Flt32 factor = static_cast<Flt32>(in_currTime - m_startInterp_) /
                             static_cast<Flt32>(m_endInterp_ - m_startInterp_);
        Point3F position{};
        position.interpolate(m_interpFrom_, m_interpTo_, factor);
        sim::SimContainerQuery query{};
        query.type = ~UInt32{0};
        query.mask = csm_collisionMask;
        query.detail = -1;
        query.id = (!m_projectileData_->collideWithOwner || m_ownerGraceMS_ > 0)
                       ? getOwnerId()
                       : static_cast<sim::SimObjectId>(-1);
        query.box = Box3F{getLinearPosition(), position};
        auto* root =
            getManager()
                ? dynamic_cast<sim::SimContainer*>(getManager()->findObject(sim::RootContainerId))
                : nullptr;
        collided = root && root->findLOS(query, collision, sim::SimCollisionImageQuery::High);
        decayGraceTime(in_currTime - m_lastUpdated_);
        if (!collided) {
            TMat3F transform = getTransform();
            transform.p = position;
            transform.flags |= RMat3F::Matrix_HasTranslation;
            setPosition(transform, true);
        }
        m_lastUpdated_ = in_currTime;
    } else {
        updateMotor();
        sim::SimObject* hitObject = nullptr;
        collided = moveProjectile(in_currTime, collision, hitObject, true, true);
        if (collided && !isProjStatic(hitObject)) {
            // Dynamic impacts are resolved by the authoritative explosion packet.
            m_lastUpdated_ = in_currTime;
            collided = false;
        }
    }
    if (collided && getManager() && !collision.surfaces.surfaces.empty()) {
        collision.transform(collision.surfaces.tWorld);
        m_explosionPosition_ = collision.surfaces.surfaces[0].position;
        m_explosionAxis_ = collision.surfaces.surfaces[0].normal;
        m_explosionId_ = getExplosionTag(collision);
        auto explosion = std::make_unique<Explosion>(m_explosionId_);
        explosion->bindDataBlockManager(resolveDataBlockManager());
        explosion->setPosition(m_explosionPosition_);
        explosion->setAxis(m_explosionAxis_);
        getManager()->addObject(std::move(explosion));
        if (auto* smoke = SmokeManager::get(getManager())) {
            smoke->removeAllInRadius(m_explosionPosition_, m_projectileData_->explosionRadius);
        }
        m_hasExploded_ = true;
        m_overrideRender_ = true;
        stopProjectileSound();
        return;
    }

    if (m_pRocketData_ && getManager() && m_pRocketData_->trailType == RocketData::Smoke &&
        !m_pRocketData_->trailString.empty() && m_pRocketData_->projSpecialTime > 0.0f) {
        const Point3F delta = getLinearPosition() - m_lastPuffPosition_;
        const Flt32 length = delta.len();
        const Int32 count = static_cast<Int32>(length / m_pRocketData_->projSpecialTime);
        if (count > 0) {
            if (auto* smoke = SmokeManager::get(getManager())) {
                const Point3F step = delta * (m_pRocketData_->projSpecialTime / length);
                for (Int32 i = 0; i < count; ++i) {
                    m_lastPuffPosition_ += step;
                    smoke->addPuff(m_pRocketData_->trailString,
                                   0.4f,
                                   m_lastPuffPosition_,
                                   getLinearVelocity() * 0.25f);
                }
            }
        }
    }
    updateTrail();
    updateProjectileSound(getTransform(), getLinearVelocity());
}

void RocketDumb::serverProcess(UInt32 in_currTime) {
    if (in_currTime < m_lastUpdated_) {
        return;
    }
    updateMotor();
    Parent::serverProcess(in_currTime);
}

void RocketDumb::spawnImpactEffect(sim::SimCollisionInfo& io_rCollision) {
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

    // A client may receive the projectile after its static collision prediction
    // point. Always publish the authoritative impact, including terrain hits.
    setMaskBits(FPExplosionMask);
}

bool RocketDumb::readExplosion(BitStream& io_pStream) {
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

    m_pendingImpactEffect_ = !m_hasExploded_;
    return true;
}

void RocketDumb::writeExplosion(BitStream& io_pStream) const {
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

} // namespace opent1::game
