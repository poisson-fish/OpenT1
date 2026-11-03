#include <opent1/game/player.hpp>
#include <ranges>
#include <opent1/game/projectile.hpp>
#include <opent1/game/sound.hpp>
#include <opent1/game/smoke_puff.hpp>
#include <opent1/world/scene_objects.hpp>

#include <algorithm>
#include <cmath>
#include <numbers>

namespace opent1::game {

namespace {
constexpr Flt32 kTwoPi = static_cast<Flt32>(2.0 * std::numbers::pi);

Random g_deflectRandom{};
Random g_explosionTagRandom{};

UInt32 currentTimeMs(const sim::SimManager* mgr, UInt32 fallback = 0) noexcept {
    if (!mgr) {
        return fallback;
    }
    return static_cast<UInt32>(mgr->getCurrentTime() * 1000.0f);
}

std::optional<Int32> findSequenceByName(const world::TsShape& shape,
                                        std::string_view name) noexcept {
    for (auto i : std::views::iota(std::size_t{0}, shape.sequences.size())) {
        const Int32 nameIdx = shape.sequences[i].name;
        if (nameIdx >= 0 && static_cast<std::size_t>(nameIdx) < shape.names.size()) {
            if (shape.names[static_cast<std::size_t>(nameIdx)] == name) {
                return static_cast<Int32>(i);
            }
        }
    }
    return std::nullopt;
}

} // namespace

void Projectile::resetDeflectionSeed(UInt32 seed) noexcept {
    g_deflectRandom.setSeed(seed);
}

void Projectile::resetExplosionTagSeed(UInt32 seed) noexcept {
    g_explosionTagRandom.setSeed(seed);
}

Projectile::Projectile(Int32 in_datFileId) {
    datFileName_.clear();
    datFileId_ = in_datFileId;
    m_renderImage_.transform.identity();

    setForceMask(0);
    setMass(1.0f);
    setDrag(1.0f);
    setDensity(1.0f);

    setType(SimProjectileObjectType);
    m_shooterId_ = static_cast<sim::SimObjectId>(-1);
    m_pShooter_ = nullptr;
}

Projectile::~Projectile() {
    m_renderImage_.clearShapeInstance();
}

bool Projectile::isProjStatic(const sim::SimObject* pObj) const noexcept {
    constexpr UInt32 cs_typeMask = SimInteriorObjectType | SimTerrainObjectType | ItemObjectType |
                                   CorpseObjectType | StaticObjectType | MineObjectType |
                                   SimShapeObjectType;
    if (isGhost()) {
        return true;
    }
    return pObj && (pObj->getType() & cs_typeMask) != 0;
}

void Projectile::loadResources(bool in_isGhost) {
    if (!m_projectileData_) {
        loadDatFile();
    }
    if (!m_projectileData_ || !in_isGhost) {
        return;
    }
    if (!m_projectileData_->bulletShapeName.empty()) {
        if (m_projectileData_->shape) {
            m_bulletRes_ = m_projectileData_->shape;
        } else if (getManager()) {
            if (auto* sr = sim::SimResource::find(*getManager())) {
                m_bulletRes_ = sr->get().load<world::TsShape>(m_projectileData_->bulletShapeName);
            }
        }
        if (m_bulletRes_) {
            m_translucentShape_ = std::make_shared<world::TsShapeInstance>(m_bulletRes_);
            m_nonTranslucentShape_ = std::make_shared<world::TsShapeInstance>(m_bulletRes_);
            m_renderImage_.setShapeInstance(m_translucentShape_.get(), m_translucentShape_);

            if (const auto ambient = findSequenceByName(*m_bulletRes_, "ambient")) {
                m_translucentThread_ = m_translucentShape_->addThread(*ambient, 0.0f);
                m_nonTranslucentThread_ = m_nonTranslucentShape_->addThread(*ambient, 0.0f);
            }
        }
    }
}

bool Projectile::initResources(GameBaseData* io_pDat) {
    if (!Parent::initResources(io_pDat)) {
        return false;
    }
    m_projectileData_ = dynamic_cast<ProjectileData*>(io_pDat);
    if (!m_projectileData_) {
        return false;
    }
    setMass(std::max(0.001f, m_projectileData_->mass));
    return true;
}

bool Projectile::applyBaseDamage(sim::SimCollisionInfo& io_rCollision, sim::SimObject* io_pHit) {
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
            pShape->applyDamage(m_projectileData_->damageType,
                                m_projectileData_->damageValue,
                                pos,
                                getLinearVelocity(),
                                Point3F{0.0f, 0.0f, 0.0f},
                                static_cast<sim::SimObjectId>(getDamageId()));
            return true;
        }
    }
    return false;
}

void Projectile::applyRadiusDamage(const Point3F& io_rDetonationPoint) {
    if (isGhost() || !m_projectileData_) {
        return;
    }
    if (m_projectileData_->explosionRadius > 0.0f) {
        Explosion::applyRadiusDamage(getManager(),
                                     m_projectileData_->damageType,
                                     io_rDetonationPoint,
                                     getLinearVelocity(),
                                     m_projectileData_->explosionRadius,
                                     m_projectileData_->damageValue,
                                     m_projectileData_->kickBackStrength,
                                     getDamageId());
    }
}

void Projectile::killProjectile() {
    m_projectileIsLive_ = false;
}

void Projectile::destroyProjectile() {}

void Projectile::spawnImpactEffect(sim::SimCollisionInfo& io_rCollision) {
    if (isGhost()) {
        return;
    }
    if (!io_rCollision.surfaces.surfaces.empty()) {
        m_explosionPosition_ = io_rCollision.surfaces.surfaces[0].position;
        m_explosionAxis_ = io_rCollision.surfaces.surfaces[0].normal;
    } else {
        m_explosionPosition_ = getLinearPosition();
        m_explosionAxis_.set(0.0f, 0.0f, 1.0f);
    }
    m_explosionId_ = getExplosionTag(io_rCollision);
    m_hasExploded_ = true;
    // Prediction may not include the struck shape yet. Publish the contact
    // for every target; the client suppresses an already predicted effect.
    setMaskBits(FPExplosionMask);
}

void Projectile::spawnImpactEffect(const Point3F& in_rPosition) {
    if (isGhost()) {
        return;
    }
    m_explosionPosition_ = in_rPosition;
    m_explosionAxis_.set(0.0f, 0.0f, 1.0f);
    m_explosionId_ = getExplosionTag();
    m_hasExploded_ = true;
    setMaskBits(FPExplosionMask);
}

void Projectile::deflectProjectile(Flt32 in_deflection) {
    if (isGhost()) {
        return;
    }
    if (in_deflection != 0.0f) {
        EulerF test{};
        test.x = (g_deflectRandom.getFloat() - 0.5f) * kTwoPi * in_deflection;
        test.y = (g_deflectRandom.getFloat() - 0.5f) * kTwoPi * in_deflection;
        test.z = (g_deflectRandom.getFloat() - 0.5f) * kTwoPi * in_deflection;
        const RMat3F testMat{test};

        Point3F finalVel{};
        m_mul(getLinearVelocity(), testMat, finalVel);
        setLinearVelocity(finalVel);
    }
}

void Projectile::initProjectile(const TMat3F& in_rTrans,
                                const Point3F& in_rShooterVel,
                                sim::SimObjectId in_shooterId) {
    if (isGhost()) {
        return;
    }
    setPosition(in_rTrans, true);
    setOwnerId(in_shooterId);
    m_shooterId_ = in_shooterId;
    m_shooterVel_ = in_rShooterVel;
}

bool Projectile::onSimLightQuery(sim::SimLightQuery& query) const {
    if (m_overrideRender_ || !m_projectileData_ || m_projectileData_->lightRange == 0.0f) {
        return false;
    }
    m_light_.type = sim::SimLight::Type::Point;
    m_light_.intensity = m_projectileData_->lightColor;
    m_light_.range = m_projectileData_->lightRange;
    m_light_.position = getLinearPosition();
    query.lights.push_back(m_light_);
    return true;
}

bool Projectile::onSimCollisionImageQuery(sim::SimCollisionImageQuery& query) const {
    query.images.clear();
    return true;
}

bool Projectile::onWorldShapeQuery(world::WorldShapeQuery& query) const {
    if (m_waitingForDelete_ || m_overrideRender_ || m_hasExploded_) {
        return false;
    }
    if (!m_bulletRes_) {
        return false;
    }
    if (!m_cachedWorldResources_) {
        auto* dbm = resolveDataBlockManager();
        if (!dbm || !dbm->resources()) {
            return false;
        }
        world::TsShapeResources resources;
        resources.shape = m_bulletRes_;
        if (resources.shape->materials) {
            auto materials =
                world::TsMaterialResources::load(*dbm->resources(), *resources.shape->materials);
            if (!materials) {
                return false;
            }
            resources.materials = std::move(*materials);
        }
        m_cachedWorldResources_ = std::move(resources);
        m_cachedWorldInstance_ = m_translucentShape_ ? m_translucentShape_ : m_nonTranslucentShape_;
        if (!m_cachedWorldInstance_) {
            m_cachedWorldInstance_ = std::make_shared<world::TsShapeInstance>(m_bulletRes_);
        }
    }

    const auto poseOpt = m_cachedWorldInstance_->pose();
    if (!poseOpt) {
        return false;
    }

    TMat3F xform = getTransform();
    Point3F direction = m_renderFacingCamera_ && query.cameraPosition
                            ? xform.p - *query.cameraPosition
                            : getLinearVelocity();
    if ((m_renderFacingCamera_ || m_faceForward_) && direction.len() > 0.0001f) {
        direction.normalize();
        if (auto facing = world::faceDirection(direction, std::nullopt, m_axisSpin_)) {
            facing->p = xform.p;
            xform = *facing;
        }
    }
    xform.flags = RMat3F::Matrix_HasRotation | RMat3F::Matrix_HasTranslation;

    world::WorldShapeImage img{};
    img.resources = *m_cachedWorldResources_;
    img.pose = *poseOpt;
    const auto& root = m_bulletRes_->transforms[m_bulletRes_->nodes[0].defaultTransform];
    m_mul(TMat3F(root.rotation, root.translation), xform, img.shapeToWorld);
    img.detail = 0;
    img.automaticDetail = true;
    img.haze = query.cameraPosition && getContainer() ? getHaze(*query.cameraPosition) : 0.f;
    query.image = std::move(img);
    return true;
}

bool Projectile::processQuery(sim::SimQuery& query) {
    if (auto* lq = dynamic_cast<sim::SimLightQuery*>(&query)) {
        return onSimLightQuery(*lq);
    }
    if (auto* ciq = dynamic_cast<sim::SimCollisionImageQuery*>(&query)) {
        return onSimCollisionImageQuery(*ciq);
    }
    if (auto* wsq = dynamic_cast<world::WorldShapeQuery*>(&query)) {
        return onWorldShapeQuery(*wsq);
    }
    return Parent::processQuery(query);
}

bool Projectile::onAdd() {
    if (!Parent::onAdd()) {
        return false;
    }
    if (!m_projectileData_) {
        loadDatFile();
    }
    if (!m_projectileData_) {
        return false;
    }

    loadResources(isGhost());
    addToSet(PlayerSetId);
    (void)addToSet(sim::RenderSetId);

    const UInt32 nowMs = currentTimeMs(getManager(), m_lastUpdated_);

    if (!isGhost()) {
        if (getManager() && m_shooterId_ != static_cast<sim::SimObjectId>(-1)) {
            if (auto* pBase = getManager()->findObject(m_shooterId_)) {
                m_pShooter_ = dynamic_cast<ShapeBase*>(pBase);
                if (m_pShooter_) {
                    deleteNotify(*m_pShooter_);
                    m_shooterClientId_ = m_pShooter_->getControlClient();
                    if (!m_shooterClientId_) {
                        m_shooterClientId_ = m_pShooter_->getLastOwnerClient();
                    }
                    if (m_shooterClientId_ != 0) {
                        if (auto* pPM = PlayerManager::get(getManager())) {
                            if (auto* pCR = pPM->findBaseRep(m_shooterClientId_)) {
                                m_shooterClientLoginId_ = pCR->loginId;
                            } else {
                                m_shooterClientLoginId_ = 0xFFFFFFFFu;
                            }
                        }
                    } else {
                        m_shooterClientLoginId_ = 0xFFFFFFFFu;
                    }
                }
            }
        }
        m_shooterId_ = static_cast<sim::SimObjectId>(-1);

        Point3F velocityVector{};
        getTransform().getRow(1, velocityVector);
        velocityVector *= m_projectileData_->muzzleVelocity;
        velocityVector += m_shooterVel_ * m_projectileData_->inheritedVelocityScale;
        setLinearVelocity(velocityVector);

        deflectProjectile(m_projectileData_->aimDeflection);

        getTransform().getRow(1, m_instTerminalVelocity_);
        m_instTerminalVelocity_ *= m_projectileData_->terminalVelocity;
    }

    if (getManager() && !getContainer()) {
        if (auto* pRoot =
                dynamic_cast<sim::SimContainer*>(getManager()->findObject(sim::RootContainerId))) {
            pRoot->addObject(*this);
        }
    }

    if (isGhost()) {
        if (m_pShooter_) {
            deleteNotify(*m_pShooter_);
            setOwnerId(m_pShooter_->getId());
        }
        if (m_projectileData_->lightRange != 0.0f) {
            m_light_.type = sim::SimLight::Type::Point;
            m_light_.intensity = m_projectileData_->lightColor;
            m_light_.range = m_projectileData_->lightRange;
            addToSet(sim::LightSetId);
        }
    }

    setCollisionMask(csm_collisionMask);
    if (!m_projectileData_->collideWithOwner || m_projectileData_->ownerGraceMS > 0) {
        setExcludedId(getOwnerId());
    } else {
        setExcludedId(0);
    }

    setTimeSlice(1.0f / 30.0f);

    m_lastUpdated_ = nowMs;
    m_ownerGraceMS_ = static_cast<Int32>(m_projectileData_->ownerGraceMS);
    m_totalTime_ = static_cast<UInt32>(m_projectileData_->totalTime * 1000.0f) + nowMs;
    m_liveTime_ = static_cast<UInt32>(m_projectileData_->liveTime * 1000.0f) + nowMs;
    commitImpactEffect(false);
    return true;
}

void Projectile::onRemove() {
    stopProjectileSound();
    if (auto* cnt = getContainer()) {
        cnt->removeObject(*this);
    }
    Parent::onRemove();
}

Box3F Projectile::buildContainerBox(const TMat3F& in_rMat) const {
    return Parent::buildContainerBox(in_rMat);
}

void Projectile::decayGraceTime(UInt32 in_decayStep) noexcept {
    if (m_ownerGraceMS_ > 0) {
        m_ownerGraceMS_ -= static_cast<Int32>(in_decayStep);
        if (m_ownerGraceMS_ <= 0) {
            m_ownerGraceMS_ = 0;
            setExcludedId(0);
        }
    }
}

bool Projectile::moveProjectile(UInt32 in_currTime,
                                sim::SimCollisionInfo& out_rCollision,
                                sim::SimObject*& out_pObject,
                                bool in_doCollisions,
                                bool in_overrideTimeSlice) {
    if (!m_projectileData_) {
        return false;
    }

    Int32 intTimeSlice = 0;
    Flt32 floatTimeSlice = 0.0f;
    if (in_overrideTimeSlice) {
        intTimeSlice = static_cast<Int32>(in_currTime - m_lastUpdated_);
        if (intTimeSlice <= 0) {
            return false;
        }
        floatTimeSlice = static_cast<Flt32>(intTimeSlice) * 0.001f;
        setTimeSlice(floatTimeSlice);
    } else {
        floatTimeSlice = getTimeSlice();
        intTimeSlice = static_cast<Int32>(floatTimeSlice * 1000.0f);
        if (intTimeSlice <= 0) {
            intTimeSlice = 1;
        }
    }

    sim::SimContainer* root = nullptr;
    if (getManager()) {
        root = dynamic_cast<sim::SimContainer*>(getManager()->findObject(sim::RootContainerId));
    }

    if (m_projectileData_->collisionRadius == 0.0f) {
        sim::SimContainerQuery collisionQuery{};
        collisionQuery.id = static_cast<sim::SimObjectId>(-1);
        collisionQuery.type = ~UInt32{0};
        collisionQuery.mask = csm_collisionMask;
        collisionQuery.detail = sim::SimCollisionImageQuery::Normal;

        while (m_lastUpdated_ + static_cast<UInt32>(intTimeSlice) <= in_currTime) {
            TMat3F finalPosition = updateMovement();
            decayGraceTime(static_cast<UInt32>(intTimeSlice));

            if (in_doCollisions && root) {
                if (!m_projectileData_->collideWithOwner || m_ownerGraceMS_ > 0) {
                    collisionQuery.id = getOwnerId();
                } else {
                    collisionQuery.id = static_cast<sim::SimObjectId>(-1);
                }
                collisionQuery.box.fMin = getTransform().p;
                collisionQuery.box.fMax = finalPosition.p;

                if (root->findLOS(
                        collisionQuery, out_rCollision, sim::SimCollisionImageQuery::High)) {
                    out_pObject =
                        getManager() ? out_rCollision.object.resolve(*getManager()) : nullptr;
                    return true;
                }
            }

            setPosition(finalPosition, true);
            m_lastUpdated_ += static_cast<UInt32>(intTimeSlice);
        }
    } else {
        while (m_lastUpdated_ + static_cast<UInt32>(intTimeSlice) <= in_currTime) {
            TMat3F finalPosition = updateMovement();
            decayGraceTime(static_cast<UInt32>(intTimeSlice));

            if (in_doCollisions) {
                m_testCollision_ = false;
                stepPosition(finalPosition, 0.1f, 0.3f);
                if (m_testCollision_) {
                    out_pObject = nullptr;
                    return true;
                }
            } else {
                setPosition(finalPosition, true);
            }

            m_lastUpdated_ += static_cast<UInt32>(intTimeSlice);
        }
    }

    return false;
}

bool Projectile::processCollision(MovementInfo& info) {
    const bool pReturn = Parent::processCollision(info);
    if (pReturn) {
        m_testCollision_ = true;
    }
    return pReturn;
}

void Projectile::clientProcess(UInt32 in_currTime) {
    if (m_translucentShape_ && m_translucentThread_ && in_currTime > m_lastUpdated_) {
        const Flt32 deltaT = static_cast<Flt32>(in_currTime - m_lastUpdated_) / 1000.0f;
        m_translucentShape_->advanceThread(*m_translucentThread_, static_cast<Flt64>(deltaT));
    }
    m_lastUpdated_ = in_currTime;
}

void Projectile::serverProcess(UInt32 in_currTime) {
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

    sim::SimCollisionInfo collision{};
    sim::SimObject* pObject = nullptr;
    const bool collided = moveProjectile(in_currTime, collision, pObject, true, false);

    if (collided && m_projectileData_) {
        if (m_projectileData_->damageClass == 0) {
            applyBaseDamage(collision, pObject);
            if (!collision.surfaces.surfaces.empty()) {
                collision.surfaces.surfaces[0].transform(collision.surfaces.tWorld);
            }
            spawnImpactEffect(collision);
        } else if (m_projectileData_->damageClass == 1) {
            // A ray collision leaves the movement transform at the start of
            // its step. Use the actual contact for both damage and the effect.
            // Sphere movement already updates its transform during collision.
            Point3F impact = getLinearPosition();
            if (!collision.surfaces.surfaces.empty()) {
                collision.surfaces.surfaces[0].transform(collision.surfaces.tWorld);
                impact = collision.surfaces.surfaces[0].position;
                TMat3F transform = getTransform();
                transform.p = impact;
                transform.flags |= RMat3F::Matrix_HasTranslation;
                setPosition(transform, true);
                applyRadiusDamage(impact);
                spawnImpactEffect(collision);
            } else {
                applyRadiusDamage(impact);
                spawnImpactEffect(impact);
            }
        }

        m_waitingForDelete_ = true;
        m_deleteTime_ = in_currTime + 500;
    }
}

Int32 Projectile::shooterGhostIndex(const sim::network::GhostManager* gm) const noexcept {
    if (m_explicitShooterGhostIndex_ != -1) {
        return m_explicitShooterGhostIndex_;
    }
    if (m_pShooter_ && gm) {
        const auto index = gm->getGhostIndex(*m_pShooter_);
        return index == sim::InvalidGhostIndex ? -1 : static_cast<Int32>(index);
    }
    return -1;
}

bool Projectile::readInitialPacket(sim::network::GhostManager* io_pGM, BitStream& io_pStream) {
    if (!unpackDatFile(io_pStream)) {
        return false;
    }

    if (io_pStream.readFlag()) {
        const UInt32 tempId = static_cast<UInt32>(io_pStream.readInt(10));
        if (!io_pStream.isValid()) {
            return false;
        }
        m_explicitShooterGhostIndex_ = static_cast<Int32>(tempId);
        if (io_pGM) {
            if (auto* pShooter = io_pGM->resolveGhost(static_cast<UInt16>(tempId))) {
                m_pShooter_ = dynamic_cast<ShapeBase*>(pShooter);
                if (m_pShooter_) {
                    deleteNotify(*m_pShooter_);
                    setOwnerId(m_pShooter_->getId());
                }
                return true;
            }
        }
    } else {
        m_explicitShooterGhostIndex_ = -1;
    }
    if (!io_pStream.isValid()) {
        return false;
    }
    setOwnerId(static_cast<sim::SimObjectId>(-1));
    return true;
}

void Projectile::writeInitialPacket(sim::network::GhostManager* io_pGM,
                                    BitStream& io_pStream) const {
    packDatFile(io_pStream);

    const Int32 ownerGhostId = shooterGhostIndex(io_pGM);
    if (ownerGhostId != -1) {
        io_pStream.writeFlag(true);
        io_pStream.writeInt(ownerGhostId, 10);
    } else {
        io_pStream.writeFlag(false);
    }
}

Int32 Projectile::getBulletHoleIndex(
    const sim::SimCollisionInfo& /*io_rCollision*/) const noexcept {
    return -1;
}

Int32 Projectile::getExplosionTag(const sim::SimCollisionInfo& /*io_rCollision*/) const noexcept {
    return getExplosionTag();
}

Int32 Projectile::getExplosionTag() const noexcept {
    if (!m_projectileData_) {
        return -1;
    }
    if (m_projectileData_->expRandCycle <= 1) {
        return m_projectileData_->explosionTag;
    }
    const Flt32 floatRand =
        g_explosionTagRandom.getFloat() * static_cast<Flt32>(m_projectileData_->expRandCycle);
    const Int32 intRand = static_cast<Int32>(floatRand);
    return m_projectileData_->explosionTag + intRand;
}

Flt32 Projectile::getInheritedVelScale() const noexcept {
    return m_projectileData_ ? m_projectileData_->inheritedVelocityScale : 0.0f;
}

UInt32
Projectile::packUpdate(sim::network::GhostManager& gm, UInt32 mask, BitStream& stream) const {
    UInt32 parentPunt = Parent::packUpdate(gm, mask, stream);

    if ((mask & InitialUpdate) == 0) {
        stream.writeFlag(false);
        if (m_hasExploded_) {
            stream.writeFlag(true);
            writeExplosion(stream);
        } else {
            stream.writeFlag(false);
        }
    } else {
        stream.writeFlag(true);
        if (m_hasExploded_) {
            parentPunt |= (mask & FPExplosionMask);
        }
    }

    return parentPunt;
}

bool Projectile::unpackUpdate(sim::network::GhostManager& gm, BitStream& stream) {
    if (!Parent::unpackUpdate(gm, stream)) {
        return false;
    }

    const bool isInitial = stream.readFlag();
    if (!stream.isValid()) {
        return false;
    }
    if (!isInitial) {
        const bool hasExp = stream.readFlag();
        if (!stream.isValid()) {
            return false;
        }
        if (hasExp) {
            if (!readExplosion(stream)) {
                return false;
            }
        }
    }
    return stream.isValid();
}

void Projectile::setEnergy(Flt32, Flt32) {}

bool Projectile::isSustained() const noexcept {
    return false;
}

void Projectile::shutOffProjectile() {}

void Projectile::updateImageTransform(const TMat3F&) {}

bool Projectile::isTargetable() const noexcept {
    return false;
}

void Projectile::setTarget(ShapeBase* /*io_pTarget*/) {}

Flt32 Projectile::getTargetRange() const noexcept {
    return 0.0f;
}

void Projectile::updateProjectileSound(const TMat3F& transform, const Point3F& velocity) {
    if (!isGhost() || m_hasExploded_ || m_waitingForDelete_ || !m_projectileData_ ||
        m_projectileData_->soundId < 0 || !getManager())
        return;
    if (!m_projectileSound_) {
        m_projectileSound_ = TSFX::PlayAt(
            static_cast<UInt32>(m_projectileData_->soundId), transform, velocity, getManager());
    } else if (auto* playback =
                   dynamic_cast<SoundPlayback*>(getManager()->findObject("GameSoundPlayback"));
               playback && playback->move) {
        playback->move(m_projectileSound_, transform, velocity);
    }
}

void Projectile::commitImpactEffect(bool clearSmoke) {
    if (!m_pendingImpactEffect_) {
        return;
    }
    if (!getManager()) {
        return;
    }
    m_pendingImpactEffect_ = false;
    if (m_hasExploded_) {
        return;
    }
    m_hasExploded_ = true;
    m_overrideRender_ = true;
    stopProjectileSound();
    if (clearSmoke) {
        if (auto* smoke = SmokeManager::get(getManager())) {
            smoke->removeAllInRadius(m_explosionPosition_,
                                     m_projectileData_ ? m_projectileData_->explosionRadius : 5.0f);
        }
    }
    auto explosion = std::make_unique<Explosion>(m_explosionId_);
    explosion->bindDataBlockManager(resolveDataBlockManager());
    explosion->setPosition(m_explosionPosition_);
    explosion->setAxis(m_explosionAxis_);
    getManager()->addObject(std::move(explosion));
}

void Projectile::stopProjectileSound() {
    if (m_projectileSound_ && getManager()) {
        if (auto* playback =
                dynamic_cast<SoundPlayback*>(getManager()->findObject("GameSoundPlayback"));
            playback && playback->stop) {
            playback->stop(m_projectileSound_);
        }
    }
    m_projectileSound_ = 0;
}

void Projectile::onDeleteNotify(sim::SimObject& io_pDelete) {
    if (&io_pDelete == m_pShooter_) {
        m_pShooter_ = nullptr;
    }
    Parent::onDeleteNotify(io_pDelete);
}

Int32 Projectile::getDamageId() const noexcept {
    if (m_shooterClientId_ == 0 || !getManager()) {
        return 0;
    }
    if (auto* pPM = PlayerManager::get(getManager())) {
        if (const auto* pCR = pPM->findClient(m_shooterClientId_)) {
            if (pCR->loginId == m_shooterClientLoginId_) {
                return m_shooterClientId_;
            }
        }
    }
    return 0;
}

} // namespace opent1::game
