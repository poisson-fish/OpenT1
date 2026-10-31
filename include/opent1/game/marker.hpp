#pragma once

#include <opent1/game/game_base.hpp>
#include <opent1/world/ts_shape_instance.hpp>

#include <memory>
#include <span>
#include <string>

namespace opent1::game {

class Marker : public GameBase {
    using Parent = GameBase;

public:
    using MarkerData = opent1::game::MarkerData;

    enum NetMaskBits : UInt32 {
        PositionMask = Parent::NextFreeMask << 0,
        RotationMask = Parent::NextFreeMask << 1,
        NextFreeMask = Parent::NextFreeMask << 2,
    };

    Marker();
    ~Marker() override;

    static void initPersistFields();

    [[nodiscard]] Int32 getDatGroup() const noexcept override {
        return DataBlockManager::MarkerDataType;
    }

    [[nodiscard]] const Point3F& getAngPosition() const noexcept {
        return angPosition_;
    }

    void setLinearPosition(const Point3F& pos);
    void setAngPosition(const Point3F& rot);

    [[nodiscard]] bool inEditMode() const noexcept {
        return inEditMode_;
    }

    [[nodiscard]] const Box3F& bbox() const noexcept {
        return bbox_;
    }

    void setTeam(Int32 team) override;
    bool initResources(GameBaseData* in_data) override;

    bool processArguments(std::span<const std::string> args) override;
    bool onAdd() override;
    void onRemove() override;
    bool processEvent(const sim::SimEvent& event) override;
    bool processQuery(sim::SimQuery& query) override;

    bool inspectRead(sim::Inspect& inspector) override;
    bool inspectWrite(sim::Inspect& inspector) const override;

    [[nodiscard]] UInt16 getGhostTag() const override {
        return static_cast<UInt16>(MarkerPersTag);
    }

    UInt32
    packUpdate(sim::network::GhostManager& gm, UInt32 mask, BitStream& stream) const override;
    bool unpackUpdate(sim::network::GhostManager& gm, BitStream& stream) override;

    Int32 version() const noexcept override {
        return 1;
    }

    Persistent::Error
    read(StreamIO& sio, Int32 version, Int32 user, Persistent::LoadContext& ctx) override;
    Persistent::Error
    write(StreamIO& sio, Int32 version, Int32 user, Persistent::StoreContext& ctx) const override;

private:
    bool onSimEditEvent(const sim::SimEditEvent& event);
    bool onSimObjectTransformEvent(const sim::SimObjectTransformEvent& event);
    bool onSimObjectTransformQuery(sim::SimObjectTransformQuery& query);
    bool onSimCollisionImageQuery(sim::SimCollisionImageQuery& query);

    Point3F angPosition_{0.0f, 0.0f, 0.0f};
    Box3F bbox_{Point3F{-0.25f, -0.25f, -0.25f}, Point3F{0.25f, 0.25f, 0.25f}};
    bool inEditMode_{false};
    FearRenderImage renderImage_{};
    std::shared_ptr<sim::SimCollisionBoxImage> collisionImage_{};
};

} // namespace opent1::game
