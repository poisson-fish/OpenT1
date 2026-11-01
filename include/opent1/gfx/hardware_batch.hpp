#pragma once
#include <opent1/gfx/software_rasterizer.hpp>
#include <opent1/gfx/texture_cache.hpp>
#include <memory>
#include <ranges>

namespace opent1::gfx {
enum class HardwareBlend : UInt8 { Opaque, Alpha, Additive, Subtractive, Multiply };
enum class HardwareDepth : UInt8 { TestAndWrite, TestOnly, Disabled, WriteOnly };
enum class HardwareMipFilter : UInt8 { None, Nearest, Linear };
enum class HardwareAlphaSource : UInt8 { Texture, Constant, Vertex, Opaque, TextureVertex };
enum class HardwarePaletteMode : UInt8 { Native, OpenGL, OpenGLPacked };
enum class HardwareLightMapMode : UInt8 { OpenGL, OpenGLPacked };

struct HardwareMaterial {
    const Bitmap* texture{}; // borrowed only during prepare()
    Point3F color{1, 1, 1};
    Flt32 opacity{1}, shade{1}, haze{};
    PolygonSource shadeSource{PolygonSource::None}, hazeSource{PolygonSource::None};
    HardwareBlend blend{HardwareBlend::Opaque};
    HardwareDepth depth{HardwareDepth::TestAndWrite};
    bool perspective{true}, wrap{}, linear{}, transparent{}, twoSided{};
    HardwareMipFilter mipFilter{HardwareMipFilter::None};
    Flt32 lodBias{};            // [-16,16], applied in the shader on every backend
    const LightMap* lightMap{}; // borrowed during prepare(); ARGB4444 / RGB565
    HardwareAlphaSource alphaSource{HardwareAlphaSource::Texture};
    Flt32 alphaCutoff{}; // discard final alpha <= cutoff before color/depth writes
    bool paletteAlpha{}; // indexed texture alpha from PaletteColor::flags
    // OpenGL modes require an indexed texture and derive alpha from original
    // RGBA8 / RGB5551 / RGBA4444 palette rules, without paletteAlpha overrides.
    HardwarePaletteMode paletteMode{HardwarePaletteMode::Native};
    Flt32 paletteGamma{1}; // original OpenGL palette gamma before texture filtering/blending
    bool deferredHaze{};   // alpha haze overlays after all material draws; no texture cutouts
    HardwareLightMapMode lightMapMode{HardwareLightMapMode::OpenGL};
    // Unset follows linear. Original OpenGL cutouts use nearest magnification
    // while retaining bilinear minification and nearest authored mip selection.
    std::optional<bool> magnificationLinear{};
    bool dynamicTexture{}; // transient overlays may reuse same-layout GPU storage
    // Legacy world textures use the active palette over embedded colors.
    bool useSurfacePalette{};
};

struct HardwarePolygon {
    std::span<const PolygonVertex> vertices;
    HardwareMaterial material;
    std::span<const Point2F> lightMapCoordinates{}; // required iff lightMap is set
    std::span<const Flt32> vertexOpacity{};         // required for Vertex / TextureVertex
    // Native SimRenderImage sort key. Every polygon of an image uses the same
    // key; sorting individual triangles can interleave unrelated shapes.
    std::optional<Flt32> imageSortValue{};
};

struct HardwareVertex {
    Point2F position{}, texture{};
    Point3F color{1, 1, 1};
    Flt32 haze{}, inverseDepth{1};
    Point2F lightMap{};
    Flt32 opacity{1};
};

struct HardwareMip {
    Int32 width{}, height{};
    std::vector<std::byte> rgba;
};

struct HardwareTexture {
    Int32 width{}, height{};
    std::vector<std::byte> rgba;   // tightly packed, top-down, base mip
    std::vector<HardwareMip> mips; // remaining supplied levels, never generated
    UInt64 hash{};                 // computed once when immutable converted storage is created
    bool dynamic{};                // permits recycling unreferenced GPU allocation storage
};

struct HardwareDraw {
    UInt32 firstVertex{}, vertexCount{}, texture{};
    Point3F color;
    Flt32 opacity{};
    HardwareBlend blend{};
    HardwareDepth depth{};
    bool perspective{}, wrap{}, linear{};
    UInt32 lightMap{}; // white texture when disabled
    HardwareMipFilter mipFilter{};
    Flt32 lodBias{};
    HardwareAlphaSource alphaSource{};
    Flt32 alphaCutoff{};
    bool transparent{};
    bool deferredHaze{};
    bool magnificationLinear{};
};

// Original ALPHA_FILL: normalized palette RGB and flags/255 constant opacity.
// Optional gamma uses original continuous fill-color correction before blending;
// keep final-frame surface gamma at 1 when reproducing this original policy.
// No software blend table is required. The caller may change depth/blend state.
[[nodiscard]] std::optional<HardwareMaterial> hardwarePaletteFill(const Palette&,
                                                                  UInt8 color,
                                                                  UInt32 paletteIndex = 0xffffffffU,
                                                                  Flt32 gamma = 1);

// Immutable, owned CPU snapshot for hardware submission. Screen coordinates and
// inverseDepth follow PolygonVertex; colors/haze are normalized, UVs may repeat.
// Convex clockwise screen polygons draw; other winding is culled unless twoSided.
// Input order is retained: submit opaque geometry before back-to-front alpha
// overlays. Alpha overlays normally use TestOnly; cutouts may TestAndWrite.
// Prepares indexed 8-bit, BGR24 and RGB565/Alpha88 textures. An embedded palette
// overrides the supplied fallback unless useSurfacePalette is set. Palette
// indices select original RGB entries;
// software shade/blend lookup tables are not hardware color/alpha encodings.
// Zero transparency, palette alpha and Alpha88 are explicit. Alpha88 is low-byte
// index plus high-byte opacity; indexed flags are used only with paletteAlpha.
// Texture alpha multiplies opacity by default; Constant/Vertex/Opaque override
// it, retaining explicit transparent zero-alpha cutouts. Vertex alpha multiplies
// constant opacity and uses the same affine/perspective interpolation as RGB.
// TextureVertex additionally multiplies sampled texture alpha. Explicit original
// OpenGL palette/packed and lightmap policies apply during owned preparation.
// Supplied mip chains are owned in full; RGB444 lightmaps use independent UVs
// and original OpenGL nibble expansion. No borrowed input storage is retained.
// Converted textures are shared across batches in a per-thread LRU (4096
// entries / 64 MiB including source snapshots). Mutable source pixels, selected
// palette colors/type, gamma, encoding and mip layout are checked before reuse.
// Invalid state fails before publication. Valid scene storage grows dynamically;
// ScopedRenderLimits optionally applies explicit work/memory limits. GPU indices
// remain UInt32, and malformed geometry or texture encodings are rejected.
class HardwareBatch {
public:
    [[nodiscard]] static std::optional<HardwareBatch> prepare(std::span<const HardwarePolygon>,
                                                              const Palette* = nullptr);
    // Combine independently prepared layers in draw order. Inputs remain owned
    // and unchanged; the aggregate obeys the same frame memory/work limits.
    [[nodiscard]] static std::optional<HardwareBatch>
        compose(std::span<const HardwareBatch* const>);

    [[nodiscard]] std::span<const HardwareVertex> vertices() const noexcept {
        return vertices_;
    }

    [[nodiscard]] auto textures() const noexcept {
        return std::span(textures_) |
               std::views::transform([](const auto& t) -> const HardwareTexture& {
                   return *t;
               });
    }

    [[nodiscard]] std::span<const std::shared_ptr<const HardwareTexture>>
    textureRefs() const noexcept {
        return textures_;
    }

    [[nodiscard]] std::span<const UInt64> textureHashes() const noexcept {
        return textureHashes_;
    }

    [[nodiscard]] std::span<const HardwareDraw> draws() const noexcept {
        return draws_;
    }

    // Deferred haze finishes within each layer before later HUD draws.
    [[nodiscard]] std::span<const UInt32> layerEnds() const noexcept {
        return layerEnds_;
    }

private:
    std::vector<HardwareVertex> vertices_;
    std::vector<std::shared_ptr<const HardwareTexture>> textures_;
    std::vector<UInt64> textureHashes_; // stable content fingerprints; equality still checks bytes
    std::vector<HardwareDraw> draws_;
    std::vector<UInt32> layerEnds_;
};
} // namespace opent1::gfx
