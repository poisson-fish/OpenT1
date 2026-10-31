#pragma once

#include <opent1/game/static_base.hpp>

namespace opent1::game {

inline constexpr Flt32 ItemRotationSpeed = 3.0f;
inline constexpr Flt32 ItemUpdatePosTime = 1.0f;
inline constexpr Flt32 ItemThrowBlackout = 1.0f;

class Item : public StaticBase {
    using Parent = StaticBase;

public:
    using ItemData = opent1::game::ItemData;

    enum LightType : Int32 {
        NoLight = 0,
        ConstantLight = 1,
        PulsingLight = 2,
    };

    enum ItemFlags : UInt32 {
        Visible = 1u << 0,
        PopItem = 1u << 1,
    };

    enum ItemMaskBits : UInt32 {
        NextFreeMask = Parent::NextFreeMask,
    };

    Item();
    ~Item() override = default;

    [[nodiscard]] Int32 getDatGroup() const noexcept override {
        return DataBlockManager::ItemDataType;
    }

    bool processArguments(std::span<const std::string> args) override;
    bool initResources(GameBaseData* in_data) override;
    bool onAdd() override;
    void onRemove() override;
    void onDeleteNotify(sim::SimObject& object) override;
    void onCollisionNotify(GameBase* object) override;
    bool processQuery(sim::SimQuery& query) override;

    void serverProcess(UInt32 curTime) override;
    void clientProcess(UInt32 curTime) override;

    [[nodiscard]] const std::string& getClassName() const noexcept;
    [[nodiscard]] const std::string& getDescription() const noexcept;

    [[nodiscard]] bool isRotating() const noexcept {
        return isRotate_;
    }

    void setRotating(bool rotate) noexcept {
        isRotate_ = rotate;
    }

    [[nodiscard]] bool isCollideable() const noexcept {
        return isCollideable_;
    }

    void setCollideable(bool collideable) noexcept {
        isCollideable_ = collideable;
    }

    [[nodiscard]] bool isHidden() const noexcept {
        return (flags_ & Visible) == 0;
    }

    void hideItem(bool hide);

    [[nodiscard]] bool isPop() const noexcept {
        return (flags_ & PopItem) != 0;
    }

    void pop() noexcept {
        flags_ |= PopItem;
    }

    void setPop(bool pop) noexcept {
        if (pop) {
            flags_ |= PopItem;
        } else {
            flags_ &= ~PopItem;
        }
    }

    [[nodiscard]] UInt32 itemFlags() const noexcept {
        return flags_;
    }

    [[nodiscard]] Int32 getCount() const noexcept {
        return count_;
    }

    void setCount(Int32 count) noexcept {
        count_ = count;
    }

    [[nodiscard]] sim::SimObject* throwSource() const noexcept {
        return throwSource_;
    }

    void setThrowSource(sim::SimObject* source, Int32 explicitGhostIndex = -1) noexcept {
        throwSource_ = source;
        explicitThrowGhostIndex_ = explicitGhostIndex;
    }

    [[nodiscard]] Int32
    throwGhostIndex(const sim::network::GhostManager* gm = nullptr) const noexcept;

    void setExplicitThrowGhostIndex(Int32 idx) noexcept {
        explicitThrowGhostIndex_ = idx;
    }

    [[nodiscard]] Int32 throwerClientId() const noexcept {
        return throwerClientId_;
    }

    [[nodiscard]] UInt32 throwerClientLoginId() const noexcept {
        return throwerClientLoginId_;
    }

    [[nodiscard]] SimTime updateTime() const noexcept {
        return updateTime_;
    }

    [[nodiscard]] SimTime popTime() const noexcept {
        return popTime_;
    }

    void setPopTime(SimTime pt) noexcept {
        popTime_ = pt;
    }

    [[nodiscard]] const ItemData* itemData() const noexcept {
        return itemData_;
    }

    void throwObject(GameBase* object, Flt32 speed, bool careless) override;

    UInt32
    packUpdate(sim::network::GhostManager& gm, UInt32 mask, BitStream& stream) const override;
    bool unpackUpdate(sim::network::GhostManager& gm, BitStream& stream) override;

    Persistent::Error
    read(StreamIO& sio, Int32 version, Int32 user, Persistent::LoadContext& ctx) override;
    Persistent::Error
    write(StreamIO& sio, Int32 version, Int32 user, Persistent::StoreContext& ctx) const override;

    [[nodiscard]] std::string_view persistClassName() const noexcept {
        return "Item";
    }

    [[nodiscard]] UInt32 persistTag() const noexcept {
        return ItemPersTag;
    }

    [[nodiscard]] UInt16 getGhostTag() const override {
        return static_cast<UInt16>(ItemPersTag);
    }

protected:
    bool processCollision(MovementInfo& info) override;
    bool onSimLightQuery(sim::SimLightQuery& query) const;

    ItemData* itemData_{nullptr};
    UInt32 flags_{Visible};
    bool isRotate_{false};
    bool isCollideable_{false};
    Int32 count_{0};
    SimTime updateTime_{0.0f};
    SimTime popTime_{0.0f};
    sim::SimObject* throwSource_{nullptr};
    Int32 explicitThrowGhostIndex_{-1};
    Int32 throwerClientId_{-1};
    UInt32 throwerClientLoginId_{0};
    mutable sim::SimLight light_{};
};

bool registerItemClass(Persistent::ClassRegistry& registry);
bool registerItemNetwork(sim::network::Registry& registry);

} // namespace opent1::game
