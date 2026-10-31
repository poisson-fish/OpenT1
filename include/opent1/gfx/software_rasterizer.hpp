#pragma once
#include <opent1/gfx/bitmap_palette_font.hpp>
#include <opent1/gfx/software_visibility.hpp>
#include <functional>

namespace opent1::gfx {
struct LightMap;
enum class Flip : UInt8 { None = 0, X = 1, Y = 2, Both = 3 };
enum class PolygonSource : UInt8 { None, Constant, Vertex };

struct PolygonVertex {
    Point2F position{};
    Point2F texture{};      // normalized UVs, scaled by allocated stride/height
    Point3F color{1, 1, 1}; // legacy .22 R + .67 G + .11 B vertex shade
    Flt32 haze{};
    Flt32 inverseDepth{1}; // positive projected 1/z for perspective/visibility
    Point2F lightMap{};    // independent normalized, clamped lightmap coordinates
};

struct PolygonMaterial {
    const Bitmap* texture{}; // borrowed only for the synchronous draw
    UInt8 color{};
    UInt32 paletteIndex{0xffffffffU}; // flat fill; textures use their own index
    PolygonSource shadeSource{PolygonSource::None}, hazeSource{PolygonSource::None};
    Flt32 shade{1}, haze{};
    bool transparent{}; // textured zero-index transparency
    bool blend{};       // selected palette's 256x256 translucency/add/subtract table
    bool perspective{}; // original 24-pixel perspective subspans
    bool wrap{};        // perspective only; power-of-two stride/height, no blend
    // Explicit modern border policy: clamp normalized UV samples to the last
    // logical texel, including UV==1 and perspective approximation overshoot.
    // Requires a texture; incompatible with wrap. Defaults retain legacy paths.
    bool clampTexture{};
    const LightMap* lightMap{}; // synchronous borrow; alpha nibble supplies per-pixel shade
};

struct SortedPolygon {
    std::span<const PolygonVertex> vertices; // inverseDepth also drives visibility
    PolygonMaterial material{};
    bool alwaysBehind{};
    std::shared_ptr<const Bitmap> retainedTexture{};
};

// Owned, indexed 2D framebuffer. No global raster/driver state and no SDL dependency.
// Sorted materials use the separate owned visibility stage. Prepared owned
// textures from texture_cache.hpp are ordinary Bitmap inputs to these draws.
class SoftwareSurface {
public:
    // A compositor can flush the indexed foreground and submit an alpha fill
    // against the actual GPU scene instead of blending against transparent zero.
    using BlendRenderer = std::function<bool(
        SoftwareSurface&, std::span<const PolygonVertex>, const PolygonMaterial&)>;

    void setBlendRenderer(BlendRenderer renderer) {
        blendRenderer_ = std::move(renderer);
    }

    [[nodiscard]] static std::unique_ptr<SoftwareSurface> create(Int32 width, Int32 height);

    [[nodiscard]] const Bitmap& bitmap() const noexcept {
        return bitmap_;
    }

    // Consume a completed surface without cloning its framebuffer. Drawing and
    // borrowed pixel/bitmap access must finish before this ownership transfer.
    [[nodiscard]] Bitmap releaseBitmap() && noexcept {
        return std::move(bitmap_);
    }

    [[nodiscard]] std::span<std::byte> pixels() noexcept {
        return bitmap_.pixels;
    }

    [[nodiscard]] RectI clip() const noexcept {
        return clip_;
    }

    void setClip(RectI) noexcept;

    void resetClip() noexcept {
        clip_ = bitmap_.bounds();
    }

    bool setPalette(const Palette&); // owns a snapshot, including remap/blend tables

    // Borrowed until setPalette/clearPalette or surface destruction.
    [[nodiscard]] const Palette* palette() const noexcept {
        return palette_ ? &*palette_ : nullptr;
    }

    void clearPalette() noexcept {
        palette_.reset();
    }

    void clear(UInt8); // whole framebuffer, including padding, independent of clip
    void point(Point2I, UInt8);
    void fill(RectI, UInt8);
    void rectangle(RectI, UInt8);
    void line(Point2I, Point2I, UInt8); // portable uplift of l_raster.asm span policy
    // Legacy attributes select opaque, zero-transparent or table-translucent
    // drawing. Flip coordinates reflect the whole bitmap, including subregions.
    // Unsupported depths/flip flags or missing blend tables fail before drawing.
    bool blit(const Bitmap&, RectI source, Point2I destination, Flip = Flip::None);

    bool blit(const Bitmap& bitmap, Point2I destination, Flip flip = Flip::None) {
        return blit(bitmap, bitmap.bounds(), destination, flip);
    }

    // Whole-bitmap stretch using the original unsigned .32 carry policy.
    // Positive destination sizes are bounded to Bitmap::MaxDimension. Unsafe
    // legacy clipping/sample combinations fail without changing the framebuffer.
    bool stretch(const Bitmap&, RectI destination, Flip = Flip::None);
    // Nearest-neighbor rotation about an explicit screen-space center. Size is
    // positive and <= MaxDimension per axis. Angles are clockwise 16-bit turns
    // (16384 = 90 degrees), reduced modulo 65536. Pixel centers and half-open
    // source bounds make clipping repeatable. Self-draws use owned snapshots.
    bool rotate(const Bitmap&, Point2F center, Point2F size, Int32 angle, Flip = Flip::None);
    // Convex screen-space polygon, 3..100 vertices, coordinates within +/-16384.
    // Clockwise screen winding draws; reverse/degenerate winding is culled.
    // Original 16.16 edges, affine/vertex triangle fans and perspective subspans.
    // Constant shade+haze, vertex shade+constant haze, or vertex haze are supported.
    // Perspective uses positive inverseDepth in [1/65536,65536]. Wrapped UVs
    // may be signed, including vertex shade/haze; unlit perspective transparency
    // also uses wrapped sampling. Wrapped textures require power-of-two storage.
    // Independent lightmaps interpolate normalized coordinates perspectively
    // when requested, bilinearly clamp samples and use the alpha shade nibble.
    // They require a shade palette and exclude simultaneous shade/haze/blend.
    // Unsupported material state and unsafe samples fail before any pixels change.
    bool polygon(std::span<const PolygonVertex>, const PolygonMaterial& = {});
    // Atomic batch of sorted flat fills, including palette table overlays.
    bool sortedFills(std::span<const SortedFill>, DepthOrder = DepthOrder::LargerInFront);
    // Sorted material batch with the same states as polygon(). Affine/vertex-lit
    // fans participate as separate triangles, matching original submission.
    // Zero-transparent/blended materials are clipped by opaque geometry and
    // retain submission order. Transparent holes do not occlude other polygons.
    // Visibility fragments restart original interpolation/subspan boundaries;
    // the surface clip only masks that result. Self-textures read the pre-batch
    // framebuffer. Failure leaves all pixels unchanged. ScopedRenderLimits can
    // explicitly bound prepared spans, primitives and visibility work.
    bool sortedPolygons(std::span<const SortedPolygon>, DepthOrder = DepthOrder::LargerInFront);
    // Typed byte/UTF-16 code units, 16.16 scales in [0,64], nearest sampling.
    // Cumulative pen/spacing scaling agrees with font metrics. Missing glyphs
    // draw index 253. Validation/allocation completes before any pixels change.
    bool text(const BitmapFont&, Point2I, std::string_view);
    bool text(const BitmapFont&, Point2I, std::u16string_view);
    // Rectangle justification uses scaled width/baseline. TEXT_FIT derives
    // independent scales from unscaled width/font height without font mutation;
    // empty strings succeed and invalid/nonpositive fit metrics fail atomically.
    bool textRect(const BitmapFont&, RectI, std::string_view);
    bool textRect(const BitmapFont&, RectI, std::u16string_view);
    // Opaque RGBA bytes in row order for presentation. Gamma 1 is exact;
    // other finite gamma values use round(255 * pow(component/255, 1/gamma)).
    bool
    rgba(std::span<std::byte> output, Flt32 gamma = 1, UInt32 paletteIndex = 0xffffffffU) const;

private:
    explicit SoftwareSurface(Bitmap bitmap) : bitmap_(std::move(bitmap)), clip_(bitmap_.bounds()) {}

    Bitmap bitmap_;
    RectI clip_;
    std::optional<Palette> palette_;
    BlendRenderer blendRenderer_;
    bool polygonSpans(std::span<const PolygonVertex>,
                      const PolygonMaterial&,
                      std::span<const VisibilitySpan>,
                      bool sorted,
                      std::size_t& spanCount);
    template <class Text> bool drawText(const BitmapFont&, Point2I, Text, Point2I scale);
    template <class Text> bool drawTextRect(const BitmapFont&, RectI, Text);
};
} // namespace opent1::gfx
