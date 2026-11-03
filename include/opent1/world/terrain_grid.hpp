#pragma once
#include <opent1/world/terrain_collision.hpp>
#include <opent1/world/ts_shape.hpp>

namespace opent1::world {
inline constexpr UInt32 MaxTerrainFileBytes = 64U * 1024U * 1024U;

struct TerrainGridMaterial {
    UInt8 flags{}, index{};

    [[nodiscard]] bool hole() const {
        return (flags & 0x38U) != 0;
    }

    [[nodiscard]] UInt32 emptyLevel() const {
        return (flags >> 3) & 7U;
    }
};

// Host records only. The original 32-bit HRLM color union is an opaque value,
// never a native pointer. ColorDump/bit-packed maps resolve through colorPool.
struct TerrainHiresLightMap {
    Int16 x{}, y{};
    UInt8 flags{}, compression{}, resolutions{}, colors{};
    UInt32 color{};
};

struct TerrainGridBlock {
    Int32 version{}, detailCount{}, lightScale{-1};
    UInt32 width{};
    std::string name;
    Flt32 minimumHeight{}, maximumHeight{};
    std::vector<Flt32> heights;
    std::vector<TerrainGridMaterial> materials;
    std::array<std::vector<std::byte>, 11> pins;
    std::vector<UInt16> lights;
    std::vector<std::byte> hiresDelta;
    Int32 hiresVersion{};
    std::vector<TerrainHiresLightMap> hiresMaps;
    std::vector<UInt16> colorPool;
    std::vector<UInt8> indexTable;
    std::vector<UInt32> treeTable;
    [[nodiscard]] static std::unique_ptr<TerrainGridBlock> load(StreamIO&, UInt32 size);
    [[nodiscard]] std::optional<TerrainCollision> collision(Flt32 squareSize) const;
    // Validated borrowed base grid; the block owns the returned samples.
    [[nodiscard]] std::optional<std::span<const UInt16>> baseLightSamples() const;
    // Owned base grid with the native LZH/XOR hires delta optionally applied.
    [[nodiscard]] std::optional<std::vector<UInt16>> lightSamples(bool hires = true) const;
    // Authored resolution: 0=1x1, 1=2x2, ... 5=17x17. Rows retain grid +Y order.
    [[nodiscard]] std::optional<std::vector<UInt16>> hiresSamples(UInt32 map,
                                                                  UInt32 resolution) const;
    // Aligned power-of-two patch in grid cells. Native tree order (quadrants
    // +X, then +Y); an empty tree/map list selects nothing. Unpublished maps
    // are ignored, as are obsolete trees retained by empty shipped blocks.
    // Malformed reached nodes/references return no partial selection.
    [[nodiscard]] std::optional<std::vector<UInt32>>
    hiresMapIndices(UInt32 x, UInt32 y, UInt32 cells) const;
};

struct TerrainGridBlockEntry {
    Int32 id{};
    std::string name;
};

struct TerrainGridFile {
    Int32 version{}, lastBlockId{}, detailCount{}, scale{}, pattern{};
    Point2I size, origin;
    Box3F bounds; // opaque legacy scratch metadata; may be stale/nonfinite
    Flt32 minimumHeight{}, maximumHeight{};
    std::string materialList;
    std::vector<Int32> blockMap; // row-major IDs; zero is a real ID, -1 is absent
    std::vector<TerrainGridBlockEntry> blocks;
    [[nodiscard]] static std::unique_ptr<TerrainGridFile> load(StreamIO&, UInt32 size);
    [[nodiscard]] std::vector<std::string> dependencies(std::string_view gridName) const;
    [[nodiscard]] static std::string blockName(std::string_view gridName, Int32 id);
};

bool registerTerrainLoaders(ResourceManager&);

inline std::size_t resourceResidentBytes(const TerrainGridBlock& v) {
    using opent1::detail::allocationBytes;
    auto bytes = sizeof(v) + allocationBytes(v.name) + allocationBytes(v.heights) +
                 allocationBytes(v.materials) + allocationBytes(v.lights) +
                 allocationBytes(v.hiresDelta) + allocationBytes(v.hiresMaps) +
                 allocationBytes(v.colorPool) + allocationBytes(v.indexTable) +
                 allocationBytes(v.treeTable);
    for (const auto& pins : v.pins)
        bytes += allocationBytes(pins);
    return bytes;
}

inline std::size_t resourceResidentBytes(const TerrainGridBlockEntry& v) {
    return sizeof(v) + opent1::detail::allocationBytes(v.name);
}

inline std::size_t resourceResidentBytes(const TerrainGridFile& v) {
    using opent1::detail::allocationBytes;
    return sizeof(v) + allocationBytes(v.materialList) + allocationBytes(v.blockMap) +
           allocationBytes(v.blocks);
}

struct TerrainCollisionBlock {
    Point3F origin;
    std::shared_ptr<const TerrainCollision> geometry;
};

// Atomic typed load of .dtf, every listed .dtb, DML and texture dependencies.
// Retained handles survive resource-manager destruction. No seam flattening:
// each block retains its own authored boundary vertices and repeating instances.
struct TerrainGridResources {
    Resource<TerrainGridFile> grid;
    std::vector<Resource<TerrainGridBlock>> blocks; // parallel to grid->blocks
    TsMaterialResources materials;
    // DTF + shared materials only; block slots are initially empty. Used by the
    // camera stream. Aggregate collisionBlocks() still requires every block.
    [[nodiscard]] static std::optional<TerrainGridResources>
    loadMetadata(ResourceManager&, std::string_view gridName);
    [[nodiscard]] static std::optional<TerrainGridResources> load(ResourceManager&,
                                                                  std::string_view gridName);
    [[nodiscard]] std::optional<std::vector<TerrainCollisionBlock>> collisionBlocks() const;
    // GridFile::getLighting's native packed-nibble interpolation (including
    // its reversed endpoint weights), in grid-local XY coordinates.
    [[nodiscard]] std::optional<UInt16> lighting(Point2F) const;
};
} // namespace opent1::world
