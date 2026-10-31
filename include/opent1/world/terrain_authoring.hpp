#pragma once
#include <opent1/world/terrain_grid.hpp>
#include <filesystem>
#include <functional>

namespace opent1::world {
struct TsLighting;
// Native GFIL v1 / GBLK v5 objects, including LZH maps. No host structs or
// pointers are written. Encoding never changes its input.
[[nodiscard]] std::optional<std::vector<std::byte>> writeTerrainGrid(const TerrainGridFile&);
[[nodiscard]] std::optional<std::vector<std::byte>> writeTerrainBlock(const TerrainGridBlock&);
[[nodiscard]] std::optional<TerrainGridBlock> createTerrainBlock(UInt32 width,
                                                                 Int32 lightScale = -1);
bool rebuildTerrainHeightRange(TerrainGridBlock&);
bool setTerrainLightScale(TerrainGridBlock&, Int32);
bool setTerrainHiresDelta(TerrainGridBlock&, std::span<const UInt16>);
// Native detail starts at 1. maxDetail is exclusive. The 3x3 height pins
// and neighboring ancestors follow GridBlock::pinSquare, including clears.
bool pinTerrainSquare(TerrainGridBlock&, UInt32 detail, Point2I, UInt32 maxDetail, bool set);

struct TerrainAuthoredLight {
    Int16 x{}, y{};
    UInt8 flags{};
    std::array<std::vector<UInt16>, 6> resolutions;
};

// Sorted native color palettes, 1/2/4/8-bit packing, mono collapse, ColorDump
// and native child-before-parent quadtree. Replaces all maps atomically.
bool setTerrainHiresMaps(TerrainGridBlock&, std::span<const TerrainAuthoredLight>);

struct TerrainVolumeExtra {
    UInt32 id{};
    std::string name;
    std::vector<std::byte> bytes;
};

struct TerrainDocument {
    TerrainGridFile grid;
    std::vector<TerrainGridBlock> blocks; // same order as grid.blocks
    std::string gridName{"terrain.dtf"};
    std::vector<TerrainVolumeExtra> extras;
    [[nodiscard]] static std::optional<TerrainDocument>
    create(Point2I size, UInt32 width, Int32 scale, Int32 pattern = 0, Int32 lightScale = -1);
    [[nodiscard]] static std::optional<TerrainDocument> load(std::span<const std::byte> ted);
    [[nodiscard]] std::optional<std::vector<std::byte>> encode() const;
    // Each publication gets independent resource identities, invalidating old
    // texture/collision caches without invalidating retained preview frames.
    [[nodiscard]] std::optional<TerrainGridResources>
    resources(TsMaterialResources materials = {}) const;
    // Stage, flush and atomically replace in the destination directory. Failed
    // encoding/writing/replacement leaves the prior destination intact.
    bool save(const std::filesystem::path&) const;
    bool rebuildHeightRanges();
    [[nodiscard]] bool valid() const;
};

// Original GridBlock::buildLightMapTest and SimTerrain seam-copy ordering.
// Lights are already prepared in terrain space; failures leave the document intact.
bool relightTerrainLowResolution(TerrainDocument&, const TsLighting&, Point2F groundOrigin = {});

struct TerrainRelightOptions {
    bool interiorShadows{}, hiresShadows{}, subSample{};
    std::vector<Box3F> interiorBounds;                 // MissionGroup order, in world coordinates
    std::function<bool(Point3F, Point3F)> interiorLOS; // world endpoints
};

// SimTerrain::buildAllLightMaps: native terrain/mission shadows, 17x17 maps,
// rounded downsampling and the two-pass XOR low-resolution shadow map.
bool relightTerrainDetailed(TerrainDocument&, const TsLighting&, const TerrainRelightOptions& = {});

// Publication and history own immutable copies. Old render/collision callers
// may keep snapshots through edits, failed loads, detach and history eviction.
class TerrainHistory {
public:
    std::function<bool(const TerrainDocument&)> publish;
    explicit TerrainHistory(std::size_t maxBytes = 256U * 1024U * 1024U, UInt32 maxSteps = 64);
    bool attach(TerrainDocument);
    void detach();
    bool edit(const std::function<bool(TerrainDocument&)>&);
    bool undo();
    bool redo();
    bool limit(UInt32 steps);

    [[nodiscard]] std::shared_ptr<const TerrainDocument> snapshot() const {
        return current_;
    }

    [[nodiscard]] UInt64 revision() const {
        return revision_;
    }

    [[nodiscard]] bool canUndo() const {
        return !undo_.empty();
    }

    [[nodiscard]] bool canRedo() const {
        return !redo_.empty();
    }

private:
    std::shared_ptr<const TerrainDocument> current_;
    std::vector<std::shared_ptr<const TerrainDocument>> undo_, redo_;
    std::size_t maxBytes_;
    UInt32 maxSteps_;
    UInt64 revision_{};
};
} // namespace opent1::world
