#pragma once

#include <opent1/game/item.hpp>

namespace opent1::game {

class Mine : public Item {
    using Parent = Item;

public:
    using MineData = opent1::game::MineData;

    enum MineMaskBits : UInt32 {
        MineRotMask = Parent::NextFreeMask,
        NextFreeMask = Parent::NextFreeMask << 1,
    };

    Mine();
    ~Mine() override = default;

    [[nodiscard]] Int32 getDatGroup() const noexcept override {
        return DataBlockManager::MineDataType;
    }

    bool processArguments(std::span<const std::string> args) override;
    bool initResources(GameBaseData* in_data) override;
    bool onAdd() override;
    void onCollisionNotify(GameBase* object) override;
    bool processEvent(const sim::SimEvent& event) override;

    void serverProcess(UInt32 curTime) override;
    void clientProcess(UInt32 curTime) override;

    void onDamageStateChange(Int32 oldState) override;
    void sinkIn(Flt32 interval);
    [[nodiscard]] Int32 getDamageId() const noexcept;

    [[nodiscard]] const MineData* mineData() const noexcept {
        return mineData_;
    }

    [[nodiscard]] const Point3F& groundNormal() const noexcept {
        return groundNormal_;
    }

    void setGroundNormal(const Point3F& n) noexcept {
        groundNormal_ = n;
    }

    UInt32
    packUpdate(sim::network::GhostManager& gm, UInt32 mask, BitStream& stream) const override;
    bool unpackUpdate(sim::network::GhostManager& gm, BitStream& stream) override;

    Persistent::Error
    read(StreamIO& sio, Int32 version, Int32 user, Persistent::LoadContext& ctx) override;
    Persistent::Error
    write(StreamIO& sio, Int32 version, Int32 user, Persistent::StoreContext& ctx) const override;

    [[nodiscard]] std::string_view persistClassName() const noexcept {
        return "Mine";
    }

    [[nodiscard]] UInt32 persistTag() const noexcept {
        return MinePersTag;
    }

    [[nodiscard]] UInt16 getGhostTag() const override {
        return static_cast<UInt16>(MinePersTag);
    }

protected:
    bool processCollision(MovementInfo& info) override;
    bool onSimTimerEvent(const sim::SimTimerEvent& event);

    MineData* mineData_{nullptr};
    Point3F groundNormal_{0.0f, 0.0f, 1.0f};
};

bool registerMineClass(Persistent::ClassRegistry& registry);
bool registerMineNetwork(sim::network::Registry& registry);

} // namespace opent1::game
