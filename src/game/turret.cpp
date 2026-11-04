#include <opent1/game/player.hpp>
#include <ranges>
#include <opent1/game/projectile_factory.hpp>
#include <opent1/game/turret.hpp>
#include <opent1/game/vehicle.hpp>
#include <opent1/game/sound.hpp>
#include <opent1/world/world_scene.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <numbers>

namespace opent1::game {

namespace {
constexpr Flt32 kPi = static_cast<Flt32>(std::numbers::pi);
constexpr Flt32 kTwoPi = static_cast<Flt32>(2.0 * std::numbers::pi);
constexpr Flt32 kHalfPi = static_cast<Flt32>(0.5 * std::numbers::pi);

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

template <class T> bool writeStreamPod(StreamIO& s, const T& val) {
    static_assert(std::is_trivially_copyable_v<T>);
    std::array<std::byte, sizeof(T)> bytes{};
    std::memcpy(bytes.data(), &val, sizeof(T));
    return s.writeBytes(bytes);
}

template <class T> bool readStreamPod(StreamIO& s, T& val) {
    static_assert(std::is_trivially_copyable_v<T>);
    std::array<std::byte, sizeof(T)> bytes{};
    if (!s.readBytes(bytes)) {
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

} // namespace

Turret::Turret() : Sensor(false) {}

Turret::~Turret() = default;

bool Turret::initResources(GameBaseData* in_data) {
    const bool initialGhost = isGhost() && hasUnpackedInitialSensor_ && !isProperlyAdded();
    const auto initialState = state_;
    // Initial packets precede onAdd/resource loading. Elevation was decoded
    // using the fallback limits, so retain its fraction until the authored
    // muzzle limits are available.
    const auto initialElevation =
        maxElevation_ > minElevation_
            ? (turretElevation_ - minElevation_) / (maxElevation_ - minElevation_)
            : .5f;
    if (!Parent::initResources(in_data)) {
        return false;
    }
    turretData_ = dynamic_cast<TurretData*>(in_data);
    if (!turretData_) {
        return false;
    }

    isSustained_ = turretData_->isSustained;
    projData_ = nullptr;
    if (auto* dbm = resolveDataBlockManager()) {
        if (turretData_->projectile.type != -1 && turretData_->projectile.dataType != -1) {
            projData_ = dynamic_cast<ProjectileData*>(dbm->lookupDataBlock(
                turretData_->projectile.dataType, turretData_->projectile.type));
        }
    }

    gunPos_ = getBoxCenter();
    minElevation_ = -kHalfPi * 0.5f;
    maxElevation_ = kHalfPi * 0.5f;
    canShift_ = false;
    state_ = RETRACTED;
    hasPrimary_ = true;
    hasSecondary_ = false;
    gunNode_ = cameraNode_ = -1;
    animationThread_.reset();
    powerSequence_ = fireSequence_ = -1;

    if (shapeInstance_) {
        const auto& shape = *shapeInstance_->shape();
        for (auto i : std::views::iota(std::size_t{0}, shape.nodes.size())) {
            const auto name = shape.nodes[i].name;
            if (name < 0 || std::size_t(name) >= shape.names.size()) {
                continue;
            }
            if (shape.names[name] == "dummy muzzle") {
                gunNode_ = Int32(i);
            }
            if (shape.names[name] == "dummy eye") {
                cameraNode_ = Int32(i);
            }
        }
        for (auto i : std::views::iota(std::size_t{0}, shape.sequences.size())) {
            const auto name = shape.sequences[i].name;
            if (name >= 0 && std::size_t(name) < shape.names.size()) {
                if (shape.names[name] == "power") {
                    powerSequence_ = Int32(i);
                }
                if (shape.names[name] == "fire") {
                    fireSequence_ = Int32(i);
                }
            }
            if (name < 0 || std::size_t(name) >= shape.names.size() ||
                shape.names[name] != "elevate" || gunNode_ < 0) {
                continue;
            }
            const std::array<world::TsAnimationSample, 1> lower{{{Int32(i), 0.f}}},
                upper{{{Int32(i), 1.f}}};
            const auto low = world::TsShapePose::sample(shapeInstance_->shape(), lower);
            const auto high = world::TsShapePose::sample(shapeInstance_->shape(), upper);
            if (low && high) {
                EulerF a, b;
                low->nodeTransforms()[gunNode_].angles(a);
                high->nodeTransforms()[gunNode_].angles(b);
                if (a.x > kPi) {
                    a.x -= kTwoPi;
                }
                if (b.x > a.x) {
                    minElevation_ = a.x;
                    maxElevation_ = b.x;
                }
            }
        }
        turretRotation_ = 0.f;
        canShift_ = powerSequence_ >= 0 && fireSequence_ >= 0;
        // Turrets own their power/fire track; StaticBase only owns ambient
        // and visibility tracks. Stations animate power through script slots.
        if (powerSequence_ >= 0) {
            animationThread_ = shapeInstance_->addThread(powerSequence_, 0.f);
        } else if (fireSequence_ >= 0) {
            animationThread_ = shapeInstance_->addThread(fireSequence_, 1.f);
        }
        setAnimationState(canShift_ ? RETRACTED : EXTENDED);
        if (initialGhost) {
            setAnimationState(initialState);
            turretRotation_ = desiredRotation_;
            turretElevation_ = desiredElevation_ =
                minElevation_ + initialElevation * (maxElevation_ - minElevation_);
        }
    }

    return true;
}

bool Turret::onAdd() {
    if (!Parent::onAdd()) {
        return false;
    }

    if (isGhost()) {
        addToSet(sim::CameraMountSetId);
        addToSet(sim::LightSetId);
    } else {
        addToSet(PlayerSetId);
    }
    updateTurretFireSound();
    return true;
}

void Turret::onRemove() {
    TSFX::Stop(fireSound_, getManager());
    TSFX::Stop(moveSound_, getManager());
    if (currentTarget_) {
        clearNotify(*currentTarget_);
        currentTarget_ = nullptr;
    }
    if (m_pCurrentProjectile_) {
        auto* projectile = m_pCurrentProjectile_;
        m_pCurrentProjectile_ = nullptr;
        clearNotify(*projectile);
        projectile->shutOffProjectile();
    }
    Parent::onRemove();
}

void Turret::onDeleteNotify(sim::SimObject& object) {
    if (&object == currentTarget_) {
        currentTarget_ = nullptr;
        unshoot();
    } else if (&object == m_pCurrentProjectile_) {
        m_pCurrentProjectile_ = nullptr;
        m_fireState_ = Reloading;
        m_waitScale_ = 1.0f;
    }
    Parent::onDeleteNotify(object);
}

void Turret::setCurrentTarget(GameBase* t) {
    if (currentTarget_) {
        clearNotify(*currentTarget_);
    }
    currentTarget_ = t;
    if (currentTarget_) {
        deleteNotify(*currentTarget_);
    }
}

void Turret::onActivate() {
    Parent::onActivate();
}

void Turret::onDeactivate() {
    unshoot();
    setCurrentTarget(nullptr);
    retract();
    Parent::onDeactivate();
}

void Turret::onPowerStateChange(GameBase* generator) {
    if (!isPowered()) {
        unshoot();
        setCurrentTarget(nullptr);
    }
    Parent::onPowerStateChange(generator);
}

Flt32 Turret::elevation(const Point3F& pos, bool clamp) const noexcept {
    TMat3F muzzle;
    getMuzzleTransform(0, &muzzle);
    Point3F aimVector = pos - muzzle.p;
    m_mul(aimVector, static_cast<const RMat3F&>(getInvTransform()), aimVector);
    const Flt32 len = aimVector.len();
    if (len <= 1e-5f) {
        return 0.0f;
    }
    const Flt32 angle = std::asin(std::clamp(aimVector.z / len, -1.0f, 1.0f));
    if (clamp) {
        return std::clamp(angle, minElevation_, maxElevation_);
    }
    return angle;
}

Flt32 Turret::elevation(Flt32 dx, Flt32 dy, Flt32 dz) const noexcept {
    const Flt32 angle = -m_atan(dx, dy);
    const Flt32 newX = (dx * m_cos(angle)) - (dy * m_sin(angle));
    return m_atan(-dz, newX) - kHalfPi;
}

Flt32 Turret::rotation(const Point3F& pos) const noexcept {
    TMat3F muzzle;
    getMuzzleTransform(0, &muzzle);
    Point3F aimVector = pos - muzzle.p;
    m_mul(aimVector, static_cast<const RMat3F&>(getInvTransform()), aimVector);
    Flt32 angle = std::atan2(-aimVector.x, aimVector.y);
    if (angle < 0.0f) {
        angle += kTwoPi;
    }
    return angle;
}

Flt32 Turret::rotation(Flt32 dx, Flt32 dy) const noexcept {
    return m_atan(dx, dy) + kHalfPi;
}

bool Turret::inDeadZone(const Point3F& pos, Flt32* r, Flt32* e) const noexcept {
    const Flt32 el = elevation(pos, false);
    const Flt32 rot = rotation(pos);
    if (r) {
        *r = rot;
    }
    if (e) {
        *e = el;
    }
    return el > maxElevation_ || el < minElevation_;
}

bool Turret::inDeadZone(Flt32 desRot, Flt32 desEle) const noexcept {
    bool result = false;
    if (desRot > kPi) {
        desRot -= kTwoPi;
    }
    if (maxRotation_ != minRotation_) {
        if ((desRot > 0.0f && desRot > maxRotation_) || (desRot < 0.0f && desRot < minRotation_)) {
            result = true;
        }
    }
    if (desEle > maxElevation_ || desEle < minElevation_) {
        result = true;
    }
    return result;
}

Point3F Turret::leadPosition(GameBase* target, bool useProjSpeed) const noexcept {
    if (!target) {
        return getBoxCenter();
    }

    Point3F pos = target->getBoxCenter();
    if (!useProjSpeed) {
        return pos;
    }
    const Flt32 dist = (pos - getBoxCenter()).len();
    const Flt32 speed = projData_ ? projData_->getTerminalVelocity() : 50.0f;
    if (speed > 1e-4f) {
        pos += target->getLeadVelocity() * (dist / speed);
    }
    return pos;
}

Flt32 Turret::wrapRotation(Flt32 rot) const noexcept {
    while (rot >= kTwoPi) {
        rot -= kTwoPi;
    }
    while (rot < 0.0f) {
        rot += kTwoPi;
    }
    return rot;
}

Flt32 Turret::wrapElevation(Flt32 elev) const noexcept {
    return std::clamp(elev, minElevation_, maxElevation_);
}

bool Turret::isTargetable(GameBase* target) {
    if (!target || target == this) {
        return false;
    }
    if (auto* pl = dynamic_cast<Player*>(target)) {
        if (pl->isDead() || !pl->isExposed()) {
            return false;
        }
        if (auto* console = resolveConsole()) {
            const auto callback = scriptName("verifyTarget");
            if (!callback.empty()) {
                const std::array<std::string, 2> args{scriptThis(), pl->scriptThis()};
                const auto result = console->execute(callback, args);
                if (!result.value.empty() && (result.value.front() == 'F' ||
                                              result.value.front() == 'f' || result.value == "0")) {
                    return false;
                }
            }
        }
        Flt32 r = 0.0f;
        Flt32 e = 0.0f;
        if (!inDeadZone(leadPosition(target), &r, &e) &&
            !inDeadZone(leadPosition(target, false), &r, &e) && detectItem(target)) {
            if (getTeam() != target->getTeam() || target->getTeam() == -1) {
                return hasTargetLOS(target);
            }
        }
    } else if (dynamic_cast<Vehicle*>(target) != nullptr) {
        Flt32 r = 0.0f;
        Flt32 e = 0.0f;
        if (!inDeadZone(leadPosition(target), &r, &e) &&
            !inDeadZone(leadPosition(target, false), &r, &e) && detectItem(target)) {
            if (getTeam() != target->getTeam() || target->getTeam() == -1) {
                return hasTargetLOS(target);
            }
        }
    } else if (auto* rM = dynamic_cast<Projectile*>(target)) {
        if (!rM->isTargetable()) {
            return false;
        }
        Flt32 r = 0.0f;
        Flt32 e = 0.0f;
        if (!inDeadZone(leadPosition(target), &r, &e) &&
            !inDeadZone(leadPosition(target, false), &r, &e) && detectItem(target)) {
            const ShapeBase* pShooter = rM->shooter();
            if (!pShooter || getTeam() != pShooter->getTeam() || pShooter->getTeam() == -1) {
                return hasTargetLOS(target);
            }
        }
    }
    return false;
}

bool Turret::hasTargetLOS(const GameBase* target) const {
    auto* manager = getManager();
    auto* root = manager
                     ? dynamic_cast<sim::SimContainer*>(manager->findObject(sim::RootContainerId))
                     : nullptr;
    if (!root || !target) {
        return false;
    }

    // Sensor(false) deliberately disables sensor LOS. Weapon LOS is a separate
    // collision query in the original turret, and must never use checkLOS().
    sim::SimContainerQuery query{};
    query.id = getId();
    query.type = ~UInt32{0};
    query.mask =
        SimTerrainObjectType | SimInteriorObjectType | StaticObjectType | MoveableObjectType;
    query.detail = -1;
    query.box.fMax = target->getBoxCenter();
    TMat3F muzzle;
    getMuzzleTransform(0, &muzzle);
    for (const auto origin : {getBoxCenter(), muzzle.p}) {
        query.box.fMin = origin;
        sim::SimCollisionInfo hit{};
        if (root->findLOS(query, hit) && hit.object.resolve(*manager) != target) {
            return false;
        }
    }
    return true;
}

void Turret::chooseTarget() {
    if (!getManager() || !turretData_) {
        return;
    }
    auto* set = dynamic_cast<sim::SimSet*>(getManager()->findObject(PlayerSetId));
    if (!set) {
        return;
    }

    const Flt32 rRange = static_cast<Flt32>(getRange());
    Flt32 minAngle = 100.0f;
    GameBase* newTarget = nullptr;

    for (sim::SimObject* obj : set->objects()) {
        auto* candidate = dynamic_cast<GameBase*>(obj);
        if (!candidate || !isTargetable(candidate)) {
            continue;
        }
        Flt32 dRot = std::fabs(rotation(leadPosition(candidate)) - turretRotation_);
        if (dRot > kPi) {
            dRot = kTwoPi - dRot;
        }
        const Flt32 dist = (getBoxCenter() - candidate->getBoxCenter()).len();
        const Flt32 metric = (rRange > 1e-4f) ? (dRot + (dist / rRange) * kTwoPi) : dRot;
        if (metric < minAngle) {
            newTarget = candidate;
            minAngle = metric;
        }
    }

    if (newTarget) {
        setCurrentTarget(newTarget);
    }
}

void Turret::extend() {
    if (canShift_ && state_ != EXTENDED && state_ != EXTENDING) {
        setAnimationState(EXTENDING);
        setMaskBits(StateMask);
    } else if (!canShift_ && state_ != EXTENDED) {
        setAnimationState(EXTENDED);
        setMaskBits(StateMask);
    }
}

void Turret::retract() {
    if (canShift_ && state_ != RETRACTED && state_ != RETRACTING) {
        setAnimationState(RETRACTING);
        setMaskBits(StateMask);
    }
}

void Turret::setAnimationState(State state) {
    const auto previous = state_;
    state_ = state;
    if (!shapeInstance_ || !animationThread_) {
        return;
    }
    const auto thread = *animationThread_;
    if (canShift_ && state != EXTENDED) {
        const auto& current = shapeInstance_->threads()[thread];
        const auto position = current.sequence == powerSequence_
                                  ? current.position
                                  : (previous == EXTENDED ? 1.f : 0.f);
        shapeInstance_->setSequence(thread, powerSequence_, state == RETRACTED ? 0.f : position);
        shapeInstance_->setPriority(thread, -1);
        shapeInstance_->setTimeScale(thread,
                                     state == EXTENDING    ? 1.f
                                     : state == RETRACTING ? -1.f
                                                           : 0.f);
        turretElevation_ = 0.f;
        setMaskBits(ElevationMask);
    } else if (fireSequence_ >= 0) {
        shapeInstance_->setSequence(thread, fireSequence_, 1.f);
        shapeInstance_->setPriority(thread, 0);
        shapeInstance_->setTimeScale(thread, 0.f);
    }
}

void Turret::advanceAnimation(Flt32 interval) {
    if (!shapeInstance_ || !animationThread_ || interval <= 0.f) {
        return;
    }
    const auto thread = *animationThread_;
    if (state_ == RETRACTING && turretRotation_ != 0.f) {
        // Legacy parks the barrel before lowering it into the base.
        const auto rotation = turretRotation_ > kPi ? turretRotation_ - kTwoPi : turretRotation_;
        const auto step = std::max(0.f, turretData_->speed * interval);
        turretRotation_ = std::abs(rotation) <= step
                              ? 0.f
                              : wrapRotation(rotation - std::copysign(step, rotation));
        setMaskBits(TRotationMask);
        if (turretRotation_ != 0.f) {
            return;
        }
    }
    shapeInstance_->advanceThread(thread, interval);
    const auto position = shapeInstance_->threads()[thread].position;
    if (state_ == EXTENDING && position >= 1.f) {
        setAnimationState(EXTENDED);
        setMaskBits(StateMask);
    } else if (state_ == RETRACTING && position <= 0.f) {
        setAnimationState(RETRACTED);
        setMaskBits(StateMask);
    } else if (state_ == EXTENDED && position >= 1.f) {
        shapeInstance_->setTimeScale(thread, 0.f);
    }
}

void Turret::playFireAnimation() {
    if (shapeInstance_ && animationThread_ && fireSequence_ >= 0) {
        shapeInstance_->setSequence(*animationThread_, fireSequence_, 0.f);
        shapeInstance_->setPriority(*animationThread_, 0);
        shapeInstance_->setTimeScale(*animationThread_, 1.f);
    }
}

void Turret::shoot() {
    if (!turretData_ || energy_ < turretData_->minGunEnergy) {
        unshoot();
        return;
    }
    if (getControlClient() == 0 && currentTarget_ && !hasTargetLOS(currentTarget_)) {
        return;
    }
    const auto firingTransform = [&]() {
        TMat3F muzzle;
        getMuzzleTransform(0, &muzzle);
        if (getControlClient() != 0) {
            const auto start = muzzle.p;
            muzzle = getEyeTransform();
            aimedTransform(&muzzle, start);
            muzzle.p = start;
        } else if (turretData_->deflection != 0.f) {
            static Random random;
            EulerF angles;
            muzzle.angles(angles);
            angles.x += (random.getFloat() - .5f) * kTwoPi * turretData_->deflection;
            angles.z += (random.getFloat() - .5f) * kTwoPi * turretData_->deflection;
            muzzle.set(angles, muzzle.p);
        }
        return muzzle;
    };
    auto* target = dynamic_cast<ShapeBase*>(currentTarget_);
    if (auto* player = dynamic_cast<Player*>(target); player && player->getMountObject()) {
        target = dynamic_cast<ShapeBase*>(player->getMountObject());
    }

    if (!isSustained_) {
        if (!hasPrimary_ && !hasSecondary_) {
            return;
        }

        if (getManager() && turretData_->projectile.type != -1 &&
            turretData_->projectile.dataType != -1) {
            const auto muzzle = firingTransform();
            if (auto proj = createProjectile(turretData_->projectile)) {
                proj->bindDataBlockManager(resolveDataBlockManager());
                proj->initProjectile(muzzle, Point3F{0.0f, 0.0f, 0.0f}, getId());
                proj->setTarget(target);
                const auto used = turretData_->maxGunEnergy > 0.f
                                      ? std::min(energy_, turretData_->maxGunEnergy)
                                      : 0.f;
                if (turretData_->maxGunEnergy > 0.0f) {
                    proj->setEnergy(used, used / turretData_->maxGunEnergy);
                }
                if (!getManager()->addObject(std::move(proj))) {
                    return;
                }
                energy_ -= used;
            } else {
                return;
            }
        } else {
            return;
        }
    } else if (!m_pCurrentProjectile_) {
        if (!hasPrimary_ && !hasSecondary_) {
            return;
        }
        if (getManager() && turretData_->projectile.type != -1 &&
            turretData_->projectile.dataType != -1) {
            const auto muzzle = firingTransform();
            if (auto proj = createProjectile(turretData_->projectile)) {
                proj->bindDataBlockManager(resolveDataBlockManager());
                proj->initProjectile(muzzle, Point3F{0.0f, 0.0f, 0.0f}, getId());
                proj->setTarget(target);
                auto* projectile = proj.get();
                if (!getManager()->addObject(std::move(proj))) {
                    return;
                }
                m_pCurrentProjectile_ = projectile;
                deleteNotify(*m_pCurrentProjectile_);
                m_lastSustainedUpdate_ =
                    getManager() ? static_cast<Int32>(getManager()->getCurrentTime() * 1000.0f) : 0;
            } else {
                return;
            }
        } else {
            return;
        }
    }

    fireCount_ = (fireCount_ + 1) & 0x3;
    setMaskBits(ShootingMask);
    shooting_ = true;
    playFireAnimation();
}

void Turret::unshoot() {
    if (shooting_) {
        shooting_ = false;
        if (m_pCurrentProjectile_) {
            auto* projectile = m_pCurrentProjectile_;
            m_pCurrentProjectile_ = nullptr;
            clearNotify(*projectile);
            projectile->shutOffProjectile();
            m_fireState_ = Reloading;
            m_waitScale_ = 1.0f;
            setMaskBits(ShootingMask);
        }
    }
}

bool Turret::trackAndFire(Flt32 interval) {
    if (!currentTarget_ || !turretData_) {
        return false;
    }

    Flt32 rot = 0.0f;
    Flt32 el = 0.0f;
    if (!inDeadZone(leadPosition(currentTarget_), &rot, &el)) {
        const Flt32 maxStep = turretData_->speed * interval;
        Flt32 dRot = rot - turretRotation_;
        if (dRot > kPi) {
            dRot -= kTwoPi;
        } else if (dRot < -kPi) {
            dRot += kTwoPi;
        }

        Flt32 dEl = el - turretElevation_;
        bool fire = true;
        if (dRot > maxStep) {
            dRot = maxStep;
            fire = false;
        } else if (dRot < -maxStep) {
            dRot = -maxStep;
            fire = false;
        }
        if (dEl > maxStep) {
            dEl = maxStep;
            fire = false;
        } else if (dEl < -maxStep) {
            dEl = -maxStep;
            fire = false;
        }

        if (dRot != 0.0f) {
            turretRotation_ = wrapRotation(turretRotation_ + dRot);
            setMaskBits(TRotationMask);
        }
        if (dEl != 0.0f) {
            turretElevation_ = wrapElevation(turretElevation_ + dEl);
            setMaskBits(ElevationMask);
        }
        return fire;
    }
    return false;
}

void Turret::serverProcessWaiting(Flt32 in_deltaT, UInt32 in_currTime) {
    if (!isSustained_) {
        if (currentTarget_) {
            extend();
            if (state_ == EXTENDED) {
                if (isTargetable(currentTarget_)) {
                    if (trackAndFire(in_deltaT)) {
                        m_fireState_ = Firing;
                        m_waitScale_ = 0.0f;
                        shoot();
                    }
                    return;
                }
                setCurrentTarget(nullptr);
                unshoot();
            }
        } else if (state_ == EXTENDED) {
            retract();
        }
        if (state_ == EXTENDED || state_ == RETRACTED) {
            chooseTarget();
        }
    } else {
        if (currentTarget_) {
            extend();
            if (state_ == EXTENDED) {
                if (isTargetable(currentTarget_)) {
                    if (trackAndFire(in_deltaT)) {
                        m_fireState_ = Firing;
                        m_waitScale_ = 0.0f;
                        shoot();
                        serverProcessFiring(0.0f, in_currTime);
                        setMaskBits(ShootingMask);
                    }
                    return;
                }
                setCurrentTarget(nullptr);
                unshoot();
            }
        } else if (state_ == EXTENDED) {
            retract();
        }
        if (state_ == EXTENDED || state_ == RETRACTED) {
            chooseTarget();
        }
    }
}

void Turret::serverProcessReloading(Flt32 in_deltaT, UInt32 in_currTime) {
    if (!turretData_) {
        return;
    }
    const Flt32 reloadDelay = std::max(0.001f, turretData_->reloadDelay);
    m_waitScale_ -= in_deltaT / reloadDelay;
    if (m_waitScale_ <= 0.0f) {
        const Flt32 overTime = (-m_waitScale_) * reloadDelay;
        m_waitScale_ = 0.0f;
        m_fireState_ = Waiting;
        serverProcessWaiting(overTime, in_currTime);
    } else if (!isSustained_) {
        if (currentTarget_) {
            if (!isTargetable(currentTarget_)) {
                setCurrentTarget(nullptr);
                unshoot();
            } else {
                trackAndFire(in_deltaT);
            }
        }
    }
}

void Turret::serverProcessFiring(Flt32 in_deltaT, UInt32 in_currTime) {
    if (!turretData_) {
        return;
    }
    if (!isSustained_) {
        const Flt32 fireDelay =
            std::max(0.001f, static_cast<Flt32>(turretData_->firingTime) * 0.001f);
        m_waitScale_ += in_deltaT / fireDelay;
        if (m_waitScale_ >= 1.0f) {
            const Flt32 overTime = (m_waitScale_ - 1.0f) * fireDelay;
            m_waitScale_ = 1.0f;
            m_fireState_ = Reloading;
            unshoot();
            setMaskBits(ShootingMask);
            serverProcessReloading(overTime, in_currTime);
        } else if (currentTarget_) {
            if (!isTargetable(currentTarget_)) {
                setCurrentTarget(nullptr);
                unshoot();
            } else {
                trackAndFire(in_deltaT);
            }
        }
    } else {
        if (!currentTarget_ || !m_pCurrentProjectile_) {
            unshoot();
            m_fireState_ = Reloading;
            m_waitScale_ = 1.0f;
            return;
        }
        extend();
        if (state_ == EXTENDED) {
            const Flt32 sustainedEnergy = turretData_->energyRate * in_deltaT;
            if (sustainedEnergy <= energy_ && isTargetable(currentTarget_)) {
                energy_ -= sustainedEnergy;
                if (trackAndFire(in_deltaT)) {
                    if (static_cast<Int32>(in_currTime) - m_lastSustainedUpdate_ >= 128) {
                        m_lastSustainedUpdate_ = static_cast<Int32>(in_currTime);
                        TMat3F muzzle{};
                        getMuzzleTransform(0, &muzzle);
                        m_pCurrentProjectile_->updateImageTransform(muzzle);
                    }
                } else {
                    unshoot();
                }
            } else {
                setCurrentTarget(nullptr);
                unshoot();
            }
        }
    }
}

void Turret::serverProcess(UInt32 curTime) {
    const Flt32 interval = curTime > getLastProcessTime()
                               ? static_cast<Flt32>(curTime - getLastProcessTime()) * 0.001f
                               : 0.f;
    Parent::serverProcess(curTime);
    advanceAnimation(interval);

    if (isActive() && getControlClient() == 0) {
        switch (m_fireState_) {
            case Waiting:
                serverProcessWaiting(interval, curTime);
                break;
            case Reloading:
                serverProcessReloading(interval, curTime);
                break;
            case Firing:
                serverProcessFiring(interval, curTime);
                break;
        }
    }
}

void Turret::clientProcess(UInt32 curTime) {
    const Flt32 interval = curTime > getLastProcessTime()
                               ? static_cast<Flt32>(curTime - getLastProcessTime()) * 0.001f
                               : 0.f;
    Parent::clientProcess(curTime);
    advanceAnimation(interval);

    if (interpTime_ > 0.0f) {
        const Flt32 delta = std::min(interval, interpTime_);
        interpTime_ -= delta;
        turretRotation_ = wrapRotation(turretRotation_ + delta * rotationStep_);
        turretElevation_ = wrapElevation(turretElevation_ + delta * elevationStep_);
    }

    if (ghostFireCount_ != fireCount_) {
        ghostFireCount_ = fireCount_;
        playFireAnimation();
        if (turretData_ && !isSustained_ && turretData_->fireSound >= 0)
            (void)TSFX::PlayAt(UInt32(turretData_->fireSound), getTransform(), {}, getManager());
    }
    updateTurretFireSound();
    TSFX::Move(moveSound_, getTransform(), {}, getManager());
    TSFX::Move(fireSound_, getTransform(), {}, getManager());
}

void Turret::updateTurretSounds(bool turning) {
    if (!isGhost() || !turretData_)
        return;
    if (turning && turretData_->whirSound >= 0) {
        if (TSFX::IsDone(moveSound_, getManager()))
            moveSound_ =
                TSFX::PlayAt(UInt32(turretData_->whirSound), getTransform(), {}, getManager());
    } else {
        TSFX::Stop(moveSound_, getManager());
    }
    updateTurretFireSound();
}

void Turret::updateTurretFireSound() {
    if (!isGhost() || !turretData_)
        return;
    if (isSustained_ && m_fireState_ == Firing && turretData_->fireSound >= 0) {
        if (!fireSound_)
            fireSound_ =
                TSFX::PlayAt(UInt32(turretData_->fireSound), getTransform(), {}, getManager());
    } else {
        TSFX::Stop(fireSound_, getManager());
    }
}

void Turret::serverUpdateMove(std::span<const PlayerMove> moves) {
    for (const auto& move : moves) {
        if (!isActive()) {
            break;
        }
        updateMove(&move, 0.032f);
    }
}

void Turret::updateMove(const PlayerMove* move, Flt32 interval) {
    if (!move || !isActive() || !turretData_) {
        return;
    }

    const Flt32 speedScale = std::min(turretData_->speed * 5.0f, 1.0f);
    Flt32 dPitch = move->pitch * speedScale;
    Flt32 dTurn = move->turnRot * speedScale;
    const Flt32 maxStep = turretData_->speed * interval;

    dPitch = std::clamp(dPitch, -maxStep, maxStep);
    dTurn = std::clamp(dTurn, -maxStep, maxStep);

    turretElevation_ = wrapElevation(turretElevation_ + dPitch);
    turretRotation_ = wrapRotation(turretRotation_ + dTurn);

    if (dPitch != 0.0f) {
        setMaskBits(ElevationMask);
    }
    if (dTurn != 0.0f) {
        setMaskBits(TRotationMask);
    }

    if (!isGhost()) {
        const Int32 curTimeMs =
            getManager() ? static_cast<Int32>(getManager()->getCurrentTime() * 1000.0f) : 0;
        switch (m_fireState_) {
            case Waiting:
                if (move->trigger) {
                    extend();
                    if (state_ == EXTENDED) {
                        m_fireState_ = Firing;
                        m_waitScale_ = 0.0f;
                        setMaskBits(ShootingMask);
                        shoot();
                    }
                }
                break;

            case Reloading: {
                const Flt32 reloadDelay = std::max(0.001f, turretData_->reloadDelay);
                m_waitScale_ -= interval / reloadDelay;
                if (m_waitScale_ <= 0.0f) {
                    m_waitScale_ = 0.0f;
                    m_fireState_ = Waiting;
                }
                break;
            }

            case Firing:
                if (!isSustained_) {
                    const Flt32 fireDelay =
                        std::max(0.001f, static_cast<Flt32>(turretData_->firingTime) * 0.001f);
                    m_waitScale_ += interval / fireDelay;
                    if (m_waitScale_ >= 1.0f) {
                        m_waitScale_ = 1.0f;
                        m_fireState_ = Reloading;
                        unshoot();
                        setMaskBits(ShootingMask);
                    }
                } else {
                    if (move->trigger) {
                        extend();
                        if (state_ == EXTENDED) {
                            const Flt32 sustainedEnergy = turretData_->energyRate * interval;
                            if (sustainedEnergy <= energy_ && m_pCurrentProjectile_) {
                                energy_ -= sustainedEnergy;
                                if (curTimeMs - m_lastSustainedUpdate_ >= 128) {
                                    m_lastSustainedUpdate_ = curTimeMs;
                                    TMat3F muzzle{};
                                    getMuzzleTransform(0, &muzzle);
                                    m_pCurrentProjectile_->updateImageTransform(muzzle);
                                }
                            } else {
                                unshoot();
                            }
                        }
                    } else {
                        unshoot();
                    }
                }
                break;
        }
    }
}

bool Turret::isControlable() const noexcept {
    return isActive();
}

bool Turret::cameraZoomAndCrosshairsEnabled() const noexcept {
    return true;
}

const TMat3F& Turret::getEyeTransform() const {
    TMat3F local{};
    if (const auto pose = turretPose(); pose && cameraNode_ >= 0) {
        local.set(EulerF{turretElevation_, 0.0f, turretRotation_},
                  pose->nodeTransforms()[cameraNode_].p);
    }
    m_mul(local, getTransform(), cachedEyeTransform_);
    return cachedEyeTransform_;
}

std::optional<world::TsShapePose> Turret::turretPose() const {
    if (!shapeInstance_) {
        return {};
    }
    const auto& shape = *shapeInstance_->shape();
    // Keep StaticBase's visibility/ambient and script threads. Sampling only
    // the aiming sequences drops the intact/hulk visibility selection and
    // draws both sets of meshes on authored turret shapes.
    auto instance = *shapeInstance_;
    for (auto i : std::views::iota(std::size_t{0}, shape.sequences.size())) {
        const auto name = shape.sequences[i].name;
        if (name < 0 || std::size_t(name) >= shape.names.size()) {
            continue;
        }
        Flt32 position;
        if (shape.names[name] == "turn") {
            position = maxRotation_ != minRotation_
                           ? (turretRotation_ - minRotation_) / (maxRotation_ - minRotation_)
                           : turretRotation_ / kTwoPi;
        } else if (shape.names[name] == "elevate") {
            position = maxElevation_ != minElevation_
                           ? (turretElevation_ - minElevation_) / (maxElevation_ - minElevation_)
                           : 0.f;
        } else {
            continue;
        }
        position = std::clamp(position, 0.f, 1.f);
        const auto threads = instance.threads();
        const auto existing = std::find_if(threads.begin(), threads.end(), [i](const auto& thread) {
            return thread.sequence == Int32(i);
        });
        if (existing != threads.end()) {
            if (!instance.setSequence(UInt32(existing - threads.begin()), Int32(i), position)) {
                return {};
            }
        } else if (!instance.addThread(Int32(i), position)) {
            return {};
        }
    }
    return instance.pose();
}

void Turret::getCameraTransform(Flt32 camDist, TMat3F* transform) const {
    if (!transform) {
        return;
    }
    *transform = getEyeTransform();
    if (camDist > 0.0f) {
        Point3F fwd{};
        transform->getRow(1, fwd);
        transform->p -= fwd * camDist;
    }
}

bool Turret::getMuzzleTransform(Int32 /*slot*/, TMat3F* transform) const {
    if (!transform) {
        return false;
    }
    *transform = getTransform();
    if (const auto pose = turretPose(); pose && gunNode_ >= 0) {
        const TMat3F local(EulerF{turretElevation_, 0.f, turretRotation_},
                           pose->nodeTransforms()[gunNode_].p);
        m_mul(local, getTransform(), *transform);
    }
    return true;
}

bool Turret::getWeaponAmmoType(ProjectileDataType& outPrimary,
                               ProjectileDataType& outSecondary) const {
    if (!turretData_) {
        outPrimary = {};
        outSecondary = {};
        return false;
    }
    outPrimary = turretData_->projectile;
    outSecondary = {};
    return true;
}

bool Turret::onSimLightQuery(sim::SimLightQuery& /*query*/) const {
    return false;
}

bool Turret::processQuery(sim::SimQuery& query) {
    if (auto* lq = dynamic_cast<sim::SimLightQuery*>(&query)) {
        return onSimLightQuery(*lq);
    }
    const auto result = Parent::processQuery(query);
    if (auto* shapes = dynamic_cast<world::WorldShapeQuery*>(&query); shapes && shapes->image) {
        if (const auto pose = turretPose()) {
            shapes->image->pose = *pose;
        }
    }
    return result;
}

void Turret::writePacketData(BitStream& bstream) const {
    writePod(bstream, turretRotation_);
    writePod(bstream, turretElevation_);
    bstream.writeInt(fireCount_, 2);
    bstream.writeFlag(m_fireState_ == Firing);
    bstream.writeInt(static_cast<Int32>(state_), 4);
    writePod(bstream, energy_);
}

bool Turret::readPacketData(BitStream& bstream) {
    BitStreamRollbackGuard guard(bstream);

    Flt32 newRot = 0.0f;
    Flt32 newElev = 0.0f;
    if (!readPod(bstream, newRot) || !readPod(bstream, newElev)) {
        return false;
    }
    const Int32 newFireCount = bstream.readInt(2);
    const bool isFiring = bstream.readFlag();
    const auto newState = static_cast<State>(bstream.readInt(4));
    Flt32 newEnergy = 0.0f;
    if (!readPod(bstream, newEnergy) || !bstream.isValid()) {
        return false;
    }

    turretRotation_ = newRot;
    turretElevation_ = newElev;
    fireCount_ = newFireCount;
    m_fireState_ = isFiring ? Firing : Waiting;
    if (state_ != newState) {
        setAnimationState(newState);
    }
    energy_ = newEnergy;
    updateTurretSounds(newRot != desiredRotation_ || newElev != desiredElevation_);
    desiredRotation_ = newRot;
    desiredElevation_ = newElev;

    guard.committed = true;
    return true;
}

UInt32 Turret::packUpdate(sim::network::GhostManager& g, UInt32 mask, BitStream& stream) const {
    const bool myClient = g.owner() && getControlClient() == static_cast<Int32>(g.owner()->getId());
    if (myClient && (mask & (InitialUpdate | BaseStateMask)) == 0) {
        stream.writeFlag(true);
        return 0;
    }
    stream.writeFlag(false);

    Parent::packUpdate(g, mask, stream);

    if (stream.writeFlag((mask & TRotationMask) != 0)) {
        stream.writeFloat(std::clamp(turretRotation_ / kTwoPi, 0.0f, 1.0f), 8);
    }
    if (stream.writeFlag((mask & ElevationMask) != 0)) {
        const Flt32 denom = (maxElevation_ - minElevation_);
        const Flt32 norm = (denom > 1e-5f) ? ((turretElevation_ - minElevation_) / denom) : 0.5f;
        stream.writeFloat(std::clamp(norm, 0.0f, 1.0f), 8);
    }
    if (stream.writeFlag((mask & ShootingMask) != 0)) {
        stream.writeInt(fireCount_, 2);
    }
    if (stream.writeFlag((mask & StateMask) != 0)) {
        stream.writeInt(static_cast<Int32>(state_), 4);
    }
    stream.writeFlag(m_fireState_ == Firing);
    return 0;
}

bool Turret::unpackUpdate(sim::network::GhostManager& g, BitStream& stream) {
    BitStreamRollbackGuard guard(stream);

    const auto savedDatId = datFileId_;
    const auto savedRot = turretRotation_;
    const auto savedElev = turretElevation_;
    const auto savedDesRot = desiredRotation_;
    const auto savedDesElev = desiredElevation_;
    const auto savedRotStep = rotationStep_;
    const auto savedElevStep = elevationStep_;
    const auto savedInterpTime = interpTime_;
    const auto savedFireCount = fireCount_;
    const auto savedState = state_;
    const auto savedFireState = m_fireState_;

    const auto rollback = [&]() noexcept {
        datFileId_ = savedDatId;
        turretRotation_ = savedRot;
        turretElevation_ = savedElev;
        desiredRotation_ = savedDesRot;
        desiredElevation_ = savedDesElev;
        rotationStep_ = savedRotStep;
        elevationStep_ = savedElevStep;
        interpTime_ = savedInterpTime;
        fireCount_ = savedFireCount;
        state_ = savedState;
        m_fireState_ = savedFireState;
        return false;
    };

    if (stream.readFlag()) {
        if (!stream.isValid()) {
            return rollback();
        }
        guard.committed = true;
        return true;
    }

    if (!Parent::unpackUpdate(g, stream)) {
        return rollback();
    }

    if (stream.readFlag()) {
        const Flt32 frac = stream.readFloat(8);
        if (!stream.isValid()) {
            return rollback();
        }
        desiredRotation_ = frac * kTwoPi;
        Flt32 rotDiff = desiredRotation_ - turretRotation_;
        if (rotDiff > kPi) {
            rotDiff -= kTwoPi;
        } else if (rotDiff < -kPi) {
            rotDiff += kTwoPi;
        }
        rotationStep_ = rotDiff * 4.0f;
        interpTime_ = 0.25f;
        if (!isProperlyAdded()) {
            turretRotation_ = desiredRotation_;
            interpTime_ = 0.0f;
        }
    } else {
        rotationStep_ = 0.0f;
    }

    if (stream.readFlag()) {
        const Flt32 frac = stream.readFloat(8);
        if (!stream.isValid()) {
            return rollback();
        }
        desiredElevation_ = minElevation_ + frac * (maxElevation_ - minElevation_);
        const Flt32 elevDiff = desiredElevation_ - turretElevation_;
        elevationStep_ = elevDiff * 4.0f;
        interpTime_ = 0.25f;
        if (!isProperlyAdded()) {
            turretElevation_ = desiredElevation_;
            interpTime_ = 0.0f;
        }
    } else {
        elevationStep_ = 0.0f;
    }

    if (stream.readFlag()) {
        fireCount_ = stream.readInt(2);
    }
    if (stream.readFlag()) {
        state_ = static_cast<State>(stream.readInt(4));
    }

    const bool isFiring = stream.readFlag();
    if (!stream.isValid()) {
        return rollback();
    }
    m_fireState_ = isFiring ? Firing : Waiting;
    const auto newState = state_;
    state_ = savedState;
    if (newState != state_) {
        setAnimationState(newState);
    }
    if (!isProperlyAdded()) {
        ghostFireCount_ = fireCount_;
    }
    updateTurretSounds(desiredRotation_ != savedDesRot || desiredElevation_ != savedDesElev);

    guard.committed = true;
    return true;
}

Persistent::Error
Turret::read(StreamIO& sio, Int32 version, Int32 user, Persistent::LoadContext& ctx) {
    const Int32 startPos = sio.getPosition();
    if (Parent::read(sio, version, user, ctx) != Persistent::Error::Ok) {
        sio.setPosition(startPos);
        return Persistent::Error::ReadError;
    }
    UInt32 vers = 0;
    if (!readStreamPod(sio, vers)) {
        sio.setPosition(startPos);
        return Persistent::Error::ReadError;
    }
    return Persistent::Error::Ok;
}

Persistent::Error
Turret::write(StreamIO& sio, Int32 version, Int32 user, Persistent::StoreContext& ctx) const {
    const Int32 startPos = sio.getPosition();
    if (Parent::write(sio, version, user, ctx) != Persistent::Error::Ok) {
        sio.setPosition(startPos);
        return Persistent::Error::WriteError;
    }
    const UInt32 vers = 0;
    if (!writeStreamPod(sio, vers)) {
        sio.setPosition(startPos);
        return Persistent::Error::WriteError;
    }
    return Persistent::Error::Ok;
}

bool registerTurretClass(Persistent::ClassRegistry& registry) {
    return registry.registerClass<Turret>("Turret", FearTurretPersTag);
}

bool registerTurretNetwork(sim::network::Registry& registry) {
    return registry.registerGhost<Turret>(static_cast<UInt16>(FearTurretPersTag));
}

} // namespace opent1::game
