#pragma once
#include <opent1/world/terrain_authoring.hpp>
#include <opent1/gfx/bitmap_palette_font.hpp>

namespace opent1::world {
using gfx::Bitmap;
enum class LandscapeScriptFormat { RawText, NativeCounted };

struct LandscapeScriptFile {
    std::string text;
    LandscapeScriptFormat format{LandscapeScriptFormat::RawText};
};

// Native PLS stores a little-endian UInt32 byte count including the final NUL.
// Raw scripts remain supported; malformed binary records never fall back to text.
[[nodiscard]] std::optional<LandscapeScriptFile> readLandscapeScript(std::span<const std::byte>,
                                                                     std::size_t maxBytes = 65536);
[[nodiscard]] std::optional<std::vector<std::byte>> writeLandscapeScript(std::string_view,
                                                                         LandscapeScriptFormat);
enum class LandscapeOpcode : UInt32 {
    Seed = 100,
    Load = 200,
    LoadMax,
    Save,
    Push = 300,
    Pop,
    Get,
    Swap,
    Dup,
    Size,
    Add = 400,
    Sub,
    Mul,
    Div,
    Exp,
    Neg,
    Clr,
    Diff,
    Normalize,
    AddStack,
    SubStack,
    Terrain = 500,
    Plasma,
    Crater,
    Peak,
    Rot,
    Ring,
    FillBasin,
    Overlay,
    Blend,
    FlipX,
    FlipY,
    LowPass = 600,
    HighPass,
    BandPass,
    BandReject,
    FastLowPass,
    FastHighPass,
    FastBandPass,
    FastBandReject,
    Fft,
    Smooth,
    Slope,
    Shave,
    Curve,
    Floor,
    Ceil,
    FillNormal,
    Clear,
    AlphaBlend,
    Tile,
    Wrap,
    Clamp,
    ClipMin,
    ClipMax,
    Mask
};

struct LandscapeMatrix {
    UInt32 size{};
    std::vector<Flt32> values;
};

struct LandscapeCommand {
    LandscapeOpcode opcode{};
    std::vector<Flt64> arguments;
    std::string name;
};

struct LandscapeOutput {
    std::string name;
    Bitmap bitmap;
};

// Instance-owned native R250 sequence and stack. Commands and entire scripts
// publish only on success. Load resolves through caller-owned resources; Save
// returns owned output bitmaps for the caller's file transaction.
class Landscape {
public:
    using Loader = std::function<std::optional<Bitmap>(std::string_view)>;
    explicit Landscape(UInt32 size = 32);
    ~Landscape();
    Landscape(const Landscape&);
    Landscape& operator=(const Landscape&);
    Landscape(Landscape&&) noexcept;
    Landscape& operator=(Landscape&&) noexcept;
    bool execute(const LandscapeCommand&, const Loader& loader = {});
    bool script(std::string_view, bool clear = true, const Loader& loader = {});
    bool importBitmap(const Bitmap&, Flt32 maxHeight = 1);
    bool importHeightfield(const LandscapeMatrix&);
    [[nodiscard]] std::optional<Bitmap> bitmap(bool normalize = false) const;
    [[nodiscard]] std::vector<LandscapeMatrix> stack() const;
    [[nodiscard]] UInt32 seed() const;

    [[nodiscard]] std::string_view error() const {
        return error_;
    }

    [[nodiscard]] const std::vector<LandscapeOutput>& outputs() const {
        return outputs_;
    }

    bool apply(TerrainDocument&, UInt32 block, Point2I origin = {}) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    std::string error_;
    std::vector<LandscapeOutput> outputs_;
    bool run(const LandscapeCommand&, const Loader&);
};
} // namespace opent1::world
