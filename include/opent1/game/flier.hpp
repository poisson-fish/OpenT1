#pragma once

#include <opent1/game/vehicle.hpp>

#include <optional>

namespace opent1::game {

class Flier : public Vehicle {
    using Parent = Vehicle;

public:
    using FlierData = opent1::game::FlierData;

    static inline constexpr UInt32 csm_puffInterval = 50;

    void setMaxAlt(Flt32 maxAlt) noexcept {
        maxAltitude_ = maxAlt;
    }

    void setMinAlt(Flt32 minAlt) noexcept {
        minAltitude_ = minAlt;
    }

    [[nodiscard]] Flt32 maxAlt() const noexcept {
        return maxAltitude_;
    }

    [[nodiscard]] Flt32 minAlt() const noexcept {
        return minAltitude_;
    }

    Flier();
    ~Flier() override = default;

    [[nodiscard]] Int32 getDatGroup() const noexcept override {
        return DataBlockManager::FlierDataType;
    }

    bool initResources(GameBaseData* in_data) override;
    bool onAdd() override;

    void clientProcess(UInt32 curTime) override;
    void updateMove(const PlayerMove* move, Flt32 interval) override;
    [[nodiscard]] const Point3F& getLeadVelocity() const noexcept override;
    [[nodiscard]] Flt32 getTurnRate(const Point3F& in_rMissileVel) const noexcept;

    [[nodiscard]] const FlierData* flierData() const noexcept {
        return flierData_;
    }

    [[nodiscard]] Flt32 lateralThrust() const noexcept {
        return lateralThrust_;
    }

    void setLateralThrust(Flt32 lt) noexcept {
        lateralThrust_ = lt;
    }

    [[nodiscard]] bool isLanded() const noexcept {
        return landed_;
    }

    void setLanded(bool l) noexcept {
        landed_ = l;
    }

    void writePacketData(BitStream& bstream) const override;
    bool readPacketData(BitStream& bstream) override;

    Persistent::Error
    read(StreamIO& sio, Int32 version, Int32 user, Persistent::LoadContext& ctx) override;
    Persistent::Error
    write(StreamIO& sio, Int32 version, Int32 user, Persistent::StoreContext& ctx) const override;

    [[nodiscard]] std::string_view persistClassName() const noexcept {
        return "Flier";
    }

    [[nodiscard]] UInt32 persistTag() const noexcept {
        return FlierPersTag;
    }

    [[nodiscard]] UInt16 getGhostTag() const override {
        return static_cast<UInt16>(FlierPersTag);
    }

protected:
    Flt32 maxAltitude_ = 1000.0f;
    Flt32 minAltitude_ = -1000.0f;

    void sampleAltitude();
    void dynamics(Flt32 adjust);
    bool processCollision(MovementInfo& info) override;

    FlierData* flierData_{nullptr};
    Int32 flameNode_{-1};
    Int32 smokeNode_{-1};
    std::optional<UInt32> flameThread_{};
    UInt32 lastPuff_{0};
    UInt32 jetSound_{0};
    Flt32 thrust_{0.0f};
    Flt32 lateralThrust_{0.0f};
    bool landed_{false};
    bool jetting_{false};
    mutable Point3F cachedLeadVel_{0.0f, 0.0f, 0.0f};
};

bool registerFlierClass(Persistent::ClassRegistry& registry);
bool registerFlierNetwork(sim::network::Registry& registry);

} // namespace opent1::game
