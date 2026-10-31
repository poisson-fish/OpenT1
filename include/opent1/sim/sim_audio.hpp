#pragma once
#include <opent1/audio/audio.hpp>
#include <opent1/sim/sim_net.hpp>
#include <opent1/sim/sim_collision_movement.hpp>
#include <opent1/sim/sim_editor_events.hpp>

namespace opent1::sim {
inline constexpr SimObjectId SoundSequenceId = 39;

struct SimSoundSequenceEvent : SimEvent {
    SimSoundSequenceEvent() {
        type = EventTypeFirst + 35;
        flags = Guaranteed | Ordered;
    }

    UInt32 sound{};
    Int32 channel{-1};
    bool spatial{};
    TMat3F transform;
    // Native payload: 32-bit ID, six-bit channel (-1 = 63), spatial bit,
    // optional 52-byte TMat3F (flags, nine floats, translation). No ABI memcpy.
    bool pack(BitStream&) const;
    bool unpack(BitStream&);
};

bool registerSoundSequenceEvent(network::Registry&);

class SimAudio : public SimObject {
public:
    explicit SimAudio(std::shared_ptr<audio::Runtime> runtime) : runtime_(std::move(runtime)) {
        setId(SoundSequenceId);
    }

    bool onAdd() override;
    void onRemove() override;
    bool processEvent(const SimEvent&) override;
    bool synchronize();

    [[nodiscard]] bool healthy() const {
        return healthy_;
    }

private:
    bool schedule();
    std::shared_ptr<audio::Runtime> runtime_;
    SimTime previous_{};
    bool healthy_{true};
};

struct SimSoundTriggerEvent : SimTriggerEvent {};

// Marker transform and native sound/channel fields. The host supplies the
// broadcast destination; generated payloads own all data until delivery ends.
class SimSoundSource : public SimMovement {
public:
    SimSoundSource() {
        setType(MissionObjectType | MarkerObjectType);
    }

    UInt32 sound{};
    Int32 channel{-1};
    std::function<bool(std::unique_ptr<SimSoundSequenceEvent>)> deliver;
    // A server host can atomically stage fanout to all ordered client queues.
    std::function<bool(const SimSoundSequenceEvent&)> broadcast;
    std::function<bool(SimQuery&, const TMat3F&)> marker;
    Point3F markerCenter{};
    float markerRadius{};
    bool onAdd() override;
    bool processEvent(const SimEvent&) override;
    bool processQuery(SimQuery&) override;
    bool writeFields(StreamIO&) const;
    bool readFields(StreamIO&);
    Persistent::Error read(StreamIO&, Int32, Int32, Persistent::LoadContext&) override;
    Persistent::Error write(StreamIO&, Int32, Int32, Persistent::StoreContext&) const override;

private:
    bool editing_{};
};

bool registerSoundSourceClass(Persistent::ClassRegistry&);
} // namespace opent1::sim
