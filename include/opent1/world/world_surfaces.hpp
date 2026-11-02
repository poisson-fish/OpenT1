#pragma once
#include <opent1/world/interior_shape.hpp>
#include <opent1/world/terrain_grid.hpp>
#include <opent1/world/terrain_collision_snapshot.hpp>
#include <opent1/world/terrain_textures.hpp>
#include <opent1/world/terrain_texture_detail.hpp>
#include <opent1/world/terrain_geometry.hpp>
#include <opent1/world/terrain_visibility.hpp>
#include <opent1/world/ts_static_shape.hpp>
#include <opent1/sim/sim_environment_render.hpp>
#include <set>

namespace opent1::world {
struct InteriorSurfaceLighting {
    Flt32 seconds{}; // autostart lights loop from an explicit absolute clock
    std::span<const InteriorLightControl> controls{}; // explicit state or time in [0,duration]
    const InteriorLighting*
        mission{}; // explicit override takes precedence over resolved missionLighting; borrowed here
    std::optional<std::span<const InteriorLightSlot>>
        slots;                 // retained playback slots, copied into owned maps
    const TsLighting* scene{}; // local-space scene lights, borrowed only during preparation
    const struct WorldSurfaceMesh* previous{}; // immutable snapshot for independent dirty maps
    Resource<InteriorLighting> missionHandle;  // optional owning identity for an explicit override
    bool refreshMaps{}; // explicit invalidation retains geometry but rebuilds lighting
};

struct TerrainLodWindow {
    UInt32 radiusCells{32}, guardCells{8}; // camera coverage + retained margin in native cells
};

struct TerrainLodView {
    Point3F camera; // grid-local coordinates; error uses distance to authored tile bounds
    Flt32 focalLength{320}, pixelError{2}, minimumDistance{.1f};
    UInt32 maximumLevel{10};   // 0 retains native cells, 10 permits 1024-cell tiles
    bool compositeTextures{};  // merge compatible opaque materials/orientations
    UInt32 texturePixels{128}; // authored mip tiles, power of two in [4,256]
    bool hiresLighting{true};
    Flt32 hysteresis{.15f}; // [0,1): previously split tiles coarsen below (1-h)*pixelError
    std::optional<TerrainLodWindow> window{}; // absent keeps the explicit region/full grid
    TerrainLightMapDensity lightMapDensity{
        TerrainLightMapDensity::
            Expanded}; // expanded, native software or native OpenGL composite lighting
    std::optional<TerrainTextureView>
        nativeTextures{}; // camera-driven authored mips; texturePixels caps composites, direct mips clamp to availability
    bool
        sceneLighting{}; // SimTerrainSurface: merge scene point lights; requires baked composite Native/NativeOpenGL maps
    std::optional<TerrainGeometryView>
        nativeGeometry{}; // native distance subdivision/geomorphing; replaces error/hysteresis, caps scan level at 7
    std::optional<TerrainVisibilityView>
        nativeVisibility{}; // requires nativeGeometry; original view-cone rows and visible-distance scan level
    bool
        nativeTraversal{}; // requires nativeGeometry: scan-row order, shared first-writer vertices and coarse empty levels
    TerrainTextureTiming textureTiming{
        TerrainTextureTiming::
            RetainedLru}; // explicit historical callback policy, independent of mip selection
    Flt32 perspectiveDistance{500};
    std::optional<Flt32> hazeDistance{};
    bool
        retainedPatches{}; // hardware native traversal; software callbacks keep their original path
};

// Bind to immutable terrain resources, as for TerrainTextureCache. Keep one per
// camera/provider. Successful preparations replace history; failures preserve it.
// The window bounds geometry preparation; all source resources remain retained.
class TerrainLodState {
public:
    explicit TerrainLodState(TerrainGridResources resources) : resources_(std::move(resources)) {}

    [[nodiscard]] bool matches(const TerrainGridResources&) const;

    void clear() {
        split_.clear();
        window_.reset();
    }

private:
    friend struct WorldSurfaceMesh;
    friend class TerrainResourceStream;
    TerrainGridResources resources_;
    std::set<std::array<UInt32, 4>> split_;      // placement, local x/y, size
    std::optional<std::array<Flt64, 4>> window_; // min/max XY in native cells
};

// Immutable after publication. Each triangle keeps local geometry for receivers
// and the same UV/material/lighting input for software and GPU rendering.
struct WorldSurfaceMesh {
    struct InteriorMesh {
        Resource<InteriorGeometry> source;
        Resource<InteriorLighting> lighting;
        std::vector<std::shared_ptr<const NativeSurfaceTexture>> software;
        std::vector<UInt32> surfaceIds;
        TsMaterialResources materials;
        std::shared_ptr<const gfx::HardwareMesh> hardware;
        std::vector<std::array<UInt32, 2>> ranges; // first index/count by authored surface
        std::vector<Box3F> surfaceBounds;
    };

    // Interior snapshots share local geometry, independently of maps and visibility.
    std::shared_ptr<const InteriorMesh> interiorMesh;
    std::shared_ptr<const std::vector<std::shared_ptr<const gfx::LightMap>>> interiorMaps;
    std::vector<InteriorLightSlot> interiorSlots;
    const InteriorLighting* interiorMission{}; // identity only, retained by provider/resources
    Resource<InteriorLighting> interiorMissionHandle;
    std::optional<TsLighting> interiorSceneLighting;
    std::vector<bool> interiorSceneAffected;
    std::optional<std::vector<UInt32>> interiorSelection;

    struct TerrainPatch {
        std::shared_ptr<const gfx::HardwareMesh> topology;
        UInt32 topologyKey{};
        std::array<std::array<Flt32, 4>, 8> heights{};
        Point3F origin;
        Flt32 extent{};
        UInt32 material{};
        std::size_t firstTriangle{};
        std::shared_ptr<const gfx::LightMap> lightMap;
        Point2F mapOrigin{}, mapScale{1, 1};
    };

    struct TerrainPatches {
        std::vector<TerrainPatch> patches;
        Point3F camera;
        Flt32 farDistance{}, hazeDistance{};
        std::size_t triangles{};
    };

    // Camera selection owns compact inputs; topology and source heightfields are shared.
    std::shared_ptr<const TerrainPatches> terrainPatches;
    [[nodiscard]] StaticSurfaceTriangle terrainTriangle(std::size_t) const;
    TsMaterialResources materials;
    std::vector<StaticSurfaceTriangle> triangles;
    std::vector<UInt32>
        interiorSurfaceIds; // parallel to interior triangles; terrain leaves this empty
    // Native selected squares: global cell X/Y, cell extent, authored block ID.
    // Retained with the frame for source traces and cache/geometry diagnosis.
    std::vector<std::array<UInt32, 4>> terrainSquareIds;
    Box3F bounds;

    [[nodiscard]] auto surfaceTriangles(bool geometryOnly = false) const {
        const auto count = interiorMesh     ? interiorMesh->surfaceIds.size()
                           : terrainPatches ? terrainPatches->triangles
                                            : triangles.size();
        return std::views::iota(std::size_t{}, count) | std::views::filter([this](auto i) {
                   return !interiorMesh || !interiorSelection ||
                          std::ranges::binary_search(*interiorSelection,
                                                     interiorMesh->surfaceIds[i]);
               }) |
               std::views::transform([this, geometryOnly](auto i) {
                   if (terrainPatches)
                       return terrainTriangle(i);
                   if (!interiorMesh) {
                       if (!geometryOnly)
                           return triangles[i];
                       StaticSurfaceTriangle triangle;
                       triangle.vertices = triangles[i].vertices;
                       triangle.material = triangles[i].material;
                       return triangle;
                   }
                   StaticSurfaceTriangle triangle;
                   const auto id = interiorMesh->surfaceIds[i];
                   triangle.material = interiorMesh->source->surfaces[id].material;
                   triangle.lit = bool(interiorMesh->lighting);
                   triangle.repeatTexture = true;
                   if (!geometryOnly)
                       triangle.nativeSoftware = interiorMesh->software[id];
                   if (!geometryOnly && interiorMaps)
                       triangle.lightMap = (*interiorMaps)[id];
                   for (std::size_t j = 0; j < 3; ++j) {
                       const auto& v =
                           interiorMesh->hardware
                               ->vertices()[interiorMesh->hardware->indices()[i * 3 + j]];
                       triangle.vertices[j] = {{v.position.x, v.position.y, v.inverseDepth},
                                               v.texture,
                                               v.color,
                                               v.lightMap};
                   }
                   return triangle;
               });
    }

    [[nodiscard]] std::size_t triangleCount() const {
        if (terrainPatches)
            return terrainPatches->triangles;
        if (!interiorMesh)
            return triangles.size();
        if (!interiorSelection)
            return interiorMesh->surfaceIds.size();
        return std::size_t(std::ranges::distance(surfaceTriangles()));
    }

    // Region selects intersecting full-resolution cells in grid-local XY. Use
    // bounded patches for large terrains, or terrainLod() for adaptive geometry.
    [[nodiscard]] static std::optional<WorldSurfaceMesh> terrain(const TerrainGridResources&,
                                                                 bool bakedLighting = true,
                                                                 std::optional<Box2F> region = {});
    [[nodiscard]] static std::optional<WorldSurfaceMesh>
    terrainLod(const TerrainGridResources&,
               const TerrainLodView&,
               bool bakedLighting = true,
               std::optional<Box2F> region = {},
               TerrainTextureCache* cache = nullptr,
               TerrainLodState* state = nullptr,
               const TsLighting* scene =
                   nullptr); // borrowed grid-local dynamic point lights, copied into owned maps
    [[nodiscard]] static std::optional<WorldSurfaceMesh>
    interior(const InteriorShapeResources&,
             UInt32 lod = 0,
             UInt32 lightState = 0,
             bool bakedLighting = true,
             const InteriorSurfaceLighting& = {});
    [[nodiscard]] std::optional<TsStaticShapeFrame> prepare(const TsStaticView&) const;
    [[nodiscard]] bool retainedHardwareCompatible() const;
    [[nodiscard]] std::optional<std::vector<gfx::HardwarePolygon>>
    hardwareInterior(const TsStaticView&, const gfx::Palette&, Flt32 gamma = 1) const;
    // Select from a complete interior mesh while retaining its immutable maps.
    [[nodiscard]] std::optional<WorldSurfaceMesh>
    visibleInterior(std::span<const UInt32> surfaces) const;
    [[nodiscard]] std::optional<UInt16> sampleLighting(Point3F localPosition) const;
};

struct WorldSurfaceInstance {
    std::shared_ptr<const WorldSurfaceMesh> mesh;
    TMat3F placement;
};

// Synchronous per-camera DTB working set. Shared DTF/DML/bitmap inputs stay
// resident. Failed preparation preserves residency, LOD history and tile cache.
// Source mounts and published resources must remain immutable. External locking
// is required, as for ResourceManager. Meshes own all inputs needed for drawing.
class TerrainResourceStream {
public:
    [[nodiscard]] static std::unique_ptr<TerrainResourceStream>
    open(std::shared_ptr<ResourceManager>,
         std::string_view gridName,
         std::size_t maximumBytes = 64U * 1024U * 1024U,
         UInt32 maximumBlocks = 64);
    [[nodiscard]] std::optional<WorldSurfaceMesh> prepare(const TerrainLodView&,
                                                          bool baked = true,
                                                          std::optional<Box2F> region = {},
                                                          const TsLighting* scene = nullptr);
    // Inclusive grid-local XY query bounds. The committed set is the union of
    // the latest render window and collision window. Old caller snapshots are
    // excluded from residency accounting and survive later eviction/destruction.
    [[nodiscard]] std::shared_ptr<const TerrainCollisionSnapshot> prepareCollision(const Box2F&);

    [[nodiscard]] const TerrainGridFile& grid() const {
        return *resources_.grid;
    }

    [[nodiscard]] std::size_t residentBytes() const {
        return bytes_;
    }

    [[nodiscard]] UInt32 residentBlocks() const;

    void trimCaches() {
        cache_.clear();
        state_.clear();
    }

    ~TerrainResourceStream();
    TerrainResourceStream(const TerrainResourceStream&) = delete;
    TerrainResourceStream& operator=(const TerrainResourceStream&) = delete;

private:
    TerrainResourceStream(
        std::shared_ptr<ResourceManager>, std::string, TerrainGridResources, std::size_t, UInt32);
    [[nodiscard]] std::optional<TerrainGridResources>
    stage(const std::vector<bool>&, std::vector<std::string>& evictions, std::size_t& bytes) const;
    std::shared_ptr<ResourceManager> manager_;
    std::string name_;
    TerrainGridResources resources_;
    TerrainLodState state_;
    TerrainTextureCache cache_;
    std::size_t maximumBytes_, bytes_{};
    UInt32 maximumBlocks_;
    std::vector<bool> renderNeeded_, collisionNeeded_;
};

// Partition projected receiver footprints and intersecting depth planes. Output
// is non-overlapping along the light, with deterministic first-input ties.
// Returns no result on malformed inputs or bounded-work/fragment exhaustion.
[[nodiscard]] std::optional<std::vector<sim::ShadowReceiver>> collectShadowReceivers(
    const sim::ShadowSourceWindow&, std::span<const WorldSurfaceInstance>, Flt32 length = 100);
[[nodiscard]] std::optional<sim::ShadowGeometry>
worldShadowGeometry(const sim::ShadowSourceWindow&,
                    std::span<const WorldSurfaceInstance>,
                    Point3F cameraDirection,
                    Flt32 length = 100,
                    Flt32 lift = .1f,
                    bool nativePartition = false);
} // namespace opent1::world
