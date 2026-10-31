#pragma once
#include <array>
#include <cstddef>
#include <cstdint>

// Deterministic collision inputs. Expected results are stored separately
// in the collision fixtures rather than calculated by the code under test.
namespace ts_collision_corpus {
constexpr std::size_t samples = 513, frames = 2, cases = samples * frames * 3;
constexpr std::array<std::array<std::uint8_t, 3>, 10> vertices{{{0, 0, 0},
                                                                {200, 200, 200},
                                                                {0, 0, 0},
                                                                {200, 0, 0},
                                                                {200, 200, 0},
                                                                {0, 200, 0},
                                                                {0, 0, 200},
                                                                {200, 0, 200},
                                                                {200, 200, 200},
                                                                {0, 200, 200}}};
constexpr std::array<std::array<int, 3>, 12> faces{{{2, 3, 4},
                                                    {2, 4, 5},
                                                    {6, 9, 8},
                                                    {6, 8, 7},
                                                    {2, 6, 7},
                                                    {2, 7, 3},
                                                    {5, 4, 8},
                                                    {5, 8, 9},
                                                    {2, 5, 9},
                                                    {2, 9, 6},
                                                    {3, 7, 8},
                                                    {3, 8, 4}}};

constexpr std::array<float, 3> scale(std::size_t frame) {
    return frame ? std::array<float, 3>{.015f, .008f, .012f}
                 : std::array<float, 3>{.01f, .01f, .01f};
}

constexpr std::array<float, 3> origin(std::size_t frame) {
    return frame ? std::array<float, 3>{-.7f, -.8f, -1.3f} : std::array<float, 3>{-1, -1, -1};
}

struct Input {
    std::array<float, 3> a, b;
    float radius;
};

constexpr Input input(std::size_t kind, std::size_t i) {
    std::uint32_t state = std::uint32_t(i + 1) * 2654435761U;
    const auto random = [&]() {
        state = state * 1664525U + 1013904223U;
        return float(int((state >> 8) % 701) - 350) * .01f;
    };
    Input result{{random(), random(), random()}, {}, .05f + float(i % 13) * .1f};
    result.b = result.a;
    if (kind != 1) {
        const auto axis = i % 3;
        result.a[axis] = i % 2 ? 4.f : -4.f;
        result.b[axis] = -result.a[axis];
        result.b[(axis + 1) % 3] += .13f;
    }
    return result;
}
} // namespace ts_collision_corpus
