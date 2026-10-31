#include <opent1/game/flier.hpp>
#include <opent1/game/mine.hpp>
#include <opent1/game/player.hpp>
#include <opent1/game/smoke_puff.hpp>
#include <opent1/world/world_mission.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <numbers>

namespace opent1::game {

namespace {
constexpr Flt32 kPi = static_cast<Flt32>(std::numbers::pi);
constexpr Flt32 kTwoPi = static_cast<Flt32>(2.0 * std::numbers::pi);

constexpr UInt32 CollisionDamageMask = SimPlayerObjectType | StaticObjectType | VehicleObjectType |
                                       MoveableObjectType | MineObjectType;

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

Flier::Flier() {
    trackGround_ = false;
    setForceMask(sim::MovementGravityForce);
    setMovementFlags((movementFlags() | UseDrag | UseCurrent | UseElasticity | UseFriction) &
                     ~RotateBoundingBox);
}

bool Flier::initResources(GameBaseData* in_data) {
    if (!Parent::initResources(in_data)) {
        return false;
    }
    flierData_ = dynamic_cast<FlierData*>(in_data);
    if (!flierData_) {
        return false;
    }

    setDrag(flierData_->drag);
    setDensity(flierData_->density);
    return true;
}

bool Flier::onAdd() {
    if (!Parent::onAdd()) {
        return false;
    }
    lastPuff_ = getManager() ? static_cast<UInt32>(getManager()->getCurrentTime() * 1000.0f) : 0;
    return true;
}

const Point3F& Flier::getLeadVelocity() const noexcept {
    if (std::fabs(speed_) > 0.1f) {
        TMat3F mat{};
        mat.set(EulerF{rotation_.x, rotation_.y, rotation_.z}, getPos());
        Point3F forward{};
        mat.getRow(1, forward);
        cachedLeadVel_ = forward * speed_ + Point3F{0.0f, 0.0f, lift_};
        return cachedLeadVel_;
    }
    return Parent::getLeadVelocity();
}

void Flier::sampleAltitude() {
    if (!flierData_ || !getManager()) {
        return;
    }
    auto* root = dynamic_cast<sim::SimContainer*>(getManager()->findObject(sim::RootContainerId));
    if (!root) {
        return;
    }
    for (auto* container = getContainer(); container; container = container->getContainer()) {
        if (const auto* terrain = dynamic_cast<const world::SimMissionTerrain*>(container)) {
            const auto range = terrain->heightRange();
            maxAltitude_ = range.y + flierData_->maxAlt + (range.y - range.x / 2.f);
            break;
        }
    }
    const auto pos = getPos();
    Flt32 height = -1.e30f;
    const auto sample = [&](UInt32 mask, Flt32 depth) {
        sim::SimContainerQuery query;
        query.id = getId();
        query.mask = mask;
        query.box.fMin = pos;
        query.box.fMax = pos - Point3F{0, 0, depth};
        sim::SimCollisionInfo hit;
        if (!root->findLOS(query, hit) || hit.surfaces.surfaces.empty()) {
            return;
        }
        const auto* object = hit.object.resolve(*getManager());
        if (const auto* vehicle = dynamic_cast<const Vehicle*>(object);
            vehicle && vehicle->getLeadVelocity().len() == 0) {
            return;
        }
        Point3F point;
        m_mul(hit.surfaces.surfaces.front().position, hit.surfaces.tWorld, point);
        height = std::max(height, point.z);
    };
    sample(SimTerrainObjectType, 1000000.f);
    sample(SimInteriorObjectType | StaticObjectType | VehicleObjectType, 5.f);
    if ((!hasFocus_ || speed_ < flierData_->maxSpeed / 3.f) &&
        damageLevel_ < flierData_->maxDamage) {
        height += 2.f;
    }
    if (pos.z <= height && speed_ == 0) {
        landed_ = true;
        if (!hasFocus_) {
            auto rotation = getRot();
            rotation.x = rotation.y = 0;
            setRot(rotation);
        }
        setPos({pos.x, pos.y, height});
        if (!isGhost()) {
            setMaskBits(OrientationMask);
        }
    } else if (pos.z > height) {
        landed_ = false;
    }
    minAltitude_ = height;
}

void Flier::dynamics(Flt32 adjust) {
    if (!flierData_ || !getManager()) {
        return;
    }

    sampleAltitude();
    const Point3F rot = getRot();
    if (!landed_ && speed_ != 0.0f) {
        lift_ -= flierData_->lift * adjust;
        lift_ -= lift_ * (speed_ / std::max(0.001f, flierData_->maxSpeed)) * adjust;
        if (lift_ > flierData_->maxVertical) {
            lift_ = flierData_->maxVertical;
        }
    } else {
        lift_ -= flierData_->lift * adjust;
        if (landed_ && lift_ < 0.0f) {
            lift_ = 0.0f;
        }
    }

    const Flt32 alt = getPos().z - minAltitude_;
    const Flt32 minLift = flierData_->maxVertical * -1.75f;
    const Flt32 maxLift = (alt < 4.0f) ? (minLift * (alt / 4.0f)) : minLift;
    if (lift_ < maxLift) {
        lift_ = maxLift;
    }
    if (getPos().z > maxAltitude_ && lift_ != 0.0f) {
        lift_ = 0.0f;
    }

    RMat3F rmat{EulerF{rot.x, rot.y, rot.z}};
    Point3F vel{};
    m_mul(Point3F{lateralThrust_, speed_, lift_}, rmat, vel);
    if (damageLevel_ < flierData_->maxDamage) {
        setLinearVelocity(vel);
    }
}

void Flier::updateMove(const PlayerMove* move, Flt32 interval) {
    if (!flierData_) {
        return;
    }
    Parent::updateMove(move, interval);

    const Flt32 adjust = interval / 0.032f;
    Flt32 turn = 0.0f;
    Flt32 pitchChange = 0.0f;
    const Flt32 oldDS = desiredSpeed_;
    const Flt32 oldS = speed_;
    Flt32 yRot = getRot().y;
    const Flt32 halfMaxVert = std::max(0.001f, flierData_->maxVertical * 0.5f);
    yRot -= (kPi / 32.0f) * (lateralThrust_ / halfMaxVert);
    bool braking = false;
    jetting_ = false;

    if (move) {
        Flt32 speedScale = 1.0f - (speed_ / std::max(0.001f, flierData_->maxSpeed));
        if (speedScale < 0.33f) {
            speedScale = 0.33f;
        }

        pitchChange = (-move->pitch * speedScale) * 0.25f * adjust;
        turn = (move->turnRot * speedScale) * 0.25f * adjust;

        if (move->trigger && !isGhost()) {
            shoot();
        }

        if (move->jumpAction && !isGhost()) {
            lastPlayerMove_.trigger = false;
            lastPlayerMove_.jumpAction = false;
            lastPlayerMove_.forwardAction = 0.0f;
            lastPlayerMove_.backwardAction = 0.0f;
            lastPlayerMove_.jetting = false;
            setMaskBits(OrientationMask);
        }

        if (move->forwardAction) {
            desiredSpeed_ = flierData_->maxSpeed;
            jetting_ = true;
        } else {
            desiredSpeed_ = 0.0f;
        }

        if (move->backwardAction) {
            braking = true;
            jetting_ = true;
            desiredSpeed_ = flierData_->minSpeed;
            if (flierData_->minSpeed != 0.0f) {
                landed_ = false;
            }
        }

        if (move->jetting) {
            lift_ += 1.25f * adjust;
            if (lift_ > flierData_->maxVertical) {
                lift_ = flierData_->maxVertical;
            }
            landed_ = false;
            jetting_ = true;
        }

        if (!landed_) {
            const Flt32 maxLateral = flierData_->maxVertical * 0.5f;
            if (move->rightAction) {
                lateralThrust_ += 0.25f * adjust;
                if (lateralThrust_ > maxLateral) {
                    lateralThrust_ = maxLateral;
                }
            } else if (move->leftAction) {
                lateralThrust_ -= 0.25f * adjust;
                if (lateralThrust_ < -maxLateral) {
                    lateralThrust_ = -maxLateral;
                }
            }
        }
    }

    if (lateralThrust_ != 0.0f &&
        (landed_ || (!move || !(move->leftAction || move->rightAction)))) {
        lateralThrust_ -= (lateralThrust_ > 0.0f ? 0.25f : -0.25f) * adjust;
        if (lateralThrust_ < 0.25f && lateralThrust_ > -0.25f) {
            lateralThrust_ = 0.0f;
        }
    }

    if (!hasFocus_ && getControlClient() == 0) {
        desiredSpeed_ = 0.0f;
        desiredRot_.x = 0.0f;
        desiredRot_.y = 0.0f;
    }

    if (desiredSpeed_ < flierData_->minSpeed) {
        desiredSpeed_ = flierData_->minSpeed;
    }
    if (desiredSpeed_ > flierData_->maxSpeed) {
        desiredSpeed_ = flierData_->maxSpeed;
    }

    const Point3F oldPos = getPos();
    Point3F rot = getRot();
    rot.y = yRot;
    const Point3F oldRot = rot;

    const Flt32 maxSpeed = desiredSpeed_;

    if (speed_ > maxSpeed) {
        speed_ -= flierData_->accel * adjust * 0.5f;
        if ((!hasFocus_ && getControlClient() == 0) || braking) {
            speed_ -= flierData_->accel * adjust;
        }
        if (speed_ < maxSpeed) {
            speed_ = maxSpeed;
        }
    }

    if (speed_ < maxSpeed) {
        speed_ += flierData_->accel * adjust;
        if (speed_ > maxSpeed) {
            speed_ = maxSpeed;
        }
    }

    if (oldPos.z > maxAltitude_ && pitchChange < 0.0f) {
        pitchChange = 0.0f;
    }

    if (!landed_ || speed_ != 0.0f) {
        rot.x -= pitchChange;
        rot.y -= turn * 0.5f;
    }

    if (rot.y < -flierData_->maxBank) {
        const Flt32 amount = rot.y - -flierData_->maxBank;
        rot.y -= (amount * 0.5f) * adjust;
    }
    if (rot.y > flierData_->maxBank) {
        const Flt32 amount = rot.y - flierData_->maxBank;
        rot.y -= (amount * 0.5f) * adjust;
    }
    if (rot.x < -flierData_->maxPitch) {
        const Flt32 amount = rot.x - -flierData_->maxPitch;
        rot.x -= (amount * 0.5f) * adjust;
    }
    if (rot.x > flierData_->maxPitch) {
        const Flt32 amount = rot.x - flierData_->maxPitch;
        rot.x -= (amount * 0.5f) * adjust;
    }

    rot.z -= (rot.y / 15.0f) * adjust;
    rot.y += (kPi / 32.0f) * (lateralThrust_ / halfMaxVert);

    if (hasFocus_ || getControlClient() != 0) {
        desiredRot_ = rot;
    }

    if (landed_ && speed_ == 0.0f) {
        desiredRot_.x = 0.0f;
        desiredRot_.y = 0.0f;
    }

    if (oldPos.z > maxAltitude_) {
        desiredRot_.x = 0.0f;
    }

    if (rot.x < desiredRot_.x) {
        rot.x += 0.0125f * adjust;
        if (rot.x > desiredRot_.x) {
            rot.x = desiredRot_.x;
        }
    } else if (rot.x > desiredRot_.x) {
        rot.x -= 0.0125f * adjust;
        if (rot.x < desiredRot_.x) {
            rot.x = desiredRot_.x;
        }
    }

    if (rot.y < desiredRot_.y) {
        rot.y += 0.0125f * adjust;
        if (rot.y > desiredRot_.y) {
            rot.y = desiredRot_.y;
        }
    } else if (rot.y > desiredRot_.y) {
        rot.y -= 0.0125f * adjust;
        if (rot.y < desiredRot_.y) {
            rot.y = desiredRot_.y;
        }
    }

    setRot(rot);
    dynamics(adjust);
    if (getManager()) {
        updateMovement(interval);
    }

    if (!isGhost()) {
        if (oldPos != getPos() || oldRot != getRot()) {
            setMaskBits(OrientationMask);
        }
        if (oldDS != desiredSpeed_ || oldS != speed_) {
            setMaskBits(StatusMask);
        }
    }
}

void Flier::clientProcess(UInt32 curTime) {
    Parent::clientProcess(curTime);
}

bool Flier::processCollision(MovementInfo& info) {
    bool hit = false;
    Point3F normal{0.0f, 0.0f, 0.0f};

    for (std::size_t i = 0; i < info.collisionList.size();) {
        sim::SimObject* obj =
            getManager() ? info.collisionList[i].object.resolve(*getManager()) : nullptr;
        if (!obj) {
            ++i;
            continue;
        }
        if (const auto* pl = dynamic_cast<const Player*>(obj)) {
            if (pl->getMountObject() == this) {
                info.collisionList.erase(info.collisionList.begin() +
                                         static_cast<std::ptrdiff_t>(i));
                continue;
            }
        }
        if (const auto* mn = dynamic_cast<const Mine*>(obj)) {
            if (mn->getOwnerId() == getId()) {
                info.collisionList.erase(info.collisionList.begin() +
                                         static_cast<std::ptrdiff_t>(i));
                continue;
            }
        }

        if ((obj->getType() & CollisionDamageMask) != 0) {
            if (auto* sb = dynamic_cast<ShapeBase*>(obj)) {
                Point3F velocity = getLinearVelocity() - sb->getLinearVelocity();
                Point3F dir = sb->getBoxCenter() - getBoxCenter();
                if (!dir.isZero()) {
                    dir.normalize();
                }
                const Flt32 impactSpeed = m_dot(velocity, dir);
                if (impactSpeed > 0.0f) {
                    Point3F mom =
                        dir * (impactSpeed * getMass() * 0.25f) + Point3F{0.0f, 0.0f, 50.0f};
                    if (!isGhost() && flierData_ &&
                        velocity.len() > (flierData_->maxSpeed * 0.25f)) {
                        sb->applyDamage(flierData_->ramDamageType,
                                        flierData_->ramDamage *
                                            (impactSpeed / std::max(0.001f, flierData_->maxSpeed)),
                                        sb->getBoxCenter(),
                                        velocity,
                                        mom,
                                        getId());
                    } else {
                        sb->applyImpulse(mom);
                    }
                }
            }
            if ((obj->getType() & (SimPlayerObjectType | MineObjectType)) != 0) {
                onCollisionNotify(static_cast<GameBase*>(obj));
                info.collisionList.erase(info.collisionList.begin() +
                                         static_cast<std::ptrdiff_t>(i));
                continue;
            }
        }

        if ((obj->getType() & (SimInteriorObjectType | SimTerrainObjectType | StaticObjectType |
                               VehicleObjectType | MoveableObjectType)) != 0) {
            auto& sList = info.collisionList[i].surfaces;
            for (const auto& surf : sList.surfaces) {
                Point3F tNorm{};
                m_mul(surf.normal, static_cast<const RMat3F&>(sList.tWorld), tNorm);
                hit = true;
                normal += tNorm;
            }
        }
        ++i;
    }

    if (hit && flierData_) {
        normal.normalize();
        Point3F vel = getLinearVelocity();
        if (!vel.isZero()) {
            vel.normalize();
        }
        const Flt32 dot = -m_dot(normal, vel);
        const Flt32 crashSpeed = std::fabs(speed_) + std::fabs(lift_) + std::fabs(lateralThrust_);

        if (dot > 0.3f - (1.0f - (crashSpeed / std::max(0.001f, flierData_->maxSpeed))) * 0.2f ||
            crashSpeed < flierData_->maxSpeed * 0.25f) {
            if (normal.z > 0.9f && lift_ <= 0.0f &&
                (speed_ < 0.1f * flierData_->maxSpeed || getControlClient() == 0)) {
                setRot(Point3F{0.0f, 0.0f, getRot().z});
                desiredRot_ = getRot();
                landed_ = true;
            }
            setLinearVelocity(Point3F{0.0f, 0.0f, 0.0f});
            speed_ = 0.0f;
            desiredSpeed_ = 0.0f;
            lift_ = 0.0f;
            lateralThrust_ = 0.0f;
        } else {
            Point3F reflect = vel + normal * (dot * 2.0f);
            if (!reflect.isZero()) {
                reflect.normalize();
            }
            desiredRot_.z = std::atan2(-reflect.x, reflect.y);
            if (desiredRot_.z < 0.0f) {
                desiredRot_.z += kTwoPi;
            }
            const Flt32 pitchSin = std::clamp(reflect.z, -1.0f, 1.0f);
            desiredRot_.x =
                std::clamp(std::asin(pitchSin), -flierData_->maxPitch, flierData_->maxPitch);
            desiredRot_.y = 0.0f;
            setRot(desiredRot_);

            speed_ = crashSpeed * (1.0f - dot) * 0.75f;
            desiredSpeed_ = std::min(desiredSpeed_, speed_);
            lift_ = 0.0f;
            lateralThrust_ = 0.0f;
        }

        if (!isGhost() && (crashSpeed * dot) > (flierData_->maxSpeed / 3.0f)) {
            applyDamage(-1,
                        flierData_->maxDamage * flierData_->groundDamageScale *
                            (crashSpeed / std::max(0.001f, flierData_->maxSpeed)) * dot,
                        getBoxCenter(),
                        getLinearVelocity(),
                        Point3F{0.0f, 0.0f, 0.0f},
                        getId());
        }
    }

    if (info.collisionList.empty()) {
        return true;
    }
    return Parent::processCollision(info);
}

void Flier::writePacketData(BitStream& bstream) const {
    Parent::writePacketData(bstream);
    writePod(bstream, lift_);
    writePod(bstream, lateralThrust_);
    bstream.writeFlag(landed_);
}

bool Flier::readPacketData(BitStream& bstream) {
    BitStreamRollbackGuard guard(bstream);

    const Point3F oldPos = getPos();
    if (!Parent::readPacketData(bstream)) {
        return false;
    }

    Flt32 newLift = 0.0f;
    Flt32 newLat = 0.0f;
    if (!readPod(bstream, newLift) || !readPod(bstream, newLat)) {
        return false;
    }
    const bool newLanded = bstream.readFlag();
    if (!bstream.isValid()) {
        return false;
    }

    lift_ = newLift;
    lateralThrust_ = newLat;
    landed_ = newLanded;
    if (landed_) {
        setPos(oldPos);
    }

    guard.committed = true;
    return true;
}

Persistent::Error
Flier::read(StreamIO& sio, Int32 version, Int32 user, Persistent::LoadContext& ctx) {
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
Flier::write(StreamIO& sio, Int32 version, Int32 user, Persistent::StoreContext& ctx) const {
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

Flt32 Flier::getTurnRate(const Point3F& in_rMissileVel) const noexcept {
    Point3F flierVel = getLinearVelocity();
    if (flierVel.len() <= 5.0f) {
        return 0.75f;
    }
    flierVel.normalize();
    Point3F missileVel = in_rMissileVel;
    if (!missileVel.isZero()) {
        missileVel.normalize();
    }
    const Flt32 dot = m_dot(flierVel, missileVel);
    if (dot >= 0.0f) {
        return 0.2f + (0.2f * dot);
    }
    return 0.2f - (0.05f * dot);
}

bool registerFlierClass(Persistent::ClassRegistry& registry) {
    return registry.registerClass<Flier>("Flier", FlierPersTag);
}

bool registerFlierNetwork(sim::network::Registry& registry) {
    return registry.registerGhost<Flier>(static_cast<UInt16>(FlierPersTag));
}

} // namespace opent1::game
