#pragma once

#include <opent1/core/bit_stream.hpp>
#include <opent1/core/stream_io.hpp>
#include <opent1/game/f_base.hpp>
#include <opent1/math/random.hpp>
#include <opent1/sim/sim_environment_render.hpp>
#include <opent1/sim/sim_net.hpp>
#include <opent1/sim/sim_resource_volume.hpp>

#include <array>
#include <span>
#include <string>
#include <vector>

namespace opent1::game {

class StarField : public sim::SimNetObject {
    using Parent = sim::SimNetObject;

public:
    enum Constants : Int32 {
        CurrentFileVersion = 2,
        REGION_COUNT = 36,
        MAX_STARS = 1500,
    };

    enum NetMaskBits : UInt32 {
        Modified = 1U << 0,
    };

    static inline constexpr Flt32 GLOBE_RADIUS = 3000.0f;

    struct Star {
        Point3F pos{};
        Flt32 brightness{0.0f};
    };

    struct Region {
        std::array<Point3F, 4> corners{};
        std::vector<Star> stars{};
    };

    StarField();
    ~StarField() override;

    static void initPersistFields();

    void setVisibility(bool vis);
    void toggleVisibility();
    void setBottomVisible(bool vis);

    [[nodiscard]] bool isVisible() const noexcept {
        return visible_;
    }

    [[nodiscard]] bool isBottomVisible() const noexcept {
        return bottomVisible_;
    }

    [[nodiscard]] bool isInFrontOfSky() const noexcept {
        return inFrontOfSky_;
    }

    void setInFrontOfSky(bool v) noexcept {
        inFrontOfSky_ = v;
        initialized_ = false;
        setMaskBits(Modified);
    }

    [[nodiscard]] const std::array<ColorF, 3>& colors() const noexcept {
        return colors_;
    }

    void setColor(std::size_t idx, const ColorF& color) noexcept {
        if (idx < colors_.size()) {
            colors_[idx] = color;
            setMaskBits(Modified);
        }
    }

    [[nodiscard]] const std::array<Region, REGION_COUNT + 1>& regions() const noexcept {
        return regions_;
    }

    [[nodiscard]] bool isInitialized() const noexcept {
        return initialized_;
    }

    void initialize();
    void setPalette();

    bool processArguments(std::span<const std::string> args) override;
    bool onAdd() override;
    void onRemove() override;
    bool processQuery(sim::SimQuery& query) override;

    bool inspectRead(sim::Inspect& inspector) override;
    bool inspectWrite(sim::Inspect& inspector) const override;

    [[nodiscard]] UInt16 getGhostTag() const override {
        return static_cast<UInt16>(StarFieldPersTag);
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
    bool initialized_{false};
    bool visible_{true};
    bool bottomVisible_{false};
    bool inFrontOfSky_{true};
    std::array<ColorF, 3> colors_{
        ColorF{1.0f, 1.0f, 1.0f},
        ColorF{0.5f, 0.5f, 0.5f},
        ColorF{0.25f, 0.25f, 0.25f},
    };
    std::array<UInt8, 3> paletteColors_{0, 0, 0};
    std::array<Region, REGION_COUNT + 1> regions_{};
};

} // namespace opent1::game
