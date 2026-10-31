#pragma once
#include <opent1/world/world_scene.hpp>
#include <opent1/world/world_collision_sim.hpp>

namespace opent1::world {
// Native owned field payloads. Parent GameBase fields belong to Layer 7;
// these codecs can be used at that boundary without a host struct ABI.
struct InteriorMissionState {
    struct Light {
        Flt32 time{};
        bool active{};
        // Preserve the native record's three padding bytes and reserved words.
        std::array<UInt8, 3> padding{};
        UInt32 dummy1{}, dummy2{};
        bool operator==(const Light&) const = default;
    };

    std::string filename;
    Int32 container{-1};
    Point3F position{}, rotation{};
    std::vector<Light> lights;
    std::vector<UInt32> checks;
    bool disableCollisions{}, editMode{};
    bool operator==(const InteriorMissionState&) const = default;
    [[nodiscard]] bool valid() const;
    [[nodiscard]] bool open(const Box3F&) const;
    bool pack(BitStream&, UInt32 mask) const;
    bool unpack(BitStream&);     // commits only after the entire record validates
    bool write(StreamIO&) const; // InteriorShape version 2, after parent
    bool read(StreamIO&);
};

struct TerrainMissionPreferences {
    Flt32 visibleDistance{10000}, perspectiveScale{1}, textureDetail{1};
};

struct TerrainMissionPolicy {
    Flt32 visibleDistance{}, hazeDistance{}, perspectiveDistance{}, screenSize{},
        containerFarDistance{};
    Int32 textureDetail{};
    [[nodiscard]] bool renderChild(const sim::EnvironmentCamera&, const Box3F&) const;
    [[nodiscard]] static bool scopeChild(Point3F camera, Flt32 visibleDistance, const Box3F&);
    [[nodiscard]] Flt32 haze(Point3F camera, Point3F position) const;
};

// The mission terrain owns visibility even while the camera is inside a place
// or interior. The fallback matches the camera's standalone world default.
[[nodiscard]] TerrainMissionPolicy sceneVisibility(const sim::SimManager&);

struct TerrainMissionState {
    enum Mask : UInt32 {
        File = 1,
        Visibility = 2,
        Detail = 4,
        Context = 8,
        Movement = 16,
        Suspend = 32,
        Crc = 64
    };

    std::string filename;
    Point3F position{}, rotation{}, gravity{0, 0, -9.8f};
    Flt32 visibleDistance{3000}, hazeDistance{2000}, hazeMin{1000}, hazeMax{2000};
    Flt32 perspectiveDistance{500}, screenSize{120}, drag{}, height{10000};
    UInt32 crc{0xffffffff};
    bool suspend{};
    bool operator==(const TerrainMissionState&) const = default;
    [[nodiscard]] bool valid() const;
    bool pack(BitStream&, UInt32 mask) const;
    bool unpack(BitStream&);
    bool write(StreamIO&, Int32 version = 3) const;
    bool read(StreamIO&, Int32 version);
    [[nodiscard]] std::optional<TerrainMissionPolicy> policy(const TerrainMissionPreferences&,
                                                             Int32 gridDetails) const;
};

// Mission/ghost providers use the existing render, light playback, spatial and
// retained-image paths. Resource replacement is prepared before publication.
class SimMissionInterior : public SimInteriorSurface {
public:
    SimMissionInterior();

    [[nodiscard]] const InteriorMissionState& state() const {
        return state_;
    }

    bool setState(const InteriorMissionState&);
    bool advanceClientTime(UInt32 milliseconds); // native 67 ms clientProcess pacing

    [[nodiscard]] UInt32 clientTime() const {
        return lastUpdated_;
    }

    bool onAdd() override;
    bool processQuery(sim::SimQuery&) override;

    UInt16 getGhostTag() const override {
        return 522;
    }

    Int32 version() const override {
        return 2;
    }

    UInt32 packUpdate(sim::network::GhostManager&, UInt32, BitStream&) const override;
    bool unpackUpdate(sim::network::GhostManager&, BitStream&) override;
    Persistent::Error read(StreamIO&, Int32, Int32, Persistent::LoadContext&) override;
    Persistent::Error write(StreamIO&, Int32, Int32, Persistent::StoreContext&) const override;

private:
    bool apply(const InteriorMissionState&, ResourceManager&);
    InteriorMissionState state_;
    std::shared_ptr<const SimInteriorCollisionImage> collision_;
    bool applying_{};
    UInt32 lastUpdated_{};
};

class SimMissionTerrain : public sim::SimContainer {
public:
    SimMissionTerrain();

    [[nodiscard]] const TerrainMissionState& state() const {
        return state_;
    }

    [[nodiscard]] const TerrainMissionPolicy& policy() const {
        return policy_;
    }

    [[nodiscard]] std::optional<UInt16> sampleLighting(Point3F position) const {
        return surface_ ? surface_->sampleLighting(position) : std::nullopt;
    }

    [[nodiscard]] Point2F heightRange() const {
        return heightRange_;
    }

    [[nodiscard]] std::optional<TerrainGridResources> editableResources() const;
    bool publishEditedResources(TerrainGridResources);
    bool setPreferences(const TerrainMissionPreferences&);
    bool setTerrain2(bool enabled);

    [[nodiscard]] bool renderChild(const sim::EnvironmentCamera& c, const Box3F& b) const {
        return policy_.renderChild(c, b);
    }

    [[nodiscard]] bool scopeChild(Point3F c, Flt32 d, const Box3F& b) const {
        return policy_.scopeChild(c, d, b);
    }

    bool setState(const TerrainMissionState&);
    bool onAdd() override;
    bool processEvent(const sim::SimEvent&) override;
    bool processQuery(sim::SimQuery&) override;

    UInt16 getGhostTag() const override {
        return 2;
    }

    Int32 version() const override {
        return 3;
    }

    UInt32 packUpdate(sim::network::GhostManager&, UInt32, BitStream&) const override;
    bool unpackUpdate(sim::network::GhostManager&, BitStream&) override;
    Persistent::Error read(StreamIO&, Int32, Int32, Persistent::LoadContext&) override;
    Persistent::Error write(StreamIO&, Int32, Int32, Persistent::StoreContext&) const override;

protected:
    void enterContainer(sim::SimContainer&) override;
    void leaveContainer(sim::SimContainer&) override;

private:
    bool apply(const TerrainMissionState&, ResourceManager&, const TerrainMissionPreferences&);
    TerrainMissionState state_;
    TerrainMissionPreferences preferences_;
    TerrainMissionPolicy policy_;
    Point2F heightRange_{};
    std::optional<TerrainGridResources> editedResources_;
    std::unique_ptr<SimTerrainSurface> surface_;
    bool terrain2Enabled_{};
    std::shared_ptr<const SimTerrainCollisionImage> collision_;
    bool applying_{};
};

bool registerWorldGhosts(sim::network::Registry&);
bool registerWorldMissionClasses(Persistent::ClassRegistry&);
} // namespace opent1::world
