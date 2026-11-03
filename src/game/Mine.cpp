#include <opent1/game/explosion.hpp>
#include <opent1/game/mine.hpp>
#include <opent1/game/player_manager.hpp>

#include <cmath>

namespace opent1::game {

namespace {

constexpr UInt32 ServerCollisionMask = SimTerrainObjectType | SimInteriorObjectType |
                                       SimPlayerObjectType | StaticObjectType | VehicleObjectType |
                                       MoveableObjectType | MineObjectType;

constexpr UInt32 ClientCollisionMask = SimTerrainObjectType | SimInteriorObjectType |
                                       StaticObjectType | VehicleObjectType | MoveableObjectType;

} // namespace

Mine::Mine() {
    setType(MineObjectType);
    setForceMask(static_cast<UInt32>(-1));
    setMovementFlags((movementFlags() | UseElasticity | UseFriction | UseDrag | UseCurrent) &
                     ~RotateBoundingBox);
}

bool Mine::initResources(GameBaseData* in_data) {
    if (!Parent::initResources(in_data)) {
        return false;
    }
    mineData_ = dynamic_cast<MineData*>(in_data);
    return mineData_ != nullptr;
}

bool Mine::onAdd() {
    if (!Parent::onAdd()) {
        return false;
    }

    if (isGhost()) {
        setCollisionMask(ClientCollisionMask);
        addToSet(SensorVisibleSetId);
    } else {
        setCollisionMask(ServerCollisionMask);
    }
    return true;
}

void Mine::serverProcess(UInt32 curTime) {
    const Flt32 timeInterval = static_cast<Flt32>(curTime - getLastProcessTime()) * 0.001f;
    const Int32 prevState = getState();
    Parent::serverProcess(curTime);
    if (getState() != prevState) {
        onDamageStateChange(prevState);
    }

    if (isAtRest()) {
        sinkIn(timeInterval);
    } else {
        groundNormal_.set(0.0f, 0.0f, 1.0f);
    }
}

void Mine::clientProcess(UInt32 curTime) {
    const Flt32 timeInterval = static_cast<Flt32>(curTime - getLastProcessTime()) * 0.001f;
    Parent::clientProcess(curTime);

    if (isAtRest()) {
        sinkIn(timeInterval);
    } else {
        groundNormal_.set(0.0f, 0.0f, 1.0f);
    }
}

void Mine::sinkIn(Flt32 interval) {
    if (groundNormal_.z == 1.0f) {
        return;
    }

    Point3F rot = getRot();
    Point3F rotNorm{std::sin(rot.y), -std::sin(rot.x), 0.0f};
    const Flt32 xySq = rotNorm.x * rotNorm.x + rotNorm.y * rotNorm.y;
    rotNorm.z = (xySq < 1.0f) ? std::sqrt(1.0f - xySq) : 0.0f;

    if (m_dot(rotNorm, groundNormal_) <= 0.99f) {
        const Flt32 step = 0.05f * (interval / 0.032f);
        if (-std::sin(rot.x) < groundNormal_.y) {
            rot.x -= step;
        } else {
            rot.x += step;
        }
        if (std::sin(rot.y) < groundNormal_.x) {
            rot.y += step;
        } else {
            rot.y -= step;
        }
        setRot(rot);
        setMaskBits(MineRotMask);
    }
}

void Mine::onDamageStateChange(Int32 /*oldState*/) {
    if (getState() == Destroyed && !isGhost() && mineData_) {
        Explosion::applyRadiusDamage(getManager(),
                                     mineData_->damageType,
                                     getBoxCenter(),
                                     Point3F{0.0f, 0.0f, 0.0f},
                                     mineData_->explosionRadius,
                                     mineData_->damageValue,
                                     mineData_->kickBackStrength,
                                     getDamageId());
    }
}

bool Mine::processCollision(MovementInfo& info) {
    for (std::size_t i = 0; i < info.collisionList.size();) {
        sim::SimObject* obj =
            getManager() ? info.collisionList[i].object.resolve(*getManager()) : nullptr;
        if (!obj) {
            ++i;
            continue;
        }
        if ((obj->getType() & (SimInteriorObjectType | SimTerrainObjectType)) != 0) {
            auto& sList = info.collisionList[i].surfaces;
            for (const auto& surf : sList.surfaces) {
                Point3F tNorm{};
                m_mul(surf.normal, static_cast<const RMat3F&>(sList.tWorld), tNorm);
                if (tNorm.z > 0.0f) {
                    groundNormal_ = tNorm;
                }
            }
        }
        if (obj->getId() == getOwnerId()) {
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

bool Mine::onSimTimerEvent(const sim::SimTimerEvent& /*event*/) {
    return true;
}

bool Mine::processEvent(const sim::SimEvent& event) {
    if (const auto* te = dynamic_cast<const sim::SimTimerEvent*>(&event)) {
        return onSimTimerEvent(*te);
    }
    return Parent::processEvent(event);
}

Int32 Mine::getDamageId() const noexcept {
    if (throwerClientId_ == 0 || throwerClientId_ == -1 || !getManager()) {
        return 0;
    }
    if (auto* pPM = PlayerManager::get(getManager())) {
        if (const auto* pCR = pPM->findBaseRep(throwerClientId_)) {
            if (pCR->loginId == throwerClientLoginId_) {
                return throwerClientId_;
            }
        }
    }
    return 0;
}

UInt32 Mine::packUpdate(sim::network::GhostManager& gm, UInt32 mask, BitStream& stream) const {
    if (isAtRest()) {
        mask &= ~RotationMask;
    }
    return Parent::packUpdate(gm, mask, stream);
}

bool Mine::unpackUpdate(sim::network::GhostManager& gm, BitStream& stream) {
    return Parent::unpackUpdate(gm, stream);
}

Persistent::Error
Mine::read(StreamIO& sio, Int32 version, Int32 user, Persistent::LoadContext& ctx) {
    return Parent::read(sio, version, user, ctx);
}

Persistent::Error
Mine::write(StreamIO& sio, Int32 version, Int32 user, Persistent::StoreContext& ctx) const {
    return Parent::write(sio, version, user, ctx);
}

bool registerMineClass(Persistent::ClassRegistry& registry) {
    return registry.registerClass<Mine>("Mine", MinePersTag, makeFourCC('M', 'I', 'N', 'E'));
}

bool registerMineNetwork(sim::network::Registry& registry) {
    return registry.registerGhost<Mine>(static_cast<UInt16>(MinePersTag));
}

} // namespace opent1::game
