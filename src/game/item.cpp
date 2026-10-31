#include <opent1/game/item.hpp>
#include <opent1/game/player_manager.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstring>
#include <numbers>

namespace opent1::game {

namespace {
constexpr Flt32 kTwoPi = static_cast<Flt32>(2.0 * std::numbers::pi);

constexpr UInt32 ServerCollisionMask = SimTerrainObjectType | SimInteriorObjectType |
                                       SimPlayerObjectType | StaticObjectType | VehicleObjectType |
                                       MoveableObjectType;

constexpr UInt32 ClientCollisionMask = SimTerrainObjectType | SimInteriorObjectType |
                                       StaticObjectType | VehicleObjectType | MoveableObjectType;

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

bool parseBoolArg(std::string_view arg) noexcept {
    if (arg.empty()) {
        return false;
    }
    const char ch = arg.front();
    return ch == 't' || ch == 'T' || ch == '1';
}

} // namespace

Item::Item() {
    setType(getType() | ItemObjectType);
    setForceMask(static_cast<UInt32>(-1));
    setMovementFlags((movementFlags() | UseElasticity | UseFriction | UseDrag | UseCurrent) &
                     ~RotateBoundingBox);
}

bool Item::processArguments(std::span<const std::string> args) {
    if (args.size() < 3) {
        return false;
    }
    setDatFileName(args[0]);

    Int32 parsedCount = 1;
    std::from_chars(args[1].data(), args[1].data() + args[1].size(), parsedCount);
    count_ = parsedCount;

    if (parseBoolArg(args[2])) {
        flags_ |= PopItem;
    }
    if (args.size() >= 4) {
        isRotate_ = parseBoolArg(args[3]);
    }
    if (args.size() >= 5) {
        isCollideable_ = parseBoolArg(args[4]);
    }
    return true;
}

bool Item::initResources(GameBaseData* in_data) {
    if (!Parent::initResources(in_data)) {
        return false;
    }
    itemData_ = dynamic_cast<ItemData*>(in_data);
    if (!itemData_) {
        return false;
    }

    setMass(itemData_->mass);
    setDrag(itemData_->drag);
    setDensity(itemData_->density);
    setElasticity(itemData_->elasticity);
    setFriction(itemData_->friction);

    if (isGhost()) {
        if (itemData_->lightType != NoLight) {
            light_.type = sim::SimLight::Type::Point;
            light_.range = itemData_->lightRadius;
            light_.intensity =
                ColorF{itemData_->lightColor.x, itemData_->lightColor.y, itemData_->lightColor.z};
            addToSet(sim::LightSetId);
        } else {
            removeFromSet(sim::LightSetId);
        }
    }

    initCollisionInfo();
    if (outerCollisionImage_) {
        outerCollisionImage_->trigger = !isCollideable_;
    }
    if (collisionImage_) {
        collisionImage_->trigger = !isCollideable_;
    }

    if (isRotate_) {
        setMovementFlags(movementFlags() | AtRest);
        setLinearVelocity(Point3F{0.0f, 0.0f, 0.0f});
        setTimeSlice(0.05f);
    }
    return true;
}

bool Item::onAdd() {
    if (const auto rotVal = getFieldValue("rotates"); rotVal && !rotVal->empty()) {
        isRotate_ = parseBoolArg(*rotVal);
    }
    if (const auto colVal = getFieldValue("collideable"); colVal && !colVal->empty()) {
        isCollideable_ = parseBoolArg(*colVal);
    }
    if (const auto cntVal = getFieldValue("count"); cntVal && !cntVal->empty()) {
        Int32 parsedCount = count_;
        const auto [ptr, ec] =
            std::from_chars(cntVal->data(), cntVal->data() + cntVal->size(), parsedCount);
        if (ec == std::errc{}) {
            count_ = parsedCount;
        }
    }

    if (!Parent::onAdd()) {
        return false;
    }

    const SimTime nowSec = getManager() ? getManager()->getCurrentTime() : 0.0f;
    if (isGhost()) {
        setCollisionMask(ClientCollisionMask);
    } else {
        addToSet(PlayerSetId);
        setCollisionMask(ServerCollisionMask);
        if ((flags_ & PopItem) != 0) {
            popTime_ = nowSec + 30.0f;
        }
    }
    return true;
}

void Item::onRemove() {
    if (throwSource_) {
        clearNotify(*throwSource_);
        throwSource_ = nullptr;
    }
    Parent::onRemove();
}

void Item::onDeleteNotify(sim::SimObject& object) {
    if (&object == throwSource_) {
        throwSource_ = nullptr;
    }
    Parent::onDeleteNotify(object);
}

void Item::onCollisionNotify(GameBase* object) {
    if (!isGhost() && (flags_ & Visible) != 0) {
        Parent::onCollisionNotify(object);
    }
}

void Item::serverProcess(UInt32 curTime) {
    updateTimers();

    const Flt32 curTimeSec = static_cast<Flt32>(curTime) * 0.001f;
    if ((flags_ & PopItem) != 0 && curTimeSec >= popTime_) {
        if (m_fading_ == NotFading) {
            startFadeOut(curTime);
            popTime_ = curTimeSec + (static_cast<Flt32>(csm_fadeLength) / 1000.0f);
            setMaskBits(SBFadeMask);
        } else {
            deleteObject();
            return;
        }
    }

    if (!isAtRest() && !isRotate_) {
        Flt32 timeInterval = static_cast<Flt32>(curTime >= getLastProcessTime()
                                                    ? std::min(curTime - getLastProcessTime(), 256u)
                                                    : 0u) *
                             0.001f;
        while (timeInterval > 0.0f) {
            const Flt32 step = std::min(timeInterval, 0.064f);
            updateMovement(step);
            timeInterval -= 0.064f;
        }
        if (curTimeSec > updateTime_) {
            updateTime_ = curTimeSec + ItemUpdatePosTime;
            setMaskBits(PositionMask | VelocityMask);
        }
        if (getOwnerId() != 0 &&
            curTimeSec >= (updateTime_ - ItemUpdatePosTime + ItemThrowBlackout)) {
            setOwnerId(0);
            throwerClientId_ = -1;
            throwerClientLoginId_ = 0;
        }
    } else {
        setOwnerId(0);
        throwerClientId_ = -1;
        throwerClientLoginId_ = 0;
    }

    Parent::serverProcess(curTime);
}

void Item::clientProcess(UInt32 curTime) {
    if (throwSource_) {
        clearNotify(*throwSource_);
        throwSource_ = nullptr;
    }

    const Flt32 timeInterval =
        static_cast<Flt32>(
            curTime >= getLastProcessTime() ? std::min(curTime - getLastProcessTime(), 256u) : 0u) *
        0.001f;
    if (isRotate_) {
        Point3F rot = getRot();
        rot.z += ItemRotationSpeed * timeInterval;
        while (rot.z > kTwoPi) {
            rot.z -= kTwoPi;
        }
        setRot(rot);
    } else if (!isAtRest()) {
        Flt32 remaining = timeInterval;
        while (remaining > 0.0f) {
            const Flt32 step = std::min(remaining, 0.064f);
            updateMovement(step);
            remaining -= 0.064f;
        }
    }

    Parent::clientProcess(curTime);
}

const std::string& Item::getClassName() const noexcept {
    static const std::string kEmpty{};
    return itemData_ ? itemData_->className : kEmpty;
}

const std::string& Item::getDescription() const noexcept {
    static const std::string kEmpty{};
    return itemData_ ? itemData_->description : kEmpty;
}

void Item::hideItem(bool hide) {
    if (hide) {
        if ((flags_ & Visible) != 0) {
            flags_ &= ~Visible;
            setNetFlags(netFlags() | NetFlag::ScopeLocal);
            disableCollision();
        }
    } else {
        if ((flags_ & Visible) == 0) {
            flags_ |= Visible;
            setNetFlags(netFlags() & ~NetFlag::ScopeLocal);
            enableCollision();
            startFadeIn();
            setMaskBits(SBFadeMask);
        }
    }
}

void Item::throwObject(GameBase* object, Flt32 speed, bool careless) {
    const SimTime nowSec = getManager() ? getManager()->getCurrentTime() : 0.0f;
    updateTime_ = nowSec + ItemUpdatePosTime;
    throwSource_ = object;
    if (throwSource_) {
        deleteNotify(*throwSource_);
    }

    if (careless || !object) {
        setOwnerId(0);
        throwerClientId_ = -1;
        throwerClientLoginId_ = 0;
    } else {
        setOwnerId(object->getId());
        throwerClientId_ = object->getControlClient();
        if (throwerClientId_ == 0) {
            throwerClientId_ = object->getLastOwnerClient();
        }
        if (throwerClientId_ != 0 && getManager()) {
            if (auto* pPM = PlayerManager::get(getManager())) {
                if (const auto* pCR = pPM->findBaseRep(throwerClientId_)) {
                    throwerClientLoginId_ = pCR->loginStatus;
                } else {
                    throwerClientLoginId_ = 0xFFFFFFFFu;
                }
            } else {
                throwerClientLoginId_ = 0xFFFFFFFFu;
            }
        } else {
            throwerClientLoginId_ = 0xFFFFFFFFu;
        }
    }

    Point3F pos{0.0f, 0.0f, 0.0f};
    Point3F vel{0.0f, 1.0f, 0.0f};
    if (object) {
        object->getThrowVector(&pos, &vel);
        const Flt32 itemMass = (itemData_ && itemData_->mass > 0.001f) ? itemData_->mass : 1.0f;
        vel *= speed / itemMass;
        vel += object->getLinearVelocity();
        if (const auto* sb = dynamic_cast<const ShapeBase*>(object)) {
            setRot(Point3F{0.0f, 0.0f, sb->getRot().z});
        }
    }
    setPos(pos);
    setLinearVelocity(vel);
    setMaskBits(PositionMask | RotationMask | VelocityMask);
}

bool Item::processCollision(MovementInfo& info) {
    for (std::size_t i = 0; i < info.collisionList.size();) {
        sim::SimObject* obj =
            getManager() ? info.collisionList[i].object.resolve(*getManager()) : nullptr;
        if (obj && obj->getId() == getOwnerId()) {
            info.collisionList.erase(info.collisionList.begin() + static_cast<std::ptrdiff_t>(i));
        } else {
            ++i;
        }
    }
    if (info.collisionList.empty()) {
        return true;
    }
    return Parent::processCollision(info);
}

bool Item::onSimLightQuery(sim::SimLightQuery& query) const {
    if (!itemData_ || itemData_->lightType == NoLight) {
        return false;
    }

    light_.type = sim::SimLight::Type::Point;
    light_.range = itemData_->lightRadius;
    light_.position = getBoxCenter();
    if (itemData_->lightType == ConstantLight) {
        light_.intensity =
            ColorF{itemData_->lightColor.x, itemData_->lightColor.y, itemData_->lightColor.z};
    } else if (itemData_->lightType == PulsingLight) {
        const Flt32 t = getManager() ? getManager()->getCurrentTime() : 0.0f;
        const Flt32 period = (itemData_->lightTime > 0.001f) ? itemData_->lightTime : 1.0f;
        const Flt32 s = 0.5f + 0.4f * std::sin(static_cast<Flt32>(std::numbers::pi) * t / period);
        light_.intensity = ColorF{
            itemData_->lightColor.x * s, itemData_->lightColor.y * s, itemData_->lightColor.z * s};
    }

    query.lights.push_back(light_);
    return true;
}

bool Item::processQuery(sim::SimQuery& query) {
    if (auto* lq = dynamic_cast<sim::SimLightQuery*>(&query)) {
        return onSimLightQuery(*lq);
    }
    return Parent::processQuery(query);
}

Int32 Item::throwGhostIndex(const sim::network::GhostManager* gm) const noexcept {
    if (explicitThrowGhostIndex_ != -1) {
        return explicitThrowGhostIndex_;
    }
    if (throwSource_ && gm) {
        if (const auto* netObj = dynamic_cast<const sim::SimNetObject*>(throwSource_)) {
            return gm->getGhostIndex(*netObj);
        }
    }
    return -1;
}

UInt32 Item::packUpdate(sim::network::GhostManager& gm, UInt32 mask, BitStream& stream) const {
    Parent::packUpdate(gm, mask & ~(PositionMask | RotationMask | VelocityMask), stream);

    if (stream.writeFlag((mask & InfoMask) != 0)) {
        packDatFile(stream);
        if (stream.writeFlag(sensorKey != -1)) {
            stream.writeInt(sensorKey, 7);
        }
        const Int32 ghost = throwGhostIndex(&gm);
        if (stream.writeFlag(ghost != -1)) {
            stream.writeInt(ghost, 10);
        }
        stream.writeFlag(isRotate_);
        stream.writeFlag(isCollideable_);
    }

    if (stream.writeFlag((mask & RotationMask) != 0)) {
        // Mission rotations are signed radians; the wire format is one unsigned
        // turn. Wrap before quantization (Raindance's flags have negative yaw).
        Flt32 yaw = std::fmod(getRot().z, kTwoPi);
        if (yaw < 0.0f) {
            yaw += kTwoPi;
        }
        stream.writeFloat(yaw / kTwoPi, 8);
    }

    if (stream.writeFlag((mask & PositionMask) != 0)) {
        const Point3F pos = getPos();
        writePod(stream, pos);
    }

    if (!stream.writeFlag(isAtRest() || isRotate_)) {
        if (stream.writeFlag((mask & VelocityMask) != 0)) {
            const Point3F vel = getLinearVelocity();
            writePod(stream, vel);
        }
    }

    return 0;
}

bool Item::unpackUpdate(sim::network::GhostManager& gm, BitStream& stream) {
    BitStreamRollbackGuard guard(stream);

    const auto savedDatId = datFileId_;
    const auto savedSensorKey = sensorKey;
    const auto savedThrowSource = throwSource_;
    const auto savedThrowGhost = explicitThrowGhostIndex_;
    const auto savedRotate = isRotate_;
    const auto savedCollide = isCollideable_;
    const auto savedRot = getRot();
    const auto savedPos = getPos();
    const auto savedVel = getLinearVelocity();
    const auto savedFlags = movementFlags();

    const auto rollback = [&]() noexcept {
        datFileId_ = savedDatId;
        sensorKey = savedSensorKey;
        throwSource_ = savedThrowSource;
        explicitThrowGhostIndex_ = savedThrowGhost;
        isRotate_ = savedRotate;
        isCollideable_ = savedCollide;
        setRot(savedRot);
        setPos(savedPos);
        setLinearVelocity(savedVel);
        setMovementFlags(savedFlags);
        return false;
    };

    if (!Parent::unpackUpdate(gm, stream)) {
        return rollback();
    }

    if (stream.readFlag()) {
        if (!unpackDatFile(stream)) {
            return rollback();
        }
        sensorKey = stream.readFlag() ? stream.readInt(7) : -1;
        if (stream.readFlag()) {
            const Int32 ghostId = stream.readInt(10);
            if (!stream.isValid()) {
                return rollback();
            }
            explicitThrowGhostIndex_ = ghostId;
            if (!isProperlyAdded()) {
                if (auto* resolved = gm.resolveGhost(static_cast<UInt16>(ghostId))) {
                    throwSource_ = dynamic_cast<GameBase*>(resolved);
                    if (throwSource_) {
                        deleteNotify(*throwSource_);
                    }
                }
            }
        } else {
            explicitThrowGhostIndex_ = -1;
        }
        isRotate_ = stream.readFlag();
        isCollideable_ = stream.readFlag();
        if (!stream.isValid()) {
            return rollback();
        }
        if (isProperlyAdded() && itemData_) {
            initResources(itemData_);
        }
    }

    if (stream.readFlag()) {
        const Flt32 zRot = stream.readFloat(8) * kTwoPi;
        if (!stream.isValid()) {
            return rollback();
        }
        setRot(Point3F{0.0f, 0.0f, zRot});
    }

    if (stream.readFlag()) {
        Point3F pos{};
        if (!readPod(stream, pos)) {
            return rollback();
        }
        setPos(pos);
    }

    if (stream.readFlag()) {
        setMovementFlags(movementFlags() | AtRest);
        setLinearVelocity(Point3F{0.0f, 0.0f, 0.0f});
    } else if (stream.readFlag()) {
        Point3F vel{};
        if (!readPod(stream, vel)) {
            return rollback();
        }
        setLinearVelocity(vel);
    }

    if (!stream.isValid()) {
        return rollback();
    }
    guard.committed = true;
    return true;
}

Persistent::Error
Item::read(StreamIO& sio, Int32 version, Int32 user, Persistent::LoadContext& ctx) {
    const Int32 startPos = sio.getPosition();
    const auto savedRotate = isRotate_;
    const auto savedCount = count_;

    Int32 fileVersion = 0;
    if (!readStreamPod(sio, fileVersion)) {
        sio.setPosition(startPos);
        return Persistent::Error::ReadError;
    }

    if (Parent::read(sio, version, user, ctx) != Persistent::Error::Ok) {
        sio.setPosition(startPos);
        return Persistent::Error::ReadError;
    }

    bool rotVal = false;
    Int32 cntVal = 1;
    if (!readStreamPod(sio, rotVal) || !readStreamPod(sio, cntVal)) {
        isRotate_ = savedRotate;
        count_ = savedCount;
        sio.setPosition(startPos);
        return Persistent::Error::ReadError;
    }

    isRotate_ = rotVal;
    count_ = cntVal;
    return Persistent::Error::Ok;
}

Persistent::Error
Item::write(StreamIO& sio, Int32 version, Int32 user, Persistent::StoreContext& ctx) const {
    const Int32 startPos = sio.getPosition();
    const Int32 fileVersion = 0;
    if (!writeStreamPod(sio, fileVersion)) {
        sio.setPosition(startPos);
        return Persistent::Error::WriteError;
    }
    if (Parent::write(sio, version, user, ctx) != Persistent::Error::Ok) {
        sio.setPosition(startPos);
        return Persistent::Error::WriteError;
    }
    if (!writeStreamPod(sio, isRotate_) || !writeStreamPod(sio, count_)) {
        sio.setPosition(startPos);
        return Persistent::Error::WriteError;
    }
    return Persistent::Error::Ok;
}

bool registerItemClass(Persistent::ClassRegistry& registry) {
    return registry.registerClass<Item>("Item", ItemPersTag, makeFourCC('I', 'T', 'E', 'M'));
}

bool registerItemNetwork(sim::network::Registry& registry) {
    return registry.registerGhost<Item>(static_cast<UInt16>(ItemPersTag));
}

} // namespace opent1::game
