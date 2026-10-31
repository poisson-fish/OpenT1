#include <opent1/game/player_manager.hpp>
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

TargetNameEvent::TargetNameEvent() {
    type = TargetNameEventType;
    flags |= SimEvent::Guaranteed | SimEvent::Ordered;
}

bool TargetNameEvent::pack(sim::SimManager& /*manager*/,
                           sim::network::PacketStream& /*ps*/,
                           BitStream& bs) const {
    bs.writeInt(targId, 7);
    bs.writeString(name);
    return bs.isValid();
}

bool TargetNameEvent::unpack(sim::SimManager& /*manager*/,
                             sim::network::PacketStream& /*ps*/,
                             BitStream& bs) {
    BitStreamRollbackGuard guard(bs);
    const Int32 readId = bs.readInt(7);
    std::string readName{};
    if (!bs.readString(readName) || !bs.isValid()) {
        return false;
    }
    targId = readId;
    name = std::move(readName);
    address.objectId = SensorManagerId;
    address.managerId = 0;
    guard.committed = true;
    return true;
}

SensorManager::SensorManager() {
    setId(SensorManagerId);
}

SensorManager::~SensorManager() = default;

bool SensorManager::onAdd() {
    return Parent::onAdd();
}

void SensorManager::onRemove() {
    for (auto& head : sensorListHeads_) {
        Sensor* cur = head.nextSensor_;
        while (cur) {
            Sensor* next = cur->nextSensor_;
            cur->prevSensor_ = nullptr;
            cur->nextSensor_ = nullptr;
            cur = next;
        }
        head.nextSensor_ = nullptr;
        head.prevSensor_ = nullptr;
    }

    Sensor* sup = supressionListHead_.nextSensor_;
    while (sup) {
        Sensor* next = sup->nextSensor_;
        sup->prevSensor_ = nullptr;
        sup->nextSensor_ = nullptr;
        sup = next;
    }
    supressionListHead_.nextSensor_ = nullptr;
    supressionListHead_.prevSensor_ = nullptr;

    Player* pl = playerSupressorListHead_.nextSup();
    while (pl) {
        Player* next = pl->nextSup();
        pl->setSupressorLinks(nullptr, nullptr);
        pl = next;
    }
    playerSupressorListHead_.setSupressorLinks(nullptr, nullptr);

    Parent::onRemove();
}

void SensorManager::onDeleteNotify(sim::SimObject& object) {
    if (auto* gb = dynamic_cast<GameBase*>(&object)) {
        if (gb->sensorKey != -1) {
            removeTarget(gb->sensorKey);
        }
    }
    Parent::onDeleteNotify(object);
}

void SensorManager::newClient(Int32 managerId) {
    if (!getManager()) {
        return;
    }
    for (Int32 i = 0; i < MaxTargets; ++i) {
        const auto& entry = targets_[static_cast<std::size_t>(i)];
        if (entry.obj && !entry.name.empty()) {
            auto ev = std::make_unique<TargetNameEvent>();
            ev->targId = i;
            ev->name = entry.name;
            ev->address.objectId = SensorManagerId;
            ev->address.managerId = static_cast<sim::SimObjectId>(managerId);
            getManager()->postEvent(std::move(ev));
        }
    }
}

void SensorManager::changeTargetName(Int32 key, std::string_view name) {
    if (key < 0 || key >= MaxTargets) {
        return;
    }
    auto& entry = targets_[static_cast<std::size_t>(key)];
    if (!entry.obj) {
        return;
    }
    entry.name = std::string(name);
    if (!getManager()) {
        return;
    }
    if (auto* pm = PlayerManager::get(getManager())) {
        for (PlayerManager::ClientRep* cl = pm->getClientList(); cl != nullptr;
             cl = static_cast<PlayerManager::ClientRep*>(cl->nextClient)) {
            auto ev = std::make_unique<TargetNameEvent>();
            ev->targId = key;
            ev->name = entry.name;
            ev->address.objectId = SensorManagerId;
            ev->address.managerId = static_cast<sim::SimObjectId>(cl->id);
            getManager()->postEvent(std::move(ev));
        }
    }
}

Int32 SensorManager::addTarget(GameBase* targ, std::string_view name) {
    if (!targ) {
        return -1;
    }
    if (targ->sensorKey != -1) {
        changeTargetName(targ->sensorKey, name);
        return targ->sensorKey;
    }

    for (Int32 i = 0; i < MaxTargets; ++i) {
        const Int32 idx = (lastKey_ + i) % MaxTargets;
        if (!targets_[static_cast<std::size_t>(idx)].obj) {
            deleteNotify(*targ);
            targets_[static_cast<std::size_t>(idx)].obj = targ;
            targets_[static_cast<std::size_t>(idx)].name.clear();
            changeTargetName(idx, name);
            lastKey_ = idx + 1;
            targ->sensorKey = idx;
            return idx;
        }
    }
    return -1;
}

void SensorManager::removeTarget(Int32 key) {
    if (key < 0 || key >= MaxTargets) {
        return;
    }
    auto& entry = targets_[static_cast<std::size_t>(key)];
    if (!entry.obj) {
        return;
    }
    if (!entry.name.empty()) {
        changeTargetName(key, "");
    }
    entry.obj->sensorKey = -1;
    clearNotify(*entry.obj);
    entry.obj = nullptr;
    entry.name.clear();
}

bool SensorManager::processEvent(const sim::SimEvent& event) {
    if (event.type == TargetNameEventType) {
        const auto& ev = static_cast<const TargetNameEvent&>(event);
        if (ev.targId >= 0 && ev.targId < MaxTargets) {
            targets_[static_cast<std::size_t>(ev.targId)].name = ev.name;
        }
        return true;
    }
    return Parent::processEvent(event);
}

void SensorManager::addPlayerSupressor(Player* pl) {
    if (!pl) {
        return;
    }
    Player* headNext = playerSupressorListHead_.nextSup();
    pl->setSupressorLinks(headNext, &playerSupressorListHead_);
    playerSupressorListHead_.setSupressorLinks(pl, nullptr);
    if (headNext) {
        headNext->setSupressorLinks(headNext->nextSup(), pl);
    }
}

void SensorManager::addSensor(Sensor* pSensor) {
    if (!pSensor) {
        return;
    }
    if (pSensor->nextSensor_) {
        pSensor->nextSensor_->prevSensor_ = pSensor->prevSensor_;
    }
    if (pSensor->prevSensor_) {
        pSensor->prevSensor_->nextSensor_ = pSensor->nextSensor_;
    }
    pSensor->nextSensor_ = nullptr;
    pSensor->prevSensor_ = nullptr;

    if (pSensor->isSupressionSensor()) {
        pSensor->nextSensor_ = supressionListHead_.nextSensor_;
        pSensor->prevSensor_ = &supressionListHead_;
        supressionListHead_.nextSensor_ = pSensor;
        if (pSensor->nextSensor_) {
            pSensor->nextSensor_->prevSensor_ = pSensor;
        }
        return;
    }

    const Int32 bucket = pSensor->getTeam() + 1;
    if (bucket < 0 || bucket >= NumTeamBuckets) {
        return;
    }
    Sensor* walk = &sensorListHeads_[static_cast<std::size_t>(bucket)];
    if (!pSensor->isSupressableSensor()) {
        if (!pSensor->isPinger()) {
            while (walk->nextSensor_ && !walk->nextSensor_->isSupressableSensor() &&
                   walk->nextSensor_->isPinger()) {
                walk = walk->nextSensor_;
            }
        }
    } else {
        while (walk->nextSensor_ && !walk->nextSensor_->isSupressableSensor()) {
            walk = walk->nextSensor_;
        }
        if (!pSensor->isPinger()) {
            while (walk->nextSensor_ && walk->nextSensor_->isPinger()) {
                walk = walk->nextSensor_;
            }
        }
    }

    pSensor->nextSensor_ = walk->nextSensor_;
    pSensor->prevSensor_ = walk;
    walk->nextSensor_ = pSensor;
    if (pSensor->nextSensor_) {
        pSensor->nextSensor_->prevSensor_ = pSensor;
    }
}

void SensorManager::testObject(GameBase* obj) {
    if (!obj) {
        return;
    }

    obj->setSensorPinged(GameBase::PingStatusClear);
    for (Int32 iTeam = 0; iTeam < 8; ++iTeam) {
        obj->setVisibleToTeam(iTeam, false);
    }

    if (!gSensorNetworkEnabled) {
        return;
    }

    if (dynamic_cast<StaticBase*>(obj) != nullptr) {
        const Int32 ownTeam = obj->getTeam();
        if (ownTeam >= 0 && ownTeam < 8) {
            obj->setVisibleToTeam(ownTeam, true);
        }
    }

    for (Sensor* pSensor = supressionListHead_.nextSensor_; pSensor != nullptr;
         pSensor = pSensor->nextSensor_) {
        if (pSensor->detectItem(obj)) {
            obj->setSensorPinged(GameBase::PingStatusSupressed);
            break;
        }
    }

    if (obj->getSensorPinged() != GameBase::PingStatusSupressed) {
        for (Player* pl = playerSupressorListHead_.nextSup(); pl != nullptr; pl = pl->nextSup()) {
            const Flt32 dist = (pl->getLinearPosition() - obj->getLinearPosition()).len();
            if (dist < pl->getSupressionField()) {
                obj->setSensorPinged(GameBase::PingStatusSupressed);
                break;
            }
        }
    }

    for (Int32 iTeam = 0; iTeam < 8; ++iTeam) {
        if (obj->getAlwaysVisibleToTeam(iTeam)) {
            obj->setVisibleToTeam(iTeam, true);
            continue;
        }
        if (obj->getNeverVisibleToTeam(iTeam)) {
            obj->setVisibleToTeam(iTeam, false);
            continue;
        }

        for (Sensor* pSensor = sensorListHeads_[static_cast<std::size_t>(iTeam + 1)].nextSensor_;
             pSensor != nullptr;
             pSensor = pSensor->nextSensor_) {
            if (pSensor->isSupressableSensor() &&
                obj->getSensorPinged() == GameBase::PingStatusSupressed) {
                break;
            }
            if (pSensor->detectItem(obj)) {
                obj->setVisibleToTeam(iTeam, true);
                if (pSensor->isPinger()) {
                    obj->setSensorPinged(GameBase::PingStatusOn);
                    break;
                }
            }
        }

        if (obj->getVisibleToTeam(iTeam) ||
            obj->getSensorPinged() == GameBase::PingStatusSupressed) {
            continue;
        }

        if (getManager()) {
            if (auto* pm = PlayerManager::get(getManager())) {
                for (PlayerManager::ClientRep* pClient = pm->getClientList(); pClient != nullptr;
                     pClient = static_cast<PlayerManager::ClientRep*>(pClient->nextClient)) {
                    GameBase* playerObj =
                        pClient->ownedObject ? pClient->ownedObject : pClient->controlObject;
                    auto* pl = dynamic_cast<Player*>(playerObj);
                    if (pClient->team == iTeam && pl) {
                        if (pl->detectItem(obj)) {
                            obj->setVisibleToTeam(iTeam, true);
                            break;
                        }
                    }
                }
            }
        }
    }
}

bool registerSensorManagerEvents(sim::network::Registry& registry) {
    return registry.registerEvent<TargetNameEvent>(
        TargetNameEventType,
        [](const TargetNameEvent& ev, BitStream& s, sim::network::EventContext& c) {
            ev.pack(c.manager, c.stream, s);
            return s.isValid();
        },
        [](TargetNameEvent& ev, BitStream& s, sim::network::EventContext& c) {
            return ev.unpack(c.manager, c.stream, s);
        });
}

} // namespace opent1::game
