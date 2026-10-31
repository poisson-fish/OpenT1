#include <opent1/game/player.hpp>
#include <opent1/game/proj_target_laser.hpp>
#include "projectile_beam.hpp"

#include <algorithm>
#include <array>
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

TargetLaser::TargetLaser(Int32 in_datFileId) : Projectile(in_datFileId) {
    setNetFlags(Ghostable | ScopeAlways);
}

TargetLaser::TargetLaser() : TargetLaser(-1) {}

bool TargetLaser::initResources(GameBaseData* io_pDat) {
    if (!Parent::initResources(io_pDat)) {
        return false;
    }
    m_pLaserData_ = dynamic_cast<TargetLaserData*>(io_pDat);
    return m_pLaserData_ != nullptr;
}

void TargetLaser::loadResources(bool in_isGhost) {
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
    if (resources) {
        m_beamBitmap_ = resources->load<gfx::Bitmap>(m_pLaserData_->pBitmapName);
    }
}

void TargetLaser::initProjectile(const TMat3F& in_rTrans,
                                 const Point3F& /*in_rShooterVel*/,
                                 sim::SimObjectId in_shooterId) {
    setPosition(in_rTrans, true);
    m_shooterId_ = in_shooterId;
    m_shooterVel_.set(0.0f, 0.0f, 0.0f);
    setOwnerId(in_shooterId);
    m_startPoint_ = in_rTrans.p;
}

bool TargetLaser::onAdd() {
    if (!Parent::onAdd() || !m_pLaserData_) {
        return false;
    }

    const UInt32 nowMs = currentTimeMs(getManager(), m_lastUpdated_);
    if (isGhost()) {
        m_spawnTime_ = nowMs;
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
    } else {
        if (m_pShooter_) {
            setTeam(m_pShooter_->getTeam());
        } else {
            setTeam(0);
        }
        updateImageTransform(getTransform());
        setLinearVelocity({0.0f, 0.0f, 0.0f});
    }

    addToSet(TargetableSetId);
    return true;
}

void TargetLaser::onRemove() {
    removeFromSet(TargetableSetId);
    Parent::onRemove();
}

Box3F TargetLaser::buildContainerBox(const TMat3F& /*in_rMat*/) const {
    Box3F box{};
    box.fMin = m_startPoint_;
    box.fMax = m_startPoint_;
    box.fMin.setMin(m_endPoint_);
    box.fMax.setMax(m_endPoint_);
    return box;
}

void TargetLaser::clientProcess(UInt32 in_currTime) {
    m_lastUpdated_ = in_currTime;
    if (!m_pShooter_) {
        // The wire includes both endpoints, so an unscoped/deleted shooter
        // does not make a remote targeting beam disappear.
        return;
    }
    TMat3F muzzleTrans{};
    if (m_pShooter_->getMuzzleTransform(0, &muzzleTrans)) {
        if (auto* player = dynamic_cast<Player*>(m_pShooter_)) {
            TMat3F aimed;
            if (player->getAimedMuzzleTransform(0, &aimed)) {
                m_startPoint_ = aimed.p;
                if (player->hasPlayerControl()) {
                    updateImageTransform(aimed);
                }
            }
        } else {
            m_startPoint_ = muzzleTrans.p;
        }
    }
    setBoundingBox(buildContainerBox(getTransform()));
}

void TargetLaser::serverProcess(UInt32 in_currTime) {
    if (m_waitingForDelete_ && in_currTime >= m_deleteTime_) {
        deleteObject();
        return;
    }

    if (m_pShooter_ && m_pShooter_->getTeam() != getTeam()) {
        setTeam(m_pShooter_->getTeam());
        setMaskBits(TeamMask);
    }

    if (in_currTime - m_lastGhosted_ > 100u) {
        m_lastGhosted_ = in_currTime;
        setMaskBits(EndPointMask);
    }
}

void TargetLaser::writeInitialPacket(sim::network::GhostManager* gm, BitStream& pStream) const {
    Parent::writeInitialPacket(gm, pStream);
    writePod(pStream, m_startPoint_);
    writePod(pStream, m_endPoint_);
    Int32 playerId = m_clientId_;
    if (m_pShooter_ && m_pShooter_->getControlClient() >= 2048) {
        playerId = m_pShooter_->getControlClient();
    }
    pStream.writeInt(std::clamp(playerId - 2048, 0, 1023), 10);
    pStream.writeInt(getTeam() & 0x7, 3);
    pStream.writeFlag(m_hasTarget_);
}

bool TargetLaser::readInitialPacket(sim::network::GhostManager* gm, BitStream& pStream) {
    if (!Parent::readInitialPacket(gm, pStream)) {
        return false;
    }
    if (!readPod(pStream, m_startPoint_) || !readPod(pStream, m_endPoint_)) {
        return false;
    }
    m_clientId_ = pStream.readInt(10) + 2048;
    const Int32 teamVal = pStream.readInt(3);
    m_hasTarget_ = pStream.readFlag();
    if (!pStream.isValid()) {
        return false;
    }
    setTeam(teamVal);

    TMat3F trans{};
    trans.identity();
    trans.p = m_endPoint_;
    trans.flags |= RMat3F::Matrix_HasTranslation;
    setPosition(trans, true);
    return true;
}

UInt32
TargetLaser::packUpdate(sim::network::GhostManager& gm, UInt32 mask, BitStream& stream) const {
    const UInt32 parentPunt = Parent::packUpdate(gm, mask, stream);

    if ((mask & Projectile::InitialUpdate) != 0) {
        stream.writeFlag(true);
        writeInitialPacket(&gm, stream);
    } else {
        stream.writeFlag(false);
        writePod(stream, m_startPoint_);
        writePod(stream, m_endPoint_);
        stream.writeFlag(m_hasTarget_);
    }

    return parentPunt;
}

bool TargetLaser::unpackUpdate(sim::network::GhostManager& gm, BitStream& stream) {
    BitStreamRollbackGuard guard(stream);

    const auto savedDatId = datFileId_;
    const auto savedOwnerId = ownerId_;
    const auto savedShooter = m_pShooter_;
    const auto savedShooterGhost = m_explicitShooterGhostIndex_;
    const auto savedStartPt = m_startPoint_;
    const auto savedEndPt = m_endPoint_;
    const auto savedHasTarg = m_hasTarget_;
    const auto savedClientId = m_clientId_;
    const auto savedTeam = getTeam();
    const auto savedTrans = getTransform();

    const auto rollback = [&]() noexcept {
        datFileId_ = savedDatId;
        ownerId_ = savedOwnerId;
        m_pShooter_ = savedShooter;
        m_explicitShooterGhostIndex_ = savedShooterGhost;
        m_startPoint_ = savedStartPt;
        m_endPoint_ = savedEndPt;
        m_hasTarget_ = savedHasTarg;
        m_clientId_ = savedClientId;
        setTeam(savedTeam);
        SimMovement::setPosition(savedTrans, true);
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
        if (!readPod(stream, m_startPoint_) || !readPod(stream, m_endPoint_)) {
            return rollback();
        }
        m_hasTarget_ = stream.readFlag();
        if (!stream.isValid()) {
            return rollback();
        }
        TMat3F trans = getTransform();
        trans.p = m_endPoint_;
        trans.flags |= RMat3F::Matrix_HasTranslation;
        setPosition(trans, true);
    }

    if (!stream.isValid()) {
        return rollback();
    }
    guard.committed = true;
    return true;
}

void TargetLaser::writeExplosion(BitStream& /*io_pStream*/) const {}

bool TargetLaser::readExplosion(BitStream& io_pStream) {
    return io_pStream.isValid();
}

bool TargetLaser::onSimLightQuery(sim::SimLightQuery& query) const {
    if (m_overrideRender_ || !m_projectileData_ || m_projectileData_->lightRange == 0.0f) {
        return false;
    }
    m_endLight_.type = sim::SimLight::Type::Point;
    m_endLight_.intensity = m_projectileData_->lightColor;
    m_endLight_.range = m_projectileData_->lightRange;
    m_endLight_.position = m_endPoint_;

    query.lights.push_back(m_endLight_);
    return true;
}

bool TargetLaser::processQuery(sim::SimQuery& query) {
    if (auto* surface = dynamic_cast<world::WorldSurfaceQuery*>(&query)) {
        return !m_overrideRender_ &&
               renderProjectileBeam(*surface, m_beamBitmap_, m_startPoint_, m_endPoint_, .75f);
    }
    if (auto* lq = dynamic_cast<sim::SimLightQuery*>(&query)) {
        return onSimLightQuery(*lq);
    }
    return Parent::processQuery(query);
}

bool TargetLaser::isSustained() const noexcept {
    return true;
}

void TargetLaser::shutOffProjectile() {
    if (isGhost()) {
        return;
    }
    deleteObject();
}

void TargetLaser::updateImageTransform(const TMat3F& in_rTrans) {
    if (!m_projectileData_) {
        loadDatFile();
    }
    setPosition(in_rTrans, true);

    m_startPoint_ = getTransform().p;
    Point3F beamDir{};
    getTransform().getRow(1, beamDir);
    beamDir *= 4000.0f;
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
        m_mul(collisionInfo.surfaces.surfaces[0].position,
              collisionInfo.surfaces.tWorld,
              m_endPoint_);
        newTrans.p = m_endPoint_;
        newTrans.flags |= RMat3F::Matrix_HasTranslation;
        m_hasTarget_ = true;
    } else {
        m_hasTarget_ = false;
    }

    setPosition(newTrans, true);
}

bool TargetLaser::getTarget(Point3F* out_pTarget, Int32* out_pTeam) const noexcept {
    if (!m_hasTarget_) {
        return false;
    }
    if (out_pTarget) {
        *out_pTarget = m_endPoint_;
    }
    if (out_pTeam) {
        *out_pTeam = getTeam();
    }
    return true;
}

} // namespace opent1::game
