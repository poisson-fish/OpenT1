#pragma once
#include <opent1/world/ts_shape_pose.hpp>
#include <opent1/world/ts_lighting.hpp>
#include <opent1/gfx/hardware_batch.hpp>
#include <unordered_map>

namespace opent1::world {
struct TsHardwareCacheStats {
    std::size_t entries{}, bytes{}, budget{};
    UInt64 hits{}, builds{};
};

[[nodiscard]] TsHardwareCacheStats tsHardwareCacheStats();
void trimTsHardwareMeshes(std::size_t budget = 32U * 1024U * 1024U);
void clearTsHardwareMeshes();

struct TsStaticView {
    // Row-vector transform to +Y-forward, +Z-up camera coordinates.
    TMat3F shapeToCamera;
    Int32 width{640}, height{480};
    Point2F focal{320, 320}, center{320, 240};
    Flt32 nearDistance{.1f}, farDistance{10000};
    bool perspective{true};
    Flt32 haze{};           // native per-image constant haze; orthographic images ignore it
    Int32 detail{};         // explicit authored detail index
    bool automaticDetail{}; // project the shape sphere and use authored thresholds
    Flt32 detailScale{1};
    Flt32 minimumProjectedAltitude{};   // optional subpixel triangle rejection, 0..16 pixels
    bool twoSided{};                    // silhouette views may gather both windings
    std::optional<TsLighting> lighting; // omitted = unlit; inputs in shape space
    Flt32 opacity{1};              // hardware constant-alpha override; legacy software ignores it
    bool texturePerspective{true}; // independent of camera projection
};

// Original camera sphere flags (0=inside, 0x80=outside, other bits=clip planes).
// Orthographic native sphere admission tests the center, ignoring radius.
[[nodiscard]] std::optional<UInt32>
tsSphereVisibility(const TsStaticView&, const Point3F&, Flt32 radius);

// Native terrain/interior adapters use the same clipping and material paths as
// TS meshes. Positions are local to shapeToCamera; intensities are baked RGB.
struct StaticSurfaceVertex {
    Point3F position;
    Point2F texture;
    Point3F intensity{1, 1, 1};
    Point2F lightMap{};
    Flt32 farSphereDistance{}, haze{};
};

struct NativeSurfaceTexture {
    gfx::TextureSettings settings;
    Flt32 texelScale{1};
    Int32 minimumMip{};
    bool fixedMip{};
    bool perspective{true};
    std::vector<Point3F> points; // full surface: native nearest-vertex mip policy
};

// Per-renderer cache for immutable native surface inputs. No global state;
// callers serialize access. Palette changes invalidate prepared colors, while
// retained resource handles prevent address reuse from producing false hits.
class NativeSoftwareTextureCache {
public:
    NativeSoftwareTextureCache() = default;
    NativeSoftwareTextureCache(const NativeSoftwareTextureCache&) = delete;
    NativeSoftwareTextureCache& operator=(const NativeSoftwareTextureCache&) = delete;
    void setNoiseEnabled(bool);

    [[nodiscard]] bool noiseEnabled() const {
        return noiseEnabled_;
    }

    bool beginFrame(const gfx::Palette&);
    std::shared_ptr<const gfx::Bitmap> prepare(const Resource<gfx::Bitmap>&,
                                               std::shared_ptr<const gfx::LightMap>,
                                               std::shared_ptr<const NativeSurfaceTexture>,
                                               Int32 mip);
    void clear();

    [[nodiscard]] UInt64 hits() const {
        return hits_;
    }

    [[nodiscard]] UInt64 builds() const {
        return builds_;
    }

private:
    struct Entry {
        Resource<gfx::Bitmap> texture;
        std::shared_ptr<const gfx::LightMap> light;
        std::shared_ptr<const NativeSurfaceTexture> native;
        Int32 mip{};
        std::shared_ptr<const gfx::Bitmap> bitmap;
        UInt64 key{};
    };

    std::list<Entry> entries_;
    std::unordered_multimap<UInt64, std::list<Entry>::iterator> index_;
    std::optional<gfx::Palette> palette_;
    std::size_t bytes_{};
    UInt64 hits_{}, builds_{};
    bool noiseEnabled_{true};
};

struct StaticSurfaceTriangle {
    std::array<StaticSurfaceVertex, 3> vertices;
    UInt32 material{};
    bool lit{}, repeatTexture{};
    std::shared_ptr<const gfx::LightMap> lightMap{};
    std::shared_ptr<const NativeSurfaceTexture> nativeSoftware{};
    bool nativeFarSphere{};
};

// Prepared default or sampled node pose, object offsets and mesh/texture frames. Root nodes
// remain identity, matching original ShapeInstance root-delta separation.
// Six-plane clipping precedes projection. At most 4096 submitted triangles.
// Resources remain strongly retained across unmount/purge/manager destruction.
// Authored colors, opaque maps, zero cutouts, indexed translucency and optional
// packed-normal lighting. Cutout/translucent textures retain legacy unlit policy.
class TsStaticShapeFrame {
public:
    [[nodiscard]] static std::optional<TsStaticShapeFrame> prepare(TsShapeResources,
                                                                   const TsStaticView&);
    [[nodiscard]] static std::optional<TsStaticShapeFrame> prepare(TsShapeResources,
                                                                   const TsStaticView&,
                                                                   const TsShapePose&,
                                                                   bool retainedHardware = false);
    [[nodiscard]] static std::optional<TsStaticShapeFrame> prepareSurfaces(
        TsMaterialResources, std::span<const StaticSurfaceTriangle>, const TsStaticView&);

    [[nodiscard]] std::size_t polygonCount() const noexcept {
        return polygons_.size();
    }

    [[nodiscard]] Int32 detail() const noexcept {
        return detail_;
    } // -1 when below the final threshold

    // Borrowed geometry/material inputs for composition with other scene draws.
    // Returned spans and texture pointers expire with this frame.
    [[nodiscard]] std::optional<std::vector<gfx::HardwarePolygon>>
    hardwarePolygons(const gfx::Palette* = nullptr, Flt32 paletteGamma = 1) const;
    // A supplied cache must have beginFrame() called with this palette first.
    [[nodiscard]] std::optional<std::vector<gfx::SortedPolygon>>
    softwarePolygons(const gfx::Palette* = nullptr, NativeSoftwareTextureCache* = nullptr) const;
    // Gamma corrects palette colors before filtering, lighting and blending,
    // matching OpenGL. Present this batch with final-frame gamma set to one.
    [[nodiscard]] std::optional<gfx::HardwareBatch> hardware(const gfx::Palette* = nullptr,
                                                             Flt32 paletteGamma = 1) const;
    // Solid projected mesh coverage (0/255), matching the original shadow
    // material override. Ignores texture cutouts/alpha. View dimensions <=256.
    [[nodiscard]] std::unique_ptr<gfx::Bitmap> silhouette() const;
    // Synchronous atomic sorted submission; unsupported software texture or
    // palette states fail before changing the destination framebuffer.
    bool draw(gfx::SoftwareSurface&) const;

private:
    struct RetainedPart {
        std::shared_ptr<const gfx::HardwareMesh> mesh;
        gfx::HardwareTransform transform;
    };

    struct Polygon {
        std::array<gfx::PolygonVertex, 3> vertices;
        std::size_t material{};
        gfx::PolygonSource shadeSource{};
        Flt32 shade{1};
        bool repeatTexture{};
        std::shared_ptr<const gfx::LightMap> lightMap{};
        std::array<Point2F, 3> lightCoordinates{};
        std::shared_ptr<const NativeSurfaceTexture> nativeSoftware{};
        Int32 nativeMip{};
        std::array<gfx::PolygonVertex, 3> softwareVertices{};
        std::shared_ptr<const RetainedPart> retained{};
        UInt32 firstIndex{};
    };

    TsShapeResources resources_;
    std::vector<Polygon> polygons_;
    Point2I viewport_;
    Int32 detail_{};
    bool perspective_{true};
    Flt32 opacity_{1};
    bool texturePerspective_{true};
    std::optional<TsShapePose> retainedPose_;
    std::optional<TsStaticView> retainedView_;
    mutable std::shared_ptr<const TsStaticShapeFrame> projected_;
    [[nodiscard]] const TsStaticShapeFrame* projected() const;
};
} // namespace opent1::world
