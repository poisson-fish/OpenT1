#pragma once
#include <opent1/game/shape_base.hpp>
#include <opent1/game/shape_collision.hpp>
#include <memory>
#include <string_view>

namespace opent1::game {

class StaticBase : public ShapeBase {
    using Parent = ShapeBase;

public:
    enum DamageState : Int32 {
        None = 0,
        Enabled = 1,
        Disabled = 2,
        Destroyed = 3,
    };

    enum Constants : Int32 {
        DamageLevelBits = 6,
    };

    enum AnimThread : Int32 {
        PowerThread = 0,
        AmbientThread = 1,
        DamageThread = 2,
    };

    StaticBase();
    ~StaticBase() override;

    bool onAdd() override;
    void onRemove() override;
    bool initResources(GameBaseData* dat) override;
    bool processEvent(const sim::SimEvent& event) override;
    bool processQuery(sim::SimQuery& query) override;

    [[nodiscard]] std::string_view getScriptDefaultClass() const noexcept override {
        return "StaticBase";
    }

    void serverProcess(UInt32 curTime) override;
    void clientProcess(UInt32 curTime) override;

    // State & activation
    void setState(Int32 ns, bool force = false);
    bool updateState();

    [[nodiscard]] Int32 state() const noexcept {
        return state_;
    }

    [[nodiscard]] Int32 getState() const noexcept {
        return state_;
    }

    [[nodiscard]] Int32 getDamageSkinLevel(Int32 detailLevel) const noexcept override;
    [[nodiscard]] std::string_view getDamageState() const noexcept override;
    bool getCommandStatus(CommandStatus* status) const override;

    [[nodiscard]] bool isActive() const noexcept override {
        return active_;
    }

    bool setActive(bool active) override;
    virtual void onActivate();
    virtual void onDeactivate();
    void onPowerStateChange(GameBase* generator) override;

    void setPos(const Point3F& pos);
    void setRot(const Point3F& rot);
    void setDamageLevel(Flt32 level) override;

    // Targetable set & score
    void setIsTarget(bool isTarget);

    [[nodiscard]] bool isTarget() const noexcept {
        return isTarget_;
    }

    bool getTarget(Point3F* outTarget, Int32* outTeam) const override;

    [[nodiscard]] Int32 scoreValue() const noexcept {
        return scoreValue_;
    }

    void setScoreValue(Int32 score) noexcept {
        scoreValue_ = score;
        setMaskBits(InfoMask);
    }

    // Collision
    void initCollisionInfo();
    void onCollisionNotify(GameBase* object) override;
    void disableCollision();
    void enableCollision();

    [[nodiscard]] bool isCollisionDisabled() const noexcept {
        return disableCollisionCount_ > 0;
    }

    [[nodiscard]] const std::shared_ptr<ShapeCollisionImage>& collisionImage() const noexcept {
        return collisionImage_;
    }

    [[nodiscard]] const std::shared_ptr<sim::SimCollisionSphereImage>&
    outerCollisionImage() const noexcept {
        return outerCollisionImage_;
    }

    // Network ghosting & persistence
    UInt32
    packUpdate(sim::network::GhostManager& gm, UInt32 mask, BitStream& stream) const override;
    bool unpackUpdate(sim::network::GhostManager& gm, BitStream& stream) override;
    bool packStaticBaseUpdate(UInt32 mask, BitStream& stream, UInt32 currentTime = 0) const;
    bool unpackStaticBaseUpdate(BitStream& stream, UInt32 currentTime = 0);

    Persistent::Error
    read(StreamIO& sio, Int32 version, Int32 user, Persistent::LoadContext& ctx) override;
    Persistent::Error
    write(StreamIO& sio, Int32 version, Int32 user, Persistent::StoreContext& ctx) const override;

protected:
    virtual void onDamageStateChange(Int32 oldState);
    void updateAmbientSound();
    void stopAmbientSound();
    std::shared_ptr<sim::SimCollisionImage> getSelfImage() override;
    bool onSimCollisionImageQuery(sim::SimCollisionImageQuery& query) const;
    bool onSimEditEvent(const sim::SimEditEvent& event);

    StaticBaseData* staticData_{nullptr};
    Int32 state_{None};
    bool active_{false};
    bool deleteOnDestroy_{false};
    bool isTarget_{false};
    bool hasUnpackedInitial_{false};
    Int32 scoreValue_{0};
    Int32 disableCollisionCount_{0};
    UInt32 ambientSound_{0};
    std::optional<UInt32> ambientThreadIndex_{};
    std::optional<UInt32> damageThreadIndex_{};
    std::shared_ptr<ShapeCollisionImage> collisionImage_;
    std::shared_ptr<sim::SimCollisionSphereImage> outerCollisionImage_;
};

class StaticShape final : public StaticBase {
    using Parent = StaticBase;

public:
    StaticShape();
    ~StaticShape() override = default;

    [[nodiscard]] Int32 getDatGroup() const noexcept override {
        return DataBlockManager::StaticShapeDataType;
    }

    [[nodiscard]] UInt16 getGhostTag() const noexcept override {
        return static_cast<UInt16>(StaticShapePersTag);
    }

    [[nodiscard]] std::string_view getScriptDefaultClass() const noexcept override {
        return "StaticShape";
    }

    UInt32
    packUpdate(sim::network::GhostManager& gm, UInt32 mask, BitStream& stream) const override;
    bool unpackUpdate(sim::network::GhostManager& gm, BitStream& stream) override;
    bool packStaticShapeUpdate(UInt32 mask, BitStream& stream, UInt32 currentTime = 0) const;
    bool unpackStaticShapeUpdate(BitStream& stream, UInt32 currentTime = 0);

    Persistent::Error
    read(StreamIO& sio, Int32 version, Int32 user, Persistent::LoadContext& ctx) override;
    Persistent::Error
    write(StreamIO& sio, Int32 version, Int32 user, Persistent::StoreContext& ctx) const override;
};

} // namespace opent1::game
