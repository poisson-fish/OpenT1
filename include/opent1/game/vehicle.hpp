#pragma once

#include <opent1/game/fear_player_psc.hpp>
#include <opent1/game/static_base.hpp>

#include <array>
#include <optional>
#include <span>

namespace opent1::game {

class Vehicle : public StaticBase {
    using Parent = StaticBase;

public:
    using VehicleData = opent1::game::VehicleData;

    enum SoundStates : Int32 {
        NO_SOUND = 0,
        IDLE_SOUND = 1,
        RUN_SOUND = 2,
    };

    enum VehicleMaskBits : UInt32 {
        OrientationMask = Parent::NextFreeMask << 0,
        StatusMask = Parent::NextFreeMask << 1,
        ShootingMask = Parent::NextFreeMask << 2,
        SoundMask = Parent::NextFreeMask << 3,
        NextFreeMask = Parent::NextFreeMask << 4,
    };

    Vehicle();
    ~Vehicle() override = default;

    [[nodiscard]] Int32 getDatGroup() const noexcept override {
        return DataBlockManager::VehicleDataType;
    }

    bool initResources(GameBaseData* in_data) override;
    bool onAdd() override;
    void onRemove() override;
    void onCollisionNotify(GameBase* object) override;
    bool processEvent(const sim::SimEvent& event) override;
    bool processQuery(sim::SimQuery& query) override;

    void serverProcess(UInt32 curTime) override;
    void clientProcess(UInt32 curTime) override;

    void serverUpdateMove(std::span<const PlayerMove> moves) override;
    virtual void updateMove(const PlayerMove* move, Flt32 interval);
    void ghostSetMove(const PlayerMove* move,
                      const Point3F& newPos,
                      const Point3F& newRot,
                      Flt32 newSpeed,
                      Flt32 newLift,
                      Int32 skipCount);

    void setPlayerControl(bool ctrl) override;

    [[nodiscard]] bool hasFocus() const noexcept {
        return hasFocus_;
    }

    // Mounting & riding
    [[nodiscard]] bool canMount(GameBase* obj) const;
    [[nodiscard]] bool canRide(GameBase* obj) const;
    [[nodiscard]] Int32 nextPassengerPoint();
    void changePassengerCount(Int32 change);

    [[nodiscard]] Int32 passengerCount() const noexcept {
        return passengerCount_;
    }

    [[nodiscard]] bool mountRender() const noexcept override;
    [[nodiscard]] Int32 getMountPose() const noexcept override;
    void getObjectMountTransform(Int32 mountPoint, TMat3F* transform) const override;
    bool getMuzzleTransform(Int32 slot, TMat3F* mat) const override;
    void getCameraTransform(Flt32 camDist, TMat3F* transform) const override;
    [[nodiscard]] const TMat3F& getEyeTransform() const override;
    bool getWeaponAmmoType(ProjectileDataType& outPrimary,
                           ProjectileDataType& outSecondary) const override;
    void shoot();

    // State & velocity accessors
    [[nodiscard]] const VehicleData* vehicleData() const noexcept {
        return data_;
    }

    [[nodiscard]] Flt32 speed() const noexcept {
        return speed_;
    }

    void setSpeed(Flt32 spd) noexcept;

    [[nodiscard]] Flt32 desiredSpeed() const noexcept {
        return desiredSpeed_;
    }

    void setDesiredSpeed(Flt32 dspd) noexcept;

    [[nodiscard]] const Point3F& desiredRot() const noexcept {
        return desiredRot_;
    }

    void setDesiredRot(const Point3F& r) noexcept {
        desiredRot_ = r;
    }

    [[nodiscard]] Flt32 lift() const noexcept {
        return lift_;
    }

    void setLift(Flt32 l) noexcept {
        lift_ = l;
    }

    [[nodiscard]] Int32 fireCount() const noexcept {
        return fireCount_;
    }

    void setFireCount(Int32 fc) noexcept {
        fireCount_ = fc & 0x3;
        setMaskBits(ShootingMask);
    }

    [[nodiscard]] Int32 soundState() const noexcept {
        return soundState_;
    }

    void setSoundState(Int32 ss) noexcept {
        soundState_ = ss & 0x3;
        setMaskBits(SoundMask);
    }

    [[nodiscard]] Int32 updateSkip() const noexcept {
        return updateSkip_;
    }

    void setUpdateSkip(Int32 skip) noexcept {
        updateSkip_ = skip;
    }

    [[nodiscard]] const PlayerMove& lastPlayerMove() const noexcept {
        return lastPlayerMove_;
    }

    void setLastPlayerMove(const PlayerMove& move) noexcept {
        lastPlayerMove_ = move;
    }

    void writePacketData(BitStream& bstream) const override;
    bool readPacketData(BitStream& bstream) override;

    UInt32 packUpdate(sim::network::GhostManager& g, UInt32 mask, BitStream& stream) const override;
    bool unpackUpdate(sim::network::GhostManager& g, BitStream& stream) override;

    Persistent::Error
    read(StreamIO& sio, Int32 version, Int32 user, Persistent::LoadContext& ctx) override;
    Persistent::Error
    write(StreamIO& sio, Int32 version, Int32 user, Persistent::StoreContext& ctx) const override;

    [[nodiscard]] std::string_view persistClassName() const noexcept {
        return "Vehicle";
    }

    [[nodiscard]] UInt32 persistTag() const noexcept {
        return VehiclePersTag;
    }

    [[nodiscard]] UInt16 getGhostTag() const override {
        return static_cast<UInt16>(VehiclePersTag);
    }

protected:
    using Parent::processBlocker;
    bool processBlocker(SimObject* obj);
    bool onSimObjectTransformEvent(const sim::SimObjectTransformEvent& event);
    bool onSimObjectTransformQuery(sim::SimObjectTransformQuery& query) const;

    VehicleData* data_{nullptr};
    Int32 passengerCount_{0};
    std::array<Int32, 8> passengerNode_{};

    Int32 updateDebt_{0};
    Int32 updateSkip_{0};
    UInt32 interpDoneTime_{0};
    Point3F deltaPos_{0.0f, 0.0f, 0.0f};
    Point3F deltaRot_{0.0f, 0.0f, 0.0f};
    Point3F desiredRot_{0.0f, 0.0f, 0.0f};
    Flt32 speed_{0.0f};
    Flt32 desiredSpeed_{0.0f};
    Flt32 lift_{0.0f};
    bool hasFocus_{false};
    bool trackGround_{false};
    bool inWorld_{true};

    Int32 soundState_{NO_SOUND};
    Int32 oldSoundState_{NO_SOUND};
    UInt32 currentSound_{0};

    Int32 pilotNode_{-1};
    Int32 exitNode_{-1};
    Int32 eyeNode_{-1};
    Int32 cameraNode_{-1};
    Int32 primaryMount_{-1};
    Int32 secondaryMount_{-1};
    bool flipBarrel_{false};

    Int32 fireCount_{0};
    Int32 ghostFireCount_{0};
    UInt32 nextFireTime_{0};

    std::optional<UInt32> animThread_{};
    PlayerMove lastPlayerMove_{};
    bool hasUnpackedInitialVehicle_{false};
    mutable TMat3F cachedEyeTransform_{};
};

bool registerVehicleClass(Persistent::ClassRegistry& registry);
bool registerVehicleNetwork(sim::network::Registry& registry);

} // namespace opent1::game
