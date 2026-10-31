#pragma once
#include <opent1/world/world_mission.hpp>
#include <opent1/sim/sim_editor_events.hpp>

namespace opent1::world {
struct LinearMotion {
    Point3F position{}, velocity{};
    bool update(float);
};

struct AcceleratedMotion : LinearMotion {
    Point3F target{}, acceleration{};
    bool update(float);
};

inline constexpr sim::SimObjectId PathManagerId = 42;

struct PathWaypoint {
    UInt32 persistTag{};
    Point3F position{}, rotation{}, linear{}, angular{};
    bool operator==(const PathWaypoint&) const = default;
};

struct PathState {
    bool looping{}, compressed{true};
    Int32 id{-1};
    std::vector<PathWaypoint> waypoints;
    bool valid() const;
    bool write(StreamIO&) const;
    bool read(StreamIO&);
};

struct PathSample {
    Point3F position, rotation, linear, angular;
    Int32 index{}, count{};
};

struct PathQuery : sim::SimQuery {
    PathQuery() {
        type = sim::EventTypeFirst + 12;
    }

    Int32 id{-1}, count{};
};

struct WaypointQuery : sim::SimQuery {
    WaypointQuery() {
        type = sim::EventTypeFirst + 11;
    }

    Int32 index{};
    std::optional<PathSample> sample;
};

[[nodiscard]] std::optional<PathSample> samplePath(const PathState&, Int32);

struct PathEvent : sim::SimEvent {
    PathEvent() {
        type = sim::EventTypeFirst + 37;
        flags = Guaranteed | Ordered;
        address.objectId = PathManagerId;
    }

    Int32 id{-1};
    std::optional<PathState> path;
    bool pack(BitStream&) const;
    bool unpack(BitStream&);
};

class SimPathManager : public sim::SimObject {
public:
    SimPathManager() {
        setId(PathManagerId);
    }

    Int32 add(PathState);
    bool replace(Int32, std::optional<PathState>);
    [[nodiscard]] std::shared_ptr<const PathState> path(Int32) const;
    [[nodiscard]] std::optional<PathSample> waypoint(Int32 path, Int32 index);
    bool processEvent(const sim::SimEvent&) override;
    [[nodiscard]] std::vector<PathEvent> fullSync() const;
    bool sync(sim::network::PacketStream&) const;
    // Native fanout eligibility: only streams in normal ghost mode receive
    // replacements/deletions. Full sync is explicit during client attachment.
    void useNetworkBroadcast();
    void retire(Int32);

    [[nodiscard]] bool deliveryHealthy() const {
        return deliveryHealthy_;
    }

    // Prepare an owned event before publishing. A rejected host delivery keeps
    // the service's old snapshot and all outstanding readers unchanged.
    std::function<bool(const PathEvent&)> broadcast;

    void reset() {
        paths_.clear();
    }

private:
    std::vector<std::shared_ptr<const PathState>> paths_;
    bool receiving_{}, deliveryHealthy_{true};
};

class SimPath : public sim::SimGroup {
public:
    const PathState& state() const {
        return state_;
    }

    bool setState(PathState);
    bool onAdd() override;
    void onRemove() override;
    bool publish();
    [[nodiscard]] std::optional<PathState> snapshot() const;
    bool compress();
    bool expand();
    bool processEvent(const sim::SimEvent&) override;
    bool processQuery(sim::SimQuery&) override;
    std::function<std::unique_ptr<sim::SimObject>(UInt32)> createWaypoint;
    std::function<UInt32(const sim::SimObject&)> waypointTag;
    Persistent::Error read(StreamIO&, Int32, Int32, Persistent::LoadContext&) override;
    Persistent::Error write(StreamIO&, Int32, Int32, Persistent::StoreContext&) const override;

private:
    PathState state_;
    sim::SimAddress service_;
    bool editing_{};
};

struct LightState {
    enum Mask : UInt32 {
        Type = 1,
        Range = 2,
        Intensity = 4,
        Ambient = 8,
        Position = 16,
        Direction = 32,
        Static = 64
    };

    Int32 type{2};
    float range{1};
    ColorF intensity{}, ambient{};
    Point3F position{}, direction{0, 1, 0};
    bool isStatic{};
    bool valid() const;
    sim::SimLight light() const;
    bool write(StreamIO&) const;
    bool read(StreamIO&, Int32 version = 2);
    bool pack(BitStream&, UInt32 mask) const;
    bool unpack(BitStream&);
};

class SimNativeLight : public sim::SimContainer {
public:
    SimNativeLight();
    bool onAdd() override;
    bool processQuery(sim::SimQuery&) override;

    const LightState& state() const {
        return state_;
    }

    bool setState(const LightState&);
    bool setMarker(TsShapeResources);
    bool edit(bool);
    bool processEvent(const sim::SimEvent&) override;

    UInt16 getGhostTag() const override {
        return 6;
    }

    Int32 version() const override {
        return 2;
    }

    UInt32 packUpdate(sim::network::GhostManager&, UInt32, BitStream&) const override;
    bool unpackUpdate(sim::network::GhostManager&, BitStream&) override;
    Persistent::Error read(StreamIO&, Int32, Int32, Persistent::LoadContext&) override;
    Persistent::Error write(StreamIO&, Int32, Int32, Persistent::StoreContext&) const override;

private:
    LightState state_;
    std::unique_ptr<SimTsShape> marker_;
    bool editing_{};
};

class SimPlace : public sim::SimContainer {
public:
    SimPlace();
    bool onAdd() override;
    float farDistance() const;
    float haze(Point3F camera, Point3F position) const;
    bool renderChild(const sim::EnvironmentCamera&, const Box3F&) const;

protected:
    void enterContainer(sim::SimContainer&) override;
    void leaveContainer(sim::SimContainer&) override;

private:
    SimMissionTerrain* terrain() const;
};

struct BoxFace {
    std::array<Point3F, 4> points;
    UInt8 color{};
};

[[nodiscard]] std::array<BoxFace, 6> boxFaces(const Box3F&);
[[nodiscard]] std::shared_ptr<const WorldSurfaceMesh> boxSurface(const Box3F&);
[[nodiscard]] std::optional<TMat3F>
faceDirection(Point3F direction, std::optional<Point3F> axis = {}, std::optional<float> spin = {});
[[nodiscard]] float effectSortValue(bool translucent, float cameraDistance, float nearDistance);

// Own both authored shape/animation instances. Switching representation copies
// thread position before publication and old WorldShapeQuery output owns assets.
class SimEffectShape : public sim::SimObject {
public:
    bool resources(TsShapeResources opaque, TsShapeResources translucent);
    bool translucent(bool);
    bool advance(double);
    bool faceCamera(Point3F camera,
                    bool away = false,
                    std::optional<Point3F> axis = {},
                    std::optional<float> spin = {});
    TMat3F transform;
    bool onAdd() override;
    bool processQuery(sim::SimQuery&) override;

private:
    TsShapeResources opaque_, translucent_;
    std::optional<TsShapeInstance> opaqueInstance_, translucentInstance_;
    bool translucentMode_{};
};

bool registerSceneObjectClasses(Persistent::ClassRegistry&);
bool registerSceneObjectNetwork(sim::network::Registry&);
// Resource hosts install native constructor names with typed, fully prepared
// assets. Leases remove only this installation and may outlive SimGame.
[[nodiscard]] std::shared_ptr<void> installLightFactory(sim::SimGame&,
                                                        std::string marker = "pyrm2.dts");
[[nodiscard]] std::shared_ptr<void>
installEffectFactory(sim::SimGame&, std::string opaque, std::string translucent);
} // namespace opent1::world
