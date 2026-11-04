#pragma once

#include <opent1/core/bit_stream.hpp>
#include <opent1/game/data_block_manager.hpp>
#include <opent1/game/f_base.hpp>
#include <opent1/game/game_base.hpp>
#include <opent1/sim/sim_net.hpp>

namespace opent1::game {

using SoundHandle = UInt32;
inline constexpr SoundHandle kInvalidSoundHandle = 0;

class SoundPlayback final : public sim::SimObject {
public:
    using Play = std::function<SoundHandle(UInt32, const TMat3F&, const Point3F&)>;
    using Move = std::function<void(SoundHandle, const TMat3F&, const Point3F&)>;
    using Stop = std::function<void(SoundHandle)>;
    using VoicePlay = std::function<SoundHandle(
        Int32, std::string_view, const SoundProfileData&, const TMat3F&, const Point3F&)>;

    explicit SoundPlayback(Play callback) : play(std::move(callback)) {
        assignName("GameSoundPlayback");
    }

    Play play;
    Play playTagged;
    VoicePlay playVoice;
    std::function<SoundHandle(UInt32, const Point3F&)> playRelative;
    Move move;
    Stop stop;
    std::function<bool(SoundHandle)> done;
    bool processEvent(const sim::SimEvent&) override;
};

struct LocSoundEvent : public sim::SimEvent {
    Int32 soundId{0};
    Point3F vec{0.0f, 0.0f, 0.0f};
    Flt32 vecLen{0.0f};
    Flt32 proMaxDist{100.0f};

    LocSoundEvent();
    ~LocSoundEvent() override = default;

    static void Post(sim::SimObjectId managerId,
                     Int32 soundId,
                     Point3F vec,
                     Flt32 vecLen,
                     Flt32 proMaxDist,
                     sim::SimManager* manager = nullptr);

    bool pack(sim::SimManager& manager, sim::network::PacketStream& ps, BitStream& bstream) const;
    bool unpack(sim::SimManager& manager, sim::network::PacketStream& ps, BitStream& bstream);
    bool packWithDbm(const DataBlockManager& dbm, BitStream& bstream) const;
    bool unpackWithDbm(const DataBlockManager& dbm, BitStream& bstream);
};

class TSFX {
public:
    static void Stop(SoundHandle&, sim::SimManager*);
    static void Move(SoundHandle, const TMat3F&, const Point3F&, sim::SimManager*);
    static bool IsDone(SoundHandle, sim::SimManager*);
    static SoundHandle PlayTaggedAt(UInt32 tag, const TMat3F& transform, sim::SimManager* manager);
    static SoundHandle PlayAt(UInt32 id,
                              const TMat3F& transform,
                              const Point3F& vel,
                              sim::SimManager* manager = nullptr);
    static void serverPlaySound(UInt32 id, Point3F pos, sim::SimManager* manager = nullptr);
    static void serverPlaySound(UInt32 id, GameBase* object, Int32 channel);
};

bool registerSoundEvents(sim::network::Registry& registry);

} // namespace opent1::game
