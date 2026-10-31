#pragma once

#include <opent1/game/player.hpp>
#include <opent1/game/sensor.hpp>
#include <opent1/sim/sim_net.hpp>

#include <array>
#include <string>
#include <string_view>

namespace opent1::game {

struct TargetNameEvent : public sim::SimEvent {
    Int32 targId{0};
    std::string name{};

    TargetNameEvent();
    ~TargetNameEvent() override = default;

    bool pack(sim::SimManager& manager, sim::network::PacketStream& ps, BitStream& bs) const;
    bool unpack(sim::SimManager& manager, sim::network::PacketStream& ps, BitStream& bs);
};

class SensorManager : public sim::SimNetObject {
    using Parent = sim::SimNetObject;

public:
    enum Constants : Int32 {
        MaxTargets = 128,
        NumTeamBuckets = 9, // team -1..7 -> index team + 1
    };

    static inline bool gSensorNetworkEnabled = true;

    struct TargetEntry {
        GameBase* obj{nullptr};
        std::string name{};
    };

    SensorManager();
    ~SensorManager() override;

    bool onAdd() override;
    void onRemove() override;
    void onDeleteNotify(sim::SimObject& object) override;
    bool processEvent(const sim::SimEvent& event) override;

    void addSensor(Sensor* pSensor);
    void addPlayerSupressor(Player* pl);
    void testObject(GameBase* obj);

    void newClient(Int32 managerId);
    Int32 addTarget(GameBase* targ, std::string_view name);
    void removeTarget(Int32 key);
    void changeTargetName(Int32 key, std::string_view name);

    [[nodiscard]] const TargetEntry& target(Int32 key) const noexcept {
        return targets_[static_cast<std::size_t>(key)];
    }

    [[nodiscard]] Sensor* sensorListHead(Int32 team) const noexcept {
        const Int32 idx = team + 1;
        if (idx < 0 || idx >= NumTeamBuckets) {
            return nullptr;
        }
        return sensorListHeads_[static_cast<std::size_t>(idx)].nextSensor();
    }

    [[nodiscard]] Sensor* supressionListHead() const noexcept {
        return supressionListHead_.nextSensor();
    }

    [[nodiscard]] Player* playerSupressorListHead() const noexcept {
        return playerSupressorListHead_.nextSup();
    }

private:
    std::array<Sensor, NumTeamBuckets> sensorListHeads_{};
    Sensor supressionListHead_{};
    Player playerSupressorListHead_{};
    std::array<TargetEntry, MaxTargets> targets_{};
    Int32 lastKey_{0};
};

bool registerSensorManagerEvents(sim::network::Registry& registry);

} // namespace opent1::game
