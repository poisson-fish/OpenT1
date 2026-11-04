#include <opent1/game/mine.hpp>
#include <ranges>
#include <opent1/game/player.hpp>
#include <opent1/game/projectile_factory.hpp>
#include <opent1/game/vehicle.hpp>
#include <opent1/game/sound.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <numbers>

namespace opent1::game {

namespace {
constexpr Flt32 kPi = static_cast<Flt32>(std::numbers::pi);
constexpr Flt32 kTwoPi = static_cast<Flt32>(2.0 * std::numbers::pi);

constexpr UInt32 ServerCollisionMask = SimTerrainObjectType | SimInteriorObjectType |
                                       SimPlayerObjectType | StaticObjectType | VehicleObjectType |
                                       MoveableObjectType | ItemObjectType | MineObjectType;

constexpr UInt32 ClientCollisionMask = SimTerrainObjectType | SimInteriorObjectType |
                                       StaticObjectType | VehicleObjectType | MoveableObjectType |
                                       SimPlayerObjectType;

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

Vehicle::Vehicle() {
    setType(VehicleObjectType);
    passengerNode_.fill(-1);
}

bool Vehicle::initResources(GameBaseData* in_data) {
    if (!Parent::initResources(in_data)) {
        return false;
    }
    data_ = dynamic_cast<VehicleData*>(in_data);
    if (!data_) {
        return false;
    }
    passengerNode_.fill(-1);
    pilotNode_ = exitNode_ = -1;
    if (shapeInstance_) {
        const auto& shape = *shapeInstance_->shape();
        const auto find = [&](std::string_view wanted) -> Int32 {
            for (auto n : std::views::iota(std::size_t{0}, shape.nodes.size())) {
                const auto index = shape.nodes[n].name;
                if (index >= 0 && std::size_t(index) < shape.names.size() &&
                    shape.names[index] == wanted) {
                    return Int32(n);
                }
            }
            return -1;
        };
        pilotNode_ = find("dummy pilot");
        exitNode_ = find("dummy exit");
        for (auto n : std::views::iota(std::size_t{0}, passengerNode_.size())) {
            passengerNode_[n] = find("dummy passenger" + std::to_string(n + 1));
            if (passengerNode_[n] < 0) {
                break;
            }
        }
    }
    setMass(std::max(1.0f, data_->mass));
    hasUnpackedInitialVehicle_ = true;
    return true;
}

bool Vehicle::onAdd() {
    if (!Parent::onAdd()) {
        return false;
    }

    if (isGhost()) {
        setCollisionMask(ClientCollisionMask);
        addToSet(SensorVisibleSetId);
        addToSet(sim::CameraMountSetId);
        addToSet(sim::LightSetId);
    } else {
        setCollisionMask(ServerCollisionMask);
        addToSet(PlayerSetId);
    }

    initCollisionInfo();
    updateVehicleSound();
    return true;
}

void Vehicle::onRemove() {
    TSFX::Stop(currentSound_, getManager());
    Parent::onRemove();
}

void Vehicle::updateVehicleSound() {
    if (!isGhost() || !data_)
        return;
    if (oldSoundState_ != soundState_) {
        TSFX::Stop(currentSound_, getManager());
        oldSoundState_ = soundState_;
    }
    // Vanilla's IDLE_SOUND playback is commented out; only RUN_SOUND emits PCM.
    if (soundState_ == RUN_SOUND && !currentSound_ && data_->moveSound >= 0)
        currentSound_ = TSFX::PlayAt(UInt32(data_->moveSound), getTransform(), {}, getManager());
    TSFX::Move(currentSound_, getTransform(), getLinearVelocity(), getManager());
}

void Vehicle::onCollisionNotify(GameBase* object) {
    if (!isGhost() && object && object->getControlClient() != getControlClient()) {
        Parent::onCollisionNotify(object);
    }
}

void Vehicle::setSpeed(Flt32 spd) noexcept {
    speed_ = spd;
    setMaskBits(StatusMask);
}

void Vehicle::setDesiredSpeed(Flt32 dspd) noexcept {
    desiredSpeed_ = dspd;
    setMaskBits(StatusMask);
}

bool Vehicle::canMount(GameBase* /*obj*/) const {
    return !hasFocus_;
}

bool Vehicle::canRide(GameBase* /*obj*/) const {
    return data_ && passengerCount_ < std::ranges::count_if(
                                          passengerNode_,
                                          [](Int32 node) {
                                              return node >= 0;
                                          });
}

Int32 Vehicle::nextPassengerPoint() {
    for (auto slot :
         std::views::iota(std::size_t{0}) | std::views::take_while([&](std::size_t slot) {
             return slot < passengerNode_.size() && passengerNode_[slot] >= 0;
         })) {
        bool occupied = false;
        if (getManager()) {
            if (auto* players = dynamic_cast<sim::SimSet*>(getManager()->findObject(PlayerSetId))) {
                for (auto* object : players->objects()) {
                    if (const auto* player = dynamic_cast<const Player*>(object);
                        player && player->getMountObject() == this &&
                        player->getMountPoint() == Int32(slot + 2)) {
                        occupied = true;
                        break;
                    }
                }
            }
        }
        if (!occupied) {
            return Int32(slot + 2);
        }
    }
    return -1;
}

void Vehicle::changePassengerCount(Int32 change) {
    passengerCount_ = std::max(0, passengerCount_ + change);
}

bool Vehicle::mountRender() const noexcept {
    return data_ ? data_->visibleDriver : true;
}

Int32 Vehicle::getMountPose() const noexcept {
    return data_ ? data_->driverPose : 0;
}

void Vehicle::getObjectMountTransform(Int32 mountPoint, TMat3F* transform) const {
    if (!transform) {
        return;
    }
    *transform = getTransform();
    const Int32 node = mountPoint == 0   ? exitNode_
                       : mountPoint == 1 ? pilotNode_
                       : mountPoint >= 2 && std::size_t(mountPoint - 2) < passengerNode_.size()
                           ? passengerNode_[mountPoint - 2]
                           : -1;
    if (const auto pose = shapeInstance_ ? shapeInstance_->pose() : std::nullopt;
        pose && node >= 0 && std::size_t(node) < pose->nodeTransforms().size()) {
        m_mul(pose->nodeTransforms()[node], getTransform(), *transform);
    }
}

bool Vehicle::getMuzzleTransform(Int32 /*slot*/, TMat3F* mat) const {
    if (!mat) {
        return false;
    }
    *mat = getTransform();
    const auto pose = shapeInstance_ ? shapeInstance_->pose() : std::nullopt;
    if (!pose) {
        return false;
    }
    const auto& shape = *pose->shape();
    const auto findNode = [&](std::string_view wanted) -> Int32 {
        for (auto i : std::views::iota(std::size_t{0}, shape.nodes.size())) {
            const auto name = shape.nodes[i].name;
            if (name >= 0 && std::size_t(name) < shape.names.size() &&
                shape.names[name] == wanted) {
                return Int32(i);
            }
        }
        return -1;
    };
    std::vector<Int32> barrels;
    for (auto number : std::views::iota(Int32{1}) | std::views::take_while([&](Int32 number) {
                           return number <= Int32(shape.nodes.size());
                       })) {
        const auto node = findNode("dummy muzzle" + std::to_string(number));
        if (node < 0) {
            break;
        }
        barrels.push_back(node);
    }
    const auto node = barrels.empty() ? findNode("dummy muzzle")
                                      : barrels[std::size_t(fireCount_) % barrels.size()];
    if (node < 0) {
        return false;
    }
    m_mul(pose->nodeTransforms()[node], getTransform(), *mat);
    return true;
}

void Vehicle::getCameraTransform(Flt32 camDist, TMat3F* transform) const {
    if (!transform) {
        return;
    }
    *transform = getTransform();
    if (camDist > 0.0f) {
        Point3F fwd{};
        transform->getRow(1, fwd);
        transform->p -= fwd * camDist;
    }
}

const TMat3F& Vehicle::getEyeTransform() const {
    cachedEyeTransform_ = getTransform();
    return cachedEyeTransform_;
}

bool Vehicle::getWeaponAmmoType(ProjectileDataType& outPrimary,
                                ProjectileDataType& outSecondary) const {
    if (!data_) {
        outPrimary = {};
        outSecondary = {};
        return false;
    }
    outPrimary = data_->projectile;
    outSecondary = {};
    return true;
}

void Vehicle::shoot() {
    if (!data_ || data_->projectile.type == -1) {
        return;
    }
    const UInt32 nowMs = getManager()
                             ? static_cast<UInt32>(getManager()->getCurrentTime() * 1000.0f)
                             : lastProcessTime_;
    if (nowMs < nextFireTime_) {
        return;
    }

    TMat3F muzzle{};
    getMuzzleTransform(0, &muzzle);
    muzzle.p += getLinearVelocity() * 0.096f;

    if (getManager()) {
        if (auto proj = createProjectile(data_->projectile)) {
            proj->bindDataBlockManager(resolveDataBlockManager());
            proj->initProjectile(muzzle, getLinearVelocity(), getId());
            getManager()->addObject(std::move(proj));
        }
    }

    flipBarrel_ = !flipBarrel_;
    fireCount_ = (fireCount_ + 1) & 0x3;
    setMaskBits(ShootingMask);
    nextFireTime_ = nowMs + static_cast<UInt32>(data_->reloadDelay * 1000.0f);
}

bool Vehicle::processBlocker(SimObject* obj) {
    if (!obj || obj == this) {
        return false;
    }
    if (auto* go = dynamic_cast<GameBase*>(obj)) {
        if ((go->getType() & CorpseObjectType) != 0) {
            go->clearRestFlag();
            return false;
        }
        TMat3F mat = getTransform();
        Point3F upVec{};
        mat.getRow(1, upVec);
        go->applyImpulse(upVec);

        if (auto* so = dynamic_cast<ShapeBase*>(go)) {
            if ((so->getType() & SimPlayerObjectType) != 0 &&
                static_cast<Player*>(so)->getMountObject() == this) {
                return false;
            }
            if (data_ && data_->maxSpeed > 1e-4f) {
                const Point3F diffVel = getLinearVelocity() - so->getLinearVelocity();
                Flt32 damageSpeed = std::min(diffVel.len(), data_->maxSpeed);
                if (damageSpeed < data_->maxSpeed * 0.25f) {
                    damageSpeed = 0.0f;
                }
                const Flt32 damage =
                    damageSpeed > 0.0f ? data_->ramDamage * (damageSpeed / data_->maxSpeed) : 0.0f;
                if (!isGhost() && damage > 0.0f) {
                    so->applyDamage(data_->ramDamageType,
                                    damage,
                                    getPos(),
                                    getLinearVelocity(),
                                    Point3F{0.0f, 0.0f, 0.0f},
                                    getId());
                }
            }
        }
        return true;
    }
    return false;
}

void Vehicle::setPlayerControl(bool ctrl) {
    interpDoneTime_ = 0;
    hasFocus_ = ctrl;
}

void Vehicle::ghostSetMove(const PlayerMove* move,
                           const Point3F& newPos,
                           const Point3F& newRot,
                           Flt32 newSpeed,
                           Flt32 newLift,
                           Int32 skipCount) {
    const Point3F oldPos = getPos();
    const Point3F oldRot = getRot();

    setRot(newRot);
    desiredRot_ = newRot;
    setPos(newPos);
    speed_ = newSpeed;
    lift_ = newLift;

    if (move) {
        lastPlayerMove_ = *move;
        while (skipCount-- > 0) {
            updateMove(&lastPlayerMove_, 0.032f);
        }
    }

    updateSkip_ = 0;
    const UInt32 nowMs = getManager()
                             ? static_cast<UInt32>(getManager()->getCurrentTime() * 1000.0f)
                             : lastProcessTime_;
    interpDoneTime_ = nowMs + 128u;
    deltaPos_ = (oldPos - getPos()) * (1.0f / 0.128f);
    deltaRot_ = (oldRot - getRot()) * (1.0f / 0.128f);
    if (deltaRot_.z < -kPi * (1.0f / 0.128f)) {
        deltaRot_.z += kTwoPi * (1.0f / 0.128f);
    } else if (deltaRot_.z > kPi * (1.0f / 0.128f)) {
        deltaRot_.z -= kTwoPi * (1.0f / 0.128f);
    }
    setPos(oldPos);
    setRot(oldRot);
}

void Vehicle::serverUpdateMove(std::span<const PlayerMove> moves) {
    if (updateDebt_ > 5) {
        return;
    }
    for (const auto& move : moves) {
        if (lastPlayerMove_.forwardAction != move.forwardAction ||
            lastPlayerMove_.backwardAction != move.backwardAction ||
            lastPlayerMove_.leftAction != move.leftAction ||
            lastPlayerMove_.rightAction != move.rightAction ||
            lastPlayerMove_.jetting != move.jetting ||
            lastPlayerMove_.crouching != move.crouching ||
            lastPlayerMove_.jumpAction != move.jumpAction ||
            lastPlayerMove_.turnRot != move.turnRot || lastPlayerMove_.pitch != move.pitch ||
            lastPlayerMove_.trigger != move.trigger) {
            setMaskBits(OrientationMask);
            updateSkip_ = 0;
        } else {
            ++updateSkip_;
        }

        updateDebt_++;
        updateMove(&move, 0.032f);
        lastPlayerMove_ = move;
    }
    if (speed_ != 0.0f || getLinearVelocity().len() > 0.01f) {
        setMaskBits(OrientationMask);
    }
}

void Vehicle::updateMove(const PlayerMove* /*move*/, Flt32 interval) {
    updateEnergy(interval);
    updateShieldThread(interval);
    updateScriptThreads(interval);
}

void Vehicle::serverProcess(UInt32 curTime) {
    if (curTime - getLastProcessTime() < 32u) {
        return;
    }
    const auto nextSound =
        getControlClient() ? (desiredSpeed_ != 0.f ? RUN_SOUND : IDLE_SOUND) : NO_SOUND;
    if (soundState_ != nextSound) {
        soundState_ = nextSound;
        setMaskBits(SoundMask);
    }
    if (getControlClient() != 0 && updateDebt_ > -5) {
        updateDebt_--;
        updateSkip_++;
    } else {
        PlayerMove move{};
        while (getLastProcessTime() < curTime) {
            updateMove(&move, 0.032f);
            setLastProcessTime(getLastProcessTime() + 32u);
        }
    }

    const Flt32 dt = 0.032f;
    updateEnergy(dt);
    updateDamage(dt);
    updateScriptThreads(dt);
    updateShieldThread(dt);
    updateTimers();
    updateState();
    setLastProcessTime(curTime);
}

void Vehicle::clientProcess(UInt32 curTime) {
    if (!hasFocus_) {
        while (getLastProcessTime() < curTime) {
            const UInt32 endTick = std::min(getLastProcessTime() + 32u, curTime);
            const UInt32 startTick = getLastProcessTime();
            const Flt32 interval = static_cast<Flt32>(endTick - startTick) * 0.001f;

            if (interpDoneTime_ > startTick) {
                const UInt32 tick = std::min(endTick, interpDoneTime_);
                const Flt32 scale = static_cast<Flt32>(tick - startTick) * 0.001f;
                setPos(getPos() - deltaPos_ * scale);
                Point3F rot = getRot() - deltaRot_ * scale;
                while (rot.z < 0.0f) {
                    rot.z += kTwoPi;
                }
                while (rot.z >= kTwoPi) {
                    rot.z -= kTwoPi;
                }
                setRot(rot);
            }

            if (updateSkip_ < 8) {
                updateMove(&lastPlayerMove_, interval);
                ++updateSkip_;
            }
            setLastProcessTime(endTick);
        }
    }

    if (ghostFireCount_ != fireCount_) {
        ghostFireCount_ = fireCount_;
    }

    const Flt32 animStep = static_cast<Flt32>(curTime - getLastAnimateTime()) * 0.001f;
    setLastAnimateTime(curTime);
    updateEnergy(animStep);
    updateShieldThread(animStep);
    updateScriptThreads(animStep);
    updateVehicleSound();
}

void Vehicle::writePacketData(BitStream& bstream) const {
    bstream.writeInt(getSensorPinged(), 2);
    const Point3F rot = getRot();
    const Point3F pos = getPos();
    writePod(bstream, rot);
    writePod(bstream, desiredRot_);
    writePod(bstream, pos);

    const Flt32 minSpd = data_ ? data_->minSpeed : 0.0f;
    const Flt32 maxSpd = data_ ? data_->maxSpeed : 1.0f;
    const Flt32 spdRange = (std::fabs(maxSpd - minSpd) > 1e-4f) ? (maxSpd - minSpd) : 1.0f;
    const Flt32 maxEn = (data_ && data_->maxEnergy > 1e-4f) ? data_->maxEnergy : 1.0f;
    const Flt32 maxDmg = (data_ && data_->maxDamage > 1e-4f) ? data_->maxDamage : 1.0f;

    bstream.writeFloat(std::clamp((speed_ - minSpd) / spdRange, 0.0f, 1.0f), 16);
    bstream.writeFloat(std::clamp((desiredSpeed_ - minSpd) / spdRange, 0.0f, 1.0f), 16);
    bstream.writeFloat(std::clamp(energy_ / maxEn, 0.0f, 1.0f), 16);

    const Int32 dlevel = std::clamp(static_cast<Int32>((damageLevel_ / maxDmg) * 63.0f), 0, 63);
    bstream.writeInt(dlevel, 6);
    bstream.writeInt(fireCount_, 2);
    bstream.writeInt(soundState_, 2);
}

bool Vehicle::readPacketData(BitStream& bstream) {
    BitStreamRollbackGuard guard(bstream);

    const auto ping = static_cast<UInt8>(bstream.readInt(2));
    Point3F rot{};
    Point3F desRot{};
    Point3F pos{};
    if (!readPod(bstream, rot) || !readPod(bstream, desRot) || !readPod(bstream, pos)) {
        return false;
    }

    const Flt32 speedFrac = bstream.readFloat(16);
    const Flt32 desSpeedFrac = bstream.readFloat(16);
    const Flt32 energyFrac = bstream.readFloat(16);
    const Int32 dlevel = bstream.readInt(6);
    const Int32 fc = bstream.readInt(2);
    const Int32 ss = bstream.readInt(2);
    if (!bstream.isValid()) {
        return false;
    }

    const Flt32 minSpd = data_ ? data_->minSpeed : 0.0f;
    const Flt32 maxSpd = data_ ? data_->maxSpeed : 1.0f;
    const Flt32 spdRange = (std::fabs(maxSpd - minSpd) > 1e-4f) ? (maxSpd - minSpd) : 1.0f;
    const Flt32 maxEn = (data_ && data_->maxEnergy > 1e-4f) ? data_->maxEnergy : 1.0f;

    setSensorPinged(ping);
    setRot(rot);
    desiredRot_ = desRot;
    setPos(pos);
    speed_ = (speedFrac * spdRange) + minSpd;
    desiredSpeed_ = (desSpeedFrac * spdRange) + minSpd;
    energy_ = energyFrac * maxEn;
    damageLevel_ = static_cast<Flt32>(dlevel) / 63.0f;
    fireCount_ = fc;
    soundState_ = ss;
    updateVehicleSound();

    guard.committed = true;
    return true;
}

UInt32 Vehicle::packUpdate(sim::network::GhostManager& g, UInt32 mask, BitStream& stream) const {
    const bool myClient = g.owner() && getControlClient() == static_cast<Int32>(g.owner()->getId());
    if (myClient && (mask & (InitialUpdate | BaseStateMask)) == 0) {
        stream.writeFlag(true);
        return 0;
    }
    stream.writeFlag(false);

    const UInt32 retMask = Parent::packUpdate(g, mask & ~(PositionMask | RotationMask), stream);
    if (mask == 0xFFFFFFFFu) {
        packDatFile(stream);
    }

    const Flt32 minSpd = data_ ? data_->minSpeed : 0.0f;
    const Flt32 maxSpd = data_ ? data_->maxSpeed : 1.0f;
    const Flt32 spdRange = (std::fabs(maxSpd - minSpd) > 1e-4f) ? (maxSpd - minSpd) : 1.0f;
    const Flt32 maxDmg = (data_ && data_->maxDamage > 1e-4f) ? data_->maxDamage : 1.0f;

    if (stream.writeFlag((mask & OrientationMask) != 0)) {
        const Point3F rot = getRot();
        const Point3F pos = getPos();
        writePod(stream, rot);
        writePod(stream, pos);

        if (stream.writeFlag(mask != 0xFFFFFFFFu && !myClient)) {
            PlayerMove moveCopy = lastPlayerMove_;
            if (!hasFocus_) {
                moveCopy.trigger = false;
                moveCopy.jumpAction = false;
                moveCopy.forwardAction = 0.0f;
                moveCopy.backwardAction = 0.0f;
                moveCopy.jetting = false;
            }
            moveCopy.write(stream, false);
            stream.writeFloat(std::clamp((speed_ - minSpd) / spdRange, 0.0f, 1.0f), 10);
            writePod(stream, lift_);
            const Int32 skip = std::clamp(updateSkip_, 0, 15);
            stream.writeInt(skip, 4);
        }
    }

    if (stream.writeFlag((mask & StatusMask) != 0)) {
        if (data_) {
            stream.writeFloat(std::clamp((speed_ - minSpd) / spdRange, 0.0f, 1.0f), 10);
            stream.writeFloat(std::clamp((desiredSpeed_ - minSpd) / spdRange, 0.0f, 1.0f), 10);
        } else {
            stream.writeFloat(0.0f, 10);
            stream.writeFloat(0.0f, 10);
        }
    }

    if (stream.writeFlag((mask & ShootingMask) != 0)) {
        stream.writeInt(fireCount_, 2);
    }

    if (stream.writeFlag((mask & DamageMask) != 0)) {
        const Int32 dlevel = std::clamp(static_cast<Int32>((damageLevel_ / maxDmg) * 63.0f), 0, 63);
        stream.writeInt(dlevel, 6);
    }

    if (stream.writeFlag((mask & SoundMask) != 0)) {
        stream.writeInt(soundState_, 2);
    }

    return retMask;
}

bool Vehicle::unpackUpdate(sim::network::GhostManager& g, BitStream& stream) {
    BitStreamRollbackGuard guard(stream);

    const auto savedDatId = datFileId_;
    const auto savedInitVeh = hasUnpackedInitialVehicle_;
    const auto savedRot = getRot();
    const auto savedDesRot = desiredRot_;
    const auto savedPos = getPos();
    const auto savedSpeed = speed_;
    const auto savedDesSpeed = desiredSpeed_;
    const auto savedLift = lift_;
    const auto savedSkip = updateSkip_;
    const auto savedMove = lastPlayerMove_;
    const auto savedFireCount = fireCount_;
    const auto savedDmg = damageLevel_;
    const auto savedSoundState = soundState_;
    const auto savedInterpDone = interpDoneTime_;
    const auto savedDeltaPos = deltaPos_;
    const auto savedDeltaRot = deltaRot_;

    const auto rollback = [&]() noexcept {
        datFileId_ = savedDatId;
        hasUnpackedInitialVehicle_ = savedInitVeh;
        setRot(savedRot);
        desiredRot_ = savedDesRot;
        setPos(savedPos);
        speed_ = savedSpeed;
        desiredSpeed_ = savedDesSpeed;
        lift_ = savedLift;
        updateSkip_ = savedSkip;
        lastPlayerMove_ = savedMove;
        fireCount_ = savedFireCount;
        damageLevel_ = savedDmg;
        soundState_ = savedSoundState;
        interpDoneTime_ = savedInterpDone;
        deltaPos_ = savedDeltaPos;
        deltaRot_ = savedDeltaRot;
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

    if (!hasUnpackedInitialVehicle_) {
        if (!unpackDatFile(stream)) {
            return rollback();
        }
        loadDatFile();
        hasUnpackedInitialVehicle_ = true;
    }

    const Flt32 minSpd = data_ ? data_->minSpeed : 0.0f;
    const Flt32 maxSpd = data_ ? data_->maxSpeed : 1.0f;
    const Flt32 spdRange = (std::fabs(maxSpd - minSpd) > 1e-4f) ? (maxSpd - minSpd) : 1.0f;

    if (stream.readFlag()) {
        Point3F rot{};
        Point3F pos{};
        if (!readPod(stream, rot) || !readPod(stream, pos)) {
            return rollback();
        }

        if (stream.readFlag()) {
            PlayerMove move{};
            if (!move.read(stream, false)) {
                return rollback();
            }
            const Flt32 speedFrac = stream.readFloat(10);
            const Flt32 newSpeed = (speedFrac * spdRange) + minSpd;
            Flt32 newLift = 0.0f;
            if (!readPod(stream, newLift)) {
                return rollback();
            }
            const Int32 skipCount = stream.readInt(4);
            if (!stream.isValid()) {
                return rollback();
            }
            ghostSetMove(&move, pos, rot, newSpeed, newLift, skipCount);
        } else {
            setRot(rot);
            setPos(pos);
            desiredRot_ = rot;
        }
    }

    if (stream.readFlag()) {
        const Flt32 speedFrac = stream.readFloat(10);
        const Flt32 desSpeedFrac = stream.readFloat(10);
        if (data_) {
            speed_ = (speedFrac * spdRange) + minSpd;
            desiredSpeed_ = (desSpeedFrac * spdRange) + minSpd;
        }
    }

    if (stream.readFlag()) {
        fireCount_ = stream.readInt(2);
    }

    if (stream.readFlag()) {
        const Int32 dlevel = stream.readInt(6);
        damageLevel_ = static_cast<Flt32>(dlevel) / 63.0f;
    }

    if (stream.readFlag()) {
        soundState_ = stream.readInt(2);
    }

    if (!stream.isValid()) {
        return rollback();
    }
    updateVehicleSound();
    guard.committed = true;
    return true;
}

bool Vehicle::onSimObjectTransformEvent(const sim::SimObjectTransformEvent& event) {
    return Parent::onSimObjectTransformEvent(event);
}

bool Vehicle::onSimObjectTransformQuery(sim::SimObjectTransformQuery& query) const {
    return Parent::onSimObjectTransformQuery(query);
}

bool Vehicle::processEvent(const sim::SimEvent& event) {
    if (const auto* te = dynamic_cast<const sim::SimObjectTransformEvent*>(&event)) {
        return onSimObjectTransformEvent(*te);
    }
    return Parent::processEvent(event);
}

bool Vehicle::processQuery(sim::SimQuery& query) {
    if (auto* tq = dynamic_cast<sim::SimObjectTransformQuery*>(&query)) {
        return onSimObjectTransformQuery(*tq);
    }
    return Parent::processQuery(query);
}

Persistent::Error
Vehicle::read(StreamIO& sio, Int32 version, Int32 user, Persistent::LoadContext& ctx) {
    const Int32 startPos = sio.getPosition();
    if (Parent::read(sio, version, user, ctx) != Persistent::Error::Ok) {
        sio.setPosition(startPos);
        return Persistent::Error::ReadError;
    }

    UInt32 vers = 0;
    TMat3F trans{};
    if (!readStreamPod(sio, vers) || !readStreamPod(sio, trans)) {
        sio.setPosition(startPos);
        return Persistent::Error::ReadError;
    }
    setPosition(trans, true);
    return Persistent::Error::Ok;
}

Persistent::Error
Vehicle::write(StreamIO& sio, Int32 version, Int32 user, Persistent::StoreContext& ctx) const {
    const Int32 startPos = sio.getPosition();
    if (Parent::write(sio, version, user, ctx) != Persistent::Error::Ok) {
        sio.setPosition(startPos);
        return Persistent::Error::WriteError;
    }

    const UInt32 vers = 0;
    const TMat3F trans = getTransform();
    if (!writeStreamPod(sio, vers) || !writeStreamPod(sio, trans)) {
        sio.setPosition(startPos);
        return Persistent::Error::WriteError;
    }
    return Persistent::Error::Ok;
}

bool registerVehicleClass(Persistent::ClassRegistry& registry) {
    return registry.registerClass<Vehicle>("Vehicle", VehiclePersTag);
}

bool registerVehicleNetwork(sim::network::Registry& registry) {
    return registry.registerGhost<Vehicle>(static_cast<UInt16>(VehiclePersTag));
}

} // namespace opent1::game
