#pragma once

#include <opent1/core/bit_stream.hpp>
#include <opent1/core/stream_io.hpp>
#include <opent1/game/f_base.hpp>
#include <opent1/sim/sim_environment_render.hpp>
#include <opent1/sim/sim_net.hpp>
#include <opent1/sim/sim_resource_volume.hpp>

#include <array>
#include <span>
#include <string>

namespace opent1::game {

class Snowfall : public sim::SimNetObject {
    using Parent = sim::SimNetObject;

public:
    enum Constants : Int32 {
        MAXFLAKES = 4096,
        FlakeActive = 1,
        FlakeOutside = 2,
    };

    enum NetMaskBits : UInt32 {
        updateIntensity = 1U << 0,
        updateWind = 1U << 1,
        updateRendering = 1U << 2,
    };

    struct SnowFlake {
        Point3F pos{};
        Point3F vel{};
        Int32 size{0};
        Int32 timeout{0};
        Flt32 nextLookupTime{0.0f};
        Int32 flags{0};
    };

    static inline Flt32 precipPref = 1.0f;

    Snowfall();
    ~Snowfall() override;

    static void initPersistFields();

    [[nodiscard]] bool isRain() const noexcept {
        return rain_;
    }

    void setRain(bool r) noexcept {
        rain_ = r;
        setMaskBits(updateRendering);
    }

    [[nodiscard]] bool isRenderingSuspended() const noexcept {
        return suspendRendering_;
    }

    void setRendering(bool r);

    [[nodiscard]] const Point3F& getWind() const noexcept {
        return wind_;
    }

    void setWind(const Point3F& w);
    void setWind(const Point2F& w);

    [[nodiscard]] Flt32 getIntensity() const noexcept {
        return intensity_;
    }

    void setIntensity(Flt32 i);

    [[nodiscard]] Int32 numFlakes() const noexcept {
        return numFlakes_;
    }

    [[nodiscard]] const std::array<SnowFlake, MAXFLAKES>& flakes() const noexcept {
        return flakes_;
    }

    [[nodiscard]] UInt32 seed() const noexcept {
        return seed_;
    }

    void setSeed(UInt32 s) noexcept {
        seed_ = s;
    }

    void setCameraState(const Point3F& pos, const Point3F& vel, Flt32 nearDist = 1.0f) noexcept {
        const auto delta = pos - camPos_;
        // Spawn, teleport and mission changes must not leave the weather box
        // hundreds of metres behind the camera for several simulation ticks.
        const bool relocate = std::abs(delta.x) > SnowBox_.x || std::abs(delta.y) > SnowBox_.y ||
                              std::abs(delta.z) > SnowBox_.z;
        camPos_ = pos;
        camVel_ = vel;
        nearDist_ = nearDist;
        if (relocate) {
            snowInit();
        }
    }

    UInt32 fastRand() noexcept;
    void snowInit(Flt32 currentTime = 0.0f);
    void snowCreateFlake(Int32 index, bool startRandom, Flt32 currentTime = 0.0f);
    void snowLogicFrame(SimTime dt, Flt32 currentTime = 0.0f);

    bool processArguments(std::span<const std::string> args) override;
    bool onAdd() override;
    bool processEvent(const sim::SimEvent& event) override;
    bool processQuery(sim::SimQuery& query) override;

    bool inspectRead(sim::Inspect& inspector) override;
    bool inspectWrite(sim::Inspect& inspector) const override;

    [[nodiscard]] UInt16 getGhostTag() const override {
        return static_cast<UInt16>(SnowfallPersTag);
    }

    UInt32
    packUpdate(sim::network::GhostManager& gm, UInt32 mask, BitStream& stream) const override;
    bool unpackUpdate(sim::network::GhostManager& gm, BitStream& stream) override;

    Persistent::Error
    read(StreamIO& sio, Int32 version, Int32 user, Persistent::LoadContext& ctx) override;
    Persistent::Error
    write(StreamIO& sio, Int32 version, Int32 user, Persistent::StoreContext& ctx) const override;

private:
    Flt32 intensity_{1.0f};
    Point3F wind_{0.0f, -10.0f, -15.0f};
    bool rain_{true};
    bool suspendRendering_{false};

    Point3F SnowBox_{200.0f, 200.0f, 200.0f};
    Point3F SnowFocus_{25.0f, 25.0f, 25.0f};
    Point3F camPos_{0.0f, 0.0f, 0.0f};
    Point3F camVel_{0.0f, 0.0f, 0.0f};
    Flt32 nearDist_{1.0f};
    UInt32 seed_{1};
    Int32 numFlakes_{0};
    std::array<SnowFlake, MAXFLAKES> flakes_{};
};

} // namespace opent1::game
