#pragma once

#include <opent1/core/bit_stream.hpp>
#include <opent1/core/stream_io.hpp>
#include <opent1/game/f_base.hpp>
#include <opent1/sim/sim_environment_render.hpp>
#include <opent1/sim/sim_net.hpp>
#include <opent1/sim/sim_resource_volume.hpp>
#include <opent1/sim/sim_world_game.hpp>

#include <array>
#include <span>
#include <string>
#include <string_view>

namespace opent1::game {

class LensFlare {
public:
    enum Constants : Int32 {
        FLARE_COUNT = 11,
    };

    struct FlareInfo {
        Int32 index{0};
        Flt32 scale{0.0f};
        Flt32 minScale{0.0f};
        Flt32 dist{0.0f};
    };

    static inline constexpr std::array<FlareInfo, FLARE_COUNT> s_fInfo{{
        {0, 0.4f, 125.0f, 1.2f},
        {3, 0.1f, -1.0f, 1.0f},
        {4, 0.05f, -1.0f, 0.9f},
        {1, 0.22f, -1.0f, 0.65f},
        {4, 0.07f, -1.0f, 0.45f},
        {4, 0.05f, -1.0f, 0.2f},
        {5, 0.1f, -1.0f, 0.0f},
        {4, 0.025f, -1.0f, -0.35f},
        {2, 0.15f, -1.0f, -0.6f},
        {3, 0.18f, -1.0f, -0.8f},
        {5, 0.3f, -1.0f, -1.0f},
    }};

    static inline bool s_renderFlares = true;

    LensFlare() = default;
    ~LensFlare() = default;

    void initialize(sim::SimManager* manager) noexcept {
        manager_ = manager;
        initialized_ = true;
    }

    void setRootPos(const Point3F& pos) noexcept {
        rootPos_ = pos;
        renderRoot_ = true;
    }

    void resetRoot() noexcept {
        renderRoot_ = false;
    }

    [[nodiscard]] bool isInitialized() const noexcept {
        return initialized_;
    }

    [[nodiscard]] bool isRenderRoot() const noexcept {
        return renderRoot_;
    }

    [[nodiscard]] const Point3F& rootPos() const noexcept {
        return rootPos_;
    }

private:
    bool initialized_{false};
    bool renderRoot_{false};
    Point3F rootPos_{};
    sim::SimManager* manager_{nullptr};
};

class PlanetRenderImage {
public:
    std::string fileName{};
    Flt32 azimuth{0.0f};
    Flt32 incidence{30.0f};
    Flt32 size{2000.0f};
    Flt32 distance{11000.0f};
    bool castShadows{false};
    bool useLensFlare{false};
    ColorF intensity{0.0f, 0.0f, 0.0f};
    ColorF ambient{0.0f, 0.0f, 0.0f};
    Point3F position{0.0f, 0.0f, 0.0f};
    Point3F lightDirection{0.0f, 0.0f, 1.0f};

    PlanetRenderImage() = default;
    ~PlanetRenderImage() = default;

    void initialize(Flt32 az,
                    Flt32 inc,
                    Flt32 sz,
                    Flt32 dist,
                    bool lensFlare,
                    bool shadows,
                    const ColorF& intens,
                    const ColorF& amb);
    void setPosition(Flt32 az, Flt32 inc);
    void setTexture(std::string_view fname);
};

class Planet : public sim::SimNetObject {
    using Parent = sim::SimNetObject;

public:
    enum Constants : Int32 {
        CurrentFileVersion = 1,
    };

    enum NetMaskBits : UInt32 {
        Modified = 1U << 0,
    };

    static inline bool shadows = false;
    static inline Point3F shadowDirection{0.0f, 0.0f, 1.0f};

    Planet();
    ~Planet() override;

    static void initPersistFields();

    void set(std::string_view fileName,
             Flt32 azimuth,
             Flt32 incidence,
             bool castShadows = false,
             bool useLensFlare = false,
             ColorF intensity = {0.0f, 0.0f, 0.0f},
             ColorF ambient = {0.0f, 0.0f, 0.0f},
             Flt32 size = 2000.0f,
             Flt32 distance = 0.0f);

    void load();
    void unload();

    [[nodiscard]] const PlanetRenderImage& planetImage() const noexcept {
        return planet_;
    }

    [[nodiscard]] PlanetRenderImage& planetImage() noexcept {
        return planet_;
    }

    [[nodiscard]] const LensFlare& lensFlare() const noexcept {
        return lensFlare_;
    }

    [[nodiscard]] LensFlare& lensFlare() noexcept {
        return lensFlare_;
    }

    [[nodiscard]] const sim::SimLight& light() const noexcept {
        return light_;
    }

    bool processArguments(std::span<const std::string> args) override;
    bool onAdd() override;
    void onRemove() override;
    bool processEvent(const sim::SimEvent& event) override;
    bool processQuery(sim::SimQuery& query) override;

    bool inspectRead(sim::Inspect& inspector) override;
    bool inspectWrite(sim::Inspect& inspector) const override;

    [[nodiscard]] UInt16 getGhostTag() const override {
        return static_cast<UInt16>(PlanetPersTag);
    }

    UInt32
    packUpdate(sim::network::GhostManager& gm, UInt32 mask, BitStream& stream) const override;
    bool unpackUpdate(sim::network::GhostManager& gm, BitStream& stream) override;

    Int32 version() const noexcept override {
        return CurrentFileVersion;
    }

    Persistent::Error
    read(StreamIO& sio, Int32 version, Int32 user, Persistent::LoadContext& ctx) override;
    Persistent::Error
    write(StreamIO& sio, Int32 version, Int32 user, Persistent::StoreContext& ctx) const override;

private:
    bool inLightSet_{false};
    sim::SimLight light_{};
    PlanetRenderImage planet_{};
    LensFlare lensFlare_{};

    void streamWrite(StreamIO& sio) const;
    bool streamRead(StreamIO& sio);
};

} // namespace opent1::game
