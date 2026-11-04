#include <opent1/game/smoke_puff.hpp>
#include <opent1/world/scene_objects.hpp>

#include <algorithm>
#include <cmath>
#include <numbers>

namespace opent1::game {

namespace {
constexpr Flt32 kTwoPi = static_cast<Flt32>(2.0 * std::numbers::pi);
} // namespace

SmokeManager::SmokeManager() {
    setId(SmokeManagerId);
    setContainerMask(0);
}

bool SmokeManager::onAdd() {
    if (!Parent::onAdd()) {
        return false;
    }
    if (auto* mgr = getManager()) {
        currentTimeMs_ = static_cast<UInt32>(mgr->getCurrentTime() * 1000.0f);
        if (auto* root = dynamic_cast<sim::SimContainer*>(mgr->findObject(sim::RootContainerId))) {
            root->addObject(*this);
        }
        addToSet(PlayerSetId);
        addToSet(sim::RenderSetId);
    }
    setBoundingBox(Box3F{Point3F{-1.0f, -1.0f, -1.0f}, Point3F{1.0f, 1.0f, 1.0f}});
    return true;
}

void SmokeManager::onRemove() {
    if (auto* cnt = getContainer()) {
        cnt->removeObject(*this);
    }
    for (auto& puff : puffs_) {
        puff.renderImage.clearShapeInstance();
        puff.shapeInstance.reset();
        puff.shape.reset();
        puff.resources.reset();
        puff.inUse = false;
    }
    Parent::onRemove();
}

SmokeManager* SmokeManager::get(sim::SimManager* io_pManager) noexcept {
    if (!io_pManager) {
        return nullptr;
    }
    return dynamic_cast<SmokeManager*>(io_pManager->findObject(SmokeManagerId));
}

SmokePuff* SmokeManager::allocatePuff(UInt32 currTimeMs) noexcept {
    SmokePuff* oldest = &puffs_[0];
    for (auto& puff : puffs_) {
        if (!puff.inUse) {
            return &puff;
        }
        if (puff.expireTime <= currTimeMs) {
            puff.inUse = false;
            return &puff;
        }
        if (puff.expireTime < oldest->expireTime) {
            oldest = &puff;
        }
    }
    oldest->inUse = false;
    return oldest;
}

void SmokeManager::addPuff(const Resource<world::TsShape>& in_rShape,
                           Flt32 in_totalTime,
                           const Point3F& in_rPosition,
                           const Point3F& in_rVelocity,
                           UInt32 explicitTimeMs) {
    const UInt32 now =
        explicitTimeMs != 0
            ? explicitTimeMs
            : (getManager() ? static_cast<UInt32>(getManager()->getCurrentTime() * 1000.0f)
                            : currentTimeMs_);
    currentTimeMs_ = now;

    SmokePuff* puff = allocatePuff(now);
    puff->shapeName.clear();
    puff->shape = in_rShape;
    puff->resources.reset();
    puff->shapeInstance.reset();
    puff->threadIndex.reset();
    puff->renderImage.clearShapeInstance();

    if (in_rShape) {
        puff->shapeInstance = std::make_shared<world::TsShapeInstance>(in_rShape);
        puff->renderImage.setShapeInstance(puff->shapeInstance.get(), puff->shapeInstance);
        if (!in_rShape->sequences.empty()) {
            puff->threadIndex = puff->shapeInstance->addThread(0, 0.0f);
            if (puff->threadIndex) {
                const Flt32 dur = std::max(0.001f, in_rShape->sequences[0].duration);
                const Flt32 safeTotal = std::max(0.001f, in_totalTime);
                puff->shapeInstance->setTimeScale(*puff->threadIndex, dur / safeTotal);
            }
        }
    }

    puff->position = in_rPosition;
    puff->velocity = in_rVelocity;
    puff->totalTime = in_totalTime;
    puff->spin = random_.getFloat(kTwoPi);
    puff->spawnTime = now;
    puff->lastUpdated = now;
    puff->expireTime = now + static_cast<UInt32>(std::max(0.0f, in_totalTime) * 1000.0f);
    puff->renderImage.transform.identity();
    puff->renderImage.transform.p = in_rPosition;
    puff->inUse = true;

    updateBoundingBox();
}

void SmokeManager::addPuff(std::string_view in_shapeName,
                           Flt32 in_totalTime,
                           const Point3F& in_rPosition,
                           const Point3F& in_rVelocity,
                           UInt32 explicitTimeMs) {
    Resource<world::TsShape> loadedShape;
    if (auto* mgr = getManager()) {
        if (auto* sr = sim::SimResource::find(*mgr)) {
            loadedShape = sr->get().load<world::TsShape>(std::string(in_shapeName));
        }
    }
    addPuff(loadedShape, in_totalTime, in_rPosition, in_rVelocity, explicitTimeMs);
    for (auto& puff : puffs_) {
        if (puff.inUse && puff.position == in_rPosition && puff.shapeName.empty()) {
            puff.shapeName.assign(in_shapeName);
            break;
        }
    }
}

void SmokeManager::removeAllInBounds(const Box3F& in_rBox) noexcept {
    for (auto& puff : puffs_) {
        if (!puff.inUse) {
            continue;
        }
        if (in_rBox.contains(puff.position)) {
            puff.renderImage.clearShapeInstance();
            puff.shapeInstance.reset();
            puff.shape.reset();
            puff.resources.reset();
            puff.inUse = false;
        }
    }
    updateBoundingBox();
}

void SmokeManager::removeAllInRadius(const Point3F& in_rPosition, Flt32 in_radius) noexcept {
    const Flt32 radSq = in_radius * in_radius;
    for (auto& puff : puffs_) {
        if (!puff.inUse) {
            continue;
        }
        const Point3F delta = puff.position - in_rPosition;
        if (m_dot(delta, delta) <= radSq) {
            puff.renderImage.clearShapeInstance();
            puff.shapeInstance.reset();
            puff.shape.reset();
            puff.resources.reset();
            puff.inUse = false;
        }
    }
    updateBoundingBox();
}

void SmokeManager::clientProcess(UInt32 in_currTime) {
    currentTimeMs_ = in_currTime;
    for (auto& puff : puffs_) {
        if (!puff.inUse) {
            continue;
        }
        if (in_currTime >= puff.expireTime) {
            puff.renderImage.clearShapeInstance();
            puff.shapeInstance.reset();
            puff.shape.reset();
            puff.resources.reset();
            puff.inUse = false;
            continue;
        }
        if (in_currTime > puff.lastUpdated) {
            const Flt32 dt = static_cast<Flt32>(in_currTime - puff.lastUpdated) * 0.001f;
            puff.position += puff.velocity * dt;
            puff.renderImage.transform.p = puff.position;
            if (puff.shapeInstance && puff.threadIndex) {
                puff.shapeInstance->advanceThread(*puff.threadIndex, static_cast<Flt64>(dt));
            }
            puff.lastUpdated = in_currTime;
        }
    }
    updateBoundingBox();
}

void SmokeManager::updateBoundingBox() noexcept {
    Box3F box{Point3F{1e10f, 1e10f, 1e10f}, Point3F{-1e10f, -1e10f, -1e10f}};
    bool any = false;
    for (const auto& puff : puffs_) {
        if (!puff.inUse) {
            continue;
        }
        any = true;
        const Flt32 rad = (puff.shape && puff.shape->radius > 0.0f) ? puff.shape->radius : 1.0f;
        const Point3F ext{rad, rad, rad};
        box.fMin.setMin(puff.position - ext);
        box.fMax.setMax(puff.position + ext);
    }
    if (!any) {
        box = Box3F{Point3F{-1.0f, -1.0f, -1.0f}, Point3F{1.0f, 1.0f, 1.0f}};
    }
    setBoundingBox(box);
}

std::size_t SmokeManager::activePuffCount() const noexcept {
    std::size_t count = 0;
    for (const auto& puff : puffs_) {
        if (puff.inUse) {
            ++count;
        }
    }
    return count;
}

bool SmokeManager::processQuery(sim::SimQuery& query) {
    if (auto* shapeQ = dynamic_cast<world::WorldShapeQuery*>(&query)) {
        bool provided = false;
        for (auto& puff : puffs_) {
            if (!puff.inUse || !puff.shapeInstance) {
                continue;
            }
            if (!puff.resources) {
                world::TsShapeResources resources;
                resources.shape = puff.shape;
                if (puff.shape->materials) {
                    auto* sr = getManager() ? sim::SimResource::find(*getManager()) : nullptr;
                    if (!sr) {
                        continue;
                    }
                    auto materials =
                        world::TsMaterialResources::load(sr->get(), *puff.shape->materials);
                    if (!materials) {
                        continue;
                    }
                    resources.materials = std::move(*materials);
                }
                puff.resources = std::move(resources);
            }
            if (const auto poseOpt = puff.shapeInstance->pose()) {
                world::WorldShapeImage img{};
                img.resources = *puff.resources;
                img.pose = *poseOpt;
                img.shapeToWorld = puff.renderImage.transform;
                if (shapeQ->cameraPosition) {
                    auto direction = puff.position - *shapeQ->cameraPosition;
                    if (direction.len() > 0) {
                        direction.normalize();
                        if (const auto facing =
                                world::faceDirection(direction, std::nullopt, puff.spin)) {
                            img.shapeToWorld = *facing;
                        }
                    }
                }
                img.shapeToWorld.p = puff.position;
                img.shapeToWorld.flags |= RMat3F::Matrix_HasTranslation;
                const auto& root = puff.shape->transforms[puff.shape->nodes[0].defaultTransform];
                TMat3F placed;
                m_mul(TMat3F(root.rotation, root.translation), img.shapeToWorld, placed);
                img.shapeToWorld = placed;
                img.detail = 0;
                img.automaticDetail = true;
                if (!shapeQ->image) {
                    shapeQ->image = std::move(img);
                } else {
                    shapeQ->attachments.push_back(std::move(img));
                }
                provided = true;
            }
        }
        return provided;
    }
    return Parent::processQuery(query);
}

} // namespace opent1::game
