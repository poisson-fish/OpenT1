#pragma once
#include <opent1/gfx/bitmap_palette_font.hpp>
#include <opent1/math/matrix.hpp>

namespace opent1::world {
inline constexpr UInt32 MaxTsFileBytes = 64U * 1024U * 1024U;

struct TsMaterial {
    UInt32 flags{};
    Flt32 alpha{1};
    Int32 index{};
    std::array<UInt8, 4> rgb{};
    std::string mapFile;
    Int32 surfaceType{};
    Flt32 elasticity{1}, friction{1};
    bool useDefaultProperties{true};

    [[nodiscard]] UInt32 type() const noexcept {
        return flags & 15U;
    }
};

struct TsMaterialList {
    Int32 version{}, details{}, materialsPerDetail{};
    std::vector<TsMaterial> materials; // detail-major, original list ordering
    [[nodiscard]] static std::unique_ptr<TsMaterialList> load(StreamIO&, UInt32 size);
    bool write(StreamIO&) const; // native DML v4
    [[nodiscard]] std::vector<std::string> dependencies() const;
};

struct TsNode {
    Int32 name{}, parent{-1}, subSequenceCount{}, firstSubSequence{}, defaultTransform{};
};

struct TsSequence {
    Int32 name{}, cyclic{};
    Flt32 duration{};
    Int32 priority{}, firstTrigger{}, triggerCount{}, iflCount{}, firstIfl{};
};

struct TsSubSequence {
    Int32 sequence{}, keyCount{}, firstKey{};
};

struct TsKeyframe {
    Flt32 position{};
    UInt16 value{}, materialFlags{};
};

struct TsTransform {
    QuatF rotation;
    Point3F translation;
};

struct TsTransition {
    Int32 startSequence{}, endSequence{};
    Flt32 startPosition{}, endPosition{}, duration{};
    TsTransform transform;
};

struct TsObject {
    Int32 name{}, flags{}, mesh{}, node{}, subSequenceCount{}, firstSubSequence{};
    Point3F offset;
};

struct TsDetail {
    Int32 root{};
    Flt32 size{};
};

struct TsTrigger {
    Flt32 position{};
    Int32 value{};
};

struct TsPackedVertex {
    UInt8 x{}, y{}, z{}, normal{};
};

struct TsVertexIndex {
    Int32 vertex{}, texture{};
};

struct TsFace {
    std::array<TsVertexIndex, 3> corners;
    Int32 material{};
};

struct TsMeshFrame {
    Int32 firstVertex{};
    Point3F scale, origin;
};

struct TsMesh {
    Int32 version{}, verticesPerFrame{}, textureVerticesPerFrame{};
    Flt32 radius{};
    std::vector<TsPackedVertex> vertices;
    std::vector<Point2F> textureVertices;
    std::vector<TsFace> faces;
    std::vector<TsMeshFrame> frames;
    [[nodiscard]] std::optional<Point3F> point(Int32 vertex, Int32 frame = 0) const;
};

// Owned host records. PERS names, little-endian scalars and every historical
// padding byte are decoded explicitly; no native struct layouts are read.
// Supports DTS 0..8, CelAnimMesh 1..3, DML 2..4. Unknown types/versions fail.
struct TsShape {
    Int32 version{}, defaultMaterials{}, alwaysNode{-1};
    Flt32 radius{};
    Point3F center;
    Box3F bounds;
    std::vector<TsNode> nodes;
    std::vector<TsSequence> sequences;
    std::vector<TsSubSequence> subSequences;
    std::vector<TsKeyframe> keyframes;
    std::vector<TsTransform> transforms;
    std::vector<std::string> names;
    std::vector<TsObject> objects;
    std::vector<TsDetail> details;
    std::vector<TsTransition> transitions;
    std::vector<TsTrigger> triggers;
    std::vector<TsMesh> meshes;
    std::optional<TsMaterialList> materials;
    [[nodiscard]] static std::unique_ptr<TsShape> load(StreamIO&, UInt32 size);
    bool write(StreamIO&) const; // native DTS v8, CelAnimMesh v3
    bool importSequence(const TsShape& source,
                        Int32 sequence); // transactional native node/object track import
    [[nodiscard]] std::vector<std::string> dependencies() const;
};

// Explicit registrations; Bitmap loaders must also be registered to resolve maps.
// Original authored threshold policy. -1 means too small to draw; nullopt means
// invalid input. Zero-sized sentinel details stop before utility hierarchies.
[[nodiscard]] std::optional<Int32> selectTsDetail(const TsShape&, Flt32 projectedSize);
// Explicit, atomic pre-publication pass. Case-sensitive prefixes and zero-size
// details move stably after render details and receive size zero. Empty prefixes
// span preserves the authored layout. Returns new-index -> original-index.
[[nodiscard]] std::optional<std::vector<Int32>>
classifyTsUtilityDetails(TsShape&, std::span<const std::string_view> prefixes);
bool registerTsLoaders(ResourceManager&);

struct TsMaterialResources {
    TsMaterialList list;
    std::vector<Resource<gfx::Bitmap>> textures; // parallel to list.materials
    [[nodiscard]] static std::optional<TsMaterialResources> load(ResourceManager&,
                                                                 const TsMaterialList&);
    // Skin bitmaps share the authored material slots, flags and animation remaps.
    [[nodiscard]] static std::optional<TsMaterialResources> loadSkin(ResourceManager&,
                                                                     const TsMaterialList&,
                                                                     std::string_view skin,
                                                                     std::string_view shapeBase);
    // Legacy damage skins stamp zero-transparent indexed decals into only the
    // base mip of opaque textures; authored lower mips and special maps stay intact.
    [[nodiscard]] static std::optional<TsMaterialResources>
    withDamageSkin(ResourceManager&,
                   TsMaterialResources,
                   Int32 damageLevel,
                   std::span<const std::string> bitmapNames);
};

struct TsShapeResources {
    Resource<TsShape> shape;
    TsMaterialResources materials;
    // All texture dependencies are required; no partially loaded result escapes.
    [[nodiscard]] static std::optional<TsShapeResources> load(ResourceManager&,
                                                              std::string_view shapeName);
};
} // namespace opent1::world
