#include <opent1/game/sensor.hpp>
#include <opent1/game/sensor_manager.hpp>

namespace opent1::game {

namespace {

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

Sensor::Sensor(bool in_fCastLOS) : fCastLOS_(in_fCastLOS) {}

Sensor::~Sensor() = default;

bool Sensor::initResources(GameBaseData* io_pDat) {
    if (!Parent::initResources(io_pDat)) {
        return false;
    }
    sensorData_ = dynamic_cast<SensorData*>(io_pDat);
    if (!sensorData_) {
        return false;
    }
    if (!isGhost() && getManager()) {
        if (nextSensor_) {
            nextSensor_->prevSensor_ = prevSensor_;
        }
        if (prevSensor_) {
            prevSensor_->nextSensor_ = nextSensor_;
        }
        nextSensor_ = nullptr;
        prevSensor_ = nullptr;
        if (auto* rm = dynamic_cast<SensorManager*>(getManager()->findObject(SensorManagerId))) {
            rm->addSensor(this);
        }
    }
    return true;
}

bool Sensor::onAdd() {
    if (isGhost() && getManager()) {
        addToSet(SensorVisibleSetId);
    }
    return Parent::onAdd();
}

void Sensor::setTeam(Int32 teamId) {
    Parent::setTeam(teamId);
    if (isGhost()) {
        return;
    }
    if (nextSensor_) {
        nextSensor_->prevSensor_ = prevSensor_;
    }
    if (prevSensor_) {
        prevSensor_->nextSensor_ = nextSensor_;
    }
    nextSensor_ = nullptr;
    prevSensor_ = nullptr;
    if (getManager()) {
        if (auto* rm = dynamic_cast<SensorManager*>(getManager()->findObject(SensorManagerId))) {
            rm->addSensor(this);
        }
    }
}

void Sensor::onRemove() {
    if (!isGhost()) {
        if (nextSensor_) {
            nextSensor_->prevSensor_ = prevSensor_;
        }
        if (prevSensor_) {
            prevSensor_->nextSensor_ = nextSensor_;
        }
        nextSensor_ = nullptr;
        prevSensor_ = nullptr;
    }
    Parent::onRemove();
}

bool Sensor::detectItem(GameBase* obj) {
    if (!obj || !sensorData_ || !isActive()) {
        return false;
    }

    const Box3F& box = obj->getBoundingBox();
    const Point3F position = (box.fMin + box.fMax) * 0.5f;
    const Point3F distance = getLinearPosition() - position;

    if (distance.len() <= static_cast<Flt32>(sensorData_->iRange)) {
        if (sensorData_->iDopplerVelocity == 0 ||
            static_cast<Flt32>(sensorData_->iDopplerVelocity) <= obj->getLinearVelocity().len()) {
            if (sensorData_->fCastLOS && fCastLOS_) {
                return checkLOS(position);
            }
            return true;
        }
    }
    return false;
}

bool Sensor::checkLOS(const Point3F& pos) {
    if (!fCastLOS_) {
        return true;
    }

    sim::SimContainer* root =
        getManager()
            ? dynamic_cast<sim::SimContainer*>(getManager()->findObject(sim::RootContainerId))
            : nullptr;
    if (!root) {
        return true;
    }

    sim::SimContainerQuery query{};
    query.id = getId();
    query.type = ~UInt32{0};
    query.mask =
        SimTerrainObjectType | SimInteriorObjectType | StaticObjectType | MoveableObjectType;
    query.detail = -1;
    query.box.fMin = getBoxCenter();
    query.box.fMax = pos;

    sim::SimCollisionInfo info{};
    root->findLOS(query, info, sim::SimCollisionImageQuery::Normal);

    sim::SimObject* hitObj = getManager() ? info.object.resolve(*getManager()) : nullptr;
    if (auto* sb = dynamic_cast<StaticBase*>(hitObj)) {
        if (sb->getState() == StaticBase::Destroyed) {
            query.id = hitObj->getId();
            query.box.fMin = info.surfaces.tWorld.p;
            info.object = {};
            root->findLOS(query, info, sim::SimCollisionImageQuery::Normal);
            hitObj = getManager() ? info.object.resolve(*getManager()) : nullptr;
        }
    }
    return hitObj == nullptr;
}

UInt32 Sensor::packUpdate(sim::network::GhostManager& gm, UInt32 dwMask, BitStream& stream) const {
    Parent::packUpdate(gm, dwMask, stream);
    if (stream.writeFlag((dwMask & InitialUpdate) != 0)) {
        packDatFile(stream);
    }
    if (getGhostTag() == SensorPersTag && sensorKey != -1) {
        stream.writeInt(sensorKey + 1, 8);
    }
    return 0;
}

bool Sensor::unpackUpdate(sim::network::GhostManager& gm, BitStream& stream) {
    BitStreamRollbackGuard guard(stream);

    const auto savedDatId = datFileId_;
    const auto savedSensorInit = hasUnpackedInitialSensor_;
    const auto savedSensorKey = sensorKey;

    const auto rollback = [&]() noexcept {
        datFileId_ = savedDatId;
        hasUnpackedInitialSensor_ = savedSensorInit;
        sensorKey = savedSensorKey;
        return false;
    };

    if (!Parent::unpackUpdate(gm, stream)) {
        return rollback();
    }

    if (stream.readFlag()) {
        if (!unpackDatFile(stream)) {
            return rollback();
        }
        hasUnpackedInitialSensor_ = true;
    }
    if (getGhostTag() == SensorPersTag && stream.remainingReadBits() != 0) {
        sensorKey = stream.readInt(8) - 1;
    }

    if (!stream.isValid()) {
        return rollback();
    }
    guard.committed = true;
    return true;
}

Persistent::Error
Sensor::read(StreamIO& sio, Int32 version, Int32 user, Persistent::LoadContext& ctx) {
    return Parent::read(sio, version, user, ctx);
}

Persistent::Error
Sensor::write(StreamIO& sio, Int32 version, Int32 user, Persistent::StoreContext& ctx) const {
    return Parent::write(sio, version, user, ctx);
}

bool registerSensorClass(Persistent::ClassRegistry& registry) {
    return registry.registerClass<Sensor>("Sensor", SensorPersTag, makeFourCC('R', 'A', 'D', 'R'));
}

bool registerSensorNetwork(sim::network::Registry& registry) {
    return registry.registerGhost<Sensor>(static_cast<UInt16>(SensorPersTag));
}

} // namespace opent1::game
