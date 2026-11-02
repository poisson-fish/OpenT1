#include <opent1/game/ai.hpp>
#include <opent1/core/text.hpp>
#include <opent1/game/projectile.hpp>
#include <opent1/game/proj_target_laser.hpp>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <format>
#include <sstream>

namespace opent1::game {

Random AIObj::rng_{Random::defaultSeed};

namespace {

[[nodiscard]] Flt32 parseFloatSv(std::string_view sv) noexcept {
    Flt32 val = 0.0f;
    std::from_chars(sv.data(), sv.data() + sv.size(), val);
    return val;
}

[[nodiscard]] Int32 parseIntSv(std::string_view sv) noexcept {
    Int32 val = 0;
    std::from_chars(sv.data(), sv.data() + sv.size(), val);
    return val;
}

[[nodiscard]] constexpr Flt32 scaleBetween(Flt32 a, Flt32 b, Flt32 pct) noexcept {
    return a + (b - a) * pct;
}

[[nodiscard]] constexpr Flt32 getPercentBetween(Flt32 val, Flt32 a, Flt32 b) noexcept {
    return (val - a) / (b - a);
}

[[nodiscard]] Flt32 arcTangent(Flt32 x, Flt32 y) noexcept {
    return (isZero(x) && isZero(y)) ? 0.0f : m_atan(x, y);
}

} // namespace

void AIObj::Incrementor::init(Flt32 inc) noexcept {
    increment = inc;
    reduce = static_cast<Flt32>(2.0 * static_cast<RealD>(inc));
    min_inc = static_cast<Flt32>(0.1 * static_cast<RealD>(inc));
}

Flt32 AIObj::Incrementor::getInc(Flt32 absDiff) const noexcept {
    Flt32 inc = increment;
    if (absDiff < reduce) {
        inc *= (absDiff / reduce);
        if (inc < min_inc) {
            inc = min_inc;
        }
        if (inc > absDiff) {
            inc = absDiff;
        }
    }
    return inc;
}

AIObj::IQVariables::IQVariables(bool isBest) noexcept {
    spotDistOverride = -1.0f;
    if (isBest) {
        thinkWait = 6;
        spotDist = 150.0f;
        maxSpeed = 1.0f;
        pitchInc.init(0.03f);
        turnInc.init(0.07f);
        vertDope[0] = 0.0f;
        vertDope[1] = 0.2f;
    } else {
        thinkWait = 20;
        spotDist = 50.0f;
        maxSpeed = 0.4f;
        pitchInc.init(0.008f);
        turnInc.init(0.05f);
        vertDope[0] = 1.3f;
        vertDope[1] = 4.0f;
    }
    smartGuyWeaponRange = 145.0f;
    dumbGuyWeaponRange = 55.0f;
    smartGuyMinAccuracy = 0.2f;
}

void AIObj::IQVariables::scale(Flt32 unitScale) noexcept {
    const IQVariables best(true);
    const IQVariables worst(false);

    thinkWait = static_cast<Int32>(scaleBetween(
        static_cast<Flt32>(worst.thinkWait), static_cast<Flt32>(best.thinkWait), unitScale));
    spotDist = scaleBetween(worst.spotDist, best.spotDist, unitScale);
    maxSpeed = scaleBetween(worst.maxSpeed, best.maxSpeed, unitScale);

    pitchInc.init(scaleBetween(0.008f, 0.03f, unitScale));
    turnInc.init(scaleBetween(0.05f, 0.07f, unitScale));

    for (std::size_t i = 0; i < 2; ++i) {
        vertDope[i] = scaleBetween(worst.vertDope[i], best.vertDope[i], unitScale);
    }

    curWeaponRange = scaleBetween(dumbGuyWeaponRange, smartGuyWeaponRange, unitScale);
    curWeaponMinPct = scaleBetween(0.0f, smartGuyMinAccuracy, unitScale);
    curWeaponMaxPct = unitScale;
}

Flt32 AIObj::IQVariables::getWeaponAccuracy(Flt32 range) const noexcept {
    Flt32 pct = getPercentBetween(range, 0.0f, curWeaponRange);
    pct = scaleBetween(curWeaponMaxPct, curWeaponMinPct, pct);
    return pct;
}

Flt32 AIObj::IQVariables::spotDistance() const noexcept {
    return (spotDistOverride < 0.0f) ? spotDist : spotDistOverride;
}

bool AIObj::Trigger::press(Random& rng) noexcept {
    wasPressed = 24;
    if (counter < windup) {
        return true;
    }
    const Flt32 pct = std::clamp(percent, 0.0f, 1.0f);
    return rng.getFloat() < pct;
}

void AIObj::Trigger::preLoop() noexcept {
    if (wasPressed != 0) {
        if (++counter > windup) {
            counter = windup;
        }
        if (--wasPressed <= 0) {
            wasPressed = 0;
        }
    } else {
        counter = 0;
    }
}

Flt32 AIObj::AngleSeek::getStep(Flt32* delta) const noexcept {
    const Flt32 twoPi = static_cast<Flt32>(2.0 * kRealDPi);
    const Flt32 pi = static_cast<Flt32>(kRealDPi);

    Flt32 cur = curAngle;
    Flt32 targ = targetAngle;
    while (cur < 0.0f) {
        cur += twoPi;
    }
    while (cur >= twoPi) {
        cur -= twoPi;
    }
    while (targ < 0.0f) {
        targ += twoPi;
    }
    while (targ >= twoPi) {
        targ -= twoPi;
    }

    Flt32 d = targ - cur;
    if (d > pi) {
        d -= twoPi;
    } else if (d < -pi) {
        d += twoPi;
    }
    if (delta) {
        *delta = d;
    }

    if (std::abs(d) <= maxSpeed) {
        return d;
    }
    return (d > 0.0f) ? maxSpeed : -maxSpeed;
}

Flt32 AIObj::AngleSeek::step(Flt32* delta) noexcept {
    const Flt32 twoPi = static_cast<Flt32>(2.0 * kRealDPi);
    const Flt32 s = getStep(delta);
    curAngle += s;
    while (curAngle < 0.0f) {
        curAngle += twoPi;
    }
    while (curAngle >= twoPi) {
        curAngle -= twoPi;
    }
    return s;
}

void AIObj::JetSkill::init(const AIObj& ai, const Point3F& dest) noexcept {
    startPos = ai.player() ? ai.player()->getBoxCenter() : Point3F{};
    destPos = dest;
    midpoint = (startPos + destPos) * .5f;
    midpoint.z += 40.f;
    stage = Jump;
    jumpCount = 0;
    savedVelocity = {};
}

bool AIObj::JetSkill::jetToPoint(AIObj& ai, PlayerMove& pm, const Point3F& dest) noexcept {
    auto* player = ai.player();
    const auto* data = player ? dynamic_cast<const PlayerData*>(player->getDatPtr()) : nullptr;
    if (!player || !data || stage == NoNav) {
        return false;
    }
    (void)dest;
    pm.forwardAction = pm.backwardAction = pm.leftAction = pm.rightAction = 0;
    const auto position = player->getBoxCenter();
    const auto velocity = player->getLinearVelocity();
    if (stage == Jump) {
        const auto heading = ai.getAngleSeekZ(destPos, position, player->getRot().z);
        pm.turnRot = std::copysign(ai.iqVars_.turnInc.getInc(heading.absDiff), heading.diff);
        if (heading.absDiff < .08f && velocity.len() < .3f) {
            pm.jumpAction = true;
            stage = Jumped;
            jumpCount = 0;
        }
    } else if (stage == Jumped) {
        pm.jetting = true;
        if (jumpCount++ > 10) {
            stage = StartFlight;
            savedVelocity = velocity;
        }
    } else if (stage == StartFlight || stage == StartFlight + 1) {
        const auto start = stage == StartFlight ? startPos : midpoint;
        const auto finish = stage == StartFlight ? midpoint : destPos;
        const auto delta = finish - position;
        const auto remaining = delta.len();
        if (remaining > .01f) {
            auto direction = delta * (1.f / remaining);
            const auto total = (finish - start).len();
            const auto fraction = total > 0 ? std::clamp(1.f - remaining / total, 0.f, 1.f) : 1.f;
            const auto endScale = stage == StartFlight ? .2f : .04f;
            const auto desiredSpeed =
                (.2f + (endScale - .2f) * fraction) * data->maxJetForwardVelocity;
            auto actualForce = velocity - savedVelocity;
            if (actualForce.len() > 10) {
                actualForce = {};
            }
            const auto projection = std::min(m_dot(actualForce, direction), desiredSpeed);
            const auto desired = direction * (desiredSpeed + projection) - actualForce;
            savedVelocity = velocity;
            direction.z = 0;
            const auto lateral = direction.len();
            if (lateral > .001f) {
                direction *= 1.f / lateral;
            }
            std::array<Point3F, 4> forces;
            const auto count =
                player->possibleJetForces(forces, lateral > .001f ? &direction : nullptr);
            Int32 best = 0;
            for (Int32 n = 1; n < count; ++n) {
                if ((forces[n] - desired).len() < (forces[best] - desired).len()) {
                    best = n;
                }
            }
            if (best) {
                pm.jetting = true;
            }
            if (best >= 2) {
                auto heading = ai.getAngleSeekZ(finish, position, player->getRot().z);
                if (best == 3) {
                    heading.desired += Flt32(kRealDPi);
                }
                AIObj::setMovementAction(pm, heading, 1.f);
            }
        }
        if (std::hypot(delta.x, delta.y) < 1.f) {
            ++stage;
        }
        if (stage > StartFlight + 1 || velocity.len() < .03f) {
            stage = NoNav;
        }
    } else {
        stage = NoNav;
    }
    // aiSkill.cpp deliberately replenishes energy in this opt-in development path.
    if (ai.rng_.getFloat() < .99f) {
        player->setEnergy(data->maxEnergy);
    }
    return stage != NoNav;
}

AIObj::AIObj(AIManager* mgr) : aiManager_(mgr) {
    resetBase();
    callbacks_[AIKilled] = "AI::onDroneKilled";
    callbacks_[Periodic] = "AI::onPeriodic";
    scaleIQVariables();
}

AIObj::~AIObj() = default;

void AIObj::scaleIQVariables() noexcept {
    const Flt32 t = std::clamp(iq_ / 100.0f, 0.0f, 1.0f);
    iqVars_.scale(t);
    iqVars_.maxTurnSpeed = iqVars_.turnInc.increment;
    iqVars_.maxPitchSpeed = iqVars_.pitchInc.increment;
    iqVars_.dopeBase = iqVars_.vertDope[0];
    // Trigger settings are independent console variables; IQ changes must not overwrite them.
    losThinkCounter_ = 0;
    iqVars_.vertEvade = static_cast<Int32>(std::lround(scaleBetween(0.0f, 4.0f, t)));
    iqVars_.horizEvade = static_cast<Int32>(std::lround(scaleBetween(0.0f, 5.0f, t)));

    yawSeek_.maxSpeed = iqVars_.maxTurnSpeed;
    pitchSeek_.maxSpeed = iqVars_.maxPitchSpeed;
}

Flt32 AIObj::wrapRotation(Flt32 r, Flt32 rangeMin) noexcept {
    const Flt32 twoPi = static_cast<Flt32>(kRealD2Pi);
    while ((r - rangeMin) > twoPi) {
        r -= twoPi;
    }
    while ((r - rangeMin) < 0.0f) {
        r += twoPi;
    }
    return r;
}

Flt32 AIObj::getDesiredRot(const Point3F& dest, const Point3F& source) noexcept {
    return static_cast<Flt32>(static_cast<RealD>(arcTangent(dest.x - source.x, dest.y - source.y)) -
                              (kRealDPi / 2.0));
}

Flt32 AIObj::getDesiredPitch(const Point3F& dest,
                             const Point3F& source,
                             bool accurateMath) noexcept {
    const Point2F xyDst(dest.x, dest.y);
    const Point2F xySrc(source.x, source.y);
    const Flt32 horizontalDist = accurateMath ? m_dist(xyDst, xySrc) : m_distf(xyDst, xySrc);
    return arcTangent(horizontalDist, dest.z - source.z);
}

void AIObj::setMovementAction(PlayerMove& pm, const AngleSeek& heading, Flt32 speed) noexcept {
    const Flt32 diffAngle = heading.desired - heading.current;
    const Flt32 lrAction = m_sin(diffAngle) * speed;
    const Flt32 fbAction = m_cos(diffAngle) * speed;

    if (lrAction > 0.0f) {
        pm.leftAction = lrAction;
        pm.rightAction = 0.0f;
    } else {
        pm.rightAction = -lrAction;
        pm.leftAction = 0.0f;
    }

    if (fbAction > 0.0f) {
        pm.forwardAction = fbAction;
        pm.backwardAction = 0.0f;
    } else {
        pm.backwardAction = -fbAction;
        pm.forwardAction = 0.0f;
    }
}

Flt32 AIObj::getSpeed(Flt32 dist) const noexcept {
    constexpr Flt32 kArrivalDistance = 0.2f;
    constexpr Flt32 kSlowdownDistance = 1.7f;
    if (dist < kSlowdownDistance) {
        if (dist > kArrivalDistance) {
            const Flt32 pct = (dist - kArrivalDistance) / (kSlowdownDistance - kArrivalDistance);
            Flt32 minSpeed = iqVars_.maxSpeed / 3.0f;
            if (minSpeed > 0.25f) {
                minSpeed = 0.25f;
            }
            return scaleBetween(minSpeed, iqVars_.maxSpeed, pct);
        }
        return 0.0f;
    }
    return iqVars_.maxSpeed;
}

AIObj::AngleSeek AIObj::getAngleSeekZ(const Point3F& seekLoc,
                                      const Point3F& ourCenter,
                                      Flt32 playerRotZ) const noexcept {
    const Flt32 twoPi = static_cast<Flt32>(kRealD2Pi);
    AngleSeek z(wrapRotation(getDesiredRot(seekLoc, ourCenter)));
    z.current = wrapRotation(playerRotZ, twoPi);
    const Flt32 pi = static_cast<Flt32>(kRealDPi + 0.000001f);
    while ((z.absDiff = std::fabs(z.diff = z.desired - z.current)) > pi) {
        z.desired += twoPi;
    }
    return z;
}

bool AIObj::setVar(std::string_view varName, std::string_view value) {
    if (equalIgnoreCase(varName, "iq")) {
        iq_ = parseFloatSv(value);
        scaleIQVariables();
        return true;
    }
    if (equalIgnoreCase(varName, "attackMode")) {
        attackMode_ = parseIntSv(value);
        return true;
    }
    if (equalIgnoreCase(varName, "spotDist")) {
        iqVars_.spotDistOverride = parseFloatSv(value);
        scaleIQVariables();
        return true;
    }
    if (equalIgnoreCase(varName, "smartGuyWeaponRange")) {
        iqVars_.smartGuyWeaponRange = parseFloatSv(value);
        scaleIQVariables();
        return true;
    }
    if (equalIgnoreCase(varName, "dumbGuyWeaponRange")) {
        iqVars_.dumbGuyWeaponRange = parseFloatSv(value);
        scaleIQVariables();
        return true;
    }
    if (equalIgnoreCase(varName, "smartGuyMinAccuracy")) {
        iqVars_.smartGuyMinAccuracy = parseFloatSv(value);
        scaleIQVariables();
        return true;
    }
    if (equalIgnoreCase(varName, "dopeOffset") || equalIgnoreCase(varName, "dopeBase")) {
        iqVars_.dopeBase = std::max(0.0f, parseFloatSv(value));
        return true;
    }
    if (equalIgnoreCase(varName, "thinkWait")) {
        iqVars_.thinkWait = std::max(0, parseIntSv(value));
        return true;
    }
    if (equalIgnoreCase(varName, "triggerPct")) {
        iqVars_.triggerPct = std::clamp(parseFloatSv(value), 0.0f, 1.0f);
        trigger_.percent = iqVars_.triggerPct;
        return true;
    }
    if (equalIgnoreCase(varName, "vertEvade")) {
        iqVars_.vertEvade = std::clamp(parseIntSv(value), 0, 3);
        return true;
    }
    if (equalIgnoreCase(varName, "horizEvade")) {
        iqVars_.horizEvade = std::clamp(parseIntSv(value), 0, 3);
        return true;
    }
    if (equalIgnoreCase(varName, "seekOff")) {
        std::istringstream input{std::string(value)};
        Point3F offset{};
        input >> offset.x >> offset.y >> offset.z;
        seekOff_ = offset;
        return true;
    }
    if (equalIgnoreCase(varName, "pathType")) {
        if (equalIgnoreCase(value, "oneWay")) {
            pathType_ = OneWay;
        } else if (equalIgnoreCase(value, "twoWay")) {
            pathType_ = TwoWay;
        } else if (equalIgnoreCase(value, "circular")) {
            pathType_ = Circular;
        } else {
            pathType_ = std::clamp(parseIntSv(value), 0, 2);
        }
        return true;
    }
    if (equalIgnoreCase(varName, "windup") || equalIgnoreCase(varName, "triggerWindup")) {
        windup_ = std::max(0.0f, parseFloatSv(value));
        trigger_.windup = Int32(windup_);
        return true;
    }
    if (equalIgnoreCase(varName, "jetNavigation")) {
        jetNavigation_ = parseFloatSv(value);
        if (jetNavigation_ == 0) {
            jetSkill_.stage = JetSkill::NoNav;
        }
        return true;
    }
    if (equalIgnoreCase(varName, "automaticTargets")) {
        automaticTargets_ = (parseIntSv(value) != 0 || equalIgnoreCase(value, "true"));
        return true;
    }
    return false;
}

std::string AIObj::getVar(std::string_view varName) const {
    if (equalIgnoreCase(varName, "jetNavigation")) {
        return std::format("{:.2f}", jetNavigation_);
    }
    if (equalIgnoreCase(varName, "iq")) {
        return std::format("{:.2f}", iq_);
    }
    if (equalIgnoreCase(varName, "attackMode")) {
        return std::format("{}", attackMode_);
    }
    if (equalIgnoreCase(varName, "spotDist")) {
        return std::format("{:.2f}", iqVars_.spotDistOverride);
    }
    if (equalIgnoreCase(varName, "smartGuyWeaponRange"))
        return std::format("{:.2f}", iqVars_.smartGuyWeaponRange);
    if (equalIgnoreCase(varName, "dumbGuyWeaponRange"))
        return std::format("{:.2f}", iqVars_.dumbGuyWeaponRange);
    if (equalIgnoreCase(varName, "smartGuyMinAccuracy")) {
        return std::format("{:.2f}", iqVars_.smartGuyMinAccuracy);
    }
    if (equalIgnoreCase(varName, "dopeOffset") || equalIgnoreCase(varName, "dopeBase")) {
        return std::format("{:.2f}", iqVars_.dopeBase);
    }
    if (equalIgnoreCase(varName, "thinkWait")) {
        return std::format("{}", iqVars_.thinkWait);
    }
    if (equalIgnoreCase(varName, "triggerPct")) {
        return std::format("{:.2f}", iqVars_.triggerPct);
    }
    if (equalIgnoreCase(varName, "vertEvade")) {
        return std::format("{}", iqVars_.vertEvade);
    }
    if (equalIgnoreCase(varName, "horizEvade")) {
        return std::format("{}", iqVars_.horizEvade);
    }
    if (equalIgnoreCase(varName, "seekOff")) {
        return std::format("{:.6f} {:.6f} {:.6f}", seekOff_.x, seekOff_.y, seekOff_.z);
    }
    if (equalIgnoreCase(varName, "pathType")) {
        return std::format("{}", pathType_);
    }
    if (equalIgnoreCase(varName, "windup") || equalIgnoreCase(varName, "triggerWindup")) {
        return std::format("{:.2f}", windup_);
    }
    if (equalIgnoreCase(varName, "automaticTargets")) {
        return automaticTargets_ ? "1" : "0";
    }
    return {};
}

bool AIObj::getTargetPosition(Point3F& pos, const DynamicTarget& targ) const noexcept {
    if (targ.obj) {
        pos = targ.obj->getBoxCenter();
        return true;
    }
    if (targ.id >= 0) {
        pos = targ.pos;
        return true;
    }
    pos = targ.pos;
    return false;
}

Flt32 AIObj::getProjectileSpeed(Int32 /*weaponImageType*/) const noexcept {
    if (!player_) {
        return 0;
    }
    ProjectileDataType primary, secondary;
    auto* dbm = player_->resolveDataBlockManager();
    if (!dbm || !player_->getWeaponAmmoType(primary, secondary) || primary.type < 0) {
        return 0;
    }
    const auto* data =
        dynamic_cast<const ProjectileData*>(dbm->lookupDataBlock(primary.dataType, primary.type));
    return data ? data->getTerminalVelocity() : 0;
}

bool AIObj::leadPosition(const DynamicTarget& targ,
                         Flt32 projSpeed,
                         Point3F& outPos) const noexcept {
    Point3F targetBasePos = targ.pos;
    if (targ.obj) {
        targetBasePos = targ.obj->getBoxCenter();
    }
    if (projSpeed <= 0.0f) {
        outPos = targetBasePos;
        return true;
    }
    const Point3F myPos = player_ ? player_->getLinearPosition() : Point3F{0.0f, 0.0f, 0.0f};
    const Flt32 dist = (targetBasePos - myPos).len();
    const Flt32 time = dist / projSpeed;
    const Point3F targetVel = (targ.obj ? targ.obj->getLeadVelocity() : targ.vel) -
                              (player_ ? player_->getLeadVelocity() : Point3F{});
    outPos = targetBasePos + (targetVel * time);
    return true;
}

Point3F AIObj::targetDope(const Point3F& target, Flt32 width, Flt32 height) noexcept {
    const Point3F source = player_ ? player_->getBoxCenter() : Point3F{};
    auto direction = target - source;
    const auto range = direction.len();
    if (range == 0) {
        direction = {1, 0, 0};
    } else {
        direction *= 1.f / range;
    }
    Point3F sideways;
    m_cross(Point3F{0, 0, 1}, direction, sideways);
    const Flt32 scale = std::clamp(iq_ / 100.f, 0.f, 1.f);
    const bool hit = rng_.getFloat() < std::clamp(iqVars_.getWeaponAccuracy(range), 0.f, 1.f);
    const Flt32 misfire = 2.2f + (1.f - 2.2f) * scale;
    Flt32 distance = hit ? 1.f - scale : misfire + rng_.getFloat(0.f, misfire);
    const Flt32 angle =
        Flt32(wrappedFireAngle_) * Flt32(kRealD2Pi) / 256.f + rng_.getFloat(-.3f, .3f);
    if (rng_.getFloat() < .001f) {
        wrappedFireAngle_ += 0x70;
    }
    if (range <= 1.7f) {
        distance = 0;
    } else if (range < 22.f) {
        const auto fraction = (range - 1.7f) / (22.f - 1.7f);
        distance *= fraction * fraction;
    }
    return Point3F{0, 0, height * m_sin(angle) * distance} +
           sideways * (width * m_cos(angle) * distance);
}

void AIObj::dopeOffset(Flt32 dist, Point3F& offset) noexcept {
    const auto source = player_ ? player_->getBoxCenter() : Point3F{};
    offset = targetDope(source + Point3F{0, dist, 0}, .6f, 1.2f);
}

void AIObj::seekLocation(PlayerMove* pm) {
    if (!pm || !player_) {
        return;
    }
    const auto source = player_->getBoxCenter();
    const Flt32 distance = (curDest_ - source).len();
    if (distance <= 1.f) {
        return;
    }
    const auto heading = getAngleSeekZ(curDest_, source, player_->getRot().z);
    pm->turnRot = std::copysign(iqVars_.turnInc.getInc(heading.absDiff), heading.diff);
    setMovementAction(*pm, heading, getSpeed(distance));
    if (jetSkill_.stage != JetSkill::NoNav) {
        (void)jetSkill_.jetToPoint(*this, *pm, curDest_);
    }
}

void AIObj::seekDirection(PlayerMove* pm) {
    if (!pm) {
        return;
    }
    pm->turnRot = yawSeek_.step();
    pm->pitch = pitchSeek_.step();
}

void AIObj::seekSpin(PlayerMove* pm) {
    if (!pm) {
        return;
    }
    yawSeek_.targetAngle = yawSeek_.curAngle + yawSeek_.maxSpeed;
    pm->turnRot = yawSeek_.step();
}

void AIObj::seekNothing(PlayerMove* /*pm*/) {}

void AIObj::aimAt(PlayerMove& move, const Point3F& target, Flt32 threshold) {
    if (!player_) {
        return;
    }
    const auto source = player_->getBoxCenter();
    const auto yaw = getAngleSeekZ(target, source, player_->getRot().z);
    const Flt32 pitch = getDesiredPitch(target, source, true) - player_->getViewPitch();
    move.turnRot = std::copysign(iqVars_.turnInc.getInc(yaw.absDiff), yaw.diff);
    if (yaw.absDiff < threshold) {
        move.pitch = std::copysign(iqVars_.pitchInc.getInc(std::abs(pitch)), pitch);
        if (std::abs(pitch) < threshold) {
            move.trigger = trigger_.press(rng_);
        }
    }
}

void AIObj::fireAtPlayerTarget(PlayerMove* pm, const DynamicTarget& target) {
    if (!pm || !player_) {
        return;
    }
    if (const auto* player = dynamic_cast<const Player*>(target.obj); player && player->isDead()) {
        return;
    }
    Point3F point;
    if (!leadPosition(target, getProjectileSpeed(), point)) {
        return;
    }
    const auto center = target.obj ? target.obj->getBoxCenter() : target.pos;
    const auto bounds =
        target.obj ? target.obj->getBoundingBox() : Box3F{{-.6f, -.6f, -1.2f}, {.6f, .6f, 1.2f}};
    point += targetDope(center, bounds.len_x() * .5f, bounds.len_z() * .5f);
    aimAt(*pm, point, .08f);
    currentTargetId_ = target.id;
}

void AIObj::fireAtLaserOwner(PlayerMove* pm, Int32 playerId) {
    if (!player_ || !player_->getManager()) {
        return;
    }
    auto* manager = player_->getManager();
    auto* players = PlayerManager::get(manager);
    auto* client = players ? players->findBaseRep(playerId) : nullptr;
    auto* owner = client ? dynamic_cast<Player*>(client->ownedObject) : nullptr;
    if (!owner) {
        return;
    }
    auto* targets = dynamic_cast<sim::SimSet*>(manager->findObject(TargetableSetId));
    if (!targets) {
        return;
    }
    for (auto* object : targets->objects()) {
        auto* laser = dynamic_cast<opent1::game::TargetLaser*>(object);
        Point3F point;
        Int32 targetTeam;
        if (laser && (laser->shooter() == owner || laser->getOwnerId() == owner->getId()) &&
            laser->getTarget(&point, &targetTeam)) {
            fireAtLaserTarget(pm, point);
        }
    }
}

void AIObj::fireAtLaserTarget(PlayerMove* pm, const Point3F& targetPos) {
    if (!pm || !player_) {
        return;
    }
    ProjectileDataType primary, secondary;
    auto* dbm = player_->resolveDataBlockManager();
    if (!dbm || !player_->getWeaponAmmoType(primary, secondary) || primary.type < 0) {
        return;
    }
    const auto* data =
        dynamic_cast<const ProjectileData*>(dbm->lookupDataBlock(primary.dataType, primary.type));
    if (!data) {
        return;
    }
    const auto origin = player_->getEyeTransform().p;
    const auto gravity = -player_->getForce(sim::MovementGravityForce).value_or(Point3F{}).len();
    Point3F primaryVector, secondaryVector;
    bool primaryRange = false, secondaryRange = false;
    if (!data->getTargetVector(origin, targetPos, gravity, &primaryVector, &primaryRange, false)) {
        return;
    }
    const bool second =
        data->getTargetVector(origin, targetPos, gravity, &secondaryVector, &secondaryRange, true);
    if (!(second && secondaryRange) && !primaryRange) {
        return;
    }
    aimAt(*pm, origin + (second && secondaryRange ? secondaryVector : primaryVector) * 50.f, .001f);
}

void AIObj::fireAtPointTarget(PlayerMove* pm, const Point3F& targetPos) {
    if (pm) {
        aimAt(*pm, targetPos, .08f);
    }
}

} // namespace opent1::game
