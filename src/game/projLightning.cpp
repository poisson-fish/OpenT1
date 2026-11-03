#include <opent1/game/player.hpp>
#include <ranges>
#include <opent1/game/proj_lightning.hpp>
#include "projectile_beam.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <format>

namespace opent1::game {

namespace {
Random sg_lightningRandom{};

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

void Lightning::resetRandomSeed(UInt32 seed) noexcept {
    sg_lightningRandom.setSeed(seed);
}

Lightning::Lightning(Int32 in_datFileId) : Projectile(in_datFileId) {
    setNetFlags(Ghostable);
}

Lightning::Lightning() : Lightning(-1) {}

bool Lightning::initResources(GameBaseData* io_pDat) {
    if (!Parent::initResources(io_pDat)) {
        return false;
    }
    m_pLightningData_ = dynamic_cast<LightningData*>(io_pDat);
    return m_pLightningData_ != nullptr;
}

void Lightning::loadResources(bool in_isGhost) {
    Parent::loadResources(in_isGhost);
    if (!in_isGhost || !m_pLightningData_) {
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
        m_beamBitmap_ = resources->load<gfx::Bitmap>(m_pLightningData_->pBitmapName);
    }
}

bool Lightning::processQuery(sim::SimQuery& query) {
    if (auto* light = dynamic_cast<sim::SimLightQuery*>(&query)) {
        if (m_overrideRender_ || !m_pShooter_ || !m_pCurrentTarget_ || !m_pLightningData_ ||
            m_pLightningData_->lightRange == 0.0f) {
            return false;
        }
        m_light_.type = sim::SimLight::Type::Point;
        m_light_.position = m_endPoint_;
        m_light_.intensity = m_pLightningData_->lightColor;
        m_light_.range = m_pLightningData_->lightRange;
        light->lights.push_back(m_light_);
        return true;
    }
    auto* surface = dynamic_cast<world::WorldSurfaceQuery*>(&query);
    if (!surface) {
        return Parent::processQuery(query);
    }
    if (m_overrideRender_ || !m_pCurrentTarget_ || !m_pLightningData_ || !m_beamBitmap_ ||
        !surface->camera) {
        return false;
    }
    auto mesh = projectileBeamMesh(m_beamBitmap_);
    const UInt32 subdivisions = 1u << std::clamp(m_pLightningData_->segmentDivisions, 0, 5);
    const UInt32 step = csm_SubdivisionMenuSize / subdivisions;
    for (const auto& line : m_lines_) {
        if (line.expireTime == 0) {
            continue;
        }
        for (UInt32 i = 0; i < csm_SubdivisionMenuSize; i += step) {
            const Point3F start = i == 0 ? m_startPoint_ : line.points[i];
            (void)appendProjectileBeam(*mesh,
                                       *surface->camera,
                                       start,
                                       line.points[i + step],
                                       std::nullopt,
                                       m_pLightningData_->beamWidth * .5f,
                                       false);
        }
    }
    if (mesh->triangles.empty()) {
        return false;
    }
    surface->image = world::WorldSurfaceInstance{std::move(mesh), TMat3F{}};
    return true;
}

Int32 Lightning::targetGhostIndex(const sim::network::GhostManager* gm) const noexcept {
    if (m_explicitTargetGhostIndex_ != -1) {
        return m_explicitTargetGhostIndex_;
    }
    if (m_pCurrentTarget_ && gm) {
        const auto index = gm->getGhostIndex(*m_pCurrentTarget_);
        return index == sim::InvalidGhostIndex ? -1 : static_cast<Int32>(index);
    }
    return -1;
}

void Lightning::initProjectile(const TMat3F& in_rTrans,
                               const Point3F& /*in_rShooterVel*/,
                               sim::SimObjectId in_shooterId) {
    m_shooterId_ = in_shooterId;
    m_shooterVel_.set(0.0f, 0.0f, 0.0f);
    setOwnerId(in_shooterId);
    updateImageTransform(in_rTrans);
}

bool Lightning::onAdd() {
    if (!Parent::onAdd() || !m_pLightningData_) {
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
        m_lastChargeTime_ = nowMs;
        determineTarget();
        m_lastTargetTime_ = nowMs;
    }
    setBoundingBox(buildContainerBox(getTransform()));
    return true;
}

Box3F Lightning::buildContainerBox(const TMat3F& /*in_rMat*/) const {
    const Flt32 width = m_pLightningData_ ? (m_pLightningData_->boltLength * 0.5f) : 10.0f;
    Box3F box{};
    box.fMin = m_startPoint_ - Point3F{width, width, width};
    box.fMax = m_startPoint_ + Point3F{width, width, width};
    box.fMin.setMin(m_endPoint_ - Point3F{width, width, width});
    box.fMax.setMax(m_endPoint_ + Point3F{width, width, width});
    return box;
}

void Lightning::createSegments(UInt32 in_lineIndex,
                               UInt32 in_currTime,
                               const Point3F& startPt,
                               const Point3F& endPt,
                               const RMat3F& xform) {
    if (in_lineIndex >= csm_numLines || !m_pLightningData_) {
        return;
    }

    Line& rLine = m_lines_[in_lineIndex];
    rLine.createTime = in_currTime;
    rLine.expireTime = in_currTime + static_cast<UInt32>(m_pLightningData_->updateTime) +
                       static_cast<UInt32>(sg_lightningRandom.getInt() % 33);

    rLine.points[0] = startPt;
    rLine.points[csm_SubdivisionMenuSize] = endPt;

    rLine.segs[0].startIndex = 0;
    rLine.segs[0].endIndex = csm_SubdivisionMenuSize;
    UInt32 numToSubdivide = 1;

    Point3F sideDir{};
    Point3F upDir{};
    xform.getRow(0, sideDir);
    xform.getRow(2, upDir);

    Flt32 randFactor = m_pLightningData_->displaceBias;
    while (numToSubdivide < csm_SubdivisionMenuSize) {
        for (auto i : std::views::iota(UInt32{0}) | std::views::take_while([&](UInt32 i) {
                          return i < numToSubdivide;
                      })) {
            const UInt32 startIndex = rLine.segs[i].startIndex;
            const UInt32 endIndex = rLine.segs[i].endIndex;
            const UInt32 midIndex = (startIndex + endIndex) / 2u;

            Point3F newPoint = (rLine.points[startIndex] + rLine.points[endIndex]) * 0.5f;
            const Flt32 sideDisplace = (sg_lightningRandom.getFloat() * 2.0f - 1.0f) * randFactor;
            const Flt32 upDisplace = (sg_lightningRandom.getFloat() * 2.0f - 1.0f) * randFactor;
            newPoint += sideDir * sideDisplace + upDir * upDisplace;
            rLine.points[midIndex] = newPoint;

            rLine.segs[i].endIndex = midIndex;
            rLine.segs[numToSubdivide + i].startIndex = midIndex;
            rLine.segs[numToSubdivide + i].endIndex = endIndex;
        }
        randFactor *= 0.5f;
        numToSubdivide *= 2u;
    }

    for (auto i : std::views::iota(UInt32{0}) | std::views::take_while([&](UInt32 i) {
                      return i < csm_SubdivisionMenuSize;
                  })) {
        rLine.segs[i].startPoint = rLine.points[rLine.segs[i].startIndex];
        rLine.segs[i].endPoint = rLine.points[rLine.segs[i].endIndex];
    }
}

void Lightning::clientProcess(UInt32 in_currTime) {
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

    if (m_pCurrentTarget_ && m_pLightningData_) {
        m_endPoint_ = m_pCurrentTarget_->getTransform().p + m_targetOffset_;
        const Box3F& rBox = m_pCurrentTarget_->getBoundingBox();
        const Point3F radii = (rBox.fMax - rBox.fMin) * 0.5f;
        for (auto i : std::views::iota(UInt32{0}) | std::views::take_while([&](UInt32 i) {
                          return i < csm_numLines;
                      })) {
            if (in_currTime >= m_lines_[i].expireTime) {
                Point3F finalPoint = m_pCurrentTarget_->getBoxCenter();
                finalPoint.x += sg_lightningRandom.getFloat(-0.8f, 0.8f) * radii.x;
                finalPoint.y += sg_lightningRandom.getFloat(-0.8f, 0.8f) * radii.y;
                finalPoint.z += sg_lightningRandom.getFloat(-0.8f, 0.8f) * radii.z;
                createSegments(i, in_currTime, m_startPoint_, finalPoint, muzzleTrans);
            }
        }
    }

    TMat3F soundTransform;
    soundTransform.p = m_endPoint_;
    soundTransform.flags |= RMat3F::Matrix_HasTranslation;
    updateProjectileSound(soundTransform, {});
    m_lastUpdatedClient_ = in_currTime;
    setBoundingBox(buildContainerBox(getTransform()));
}

void Lightning::serverProcess(UInt32 in_currTime) {
    if (m_waitingForDelete_) {
        if (in_currTime >= m_deleteTime_)
            deleteObject();
        return;
    }
    if (!m_targetEvaluated_ || in_currTime - m_lastTargetTime_ >= 100u) {
        determineTarget();
        m_lastTargetTime_ = in_currTime;
    }
    const UInt32 elapsed = in_currTime >= m_lastChargeTime_ ? in_currTime - m_lastChargeTime_ : 0;
    m_lastChargeTime_ = in_currTime;
    m_lastUpdated_ = in_currTime;
    if (elapsed && m_pCurrentTarget_ && m_pLightningData_) {
        const Flt32 seconds = static_cast<Flt32>(elapsed) * .001f;
        Point3F direction = m_endPoint_ - m_startPoint_;
        if (!direction.isZero())
            direction.normalize();
        const auto callback = scriptName("damageTarget");
        if (auto* console = resolveConsole(); console && !callback.empty()) {
            const std::array<std::string, 8> args{
                std::to_string(m_pCurrentTarget_->getId()),
                std::format("{:g}", seconds),
                std::format("{:g}", m_pLightningData_->damagePerSec),
                std::format("{:g}", m_pLightningData_->energyDrainPerSec),
                std::format("{:g} {:g} {:g}", m_endPoint_.x, m_endPoint_.y, m_endPoint_.z),
                std::format("{:g} {:g} {:g}", direction.x, direction.y, direction.z),
                "0 0 0",
                std::to_string(getDamageId())};
            (void)console->execute(callback, args);
        }
    }
    if (in_currTime - m_lastGhosted_ > 300u) {
        m_lastGhosted_ = in_currTime;
        setMaskBits(TargetAndPositionMask);
    }
}

void Lightning::writeInitialPacket(sim::network::GhostManager* gm, BitStream& pStream) const {
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

bool Lightning::readInitialPacket(sim::network::GhostManager* gm, BitStream& pStream) {
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

UInt32 Lightning::packUpdate(sim::network::GhostManager& gm, UInt32 mask, BitStream& stream) const {
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

bool Lightning::unpackUpdate(sim::network::GhostManager& gm, BitStream& stream) {
    BitStreamRollbackGuard guard(stream);

    const auto savedDatId = datFileId_;
    const auto savedOwnerId = ownerId_;
    const auto savedShooter = m_pShooter_;
    const auto savedShooterGhost = m_explicitShooterGhostIndex_;
    const auto savedTarget = m_pCurrentTarget_;
    const auto savedTargetGhost = m_explicitTargetGhostIndex_;
    const auto savedStartPt = m_startPoint_;
    const auto savedEndPt = m_endPoint_;
    const auto savedTargetOffset = m_targetOffset_;

    const auto rollback = [&]() noexcept {
        datFileId_ = savedDatId;
        ownerId_ = savedOwnerId;
        m_pShooter_ = savedShooter;
        m_explicitShooterGhostIndex_ = savedShooterGhost;
        m_pCurrentTarget_ = savedTarget;
        m_explicitTargetGhostIndex_ = savedTargetGhost;
        m_startPoint_ = savedStartPt;
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

void Lightning::writeExplosion(BitStream& /*io_pStream*/) const {}

bool Lightning::readExplosion(BitStream& io_pStream) {
    return io_pStream.isValid();
}

bool Lightning::isSustained() const noexcept {
    return true;
}

void Lightning::shutOffProjectile() {
    if (isGhost()) {
        return;
    }
    m_waitingForDelete_ = true;
    m_deleteTime_ = m_lastUpdated_;
    if (m_pCurrentTarget_) {
        clearNotify(*m_pCurrentTarget_);
        m_pCurrentTarget_ = nullptr;
        setMaskBits(TargetMask);
    }
}

void Lightning::determineTarget() {
    if (!m_pLightningData_ || isGhost()) {
        return;
    }
    m_targetEvaluated_ = true;
    Point3F beamDir{};
    getTransform().getRow(1, beamDir);
    const Flt32 cosCone = std::cos(m_pLightningData_->coneAngle * 0.0174532925f);
    m_endPoint_ = m_startPoint_ + beamDir * m_pLightningData_->boltLength;
    sim::SimContainerQuery query{};
    query.id = getOwnerId();
    query.type = ~UInt32{0};
    constexpr UInt32 targetMask = csm_damageMask | SimShapeObjectType | SimVehicleObjectType;
    query.mask = targetMask;
    query.detail = -1;
    query.box.fMin = m_startPoint_;
    query.box.fMax = m_endPoint_;
    sim::SimContainer* root =
        getManager()
            ? dynamic_cast<sim::SimContainer*>(getManager()->findObject(sim::RootContainerId))
            : nullptr;
    if (!root)
        return;
    ShapeBase* target = nullptr;
    sim::SimCollisionInfo hit{};
    if (root->findLOS(query, hit, sim::SimCollisionImageQuery::High)) {
        target = dynamic_cast<ShapeBase*>(hit.object.resolve(*getManager()));
        if (target) {
            Point3F hitPoint = target->getBoxCenter();
            if (!hit.surfaces.surfaces.empty()) {
                m_mul(hit.surfaces.surfaces[0].position, hit.surfaces.tWorld, hitPoint);
            }
            query.mask = SimInteriorObjectType | SimTerrainObjectType;
            query.box.fMax = target->getBoxCenter();
            sim::SimCollisionInfo obstruction{};
            if (root->findLOS(query, obstruction, sim::SimCollisionImageQuery::High))
                target = nullptr;
            else {
                m_endPoint_ = hitPoint;
                if (m_endPoint_ == m_startPoint_)
                    m_endPoint_ += beamDir * .1f;
            }
        }
    } else {
        const Flt32 sinCone = std::sin(m_pLightningData_->coneAngle * 0.0174532925f);
        query.box = Box3F{m_startPoint_, m_startPoint_};
        query.box.fMin.setMin(m_endPoint_ - beamDir * (m_pLightningData_->boltLength * sinCone));
        query.box.fMax.setMax(m_endPoint_ + beamDir * (m_pLightningData_->boltLength * sinCone));
        sim::SimContainerList candidates;
        root->findIntersections(query, candidates);
        // Preserve the original cone selection order/ranking after the direct ray.
        Flt32 closestDot = 2.f;
        for (auto* candidate : candidates) {
            auto* shape = dynamic_cast<ShapeBase*>(candidate);
            if (!shape || shape == m_pShooter_)
                continue;
            const Point3F offset = shape->getBoxCenter() - m_startPoint_;
            const Flt32 along = m_dot(offset, beamDir), distance = offset.len();
            if (distance < .0001f || along > m_pLightningData_->boltLength)
                continue;
            const Flt32 dot = along / distance;
            if (dot < cosCone || dot >= closestDot)
                continue;
            query.mask = targetMask | SimInteriorObjectType | SimTerrainObjectType;
            query.box = Box3F{m_startPoint_, shape->getBoxCenter()};
            sim::SimCollisionInfo obstruction{};
            if (root->findLOS(query, obstruction, sim::SimCollisionImageQuery::High) &&
                obstruction.object.resolve(*getManager()) != shape)
                continue;
            target = shape;
            closestDot = dot;
            m_endPoint_ = shape->getBoxCenter();
        }
    }
    if (target != m_pCurrentTarget_) {
        if (m_pCurrentTarget_)
            clearNotify(*m_pCurrentTarget_);
        m_pCurrentTarget_ = target;
        if (target)
            deleteNotify(*target);
        setMaskBits(TargetMask);
    }
}

void Lightning::updateImageTransform(const TMat3F& in_rTrans) {
    if (!m_projectileData_) {
        loadDatFile();
    }
    m_startPoint_ = in_rTrans.p;
    setPosition(in_rTrans, true);
}

void Lightning::onDeleteNotify(sim::SimObject& io_pDelete) {
    if (&io_pDelete == m_pCurrentTarget_) {
        m_pCurrentTarget_ = nullptr;
        setMaskBits(TargetMask);
    }
    Parent::onDeleteNotify(io_pDelete);
}

} // namespace opent1::game
