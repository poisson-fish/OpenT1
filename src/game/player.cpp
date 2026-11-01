#include <opent1/game/player.hpp>
#include <opent1/game/player_manager.hpp>
#include <opent1/game/sensor_manager.hpp>
#include <opent1/game/sound.hpp>
#include <algorithm>
#include <cmath>
#include <format>

namespace opent1::game {

Player::Player() {
    setType(SimPlayerObjectType);
    setNetFlags(sim::SimNetObject::Ghostable);
    setCollisionMask(SimTerrainObjectType | SimInteriorObjectType | SimPlayerObjectType |
                     StaticObjectType | VehicleObjectType | MoveableObjectType | MineObjectType |
                     CorpseObjectType | TriggerObjectType | ItemObjectType);
    setMovementFlags(UseFriction | UseElasticity | UseDrag | UseCurrent | AtRest |
                     AlwaysCalcAVelocity);
    setForceMask(~UInt32{0});
    setTimeSlice(0.032f);

    for (auto& img : itemImageList_) {
        img.state = ItemImageEntry::Reset;
        img.itemId = -1;
        img.itemCount = 0;
        img.imageId = -1;
        img.teamId = -1;
        img.delayTime = 0.0f;
        img.fireCount = 0;
        img.triggerDown = false;
        img.ammo = false;
    }
}

Player::~Player() {
    collisionImage_.clearShapeInstance();
}

bool Player::onAdd() {
    if (!Parent::onAdd()) {
        return false;
    }
    if (!isGhost()) {
        addToSet(PlayerSetId);
        addToSet(TargetableSetId);
    } else {
        addToSet(SensorVisibleSetId);
    }
    return true;
}

void Player::onRemove() {
    stopJetSound();
    for (Int32 slot = 0; slot < MaxImageSlots; ++slot) {
        stopImageSound(slot);
    }
    disableSupressionField();
    if (!isGhost()) {
        for (Int32 slot = 0; slot < MaxImageSlots; ++slot) {
            stopImageFire(slot);
        }
        removeFromSet(PlayerSetId);
        removeFromSet(TargetableSetId);
    }
    Parent::onRemove();
}

void Player::disableSupressionField() {
    if (nextSup_) {
        nextSup_->prevSup_ = prevSup_;
    }
    if (prevSup_) {
        prevSup_->nextSup_ = nextSup_;
    }
    nextSup_ = nullptr;
    prevSup_ = nullptr;
}

void Player::setSupressionField(Flt32 distance) {
    if (!isGhost()) {
        disableSupressionField();
        if (distance > 0.0f && getManager()) {
            if (auto* rm =
                    dynamic_cast<SensorManager*>(getManager()->findObject(SensorManagerId))) {
                rm->addPlayerSupressor(this);
            }
        }
        supressionDist_ = distance;
    }
}

bool Player::detectItem(GameBase* obj) {
    if (!obj || fov_ <= 0.0f || !getManager()) {
        return false;
    }
    if (oldFov_ != fov_) {
        cosFov_ = std::cos(fov_ * 0.5f);
        tanFov_ = std::tan(fov_ * 0.5f);
        oldFov_ = fov_;
    }

    GameBase* src = this;
    if (auto* pm = PlayerManager::get(getManager())) {
        if (auto* me = pm->findClient(getOwnerClient())) {
            if (me->controlObject) {
                src = me->controlObject;
            }
        }
    }

    const TMat3F mat = src->getEyeTransform();
    TMat3F inv = mat;
    inv.inverse();

    const Box3F& box = obj->getBoundingBox();
    const Point3F objPos = (box.fMin + box.fMax) * 0.5f;
    Point3F distance{};
    m_mul(objPos, inv, distance);

    const Flt32 rDistance = distance.len();
    if (rDistance < 1.0f) {
        return true;
    }

    const Flt32 cosTheta = distance.y / rDistance;
    if (cosTheta >= cosFov_) {
        const Flt32 size = std::max(box.fMax.z - box.fMin.z, box.fMax.x - box.fMin.x);
        const Flt32 pSize = size / (rDistance * std::max(1e-5f, tanFov_));
        if ((pSize * (1.0f - detectHazeFactor_ * obj->getHaze(mat.p))) > detectMinSize_) {
            auto* root =
                dynamic_cast<sim::SimContainer*>(getManager()->findObject(sim::RootContainerId));
            if (!root) {
                return true;
            }
            sim::SimContainerQuery query{};
            query.id = src->getId();
            query.type = ~UInt32{0};
            query.mask = SimTerrainObjectType | SimInteriorObjectType | StaticObjectType |
                         VehicleObjectType | MoveableObjectType;
            query.detail = -1;
            query.box.fMin = mat.p;
            query.box.fMax = objPos;

            sim::SimCollisionInfo info{};
            root->findLOS(query, info, sim::SimCollisionImageQuery::Normal);
            sim::SimObject* hitObj = info.object.resolve(*getManager());
            return !hitObj || hitObj == obj;
        }
    }
    return false;
}

void Player::onDeleteNotify(sim::SimObject& object) {
    if (&object == mount_) {
        mount_ = nullptr;
        mountPoint_ = 0;
        setMaskBits(MountMask);
    }
    Parent::onDeleteNotify(object);
}

bool Player::processEvent(const sim::SimEvent& event) {
    return Parent::processEvent(event);
}

bool Player::processQuery(sim::SimQuery& query) {
    if (auto* shapes = dynamic_cast<world::WorldShapeQuery*>(&query)) {
        reSkin();
        appendMountedImages(*shapes);
        appendJetFlame(*shapes);
        (void)Parent::processQuery(query);
        if (isFirstPersonView()) {
            shapes->image.reset();
        }
        if (!isFirstPersonView()) {
            if (shapes->image) {
                if (auto pose = animationPose()) {
                    shapes->image->pose = std::move(*pose);
                }
                const auto skin = std::string(getSkinBase());
                if (!skinMaterials_ || renderedSkin_ != skin) {
                    auto* dbm = resolveDataBlockManager();
                    if (dbm && dbm->resources() && playerData_ &&
                        shapes->image->resources.shape->materials) {
                        skinMaterials_ = world::TsMaterialResources::loadSkin(
                            *dbm->resources(),
                            *shapes->image->resources.shape->materials,
                            skin,
                            playerData_->fileName);
                        if (skinMaterials_) {
                            renderedSkin_ = skin;
                        }
                    }
                }
                if (skinMaterials_) {
                    if (auto materials = damageMaterials(*skinMaterials_)) {
                        shapes->image->resources.materials = std::move(*materials);
                    }
                }
            }
        }
        return shapes->image.has_value() || !shapes->attachments.empty();
    }
    if (query.type == sim::SimCollisionImageQuery{}.type) {
        if (auto* ciq = dynamic_cast<sim::SimCollisionImageQuery*>(&query)) {
            if (auto img = getSelfImage()) {
                ciq->images.push_back(std::move(img));
                return true;
            }
        }
    }
    return Parent::processQuery(query);
}

bool Player::initResources(GameBaseData* inData) {
    if (!Parent::initResources(inData)) {
        return false;
    }
    playerData_ = dynamic_cast<PlayerData*>(getDatPtr());
    if (!playerData_) {
        return false;
    }

    const Flt32 halfW = (playerData_->boxWidth > 0.0f) ? playerData_->boxWidth : 0.6f;
    const Flt32 halfD = (playerData_->boxDepth > 0.0f) ? playerData_->boxDepth : 0.6f;
    const Flt32 normH = (playerData_->boxNormalHeight > 0.0f) ? playerData_->boxNormalHeight : 2.4f;
    const Flt32 crouchH =
        (playerData_->boxCrouchHeight > 0.0f) ? playerData_->boxCrouchHeight : 1.5f;

    collisionImage_.bbox.fMin.set(-halfW, -halfD, 0.0f);
    collisionImage_.bbox.fMax.set(halfW, halfD, normH);
    collisionImage_.crouchBox = collisionImage_.bbox;
    collisionImage_.crouchBox.fMax.z = crouching_ ? crouchH : normH;
    collisionImage_.sphere.radius =
        (collisionImage_.bbox.fMax.z - collisionImage_.bbox.fMin.z) * 0.5f;
    collisionImage_.sphere.center.set(0.0f, 0.0f, collisionImage_.sphere.radius);
    collisionImage_.collisionDetail = 0;
    if (shapeInstance()) {
        collisionImage_.setShapeInstance(shapeInstance(), 0, retainedShapeInstance());
    }

    setLocalBoundingBox(collisionImage_.bbox);
    if (playerData_->mass > 0.0f) {
        setMass(playerData_->mass);
    }
    if (playerData_->drag >= 0.0f) {
        setDrag(playerData_->drag);
    }
    if (playerData_->density > 0.0f) {
        setDensity(playerData_->density);
    }
    updateImageMass();
    animationThread_.reset();
    skinMaterials_.reset();
    lookThread_.reset();
    flameShape_.reset();
    flameThread_.reset();
    flameResources_.reset();
    if (isGhost() && !playerData_->flameShapeName.empty()) {
        auto* dbm = resolveDataBlockManager();
        auto* resources = dbm ? dbm->resources() : nullptr;
        if (!resources && getManager()) {
            if (auto* simResources = sim::SimResource::find(*getManager())) {
                resources = &simResources->get();
            }
        }
        auto name = playerData_->flameShapeName;
        if (!name.ends_with(".dts") && !name.ends_with(".DTS")) {
            name += ".dts";
        }
        const auto shape =
            resources ? resources->load<world::TsShape>(name) : Resource<world::TsShape>{};
        if (shape) {
            flameShape_ = std::make_shared<world::TsShapeInstance>(shape);
            for (std::size_t i = 0; i < shape->sequences.size(); ++i) {
                if (shape->names[shape->sequences[i].name] == "activation") {
                    flameThread_ = flameShape_->addThread(Int32(i));
                    break;
                }
            }
        }
    }
    updateAnimation(0);
    return true;
}

std::shared_ptr<sim::SimCollisionImage> Player::getSelfImage() {
    collisionImage_.transform = getTransform();
    if (shapeInstance()) {
        collisionImage_.setShapeInstance(shapeInstance(), 0, retainedShapeInstance());
    }
    if (!sharedCollisionImage_) {
        sharedCollisionImage_ = std::make_shared<PlayerCollisionImage>(collisionImage_);
    } else {
        *sharedCollisionImage_ = collisionImage_;
    }
    return sharedCollisionImage_;
}

bool Player::processCollision(MovementInfo& info) {
    if (!playerData_) {
        return Parent::processCollision(info);
    }
    constexpr UInt32 ignoreMask = CorpseObjectType | MineObjectType | ItemObjectType;
    constexpr UInt32 stepMask = SimTerrainObjectType | SimInteriorObjectType | MoveableObjectType;

    struct Surface {
        const CollisionSurface* surface;
        sim::SimObject* object;
        Point3F normal;
        Flt32 verticalDot, stepHeight;
        sim::SimCollisionMaterial material;
    };

    std::vector<Surface> surfaces;
    auto vertical = getForce(sim::MovementGravityForce).value_or(Point3F{0, 0, -1});
    if (vertical.len() < .0001f) {
        vertical = {0, 0, -1};
    }
    vertical.normalize();
    vertical *= -1;
    for (const auto& collision : info.collisionList) {
        auto* object = getManager() ? collision.object.resolve(*getManager()) : nullptr;
        if (object && object == mount_) {
            continue;
        }
        info.collidedList.push_back(collision.object);
        if (object && (object->getType() & ignoreMask)) {
            continue;
        }
        for (const auto& surface : collision.surfaces.surfaces) {
            Point3F normal;
            m_mul(surface.normal, static_cast<const RMat3F&>(collision.surfaces.tWorld), normal);
            surfaces.push_back({&surface,
                                object,
                                normal,
                                m_dot(normal, vertical),
                                collision.surfaces.stepVector.z,
                                collisionMaterial(info, collision, surface)});
        }
    }
    if (info.retryCount == 0) {
        Flt32 flattest = 0;
        for (const auto& s : surfaces) {
            if (s.verticalDot > flattest && s.material.surfaceType >= 0) {
                surfaceType_ = s.material.surfaceType;
                flattest = s.verticalDot;
            }
        }
    }
    const auto collide = [&](const Surface& surface) {
        setContact(true);
        if (surface.surface->distance < 0) {
            return false;
        }
        auto velocity = getLinearVelocity();
        const Flt32 dot = m_dot(velocity, surface.normal);
        if (dot >= 0) {
            return false;
        }
        if (surface.normal.z > playerData_->jumpSurfaceMinDot &&
            (surface.normal.z > jumpSurfaceNormal_.z || jumpSurfaceLastContact_ > 1)) {
            jumpSurfaceNormal_ = surface.normal;
            jumpSurfaceLastContact_ = 0;
        }
        lastImpactSpeed_ = std::max(lastImpactSpeed_, -dot);
        if (!isGhost() && -dot > playerData_->minDamageSpeed) {
            applyDamage(0,
                        playerData_->damageScale * (-dot - playerData_->minDamageSpeed),
                        {},
                        {},
                        {},
                        getOwnerClient());
        }
        if (-dot > playerData_->minDamageSpeed && !dead_ && currentAnimation_ != ANIM_LAND) {
            setAnimation(ANIM_LAND);
        }
        velocity += surface.normal * (-dot + .0001f);
        if (surface.verticalDot > 0) {
            const Flt32 traction = surface.verticalDot * surface.material.friction;
            traction_ += traction;
            if ((movementFlags() & UseFriction) && velocity.len() < traction * .75f) {
                velocity = {};
            }
            if (surface.verticalDot > .985f && surface.object &&
                (surface.object->getType() & MoveableObjectType)) {
                if (auto* platform = dynamic_cast<sim::SimMovement*>(surface.object)) {
                    addContact(*platform);
                }
            }
        }
        setLinearVelocity(velocity);
        return true;
    };
    if (!mount_ && surfaces.size() > 1) {
        const bool blocked = std::ranges::any_of(surfaces, [&](const auto& s) {
            return s.object && (s.object->getType() & stepMask) && s.verticalDot <= .707f;
        });
        const Surface* step = nullptr;
        Flt32 distance = 0;
        if (blocked) {
            for (const auto& s : surfaces) {
                if (!s.object || !(s.object->getType() & stepMask) || s.verticalDot < .707f) {
                    continue;
                }
                distance =
                    std::max(distance,
                             s.stepHeight < 0 ? collisionImage_.sphere.radius - s.surface->distance
                                              : s.stepHeight);
                if (!step || s.verticalDot >= step->verticalDot) {
                    step = &s;
                }
            }
        }
        if (step && distance <= collisionImage_.sphere.radius * .6f) {
            MovementInfo test;
            test.image = info.image;
            test.imageList = info.imageList;
            auto stepped = info.image ? info.image->transform : info.transform;
            stepped.p.z += distance + .001f;
            if (testPosition(stepped, test)) {
                collide(*step);
                info.transform = info.target = stepped;
                info.target.p += getLinearVelocity() * (getTimeSlice() - info.collisionTime);
                return true;
            }
        }
    }
    bool hit = false;
    for (const auto& surface : surfaces) {
        hit = collide(surface) || hit;
    }
    if (hit) {
        info.target.p = getLinearVelocity() * (getTimeSlice() - info.startTime) + info.transform.p;
    }
    return hit;
}

void Player::setPlayerControl(bool ctrl) {
    hasFocus_ = ctrl;
}

bool Player::isFirstPersonView() const noexcept {
    // The authoritative muzzle uses the fixed first-person torso override,
    // independent of the local client's camera or movement animation.
    if (!isGhost()) {
        return true;
    }
    if (!hasFocus_) {
        return false;
    }
    if (auto* mgr = getManager()) {
        if (const auto* psc = dynamic_cast<const PlayerPSC*>(mgr->findObject(PlayerPSCId))) {
            return psc->isFirstPerson() && psc->camDist() <= 0.f;
        }
    }
    return true;
}

void Player::setCrouching(bool newCrouch) {
    crouching_ = newCrouch;
    if (playerData_) {
        const Flt32 h = crouching_ ? playerData_->boxCrouchHeight : playerData_->boxNormalHeight;
        if (h > 0.0f) {
            collisionImage_.crouchBox.fMax.z = h;
        }
    }
}

void Player::setDead(bool dead) {
    if (dead_ == dead) {
        return;
    }
    dead_ = dead;
    if (dead_) {
        setType(CorpseObjectType);
        jetting_ = false;
        removeForce(TribesJetForce);
        for (Int32 i = 0; i < MaxImageSlots; ++i) {
            setImageTriggerUp(i);
        }
    } else {
        setType(SimPlayerObjectType);
    }
    setMaskBits(static_cast<UInt32>(DamageMask) | static_cast<UInt32>(AnimationMask));
}

void Player::setViewPitch(Flt32 pitch) noexcept {
    viewPitch_ = std::clamp(pitch, -kMaxPitch, kMaxPitch);
}

void Player::applyDamage(Int32 type,
                         Flt32 value,
                         const Point3F& pos,
                         const Point3F& vec,
                         const Point3F& mom,
                         sim::SimObjectId objectId) {
    if (isGhost()) {
        return;
    }
    auto* console = resolveConsole();
    if (!console) {
        return;
    }
    const auto callback = scriptName("onDamage");
    if (callback.empty()) {
        return;
    }
    std::string_view vertical, quadrant;
    getDamageLocation(pos, vertical, quadrant);
    const std::array<std::string, 9> args{scriptThis(),
                                          std::to_string(type),
                                          std::format("{:g}", value),
                                          std::format("{:g} {:g} {:g}", pos.x, pos.y, pos.z),
                                          std::format("{:g} {:g} {:g}", vec.x, vec.y, vec.z),
                                          std::format("{:g} {:g} {:g}", mom.x, mom.y, mom.z),
                                          std::string(vertical),
                                          std::string(quadrant),
                                          std::to_string(static_cast<Int32>(objectId))};
    (void)console->execute(callback, args);
}

void Player::setDamageLevel(Flt32 level) {
    Parent::setDamageLevel(level);
    const Flt32 maxDmg = playerData_ ? playerData_->maxDamage : 1.0f;
    if (damageLevel_ >= maxDmg && !dead_) {
        setDead(true);
        setAnimation(crouching_ ? ANIM_CROUCH_DIE : ANIM_DIE_TORSO_FRONT);
        if (!isGhost()) {
            if (auto* console = resolveConsole()) {
                const auto callback = scriptName("onKilled");
                if (!callback.empty()) {
                    const std::array<std::string, 1> args{scriptThis()};
                    (void)console->execute(callback, args);
                }
            }
        }
    } else if (damageLevel_ < maxDmg && dead_) {
        setDead(false);
        setAnimation(ANIM_STAND);
    }
}

void Player::setDamageFlash(Flt32 df) noexcept {
    damageFlash_ = std::clamp(df, 0.0f, 1.0f);
}

void Player::getDamageLocation(const Point3F& pos,
                               std::string_view& outVert,
                               std::string_view& outQuad) const noexcept {
    Point3F relPos{};
    m_mul(pos, getInvTransform(), relPos);

    const Flt32 normH =
        (playerData_ && playerData_->boxNormalHeight > 0.0f) ? playerData_->boxNormalHeight : 2.4f;
    const Flt32 crouchH =
        (playerData_ && playerData_->boxCrouchHeight > 0.0f) ? playerData_->boxCrouchHeight : 1.5f;
    const Flt32 height = crouching_ ? crouchH : normH;

    const Flt32 torsoPct = playerData_ ? (crouching_ ? playerData_->boxCrouchTorsoPercentage
                                                     : playerData_->boxNormalTorsoPercentage)
                                       : 0.3333f;
    const Flt32 headPct = playerData_ ? (crouching_ ? playerData_->boxCrouchHeadPercentage
                                                    : playerData_->boxNormalHeadPercentage)
                                      : 0.6666f;

    const Flt32 torsoZ = height * torsoPct;
    const Flt32 headZ = height * headPct;

    if (relPos.z <= torsoZ) {
        outVert = "legs";
    } else if (relPos.z <= headZ) {
        outVert = "torso";
    } else {
        outVert = "head";
    }

    if (outVert != "head") {
        if (relPos.y >= 0.0f) {
            outQuad = (relPos.x <= 0.0f) ? "front_left" : "front_right";
        } else {
            outQuad = (relPos.x <= 0.0f) ? "back_left" : "back_right";
        }
        return;
    }

    const Box3F& bbox = getBoundingBox();
    const Flt32 backToFront = bbox.len_x();
    const Flt32 leftToRight = bbox.len_y();

    const Flt32 leftPct = playerData_ ? playerData_->boxHeadLeftPercentage : 0.0f;
    const Flt32 rightPct = playerData_ ? playerData_->boxHeadRightPercentage : 1.0f;
    const Flt32 backPct = playerData_ ? playerData_->boxHeadBackPercentage : 0.0f;
    const Flt32 frontPct = playerData_ ? playerData_->boxHeadFrontPercentage : 1.0f;

    const Flt32 backPoint = backToFront * (backPct - 0.5f);
    const Flt32 frontPoint = backToFront * (frontPct - 0.5f);
    const Flt32 leftPoint = leftToRight * (leftPct - 0.5f);
    const Flt32 rightPoint = leftToRight * (rightPct - 0.5f);

    Int32 noctant = 0;
    if (relPos.y < backPoint) {
        noctant += 0;
    } else if (relPos.y <= frontPoint) {
        noctant += 3;
    } else {
        noctant += 6;
    }

    if (relPos.x < leftPoint) {
        noctant += 0;
    } else if (relPos.x <= rightPoint) {
        noctant += 1;
    } else {
        noctant += 2;
    }

    static constexpr std::array<std::string_view, 9> kHeadQuads = {
        "left_back",
        "middle_back",
        "right_back",
        "left_middle",
        "middle_middle",
        "right_middle",
        "left_front",
        "middle_front",
        "right_front",
    };
    outQuad = kHeadQuads[static_cast<std::size_t>(std::clamp(noctant, 0, 8))];
}

std::string Player::getDamageLocation(const Point3F& pos) const {
    std::string_view vert{};
    std::string_view quad{};
    getDamageLocation(pos, vert, quad);
    return std::format("{}_{}", vert, quad);
}

std::string_view Player::getSkinBase() const noexcept {
    if (auto* mgr = getManager()) {
        if (const auto* pm = PlayerManager::get(mgr)) {
            if (const auto* cr = pm->findBaseRep(getLastOwnerClient())) {
                if (!cr->skinBase.empty()) {
                    return cr->skinBase;
                }
            }
            if (const auto* tr = pm->findTeam(getTeam())) {
                if (!tr->skinBase.empty()) {
                    return tr->skinBase;
                }
            }
        }
    }
    return "base";
}

Int32 Player::getDamageSkinLevel(Int32 detailLevel) const noexcept {
    // Legacy ghost damage is already normalized. This port retains absolute
    // player damage for HUD/simulation, so normalize only the skin thresholds.
    const auto maxDamage = playerData_ && playerData_->maxDamage > 0 ? playerData_->maxDamage : 1.f;
    const auto damage = getDamageLevel() / maxDamage;
    if (detailLevel == 1) {
        return damage > .4f ? 2 : 0;
    }
    if (detailLevel == 2) {
        return damage > .75f ? 3 : damage > .5f ? 2 : damage > .25f ? 1 : 0;
    }
    return 0;
}

void Player::setAnimation(UInt32 anim) {
    if (anim >= NUM_ANIMS) {
        return;
    }
    serverAnimation_ = static_cast<Int32>(anim);
    currentAnimation_ = static_cast<Int32>(anim);
    animationPending_ = true;
    if (!isGhost()) {
        setMaskBits(AnimationMask);
    }
}

Int32 Player::pickAnim() const noexcept {
    if (dead_) {
        if (serverAnimation_ >= ANIM_DIE_START && serverAnimation_ <= ANIM_DIE_END) {
            return serverAnimation_;
        }
        return crouching_ ? ANIM_CROUCH_DIE : ANIM_DIE_TORSO_FRONT;
    }
    // The legacy server keeps an idle collision/weapon pose. Locomotion is
    // visual client animation and must not move the authoritative muzzle.
    if (!isGhost()) {
        if (serverAnimation_ >= 0 && serverAnimation_ < NUM_ANIMS) {
            return serverAnimation_;
        }
        return crouching_ && !jetting_ && !falling_ && !mount_ ? ANIM_CROUCH : ANIM_STAND;
    }
    if (mount_) {
        return mount_->getMountPose();
    }
    // Jets take priority even while descending. Ordinary falling uses the
    // aim-enabled fall pose, not the tumble animation's full-body lean.
    if (jetting_) {
        return ANIM_JET;
    }
    if (falling_) {
        return ANIM_FALL;
    }
    if (crouching_) {
        if (std::fabs(forwardAxisMovement_) > 0.01f || std::fabs(sideAxisMovement_) > 0.01f) {
            return ANIM_CROUCH_MOVE;
        }
        return ANIM_CROUCH;
    }
    if (curGuiMode_ != 1) {
        return ANIM_PDA;
    }
    // Takeoff explicitly requests a one-shot. Being airborne alone must not
    // replace it with jump stand (which is absent from the light armor shape).
    if (currentAnimation_ == ANIM_JUMP || currentAnimation_ == ANIM_JUMPRUN) {
        return currentAnimation_;
    }
    if (std::fabs(forwardAxisMovement_) >= std::fabs(sideAxisMovement_)) {
        if (forwardAxisMovement_ > 0.01f) {
            return ANIM_RUN;
        }
        if (forwardAxisMovement_ < -0.01f) {
            return ANIM_BACK;
        }
    } else if (std::fabs(sideAxisMovement_) > 0.01f) {
        return sideAxisMovement_ > 0 ? ANIM_SIDE : ANIM_SIDE_LEFT;
    }
    return ANIM_STAND;
}

void Player::updateAnimation(Flt32 seconds) {
    if (!shapeInstance_ || !playerData_) {
        return;
    }
    const auto& shape = *shapeInstance_->shape();
    const auto sequence = [&](std::string_view name) -> Int32 {
        for (std::size_t i = 0; i < shape.sequences.size(); ++i) {
            const auto index = shape.sequences[i].name;
            if (index >= 0 && std::size_t(index) < shape.names.size() &&
                shape.names[index] == name) {
                return Int32(i);
            }
        }
        return -1;
    };
    if (currentAnimation_ == ANIM_JUMP || currentAnimation_ == ANIM_JUMPRUN) {
        const auto& jump = playerData_->animData[std::size_t(currentAnimation_)];
        const auto jumpSeq = sequence(jump.name);
        bool finished = false;
        if (!animationPending_ && animationThread_) {
            const auto& thread = shapeInstance_->threads()[*animationThread_];
            finished = thread.sequence == jumpSeq &&
                       (jump.direction < 0 ? thread.position <= .01f : thread.position >= .99f);
        }
        // Release a completed or unavailable takeoff before choosing the next
        // pose. Keep the final jump frame's view flags until this update.
        if (jumpSeq < 0 || finished) {
            currentAnimation_ = -1;
            if (!isGhost()) {
                serverAnimation_ = -1;
            }
        }
    }
    if (!isGhost() && !dead_ && serverAnimation_ >= 0 && !animationPending_ && animationThread_) {
        const auto& explicitAnimation = playerData_->animData[std::size_t(serverAnimation_)];
        const auto& thread = shapeInstance_->threads()[*animationThread_];
        if (explicitAnimation.direction < 0 ? thread.position <= .01f : thread.position >= .99f) {
            serverAnimation_ = currentAnimation_ = -1;
        }
    }
    const auto anim = pickAnim();
    const auto& data = playerData_->animData[std::size_t(anim)];
    auto seq = sequence(data.name);
    if (seq < 0) {
        seq = sequence("root");
    }
    if (seq < 0) {
        return;
    }
    const auto start =
        data.direction < 0 || (!isGhost() && serverAnimation_ < 0 && anim == ANIM_CROUCH) ? 1.f
                                                                                          : 0.f;
    const bool changing =
        animationPending_ || currentAnimation_ != anim ||
        (animationThread_ && shapeInstance_->threads()[*animationThread_].sequence != seq);
    if (isGhost() && getManager() && animationThread_ && changing && data.soundTag >= 0) {
        TSFX::PlayAt(UInt32(data.soundTag), getTransform(), {}, getManager());
    }
    if (!animationThread_) {
        animationThread_ = shapeInstance_->addThread(seq, start);
    } else if (animationPending_ || shapeInstance_->threads()[*animationThread_].sequence != seq ||
               currentAnimation_ != anim) {
        shapeInstance_->setSequence(*animationThread_, seq, start);
    }
    currentAnimation_ = anim;
    animationPending_ = false;
    if (animationThread_) {
        shapeInstance_->setTimeScale(*animationThread_, data.direction < 0 ? -1.f : 1.f);
        if (isGhost() || dead_ || serverAnimation_ >= 0) {
            shapeInstance_->advanceThread(*animationThread_, seconds);
        }
    }
    auto look = sequence(crouching_ ? "crouch looks" : "looks");
    if (look < 0) {
        look = sequence("looks");
    }
    if (look >= 0) {
        const auto position = std::clamp(.5f - viewPitch_ / kMaxPitch * .5f, 0.f, 1.f);
        if (!lookThread_) {
            lookThread_ = shapeInstance_->addThread(look, position);
        }
        if (lookThread_) {
            shapeInstance_->setSequence(*lookThread_, look, position);
            const auto flag = isFirstPersonView() ? 1 : (hasPlayerControl() ? 2 : 4);
            shapeInstance_->setPriority(*lookThread_, (data.viewFlags & flag) ? -1 : 5000);
        }
    }
    // Animation root motion is used to choose playback speed, never to move the
    // collision body a second time; locomotion belongs to updateMove.
    shapeInstance_->resetRootMotion();
    if (flameShape_ && flameThread_) {
        if (currentAnimation_ == ANIM_JET && jetting_ && !dead_ && !mount_) {
            flameShape_->advanceThread(*flameThread_, seconds);
        } else {
            flameShape_->setSequence(
                *flameThread_, flameShape_->threads()[*flameThread_].sequence, 0);
        }
    }
}

std::optional<world::TsShapePose> Player::animationPose() const {
    auto pose = shapeInstance_ ? shapeInstance_->pose() : std::nullopt;
    if (!pose || !playerData_ || dead_) {
        return pose;
    }
    const auto flag = isFirstPersonView() ? 1 : (hasPlayerControl() ? 2 : 4);
    if (!(playerData_->animData[std::size_t(pickAnim())].viewFlags & flag)) {
        return pose;
    }
    const auto& shape = *pose->shape();
    float rootZ = 0;
    for (std::size_t i = 0; i < shape.nodes.size(); ++i) {
        const auto name = shape.nodes[i].name;
        if (name >= 0 && std::size_t(name) < shape.names.size() &&
            shape.names[name] == "dummyalways root") {
            rootZ = pose->nodeTransforms()[i].p.z;
        }
    }
    for (std::size_t i = 0; i < shape.nodes.size(); ++i) {
        const auto& node = shape.nodes[i];
        if (node.name < 0 || std::size_t(node.name) >= shape.names.size() ||
            !shape.names[node.name].starts_with("lowerback") || node.parent < 0) {
            continue;
        }
        const auto position =
            isFirstPersonView() ? Point3F{0, 0, rootZ} : pose->nodeTransforms()[node.parent].p;
        (void)pose->overrideParentTransform(Int32(i),
                                            TMat3F(EulerF{0, 0, 1.57079632679f}, position));
    }
    return pose;
}

void Player::appendJetFlame(world::WorldShapeQuery& query) {
    if (!jetting_ || dead_ || mount_ || isFirstPersonView() || !flameShape_) {
        return;
    }
    if (!flameResources_) {
        world::TsShapeResources resources;
        resources.shape = flameShape_->shape();
        if (resources.shape->materials) {
            auto* dbm = resolveDataBlockManager();
            if (!dbm || !dbm->resources()) {
                return;
            }
            auto materials =
                world::TsMaterialResources::load(*dbm->resources(), *resources.shape->materials);
            if (!materials) {
                return;
            }
            resources.materials = std::move(*materials);
        }
        flameResources_ = std::move(resources);
    }
    const auto body = animationPose(), flame = flameShape_->pose();
    if (!body || !flame) {
        return;
    }
    const auto& shape = *body->shape();
    for (std::size_t i = 0; i < shape.nodes.size(); ++i) {
        const auto name = shape.nodes[i].name;
        if (name < 0 || std::size_t(name) >= shape.names.size() ||
            !shape.names[name].starts_with("dummy lowback")) {
            continue;
        }
        TMat3F mount, transform;
        m_mul(body->nodeTransforms()[i], getTransform(), mount);
        const auto& root =
            flameResources_->shape->transforms[flameResources_->shape->nodes[0].defaultTransform];
        m_mul(TMat3F(root.rotation, root.translation), mount, transform);
        transform.normalize();
        transform.flags = RMat3F::Matrix_HasRotation | RMat3F::Matrix_HasTranslation;
        world::WorldShapeImage image;
        image.resources = *flameResources_;
        image.pose = *flame;
        image.shapeToWorld = transform;
        image.automaticDetail = true;
        query.attachments.push_back(std::move(image));
        return;
    }
}

void Player::appendMountedImages(world::WorldShapeQuery& query) {
    if (!shapeInstance_ || dead_) {
        return;
    }
    for (Int32 index = 0; index < MaxImageSlots; ++index) {
        auto& slot = itemImageList_[index];
        const auto* data = getImageData(slot.imageId);
        if (!data || !data->shape || (isFirstPersonView() && !data->firstPerson)) {
            continue;
        }
        if (slot.renderedImageId != slot.imageId || !slot.shape) {
            slot.shape = std::make_shared<world::TsShapeInstance>(data->shape);
            world::TsShapeResources resources;
            resources.shape = data->shape;
            auto* dbm = resolveDataBlockManager();
            if (resources.shape->materials) {
                if (!dbm || !dbm->resources()) {
                    continue;
                }
                auto materials = world::TsMaterialResources::load(*dbm->resources(),
                                                                  *resources.shape->materials);
                if (!materials) {
                    continue;
                }
                resources.materials = std::move(*materials);
            }
            slot.resources = std::move(resources);
            slot.renderedImageId = slot.imageId;
            slot.renderedState = -1;
            slot.animationThread.reset();
            slot.ambientThread.reset();
            slot.spinThread.reset();
        }
        if (!slot.resources) {
            continue;
        }
        updateImageAnimation(slot, 0);
        TMat3F local, transform;
        if (!getImageTransform(index, &local)) {
            continue;
        }
        m_mul(local, getTransform(), transform);
        const auto pose = slot.shape->pose();
        if (!pose) {
            continue;
        }
        // ShapeInstance::render pushes fRootDeltaTransform, initialized from
        // node zero's default transform. Only the player body clears it.
        const auto& root = data->shape->transforms[data->shape->nodes[0].defaultTransform];
        TMat3F placed;
        m_mul(TMat3F(root.rotation, root.translation), transform, placed);
        transform = placed;
        world::WorldShapeImage image;
        image.resources = *slot.resources;
        image.pose = *pose;
        // Packed Quat16 joints accumulate small scale/shear errors. The scene
        // and inverse-light transforms require a rigid attachment placement.
        transform.normalize();
        transform.flags = RMat3F::Matrix_HasRotation | RMat3F::Matrix_HasTranslation;
        image.shapeToWorld = transform;
        image.automaticDetail = true;
        image.firstPerson = isFirstPersonView();
        query.attachments.push_back(std::move(image));
    }
}

bool Player::getImageTransform(Int32 slot, TMat3F* transform) const {
    if (!transform || slot < 0 || slot >= MaxImageSlots) {
        return false;
    }
    const auto* data = getImageData(itemImageList_[slot].imageId);
    const auto pose = animationPose();
    constexpr std::array nodes{"dummy hand", "dummy unused", "dummy midback", "dummy lowback"};
    if (!data || !pose || data->mountPoint < 0 || std::size_t(data->mountPoint) >= nodes.size()) {
        return false;
    }
    const auto& shape = *pose->shape();
    for (std::size_t i = 0; i < shape.nodes.size(); ++i) {
        const auto name = shape.nodes[i].name;
        if (name < 0 || std::size_t(name) >= shape.names.size() ||
            !shape.names[name].starts_with(nodes[data->mountPoint])) {
            continue;
        }
        auto mount = pose->nodeTransforms()[i];
        const auto view = isFirstPersonView() ? 1 : (hasPlayerControl() ? 2 : 4);
        if (!dead_ && data->mountPoint <= 1 && playerData_ &&
            (playerData_->animData[std::size_t(pickAnim())].viewFlags & view)) {
            mount.set(EulerF{viewPitch_, 0, 0}, mount.p);
        }
        const TMat3F offset(
            EulerF{data->mountRotation.x, data->mountRotation.y, data->mountRotation.z},
            data->mountOffset);
        m_mul(offset, mount, *transform);
        return true;
    }
    transform->identity();
    return false;
}

bool Player::getMuzzleTransform(Int32 slot, TMat3F* transform) const {
    if (!transform) {
        return false;
    }
    TMat3F image;
    if (!getImageTransform(slot, &image)) {
        transform->identity();
        return false;
    }
    const auto& entry = itemImageList_[slot];
    const auto* data = getImageData(entry.imageId);
    const auto pose = entry.shape   ? entry.shape->pose()
                      : data->shape ? world::TsShapePose::sample(data->shape)
                                    : std::nullopt;
    if (pose) {
        const auto& shape = *pose->shape();
        // Pose nodes exclude the DTS root. Mounted-image rendering applies
        // that authored placement separately; the muzzle must use it too.
        const auto& root = shape.transforms[shape.nodes[0].defaultTransform];
        TMat3F placed;
        m_mul(TMat3F(root.rotation, root.translation), image, placed);
        image = placed;
        for (std::size_t i = 0; i < shape.nodes.size(); ++i) {
            const auto name = shape.nodes[i].name;
            if (name >= 0 && std::size_t(name) < shape.names.size() &&
                shape.names[name] == "dummyalways muzzle") {
                TMat3F muzzle;
                m_mul(pose->nodeTransforms()[i], image, muzzle);
                image = muzzle;
                break;
            }
        }
    }
    // Player muzzle transforms are local, as expected by aimed fire and the
    // projectile interpolation paths inherited from the original game.
    *transform = image;
    return true;
}

void Player::updateImageAnimation(ItemImageEntry& image, Flt32 seconds) {
    if (!image.shape) {
        return;
    }
    const auto& shape = *image.shape->shape();
    const auto sequence = [&](std::string_view name) -> Int32 {
        for (std::size_t i = 0; i < shape.sequences.size(); ++i) {
            const auto index = shape.sequences[i].name;
            if (index >= 0 && std::size_t(index) < shape.names.size() &&
                shape.names[index] == name) {
                return Int32(i);
            }
        }
        return -1;
    };
    if (image.renderedState < 0) {
        const auto activation = sequence("activation"), ambient = sequence("ambient"),
                   spin = sequence("spin");
        if (activation >= 0) {
            image.animationThread = image.shape->addThread(
                activation, image.state == ItemImageEntry::Activate ? 0.f : 1.f);
        }
        if (ambient >= 0) {
            image.ambientThread = image.shape->addThread(ambient);
        }
        if (spin >= 0) {
            image.spinThread = image.shape->addThread(spin);
        }
    }
    if (image.renderedState != image.state) {
        // Legacy setImageState stops cyclic fire/ready tracks even when the
        // destination (SpinDown/Idle) has no replacement animation.
        if (image.animationThread) {
            const auto seq = image.shape->threads()[*image.animationThread].sequence;
            if (shape.sequences[seq].cyclic) {
                image.shape->setSequence(*image.animationThread, seq, 0.f);
                image.shape->setTimeScale(*image.animationThread, 0.f);
            }
        }
        std::string_view name;
        switch (image.state) {
            case ItemImageEntry::Activate:
                name = "activation";
                break;
            case ItemImageEntry::Fire:
                name = "fire";
                break;
            case ItemImageEntry::Reload:
                name = "reload";
                break;
            case ItemImageEntry::Ready:
                name = "ready";
                break;
            case ItemImageEntry::NoAmmo:
                name = "noammo";
                break;
            default:
                break;
        }
        const auto seq = sequence(name);
        if (seq >= 0) {
            if (!image.animationThread) {
                image.animationThread = image.shape->addThread(seq);
            } else {
                image.shape->setSequence(*image.animationThread, seq);
            }
            image.shape->setTimeScale(*image.animationThread, 1.f);
        }
        image.renderedState = image.state;
    }
    if (image.spinThread) {
        const auto* data = getImageData(image.imageId);
        float speed =
            image.state == ItemImageEntry::Ready || image.state == ItemImageEntry::Fire ? 1.f : 0.f;
        if (data && image.state == ItemImageEntry::SpinUp) {
            speed = data->spinUpTime > 0 ? 1.f - image.delayTime / data->spinUpTime : 1.f;
        }
        if (data && image.state == ItemImageEntry::SpinDown) {
            speed = data->spinDownTime > 0 ? image.delayTime / data->spinDownTime : 0.f;
        }
        image.shape->setTimeScale(*image.spinThread, std::clamp(speed, 0.f, 1.f));
    }
    image.shape->advance(seconds);
    image.shape->resetRootMotion();
}

void Player::setMountObject(GameBase* obj, Int32 mountPoint) {
    if (mount_ == obj && mountPoint_ == mountPoint) {
        return;
    }
    if (mount_ && mount_ != obj) {
        clearNotify(*mount_);
    }
    mount_ = obj;
    mountPoint_ = mountPoint;
    if (mount_) {
        deleteNotify(*mount_);
    }
    if (!isGhost()) {
        setMaskBits(MountMask);
    }
}

const TMat3F& Player::getEyeTransform() const {
    RMat3F pitchMat{};
    pitchMat.set(EulerF{viewPitch_, 0.0f, 0.0f});
    TMat3F localEye{};
    localEye.identity();
    static_cast<RMat3F&>(localEye) = pitchMat;
    const Flt32 eyeZ =
        playerData_
            ? ((crouching_ ? playerData_->boxCrouchHeight : playerData_->boxNormalHeight) * 0.85f)
            : 1.8f;
    localEye.p.set(0.0f, 0.0f, eyeZ);
    if (shapeInstance_) {
        if (const auto pose = animationPose()) {
            const auto& shape = *pose->shape();
            for (std::size_t i = 0; i < shape.nodes.size(); ++i) {
                const auto name = shape.nodes[i].name;
                if (name >= 0 && std::size_t(name) < shape.names.size() &&
                    shape.names[name].starts_with("dummy eye")) {
                    localEye.p = pose->nodeTransforms()[i].p;
                    break;
                }
            }
        }
    }
    m_mul(localEye, getTransform(), eyeTransformCache_);
    return eyeTransformCache_;
}

TMat3F Player::getLOSTransform() const noexcept {
    return getEyeTransform();
}

void Player::getCameraTransform(Flt32 camDist, TMat3F* transform) const {
    if (!transform) {
        return;
    }
    *transform = getEyeTransform();
    if (camDist >= .01f) {
        // The chase camera has its own centered anchor. Following the animated
        // eye node makes the entire view sway with the player's torso/weapon.
        Point3F anchor{0, 0, playerData_ ? playerData_->boxNormalHeight * .85f : 1.8f};
        if (shapeInstance_) {
            if (const auto pose = shapeInstance_->pose()) {
                const auto& shape = *pose->shape();
                for (std::size_t i = 0; i < shape.nodes.size(); ++i) {
                    const auto name = shape.nodes[i].name;
                    if (name >= 0 && std::size_t(name) < shape.names.size() &&
                        shape.names[name] == "dummyalways chasecam") {
                        anchor.z = pose->nodeTransforms()[i].p.z;
                        break;
                    }
                }
            }
        }
        m_mul(TMat3F(EulerF{viewPitch_, 0, 0}, anchor), getTransform(), *transform);
        validateEyePoint(transform, camDist);
    }
}

Persistent::Error
Player::read(StreamIO& sio, Int32 version, Int32 user, Persistent::LoadContext& ctx) {
    return Parent::read(sio, version, user, ctx);
}

Persistent::Error
Player::write(StreamIO& sio, Int32 version, Int32 user, Persistent::StoreContext& ctx) const {
    return Parent::write(sio, version, user, ctx);
}

bool registerPlayerClass(Persistent::ClassRegistry& registry) {
    return registry.registerClass<Player>("Player", PlayerPersTag);
}

bool registerPlayerNetwork(sim::network::Registry& registry) {
    return registry.registerGhost<Player>(static_cast<UInt16>(PlayerPersTag));
}

} // namespace opent1::game
