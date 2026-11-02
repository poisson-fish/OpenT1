#pragma once
#include <opent1/world/interior_light_player.hpp>
#include <opent1/world/interior_links.hpp>
#include <opent1/world/ts_shape_instance.hpp>
#include <opent1/world/ts_static_shape.hpp>
#include <opent1/world/ts_collision_sim.hpp>
#include <opent1/world/world_surfaces.hpp>
#include <opent1/sim/sim_hardware_environment.hpp>
#include <opent1/sim/sim_software_environment.hpp>

namespace opent1::world {
// Install after registering TS, terrain, interior and bitmap decoders. Dependencies are
// decoded through their typed loaders; failed dependencies remain retryable.
bool registerWorldPreloaders(sim::SimPreLoadManager&);

// Owned DML/bitmap snapshots. Detail zero's material slots are animation frames,
// matching SimVolumetric; all authored details' dependencies are retained.
// Empty names/lists use the environment fallback. Named missing/invalid assets
// fail atomically. Non-texture volume materials are unsupported.
class WorldVolumeMaterials {
public:
    [[nodiscard]] static std::optional<WorldVolumeMaterials>
    load(ResourceManager&, std::span<const sim::SimVolumetricRenderImage>);
    // Borrowed views expire with this object. Copies/moves retain valid bitmaps.
    [[nodiscard]] std::vector<sim::VolumeMaterialFrames> frames() const;

private:
    struct Entry {
        std::string name;
        TsMaterialResources resources;
        std::vector<const gfx::Bitmap*> frames;
    };

    std::vector<Entry> entries_;
};

// Preserve world-space range/attenuation under rigid instance transforms.
// Reject scale/shear, malformed lights or >64 lights instead of truncating.
[[nodiscard]] std::optional<TsLighting> tsSceneLighting(const sim::SimSceneLighting&,
                                                        const TMat3F& shapeToWorld);

// Concrete local provider for the existing simulation light-query seam. Network
// serialization, editor marker shapes and mission persistence remain separate.
class SimSceneLight : public sim::SimObject {
public:
    bool onAdd() override;
    bool setLighting(const sim::SimSceneLighting&); // ambient + at most five lights
    bool setLight(const sim::SimLight&, ColorF ambient = {});

    void enable(bool value) {
        enabled_ = value;
    }

    bool processQuery(sim::SimQuery&) override;

private:
    sim::SimSceneLighting lighting_;
    bool enabled_{true};
};

struct WorldShapeImage {
    TsShapeResources resources;
    TsShapePose pose;
    TMat3F shapeToWorld;
    Int32 detail{};
    bool automaticDetail{};
    Flt32 haze{};
    // First-person attachments sit closer than the world camera's near plane.
    bool firstPerson{};
    // Native ShapeInstance::alwaysAlpha affects hardware material alpha. The
    // original software device's SetConstantAlpha is intentionally a no-op.
    Flt32 opacity{1};
    bool texturePerspective{true};
    bool castShadow{};

    struct SunOverride {
        Flt32 factor{};
        ColorF color{};
    };

    std::optional<SunOverride> sunOverride{};
};

struct WorldShapeQuery : sim::SimQuery {
    WorldShapeQuery() {
        type = sim::EventTypeFirst + 100;
    }

    std::optional<Point3F> cameraPosition;
    std::optional<WorldShapeImage> image;
    // Mounted items share their owner's visibility, lifetime and pose snapshot.
    std::vector<WorldShapeImage> attachments;
};

// Shared by mission adapters and gameplay InteriorShape without a dependency
// from the world library back to the game library.
struct WorldInteriorQuery : sim::SimQuery {
    WorldInteriorQuery() {
        type = sim::EventTypeFirst + 103;
    }

    std::optional<InteriorLinkInstance> image;
    bool pvs{true};
};

struct SimTsAnimationTriggerEvent : sim::SimEvent {
    SimTsAnimationTriggerEvent() {
        type = sim::EventTypeFirst + 101;
    }

    TsTriggerEvent trigger;
    Resource<TsShape> shape; // sequence/name metadata remains valid after reload
};

struct TsAdvanceOptions {
    bool applyRootMotion{true}, bidirectionalTriggers{};
    Flt32 minStep{.01f}, maxStep{.1f};
};

struct TsAdvanceResult {
    bool movementComplete{true};
    UInt32 triggersQueued{}, triggersDropped{};
    TMat3F requestedTransform, appliedTransform;
};

// Local RenderSet and spatial provider. Collision is opt-in through an explicit
// authored detail. Playback advances even when root movement hits a blocker.
class SimTsShape : public sim::SimMovement {
public:
    SimTsShape();
    bool onAdd() override;
    void onRemove() override;
    void onPreLoad(sim::SimPreLoadManager&) override;
    bool setShapeName(std::string_view); // before registration, or atomic reload
    bool setResources(TsShapeResources); // resets animation only on success
    bool setTransform(const TMat3F&);    // rigid local-to-world transform
    bool setCollisionDetail(Int32 detail,
                            TsCollisionLevel = TsCollisionLevel::Faces,
                            bool trigger = false); // -1 disables
    // Call after directly editing instance() to refresh spatial bounds. Prefer
    // advanceAnimation() for playback + motion + queued, owned trigger delivery.
    bool synchronizePose();
    bool setHaze(Flt32 value);
    [[nodiscard]] std::optional<TsAdvanceResult> advanceAnimation(Flt64 seconds,
                                                                  const TsAdvanceOptions& = {});

    void setDetail(Int32 value, bool automatic = false) {
        detail_ = value;
        automaticDetail_ = automatic;
    }

    [[nodiscard]] TsShapeInstance* instance() {
        return instance_ ? &*instance_ : nullptr;
    }

    bool processQuery(sim::SimQuery&) override;

protected:
    bool processCollision(MovementInfo&) override;

private:
    struct SpatialState {
        Box3F bounds;
        std::shared_ptr<const TsShapeCollision> collision;
    };

    [[nodiscard]] std::optional<SpatialState> spatialState(const TsShapePose&, Int32 detail) const;
    bool applySpatialState(SpatialState);
    std::string name_;
    TsShapeResources resources_;
    std::optional<TsShapeInstance> instance_;
    std::shared_ptr<const TsShapeCollision> collision_;
    Int32 collisionDetail_{-1};
    TsCollisionLevel collisionLevel_{TsCollisionLevel::Faces};
    bool collisionTrigger_{}, advancing_{}, applyingRoot_{};
    Int32 detail_{};
    bool automaticDetail_{};
    Flt32 haze_{};
};

struct WorldSurfaceQuery : sim::SimQuery {
    WorldSurfaceQuery() {
        type = sim::EventTypeFirst + 102;
    }

    std::optional<WorldSurfaceInstance> image;
    std::optional<sim::EnvironmentCamera> camera; // input for view-dependent providers
    std::optional<sim::SimSceneLighting>
        lighting; // owned world-space input; absent disables scene merging
    // Override terrain lighting/cache policy for this capture. RetainedLru uses
    // current-size hardware tiles with OpenGL lightmaps. Absence
    // preserves the provider's explicit policy (including comparison viewers).
    std::optional<TerrainTextureTiming> terrainTiming;
    bool retainedTerrain{};
    bool trimCaches{};    // release disposable provider caches; no camera or image request
    bool linkRendering{}; // force automatic interior providers to their state's highest LOD
    bool failed{};        // a provider rejected preparation; capture must fail atomically
private:
    friend class SimInteriorSurface;
    friend class WorldSceneFrame;

    // Scene capture commits next-frame link state only after the whole frame
    // succeeds. Revisions preserve newer detail modes or reentrant camera views.
    struct LinkDetail {
        UInt32 lod{};
        UInt64 revision{};
    };

    std::optional<LinkDetail> linkDetail_;
    bool deferLinkDetail_{};
};

void trimWorldRenderCaches(sim::SimWorld&);

// Static RenderSet provider; pairs with the existing per-block/interior static
// collision providers. Mesh snapshots and rigid placements survive removal.
class SimWorldSurface : public sim::SimObject {
public:
    explicit SimWorldSurface(WorldSurfaceInstance);
    bool onAdd() override;
    bool processQuery(sim::SimQuery&) override;

private:
    WorldSurfaceInstance image_;
};
// Retains LOD hysteresis/window history for its scene camera. A shared stream
// can also serve a SimTerrainStreamCollider with authored full-resolution DTBs.
class Terrain2;

class SimTerrainSurface : public sim::SimObject {
public:
    explicit SimTerrainSurface(TerrainGridResources,
                               const TMat3F& placement = {},
                               bool baked = true,
                               std::optional<Box2F> region = {},
                               const TerrainLodView& policy = {});
    explicit SimTerrainSurface(std::unique_ptr<TerrainResourceStream>,
                               const TMat3F& placement = {},
                               bool baked = true,
                               std::optional<Box2F> region = {},
                               const TerrainLodView& policy = {});
    explicit SimTerrainSurface(std::shared_ptr<TerrainResourceStream>,
                               const TMat3F& placement = {},
                               bool baked = true,
                               std::optional<Box2F> region = {},
                               const TerrainLodView& policy = {});
    bool onAdd() override;
    bool setTerrain2(bool enabled); // bounded eager alternative; default GridRender
    bool processQuery(sim::SimQuery&) override;
    [[nodiscard]] std::optional<UInt16> sampleLighting(Point3F worldPosition) const;

private:
    TerrainGridResources resources_;
    TMat3F placement_;
    bool baked_{};
    std::optional<Box2F> region_;
    TerrainLodView policy_;
    TerrainTextureCache textureCache_;
    TerrainLodState lodState_;
    std::shared_ptr<TerrainResourceStream> stream_;
    std::shared_ptr<Terrain2> terrain2_;
    std::shared_ptr<const WorldSurfaceMesh> retainedMesh_;
    std::optional<sim::EnvironmentCamera> retainedCamera_;
    std::optional<TsLighting> retainedLights_;
    std::optional<TerrainTextureTiming> retainedTiming_;
    bool retainedPatchPolicy_{};
};

// Explicit simulation-clock interior playback. Render queries never advance
// playback; derived scene-light snapshots and old frames own their lightmaps.
// Camera queries select native PVS/backfaces by default; camera-free queries
// return the complete mesh. Explicit scene roots enable linked traversal.
class SimInteriorSurface : public sim::SimContainer {
public:
    explicit SimInteriorSurface(InteriorShapeResources,
                                const TMat3F& placement = {},
                                UInt32 lod = 0,
                                UInt32 lightState = 0,
                                bool baked = true,
                                UInt32 seed = Random::defaultSeed,
                                Resource<InteriorLighting> mission = {});
    bool onAdd() override;
    // Before registration, opt into the installed spatial root. Open containers
    // use native PVS filtering for contained objects; pvs=false uses a simple
    // database. Closed interiors remain leaf images and ignore pvs.
    bool configureContainer(bool open = true, UInt32 mask = ~UInt32{0}, bool pvs = true);

    [[nodiscard]] bool containerPvs() const {
        return pvs_;
    }

    bool processQuery(sim::SimQuery&) override;

    [[nodiscard]] const InteriorLightPlayer* lights() const {
        return player_ ? &*player_ : nullptr;
    }

    bool advanceAnimation(Flt32 seconds);
    bool setLightTime(Int32 id, Flt32 seconds);
    bool setLightState(Int32 id, UInt32 state);
    bool setLightLoopCount(Int32 id, UInt32 loops);
    bool setLightTimeScale(Int32 id, Flt32 scale);
    bool setLightPlaying(Int32 id, bool playing);

    void setVisibilityEnabled(bool enabled) {
        visibilityEnabled_ = enabled;
    }

    // Opt in to camera selection within one authored state. Camera-free queries
    // keep the constructor's fixed LOD. Alternate timelines are evaluated at
    // the retained instance clocks. Rendering never advances light threads.
    bool setAutomaticDetail(UInt32 state);
    bool setFixedDetail();
    // Highest-detail boxes/object PVS, with the last rendered detail's face mask
    // and outside geometry. A successful camera query affects the next capture.
    [[nodiscard]] std::optional<InteriorLinkInstance> linkInstance() const;

private:
    friend class SimMissionInterior;
    SimInteriorSurface() = default;
    friend class WorldSceneFrame;
    bool publish(InteriorLightPlayer);
    bool changeDetail(std::optional<UInt32>);
    void commitLinkDetail(const WorldSurfaceQuery::LinkDetail&);
    [[nodiscard]] std::optional<UInt32> highestDetail(std::optional<UInt32>) const;
    [[nodiscard]] std::optional<Box3F> containerBounds(std::optional<UInt32>) const;
    InteriorShapeResources resources_;
    Resource<InteriorLighting> mission_;
    UInt32 lod_{}, lightState_{};
    UInt32 linkLod_{};
    UInt64 detailRevision_{};
    std::optional<InteriorLightPlayer> player_;
    WorldSurfaceInstance image_;
    std::optional<UInt32> automaticState_;

    struct ViewCache {
        UInt32 lod{};
        std::vector<InteriorLightSlot> slots;
        std::shared_ptr<const WorldSurfaceMesh> base, scene, visibleSource, visible;
        std::optional<TsLighting> lighting;
        std::vector<UInt32> surfaces;
    } cache_;

    bool visibilityEnabled_{true};
    bool spatial_{};
    bool pvs_{};
    bool forceRender_{};
};

struct InteriorSceneRoot {
    sim::SimAddress provider;
    bool cameraContained{};
};

// Camera-first container/PVS membership for ghost scoping. Uses native
// distance-based scope admission, without a render view-cone filter.
[[nodiscard]] std::optional<std::vector<sim::SimAddress>>
scopedSceneObjects(sim::SimManager&,
                   Point3F camera,
                   Flt32 visibleDistance,
                   std::span<const sim::SimAddress> candidates);

struct WorldShadow {
    sim::ShadowGeometry geometry;
    sim::ShadowBitmap bitmap;
};

// Owned, repeatable scene snapshot: shape poses, lighting, DMLs and volume
// geometry survive provider/service removal and resource-manager destruction.
// Immutable surface meshes and their material dependencies are strongly retained.
// Perspective/orthographic TS/terrain camera, origin-zero viewport, <=512 shapes/volumes and <=4096
// total shape polygons. Query callbacks are protected by manager dispatch.
class WorldSceneFrame {
public:
    [[nodiscard]] static std::optional<WorldSceneFrame>
    capture(sim::SimWorld&,
            const sim::EnvironmentCamera&,
            const sim::VolumetricView&,
            bool lighting = true,
            std::optional<std::span<const InteriorSceneRoot>> interiorRoots = std::nullopt,
            std::optional<TerrainTextureTiming> terrainTiming = std::nullopt);
    // Automatic camera-first traversal of the installed root and open interiors.
    // Uncontained providers belong to the exterior; hidden providers are never
    // queried. Spatial parentage is supplied by the simulation container system.
    [[nodiscard]] static std::optional<WorldSceneFrame>
    captureContainers(sim::SimWorld&,
                      const sim::EnvironmentCamera&,
                      const sim::VolumetricView&,
                      bool lighting = true,
                      std::optional<TerrainTextureTiming> terrainTiming = std::nullopt,
                      bool retainedHardware = false);

    // Present for explicit linked capture or automatic container capture.
    [[nodiscard]] std::optional<bool> interiorOutsideVisible() const {
        return outsideVisible_;
    }

    [[nodiscard]] std::span<const TsStaticShapeFrame> shapes() const {
        return shapes_;
    }

    [[nodiscard]] std::span<const sim::SimVolumetricRenderImage> volumes() const {
        return volumes_;
    }

    [[nodiscard]] std::span<const WorldSurfaceInstance> surfaces() const {
        return surfaces_;
    }

    [[nodiscard]] const sim::SimSceneLighting& lighting() const {
        return lighting_;
    }

    [[nodiscard]] std::optional<sim::ShadowGeometry>
    shadowGeometry(const sim::ShadowSourceWindow&,
                   Point3F cameraDirection,
                   Flt32 length = 100,
                   Flt32 lift = .1f,
                   bool nativePartition = false) const;
    [[nodiscard]] std::optional<std::vector<WorldShadow>> shadows(Point3F lightDirection,
                                                                  Flt32 opacity = .5f) const;
    [[nodiscard]] std::optional<gfx::HardwareBatch>
    hardware(const gfx::Palette&,
             std::span<const sim::EnvironmentShadow> = {},
             Flt32 paletteGamma = 1) const;
    bool draw(gfx::SoftwareSurface&,
              std::span<const sim::SoftwareShadow> = {},
              NativeSoftwareTextureCache* = nullptr,
              std::span<const gfx::SortedPolygon> additional = {}) const;
    // Reproject retained inputs for a dirty viewport without querying live
    // providers or changing their LOD/resource state. Only the viewport and
    // optical projection may change; scene selection and world pose stay fixed.
    [[nodiscard]] std::optional<WorldSceneFrame> reframed(const sim::EnvironmentCamera&) const;

private:
    [[nodiscard]] static std::optional<WorldSceneFrame>
    captureImpl(sim::SimWorld&,
                const sim::EnvironmentCamera&,
                const sim::VolumetricView&,
                bool,
                std::optional<std::span<const InteriorSceneRoot>>,
                bool containers,
                std::optional<TerrainTextureTiming>,
                bool retainedHardware = false);
    sim::EnvironmentCamera camera_;
    std::vector<TsStaticShapeFrame> shapes_;
    std::vector<WorldShapeImage> shapeInputs_;
    std::vector<WorldSurfaceInstance> surfaces_;
    std::vector<TsStaticShapeFrame> surfaceFrames_;
    std::vector<sim::SimVolumetricRenderImage> volumes_;
    WorldVolumeMaterials materials_;
    std::optional<bool> outsideVisible_;
    sim::SimSceneLighting lighting_;
    bool lightingEnabled_{};
    bool retainedHardware_{};
};
} // namespace opent1::world
