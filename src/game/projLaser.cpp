#include <opent1/game/player.hpp>
#include <opent1/game/proj_laser.hpp>
#include <opent1/game/sound.hpp>
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

class LaserHitShape final : public world::SimTsShape {
public:
    bool visible{true};

    bool processQuery(sim::SimQuery& query) override {
        auto* shapeQuery = dynamic_cast<world::WorldShapeQuery*>(&query);
        if (shapeQuery && !visible) {
            return false;
        }
        if (!SimTsShape::processQuery(query)) {
            return false;
        }
        if (shapeQuery && shapeQuery->image && shapeQuery->cameraPosition) {
            Point3F direction = getTransform().p - *shapeQuery->cameraPosition;
            if (direction.len() > .00001f) {
                direction.normalize();
                if (auto facing = world::faceDirection(direction)) {
                    facing->p = getTransform().p;
                    facing->flags |= RMat3F::Matrix_HasTranslation;
                    shapeQuery->image->shapeToWorld = *facing;
                }
            }
        }
        return true;
    }
};

} // namespace

LaserProjectile::LaserProjectile(Int32 in_datFileId) : Projectile(in_datFileId) {
    setNetFlags(Ghostable | ScopeAlways);
}

LaserProjectile::LaserProjectile() : LaserProjectile(-1) {}

bool LaserProjectile::initResources(GameBaseData* io_pDat) {
    if (!Parent::initResources(io_pDat)) {
        return false;
    }
    m_pLaserData_ = dynamic_cast<LaserData*>(io_pDat);
    return m_pLaserData_ != nullptr;
}

void LaserProjectile::loadResources(bool in_isGhost) {
    Parent::loadResources(in_isGhost);
    if (!in_isGhost || !m_pLaserData_) {
        return;
    }
    auto* dbm = resolveDataBlockManager();
    auto* resources = dbm ? dbm->resources() : nullptr;
    if (!resources && getManager()) {
        if (auto* service = sim::SimResource::find(*getManager())) {
            resources = &service->get();
        }
    }
    if (!resources) {
        return;
    }
    m_beamBitmap_ = resources->load<gfx::Bitmap>(m_pLaserData_->pBitmapName);
    if (!m_pLaserData_->pHitShapeName.empty()) {
        m_hitResources_ = world::TsShapeResources::load(*resources, m_pLaserData_->pHitShapeName);
    }
}

void LaserProjectile::initProjectile(const TMat3F& in_rTrans,
                                     const Point3F& /*in_rShooterVel*/,
                                     sim::SimObjectId in_shooterId) {
    m_finalDamage_ = 0.0f;
    setPosition(in_rTrans, true);
    setOwnerId(in_shooterId);
    m_shooterId_ = in_shooterId;
    m_shooterVel_.set(0.0f, 0.0f, 0.0f);
    m_startPoint_ = in_rTrans.p;
}

void LaserProjectile::setEnergy(Flt32 in_energy, Flt32 in_pOfM) {
    m_finalDamage_ = in_energy * in_pOfM;
    if (m_finalDamage_ > 55.0f) {
        m_startIntensity_ = 1.0f;
    } else {
        m_startIntensity_ = m_finalDamage_ / 55.0f;
    }
}

bool LaserProjectile::onAdd() {
    if (!Parent::onAdd() || !m_pLaserData_) {
        return false;
    }

    const UInt32 nowMs = currentTimeMs(getManager(), m_lastUpdated_);
    m_spawnTime_ = nowMs;
    if (isGhost()) {
        if (m_pShooter_) {
            TMat3F muzzleTrans{};
            if (m_pShooter_->getMuzzleTransform(0, &muzzleTrans)) {
                if (dynamic_cast<Player*>(m_pShooter_) != nullptr) {
                    TMat3F finalTrans{};
                    m_mul(muzzleTrans, m_pShooter_->getTransform(), finalTrans);
                    m_startPoint_ = finalTrans.p;
                } else {
                    m_startPoint_ = muzzleTrans.p;
                }
            }
        }
        if (m_projectileData_ && m_projectileData_->lightRange != 0.0f) {
            m_endLight_.type = sim::SimLight::Type::Point;
            m_endLight_.intensity = m_projectileData_->lightColor;
            m_endLight_.range = m_projectileData_->lightRange;
        }
        if (m_hitResources_ && getManager()) {
            auto image = std::make_unique<LaserHitShape>();
            if (image->setResources(*m_hitResources_)) {
                if (auto* instance = image->instance()) {
                    const auto& shape = *m_hitResources_->shape;
                    for (std::size_t i = 0; i < shape.sequences.size(); ++i) {
                        const auto& sequence = shape.sequences[i];
                        if (sequence.name >= 0 &&
                            static_cast<std::size_t>(sequence.name) < shape.names.size() &&
                            shape.names[static_cast<std::size_t>(sequence.name)] == "ambient") {
                            if (auto thread = instance->addThread(static_cast<Int32>(i));
                                thread && m_projectileData_->liveTime > 0) {
                                instance->setTimeScale(
                                    *thread, sequence.duration / m_projectileData_->liveTime);
                            }
                            break;
                        }
                    }
                }
                if (auto* added = getManager()->addObject(std::move(image))) {
                    m_hitImage_ = added->address();
                }
            }
        }
        updateHitImage();
        if (m_hitSomething_ && m_projectileData_->soundId >= 0) {
            TMat3F transform;
            transform.p = m_endPoint_;
            transform.flags |= RMat3F::Matrix_HasTranslation;
            (void)TSFX::PlayAt(UInt32(m_projectileData_->soundId), transform, {}, getManager());
        }
    } else {
        m_startPoint_ = getTransform().p;

        Point3F beamDir{};
        getTransform().getRow(1, beamDir);
        beamDir *= projectileBeamRange(getManager());
        m_endPoint_ = m_startPoint_ + beamDir;

        TMat3F newTrans = getTransform();
        newTrans.p = m_endPoint_;
        newTrans.flags |= RMat3F::Matrix_HasTranslation;

        sim::SimCollisionInfo collisionInfo{};
        sim::SimContainerQuery query{};
        query.id = getOwnerId();
        query.type = ~UInt32{0};
        query.mask = csm_collisionMask;
        query.detail = -1;
        query.box.fMin = m_startPoint_;
        query.box.fMax = m_endPoint_;

        sim::SimContainer* root =
            getManager()
                ? dynamic_cast<sim::SimContainer*>(getManager()->findObject(sim::RootContainerId))
                : nullptr;
        if (root && root->findLOS(query, collisionInfo, sim::SimCollisionImageQuery::Normal) &&
            !collisionInfo.surfaces.surfaces.empty()) {
            sim::SimObject* pObject = collisionInfo.object.resolve(*getManager());
            if (pObject && (pObject->getType() & csm_damageMask) != 0) {
                if (auto* pShape = dynamic_cast<ShapeBase*>(pObject)) {
                    Point3F pos{};
                    m_mul(collisionInfo.surfaces.surfaces[0].position,
                          collisionInfo.surfaces.tWorld,
                          pos);
                    const Flt32 dmg = m_finalDamage_ * m_projectileData_->damageValue;
                    if (dmg != 0.0f) {
                        pShape->applyDamage(LaserDamageType,
                                            dmg,
                                            pos,
                                            pos - m_startPoint_,
                                            Point3F{0.0f, 0.0f, 0.0f},
                                            static_cast<sim::SimObjectId>(getDamageId()));
                    }
                }
            }
            m_mul(collisionInfo.surfaces.surfaces[0].position,
                  collisionInfo.surfaces.tWorld,
                  m_endPoint_);
            newTrans.p = m_endPoint_;
            newTrans.flags |= RMat3F::Matrix_HasTranslation;
            m_hitSomething_ = true;
        } else {
            m_hitSomething_ = false;
        }

        setPosition(newTrans, true);
        setLinearVelocity({0.0f, 0.0f, 0.0f});
    }

    return true;
}

void LaserProjectile::onRemove() {
    if (getManager()) {
        if (auto* image = m_hitImage_.resolve(*getManager())) {
            image->deleteObject();
        }
    }
    m_hitImage_ = {};
    Parent::onRemove();
}

Box3F LaserProjectile::buildContainerBox(const TMat3F& /*in_rMat*/) const {
    Box3F box{};
    box.fMin = m_startPoint_;
    box.fMax = m_startPoint_;
    box.fMin.setMin(m_endPoint_);
    box.fMax.setMax(m_endPoint_);
    return box;
}

bool LaserProjectile::readjustEnd() {
    if (m_pLaserData_ && !m_pLaserData_->detachFromShooter && m_pShooter_) {
        if (auto* player = dynamic_cast<Player*>(m_pShooter_)) {
            TMat3F muzzle;
            if (!player->getAimedMuzzleTransform(0, &muzzle)) {
                return false;
            }
            m_startPoint_ = muzzle.p;
            if (player->hasPlayerControl() && getManager()) {
                Point3F direction;
                muzzle.getRow(1, direction);
                direction.normalize();
                m_endPoint_ = m_startPoint_ + direction * projectileBeamRange(getManager());
                sim::SimContainerQuery query;
                query.id = player->getId();
                query.type = ~UInt32{0};
                query.mask = csm_collisionMask;
                query.detail = -1;
                query.box.fMin = m_startPoint_;
                query.box.fMax = m_endPoint_;
                sim::SimCollisionInfo collision;
                auto* root = dynamic_cast<sim::SimContainer*>(
                    getManager()->findObject(sim::RootContainerId));
                m_hitSomething_ =
                    root && root->findLOS(query, collision, sim::SimCollisionImageQuery::Normal) &&
                    !collision.surfaces.surfaces.empty();
                if (m_hitSomething_) {
                    m_mul(collision.surfaces.surfaces[0].position,
                          collision.surfaces.tWorld,
                          m_endPoint_);
                }
            }
        }
    }
    updateHitImage();
    return true;
}

void LaserProjectile::updateHitImage() {
    if (!getManager()) {
        return;
    }
    if (auto* image = dynamic_cast<LaserHitShape*>(m_hitImage_.resolve(*getManager()))) {
        image->visible = m_hitSomething_ && !m_overrideRender_;
        TMat3F transform;
        transform.p = m_endPoint_;
        transform.flags |= RMat3F::Matrix_HasTranslation;
        (void)image->setTransform(transform);
    }
}

void LaserProjectile::clientProcess(UInt32 in_currTime) {
    m_overrideRender_ = in_currTime > m_liveTime_;
    if (!m_overrideRender_) {
        (void)readjustEnd();
    }
    if (getManager()) {
        if (auto* image = dynamic_cast<LaserHitShape*>(m_hitImage_.resolve(*getManager()))) {
            image->visible = m_hitSomething_ && !m_overrideRender_;
            if (in_currTime >= m_lastUpdated_) {
                world::TsAdvanceOptions options;
                options.applyRootMotion = false;
                (void)image->advanceAnimation(Flt64(in_currTime - m_lastUpdated_) / 1000.0,
                                              options);
            }
        }
    }
    m_lastUpdated_ = in_currTime;

    setBoundingBox(buildContainerBox(getTransform()));
}

void LaserProjectile::serverProcess(UInt32 in_currTime) {
    if (in_currTime > m_liveTime_) {
        deleteObject();
    }
}

void LaserProjectile::writeInitialPacket(sim::network::GhostManager* gm, BitStream& pStream) const {
    Parent::writeInitialPacket(gm, pStream);
    if (shooterGhostIndex(gm) == -1) {
        writePod(pStream, m_startPoint_);
    }
    pStream.writeFlag(m_hitSomething_);
    writePod(pStream, m_endPoint_);
    pStream.writeFloat(m_startIntensity_, 5);
}

bool LaserProjectile::readInitialPacket(sim::network::GhostManager* gm, BitStream& pStream) {
    if (!Parent::readInitialPacket(gm, pStream)) {
        return false;
    }
    if (!m_pShooter_ && m_explicitShooterGhostIndex_ == -1) {
        if (!readPod(pStream, m_startPoint_)) {
            return false;
        }
    }
    m_hitSomething_ = pStream.readFlag();
    if (!readPod(pStream, m_endPoint_)) {
        return false;
    }
    m_startIntensity_ = pStream.readFloat(5);
    if (!pStream.isValid()) {
        return false;
    }

    TMat3F trans{};
    trans.identity();
    trans.p = m_endPoint_;
    trans.flags |= RMat3F::Matrix_HasTranslation;
    setPosition(trans, true);
    return true;
}

UInt32
LaserProjectile::packUpdate(sim::network::GhostManager& gm, UInt32 mask, BitStream& stream) const {
    const UInt32 parentPunt = Parent::packUpdate(gm, mask, stream);

    if ((mask & Projectile::InitialUpdate) != 0) {
        stream.writeFlag(true);
        writeInitialPacket(&gm, stream);
    } else {
        stream.writeFlag(false);
    }

    return parentPunt;
}

bool LaserProjectile::unpackUpdate(sim::network::GhostManager& gm, BitStream& stream) {
    BitStreamRollbackGuard guard(stream);

    const auto savedDatId = datFileId_;
    const auto savedOwnerId = ownerId_;
    const auto savedShooter = m_pShooter_;
    const auto savedShooterGhost = m_explicitShooterGhostIndex_;
    const auto savedStartPt = m_startPoint_;
    const auto savedEndPt = m_endPoint_;
    const auto savedIntensity = m_startIntensity_;
    const auto savedHit = m_hitSomething_;
    const auto savedTrans = getTransform();
    const auto savedHasExp = m_hasExploded_;

    const auto rollback = [&]() noexcept {
        datFileId_ = savedDatId;
        ownerId_ = savedOwnerId;
        m_pShooter_ = savedShooter;
        m_explicitShooterGhostIndex_ = savedShooterGhost;
        m_startPoint_ = savedStartPt;
        m_endPoint_ = savedEndPt;
        m_startIntensity_ = savedIntensity;
        m_hitSomething_ = savedHit;
        SimMovement::setPosition(savedTrans, true);
        m_hasExploded_ = savedHasExp;
        return false;
    };

    if (!Parent::unpackUpdate(gm, stream)) {
        return rollback();
    }

    if (stream.readFlag()) {
        if (!readInitialPacket(&gm, stream)) {
            return rollback();
        }
    }

    if (!stream.isValid()) {
        return rollback();
    }
    guard.committed = true;
    return true;
}

void LaserProjectile::writeExplosion(BitStream& /*io_pStream*/) const {}

bool LaserProjectile::readExplosion(BitStream& io_pStream) {
    // Lasers have a hit shape, never a projectile explosion packet.
    return io_pStream.isValid();
}

bool LaserProjectile::onSimLightQuery(sim::SimLightQuery& query) const {
    if (m_overrideRender_ || !m_projectileData_ || m_projectileData_->lightRange == 0.0f) {
        return false;
    }
    const UInt32 now = std::max(m_lastUpdated_, currentTimeMs(getManager(), m_lastUpdated_));
    const Flt32 age = Flt32(now - m_spawnTime_) / 1000.0f;
    const Flt32 factor =
        m_projectileData_->liveTime > 0.0f ? 1.0f - age / m_projectileData_->liveTime : 0.0f;
    if (factor <= 0.0f) {
        return false;
    }
    m_endLight_.type = sim::SimLight::Type::Point;
    m_endLight_.intensity = m_projectileData_->lightColor * factor;
    m_endLight_.range = m_projectileData_->lightRange;
    m_endLight_.position = m_endPoint_;

    query.lights.push_back(m_endLight_);
    return true;
}

bool LaserProjectile::processQuery(sim::SimQuery& query) {
    if (auto* surface = dynamic_cast<world::WorldSurfaceQuery*>(&query)) {
        if (m_overrideRender_ || !m_projectileData_ || !readjustEnd()) {
            return false;
        }
        const UInt32 now = std::max(m_lastUpdated_, currentTimeMs(getManager(), m_lastUpdated_));
        const Flt32 age = Flt32(now - m_spawnTime_) / 1000.0f;
        if (m_projectileData_->liveTime <= 0.0f || age > m_projectileData_->liveTime) {
            return false;
        }
        const Flt32 time =
            (1.0f - m_startIntensity_) + m_startIntensity_ * age / m_projectileData_->liveTime;
        return renderProjectileBeam(*surface, m_beamBitmap_, m_startPoint_, m_endPoint_, time);
    }
    if (auto* lq = dynamic_cast<sim::SimLightQuery*>(&query)) {
        if (!readjustEnd()) {
            return false;
        }
        return onSimLightQuery(*lq);
    }
    return Parent::processQuery(query);
}

} // namespace opent1::game
