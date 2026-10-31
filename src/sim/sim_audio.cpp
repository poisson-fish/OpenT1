#include <opent1/sim/sim_audio.hpp>
#include <opent1/sim/sim_world_game.hpp>

namespace opent1::sim {
namespace {
struct AudioPoll : SimEvent {};

bool valid(const SimSoundSequenceEvent& e) {
    return e.channel >= -1 && e.channel < 32 && audio::valid(e.transform);
}
}

bool SimSoundSequenceEvent::pack(BitStream& b) const {
    const auto bits = 39U + (spatial ? 416U : 0U);
    if (!valid(*this) || b.remainingWriteBits() < bits) {
        return false;
    }
    b.write(sound);
    b.writeInt(channel, 6);
    b.writeFlag(spatial);
    if (spatial) {
        b.write(transform.flags);
        for (const auto& row : transform.m) {
            for (auto v : row) {
                b.write(v);
            }
        }
        b.write(transform.p.x);
        b.write(transform.p.y);
        b.write(transform.p.z);
    }
    return b.isValid();
}

bool SimSoundSequenceEvent::unpack(BitStream& b) {
    auto e = *this;
    UInt32 id{};
    if (b.remainingReadBits() < 39 || !b.read(id)) {
        return false;
    }
    e.sound = id;
    e.channel = b.readInt(6);
    if (e.channel == 63) {
        e.channel = -1;
    }
    e.spatial = b.readFlag();
    if (e.spatial) {
        if (b.remainingReadBits() < 416 || !b.read(e.transform.flags)) {
            return false;
        }
        for (auto& row : e.transform.m) {
            for (auto& v : row) {
                if (!b.read(v)) {
                    return false;
                }
            }
        }
        if (!b.read(e.transform.p.x) || !b.read(e.transform.p.y) || !b.read(e.transform.p.z)) {
            return false;
        }
    }
    if (!b.isValid() || !valid(e)) {
        return false;
    }
    *this = e;
    return true;
}

bool registerSoundSequenceEvent(network::Registry& r) {
    return r.registerEvent<SimSoundSequenceEvent>(
        EventTypeFirst + 35,
        [](const auto& e, auto& b, auto&) {
            return e.packAddress(b) && e.pack(b);
        },
        [](auto& e, auto& b, auto&) {
            return e.unpackAddress(b) && e.unpack(b);
        },
        network::ReceivePolicy::Client);
}

bool SimAudio::schedule() {
    if (!getManager()) {
        return false;
    }
    auto e = std::make_unique<AudioPoll>();
    e->address = address();
    e->time = getManager()->getCurrentTime() + 2.f;
    return getManager()->postEvent(std::move(e));
}

bool SimAudio::onAdd() {
    if (!runtime_ || !getManager()) {
        return false;
    }
    previous_ = getManager()->getCurrentTime();
    healthy_ = true;
    return schedule();
}

void SimAudio::onRemove() {
    if (runtime_) {
        runtime_->stopAll();
        runtime_->clearSequences();
        runtime_->purge();
    }
}

bool SimAudio::synchronize() {
    if (!runtime_ || !getManager()) {
        return false;
    }
    const auto now = getManager()->getCurrentTime();
    if (now < previous_) {
        previous_ = now;
    } // SimManager::resetTime rebases queued polls
    if (!runtime_->advance(double(now) - previous_)) {
        healthy_ = false;
        return false;
    }
    previous_ = now;
    return true;
}

bool SimAudio::processEvent(const SimEvent& e) {
    if (const auto* sound = dynamic_cast<const SimSoundSequenceEvent*>(&e)) {
        return valid(*sound) && synchronize() &&
               runtime_->queue(sound->channel, sound->sound, sound->transform, sound->spatial);
    }
    if (dynamic_cast<const AudioPoll*>(&e)) {
        healthy_ = synchronize();
        if (healthy_) {
            healthy_ = runtime_->pollSequences();
            runtime_->purge();
        }
        healthy_ = schedule() && healthy_;
        return healthy_;
    }
    return SimObject::processEvent(e);
}

bool SimSoundSource::processEvent(const SimEvent& event) {
    if (const auto* transform = dynamic_cast<const SimObjectTransformEvent*>(&event)) {
        return setPosition(transform->transform, true);
    }
    if (const auto* edit = dynamic_cast<const SimEditEvent*>(&event)) {
        if (edit->editor != MissionEditorId) {
            return true;
        }
        if (edit->state && !addToSet(RenderSetId)) {
            return false;
        }
        if (!edit->state) {
            removeFromSet(RenderSetId);
        }
        editing_ = edit->state;
        return true;
    }
    if (const auto* trigger = dynamic_cast<const SimTriggerEvent*>(&event)) {
        if (!trigger->activate || channel < -1 || channel >= 32 || !audio::valid(getTransform()) ||
            (!deliver && !broadcast)) {
            return false;
        }
        auto payload = std::make_unique<SimSoundSequenceEvent>();
        payload->address.objectId = SoundSequenceId;
        payload->sound = sound;
        payload->channel = channel;
        payload->spatial = true;
        payload->transform = getTransform();
        try {
            return broadcast ? broadcast(*payload) : deliver(std::move(payload));
        } catch (...) {
            return false;
        }
    }
    return SimMovement::processEvent(event);
}

bool SimSoundSource::writeFields(StreamIO& s) const {
    return channel >= -1 && channel < 32 && s.write(sound) && s.write(channel);
}

bool SimSoundSource::readFields(StreamIO& s) {
    UInt32 nextSound{};
    Int32 nextChannel{};
    if (!s.read(nextSound) || !s.read(nextChannel) || nextChannel < -1 || nextChannel >= 32) {
        return false;
    }
    sound = nextSound;
    channel = nextChannel;
    return true;
}

bool SimSoundSource::onAdd() {
    if (!SimMovement::onAdd() || !getManager()) {
        return false;
    }
    auto* root = dynamic_cast<SimContainer*>(getManager()->findObject(RootContainerId));
    return root && root->addObject(*this);
}

bool SimSoundSource::processQuery(SimQuery& query) {
    if (auto* q = dynamic_cast<SimImageTransformQuery*>(&query)) {
        q->transform = getTransform();
        return true;
    }
    if (editing_) {
        if (auto* q = dynamic_cast<SimCollisionImageQuery*>(&query)) {
            if (!std::isfinite(markerRadius) || markerRadius < 0) {
                return false;
            }
            auto image = std::make_shared<SimCollisionSphereImage>();
            image->center = markerCenter;
            image->radius = markerRadius;
            image->trigger = true;
            image->transform = getTransform();
            q->images = {std::move(image)};
            return true;
        }
        if (marker && marker(query, getTransform())) {
            return true;
        }
    }
    return SimMovement::processQuery(query);
}

Persistent::Error
SimSoundSource::write(StreamIO& s, Int32 v, Int32 u, Persistent::StoreContext& c) const {
    const auto& t = getTransform();
    if (!audio::valid(t) || SimNetObject::write(s, v, u, c) != Persistent::Error::Ok ||
        !s.write(t.flags)) {
        return Persistent::Error::WriteError;
    }
    for (const auto& row : t.m) {
        for (auto f : row) {
            if (!s.write(f)) {
                return Persistent::Error::WriteError;
            }
        }
    }
    return s.write(t.p.x) && s.write(t.p.y) && s.write(t.p.z) && writeFields(s)
               ? Persistent::Error::Ok
               : Persistent::Error::WriteError;
}

Persistent::Error SimSoundSource::read(StreamIO& s, Int32 v, Int32 u, Persistent::LoadContext& c) {
    if (getManager()) {
        return Persistent::Error::ReadError;
    }
    SimSoundSource staged;
    TMat3F t;
    if (staged.SimNetObject::read(s, v, u, c) != Persistent::Error::Ok || !s.read(t.flags)) {
        return Persistent::Error::ReadError;
    }
    for (auto& row : t.m) {
        for (auto& f : row) {
            if (!s.read(f)) {
                return Persistent::Error::ReadError;
            }
        }
    }
    if (!s.read(t.p.x) || !s.read(t.p.y) || !s.read(t.p.z) || !audio::valid(t) ||
        !staged.readFields(s) || !setPosition(t, true)) {
        return Persistent::Error::ReadError;
    }
    setId(staged.getId());
    setNetFlags(staged.netFlags());
    setLocked(staged.isLocked());
    sound = staged.sound;
    channel = staged.channel;
    return Persistent::Error::Ok;
}

bool registerSoundSourceClass(Persistent::ClassRegistry& r) {
    return r.registerClass<SimSoundSource>("SimSoundSource", makeFourCC('s', 'n', 'd', 's'), 131);
}
} // namespace opent1::sim
