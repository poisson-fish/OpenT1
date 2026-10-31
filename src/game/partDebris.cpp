#include <opent1/game/part_debris.hpp>

#include <algorithm>
#include <cmath>

namespace opent1::game {

// -----------------------------------------------------------------------------
// PartDebris
// -----------------------------------------------------------------------------

PartDebris::PartDebris(const PartDebrisPiece& part, Int32 id, Flt32 spawnTO)
    : Debris(id, spawnTO, false), piece_(part) {
    debrisMass_ = 250.0f;
    setMass(debrisMass_);
    if (piece_.shapeInstance) {
        shapeInstance_ = piece_.shapeInstance;
        shape_ = shapeInstance_->shape();
        image_.setShapeInstance(shapeInstance_.get(), shapeInstance_);
    }
    initShape();
}

PartDebris::PartDebris(const PartDebrisPiece& part,
                       std::span<const DebrisPartInfo> chooserTable,
                       Flt32 spawnTO)
    : Debris(chooserTable.empty() ? -1 : chooserTable.front().id, spawnTO, false), piece_(part),
      chooser_(chooserTable.begin(), chooserTable.end()) {
    const Point3F span = piece_.bounds.fMax - piece_.bounds.fMin;
    const Flt32 radiusSq = m_dot(span, span) * 0.25f;

    auto& rng = Debris::sharedRandom();
    std::size_t idx = 0;
    if (!chooser_.empty()) {
        const Flt32 firstRadius = chooser_.front().radius;
        while (idx + 1 < chooser_.size() && (radiusSq > chooser_[idx].radius * firstRadius ||
                                             rng.getFloat(0.99f) > chooser_[idx].p)) {
            ++idx;
        }
        const auto& chosen = chooser_[idx];
        datFileId_ = chosen.id;
        debrisMass_ = chosen.mass;
        setMass(std::max(0.01f, debrisMass_));

        if (rng.getFloat(0.99f) >
            DebrisCloud::detailScale * (1.0f - chosen.detailScaleMin) + chosen.detailScaleMin) {
            expire_ = true;
            spawnTimeout_ = -spawnTimeout_;
        }
    } else {
        debrisMass_ = 250.0f;
        setMass(debrisMass_);
    }

    if (piece_.shapeInstance) {
        shapeInstance_ = piece_.shapeInstance;
        shape_ = shapeInstance_->shape();
        image_.setShapeInstance(shapeInstance_.get(), shapeInstance_);
    }
    initShape();
}

void PartDebris::initShape() {
    const Box3F& bounceBox = piece_.bounds;
    const Flt32 rad = 0.5f * (bounceBox.fMax - bounceBox.fMin).len();
    collisionImage_->sphere.radius = rad;
    collisionImage_->sphere.center = (bounceBox.fMin + bounceBox.fMax) * 0.5f;

    Box3F localBox{};
    localBox.fMin = collisionImage_->sphere.center - Point3F{rad, rad, rad};
    localBox.fMax = collisionImage_->sphere.center + Point3F{rad, rad, rad};
    collisionImage_->bbox = localBox;
    setLocalBoundingBox(localBox);

    radii_ = (bounceBox.fMax - bounceBox.fMin) * 0.5f;
    if (radii_.x < 0.5f) {
        radii_.x = 0.5f;
    }
    if (radii_.y < 0.5f) {
        radii_.y = 0.5f;
    }
    if (radii_.z < 0.5f) {
        radii_.z = 0.5f;
    }

    const Box3F worldBox = buildContainerBox(getTransform());
    setBoundingBox(worldBox);
}

bool PartDebris::onSimMessageEvent(const sim::SimMessageEvent& event) {
    if (piece_.objectCount <= 1) {
        return Parent::onSimMessageEvent(event);
    }

    const SimTime now = getManager() ? getManager()->getCurrentTime() : lastUpdateTime_;
    const Flt32 elapsed = now - debrisTime_;
    spawnTimeout_ -= elapsed;
    expire_ = true;

    if (spawnTimeout_ > 0.0f && getManager()) {
        const Flt32 outStrength =
            m_debrisData_ ? m_debrisData_->spawnedDebrisStrength : spawnImpulse_;
        std::unique_ptr<PartDebrisCloud> newCloud;
        if (!chooser_.empty()) {
            newCloud = std::make_unique<PartDebrisCloud>(shapeInstance_,
                                                         getTransform(),
                                                         getTransform().p,
                                                         chooser_,
                                                         getLinearVelocity(),
                                                         outStrength,
                                                         Point3F{0.0f, 0.0f, 0.0f},
                                                         spawnTimeout_);
        } else {
            newCloud = std::make_unique<PartDebrisCloud>(shapeInstance_,
                                                         getTransform(),
                                                         getTransform().p,
                                                         datFileId_,
                                                         getLinearVelocity(),
                                                         outStrength,
                                                         Point3F{0.0f, 0.0f, 0.0f},
                                                         spawnTimeout_);
        }
        newCloud->bindDataBlockManager(resolveDataBlockManager());
        newCloud->setBreakProbability(0.65f);
        if (inCloud_ && parentCloud_) {
            newCloud->setSpawnedFrom(parentCloud_);
        }
        getManager()->addObject(std::move(newCloud));

        if (spawnedExplosionId_ > 0) {
            auto exp = std::make_unique<Explosion>(spawnedExplosionId_);
            exp->bindDataBlockManager(resolveDataBlockManager());
            exp->setPosition(getTransform().p);
            getManager()->addObject(std::move(exp));
        }
    }

    if (getManager() && !inCloud_) {
        deleteObject();
    }
    return true;
}

// -----------------------------------------------------------------------------
// PartDebrisCloud
// -----------------------------------------------------------------------------

PartDebrisCloud::PartDebrisCloud(std::vector<std::unique_ptr<PartDebris>> list)
    : initialList_(std::move(list)) {
    transform_.identity();
    if (!initialList_.empty() && initialList_.front()) {
        transform_.p = initialList_.front()->getTransform().p;
    } else {
        transform_.p.set(0.0f, 0.0f, 0.0f);
    }
    pos_ = transform_.p;
}

PartDebrisCloud::PartDebrisCloud(std::shared_ptr<world::TsShapeInstance> shape,
                                 const TMat3F& transform,
                                 const Point3F& pos,
                                 Int32 id,
                                 const Point3F& vel,
                                 Flt32 outJ,
                                 const Point3F& inJ,
                                 Flt32 spawnTO)
    : transform_(transform), cannibalize_(std::move(shape)), debrisId_(id) {
    pos_ = pos;
    vel_ = vel;
    outwardImpulse_ = outJ;
    incomingImpulse_ = inJ;
    spawnTimeout_ = spawnTO;
}

PartDebrisCloud::PartDebrisCloud(std::shared_ptr<world::TsShapeInstance> shape,
                                 const TMat3F& transform,
                                 const Point3F& pos,
                                 std::span<const DebrisPartInfo> chooser,
                                 const Point3F& vel,
                                 Flt32 outJ,
                                 const Point3F& inJ,
                                 Flt32 spawnTO)
    : transform_(transform), cannibalize_(std::move(shape)), debrisId_(-1),
      chooser_(chooser.begin(), chooser.end()) {
    pos_ = pos;
    vel_ = vel;
    outwardImpulse_ = outJ;
    incomingImpulse_ = inJ;
    spawnTimeout_ = spawnTO;
}

void PartDebrisCloud::setBreakProbability(Flt32 p) noexcept {
    useBreakP_ = true;
    breakP_ = p;
}

std::vector<PartDebrisPiece> PartDebrisCloud::breakShapeIntoParts(
    const std::shared_ptr<world::TsShapeInstance>& shapeInst, bool useBreakP, Flt32 breakP) {
    std::vector<PartDebrisPiece> parts;
    if (!shapeInst || !shapeInst->shape()) {
        return parts;
    }
    const auto& shape = *shapeInst->shape();
    const auto poseOpt = shapeInst->pose();
    const auto nodeTransforms = poseOpt ? poseOpt->nodeTransforms() : std::span<const TMat3F>{};

    auto& rng = Debris::sharedRandom();
    for (const auto& obj : shape.objects) {
        if (useBreakP && rng.getFloat(0.99f) > breakP) {
            continue;
        }
        PartDebrisPiece piece{};
        piece.shapeInstance = shapeInst;
        piece.objectCount = 1;
        piece.rootDeltaTransform.identity();
        if (obj.node >= 0 && static_cast<std::size_t>(obj.node) < nodeTransforms.size()) {
            piece.rootDeltaTransform = nodeTransforms[static_cast<std::size_t>(obj.node)];
        } else {
            piece.rootDeltaTransform.p = obj.offset;
        }
        if (obj.mesh >= 0 && static_cast<std::size_t>(obj.mesh) < shape.meshes.size()) {
            const auto& mesh = shape.meshes[static_cast<std::size_t>(obj.mesh)];
            const Flt32 r = std::max(0.5f, mesh.radius);
            piece.bounds = Box3F{Point3F{-r, -r, -r}, Point3F{r, r, r}};
        } else {
            piece.bounds = shape.bounds;
        }
        parts.push_back(std::move(piece));
    }

    if (parts.empty()) {
        PartDebrisPiece fallback{};
        fallback.shapeInstance = shapeInst;
        fallback.objectCount = 1;
        fallback.rootDeltaTransform.identity();
        fallback.bounds = shape.bounds;
        parts.push_back(std::move(fallback));
    }
    return parts;
}

void PartDebrisCloud::addDebrisList(std::vector<std::unique_ptr<PartDebris>>& list) {
    auto* dbm = resolveDbm();
    for (auto& part : list) {
        if (!part) {
            continue;
        }
        part->bindDataBlockManager(dbm);
        part->finishAddToManager();
        part->initShape();
        addManagedDebris(std::move(part));
    }
    list.clear();
}

void PartDebrisCloud::generateDebris() {
    std::vector<PartDebrisPiece> partList = syntheticPieces_;
    if (partList.empty()) {
        partList = breakShapeIntoParts(cannibalize_, useBreakP_, breakP_);
    }

    auto& rng = Debris::sharedRandom();
    std::vector<std::unique_ptr<PartDebris>> generated;
    generated.reserve(partList.size());

    for (auto& part : partList) {
        TMat3F partTransform{};
        m_mul(part.rootDeltaTransform, transform_, partTransform);
        part.rootDeltaTransform.identity();
        const Point3F center = partTransform.p;

        std::unique_ptr<PartDebris> debris;
        if (!chooser_.empty()) {
            debris = std::make_unique<PartDebris>(part, chooser_, spawnTimeout_);
        } else {
            debris = std::make_unique<PartDebris>(part, debrisId_, spawnTimeout_);
        }

        debris->setPosition(partTransform, true);
        debris->setLinearVelocity(vel_);

        Point3F velVec = center - pos_;
        velVec.normalizef();
        velVec *= outwardImpulse_;
        debris->applyImpulse(velVec);
        debris->applyImpulse(incomingImpulse_);

        Flt32 amag = outwardImpulse_ * 0.005f;
        if (amag > 5.0f) {
            amag = 5.0f;
        }
        const Point3F avel{rng.getFloat(amag) - amag * 0.5f,
                           rng.getFloat(amag) - amag * 0.5f,
                           rng.getFloat(amag) - amag * 0.5f};
        debris->setAngularVelocity(avel);

        generated.push_back(std::move(debris));
    }

    addDebrisList(generated);
}

bool PartDebrisCloud::onAdd() {
    if (!sim::SimContainer::onAdd()) {
        return false;
    }
    setNetFlags(0);

    if (!spawnedFrom_ && getManager()) {
        if (auto* root =
                dynamic_cast<sim::SimContainer*>(getManager()->findObject(sim::RootContainerId))) {
            setContainerMask(0);
            root->addObject(*this);
            setBoundingBox(Box3F{transform_.p, transform_.p});
        }
        addToSet(sim::TimerSetId);
    }

    if (!initialList_.empty()) {
        addDebrisList(initialList_);
    } else {
        generateDebris();
    }

    if (debrisList_.empty() && getManager()) {
        onSimMessageEvent(sim::SimMessageEvent{});
    }
    return true;
}

} // namespace opent1::game
