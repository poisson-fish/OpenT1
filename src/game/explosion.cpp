#include <opent1/game/sound.hpp>
#include <ranges>
#include <opent1/core/text.hpp>
#include <opent1/game/explosion.hpp>
#include <opent1/world/scene_objects.hpp>

#include <algorithm>
#include <cmath>
#include <numbers>

namespace opent1::game {

namespace {
constexpr Flt32 kTwoPi = static_cast<Flt32>(2.0 * std::numbers::pi);
Random g_expRandom{};

Int32 findSequenceIndex(const world::TsShape& shape, std::string_view name) noexcept {
    for (auto i : std::views::iota(std::size_t{0}, shape.sequences.size())) {
        const Int32 nameIdx = shape.sequences[i].name;
        if (nameIdx >= 0 && static_cast<std::size_t>(nameIdx) < shape.names.size() &&
            equalIgnoreCase(shape.names[static_cast<std::size_t>(nameIdx)], name)) {
            return static_cast<Int32>(i);
        }
    }
    return -1;
}
} // namespace

void Explosion::resetRandomSeed(UInt32 seed) noexcept {
    g_expRandom.setSeed(seed);
}

Explosion::Explosion(Int32 in_datFileId) {
    datFileName_.clear();
    datFileId_ = in_datFileId;
    m_renderImage_.transform.identity();
}

Explosion::~Explosion() {
    m_renderImage_.clearShapeInstance();
}

void Explosion::setPosition(const Point3F& in_rPosition) {
    m_position_ = in_rPosition;
    m_renderImage_.transform.p = in_rPosition;
    m_renderImage_.transform.flags |= RMat3F::Matrix_HasTranslation;

    TMat3F newPosition = getTransform();
    newPosition.p = m_position_;
    newPosition.flags |= RMat3F::Matrix_HasTranslation;
    SimMovement::setPosition(newPosition, true);
}

void Explosion::setAxis(const Point3F& in_rAxis) noexcept {
    if (m_dot(in_rAxis, in_rAxis) > 0.1f) {
        m_axis_ = in_rAxis;
        m_axis_.normalize();
    }
}

bool Explosion::initResources(GameBaseData* io_pDat) {
    if (!Parent::initResources(io_pDat)) {
        return false;
    }
    m_explosionData_ = dynamic_cast<ExplosionData*>(io_pDat);
    return m_explosionData_ != nullptr;
}

void Explosion::loadResources() {
    m_renderImage_.clearShapeInstance();
    m_shapeInstance_.reset();
    m_mainThread_.reset();
    m_visThread_.reset();
    m_hShape_.reset();
    m_worldResources_.reset();

    if (!m_explosionData_) {
        return;
    }

    if (m_explosionData_->shape) {
        m_hShape_ = m_explosionData_->shape;
    } else if (!m_explosionData_->pShapeName.empty() && getManager()) {
        if (auto* sr = sim::SimResource::find(*getManager())) {
            m_hShape_ = sr->get().load<world::TsShape>(m_explosionData_->pShapeName);
        }
    }

    if (m_hShape_) {
        m_shapeInstance_ = std::make_shared<world::TsShapeInstance>(m_hShape_);
        m_renderImage_.setShapeInstance(m_shapeInstance_.get(), m_shapeInstance_);

        const Int32 visSeq = findSequenceIndex(*m_hShape_, "visibility");
        if (visSeq != -1) {
            const Flt32 visDur = m_hShape_->sequences[static_cast<std::size_t>(visSeq)].duration;
            const Flt32 pos = (2.0f / 3.0f) + g_expRandom.getFloat() * (visDur / 3.0f);
            m_visThread_ = m_shapeInstance_->addThread(visSeq, pos);
            if (m_visThread_) {
                m_shapeInstance_->setTimeScale(*m_visThread_, 0.0f);
            }
        }

        if (!m_hShape_->sequences.empty()) {
            m_mainThread_ = m_shapeInstance_->addThread(0, 0.0f);
            if (m_mainThread_) {
                const Flt32 ts = (m_explosionData_->timeScale != 0.0f)
                                     ? (1.0f / m_explosionData_->timeScale)
                                     : 1.0f;
                m_shapeInstance_->setTimeScale(*m_mainThread_, ts);
                m_sequenceDuration_ = m_hShape_->sequences[0].duration;
            }
        }

        m_bbox_.fMin = m_hShape_->center + m_shapeInstance_->rootDelta().p;
        m_bbox_.fMax = m_bbox_.fMin;
        const Point3F radVec{m_hShape_->radius, m_hShape_->radius, m_hShape_->radius};
        m_bbox_.fMin -= radVec;
        m_bbox_.fMax += radVec;
    }
    setPosition(m_position_);
}

void Explosion::setExplicitTiming(UInt32 spawnTimeMs, Flt32 sequenceDurationSec) noexcept {
    m_spawnTime_ = spawnTimeMs;
    m_lastUpdated_ = spawnTimeMs;
    m_sequenceDuration_ = sequenceDurationSec;
    const Flt32 ts = m_explosionData_ ? m_explosionData_->timeScale : 1.0f;
    const Flt32 tZero = m_explosionData_ ? m_explosionData_->timeZero : 0.25f;
    const Flt32 tOne = m_explosionData_ ? m_explosionData_->timeOne : 0.75f;
    m_expireTime_ = m_spawnTime_ + static_cast<UInt32>(ts * sequenceDurationSec * 1000.0f);
    m_timeZeroMS_ = static_cast<Int32>(ts * sequenceDurationSec * 1000.0f * tZero);
    m_timeOneMS_ = static_cast<Int32>(ts * sequenceDurationSec * 1000.0f * tOne);
}

bool Explosion::onAdd() {
    if (!Parent::onAdd()) {
        return false;
    }

    if (!m_explosionData_ && datFileId_ != -1) {
        loadDatFile();
    }
    loadResources();
    // Explosions are local effects created by client projectile processing,
    // rather than network ghosts in their own right.
    if (m_explosionData_ && m_explosionData_->soundId >= 0) {
        (void)TSFX::PlayAt(UInt32(m_explosionData_->soundId), getTransform(), {}, getManager());
    }

    if (m_explosionData_) {
        if (m_explosionData_->randomSpin) {
            m_axisSpin_ = g_expRandom.getFloat(kTwoPi);
        }
        if (!m_explosionData_->faceCamera && m_dot(m_axis_, m_axis_) > 0.01f) {
            if (const auto matOpt = world::faceDirection(m_axis_, std::nullopt, m_axisSpin_)) {
                m_renderImage_.transform = *matOpt;
            }
        }
    }
    m_renderImage_.transform.p = m_position_;
    m_renderImage_.transform.flags |= RMat3F::Matrix_HasTranslation;

    const UInt32 now =
        getManager() ? static_cast<UInt32>(getManager()->getCurrentTime() * 1000.0f) : m_spawnTime_;
    setExplicitTiming(now, m_sequenceDuration_);

    if (m_explosionData_ && m_explosionData_->hasLight && sm_useDynamicLighting) {
        m_light_.type = sim::SimLight::Type::Point;
        addToSet(sim::LightSetId);
    }

    if (auto* mgr = getManager()) {
        if (auto* pRoot = dynamic_cast<sim::SimContainer*>(mgr->findObject(sim::RootContainerId))) {
            pRoot->addObject(*this);
        }
    }
    addToSet(PlayerSetId);
    addToSet(sim::RenderSetId);
    return true;
}

void Explosion::onRemove() {
    if (auto* cnt = getContainer()) {
        cnt->removeObject(*this);
    }
    m_renderImage_.clearShapeInstance();
    m_shapeInstance_.reset();
    m_worldResources_.reset();
    Parent::onRemove();
}

Point3F Explosion::computeShiftedPosition(const Point3F& cameraAxis,
                                          Flt32 cameraDistance) const noexcept {
    if (!m_explosionData_ || !m_explosionData_->shiftPosition) {
        return m_position_;
    }
    constexpr Flt32 kMaxShiftRadius = 3.0f;
    constexpr Flt32 kMaxShiftCamAxis = 0.3f;

    Flt32 dot = m_dot(cameraAxis, m_axis_);
    if (dot < 0.0f) {
        if (dot > -1.0f / kMaxShiftRadius) {
            dot = -kMaxShiftRadius;
        } else {
            dot = 1.0f / dot;
        }
        const Flt32 radius = (m_hShape_ && m_hShape_->radius > 0.0f) ? m_hShape_->radius : 1.0f;
        Flt32 shiftDist = radius * dot;
        if (-shiftDist > cameraDistance * kMaxShiftCamAxis) {
            shiftDist = -cameraDistance * kMaxShiftCamAxis;
        }
        Point3F getAway = cameraAxis;
        getAway *= shiftDist;
        return m_position_ + getAway;
    }
    return m_position_;
}

bool Explosion::computeLightAtTime(UInt32 currTimeMs, sim::SimLight& outLight) const noexcept {
    if (!m_explosionData_ || !m_explosionData_->hasLight || !sm_useDynamicLighting) {
        return false;
    }
    const Int32 currTime = static_cast<Int32>(currTimeMs) - static_cast<Int32>(m_spawnTime_);
    if (currTime < 0 || currTimeMs >= m_expireTime_) {
        return false;
    }

    ColorF color{};
    Flt32 factor = 0.0f;

    if (currTime < m_timeZeroMS_ && m_timeZeroMS_ > 0) {
        const Flt32 factor2 =
            static_cast<Flt32>(m_timeZeroMS_ - currTime) / static_cast<Flt32>(m_timeZeroMS_);
        factor = m_explosionData_->radFactors[1] +
                 (factor2 * (m_explosionData_->radFactors[0] - m_explosionData_->radFactors[1]));
        color.interpolate(m_explosionData_->colors[1], m_explosionData_->colors[0], factor2);
    } else if (currTime <= m_timeOneMS_ && m_timeOneMS_ != m_timeZeroMS_) {
        const Flt32 factor2 = static_cast<Flt32>(m_timeOneMS_ - currTime) /
                              static_cast<Flt32>(m_timeOneMS_ - m_timeZeroMS_);
        factor = m_explosionData_->radFactors[1] +
                 (factor2 * (m_explosionData_->radFactors[2] - m_explosionData_->radFactors[1]));
        color.interpolate(m_explosionData_->colors[2], m_explosionData_->colors[1], factor2);
    } else {
        const Int32 totalSpan = static_cast<Int32>(m_expireTime_ - m_spawnTime_);
        const Int32 denom = std::max(1, totalSpan - m_timeOneMS_);
        factor = static_cast<Flt32>(totalSpan - currTime) / static_cast<Flt32>(denom);
        color = m_explosionData_->colors[2] * factor;
        factor *= m_explosionData_->radFactors[2];
    }

    outLight.type = sim::SimLight::Type::Point;
    outLight.position = m_position_;
    outLight.intensity = color;
    outLight.range = m_explosionData_->lightRange * factor;
    return true;
}

bool Explosion::onSimLightQuery(sim::SimLightQuery& query) const {
    query.ambient = ColorF{0.0f, 0.0f, 0.0f};
    query.lights.clear();
    const UInt32 now = getManager() ? static_cast<UInt32>(getManager()->getCurrentTime() * 1000.0f)
                                    : m_lastUpdated_;
    if (computeLightAtTime(now, m_light_)) {
        query.lights.push_back(m_light_);
        return true;
    }
    return true;
}

bool Explosion::onWorldShapeQuery(world::WorldShapeQuery& query) {
    const UInt32 now = getManager() ? static_cast<UInt32>(getManager()->getCurrentTime() * 1000.0f)
                                    : m_lastUpdated_;
    if (now > m_expireTime_ || !m_shapeInstance_) {
        return false;
    }
    if (!m_worldResources_) {
        world::TsShapeResources resources;
        resources.shape = m_hShape_;
        if (m_hShape_->materials) {
            auto* dbm = resolveDataBlockManager();
            if (!dbm || !dbm->resources()) {
                return false;
            }
            auto materials =
                world::TsMaterialResources::load(*dbm->resources(), *m_hShape_->materials);
            if (!materials) {
                return false;
            }
            resources.materials = std::move(*materials);
        }
        m_worldResources_ = std::move(resources);
    }
    if (m_mainThread_ && now > m_lastUpdated_) {
        const Flt32 dt = static_cast<Flt32>(now - m_lastUpdated_) * 0.001f;
        m_shapeInstance_->advanceThread(*m_mainThread_, static_cast<Flt64>(dt));
        m_lastUpdated_ = now;
    }
    if (const auto poseOpt = m_shapeInstance_->pose()) {
        world::WorldShapeImage img{};
        img.resources = *m_worldResources_;
        img.pose = *poseOpt;
        img.shapeToWorld = m_renderImage_.transform;
        if (query.cameraPosition && m_explosionData_) {
            Point3F cameraAxis = m_position_ - *query.cameraPosition;
            const Flt32 cameraDistance = cameraAxis.len();
            if (cameraDistance > 0.0f) {
                cameraAxis /= cameraDistance;
                if (m_explosionData_->faceCamera) {
                    if (const auto facing =
                            world::faceDirection(cameraAxis, std::nullopt, m_axisSpin_)) {
                        img.shapeToWorld = *facing;
                    }
                }
                img.shapeToWorld.p = computeShiftedPosition(cameraAxis, cameraDistance);
            }
        }
        // As with mounted weapons, pose nodes exclude the authored DTS root.
        const auto& root = m_hShape_->transforms[m_hShape_->nodes[0].defaultTransform];
        TMat3F placed;
        m_mul(TMat3F(root.rotation, root.translation), img.shapeToWorld, placed);
        img.shapeToWorld = placed;
        img.shapeToWorld.flags |= RMat3F::Matrix_HasTranslation;
        img.detail = 0;
        img.automaticDetail = true;
        query.image = std::move(img);
        return true;
    }
    return false;
}

bool Explosion::processQuery(sim::SimQuery& query) {
    if (auto* lightQ = dynamic_cast<sim::SimLightQuery*>(&query)) {
        return onSimLightQuery(*lightQ);
    }
    if (auto* shapeQ = dynamic_cast<world::WorldShapeQuery*>(&query)) {
        return onWorldShapeQuery(*shapeQ);
    }
    return Parent::processQuery(query);
}

void Explosion::clientProcess(UInt32 in_currTime) {
    if (in_currTime > m_expireTime_) {
        deleteObject();
    }
}

Box3F Explosion::buildContainerBox(const TMat3F& in_rMat) const {
    return Box3F{in_rMat.p - Point3F{0.1f, 0.1f, 0.1f}, in_rMat.p + Point3F{0.1f, 0.1f, 0.1f}};
}

void Explosion::applyRadiusDamage(sim::SimManager* manager,
                                  Int32 type,
                                  const Point3F& pos,
                                  const Point3F& vec,
                                  Flt32 radius,
                                  Flt32 damageValue,
                                  Flt32 force,
                                  Int32 srcObjectId) {
    if (!manager || radius <= 0.0f) {
        return;
    }
    auto* root = dynamic_cast<sim::SimContainer*>(manager->findObject(sim::RootContainerId));
    if (!root) {
        return;
    }

    sim::SimContainerQuery query{};
    query.id = ~sim::SimObjectId{0};
    query.type = ~UInt32{0};
    query.mask = DamageMask;
    query.detail = sim::SimCollisionImageQuery::Normal;
    query.box.fMin = pos - Point3F{radius, radius, radius};
    query.box.fMax = pos + Point3F{radius, radius, radius};

    sim::SimContainerList scList;
    root->findIntersections(query, scList);

    for (auto* cnt : scList) {
        auto* pShape = dynamic_cast<ShapeBase*>(cnt);
        if (!pShape) {
            continue;
        }

        Point3F mVec = pShape->getBoxCenter();
        mVec -= pos;
        Flt32 dist = mVec.len();

        const Box3F& bbox = pShape->getBoundingBox();
        Flt32 minDim = bbox.len_x();
        if (bbox.len_y() < minDim) {
            minDim = bbox.len_y();
        }
        if (bbox.len_z() < minDim) {
            minDim = bbox.len_z();
        }
        dist -= minDim;

        if (dist <= 1.0f) {
            dist = 1.0f;
        }
        if (dist >= radius) {
            continue;
        }

        const Flt32 od = 1.0f / dist;
        Point3F vVec{};
        if (vec.x + vec.y + vec.z == 0.0f) {
            vVec.x = mVec.x * od;
            vVec.y = mVec.y * od;
            vVec.z = mVec.z * od;
        } else {
            vVec = vec;
        }

        const Flt32 scale = (radius - dist) / radius;
        Flt32 damage = damageValue * scale;
        mVec *= force * scale * od;

        const Flt32 cover = pShape->coverage(pos);
        damage *= cover;
        mVec *= cover;

        if (cover != 0.0f) {
            pShape->applyDamage(
                type, damage, pos, vVec, mVec, static_cast<sim::SimObjectId>(srcObjectId));
        }
    }
}

bool registerExplosionClass(Persistent::ClassRegistry& registry) {
    return registry.registerClass<Explosion>("Explosion", ExplosionPersTag);
}

bool registerExplosionNetwork(sim::network::Registry& registry) {
    return registry.registerGhost<Explosion>(static_cast<UInt16>(ExplosionPersTag));
}

} // namespace opent1::game
