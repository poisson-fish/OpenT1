#pragma once
#include <opent1/world/interior_collision.hpp>
#include <opent1/world/ts_shape.hpp>

namespace opent1::world {
struct InteriorVertex {
    UInt16 point{}, texture{};
};

struct InteriorSurface {
    UInt8 flags{}, material{}, width{}, height{}, offsetX{}, offsetY{}, vertexCount{}, pointCount{};
    UInt16 plane{};
    UInt32 firstVertex{}, pointIndex{};

    [[nodiscard]] bool planeFront() const {
        return (flags & 0x80U) != 0;
    }

    [[nodiscard]] bool renderable() const {
        return !(flags & 1U) && material != 255;
    }
};

struct InteriorLeaf {
    bool solid{};
    UInt16 flags{}, pvsCount{}, surfaceCount{}, planeCount{};
    UInt32 pvsIndex{}, surfaceIndex{}, planeIndex{};
    Box3F bounds;
};

// Optional native perspective leaf-sphere test. Slopes are camera x/y and z/y;
// the original deliberately uses the full leaf-box diagonal as sphere radius.
struct InteriorViewCone {
    TMat3F objectToCamera;
    Flt32 left{-1}, right{1}, bottom{-1}, top{1}, nearDistance{1}, farDistance{1500};
};

struct InteriorVisibility {
    UInt32 leaf{}, outsideBits{}; // native six-face mask, 0xFC
    std::vector<UInt32> surfaces; // sorted authored surface IDs, includes null materials
};

struct InteriorOutsideVisibility {
    UInt32 leaf{}, outsideBits{};
};

// Native ITRGeometry V7. Original reserved/solid/empty leaf numbering is retained.
// Points are the authored values, before the old renderer's bounding-box jitter.
struct InteriorGeometry {
    Int32 buildId{}, highestMipLevel{};
    UInt32 flags{};
    Flt32 textureScale{};
    Box3F bounds;
    std::vector<InteriorSurface> surfaces;
    std::vector<InteriorCollisionNode> nodes;
    std::vector<InteriorLeaf> leaves;
    std::vector<std::byte> bits;
    std::vector<InteriorVertex> vertices;
    std::vector<Point3F> points;
    std::vector<Point2F> texturePoints;
    std::vector<PlaneF> planes;
    bool expandBoundaryVertices{}; // set by DIG load; native in-memory geometry is unchanged
    // Native ITRGeometry::read expands exact bounding-face coordinates by
    // 0.0125, independently for every LOD. Keep file coordinates for writers.
    [[nodiscard]] Point3F boundaryPoint(Point3F) const;
    [[nodiscard]] static std::unique_ptr<InteriorGeometry> load(StreamIO&, UInt32 size);
    [[nodiscard]] std::optional<std::vector<UInt32>>
    bitIndices(UInt32 offset, UInt32 bytes, UInt32 limit) const;
    [[nodiscard]] std::optional<InteriorCollision> collision() const;
    // No view cone means a conservative PVS + backface selection. Failure owns
    // no partial result. Geometry and the optional cone are borrowed only here.
    [[nodiscard]] std::optional<InteriorVisibility>
    visibility(Point3F camera, const InteriorViewCone* = nullptr) const;
    // Native leaf/outside query without surface, plane or view-cone expansion.
    [[nodiscard]] std::optional<InteriorOutsideVisibility> outsideVisibility(Point3F camera) const;
    // Native point classification without reading the leaf's PVS records.
    [[nodiscard]] std::optional<UInt32> leafAt(Point3F) const;
    // Native container-database selection by world bounding-box centers. Results
    // are input indices in leaf/list order; solid objects use the always bucket.
    [[nodiscard]] std::optional<std::vector<UInt32>>
    visibleObjects(Point3F worldCamera,
                   std::span<const Box3F> worldBounds,
                   const TMat3F& worldToObject = {}) const;
};

struct InteriorState {
    std::string name;
    UInt32 firstLod{}, lodCount{};
};

struct InteriorLod {
    UInt32 minPixels{}, firstLightState{}, linkableFaces{};
    std::string geometry;
};

struct InteriorShape {
    std::vector<InteriorState> states;
    std::vector<InteriorLod> lods;
    std::vector<std::string> lightFiles, lightStateNames;
    std::string materialList;
    bool linked{};
    [[nodiscard]] static std::unique_ptr<InteriorShape> load(StreamIO&, UInt32 size);
    [[nodiscard]] std::vector<std::string> dependencies() const;
    // Absolute LOD index in the selected state. Native truncation and strict
    // minPixels comparison; invalid state/range/pixel inputs fail.
    [[nodiscard]] std::optional<UInt32> detailByPixels(UInt32 state, Flt32 pixels) const;
};

struct InteriorLightSurface {
    UInt32 map{};
    Int16 lightCount{}, lightIndex{};
    UInt8 width{}, height{}, offsetX{}, offsetY{};
};

struct InteriorLightState {
    std::array<UInt16, 4> color{};
    Flt32 time{};
    Int16 dataCount{}, dataIndex{};
};

struct InteriorLightStateData {
    Int16 surface{}, slot{};
    Int32 map{-1};
};

struct InteriorLightAnimation {
    Int32 id{}, name{-1}, stateCount{}, firstState{};
    Flt32 duration{};
    UInt32 flags{};
};

struct InteriorLightControl {
    Int32 id{};
    Flt32 time{};
    std::optional<UInt32> state;
};

struct InteriorLightSlot {
    Int32 map{-1};
    std::array<UInt8, 4> color{};
    bool operator==(const InteriorLightSlot&) const = default;
};

// Native static, animated and mission lightmaps. Host records never contain pointers.
struct InteriorLighting {
    Int32 buildId{}, scaleShift{}, mapSlots{};
    std::vector<InteriorLightSurface> surfaces;
    std::vector<InteriorLightState> states;
    std::vector<InteriorLightStateData> stateData;
    std::vector<InteriorLightAnimation> lights;
    std::vector<std::byte> names, mapData;
    bool mission{};
    std::vector<std::array<Int32, 2>> missionEntries; // source map offset -> override offset
    std::vector<std::array<Int32, 2>> huffmanNodes;
    std::vector<UInt32> huffmanLeaves;
    [[nodiscard]] static std::unique_ptr<InteriorLighting> load(StreamIO&, UInt32 size);
    [[nodiscard]] bool validAnimation() const;
    [[nodiscard]] std::optional<std::vector<InteriorLightSlot>>
    animation(Flt32 seconds = 0, std::span<const InteriorLightControl> controls = {}) const;
    [[nodiscard]] std::optional<std::vector<UInt16>>
    lightmap(std::size_t surface,
             const InteriorLighting* missionOverride = nullptr,
             std::span<const InteriorLightSlot> slots = {}) const;
};

bool registerInteriorLoaders(ResourceManager&);

struct InteriorShapeResources {
    Resource<InteriorShape> shape;
    std::vector<Resource<InteriorGeometry>> geometry; // parallel to LODs
    std::vector<Resource<InteriorLighting>> lighting; // parallel to lightFiles
    TsMaterialResources materials;
    // Numbered mission DIS resources resolve original base lighting separately.
    // Overrides use lod * lightStateNames.size() + state; one failed LOD disables
    // that state across all LODs. Empty means no automatic mission lighting.
    std::vector<Resource<InteriorLighting>> missionLighting;
    [[nodiscard]] static std::optional<InteriorShapeResources> load(ResourceManager&,
                                                                    std::string_view);
    // Native game projection: lowest-detail maximum box extent, horizontal
    // focal length and .001 depth floor. Contained/link views use highest LOD.
    [[nodiscard]] std::optional<UInt32> selectDetail(UInt32 state,
                                                     const TMat3F& objectToCamera,
                                                     Flt32 focalLength,
                                                     bool linkRendering = false,
                                                     bool perspective = true) const;
};
} // namespace opent1::world
