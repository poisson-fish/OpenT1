#pragma once
#include <opent1/core/res_manager.hpp>
#include <array>
#include <optional>

namespace opent1::gfx {
// PALETTEENTRY bytes are RGB + format-specific flags, never host OS structs.
struct PaletteColor {
    UInt8 red{}, green{}, blue{}, flags{};
    bool operator==(const PaletteColor&) const = default;
};
enum class PaletteType : UInt32 {
    NoRemap,
    ShadeHaze,
    Translucent,
    ColorQuant,
    AlphaQuant,
    AdditiveQuant,
    Additive,
    SubtractiveQuant,
    Subtractive
};

struct SubPalette {
    bool operator==(const SubPalette&) const = default;
    std::array<PaletteColor, 256> colors{};
    UInt32 index{};
    PaletteType type{PaletteType::NoRemap};
    // Shade/haze or 256x256 blend bytes, followed on disk by auxiliary maps.
    std::vector<std::byte> remap;
    std::array<UInt8, 256> identity{};
    std::array<std::array<Flt32, 256>, 4> components{}; // R, G, B, A
};

struct PaletteBuildOptions {
    Int32 shadeShift{5}, hazeLevels{16};
    std::array<float, 3> shadeColor{0, 0, 0}, hazeColor{1, 1, 1};
    float shadePercentage{1}, hazePercentage{1};
};

struct Palette {
    bool operator==(const Palette&) const = default;
    static constexpr UInt32 MaxFileBytes = 64U * 1024U * 1024U;
    std::vector<SubPalette> palettes{1};
    Int32 shadeShift{}, hazeLevels{}, hazeColor{};
    std::array<UInt32, 8> allowedMatches{0x003fffffU,
                                         0xffffffffU,
                                         0xffffffffU,
                                         0xffffffffU,
                                         0xffffffffU,
                                         0xffffffffU,
                                         0xffffffffU,
                                         0xfffffc00U}; // legacy MSB-first bits 10..245
    std::optional<std::array<Flt32, 256>> weights;
    UInt32 usedRangeStart{}, usedRangeEnd{};
    // Bounded exact-size load. Failure publishes no partially decoded palette.
    [[nodiscard]] static std::unique_ptr<Palette> load(StreamIO&, UInt32 size);
    [[nodiscard]] bool valid() const;
    bool write(StreamIO&, bool microsoft = false) const;
    // Active mpMerger RGB authoring contract: shade/haze, alpha, additive and
    // subtractive maps plus identity/component tables. Publishes atomically.
    bool buildRemapTables(const PaletteBuildOptions& = {});
    [[nodiscard]] const SubPalette& find(UInt32 index = 0xffffffffU) const;
    [[nodiscard]] UInt8 nearest(PaletteColor) const;
    [[nodiscard]] std::optional<UInt8>
    mapColor(UInt8 color, UInt32 shade, UInt32 haze, UInt32 index = 0xffffffffU) const;
};

bool registerPaletteLoaders(ResourceManager&); // .pal, .ppl and .dpl
} // namespace opent1::gfx
