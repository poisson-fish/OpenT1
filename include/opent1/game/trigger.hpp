#pragma once

#include <opent1/game/game_base.hpp>

#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace opent1::game {

class Trigger : public GameBase {
    using Parent = GameBase;

public:
    using TriggerData = opent1::game::TriggerData;

    enum NetMaskBits : UInt32 {
        PositionMask = 1U << 3,
        RotationMask = 1U << 4,
        SizedMask = 1U << 7,
    };

    static inline constexpr Flt32 TIMEOUT = 0.25f;
    static inline constexpr Flt32 RATE = 0.25f;

    struct Pair {
        sim::SimObject* object{nullptr};
        sim::SimObjectId objectId{0};
        SimTime expire{0.0f};
        SimTime contact{0.0f};
    };

    using TriggerList = std::vector<Pair>;

    struct EventRecord {
        std::string callback{};
        std::string thisArg{};
        std::string objArg{};
    };

    Trigger();
    ~Trigger() override;

    static void initPersistFields();

    [[nodiscard]] Int32 getDatGroup() const noexcept override {
        return DataBlockManager::TriggerDataType;
    }

    [[nodiscard]] bool isActive() const noexcept override {
        return active_;
    }

    bool setActive(bool b) override;

    [[nodiscard]] const Point3F& getAngPosition() const noexcept {
        return angPosition_;
    }

    void setLinearPosition(const Point3F& pos);
    void setAngPosition(const Point3F& rot);

    [[nodiscard]] const Box3F& localBox() const noexcept {
        return boundingBox_;
    }

    void setDimensions(const Point3F& dim);

    [[nodiscard]] bool isSphere() const noexcept {
        return isSphere_;
    }

    void setSphere(bool value) {
        isSphere_ = value;
        setFieldValue("isSphere", value ? "true" : "false");
    }

    [[nodiscard]] const TriggerList& objects() const noexcept {
        return objectList_;
    }

    [[nodiscard]] const std::vector<EventRecord>& eventHistory() const noexcept {
        return eventHistory_;
    }

    void clearEventHistory() noexcept {
        eventHistory_.clear();
    }

    void setCurrentSimTime(SimTime t) noexcept {
        explicitSimTime_ = t;
    }

    [[nodiscard]] SimTime currentSimTime() const noexcept;

    bool initResources(GameBaseData* in_data) override;
    void onCollisionNotify(GameBase* obj) override;
    void handleCollision(sim::SimObject* obj, SimTime now);
    void expireContacts(SimTime now);

    bool processArguments(std::span<const std::string> args) override;
    bool onAdd() override;
    void onRemove() override;
    void onDeleteNotify(sim::SimObject& object) override;
    void serverProcess(UInt32 curTime) override;
    bool processEvent(const sim::SimEvent& event) override;
    bool processQuery(sim::SimQuery& query) override;

    bool inspectRead(sim::Inspect& inspector) override;
    bool inspectWrite(sim::Inspect& inspector) const override;

    [[nodiscard]] UInt16 getGhostTag() const override {
        return static_cast<UInt16>(TriggerPersTag);
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
    bool onSimTriggerEvent(const sim::SimTriggerEvent& event);
    bool onSimEditEvent(const sim::SimEditEvent& event);
    bool onSimObjectTransformEvent(const sim::SimObjectTransformEvent& event);
    bool onSimMovementCollisionEvent(const sim::SimMovementCollisionEvent& event);
    bool onSimObjectTransformQuery(sim::SimObjectTransformQuery& query);
    bool onSimCollisionImageQuery(sim::SimCollisionImageQuery& query);

    void updateBoundingBox();
    void scriptExec(std::string_view fn, std::string_view arg = {});

    bool isSphere_{true};
    std::shared_ptr<sim::SimCollisionSphereImage> sphereCollisionImage_;
    bool active_{true};
    bool inEditMode_{false};
    Point3F angPosition_{0.0f, 0.0f, 0.0f};
    Box3F boundingBox_{Point3F{-1.0f, -1.0f, -1.0f}, Point3F{1.0f, 1.0f, 1.0f}};
    TriggerData* triggerData_{nullptr};
    TriggerList objectList_{};
    std::vector<EventRecord> eventHistory_{};
    SimTime explicitSimTime_{-1.0f};
    std::shared_ptr<sim::SimCollisionBoxImage> collisionImage_{};
};

} // namespace opent1::game
