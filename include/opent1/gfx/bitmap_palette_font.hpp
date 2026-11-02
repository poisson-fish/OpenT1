#pragma once
#include <opent1/gfx/palette.hpp>
#include <opent1/math/point.hpp>

namespace opent1::gfx {
// Graphics rectangles include both endpoints (unlike math::Box2I containment).
struct RectI {
    Int32 left{}, top{}, right{-1}, bottom{-1};

    [[nodiscard]] bool valid() const noexcept {
        return left <= right && top <= bottom;
    }

    [[nodiscard]] Int64 width() const noexcept {
        return Int64(right) - left + 1;
    }

    [[nodiscard]] Int64 height() const noexcept {
        return Int64(bottom) - top + 1;
    }

    [[nodiscard]] bool contains(Point2I) const noexcept;
    bool operator==(const RectI&) const = default;
};

[[nodiscard]] std::optional<RectI> intersect(RectI, RectI) noexcept;
// Rejection leaves the caller's endpoints/rectangles unchanged.
bool clipLine(Point2I&, Point2I&, RectI) noexcept;
bool clipBlit(RectI& source, RectI sourceClip, RectI& destination, RectI destinationClip) noexcept;
// Original stretched-blit clipping and signed fractional-phase adjustments.
// Flip bits are X=1, Y=2. Invalid/rejected inputs leave every argument unchanged.
bool clipScaledBlit(
    RectI& source, RectI& destination, RectI clip, Point2I& adjustment, UInt8 flip = 0) noexcept;

enum BitmapAttribute : UInt32 {
    BitmapTransparent = 1,
    BitmapFuzzy = 2,
    BitmapTranslucent = 4,
    BitmapLegacyOwnMemory = 8,
    BitmapAdditive = 16,
    BitmapSubtractive = 32,
    BitmapAlpha88 = 64
};
enum class BitmapFormat { Darkstar, Microsoft };

struct Bitmap {
    static constexpr UInt32 MaxFileBytes = 64U * 1024U * 1024U;
    static constexpr Int32 MaxDimension = 16384, MaxDetailLevels = 9;
    Int32 width{}, height{}, bitDepth{8}, detailLevels{1};
    UInt32 attributes{BitmapLegacyOwnMemory}, paletteIndex{0xffffffffU};
    std::vector<std::byte> pixels;
    std::optional<Palette> palette;

    [[nodiscard]] static std::unique_ptr<Bitmap>
    create(Int32 width, Int32 height, Int32 bitDepth = 8);
    [[nodiscard]] static std::unique_ptr<Bitmap>
    load(StreamIO&, UInt32 size, bool includePalette = true);
    [[nodiscard]] bool valid() const;
    [[nodiscard]] Int32 stride() const noexcept;

    [[nodiscard]] RectI bounds() const noexcept {
        return {0, 0, width > 0 ? width - 1 : -1, height > 0 ? height - 1 : -1};
    }

    // Borrowed spans expire on destruction or mutation of pixels/metadata.
    [[nodiscard]] std::span<const std::byte> level(Int32 detail = 0) const;
    [[nodiscard]] std::span<std::byte> level(Int32 detail = 0);
    [[nodiscard]] std::span<const std::byte> row(Int32 y, Int32 detail = 0) const;
    [[nodiscard]] std::span<std::byte> row(Int32 y, Int32 detail = 0);
    [[nodiscard]] std::unique_ptr<Bitmap> crop(RectI) const; // independent owned copy, base level
    bool flipVertical(); // all detail levels, including row padding
    bool write(StreamIO&, BitmapFormat = BitmapFormat::Darkstar, bool includePalette = true) const;
};

struct BitmapArray {
    static constexpr UInt32 MaxBitmaps = 4096;
    std::vector<Bitmap> bitmaps;
    std::vector<Int32> indexRemap; // sorted, unique external IDs, or empty for dense indices
    UInt32 userDataSize{};
    std::vector<std::byte> userData;
    [[nodiscard]] static std::unique_ptr<BitmapArray>
    load(StreamIO&, UInt32 size, bool includePalette = false);
    [[nodiscard]] bool valid() const;
    [[nodiscard]] const Bitmap* get(Int32 index) const noexcept;
    [[nodiscard]] std::span<const std::byte> data(Int32 index) const noexcept;
    bool write(StreamIO&, bool includePalette = false) const;
};

enum FontFlag : UInt32 {
    FontProportional = 1,
    FontFixed = 2,
    FontMono = 4,
    FontBitmap = 8,
    FontTrueType = 16,
    FontRasterized = 32,
    FontTransparent = 64,
    FontTranslucent = 128,
    FontUnderlined = 256,
    FontUnicode = 512,
    FontLowerCaps = 1024
};

enum TextJustification : UInt32 {
    TextLeft = 1,
    TextRight = 2,
    TextCenterH = 4,
    TextCenterV = 8,
    TextTop = 16,
    TextBottom = 32,
    TextFit = 64
};

struct FontInfo {
    UInt32 flags{FontProportional}, justification{};
    Int32 height{}, width{}, foreColor{255}, backColor{}, baseline{};
    Point2I scale{65536, 65536};
    Int32 spacing{};
};

struct CharInfo {
    UInt8 bitmap{}, x{}, y{}, width{}, height{};
    Int8 baseline{};
    std::array<UInt8, 2> padding{}; // preserve the eight-byte legacy record
    bool operator==(const CharInfo&) const = default;
};

struct Glyph {
    CharInfo info;
    const Bitmap* sheet{}; // borrowed from this font; null denotes missing pink-box glyph
    RectI bounds;
};

struct BitmapFont {
    FontInfo info;
    Int16 lookupOffset{};
    std::vector<Int16> lookup;
    std::vector<CharInfo> characters;
    BitmapArray sheets;
    std::optional<Palette> palette;
    [[nodiscard]] static std::unique_ptr<BitmapFont> load(StreamIO&, UInt32 size);
    [[nodiscard]] bool valid() const;
    [[nodiscard]] Glyph glyph(UInt32 code) const noexcept;
    // Legacy byte strings and UTF-16 code units, bounded and stopped at NUL.
    // Width retains legacy trailing-spacing removal, including empty strings.
    [[nodiscard]] Int64 textWidth(std::string_view) const;
    [[nodiscard]] Int64 textWidth(std::u16string_view) const;
    [[nodiscard]] Int64 textHeight(std::string_view) const;
    [[nodiscard]] Int64 textHeight(std::u16string_view) const;
    void remapColors(std::span<const UInt8, 256>);
    bool setForeColor(UInt8);
    bool setBackColor(UInt8);
    bool write(StreamIO&) const; // version-2 PFON persistent block
};

// No global registrations. Call on the application's resource manager.
// Bitmap: .bmp/.dib/.pbm/.png (legacy alias, not PNG decoding); array: .pba;
// font: .pft. Palettes remain separately registered.
bool registerBitmapFontLoaders(ResourceManager&);

inline std::size_t resourceResidentBytes(const Bitmap& v) {
    return sizeof(v) + opent1::detail::allocationBytes(v.pixels) +
           (v.palette ? resourceResidentBytes(*v.palette) - sizeof(Palette) : 0);
}

inline std::size_t resourceResidentBytes(const BitmapArray& v) {
    using opent1::detail::allocationBytes;
    return sizeof(v) + allocationBytes(v.bitmaps) + allocationBytes(v.indexRemap) +
           allocationBytes(v.userData);
}

inline std::size_t resourceResidentBytes(const BitmapFont& v) {
    using opent1::detail::allocationBytes;
    return sizeof(v) + allocationBytes(v.lookup) + allocationBytes(v.characters) +
           resourceResidentBytes(v.sheets) - sizeof(BitmapArray) +
           (v.palette ? resourceResidentBytes(*v.palette) - sizeof(Palette) : 0);
}
} // namespace opent1::gfx
