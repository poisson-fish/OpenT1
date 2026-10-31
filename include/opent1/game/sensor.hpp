#pragma once

#include <opent1/game/static_base.hpp>

namespace opent1::game {

class SensorManager;

class Sensor : public StaticBase {
    using Parent = StaticBase;
    friend class SensorManager;

public:
    using SensorData = opent1::game::SensorData;

    enum SensorMaskBits : UInt32 {
        NextFreeMask = Parent::NextFreeMask,
    };

    Sensor(bool in_fCastLOS = true);
    ~Sensor() override;

    [[nodiscard]] Int32 getDatGroup() const noexcept override {
        return DataBlockManager::SensorDataType;
    }

    bool initResources(GameBaseData* io_pDat) override;
    bool onAdd() override;
    void onRemove() override;
    void setTeam(Int32 teamId) override;

    [[nodiscard]] bool isActive() const noexcept override {
        return Parent::isActive();
    }

    [[nodiscard]] bool isSupressionSensor() const noexcept {
        return sensorData_ ? sensorData_->supression : false;
    }

    [[nodiscard]] bool isSupressableSensor() const noexcept {
        return sensorData_ ? sensorData_->supressable : true;
    }

    [[nodiscard]] bool isPinger() const noexcept {
        return sensorData_ ? sensorData_->pinger : true;
    }

    [[nodiscard]] Int32 getRange() const noexcept {
        return sensorData_ ? sensorData_->iRange : 0;
    }

    [[nodiscard]] Int32 getDopplerVelocity() const noexcept {
        return sensorData_ ? sensorData_->iDopplerVelocity : 0;
    }

    [[nodiscard]] bool castsLOS() const noexcept {
        return fCastLOS_;
    }

    void setCastsLOS(bool castLos) noexcept {
        fCastLOS_ = castLos;
    }

    [[nodiscard]] const SensorData* sensorData() const noexcept {
        return sensorData_;
    }

    [[nodiscard]] Sensor* nextSensor() const noexcept {
        return nextSensor_;
    }

    [[nodiscard]] Sensor* prevSensor() const noexcept {
        return prevSensor_;
    }

    bool detectItem(GameBase* obj);

    UInt32
    packUpdate(sim::network::GhostManager& gm, UInt32 dwMask, BitStream& stream) const override;
    bool unpackUpdate(sim::network::GhostManager& gm, BitStream& stream) override;

    Persistent::Error
    read(StreamIO& sio, Int32 version, Int32 user, Persistent::LoadContext& ctx) override;
    Persistent::Error
    write(StreamIO& sio, Int32 version, Int32 user, Persistent::StoreContext& ctx) const override;

    [[nodiscard]] std::string_view persistClassName() const noexcept {
        return "Sensor";
    }

    [[nodiscard]] UInt32 persistTag() const noexcept {
        return SensorPersTag;
    }

    [[nodiscard]] UInt16 getGhostTag() const override {
        return static_cast<UInt16>(SensorPersTag);
    }

protected:
    bool checkLOS(const Point3F& pos);

    Sensor* nextSensor_{nullptr};
    Sensor* prevSensor_{nullptr};
    SensorData* sensorData_{nullptr};
    bool fCastLOS_{true};
    bool hasUnpackedInitialSensor_{false};
};

bool registerSensorClass(Persistent::ClassRegistry& registry);
bool registerSensorNetwork(sim::network::Registry& registry);

} // namespace opent1::game
