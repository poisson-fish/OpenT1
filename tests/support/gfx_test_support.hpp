#pragma once
#include "test_support.hpp"
#include "gfx_assets_corpus.hpp"
#include <opent1/gfx/bitmap_palette_font.hpp>
#include <opent1/core/mem_stream.hpp>
#include <algorithm>
#include <ranges>

namespace gfx_test {
using namespace opent1;
using namespace opent1::gfx;
namespace corpus = gfx_assets_corpus;

template <class T, class... Args> std::vector<std::byte> encode(const T& value, Args... args) {
    std::vector<std::byte> bytes(262144);
    MemRWStream stream{std::span(bytes)};
    CHECK(value.write(stream, args...));
    bytes.resize(std::size_t(stream.getPosition()));
    return bytes;
}

template <class T, class... Args>
std::unique_ptr<T> decode(std::span<const std::byte> bytes, Args... args) {
    MemRWStream stream(bytes);
    return T::load(stream, static_cast<UInt32>(bytes.size()), args...);
}

inline Palette palette(unsigned seed) {
    Palette value;
    std::ranges::for_each(std::views::iota(0U, 256U), [&](auto i) {
        value.palettes[0].colors[i] = {corpus::color(seed, i, 0),
                                       corpus::color(seed, i, 1),
                                       corpus::color(seed, i, 2),
                                       corpus::color(seed, i, 3)};
    });
    return value;
}

inline Bitmap bitmap(unsigned test) {
    Bitmap b;
    b.width = corpus::width(test);
    b.height = corpus::height(test);
    b.bitDepth = corpus::depth(test);
    b.attributes = corpus::attributes(test);
    b.paletteIndex = corpus::paletteIndex(test);
    b.detailLevels = corpus::levels(test);
    b.pixels.resize(corpus::imageSize(test));
    std::ranges::for_each(std::views::iota(std::size_t{0}, b.pixels.size()), [&](auto i) {
        b.pixels[i] = std::byte(corpus::pixel(test, unsigned(i)));
    });
    if (corpus::palette(test)) {
        b.palette = palette(test);
    }
    CHECK(b.valid());
    return b;
}

inline BitmapFont font(unsigned test) {
    BitmapFont f;
    f.info.flags = corpus::fontFlags(test);
    f.info.justification = 17;
    f.info.height = 8;
    f.info.width = 7;
    f.info.baseline = 6;
    f.info.scale = {test % 3 == 0 ? 98304 : 65536, test % 3 == 1 ? 32768 : 65536};
    f.info.spacing = int(test % 4) - 1;
    f.lookupOffset = 32;
    f.lookup.assign(68, -1);
    f.lookup[0] = 0;
    f.lookup[33] = f.lookup[65] = 1;
    f.lookup[34] = f.lookup[66] = 2;
    f.lookup[35] = f.lookup[67] = 3;
    std::ranges::for_each(std::views::iota(0U, 4U), [&](auto i) {
        f.characters.push_back(
            {0, UInt8(i * 4), UInt8(i), UInt8(i + 1), UInt8(i + 3), Int8(int(i) - 2), {}});
    });
    auto sheet = Bitmap::create(16, 16);
    CHECK(sheet);
    sheet->attributes |= BitmapTransparent;
    std::ranges::for_each(std::views::iota(std::size_t{0}, sheet->pixels.size()), [&](auto i) {
        sheet->pixels[i] = std::byte(corpus::pixel(test, unsigned(i)));
    });
    f.sheets.bitmaps.push_back(std::move(*sheet));
    if (test & 8) {
        f.palette = palette(test);
    }
    CHECK(f.valid());
    return f;
}

inline void setWord(std::vector<std::byte>& bytes, std::size_t offset, UInt32 value) {
    CHECK(offset + 4 <= bytes.size());
    std::ranges::for_each(std::views::iota(0U, 4U), [&](auto i) {
        bytes[offset + i] = std::byte((value >> (i * 8)) & 255U);
    });
}
} // namespace gfx_test
