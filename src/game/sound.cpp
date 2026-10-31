#include <opent1/game/sound.hpp>
#include <opent1/game/f_base.hpp>
#include <opent1/game/fear_player_psc.hpp>
#include <opent1/game/player_manager.hpp>
#include <opent1/game/shape_base.hpp>
#include <opent1/sim/sim_world_game.hpp>

#include <algorithm>
#include <cmath>

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

bool SoundPlayback::processEvent(const sim::SimEvent& event) {
    if (const auto* sound = dynamic_cast<const LocSoundEvent*>(&event)) {
        // The legacy wire vector points from the source to the scope object.
        // Playback takes a listener-relative source position.
        if (playRelative) {
            (void)playRelative(static_cast<UInt32>(sound->soundId), -sound->vec);
        }
        return true;
    }
    return SimObject::processEvent(event);
}

LocSoundEvent::LocSoundEvent() {
    type = LocSoundEventType;
}

void LocSoundEvent::Post(sim::SimObjectId managerId,
                         Int32 soundId,
                         Point3F vec,
                         Flt32 vecLen,
                         Flt32 proMaxDist,
                         sim::SimManager* manager) {
    if (!manager) {
        return;
    }
    auto evt = std::make_unique<LocSoundEvent>();
    evt->address.managerId = managerId;
    evt->soundId = soundId;
    evt->vec = vec;
    evt->vecLen = vecLen;
    evt->proMaxDist = proMaxDist;
    manager->postEvent(std::move(evt), true);
}

bool LocSoundEvent::packWithDbm(const DataBlockManager& dbm, BitStream& bstream) const {
    dbm.writeBlockId(bstream, DataBlockManager::SoundDataType, soundId);
    if (bstream.writeFlag(vecLen > 0.05f)) {
        Point3F normVec = vec;
        normVec *= 1.0f / vecLen;
        bstream.writeNormalVector(NormalVector3F{normVec.x, normVec.y, normVec.z}, 8);
        const Flt32 ratio = proMaxDist > 0.0f ? std::clamp(vecLen / proMaxDist, 0.0f, 1.0f) : 0.0f;
        bstream.writeFloat(ratio, 9);
    }
    return bstream.isValid();
}

bool LocSoundEvent::unpackWithDbm(const DataBlockManager& dbm, BitStream& bstream) {
    BitStreamRollbackGuard guard(bstream);
    const auto backup = *this;

    const Int32 id = dbm.readBlockId(bstream, DataBlockManager::SoundDataType);
    const auto* snd =
        dynamic_cast<const SoundData*>(dbm.lookupDataBlock(id, DataBlockManager::SoundDataType));
    const SoundProfileData* pro =
        snd ? dynamic_cast<const SoundProfileData*>(
                  dbm.lookupDataBlock(snd->profileIndex, DataBlockManager::SoundProfileDataType))
            : nullptr;

    const Flt32 maxDist = pro ? pro->profile.maxDistance : proMaxDist;
    Point3F decodedVec{0.0f, 0.0f, 0.0f};
    if (bstream.readFlag()) {
        NormalVector3F nv{};
        bstream.readNormalVector(nv, 8);
        const Flt32 pct = bstream.readFloat(9);
        decodedVec.set(nv.x, nv.y, nv.z);
        decodedVec *= (pct * maxDist);
    }
    if (!bstream.isValid()) {
        *this = backup;
        return false;
    }

    soundId = id;
    vec = decodedVec;
    vecLen = decodedVec.len();
    proMaxDist = maxDist;
    address.objectId = static_cast<sim::SimObjectId>(-2);
    guard.committed = true;
    return true;
}

bool LocSoundEvent::pack(sim::SimManager& manager,
                         sim::network::PacketStream& /*ps*/,
                         BitStream& bstream) const {
    auto* dbm = dynamic_cast<DataBlockManager*>(manager.findObject(DataBlockManagerId));
    if (!dbm) {
        return false;
    }
    return packWithDbm(*dbm, bstream);
}

bool LocSoundEvent::unpack(sim::SimManager& manager,
                           sim::network::PacketStream& /*ps*/,
                           BitStream& bstream) {
    auto* dbm = dynamic_cast<DataBlockManager*>(manager.findObject(DataBlockManagerId));
    if (!dbm) {
        return false;
    }
    if (!unpackWithDbm(*dbm, bstream)) {
        return false;
    }
    if (auto* playback = dynamic_cast<SoundPlayback*>(manager.findObject("GameSoundPlayback"))) {
        address = playback->address();
    }
    return true;
}

SoundHandle TSFX::PlayTaggedAt(UInt32 tag, const TMat3F& transform, sim::SimManager* manager) {
    auto* playback =
        manager ? dynamic_cast<SoundPlayback*>(manager->findObject("GameSoundPlayback")) : nullptr;
    return playback && playback->playTagged ? playback->playTagged(tag, transform, {})
                                            : kInvalidSoundHandle;
}

SoundHandle
TSFX::PlayAt(UInt32 id, const TMat3F& transform, const Point3F& vel, sim::SimManager* manager) {
    if (!manager) {
        return kInvalidSoundHandle;
    }
    auto* dbm = dynamic_cast<DataBlockManager*>(manager->findObject(DataBlockManagerId));
    if (!dbm) {
        return kInvalidSoundHandle;
    }
    auto* snd = dynamic_cast<SoundData*>(
        dbm->lookupDataBlock(static_cast<Int32>(id), DataBlockManager::SoundDataType));
    if (!snd || snd->wavFileName.empty()) {
        return kInvalidSoundHandle;
    }
    auto* playback = dynamic_cast<SoundPlayback*>(manager->findObject("GameSoundPlayback"));
    return playback && playback->play ? playback->play(id, transform, vel) : kInvalidSoundHandle;
}

void TSFX::serverPlaySound(UInt32 id, Point3F pos, sim::SimManager* manager) {
    if (!manager) {
        return;
    }
    auto* dbm = dynamic_cast<DataBlockManager*>(manager->findObject(DataBlockManagerId));
    if (!dbm) {
        return;
    }
    auto* snd = dynamic_cast<SoundData*>(
        dbm->lookupDataBlock(static_cast<Int32>(id), DataBlockManager::SoundDataType));
    if (!snd || snd->profileIndex == -1) {
        return;
    }
    auto* pro = dynamic_cast<SoundProfileData*>(
        dbm->lookupDataBlock(snd->profileIndex, DataBlockManager::SoundProfileDataType));
    if (!pro) {
        return;
    }

    bool remote{};
    if (auto* set = dynamic_cast<sim::SimSet*>(manager->findObject(sim::GhostManagerSetId))) {
        for (sim::SimObject* obj : set->objects()) {
            auto* ghosts = dynamic_cast<sim::network::GhostManager*>(obj);
            if (!ghosts || !ghosts->owner()) {
                continue;
            }
            remote = true;
            auto* gb = dynamic_cast<GameBase*>(ghosts->scopeObject());
            if (!gb) {
                continue;
            }
            const Point3F vec = gb->getLinearPosition() - pos;
            const Flt32 vecLen = vec.len();
            if (vecLen < pro->dist) {
                auto event = std::make_unique<LocSoundEvent>();
                event->soundId = static_cast<Int32>(id);
                event->vec = vec;
                event->vecLen = vecLen;
                event->proMaxDist = pro->profile.maxDistance;
                (void)ghosts->owner()->sendRemoteEvent(std::move(event));
            }
        }
    }
    if (!remote && manager->findObject("GameSoundPlayback")) {
        TMat3F transform;
        transform.identity();
        transform.p = pos;
        (void)PlayAt(id, transform, {}, manager);
    }
}

void TSFX::serverPlaySound(UInt32 id, GameBase* object, Int32 channel) {
    if (!object || !object->getManager() || channel < 0 || channel >= ShapeBase::MaxSoundChannels) {
        return;
    }
    auto* manager = object->getManager();
    auto* blocks = dynamic_cast<DataBlockManager*>(manager->findObject(DataBlockManagerId));
    auto* sound = blocks ? dynamic_cast<SoundData*>(blocks->lookupDataBlock(
                               static_cast<Int32>(id), DataBlockManager::SoundDataType))
                         : nullptr;
    auto* profile = sound ? dynamic_cast<SoundProfileData*>(blocks->lookupDataBlock(
                                sound->profileIndex, DataBlockManager::SoundProfileDataType))
                          : nullptr;
    if (!profile) {
        return;
    }
    bool remote{};
    if (auto* set = dynamic_cast<sim::SimSet*>(manager->findObject(sim::GhostManagerSetId))) {
        for (auto* member : set->objects()) {
            auto* ghosts = dynamic_cast<sim::network::GhostManager*>(member);
            if (!ghosts || !ghosts->owner()) {
                continue;
            }
            remote = true;
            auto* scope = dynamic_cast<GameBase*>(ghosts->scopeObject());
            const auto index = ghosts->getGhostIndex(*object);
            if (!scope || index == sim::InvalidGhostIndex ||
                (scope->getLinearPosition() - object->getLinearPosition()).len() >= profile->dist) {
                continue;
            }
            auto event = std::make_unique<SoundEvent>();
            event->flags = sim::SimEvent::Guaranteed;
            event->ghostIndex = index;
            event->channel = channel;
            event->soundId = static_cast<Int32>(id);
            (void)ghosts->owner()->sendRemoteEvent(std::move(event));
        }
    }
    if (!remote && manager->findObject("GameSoundPlayback")) {
        SoundEvent event;
        event.channel = channel;
        event.soundId = static_cast<Int32>(id);
        (void)object->processEvent(event);
    }
}

bool registerSoundEvents(sim::network::Registry& registry) {
    return registry.registerEvent<LocSoundEvent>(
        LocSoundEventType,
        [](const LocSoundEvent& evt, BitStream& bs, sim::network::EventContext& ctx) {
            return evt.pack(ctx.manager, ctx.stream, bs);
        },
        [](LocSoundEvent& evt, BitStream& bs, sim::network::EventContext& ctx) {
            return evt.unpack(ctx.manager, ctx.stream, bs);
        },
        sim::network::ReceivePolicy::Client);
}

} // namespace opent1::game
