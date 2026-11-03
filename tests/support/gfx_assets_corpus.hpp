#pragma once
#include <array>
#include <ranges>
#include <cstdint>
#include <string_view>

namespace gfx_assets_corpus {
inline constexpr unsigned BitmapCases = 28, FontCases = 16, ClipCases = 4096;

inline int width(unsigned test) {
    return test < 18 ? int(1 + test % 7) : test < 24 ? (test % 2 ? 16 : 8) : (test % 2 ? 32 : 64);
}

inline int height(unsigned test) {
    return test < 18 ? int(1 + (test * 3) % 9) : test < 24 ? 8 : (test % 2 ? 64 : 32);
}

inline int depth(unsigned test) {
    return test >= 18 ? 8 : (test % 3 == 0 ? 24 : test % 3 == 1 ? 8 : 16);
}

inline int levels(unsigned test) {
    return test < 18 ? 1 : test < 24 ? int(2 + test % 2) : 7;
}

inline unsigned imageSize(unsigned test) {
    const auto w = unsigned(width(test)), h = unsigned(height(test));
    if (levels(test) == 1) {
        return ((w * unsigned(depth(test) / 8) + 3) & ~3U) * h;
    }
    unsigned size{};
    for (auto i : std::views::iota(0) | std::views::take_while([&](int i) {
                      return i < levels(test);
                  })) {
        size += (w >> i ? w >> i : 1) * (h >> i ? h >> i : 1);
    }
    return size;
}

inline unsigned attributes(unsigned test) {
    return 8U | ((test * 5) & 0x37U) | (depth(test) == 16 ? 64U : 0U);
}

inline unsigned paletteIndex(unsigned test) {
    return test % 4 == 0 ? 0xffffffffU : 100U + test;
}

inline bool palette(unsigned test) {
    return depth(test) == 8 && test % 2 == 1;
}

inline unsigned char pixel(unsigned test, unsigned offset) {
    return static_cast<unsigned char>((test * 17 + offset * 37) & 255);
}

inline unsigned char color(unsigned seed, unsigned i, unsigned channel) {
    return static_cast<unsigned char>((seed * 31 + i * (channel * 2 + 1)) & 255);
}

inline unsigned fontFlags(unsigned test) {
    return (test & 1 ? 2U : 1U) | 4U | 8U | 64U | (test & 2 ? 1024U : 0U) | (test & 4 ? 512U : 0U);
}

inline constexpr std::array<std::string_view, 7> Texts{
    "", "A", "ABC", "a b", "?A!", " AB C ", "abca"};
inline constexpr std::array<std::u16string_view, 7> WideTexts{
    u"", u"A", u"ABC", u"a b", u"?A!", u" AB C ", u"abca"};

struct ClipInput {
    std::array<int, 4> rect;
    std::array<int, 2> first, second;
};

inline ClipInput clip(unsigned test) {
    auto seed = std::uint32_t(test + 1234);
    const auto next = [&]() {
        seed = seed * 1664525U + 1013904223U;
        return int((seed >> 16) % 151) - 75;
    };
    const int x = next(), y = next();
    return {{x, y, x + int(test % 27), y + int(test % 31)}, {next(), next()}, {next(), next()}};
}
} // namespace gfx_assets_corpus
