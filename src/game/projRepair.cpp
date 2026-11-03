#include <opent1/game/player.hpp>
#include <ranges>
#include <opent1/game/proj_repair.hpp>
#include "projectile_beam.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>

namespace opent1::game {

namespace {
Random sg_repairRandom{};

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

RepairEffect::RepairEffect(Int32 in_datFileId) : Projectile(in_datFileId) {
    setNetFlags(Ghostable);
}

RepairEffect::RepairEffect() : RepairEffect(-1) {}

bool RepairEffect::initResources(GameBaseData* io_pDat) {
    if (!Parent::initResources(io_pDat)) {
        return false;
    }
    m_pRepairData_ = dynamic_cast<RepairEffectData*>(io_pDat);
    return m_pRepairData_ != nullptr;
}

void RepairEffect::loadResources(bool in_isGhost) {
    Parent::loadResources(in_isGhost);
    if (!in_isGhost || !m_pRepairData_) {
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
        m_beamBitmap_ = resources->load<gfx::Bitmap>(m_pRepairData_->pBitmapName);
    }
}

bool RepairEffect::processQuery(sim::SimQuery& query) {
    if (auto* light = dynamic_cast<sim::SimLightQuery*>(&query)) {
        if (m_overrideRender_ || !m_pShooter_ || !m_pCurrentTarget_ || !m_pRepairData_ ||
            m_pRepairData_->lightRange == 0.0f) {
            return false;
        }
        m_light_.type = sim::SimLight::Type::Point;
        m_light_.position = m_endPoint_;
        m_light_.intensity = m_pRepairData_->lightColor;
        m_light_.range = m_pRepairData_->lightRange;
        light->lights.push_back(m_light_);
        return true;
    }
    auto* surface = dynamic_cast<world::WorldSurfaceQuery*>(&query);
    if (!surface) {
        return Parent::processQuery(query);
    }
    if (m_overrideRender_ || !m_pRepairData_ || !m_beamBitmap_ || !surface->camera ||
        m_line_.expireTime == 0) {
        return false;
    }
    // The authored repair effect only attaches when a target is acquired.
    if (!m_pCurrentTarget_ || m_pCurrentTarget_ == m_pShooter_) {
        return false;
    }
    auto mesh = projectileBeamMesh(m_beamBitmap_);
    const UInt32 subdivisions = 1u << std::clamp(m_pRepairData_->segmentDivisions, 0, 5);
    const UInt32 step = csm_SubdivisionMenuSize / subdivisions;
    for (UInt32 i = 0; i < csm_SubdivisionMenuSize; i += step) {
        const Point3F start = i == 0 ? m_startPoint_ : m_line_.points[i];
        (void)appendProjectileBeam(*mesh,
                                   *surface->camera,
                                   start,
                                   m_line_.points[i + step],
                                   std::nullopt,
                                   m_pRepairData_->beamWidth * .5f,
                                   false);
    }
    if (mesh->triangles.empty()) {
        return false;
    }
    surface->image = world::WorldSurfaceInstance{std::move(mesh), TMat3F{}};
    return true;
}

Int32 RepairEffect::targetGhostIndex(const sim::network::GhostManager* gm) const noexcept {
    if (m_explicitTargetGhostIndex_ != -1) {
        return m_explicitTargetGhostIndex_;
    }
    if (m_pCurrentTarget_ && gm) {
        const auto index = gm->getGhostIndex(*m_pCurrentTarget_);
        return index == sim::InvalidGhostIndex ? -1 : static_cast<Int32>(index);
    }
    return -1;
}

void RepairEffect::initProjectile(const TMat3F& in_rTrans,
                                  const Point3F& /*in_rShooterVel*/,
                                  sim::SimObjectId in_shooterId) {
    m_shooterId_ = in_shooterId;
    m_shooterVel_.set(0.0f, 0.0f, 0.0f);
    setOwnerId(in_shooterId);
    updateImageTransform(in_rTrans);
}

bool RepairEffect::onAdd() {
    if (!Parent::onAdd() || !m_pRepairData_) {
        return false;
    }

    const UInt32 nowMs = currentTimeMs(getManager(), m_lastUpdated_);
    setLinearVelocity({0.0f, 0.0f, 0.0f});
    m_lastGhosted_ = nowMs;
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
        m_lastUpdatedClient_ = nowMs;
    } else {
        determineTarget();
        m_lastTargetTime_ = nowMs;
    }
    setBoundingBox(buildContainerBox(getTransform()));
    return true;
}

Box3F RepairEffect::buildContainerBox(const TMat3F& /*in_rMat*/) const {
    const Flt32 width = m_pRepairData_ ? (m_pRepairData_->boltLength * 0.5f) : 5.0f;
    Box3F box{};
    box.fMin = m_startPoint_ - Point3F{width, width, width};
    box.fMax = m_startPoint_ + Point3F{width, width, width};
    box.fMin.setMin(m_endPoint_ - Point3F{width, width, width});
    box.fMax.setMax(m_endPoint_ + Point3F{width, width, width});
    return box;
}

void RepairEffect::createSegments(UInt32 in_currTime,
                                  const Point3F& startPt,
                                  const Point3F& endPt,
                                  const RMat3F& xform) {
    if (!m_pRepairData_) {
        return;
    }

    m_line_.createTime = in_currTime;
    m_line_.expireTime = in_currTime + static_cast<UInt32>(m_pRepairData_->updateTime) +
                         static_cast<UInt32>(sg_repairRandom.getInt() % 33);

    m_line_.points[0] = startPt;
    m_line_.points[csm_SubdivisionMenuSize] = endPt;

    m_line_.segs[0].startIndex = 0;
    m_line_.segs[0].endIndex = csm_SubdivisionMenuSize;
    UInt32 numToSubdivide = 1;

    Point3F sideDir{};
    Point3F upDir{};
    xform.getRow(0, sideDir);
    xform.getRow(2, upDir);

    Flt32 randFactor = m_pRepairData_->displaceBias;
    while (numToSubdivide < csm_SubdivisionMenuSize) {
        for (auto i : std::views::iota(UInt32{0}) | std::views::take_while([&](UInt32 i) {
                          return i < numToSubdivide;
                      })) {
            const UInt32 startIndex = m_line_.segs[i].startIndex;
            const UInt32 endIndex = m_line_.segs[i].endIndex;
            const UInt32 midIndex = (startIndex + endIndex) / 2u;

            Point3F newPoint = (m_line_.points[startIndex] + m_line_.points[endIndex]) * 0.5f;
            const Flt32 sideDisplace = (sg_repairRandom.getFloat() * 2.0f - 1.0f) * randFactor;
            const Flt32 upDisplace = (sg_repairRandom.getFloat() * 2.0f - 1.0f) * randFactor;
            newPoint += sideDir * sideDisplace + upDir * upDisplace;
            m_line_.points[midIndex] = newPoint;

            m_line_.segs[i].endIndex = midIndex;
            m_line_.segs[numToSubdivide + i].startIndex = midIndex;
            m_line_.segs[numToSubdivide + i].endIndex = endIndex;
        }
        randFactor *= 0.5f;
        numToSubdivide *= 2u;
    }

    for (auto i : std::views::iota(UInt32{0}) | std::views::take_while([&](UInt32 i) {
                      return i < csm_SubdivisionMenuSize;
                  })) {
        m_line_.segs[i].startPoint = m_line_.points[m_line_.segs[i].startIndex];
        m_line_.segs[i].endPoint = m_line_.points[m_line_.segs[i].endIndex];
    }
}

void RepairEffect::clientProcess(UInt32 in_currTime) {
    if (!m_pShooter_) {
        m_overrideRender_ = true;
        stopProjectileSound();
        return;
    }

    TMat3F muzzleTrans{};
    if (m_pShooter_->getMuzzleTransform(0, &muzzleTrans)) {
        if (dynamic_cast<Player*>(m_pShooter_) != nullptr) {
            TMat3F finalTrans{};
            m_mul(muzzleTrans, m_pShooter_->getTransform(), finalTrans);
            m_startPoint_ = finalTrans.p;
            muzzleTrans = finalTrans;
        } else {
            m_startPoint_ = muzzleTrans.p;
        }
    }

    if (m_pCurrentTarget_) {
        m_endPoint_ =
            (m_pCurrentTarget_ == m_pShooter_) ? m_startPoint_ : m_pCurrentTarget_->getBoxCenter();
        if (in_currTime >= m_line_.expireTime) {
            createSegments(in_currTime, m_startPoint_, m_endPoint_, muzzleTrans);
        }
    } else {
        Point3F beamDir{};
        muzzleTrans.getRow(1, beamDir);
        m_endPoint_ = m_startPoint_ + beamDir * 0.5f;
        if (in_currTime >= m_line_.expireTime) {
            createSegments(in_currTime, m_startPoint_, m_endPoint_, muzzleTrans);
        }
    }

    TMat3F soundTransform;
    soundTransform.p = m_endPoint_;
    soundTransform.flags |= RMat3F::Matrix_HasTranslation;
    updateProjectileSound(soundTransform, {});
    m_lastUpdatedClient_ = in_currTime;
    setBoundingBox(buildContainerBox(getTransform()));
}

void RepairEffect::serverProcess(UInt32 in_currTime) {
    if (m_waitingForDelete_) {
        if (in_currTime >= m_deleteTime_)
            deleteObject();
        return;
    }
    if (!m_targetEvaluated_ || in_currTime - m_lastTargetTime_ >= 100u) {
        determineTarget();
        m_lastTargetTime_ = in_currTime;
    }
    m_lastUpdated_ = in_currTime;
    if (in_currTime - m_lastGhosted_ > 300u) {
        m_lastGhosted_ = in_currTime;
        setMaskBits(TargetMask);
    }
}

void RepairEffect::writeInitialPacket(sim::network::GhostManager* gm, BitStream& pStream) const {
    Parent::writeInitialPacket(gm, pStream);
    const Int32 targGhost = targetGhostIndex(gm);
    if (pStream.writeFlag(targGhost != -1)) {
        pStream.writeInt(targGhost, 10);
        const Point3F targetOffset =
            m_endPoint_ -
            (m_pCurrentTarget_ ? m_pCurrentTarget_->getTransform().p : Point3F{0.0f, 0.0f, 0.0f});
        writePod(pStream, targetOffset);
    }
}

bool RepairEffect::readInitialPacket(sim::network::GhostManager* gm, BitStream& pStream) {
    if (!Parent::readInitialPacket(gm, pStream)) {
        return false;
    }

    if (pStream.readFlag()) {
        const UInt32 ghostId = static_cast<UInt32>(pStream.readInt(10));
        Point3F targetOffset{};
        if (!readPod(pStream, targetOffset) || !pStream.isValid()) {
            return false;
        }
        m_explicitTargetGhostIndex_ = static_cast<Int32>(ghostId);
        m_targetOffset_ = targetOffset;
        if (gm) {
            if (auto* pObject = gm->resolveGhost(static_cast<UInt16>(ghostId))) {
                m_pCurrentTarget_ = dynamic_cast<ShapeBase*>(pObject);
                if (m_pCurrentTarget_) {
                    deleteNotify(*m_pCurrentTarget_);
                    m_endPoint_ = m_pCurrentTarget_->getTransform().p + targetOffset;
                }
            }
        }
        if (!m_pCurrentTarget_) {
            m_endPoint_ = targetOffset;
        }
    } else {
        m_explicitTargetGhostIndex_ = -1;
        m_pCurrentTarget_ = nullptr;
        m_targetOffset_.set(0.0f, 0.0f, 0.0f);
    }

    return pStream.isValid();
}

UInt32
RepairEffect::packUpdate(sim::network::GhostManager& gm, UInt32 mask, BitStream& stream) const {
    const UInt32 parentPunt = Parent::packUpdate(gm, mask, stream);

    if ((mask & Projectile::InitialUpdate) != 0) {
        stream.writeFlag(true);
        writeInitialPacket(&gm, stream);
    } else {
        stream.writeFlag(false);
        if (stream.writeFlag((mask & TargetMask) != 0)) {
            const Int32 targGhost = targetGhostIndex(&gm);
            if (stream.writeFlag(targGhost != -1)) {
                stream.writeInt(targGhost, 10);
                const Point3F targetOffset =
                    m_endPoint_ - (m_pCurrentTarget_ ? m_pCurrentTarget_->getTransform().p
                                                     : Point3F{0.0f, 0.0f, 0.0f});
                writePod(stream, targetOffset);
            }
        }
    }

    return parentPunt;
}

bool RepairEffect::unpackUpdate(sim::network::GhostManager& gm, BitStream& stream) {
    BitStreamRollbackGuard guard(stream);

    const auto savedDatId = datFileId_;
    const auto savedOwnerId = ownerId_;
    const auto savedShooter = m_pShooter_;
    const auto savedShooterGhost = m_explicitShooterGhostIndex_;
    const auto savedTarget = m_pCurrentTarget_;
    const auto savedTargetGhost = m_explicitTargetGhostIndex_;
    const auto savedEndPt = m_endPoint_;
    const auto savedTargetOffset = m_targetOffset_;

    const auto rollback = [&]() noexcept {
        datFileId_ = savedDatId;
        ownerId_ = savedOwnerId;
        m_pShooter_ = savedShooter;
        m_explicitShooterGhostIndex_ = savedShooterGhost;
        m_pCurrentTarget_ = savedTarget;
        m_explicitTargetGhostIndex_ = savedTargetGhost;
        m_endPoint_ = savedEndPt;
        m_targetOffset_ = savedTargetOffset;
        return false;
    };

    if (!Parent::unpackUpdate(gm, stream)) {
        return rollback();
    }

    if (stream.readFlag()) {
        if (!readInitialPacket(&gm, stream)) {
            return rollback();
        }
    } else if (stream.readFlag()) {
        if (!stream.isValid()) {
            return rollback();
        }
        if (m_pCurrentTarget_) {
            clearNotify(*m_pCurrentTarget_);
            m_pCurrentTarget_ = nullptr;
        }
        if (stream.readFlag()) {
            const UInt32 ghostId = static_cast<UInt32>(stream.readInt(10));
            Point3F targetOffset{};
            if (!readPod(stream, targetOffset) || !stream.isValid()) {
                return rollback();
            }
            m_explicitTargetGhostIndex_ = static_cast<Int32>(ghostId);
            m_targetOffset_ = targetOffset;
            if (auto* pObject = gm.resolveGhost(static_cast<UInt16>(ghostId))) {
                m_pCurrentTarget_ = dynamic_cast<ShapeBase*>(pObject);
                if (m_pCurrentTarget_) {
                    deleteNotify(*m_pCurrentTarget_);
                    m_endPoint_ = m_pCurrentTarget_->getTransform().p + targetOffset;
                }
            }
            if (!m_pCurrentTarget_) {
                m_endPoint_ = targetOffset;
            }
        } else {
            m_explicitTargetGhostIndex_ = -1;
            m_targetOffset_.set(0.0f, 0.0f, 0.0f);
        }
    }

    if (!stream.isValid()) {
        return rollback();
    }
    guard.committed = true;
    return true;
}

void RepairEffect::writeExplosion(BitStream& /*io_pStream*/) const {}

bool RepairEffect::readExplosion(BitStream& io_pStream) {
    return io_pStream.isValid();
}

bool RepairEffect::isSustained() const noexcept {
    return true;
}

void RepairEffect::shutOffProjectile() {
    if (isGhost() || m_waitingForDelete_) {
        return;
    }
    m_waitingForDelete_ = true;
    m_deleteTime_ = m_lastUpdated_;
    if (m_pCurrentTarget_)
        clearNotify(*m_pCurrentTarget_);
    m_pCurrentTarget_ = nullptr;
    const auto callback = scriptName("onRelease");
    if (auto* console = resolveConsole();
        m_targetEvaluated_ && m_pShooter_ && console && !callback.empty()) {
        const std::array<std::string, 2> args{scriptThis(), std::to_string(m_pShooter_->getId())};
        (void)console->execute(callback, args);
    }
}

void RepairEffect::determineTarget() {
    auto* player = dynamic_cast<Player*>(m_pShooter_);
    if (!player || !m_pRepairData_ || isGhost() || m_waitingForDelete_) {
        return;
    }
    const auto invoke = [&](std::string_view name, bool acquire) {
        const auto callback = scriptName(name);
        if (auto* console = resolveConsole(); console && !callback.empty()) {
            const std::array<std::string, 3> args{
                scriptThis(),
                std::to_string(player->getId()),
                std::to_string(m_pCurrentTarget_ ? m_pCurrentTarget_->getId() : player->getId())};
            (void)console->execute(callback,
                                   std::span<const std::string>(args.data(), acquire ? 3 : 2));
        }
    };
    // Once self repair starts, keep that target until the weapon shuts off.
    if (m_targetEvaluated_ && !m_pCurrentTarget_) {
        invoke("checkDone", false);
        return;
    }
    Point3F position{}, normal{};
    auto* hit = player->findLOSObject(player->getEyeTransform(),
                                      m_pRepairData_->boltLength,
                                      csm_damageMask | SimShapeObjectType | SimVehicleObjectType |
                                          SimInteriorObjectType | SimTerrainObjectType,
                                      &position,
                                      &normal);
    if (hit && (hit->getType() & (SimInteriorObjectType | SimTerrainObjectType)))
        hit = nullptr;
    auto* target = dynamic_cast<ShapeBase*>(hit);
    if (m_targetEvaluated_ && target != m_pCurrentTarget_) {
        player->setImageTriggerUp(0);
        shutOffProjectile();
        return;
    }
    if (target != m_pCurrentTarget_) {
        if (m_pCurrentTarget_)
            clearNotify(*m_pCurrentTarget_);
        m_pCurrentTarget_ = target;
        if (target)
            deleteNotify(*target);
        setMaskBits(TargetMask);
    }
    m_endPoint_ = target ? position : m_startPoint_;
    if (target && m_endPoint_ == m_startPoint_) {
        Point3F direction;
        player->getTransform().getRow(1, direction);
        m_endPoint_ += direction * .1f;
    }
    if (!m_targetEvaluated_) {
        m_targetEvaluated_ = true;
        invoke("onAcquire", true);
    } else {
        invoke("checkDone", false);
    }
}

void RepairEffect::updateImageTransform(const TMat3F& in_rTrans) {
    if (!m_projectileData_) {
        loadDatFile();
    }
    m_startPoint_ = in_rTrans.p;
    setPosition(in_rTrans, true);
}

void RepairEffect::onDeleteNotify(sim::SimObject& io_pDelete) {
    if (&io_pDelete == m_pCurrentTarget_) {
        m_pCurrentTarget_ = nullptr;
        setMaskBits(TargetMask);
    }
    Parent::onDeleteNotify(io_pDelete);
}

} // namespace opent1::game
