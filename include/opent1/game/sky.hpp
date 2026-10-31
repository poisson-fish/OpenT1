#pragma once

#include <opent1/core/bit_stream.hpp>
#include <opent1/core/stream_io.hpp>
#include <opent1/game/f_base.hpp>
#include <opent1/sim/sim_collision_movement.hpp>
#include <opent1/sim/sim_environment_render.hpp>
#include <opent1/sim/sim_resource_volume.hpp>
#include <opent1/world/ts_shape.hpp>

#include <array>
#include <string>
#include <string_view>

namespace opent1::game {

class Sky : public sim::SimContainer {
    using Parent = sim::SimContainer;

public:
    enum Constants : Int32 {
        CurrentFileVersion = 2,
        TEXTURES = 16,
    };

    enum NetMaskBits : UInt32 {
        Modified = 1U << 0,
    };

    Sky();
    explicit Sky(std::string_view in_dmlName);
    ~Sky() override;

    static void initPersistFields();

    [[nodiscard]] const ColorF& getHazeColor() const noexcept {
        return hazeColor_;
    }

    [[nodiscard]] const ColorF& getSkyColor() const noexcept {
        return skyColor_;
    }

    void setSkyColor(const ColorF& color) noexcept {
        skyColor_ = color;
        setMaskBits(Modified);
    }

    void setHazeColor(const ColorF& color) noexcept {
        hazeColor_ = color;
        setMaskBits(Modified);
    }

    [[nodiscard]] std::string_view dmlName() const noexcept {
        return dmlName_;
    }

    [[nodiscard]] const world::TsMaterialResources* materials() const;
    void setDmlName(std::string_view name);

    [[nodiscard]] Flt32 size() const noexcept {
        return size_;
    }

    void setSize(Flt32 s) noexcept {
        size_ = s;
        initialized_ = false;
        setMaskBits(Modified);
    }

    [[nodiscard]] Flt32 featurePosition() const noexcept {
        return featurePosition_;
    }

    void setFeaturePosition(Flt32 pos) noexcept {
        featurePosition_ = pos;
        initialized_ = false;
        setMaskBits(Modified);
    }

    [[nodiscard]] Flt32 distance() const noexcept {
        return distance_;
    }

    [[nodiscard]] Flt32 getDistance() const noexcept {
        return (2.0f * size_ > distance_) ? (2.0f * size_) : distance_;
    }

    void setDistance(Flt32 dist) noexcept {
        distance_ = dist;
        initialized_ = false;
    }

    [[nodiscard]] Int32 matListTag() const noexcept {
        return matListTag_;
    }

    void setMatListTag(Int32 tag) noexcept {
        matListTag_ = tag;
    }

    [[nodiscard]] const std::array<Int32, TEXTURES>& textures() const noexcept {
        return textures_;
    }

    void setTexture(std::size_t idx, Int32 val) noexcept {
        if (idx < TEXTURES) {
            textures_[idx] = val & 0x0F;
            setMaskBits(Modified);
        }
    }

    [[nodiscard]] const std::array<Point3F, TEXTURES>& points() const noexcept {
        return points_;
    }

    [[nodiscard]] bool isInitialized() const noexcept {
        return initialized_;
    }

    void initialize();
    void load();
    void setPalette();

    bool processArguments(std::span<const std::string> args) override;
    bool onAdd() override;
    void onRemove() override;
    bool processQuery(sim::SimQuery& query) override;

    bool inspectRead(sim::Inspect& inspector) override;
    bool inspectWrite(sim::Inspect& inspector) const override;

    [[nodiscard]] UInt16 getGhostTag() const override {
        return static_cast<UInt16>(TribesSkyPersTag);
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
    Int32 skyIndex_{0};
    Int32 hazeIndex_{0};
    Flt32 size_{600.0f};
    Flt32 distance_{13000.0f};
    std::array<Int32, TEXTURES> textures_{0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1};
    std::array<Point3F, TEXTURES> points_{};
    Flt32 featurePosition_{0.0f};
    Int32 matListTag_{0};
    ColorF skyColor_{0.0f, 0.0f, 0.0f};
    ColorF hazeColor_{0.0f, 0.0f, 0.0f};
    std::string dmlName_{};
    mutable std::optional<world::TsMaterialResources> materials_;
};

} // namespace opent1::game
