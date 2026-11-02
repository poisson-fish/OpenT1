#include <opent1/gfx/bitmap_palette_font.hpp>
#include <opent1/gfx/bitmap_content.hpp>
#include <opent1/core/mem_stream.hpp>
#include <algorithm>
#include <bit>
#include <cstring>
#include <limits>
#include <ranges>

namespace opent1::gfx {
namespace {
constexpr auto pbmp = makeFourCC('P', 'B', 'M', 'P'), pbma = makeFourCC('P', 'B', 'M', 'A');
constexpr auto head = makeFourCC('h', 'e', 'a', 'd'), dataTag = makeFourCC('d', 'a', 't', 'a');
constexpr auto riff = makeFourCC('R', 'I', 'F', 'F'), pfon = makeFourCC('P', 'F', 'O', 'N');
constexpr auto detailTag = makeFourCC('D', 'E', 'T', 'L'),
               indexTag = makeFourCC('P', 'i', 'D', 'X');
constexpr auto userTag = makeFourCC('u', 's', 'e', 'r'), remapTag = makeFourCC('r', 'm', 'a', 'p');

struct Reader {
    std::span<const std::byte> bytes;
    std::size_t position{};
    bool ok{true};

    [[nodiscard]] std::size_t remaining() const {
        return bytes.size() - position;
    }

    std::span<const std::byte> take(std::size_t count) {
        if (!ok || count > remaining()) {
            ok = false;
            return {};
        }
        const auto result = bytes.subspan(position, count);
        position += count;
        return result;
    }

    template <class T> bool read(T& value) {
        const auto raw = take(sizeof(T));
        if (!ok) {
            return false;
        }
        using U = std::make_unsigned_t<T>;
        UInt64 result{};
        std::ranges::for_each(std::views::iota(std::size_t{0}, sizeof(T)), [&](auto i) {
            result |= UInt64(std::to_integer<UInt8>(raw[i])) << (i * 8);
        });
        value = std::bit_cast<T>(static_cast<U>(result));
        return true;
    }
};

struct Writer {
    std::vector<std::byte> bytes;
    bool ok{true};

    void append(std::span<const std::byte> values) {
        if (values.size() > Bitmap::MaxFileBytes - bytes.size()) {
            ok = false;
            return;
        }
        bytes.insert(bytes.end(), values.begin(), values.end());
    }

    template <class T> void write(T value) {
        using U = std::make_unsigned_t<T>;
        const auto bits = std::bit_cast<U>(value);
        std::array<std::byte, sizeof(T)> raw{};
        std::ranges::for_each(std::views::iota(std::size_t{0}, sizeof(T)), [&](auto i) {
            raw[i] = static_cast<std::byte>((UInt64(bits) >> (i * 8)) & 255U);
        });
        append(raw);
    }

    void chunk(UInt32 id, UInt32 size) {
        write(id);
        write(size);
    }
};

std::optional<std::vector<std::byte>> readFile(StreamIO& stream, UInt32 size) {
    if (size < 8 || size > Bitmap::MaxFileBytes) {
        return {};
    }
    std::vector<std::byte> bytes(size);
    if (!stream.readBytes(bytes)) {
        return {};
    }
    return bytes;
}

struct Layout {
    std::size_t offset{}, size{}, stride{};
    Int32 width{}, height{};
};

std::optional<Layout> layout(const Bitmap& bitmap, Int32 level) {
    if (bitmap.width <= 0 || bitmap.width > Bitmap::MaxDimension || bitmap.height <= 0 ||
        bitmap.height > Bitmap::MaxDimension ||
        (bitmap.bitDepth != 8 && bitmap.bitDepth != 16 && bitmap.bitDepth != 24) ||
        bitmap.detailLevels < 1 || bitmap.detailLevels > Bitmap::MaxDetailLevels || level < 0 ||
        level >= bitmap.detailLevels) {
        return {};
    }
    if (bitmap.detailLevels > 1 &&
        (bitmap.bitDepth != 8 || bitmap.width < 4 || !std::has_single_bit(UInt32(bitmap.width)) ||
         !std::has_single_bit(UInt32(bitmap.height)) ||
         bitmap.detailLevels >
             Int32(std::bit_width(UInt32(std::max(bitmap.width, bitmap.height)))))) {
        return {};
    }
    Layout result{0, 0, 0, std::max(1, bitmap.width >> level), std::max(1, bitmap.height >> level)};
    result.stride =
        bitmap.detailLevels == 1 ? std::size_t(bitmap.stride()) : std::size_t(result.width);
    result.size = result.stride * std::size_t(result.height);
    // Quantization tools keep shrinking the longer axis after the shorter one
    // reaches one texel. The original generic reader incorrectly divided the
    // area by four even after that point.
    std::ranges::for_each(std::views::iota(Int32{0}, level), [&](auto i) {
        result.offset += std::size_t(std::max(1, bitmap.width >> i)) *
                         std::size_t(std::max(1, bitmap.height >> i));
    });
    if (result.offset + result.size > Bitmap::MaxFileBytes) {
        return {};
    }
    return result;
}

bool pixelLayoutValid(const Bitmap& bitmap) {
    if (bitmap.detailLevels < 1 || bitmap.detailLevels > Bitmap::MaxDetailLevels) {
        return false;
    }
    const auto last = layout(bitmap, bitmap.detailLevels - 1);
    if (!last) {
        return false;
    }
    const auto size = last->offset + last->size;
    // Historical mip tools added up to three trailing bytes, excluded from levels.
    return bitmap.pixels.size() >= size &&
           bitmap.pixels.size() <= size + (bitmap.detailLevels > 1 ? 3U : 0U);
}

std::unique_ptr<Palette> readPalette(std::span<const std::byte> bytes) {
    MemRWStream input(bytes);
    return Palette::load(input, static_cast<UInt32>(bytes.size()));
}

void writePalette(Writer& output, const Palette& palette, bool microsoft) {
    if (!palette.valid()) {
        output.ok = false;
        return;
    }
    std::size_t size = microsoft ? 1048 : 57 + palette.palettes.size() * 1032;
    if (!microsoft) {
        std::ranges::for_each(palette.palettes, [&](const auto& sub) {
            size += sub.remap.size();
            if (sub.type == PaletteType::ShadeHaze || sub.type == PaletteType::Translucent ||
                sub.type == PaletteType::Additive || sub.type == PaletteType::Subtractive) {
                size += 4352;
            }
        });
        if (palette.weights) {
            size += 1032;
        }
    }
    if (size > Bitmap::MaxFileBytes) {
        output.ok = false;
        return;
    }
    std::vector<std::byte> bytes(size);
    MemRWStream stream{std::span(bytes)};
    if (!palette.write(stream, microsoft)) {
        output.ok = false;
        return;
    }
    bytes.resize(static_cast<std::size_t>(stream.getPosition()));
    output.append(bytes);
}

bool readDarkstarBitmap(Reader& input, Bitmap& bitmap, bool includePalette) {
    UInt32 magic{}, advisorySize{}, tag{}, size{}, chunks{};
    if (!input.read(magic) || magic != pbmp || !input.read(advisorySize) || !input.read(tag) ||
        tag != head || !input.read(size) || size != 20 || !input.read(chunks) || chunks > 64 ||
        chunks == 0 || !input.read(bitmap.width) || !input.read(bitmap.height) ||
        !input.read(bitmap.bitDepth) || !input.read(bitmap.attributes)) {
        return false;
    }
    bool pixels = false, details = false, index = false, palette = false;
    for (UInt32 i = 0; i < chunks; ++i) {
        if (!input.read(tag) || !input.read(size)) {
            return false;
        }
        const auto start = input.position;
        const auto raw = input.take(size);
        if (!input.ok) {
            return false;
        }
        Reader chunk{raw};
        if (tag == dataTag) {
            if (pixels) {
                return false;
            }
            bitmap.pixels.assign(raw.begin(), raw.end());
            pixels = true;
        } else if (tag == detailTag) {
            if (details || size != 4 || !chunk.read(bitmap.detailLevels)) {
                return false;
            }
            details = true;
        } else if (tag == indexTag) {
            if (index || size != 4 || !chunk.read(bitmap.paletteIndex)) {
                return false;
            }
            index = true;
        } else if (tag == riff) {
            if (palette) {
                return false;
            }
            palette = true;
            if (includePalette) {
                auto value = readPalette(input.bytes.subspan(start - 8, std::size_t(size) + 8));
                if (!value) {
                    return false;
                }
                bitmap.palette.emplace(std::move(*value));
            } else if (!readPalette(input.bytes.subspan(start - 8, std::size_t(size) + 8))) {
                return false;
            }
        } else if (tag == head || tag == pbmp) {
            return false;
        }
    }
    // The original outer size omits DETL/PiDX and even four head bytes; chunk
    // count plus individually bounded chunks determine the actual extent.
    return pixels && bitmap.valid();
}

bool readMicrosoftBitmap(Reader& input, Bitmap& bitmap, bool includePalette) {
    UInt16 signature{}, reserved1{}, reserved2{}, planes{}, depth{};
    UInt32 fileSize{}, offset{}, headerSize{}, compression{}, imageSize{}, used{}, important{};
    Int32 signedHeight{}, xppm{}, yppm{};
    if (!input.read(signature) || signature != 0x4d42 || !input.read(fileSize) ||
        !input.read(reserved1) || !input.read(reserved2) || !input.read(offset) ||
        !input.read(headerSize) || headerSize < 40 || headerSize > input.bytes.size() - 14 ||
        !input.read(bitmap.width) || !input.read(signedHeight) ||
        signedHeight == std::numeric_limits<Int32>::min() || !input.read(planes) || planes != 1 ||
        !input.read(depth) || (depth != 8 && depth != 24) || !input.read(compression) ||
        compression != 0 || !input.read(imageSize) || !input.read(xppm) || !input.read(yppm) ||
        !input.read(used) || !input.read(important)) {
        return false;
    }
    bitmap.height = signedHeight < 0 ? -signedHeight : signedHeight;
    bitmap.bitDepth = depth;
    if (reserved1 == 0xf5f7 && reserved2 != 0xffff) {
        bitmap.paletteIndex = reserved2;
    }
    const auto base = layout(bitmap, 0);
    if (!base || base->size > input.bytes.size() || (imageSize != 0 && imageSize != base->size) ||
        used > 256) {
        return false;
    }
    const UInt32 colors = depth == 8 ? (used == 0 ? 256 : used) : used;
    const UInt64 colorEnd = UInt64{14} + headerSize + UInt64{4} * colors;
    if (colorEnd > offset || UInt64(offset) + base->size > input.bytes.size() ||
        fileSize != input.bytes.size()) {
        return false;
    }
    input.take(headerSize - 40);
    if (depth == 8) {
        Palette value;
        std::ranges::for_each(std::views::iota(UInt32{0}, colors), [&](auto i) {
            auto& c = value.palettes[0].colors[i];
            UInt8 ignored{};
            input.read(c.blue);
            input.read(c.green);
            input.read(c.red);
            input.read(ignored);
        });
        // Use the palette codec to build matching auxiliary maps consistently.
        if (includePalette) {
            Writer encoded;
            writePalette(encoded, value, true);
            auto decoded = readPalette(encoded.bytes);
            if (!decoded) {
                return false;
            }
            bitmap.palette.emplace(std::move(*decoded));
        }
    }
    if (!input.ok || input.position > offset) {
        return false;
    }
    input.take(offset - input.position);
    const auto raw = input.take(base->size);
    bitmap.pixels.assign(raw.begin(), raw.end());
    input.take(input.remaining()); // explicitly declared BMP trailer/profile bytes
    if (signedHeight > 0 && !bitmap.flipVertical()) {
        return false;
    }
    return input.ok && bitmap.valid();
}

UInt32 oldBitmapSize(const Bitmap& bitmap) {
    return 32U + static_cast<UInt32>(bitmap.pixels.size()) + (bitmap.palette ? 1048U : 0U);
}

void writeDarkstarBitmap(Writer& output, const Bitmap& bitmap, bool includePalette) {
    output.chunk(pbmp, oldBitmapSize(bitmap));
    output.chunk(head, 20);
    output.write(UInt32{3} + UInt32(includePalette && bool(bitmap.palette)));
    output.write(bitmap.width);
    output.write(bitmap.height);
    output.write(bitmap.bitDepth);
    output.write(bitmap.attributes);
    if (includePalette && bitmap.palette) {
        writePalette(output, *bitmap.palette, true);
    }
    output.chunk(dataTag, static_cast<UInt32>(bitmap.pixels.size()));
    output.append(bitmap.pixels);
    output.chunk(detailTag, 4);
    output.write(bitmap.detailLevels);
    output.chunk(indexTag, 4);
    output.write(bitmap.paletteIndex);
}

void writeMicrosoftBitmap(Writer& output, const Bitmap& bitmap) {
    if (bitmap.bitDepth != 8 && bitmap.bitDepth != 24) {
        output.ok = false;
        return;
    }
    if (bitmap.paletteIndex != 0xffffffffU && bitmap.paletteIndex >= 65534) {
        output.ok = false;
        return;
    }
    const auto offset = UInt32{54} + (bitmap.bitDepth == 8 ? UInt32{1024} : 0U);
    const auto imageSize = UInt32(bitmap.stride()) * UInt32(bitmap.height);
    output.write(UInt16{0x4d42});
    output.write(offset + imageSize);
    output.write(UInt16(bitmap.paletteIndex == 0xffffffffU ? 0 : 0xf5f7));
    output.write(UInt16(bitmap.paletteIndex == 0xffffffffU ? 0 : bitmap.paletteIndex));
    output.write(offset);
    output.write(UInt32{40});
    output.write(bitmap.width);
    output.write(bitmap.height);
    output.write(UInt16{1});
    output.write(UInt16(bitmap.bitDepth));
    output.write(UInt32{0});
    output.write(imageSize);
    std::ranges::for_each(std::views::iota(0, 4), [&](auto) {
        output.write(UInt32{0});
    });
    if (bitmap.bitDepth == 8) {
        std::ranges::for_each(std::views::iota(0U, 256U), [&](auto i) {
            const auto c = bitmap.palette ? bitmap.palette->palettes.front().colors[i]
                                          : PaletteColor{UInt8(i), UInt8(i), UInt8(i), 0};
            output.write(c.blue);
            output.write(c.green);
            output.write(c.red);
            output.write(UInt8{0});
        });
    }
    // Never invert or discard the source's mip levels when exporting a BMP.
    const auto stride = std::size_t(bitmap.stride());
    std::ranges::for_each(std::views::iota(Int32{0}, bitmap.height), [&](auto y) {
        const auto row = bitmap.row(bitmap.height - 1 - y);
        output.append(row);
        const auto padding = stride - row.size();
        if (bitmap.detailLevels == 1) {
            output.append(
                std::span(bitmap.pixels)
                    .subspan(std::size_t(bitmap.height - 1 - y) * stride + row.size(), padding));
        } else {
            std::array<std::byte, 3> zeros{};
            output.append(std::span(zeros).first(padding));
        }
    });
}

bool readArray(Reader& input, BitmapArray& array, bool includePalette) {
    UInt32 magic{}, advisorySize{}, tag{}, size{}, chunks{}, count{};
    if (!input.read(magic) || magic != pbma || !input.read(advisorySize) || !input.read(tag) ||
        tag != head || !input.read(size) || size != 8 || !input.read(chunks) ||
        chunks > BitmapArray::MaxBitmaps + 64 || !input.read(count) ||
        count > BitmapArray::MaxBitmaps || count > input.remaining() / 36) {
        return false;
    }
    bool user = false, remap = false;
    for (UInt32 i = 0; i < chunks; ++i) {
        const auto start = input.position;
        if (!input.read(tag) || !input.read(size)) {
            return false;
        }
        if (tag == pbmp) {
            if (array.bitmaps.size() >= count) {
                return false;
            }
            input.position = start;
            Bitmap bitmap;
            if (!readDarkstarBitmap(input, bitmap, includePalette)) {
                return false;
            }
            array.bitmaps.push_back(std::move(bitmap));
        } else {
            auto raw = input.take(size);
            if (!input.ok) {
                return false;
            }
            if (tag == userTag) {
                if (user || count == 0 || size == 0 || size % count != 0) {
                    return false;
                }
                user = true;
                array.userDataSize = size / count;
                array.userData.assign(raw.begin(), raw.end());
            } else if (tag == remapTag) {
                if (remap || count == 0 || size != UInt64{4} * count) {
                    return false;
                }
                remap = true;
                array.indexRemap.resize(count);
                Reader mapping{raw};
                if (!std::ranges::all_of(array.indexRemap, [&](auto& v) {
                        return mapping.read(v);
                    })) {
                    return false;
                }
            } else if (tag == head || tag == pbma) {
                return false;
            }
        }
    }
    return array.bitmaps.size() == count && array.valid();
}

void writeArray(Writer& output, const BitmapArray& array, bool includePalette) {
    UInt64 advisorySize = 16 + (array.indexRemap.empty() ? 0 : 8 + 4 * array.indexRemap.size()) +
                          (array.userData.empty() ? 0 : 8 + array.userData.size());
    std::ranges::for_each(array.bitmaps, [&](const auto& bitmap) {
        advisorySize += oldBitmapSize(bitmap);
    });
    output.chunk(pbma, static_cast<UInt32>(advisorySize));
    output.chunk(head, 8);
    output.write(UInt32(array.bitmaps.size()) + UInt32(!array.userData.empty()) +
                 UInt32(!array.indexRemap.empty()));
    output.write(UInt32(array.bitmaps.size()));
    if (!array.userData.empty()) {
        output.chunk(userTag, UInt32(array.userData.size()));
        output.append(array.userData);
    }
    if (!array.indexRemap.empty()) {
        output.chunk(remapTag, UInt32(4 * array.indexRemap.size()));
        std::ranges::for_each(array.indexRemap, [&](auto v) {
            output.write(v);
        });
    }
    std::ranges::for_each(array.bitmaps, [&](const auto& bitmap) {
        writeDarkstarBitmap(output, bitmap, includePalette);
    });
}

std::optional<std::size_t> realIndex(const BitmapArray& array, Int32 index) noexcept {
    if (index < 0) {
        return {};
    }
    if (array.indexRemap.empty()) {
        return std::size_t(index) < array.bitmaps.size() ? std::optional(std::size_t(index))
                                                         : std::nullopt;
    }
    const auto found = std::ranges::lower_bound(array.indexRemap, index);
    const auto offset = std::size_t(found - array.indexRemap.begin());
    return found != array.indexRemap.end() && *found == index && offset < array.bitmaps.size()
               ? std::optional(offset)
               : std::nullopt;
}

Int64 scaled(Int64 value, Int32 scale) {
    // Floor at the fixed-point boundary, matching the original arithmetic shift.
    const auto product = value * scale;
    return product / 65536 - (product < 0 && product % 65536 != 0 ? 1 : 0);
}

template <class Text> Int64 measure(const BitmapFont& font, Text text, bool height) {
    // A bounded engine string avoids unbounded accumulation and multiplication.
    if (text.size() > 1024U * 1024U) {
        throw std::length_error("font text exceeds 1 Mi code units");
    }
    if (font.info.width < 0 || font.info.width > 16384 || font.info.spacing < -16384 ||
        font.info.spacing > 16384 || font.info.scale.x < 0 || font.info.scale.y < 0 ||
        font.info.scale.x > 64 * 65536 || font.info.scale.y > 64 * 65536) {
        throw std::invalid_argument("invalid font metrics");
    }
    Int64 total = height ? 0 : -Int64(font.info.spacing);
    for (const auto value : text) {
        using C = std::make_unsigned_t<typename Text::value_type>;
        const auto code = UInt32(static_cast<C>(value));
        if (code == 0) {
            break;
        }
        const auto glyph = font.glyph(code);
        if (height) {
            total = std::max(total, Int64(glyph.info.height));
        } else {
            total += ((font.info.flags & FontFixed) ? font.info.width : glyph.info.width) +
                     Int64(font.info.spacing);
        }
    }
    return scaled(total, height ? font.info.scale.y : font.info.scale.x);
}
} // namespace

bool RectI::contains(Point2I p) const noexcept {
    return p.x >= left && p.x <= right && p.y >= top && p.y <= bottom;
}

std::optional<RectI> intersect(RectI a, RectI b) noexcept {
    RectI result{std::max(a.left, b.left),
                 std::max(a.top, b.top),
                 std::min(a.right, b.right),
                 std::min(a.bottom, b.bottom)};
    return result.valid() ? std::optional(result) : std::nullopt;
}

bool clipBlit(RectI& source, RectI sourceClip, RectI& destination, RectI destinationClip) noexcept {
    if (!source.valid() || !destination.valid() || !sourceClip.valid() ||
        !destinationClip.valid() || source.width() != destination.width() ||
        source.height() != destination.height()) {
        return false;
    }
    const auto dx = Int64(destination.left) - source.left, dy = Int64(destination.top) - source.top;
    const auto left =
        std::max({Int64(source.left), Int64(sourceClip.left), Int64(destinationClip.left) - dx});
    const auto top =
        std::max({Int64(source.top), Int64(sourceClip.top), Int64(destinationClip.top) - dy});
    const auto right =
        std::min({Int64(source.right), Int64(sourceClip.right), Int64(destinationClip.right) - dx});
    const auto bottom = std::min(
        {Int64(source.bottom), Int64(sourceClip.bottom), Int64(destinationClip.bottom) - dy});
    if (left > right || top > bottom) {
        return false;
    }
    source = {Int32(left), Int32(top), Int32(right), Int32(bottom)};
    destination = {Int32(left + dx), Int32(top + dy), Int32(right + dx), Int32(bottom + dy)};
    return true;
}

bool clipLine(Point2I& first, Point2I& second, RectI clip) noexcept {
    if (!clip.valid()) {
        return false;
    }
    // Preserve the legacy X-then-Y clipping and float/truncation policy; widen
    // differences before converting so extreme coordinates cannot overflow.
    Int64 x1 = first.x, y1 = first.y, x2 = second.x, y2 = second.y;
    const auto rejected = [&] {
        return (x1 > clip.right && x2 > clip.right) || (x1 < clip.left && x2 < clip.left) ||
               (y1 > clip.bottom && y2 > clip.bottom) || (y1 < clip.top && y2 < clip.top);
    };
    if (rejected()) {
        return false;
    }
    bool flipped = false;
    if (x1 > x2) {
        std::swap(x1, x2);
        std::swap(y1, y2);
        flipped = !flipped;
    }
    if (x1 < clip.left) {
        const auto t = Flt32(Int64(clip.left) - x1) / Flt32(x2 - x1);
        x1 = clip.left;
        y1 += Int64(t * Flt32(y2 - y1));
    }
    if (x2 > clip.right) {
        const auto t = Flt32(Int64(clip.right) - x1) / Flt32(x2 - x1);
        x2 = clip.right;
        y2 = Int64(Flt32(y1) + t * Flt32(y2 - y1));
    }
    if (rejected()) {
        return false;
    }
    if (y1 > y2) {
        std::swap(x1, x2);
        std::swap(y1, y2);
        flipped = !flipped;
    }
    if (y1 < clip.top) {
        const auto t = Flt32(Int64(clip.top) - y1) / Flt32(y2 - y1);
        y1 = clip.top;
        x1 += Int64(t * Flt32(x2 - x1));
    }
    if (y2 > clip.bottom) {
        const auto t = Flt32(Int64(clip.bottom) - y1) / Flt32(y2 - y1);
        y2 = clip.bottom;
        x2 = Int64(Flt32(x1) + t * Flt32(x2 - x1));
    }
    x1 = std::clamp(x1, Int64(clip.left), Int64(clip.right));
    x2 = std::clamp(x2, Int64(clip.left), Int64(clip.right));
    y1 = std::clamp(y1, Int64(clip.top), Int64(clip.bottom));
    y2 = std::clamp(y2, Int64(clip.top), Int64(clip.bottom));
    Point2I a{Int32(x1), Int32(y1)}, b{Int32(x2), Int32(y2)};
    first = flipped ? b : a;
    second = flipped ? a : b;
    return true;
}

Int32 Bitmap::stride() const noexcept {
    if (width <= 0 || width > MaxDimension || (bitDepth != 8 && bitDepth != 16 && bitDepth != 24)) {
        return 0;
    }
    return (width * (bitDepth / 8) + 3) & ~3;
}

std::unique_ptr<Bitmap> Bitmap::create(Int32 w, Int32 h, Int32 depth) {
    auto bitmap = std::make_unique<Bitmap>();
    bitmap->width = w;
    bitmap->height = h;
    bitmap->bitDepth = depth;
    const auto base = layout(*bitmap, 0);
    if (!base) {
        return {};
    }
    bitmap->pixels.resize(base->size);
    return bitmap;
}

bool Bitmap::valid() const {
    return pixelLayoutValid(*this) && (!palette || palette->valid());
}

std::unique_ptr<Bitmap> Bitmap::load(StreamIO& stream, UInt32 size, bool includePalette) {
    auto bytes = readFile(stream, size);
    if (!bytes) {
        return {};
    }
    Reader input{*bytes};
    UInt32 magic{};
    if (!input.read(magic)) {
        return {};
    }
    input.position = 0;
    auto bitmap = std::make_unique<Bitmap>();
    const bool ok = magic == pbmp ? readDarkstarBitmap(input, *bitmap, includePalette)
                                  : readMicrosoftBitmap(input, *bitmap, includePalette);
    return ok && input.remaining() == 0 ? std::move(bitmap) : nullptr;
}

std::span<const std::byte> Bitmap::level(Int32 detail) const {
    const auto part = layout(*this, detail);
    if (!part || part->offset + part->size > pixels.size()) {
        return {};
    }
    return std::span(pixels).subspan(part->offset, part->size);
}

std::span<std::byte> Bitmap::level(Int32 detail) {
    const auto part = std::as_const(*this).level(detail);
    return part.empty()
               ? std::span<std::byte>{}
               : std::span(pixels).subspan(std::size_t(part.data() - pixels.data()), part.size());
}

std::span<const std::byte> Bitmap::row(Int32 y, Int32 detail) const {
    const auto part = layout(*this, detail);
    const auto bytes = level(detail);
    if (!part || y < 0 || y >= part->height || bytes.empty()) {
        return {};
    }
    return bytes.subspan(std::size_t(y) * part->stride,
                         std::size_t(part->width) * std::size_t(bitDepth / 8));
}

std::span<std::byte> Bitmap::row(Int32 y, Int32 detail) {
    const auto part = std::as_const(*this).row(y, detail);
    return part.empty()
               ? std::span<std::byte>{}
               : std::span(pixels).subspan(std::size_t(part.data() - pixels.data()), part.size());
}

std::optional<RectI> indexedContentBounds(const Bitmap& bitmap) {
    if (bitmap.bitDepth != 8 || !bitmap.valid()) {
        return {};
    }
    RectI result{bitmap.width, bitmap.height, -1, -1};
    for (Int32 y = 0; y < bitmap.height; ++y) {
        const auto row = bitmap.row(y);
        std::size_t first = 0, last = row.size();
        // Word loads skip sparse zero spans without alignment assumptions or
        // reading padding/beyond the row. Boundary bytes keep this endian-neutral.
        while (row.size() - first >= sizeof(UInt64)) {
            UInt64 word{};
            std::memcpy(&word, row.data() + first, sizeof(word));
            if (word) {
                break;
            }
            first += sizeof(word);
        }
        while (first < row.size() && row[first] == std::byte{}) {
            ++first;
        }
        if (first == row.size()) {
            continue;
        }
        while (last - first >= sizeof(UInt64)) {
            UInt64 word{};
            std::memcpy(&word, row.data() + last - sizeof(word), sizeof(word));
            if (word) {
                break;
            }
            last -= sizeof(word);
        }
        while (last > first && row[last - 1] == std::byte{}) {
            --last;
        }
        result.left = std::min(result.left, Int32(first));
        result.right = std::max(result.right, Int32(last - 1));
        result.top = std::min(result.top, y);
        result.bottom = y;
    }
    return result;
}

std::unique_ptr<Bitmap> Bitmap::crop(RectI region) const {
    if (!valid() || !region.valid() || !bounds().contains({region.left, region.top}) ||
        !bounds().contains({region.right, region.bottom})) {
        return {};
    }
    auto result = create(Int32(region.width()), Int32(region.height()), bitDepth);
    if (!result) {
        return {};
    }
    result->attributes = attributes;
    result->paletteIndex = paletteIndex;
    result->palette = palette;
    std::ranges::for_each(std::views::iota(Int32{0}, result->height), [&](auto y) {
        std::ranges::copy(row(y + region.top)
                              .subspan(std::size_t(region.left) * std::size_t(bitDepth / 8),
                                       result->row(y).size()),
                          result->row(y).begin());
    });
    return result;
}

bool Bitmap::flipVertical() {
    if (!pixelLayoutValid(*this)) {
        return false;
    }
    std::ranges::for_each(std::views::iota(Int32{0}, detailLevels), [&](auto detail) {
        const auto part = *layout(*this, detail);
        auto bytes = level(detail);
        std::ranges::for_each(std::views::iota(Int32{0}, part.height / 2), [&](auto y) {
            std::ranges::swap_ranges(
                bytes.subspan(std::size_t(y) * part.stride, part.stride),
                bytes.subspan(std::size_t(part.height - 1 - y) * part.stride, part.stride));
        });
    });
    return true;
}

bool Bitmap::write(StreamIO& stream, BitmapFormat format, bool includePalette) const {
    if (!valid()) {
        return false;
    }
    Writer output;
    if (format == BitmapFormat::Microsoft) {
        writeMicrosoftBitmap(output, *this);
    } else if (format == BitmapFormat::Darkstar) {
        writeDarkstarBitmap(output, *this, includePalette);
    } else {
        return false;
    }
    return output.ok && stream.writeBytes(output.bytes);
}

bool BitmapArray::valid() const {
    if (bitmaps.size() > MaxBitmaps || userData.size() > Bitmap::MaxFileBytes ||
        userData.size() != UInt64(userDataSize) * bitmaps.size() ||
        (!indexRemap.empty() && (indexRemap.size() != bitmaps.size() || indexRemap.front() < 0 ||
                                 !std::ranges::is_sorted(indexRemap) ||
                                 std::ranges::adjacent_find(indexRemap) != indexRemap.end()))) {
        return false;
    }
    UInt64 total = 24 + userData.size() + UInt64{4} * indexRemap.size();
    return std::ranges::all_of(bitmaps, [&](const auto& bitmap) {
        total += 68 + bitmap.pixels.size() + (bitmap.palette ? 1048U : 0U);
        return total <= Bitmap::MaxFileBytes && bitmap.valid();
    });
}

std::unique_ptr<BitmapArray> BitmapArray::load(StreamIO& stream, UInt32 size, bool includePalette) {
    auto bytes = readFile(stream, size);
    if (!bytes) {
        return {};
    }
    Reader input{*bytes};
    auto array = std::make_unique<BitmapArray>();
    return readArray(input, *array, includePalette) && input.remaining() == 0 ? std::move(array)
                                                                              : nullptr;
}

const Bitmap* BitmapArray::get(Int32 index) const noexcept {
    const auto real = realIndex(*this, index);
    return real ? &bitmaps[*real] : nullptr;
}

std::span<const std::byte> BitmapArray::data(Int32 index) const noexcept {
    const auto real = realIndex(*this, index);
    const auto offset = real ? UInt64(*real) * userDataSize : UInt64(userData.size());
    if (!real || offset > userData.size() || userDataSize > userData.size() - offset) {
        return {};
    }
    return std::span(userData).subspan(std::size_t(offset), userDataSize);
}

bool BitmapArray::write(StreamIO& stream, bool includePalette) const {
    if (!valid()) {
        return false;
    }
    Writer output;
    writeArray(output, *this, includePalette);
    return output.ok && stream.writeBytes(output.bytes);
}

std::unique_ptr<BitmapFont> BitmapFont::load(StreamIO& stream, UInt32 size) {
    auto bytes = readFile(stream, size);
    if (!bytes) {
        return {};
    }
    Reader input{*bytes};
    UInt32 magic{}, blockSize{}, version{};
    if (!input.read(magic) || magic != pfon || !input.read(blockSize)) {
        return {};
    }
    const UInt32 alignment = (blockSize & 0x80000000U) ? 3U : 1U;
    blockSize &= 0x7fffffffU;
    if (UInt64{8} + ((UInt64(blockSize) + alignment) & ~UInt64(alignment)) != size) {
        return {};
    }
    Reader body{input.take(blockSize)};
    if (!body.ok || !body.read(version) || version != 2) {
        return {};
    }
    auto font = std::make_unique<BitmapFont>();
    auto& f = font->info;
    Int32 count{};
    Int16 lookupSize{};
    if (!body.read(f.flags) || !body.read(f.justification) || !body.read(count) || count < 0 ||
        count > 32767 || !body.read(f.height) || !body.read(f.width) || !body.read(f.foreColor) ||
        !body.read(f.backColor) || !body.read(f.baseline) || !body.read(f.scale.x) ||
        !body.read(f.scale.y) || !body.read(f.spacing) || !body.read(lookupSize) ||
        lookupSize < 0 || !body.read(font->lookupOffset) ||
        UInt64(lookupSize) * 2 + UInt64(count) * 8 > body.remaining()) {
        return {};
    }
    font->lookup.resize(std::size_t(lookupSize));
    font->characters.resize(std::size_t(count));
    if (!std::ranges::all_of(font->lookup,
                             [&](auto& v) {
                                 return body.read(v);
                             }) ||
        !std::ranges::all_of(font->characters,
                             [&](auto& c) {
                                 return body.read(c.bitmap) && body.read(c.x) && body.read(c.y) &&
                                        body.read(c.width) && body.read(c.height) &&
                                        body.read(c.baseline) && body.read(c.padding[0]) &&
                                        body.read(c.padding[1]);
                             }) ||
        !readArray(body, font->sheets, false)) {
        return {};
    }
    UInt32 attached{};
    if (!body.read(attached) || attached > 1) {
        return {};
    }
    if (attached) {
        auto palette = readPalette(body.take(body.remaining()));
        if (!palette) {
            return {};
        }
        font->palette.emplace(std::move(*palette));
    }
    return body.ok && body.remaining() == 0 && font->valid() ? std::move(font) : nullptr;
}

bool BitmapFont::valid() const {
    if (characters.size() > 32767 || lookup.size() > 32767 || info.width < 0 || info.height < 0 ||
        info.width > 16384 || info.height > 16384 || info.scale.x < 0 || info.scale.y < 0 ||
        info.scale.x > 64 * 65536 || info.scale.y > 64 * 65536 || info.spacing < -16384 ||
        info.spacing > 16384 || info.foreColor < 0 || info.foreColor > 255 || info.backColor < 0 ||
        info.backColor > 255 || !sheets.valid() || (palette && !palette->valid()) ||
        !std::ranges::all_of(sheets.bitmaps,
                             [](const auto& bitmap) {
                                 return bitmap.bitDepth == 8;
                             }) ||
        !std::ranges::all_of(lookup, [&](auto index) {
            return index == -1 || (index >= 0 && std::size_t(index) < characters.size());
        })) {
        return false;
    }
    return std::ranges::all_of(characters, [&](const auto& c) {
        return c.bitmap < sheets.bitmaps.size() &&
               UInt32(c.x) + c.width <= UInt32(sheets.bitmaps[c.bitmap].width) &&
               UInt32(c.y) + c.height <= UInt32(sheets.bitmaps[c.bitmap].height);
    });
}

Glyph BitmapFont::glyph(UInt32 code) const noexcept {
    if ((info.flags & FontLowerCaps) && code >= 'a' && code <= 'z') {
        code -= 'a' - 'A';
    }
    const auto position = Int64(code) - lookupOffset;
    if (position >= 0 && UInt64(position) < lookup.size()) {
        const auto index = lookup[std::size_t(position)];
        if (index >= 0 && std::size_t(index) < characters.size()) {
            const auto c = characters[std::size_t(index)];
            if (c.bitmap < sheets.bitmaps.size()) {
                return {c,
                        &sheets.bitmaps[c.bitmap],
                        {c.x, c.y, Int32(c.x) + c.width - 1, Int32(c.y) + c.height - 1}};
            }
        }
    }
    const auto width = std::max(UInt8(info.width), UInt8{5}),
               height = std::max(UInt8(info.height), UInt8{1});
    return {{0, 0, 0, width, height, 0, {}}, nullptr, {0, 0, width - 1, height - 1}};
}

Int64 BitmapFont::textWidth(std::string_view text) const {
    return measure(*this, text, false);
}

Int64 BitmapFont::textWidth(std::u16string_view text) const {
    return measure(*this, text, false);
}

Int64 BitmapFont::textHeight(std::string_view text) const {
    return measure(*this, text, true);
}

Int64 BitmapFont::textHeight(std::u16string_view text) const {
    return measure(*this, text, true);
}

void BitmapFont::remapColors(std::span<const UInt8, 256> mapping) {
    std::ranges::for_each(sheets.bitmaps, [&](auto& bitmap) {
        std::ranges::transform(bitmap.pixels, bitmap.pixels.begin(), [&](auto value) {
            return std::byte(mapping[std::to_integer<UInt8>(value)]);
        });
    });
    info.backColor = 0;
}

bool BitmapFont::setForeColor(UInt8 color) {
    if (!(info.flags & FontMono)) {
        return false;
    }
    // Legacy foreground replacement treats every nonzero pixel as foreground.
    std::ranges::for_each(sheets.bitmaps, [&](auto& bitmap) {
        std::ranges::replace_if(
            bitmap.pixels,
            [](auto value) {
                return value != std::byte{};
            },
            std::byte(color));
    });
    info.foreColor = color;
    return true;
}

bool BitmapFont::setBackColor(UInt8 color) {
    if (!(info.flags & FontMono)) {
        return false;
    }
    std::ranges::for_each(sheets.bitmaps, [&](auto& bitmap) {
        std::ranges::replace(bitmap.pixels, std::byte(info.backColor), std::byte(color));
    });
    info.backColor = color;
    return true;
}

bool BitmapFont::write(StreamIO& stream) const {
    if (!valid()) {
        return false;
    }
    Writer body;
    body.write(Int32{2});
    body.write(info.flags);
    body.write(info.justification);
    body.write(Int32(characters.size()));
    body.write(info.height);
    body.write(info.width);
    body.write(info.foreColor);
    body.write(info.backColor);
    body.write(info.baseline);
    body.write(info.scale.x);
    body.write(info.scale.y);
    body.write(info.spacing);
    body.write(Int16(lookup.size()));
    body.write(lookupOffset);
    std::ranges::for_each(lookup, [&](auto v) {
        body.write(v);
    });
    std::ranges::for_each(characters, [&](const auto& c) {
        body.write(c.bitmap);
        body.write(c.x);
        body.write(c.y);
        body.write(c.width);
        body.write(c.height);
        body.write(c.baseline);
        body.write(c.padding[0]);
        body.write(c.padding[1]);
    });
    writeArray(body, sheets, false);
    body.write(UInt32(bool(palette)));
    if (palette) {
        writePalette(body, *palette, false);
    }
    if (!body.ok) {
        return false;
    }
    Writer output;
    output.chunk(pfon, UInt32(body.bytes.size()));
    output.append(body.bytes);
    if (body.bytes.size() & 1U) {
        output.write(UInt8{0});
    }
    return output.ok && stream.writeBytes(output.bytes);
}

bool registerBitmapFontLoaders(ResourceManager& resources) {
    const auto bitmap = [](StreamIO& s, UInt32 size) {
        return Bitmap::load(s, size);
    };
    const bool bmp = resources.registerLoader<Bitmap>(".bmp", bitmap),
               dib = resources.registerLoader<Bitmap>(".dib", bitmap),
               pbm = resources.registerLoader<Bitmap>(".pbm", bitmap),
               png = resources.registerLoader<Bitmap>(".png", bitmap),
               pba = resources.registerLoader<BitmapArray>(".pba",
                                                           [](StreamIO& s, UInt32 size) {
                                                               return BitmapArray::load(s, size);
                                                           }),
               pft = resources.registerLoader<BitmapFont>(".pft", [](StreamIO& s, UInt32 size) {
                   return BitmapFont::load(s, size);
               });
    if (bmp)
        resources.setSourceOnlyLoader(".bmp");
    if (dib)
        resources.setSourceOnlyLoader(".dib");
    if (pbm)
        resources.setSourceOnlyLoader(".pbm");
    if (png)
        resources.setSourceOnlyLoader(".png");
    if (pba)
        resources.setSourceOnlyLoader(".pba");
    if (pft)
        resources.setSourceOnlyLoader(".pft");
    return bmp && dib && pbm && png && pba && pft;
}
} // namespace opent1::gfx
