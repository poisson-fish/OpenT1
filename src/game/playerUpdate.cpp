#include <opent1/game/player.hpp>
#include <opent1/game/player_manager.hpp>
#include <opent1/game/commander_huds.hpp>
#include <opent1/game/decal_manager.hpp>
#include <opent1/game/sound.hpp>
#include <algorithm>
#include <charconv>
#include <cmath>
#include <numbers>

namespace opent1::game {

namespace {

constexpr Flt32 kTwoPi = static_cast<Flt32>(2.0 * std::numbers::pi);
constexpr Flt32 kPi = static_cast<Flt32>(std::numbers::pi);

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

} // namespace

void Player::serverUpdateMove(std::span<const PlayerMove> moves) {
    if (dead_) {
        return;
    }
    for (const auto& move : moves) {
        if (updateDebt_ > 5) {
            break;
        }
        ++updateDebt_;
        if (move.useItem != -1) {
            if (auto* console = resolveConsole(); console && console->isFunction("remoteUseItem")) {
                const std::array<std::string, 2> args{scriptThis(), std::to_string(move.useItem)};
                (void)console->execute("remoteUseItem", args);
            }
        }
        if (lastPlayerMove_.trigger && !move.trigger) {
            setImageTriggerUp(0);
        } else if (!lastPlayerMove_.trigger && move.trigger) {
            setImageTriggerDown(0);
        }
        if (aiControlled_) {
            lastPlayerMove_ = move;
        }
        updateDamage(.032f);
        updateMove(&lastPlayerMove_, true);
        updateAnimation(.032f);
        updateShieldThread(.032f);
        updateScriptThreads(.032f);
        if (!aiControlled_) {
            lastPlayerMove_ = move;
        }
    }
    updateSkipCount_ = 0;
    if (!mount_ || mountPoint_ != 1) {
        setMaskBits(OrientationMask);
    }
}

void Player::serverProcess(UInt32 curTime) {
    // Moves received from the client own movement simulation. Each server tick
    // repays one move of debt; after a timeout we advance with neutral input.
    if (updateDebt_ < -25 || dead_ || (!aiControlled_ && !getControlClient())) {
        PlayerMove emptyMove{};
        updateDamage(.032f);
        updateMove(&emptyMove, true);
        updateAnimation(.032f);
        updateShieldThread(.032f);
        updateScriptThreads(.032f);
        if (!mount_ || mountPoint_ != 1) {
            setMaskBits(OrientationMask);
        }
        lastPlayerMove_ = emptyMove;
        updateSkipCount_ = 0;
    } else {
        ++updateSkipCount_;
        --updateDebt_;
    }
    lastProcessTime_ = curTime;
    updateTimers();
    if (jetting_) {
        lastJetTime_ = curTime;
    }
    const bool freeLook = prefFreeLook != 0;
    if (!freeLook && lastFreeLook_) {
        setViewPitch(0);
        setMaskBits(OrientationMask);
    }
    lastFreeLook_ = freeLook;
    const auto* pm = PlayerManager::get(getManager());
    const auto* client = pm ? pm->findClient(getControlClient()) : nullptr;
    setPDA(!aiControlled_ && (!client || client->curGuiMode != 1));
    if (auto* manager = getManager()) {
        if (const auto* area =
                dynamic_cast<const MissionCenterPos*>(manager->findObject(MissionCenterPosId))) {
            const auto& p = getPos();
            const bool inside = p.x >= area->pos.x && p.x <= area->pos.x + area->extent.x &&
                                p.y >= area->pos.y && p.y <= area->pos.y + area->extent.y;
            if (inside != inWorld_) {
                inWorld_ = inside;
                if (auto* console = resolveConsole()) {
                    const std::array<std::string, 1> args{scriptThis()};
                    (void)console->execute(
                        inside ? "Player::enterMissionArea" : "Player::leaveMissionArea", args);
                }
            }
        }
    }
}

void Player::clientProcess(UInt32 curTime) {
    if (curTime < lastProcessTime_) {
        lastProcessTime_ = curTime;
    }
    const Flt32 animationSeconds =
        curTime >= lastAnimateTime_ ? (curTime - lastAnimateTime_) * .001f : 0.f;
    if (dead_) {
        fallToGround(animationSeconds);
    }
    if (interpDoneTime_) {
        const auto end = std::min(curTime, interpDoneTime_);
        if (lastProcessTime_ < end) {
            stepDelta(Flt32(end - lastProcessTime_));
            lastProcessTime_ = end;
        }
        if (lastProcessTime_ >= interpDoneTime_) {
            interpDoneTime_ = 0;
        }
    }
    auto* psc =
        getManager() ? dynamic_cast<PlayerPSC*>(getManager()->findObject(PlayerPSCId)) : nullptr;
    while (lastProcessTime_ < curTime) {
        const UInt32 endTick = std::min((lastProcessTime_ + 32) & ~UInt32{31}, curTime);
        const UInt32 step = lastProcessTime_ & 31;
        const UInt32 endStep = ((endTick - 1) & 31) + 1;
        if (step) {
            stepDelta(Flt32(endStep - step) / 32.f);
        } else {
            const PlayerMove* move = hasFocus_
                                         ? (psc ? psc->getClientMove(lastProcessTime_) : nullptr)
                                         : &lastPlayerMove_;
            if (move) {
                lastPlayerMove_ = *move;
                updateMove(move, false);
            } else {
                deltaPos_ = {};
                deltaRot_ = deltaPitch_ = 0;
            }
            if (endStep != 32) {
                stepDelta((Flt32(endStep) - 32.f) / 32.f);
            }
        }
        lastProcessTime_ = endTick;
    }
    updateAnimation(animationSeconds);
    for (auto& image : itemImageList_) {
        updateImageAnimation(image, animationSeconds);
    }
    updateClientEffects();
    lastAnimateTime_ = curTime;
}

void Player::stopJetSound() {
    if (jetSound_ && getManager()) {
        if (auto* audio =
                dynamic_cast<SoundPlayback*>(getManager()->findObject("GameSoundPlayback"));
            audio && audio->stop) {
            audio->stop(jetSound_);
        }
    }
    jetSound_ = 0;
}

void Player::updateClientEffects() {
    if (!playerData_) {
        return;
    }
    if (surfaceType_ >= 0 && surfaceType_ < NUM_FOOTSOUNDS && !mount_ && lastContactCount_ < 8 &&
        shapeInstance_) {
        for (const auto& trigger : shapeInstance_->triggers()) {
            if (!animationThread_ || trigger.thread != *animationThread_ ||
                (trigger.value != 1 && trigger.value != 2)) {
                continue;
            }
            const bool right = trigger.value == 1;
            const auto sound = right ? playerData_->rFootSounds[surfaceType_]
                                     : playerData_->lFootSounds[surfaceType_];
            if (sound >= 0) {
                TSFX::PlayAt(UInt32(sound), getTransform(), {}, getManager());
            }
            if (getManager()) {
                if (auto* decals =
                        dynamic_cast<DecalManager*>(getManager()->findObject(DecalManagerId))) {
                    const auto type = playerData_->footPrints[right ? 1 : 0];
                    if (type >= 0 && type < DecalManager::DECALS) {
                        decals->addFootprint(
                            getTransform(), DecalManager::DECALTYPE(type), right ? .5f : -.5f);
                    }
                }
            }
            break; // Legacy consumes at most one footfall per rendered frame.
        }
    }
    if (!jetting_ || mount_ || dead_) {
        stopJetSound();
        return;
    }
    if (!jetSound_ && playerData_->jetSound >= 0) {
        jetSound_ = TSFX::PlayAt(UInt32(playerData_->jetSound), getTransform(), {}, getManager());
    } else if (getManager()) {
        if (auto* audio =
                dynamic_cast<SoundPlayback*>(getManager()->findObject("GameSoundPlayback"));
            audio && audio->move) {
            audio->move(jetSound_, getTransform(), getLinearVelocity());
        }
    }
}

Int32 Player::possibleJetForces(std::span<Point3F, 4> forces,
                                const Point3F* unitLateralDir) const noexcept {
    if (!playerData_) {
        return 0;
    }
    forces[0].set(0.0f, 0.0f, 0.0f);
    forces[1].set(0.0f, 0.0f, playerData_->jetForce);
    Int32 numForces = 2;

    if (jumpSurfaceLastContact_ > 8 && unitLateralDir != nullptr) {
        const Point3F& lVel = getLinearVelocity();
        Point3F latVel = *unitLateralDir;
        Flt32 dot = m_dot(lVel, latVel);
        while (numForces < 4) {
            Flt32 pct = 0.0f;
            if (dot > playerData_->maxJetForwardVelocity) {
                pct = 0.0f;
            } else if (dot <= 0.0f) {
                pct = 1.0f;
            } else {
                pct = 1.0f - (dot / playerData_->maxJetForwardVelocity);
            }
            if (pct > playerData_->maxJetSideForceFactor) {
                pct = playerData_->maxJetSideForceFactor;
            }
            latVel *= pct * playerData_->jetForce;
            latVel.z += (1.0f - pct) * playerData_->jetForce;
            forces[static_cast<std::size_t>(numForces++)] = latVel;
            latVel = (*unitLateralDir) * -1.0f;
            dot = -dot;
        }
    }

    const Point3F otherForce = forceSumExcept(TribesJetForce);
    const Flt32 ts = getTimeSlice();
    for (Int32 i = 0; i < numForces; ++i) {
        forces[static_cast<std::size_t>(i)] =
            (forces[static_cast<std::size_t>(i)] + otherForce) * ts;
    }
    return numForces;
}

void Player::possibleJetForces(Flt32* minForce,
                               Flt32* maxForce,
                               const Point3F& iv,
                               Point3F* dvec) const noexcept {
    if (!playerData_ || !minForce || !maxForce || !dvec) {
        return;
    }
    const Flt32 dot = m_dot(iv, *dvec);
    Flt32 pct = 0.0f;
    if (dot > playerData_->maxJetForwardVelocity) {
        pct = 0.0f;
    } else if (dot < 0.0f) {
        pct = 1.0f;
    } else {
        pct = 1.0f - (dot / playerData_->maxJetForwardVelocity);
    }
    if (pct > playerData_->maxJetSideForceFactor) {
        pct = playerData_->maxJetSideForceFactor;
    }
    *minForce = 0.0f;
    *maxForce = pct * playerData_->jetForce;
}

void Player::updateMove(const PlayerMove* curMove, bool server) {
    if (!playerData_) {
        playerData_ = dynamic_cast<PlayerData*>(getDatPtr());
        if (!playerData_) {
            return;
        }
    }

    removeForce(TribesJetForce);
    lastImpactSpeed_ = 0.0f;

    PlayerMove localMove{};
    if (curMove && !dead_) {
        localMove = *curMove;
    }

    // Trigger and item requests are consumed by serverUpdateMove before the
    // delayed movement input. Never replay them from lastPlayerMove here.
    for (Int32 slot = 0; slot < MaxImageSlots; ++slot) {
        updateImageState(slot, .032f);
    }
    deltaRot_ = localMove.turnRot;
    const Flt32 oldPitch = viewPitch_;

    // Forward & side movement speeds with legacy diagonal clamp
    forwardAxisMovement_ = localMove.forwardAction * playerData_->maxForwardSpeed -
                           localMove.backwardAction * playerData_->maxBackwardSpeed;
    sideAxisMovement_ = (localMove.rightAction - localMove.leftAction) * playerData_->maxSideSpeed;

    const Flt32 movelen = std::sqrt(forwardAxisMovement_ * forwardAxisMovement_ +
                                    sideAxisMovement_ * sideAxisMovement_);
    const Flt32 maxmovelen =
        (forwardAxisMovement_ > 0.0f)
            ? std::max(playerData_->maxForwardSpeed, playerData_->maxSideSpeed)
            : std::max(playerData_->maxBackwardSpeed, playerData_->maxSideSpeed);
    if (movelen > maxmovelen && movelen > 0.0f) {
        forwardAxisMovement_ *= (maxmovelen / movelen);
        sideAxisMovement_ *= (maxmovelen / movelen);
    }
    if (crouching_) {
        forwardAxisMovement_ *= 0.5f;
        sideAxisMovement_ *= 0.5f;
    }

    // Jump impulse
    if (mount_ == nullptr && localMove.jumpAction && !dead_ && currentAnimation_ != ANIM_LAND &&
        jumpSurfaceLastContact_ < 8 && (!isGhost() || hasFocus_)) {
        jumpSurfaceLastContact_ = 8;
        setAnimation(ANIM_JUMPRUN);
        Point3F frtv{};
        Point3F ffdv{};
        getTransform().getRow(0, frtv);
        getTransform().getRow(1, ffdv);
        frtv *= (localMove.rightAction - localMove.leftAction);
        ffdv *= (localMove.forwardAction - localMove.backwardAction);
        frtv += ffdv;
        const Flt32 len = frtv.len();
        Flt32 dot = 0.0f;
        if (len >= 0.01f) {
            frtv.normalize();
            dot = m_dot(frtv, jumpSurfaceNormal_);
        }
        if (dot <= 0.0f) {
            applyImpulse(Point3F{0.0f, 0.0f, jumpSurfaceNormal_.z * playerData_->jumpImpulse});
        } else {
            frtv *= dot * playerData_->jumpImpulse;
            frtv.z = jumpSurfaceNormal_.z * playerData_->jumpImpulse;
            applyImpulse(frtv);
        }
    }

    // Jetting activation & energy threshold (JetFuelMin = 5.0f)
    if (jetting_) {
        if (energy_ <= playerData_->minJetEnergy || !localMove.jetting) {
            jetting_ = false;
        }
    } else {
        if (energy_ >= kJetFuelMin && localMove.jetting && mount_ == nullptr) {
            jetting_ = true;
        }
    }

    // Jet force computation
    if (jetting_ && !dead_) {
        energy_ = std::max(0.0f, energy_ - playerData_->jetEnergyDrain);
        Point3F frtv{};
        Point3F ffdv{};
        Point3F fudv{};
        getTransform().getRow(0, frtv);
        getTransform().getRow(1, ffdv);
        getTransform().getRow(2, fudv);

        Flt32 len = 0.0f;
        if (jumpSurfaceLastContact_ > 8) {
            frtv *= (localMove.rightAction - localMove.leftAction);
            ffdv *= (localMove.forwardAction - localMove.backwardAction);
            frtv += ffdv;
            len = frtv.len();
        }
        if (len > 0.01f) {
            frtv *= (1.0f / len);
            const Flt32 dot = m_dot(getLinearVelocity(), frtv);
            Flt32 pct = 0.0f;
            if (dot > playerData_->maxJetForwardVelocity) {
                pct = 0.0f;
            } else if (dot < 0.0f) {
                pct = 1.0f;
            } else {
                pct = 1.0f - (dot / playerData_->maxJetForwardVelocity);
            }
            if (pct > playerData_->maxJetSideForceFactor) {
                pct = playerData_->maxJetSideForceFactor;
            }
            frtv *= pct * playerData_->jetForce;
            fudv *= (1.0f - pct) * playerData_->jetForce;
            frtv += fudv;
            addForce(TribesJetForce, frtv);
        } else {
            addForce(TribesJetForce, Point3F{0.0f, 0.0f, playerData_->jetForce});
        }
    } else {
        jetting_ = false;
        addForce(TribesJetForce, Point3F{0.0f, 0.0f, 0.0f});
    }

    // View yaw & pitch + energy recharge
    if (!dead_) {
        Point3F curRot = getRot();
        curRot.z += localMove.turnRot;
        if (curRot.z < 0.0f) {
            curRot.z += kTwoPi;
        } else if (curRot.z > kTwoPi) {
            curRot.z -= kTwoPi;
        }
        // A forced setRot/setPosition clears SimMovement's contact flag. The
        // legacy movement code keeps last tick's contact until traction has
        // been applied, then commits rotation with the stepped transform.
        rotation_ = curRot;
        if (mount_) {
            setRot(curRot);
        }
        setViewPitch(viewPitch_ + localMove.pitch);
        updateEnergy(0.032f);
    }

    deltaPitch_ = viewPitch_ - oldPitch;

    // Crouch & falling state
    if (!isGhost() && !dead_) {
        const bool wantCrouch =
            playerData_->canCrouch && localMove.crouching && !jetting_ && lastContactCount_ < 32;
        setCrouching(wantCrouch);
    }
    falling_ = (getLinearVelocity().z <= -10.0f);
    setTimeSlice(0.032f);

    if (mount_ == nullptr) {
        if (isGhost()) {
            updateSkipCount_ = hasFocus_ ? 0 : updateSkipCount_ + 1;
        }
        const Point3F relVel{sideAxisMovement_, forwardAxisMovement_, 0.0f};
        // rotation_ already includes this move's turn. The collision transform
        // is committed below so last tick's contact survives traction, but
        // legacy resolves the desired velocity against the new heading.
        const Point3F& rotation = getRot();
        const RMat3F movementRotation{EulerF{rotation.x, rotation.y, rotation.z}};
        Point3F vel{};
        m_mul(relVel, movementRotation, vel);
        const Point3F sVel = getLinearVelocity();
        updateLinearVelocity();

        if (didContact()) {
            Point3F lVel = getLinearVelocity();
            Point3F dVel = vel - lVel;
            dVel.z = 0.0f;
            const Flt32 len = dVel.len();
            const Flt32 adjTraction = std::min(traction_ * playerData_->groundTraction, 1.0f);
            const Flt32 maxAcc = (playerData_->groundForce / std::max(0.01f, getMass())) *
                                 adjTraction * getTimeSlice();
            if (len > maxAcc && len > 0.0f) {
                dVel *= (maxAcc / len);
                setMovementFlags(movementFlags() & ~static_cast<UInt32>(UseFriction));
            } else {
                setMovementFlags(movementFlags() | static_cast<UInt32>(UseFriction));
            }
            lVel += dVel;
            setLinearVelocity(lVel);
        } else {
            if (sVel.len() < 0.1f) {
                setMovementFlags(movementFlags() | static_cast<UInt32>(UseFriction));
            } else {
                setMovementFlags(movementFlags() & ~static_cast<UInt32>(UseFriction));
            }
        }

        clearContacts();
        TMat3F targetMat = getTransform();
        targetMat.p += (sVel + getLinearVelocity()) * (0.5f * getTimeSlice());
        static_cast<RMat3F&>(targetMat).set(EulerF{rotation.x, rotation.y, rotation.z});

        Flt32 strac = traction_ * 0.6f;
        if (strac < 0.1f) {
            strac = 0.0f;
        }
        traction_ = 0.0f;
        const Point3F oldPos = getTransform().p;

        if (!Parent::stepPosition(targetMat, 0.075f, 0.3f)) {
            setRotation(static_cast<const RMat3F&>(targetMat), true);
        }
        if (traction_ < strac) {
            traction_ = strac;
        }
        if (traction_ > 1.0f) {
            traction_ = 1.0f;
        }
        deltaPos_ = getTransform().p - oldPos;
    }

    // Contact aging
    if (didContact()) {
        lastContactCount_ = 0;
    } else {
        ++lastContactCount_;
    }
    ++jumpSurfaceLastContact_;

    if ((damageFlash_ > 0.0f && !dead_) || (damageFlash_ > 0.4f && dead_)) {
        damageFlash_ = std::max(0.0f, damageFlash_ - 0.007f);
    }
    setCrouching(crouching_);
    (void)server;
}

void Player::ghostSetMove(const PlayerMove& move,
                          const Point3F& newPos,
                          const Point3F& newVel,
                          bool cont,
                          Flt32 newRot,
                          Flt32 newPitch,
                          Int32 skipCount,
                          bool noInterp,
                          std::optional<UInt32> packetTime) {
    const Point3F oldPos = getPos();
    const Flt32 oldRot = getRot().z;
    const Flt32 oldPitch = viewPitch_;

    setLinearVelocity(newVel);
    setPos(newPos);
    setRot(Point3F{0.0f, 0.0f, newRot});
    setViewPitch(newPitch);
    setContact(cont);

    lastPlayerMove_ = move;
    const auto timing = [&](std::string_view name, Int32 fallback) {
        if (auto* console = resolveConsole(); console && console->hasVariable(name)) {
            const auto text = console->getVariable(name);
            Int32 value{};
            const auto [end, error] =
                std::from_chars(text.data(), text.data() + text.size(), value);
            if (error == std::errc{} && end == text.data() + text.size()) {
                return UInt32(std::clamp(value, 0, 1000));
            }
        }
        return UInt32(fallback);
    };
    const UInt32 predictTime = timing("net::predictForwardTime", 64);
    const UInt32 interpolateTime = timing("net::interpolateTime", 0);
    // A receive callback runs before the manager advances to this frame.
    // Legacy uses cg.currentTime here, rather than the previous event time.
    const UInt32 currentTime = packetTime.value_or(
        getManager()
            ? std::max(lastProcessTime_,
                       UInt32(std::max(0.0, double(getManager()->getCurrentTime())) * 1000.0))
            : lastProcessTime_);
    interpDoneTime_ = 0;
    updateSkipCount_ = 0;
    for (UInt32 time = 0; time < predictTime + 32; time += 32) {
        updateMove(&lastPlayerMove_, false);
    }
    updateSkipCount_ = skipCount;

    if (noInterp || interpolateTime == 0) {
        interpDoneTime_ = 0;
        lastProcessTime_ = currentTime & ~UInt32{31};
        deltaPos_.set(0.0f, 0.0f, 0.0f);
        deltaRot_ = 0.0f;
        deltaPitch_ = 0.0f;
    } else {
        interpDoneTime_ = (currentTime + interpolateTime + 31) & ~UInt32{31};
        const Flt32 invTime = 1.0f / Flt32(interpDoneTime_ - lastProcessTime_);
        deltaPos_ = (getPos() - oldPos) * invTime;
        Flt32 dRot = getRot().z - oldRot;
        if (dRot > kPi) {
            dRot -= kTwoPi;
        } else if (dRot < -kPi) {
            dRot += kTwoPi;
        }
        deltaRot_ = dRot * invTime;
        deltaPitch_ = (viewPitch_ - oldPitch) * invTime;

        if (!mount_) {
            setPos(oldPos);
        }
        const auto rotation = getRot();
        setRot(Point3F{rotation.x, rotation.y, oldRot});
        setViewPitch(oldPitch);
        if (mount_) {
            TMat3F mountTransform, result;
            mount_->getObjectMountTransform(mountPoint_, &mountTransform);
            m_mul(TMat3F(EulerF{rotation.x, rotation.y, oldRot}, {}), mountTransform, result);
            setPosition(result, true);
        }
    }
}

void Player::stepDelta(Flt32 dt) {
    if (!std::isfinite(dt) || dt == 0.0f) {
        return;
    }
    const bool contact = didContact();
    if (!mount_) {
        setPos(getPos() + deltaPos_ * dt);
    }
    Point3F r = getRot();
    r.z += deltaRot_ * dt;
    while (r.z < 0.0f) {
        r.z += kTwoPi;
    }
    while (r.z >= kTwoPi) {
        r.z -= kTwoPi;
    }
    setRot(r);
    setViewPitch(viewPitch_ + deltaPitch_ * dt);
    if (mount_) {
        TMat3F mountTransform, result;
        mount_->getObjectMountTransform(mountPoint_, &mountTransform);
        m_mul(TMat3F(EulerF{r.x, r.y, r.z}, {}), mountTransform, result);
        setPosition(result, true);
    }
    setContact(contact);
}

void Player::fallToGround(Flt32 interval) {
    if (interval <= 0 || !getManager()) {
        return;
    }
    auto* root = dynamic_cast<sim::SimContainer*>(getManager()->findObject(sim::RootContainerId));
    if (!root) {
        return;
    }
    sim::SimContainerQuery query;
    query.id = getId();
    query.mask = SimTerrainObjectType | SimInteriorObjectType;
    query.box.fMin = getPos() + Point3F{0, 0, .125f};
    query.box.fMax = getPos() - Point3F{0, 0, 1.f};
    sim::SimCollisionInfo hit;
    if (!root->findLOS(query, hit) || hit.surfaces.surfaces.empty()) {
        return;
    }
    const auto& surface = hit.surfaces.surfaces.front();
    Point3F point, normal;
    m_mul(surface.position, hit.surfaces.tWorld, point);
    m_mul(surface.normal, static_cast<const RMat3F&>(hit.surfaces.tWorld), normal);
    if (std::abs(getPos().z - point.z) >= .5f || normal.len() < .001f) {
        return;
    }
    normal.normalize();
    Point3F local;
    auto rotation = getRot();
    m_mul(normal, RMat3F(EulerF{0, 0, -rotation.z}), local);
    const Flt32 amount = .05f * interval / .032f;
    const auto approach = [amount](Flt32 value, Flt32 target) {
        return value + std::clamp(target - value, -amount, amount);
    };
    rotation.x = approach(rotation.x, -std::atan2(local.y, local.z));
    rotation.y = approach(rotation.y, std::atan2(local.x, local.z));
    setRot(rotation);
}

void Player::fallToGround(Flt32 interval, const Point3F& deltaRot) {
    Point3F r = getRot();
    r += deltaRot * interval;
    setRot(r);
}

void Player::writePacketData(BitStream& stream) const {
    stream.writeInt(static_cast<Int32>(getSensorPinged()), 2);
    stream.write(getRot().z);
    if (stream.writeFlag(mount_ == nullptr)) {
        const Point3F& pos = getLinearPosition();
        const Point3F& vel = getLinearVelocity();
        stream.write(pos.x);
        stream.write(pos.y);
        stream.write(pos.z);
        stream.write(vel.x);
        stream.write(vel.y);
        stream.write(vel.z);
    }
    stream.write(viewPitch_);
    stream.write(energy_);
    stream.writeFlag(didContact());
    stream.writeFlag(jetting_);
    stream.writeFloat(std::clamp(traction_, 0.0f, 1.0f), 7);
    stream.writeFlag(crouching_);
    stream.writeInt(jumpSurfaceLastContact_ > 15 ? 15 : std::max(0, jumpSurfaceLastContact_), 4);
}

bool Player::readPacketData(BitStream& stream) {
    BitStreamRollbackGuard guard(stream);

    const UInt32 savedPinged = getSensorPinged();
    const Point3F savedRot = getRot();
    const Point3F savedPos = getPos();
    const Point3F savedVel = getLinearVelocity();
    const Flt32 savedPitch = viewPitch_;
    const Flt32 savedEnergy = energy_;
    const bool savedJetting = jetting_;
    const Flt32 savedTraction = traction_;
    const bool savedCrouching = crouching_;
    const Int32 savedLastContact = jumpSurfaceLastContact_;

    const auto rollback = [&]() noexcept {
        setSensorPinged(static_cast<UInt8>(savedPinged));
        setRot(savedRot);
        setPos(savedPos);
        setLinearVelocity(savedVel);
        viewPitch_ = savedPitch;
        energy_ = savedEnergy;
        jetting_ = savedJetting;
        traction_ = savedTraction;
        setCrouching(savedCrouching);
        jumpSurfaceLastContact_ = savedLastContact;
        return false;
    };

    const auto pinged = static_cast<UInt8>(stream.readInt(2));
    Flt32 rotZ = 0.0f;
    if (!stream.read(rotZ)) {
        return rollback();
    }
    const bool unmounted = stream.readFlag();
    Point3F pos = savedPos;
    Point3F vel = savedVel;
    if (unmounted) {
        if (!stream.read(pos.x) || !stream.read(pos.y) || !stream.read(pos.z) ||
            !stream.read(vel.x) || !stream.read(vel.y) || !stream.read(vel.z)) {
            return rollback();
        }
    }
    Flt32 pitch = 0.0f;
    Flt32 eng = 0.0f;
    if (!stream.read(pitch) || !stream.read(eng)) {
        return rollback();
    }
    const bool contactFlag = stream.readFlag();
    const bool jetFlag = stream.readFlag();
    const Flt32 trac = stream.readFloat(7);
    const bool crouchFlag = stream.readFlag();
    const Int32 lastContact = stream.readInt(4);
    if (!stream.isValid()) {
        return rollback();
    }

    setSensorPinged(pinged);
    setRot(Point3F{0.0f, 0.0f, rotZ});
    if (unmounted) {
        setPos(pos);
        setLinearVelocity(vel);
    }
    viewPitch_ = pitch;
    energy_ = eng;
    setContact(contactFlag);
    jetting_ = jetFlag;
    traction_ = trac;
    setCrouching(crouchFlag);
    jumpSurfaceLastContact_ = lastContact;
    interpDoneTime_ = 0;

    guard.committed = true;
    return true;
}

bool Player::packPlayerUpdate(UInt32 mask,
                              BitStream& stream,
                              sim::network::GhostManager* gm) const {
    if (!packShapeBaseUpdate(mask, stream)) {
        return false;
    }

    bool myClient = false;
    if (gm && getControlClient() != 0 && gm->owner() != nullptr) {
        myClient = (getControlClient() == static_cast<Int32>(gm->owner()->getId()));
    }

    if (stream.writeFlag((mask & BaseStateMask) != 0)) {
        packDatFile(stream);
        stream.writeFlag(aiControlled_);
    }

    if (mount_ != nullptr && (mask & MountMask) != 0) {
        UInt16 ghostIdx = mountGhostIndex_;
        if (gm) {
            ghostIdx = gm->getGhostIndex(*mount_);
        }
        if (ghostIdx != sim::InvalidGhostIndex) {
            stream.writeFlag(true);
            stream.writeFlag(true);
            stream.writeInt(static_cast<Int32>(ghostIdx), 10);
            stream.writeInt(mountPoint_, 5);
        } else {
            stream.writeFlag(false);
        }
    } else if (mount_ == nullptr && (mask & MountMask) != 0) {
        stream.writeFlag(true);
        stream.writeFlag(false);
    } else {
        stream.writeFlag(false);
    }

    const Flt32 maxDamage =
        (playerData_ && playerData_->maxDamage > 0.0f) ? playerData_->maxDamage : 100.0f;
    const Flt32 maxEnergy =
        (playerData_ && playerData_->maxEnergy > 0.0f) ? playerData_->maxEnergy : 100.0f;

    if (stream.writeFlag((mask & DamageMask) != 0)) {
        stream.writeFlag(dead_);
        if (!dead_) {
            const Flt32 dl = std::clamp(damageLevel_ / maxDamage, 0.0f, 1.0f);
            stream.writeInt(static_cast<Int32>(dl * 63.0f), 6);
        } else {
            if (!stream.writeFlag(blowUpPending_)) {
                stream.writeInt(std::clamp(currentAnimation_, 0, 63), 6);
            }
        }
    }

    const bool writePos = (mask == 0xFFFFFFFFu) || (((mask & OrientationMask) != 0) && !myClient);
    if (stream.writeFlag(writePos)) {
        stream.writeSignedFloat(std::clamp(viewPitch_ / kMaxPitch, -1.0f, 1.0f), 5);
        const Point3F& pos = getLinearPosition();
        stream.write(pos.x);
        stream.write(pos.y);
        stream.write(pos.z);
        Point3F v = getLinearVelocity();
        const Flt32 len = v.len();
        if (stream.writeFlag(len > 0.02f)) {
            v *= 1.0f / len;
            const Flt32 outLen = std::min(len * 512.0f, 131071.0f);
            stream.writeInt(static_cast<Int32>(outLen), 17);
            const NormalVector3F nv{v.x, v.y, v.z};
            stream.writeNormalVector(nv, 10);
        }
        stream.writeFlag(didContact());
        Flt32 rotZ = getRot().z;
        while (rotZ < 0.0f) {
            rotZ += kTwoPi;
        }
        while (rotZ >= kTwoPi) {
            rotZ -= kTwoPi;
        }
        stream.writeInt(std::clamp(static_cast<Int32>(rotZ * 511.0f / kTwoPi), 0, 511), 9);
        if (mask != 0xFFFFFFFFu) {
            lastPlayerMove_.write(stream, false, nullptr);
            stream.writeInt(std::clamp(static_cast<Int32>(energy_ * 127.0f / maxEnergy), 0, 127),
                            7);
            stream.writeInt(std::clamp(updateSkipCount_, 0, 15), 4);
            stream.writeFlag((mask & NoInterpMask) != 0);
        }
    }

    if (mask == 0xFFFFFFFFu) {
        stream.writeFlag(false);
    } else if (stream.writeFlag(((mask & AnimationMask) != 0) && serverAnimation_ > 0)) {
        stream.writeInt(std::clamp(serverAnimation_, 0, 63), 6);
    }

    if (stream.writeFlag((myClient || mask == 0xFFFFFFFFu) && ((mask & InfoMask) != 0))) {
        stream.write(rechargeRate_);
    }

    stream.writeFlag(pdaing_);
    stream.writeFlag(crouching_);
    packItemImages(stream, mask);

    return stream.isValid();
}

bool Player::unpackPlayerUpdate(BitStream& stream, sim::network::GhostManager* gm) {
    BitStreamRollbackGuard guard(stream);

    const auto savedTeam = teamId_;
    const auto savedCtrl = ctrlClientId_;
    const auto savedOwner = ownerClientId_;
    const auto savedLastOwner = lastOwnerClientId_;
    const auto savedShieldVec = shieldVec_;
    const auto savedZOffset = zOffset_;
    const auto savedThreads = scriptThread_;
    const auto savedFading = m_fading_;
    const auto savedFadeStarted = m_fadeStarted_;
    const auto savedDatId = datFileId_;
    const auto savedDatName = datFileName_;
    const bool savedAi = aiControlled_;
    GameBase* const savedMount = mount_;
    const Int32 savedMountPt = mountPoint_;
    const UInt16 savedMountGhost = mountGhostIndex_;
    const bool savedDead = dead_;
    const Flt32 savedDamage = damageLevel_;
    const bool savedBlowUp = blowUpPending_;
    const Int32 savedAnim = currentAnimation_;
    const Int32 savedServerAnim = serverAnimation_;
    const bool savedAnimationPending = animationPending_;
    const Point3F savedPos = getPos();
    const Point3F savedVel = getLinearVelocity();
    const Point3F savedRot = getRot();
    const Flt32 savedPitch = viewPitch_;
    const Flt32 savedEnergy = energy_;
    const Flt32 savedRecharge = rechargeRate_;
    const PlayerMove savedMove = lastPlayerMove_;
    const Int32 savedSkip = updateSkipCount_;
    const bool savedPda = pdaing_;
    const bool savedCrouch = crouching_;
    const auto savedImages = itemImageList_;

    const auto rollback = [&]() noexcept {
        teamId_ = savedTeam;
        ctrlClientId_ = savedCtrl;
        ownerClientId_ = savedOwner;
        lastOwnerClientId_ = savedLastOwner;
        shieldVec_ = savedShieldVec;
        zOffset_ = savedZOffset;
        scriptThread_ = savedThreads;
        m_fading_ = savedFading;
        m_fadeStarted_ = savedFadeStarted;
        datFileId_ = savedDatId;
        datFileName_ = savedDatName;
        aiControlled_ = savedAi;
        mount_ = savedMount;
        mountPoint_ = savedMountPt;
        mountGhostIndex_ = savedMountGhost;
        setDead(savedDead);
        damageLevel_ = savedDamage;
        blowUpPending_ = savedBlowUp;
        currentAnimation_ = savedAnim;
        serverAnimation_ = savedServerAnim;
        animationPending_ = savedAnimationPending;
        setPos(savedPos);
        setLinearVelocity(savedVel);
        setRot(savedRot);
        viewPitch_ = savedPitch;
        energy_ = savedEnergy;
        rechargeRate_ = savedRecharge;
        lastPlayerMove_ = savedMove;
        updateSkipCount_ = savedSkip;
        pdaing_ = savedPda;
        setCrouching(savedCrouch);
        itemImageList_ = savedImages;
        return false;
    };

    if (!unpackShapeBaseUpdate(stream)) {
        return rollback();
    }

    bool isInitialUpdate = false;
    const PlayerData* updateData = playerData_;

    struct PendingOrientation {
        PlayerMove move;
        Point3F position, velocity;
        Flt32 rotation, pitch;
        Int32 skipCount{};
        bool contact{}, noInterp{};
        std::optional<UInt32> packetTime{};
    };

    std::optional<PendingOrientation> orientation;
    if (stream.readFlag()) {
        if (!unpackDatFile(stream)) {
            return rollback();
        }
        aiControlled_ = stream.readFlag();
        if (!stream.isValid()) {
            return rollback();
        }
        isInitialUpdate = (getManager() == nullptr);
        if (!isInitialUpdate && datFileId_ != savedDatId) {
            auto* dbm = resolveDataBlockManager();
            updateData =
                dbm ? dynamic_cast<PlayerData*>(dbm->lookupDataBlock(datFileId_, getDatGroup()))
                    : nullptr;
            if (!updateData) {
                return rollback();
            }
        }
    }

    if (stream.readFlag()) {
        if (stream.readFlag()) {
            const auto gIdx = static_cast<UInt16>(stream.readInt(10));
            const Int32 mPt = stream.readInt(5);
            if (!stream.isValid()) {
                return rollback();
            }
            mountGhostIndex_ = gIdx;
            mountPoint_ = mPt;
            if (gm) {
                mount_ = dynamic_cast<GameBase*>(gm->resolveGhost(gIdx));
            }
        } else {
            mount_ = nullptr;
            mountPoint_ = 0;
            mountGhostIndex_ = sim::InvalidGhostIndex;
        }
    }

    const Flt32 maxDamage =
        (updateData && updateData->maxDamage > 0.0f) ? updateData->maxDamage : 100.0f;
    const Flt32 maxEnergy =
        (updateData && updateData->maxEnergy > 0.0f) ? updateData->maxEnergy : 100.0f;

    if (stream.readFlag()) {
        const bool deadFlag = stream.readFlag();
        if (!deadFlag) {
            const Int32 dlQuant = stream.readInt(6);
            if (!stream.isValid()) {
                return rollback();
            }
            setDead(false);
            damageLevel_ = (static_cast<Flt32>(dlQuant) * maxDamage) / 63.0f;
        } else {
            const bool blownFlag = stream.readFlag();
            Int32 dAnim = currentAnimation_;
            if (!blownFlag) {
                dAnim = stream.readInt(6);
            }
            if (!stream.isValid()) {
                return rollback();
            }
            setDead(true);
            damageLevel_ = maxDamage;
            blowUpPending_ = blownFlag;
            if (!blownFlag) {
                currentAnimation_ = dAnim;
                serverAnimation_ = dAnim;
                animationPending_ = !savedDead || savedAnim != dAnim;
            }
        }
    }

    if (stream.readFlag()) {
        const Flt32 pitch = stream.readSignedFloat(5) * kMaxPitch;
        Point3F pos{};
        if (!stream.read(pos.x) || !stream.read(pos.y) || !stream.read(pos.z)) {
            return rollback();
        }
        Point3F vel{0.0f, 0.0f, 0.0f};
        if (stream.readFlag()) {
            const Flt32 speed = static_cast<Flt32>(stream.readInt(17)) / 512.0f;
            NormalVector3F nv{};
            stream.readNormalVector(nv, 10);
            vel.set(nv.x * speed, nv.y * speed, nv.z * speed);
        }
        const bool cont = stream.readFlag();
        const Flt32 rotZ = static_cast<Flt32>(stream.readInt(9)) * kTwoPi / 511.0f;
        if (!stream.isValid()) {
            return rollback();
        }
        if (!isInitialUpdate) {
            PlayerMove pm{};
            if (!pm.read(stream, false, nullptr)) {
                return rollback();
            }
            const Int32 engQuant = stream.readInt(7);
            const Int32 skipCount = stream.readInt(4);
            const bool noInterp = stream.readFlag();
            if (!stream.isValid()) {
                return rollback();
            }
            energy_ = (static_cast<Flt32>(engQuant) * maxEnergy) / 127.0f;
            const auto packetTime =
                gm && gm->owner() ? std::optional{gm->owner()->currentTime()} : std::nullopt;
            orientation = PendingOrientation{
                pm, pos, vel, rotZ, pitch, skipCount, cont, noInterp, packetTime};
        } else {
            orientation = PendingOrientation{{}, pos, vel, rotZ, pitch};
        }
    }

    if (stream.readFlag()) {
        const Int32 anim = stream.readInt(6);
        if (!stream.isValid()) {
            return rollback();
        }
        serverAnimation_ = anim;
        currentAnimation_ = anim;
        animationPending_ = true;
    }

    if (stream.readFlag()) {
        if (!stream.read(rechargeRate_)) {
            return rollback();
        }
    }

    const bool pdaFlag = stream.readFlag();
    const bool crouchFlag = stream.readFlag();
    if (!stream.isValid()) {
        return rollback();
    }
    pdaing_ = pdaFlag;
    setCrouching(crouchFlag);

    if (!unpackItemImages(stream, isInitialUpdate) || !stream.isValid()) {
        return rollback();
    }

    if (getManager() && datFileId_ != savedDatId) {
        // Existing ghosts must replace their body, collision geometry, and
        // animation threads when armor changes, just as initial ghosts do.
        // Wait for the complete packet so a truncated update cannot publish
        // a new shape alongside the rolled-back datablock ID.
        const auto packetEnergy = energy_;
        datFileName_.clear();
        if (!loadDatFile()) {
            return rollback();
        }
        energy_ = packetEnergy;
        reSkin(true);
    }
    if (orientation) {
        const auto& next = *orientation;
        if (isInitialUpdate) {
            setLinearVelocity(next.velocity);
            setRot(Point3F{0, 0, next.rotation});
            setPos(next.position);
            setViewPitch(next.pitch);
        } else {
            ghostSetMove(next.move,
                         next.position,
                         next.velocity,
                         next.contact,
                         next.rotation,
                         next.pitch,
                         next.skipCount,
                         next.noInterp,
                         next.packetTime);
        }
    }
    // Prediction consumes the move's crouch input; the final ghost flag owns
    // the transmitted stance, including stationary updates.
    setCrouching(crouchFlag);

    guard.committed = true;
    return true;
}

UInt32 Player::packUpdate(sim::network::GhostManager& gm, UInt32 mask, BitStream& stream) const {
    UInt32 retMask = 0;
    if (mount_ != nullptr && (mask & MountMask) != 0) {
        if (gm.getGhostIndex(*mount_) == sim::InvalidGhostIndex) {
            retMask |= MountMask;
        }
    }
    packPlayerUpdate(mask, stream, &gm);
    return retMask;
}

bool Player::unpackUpdate(sim::network::GhostManager& gm, BitStream& stream) {
    return unpackPlayerUpdate(stream, &gm);
}

} // namespace opent1::game
