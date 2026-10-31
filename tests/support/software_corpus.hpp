#pragma once
#include <array>
#include <cstdint>

namespace software_corpus {
inline constexpr int Width = 17, Height = 13, Stride = 20;
inline constexpr unsigned LineCases = 1024, BlitCases = 256;

inline std::array<int, 4> line(unsigned test) {
    auto seed = std::uint32_t(test + 213);
    const auto next = [&] {
        seed = seed * 1664525U + 1013904223U;
        return int((seed >> 16) % 41) - 12;
    };
    return {next(), next(), next(), next()};
}

inline unsigned char pixel(unsigned offset) {
    return static_cast<unsigned char>(offset % 7 == 0 ? 0 : (offset * 17) & 255);
}

inline unsigned char identity(unsigned value) {
    return static_cast<unsigned char>(255 - value);
}

inline unsigned char blend(unsigned source, unsigned dest) {
    return static_cast<unsigned char>((source * 3 + dest * 5) & 255);
}

inline std::array<int, 2> at(unsigned test) {
    return {int((test * 3) % 27) - 7, int((test * 5) % 23) - 6};
}

inline unsigned flags(unsigned test) {
    return test % 4 == 1 ? 1 : test % 4 == 3 ? 4 : 0;
}
} // namespace software_corpus
