#pragma once
#include <array>
#include <cstddef>
#include <cstdint>

namespace world_collision_corpus {
constexpr std::size_t samples = 257;
constexpr std::array<std::array<float, 4>, 3> heights{
    {{{0, 0, 0, 0}}, {{0, 1, 2, 4}}, {{-2, 3, 1, -4}}}};
// Outward planes of a solid cube. The front child is empty; the back chain
// intersects all six half-spaces before reaching its solid leaf.
constexpr std::array<std::array<float, 4>, 6> planes{{{{1, 0, 0, -1}},
                                                      {{-1, 0, 0, -1}},
                                                      {{0, 1, 0, -1}},
                                                      {{0, -1, 0, -1}},
                                                      {{0, 0, 1, -1}},
                                                      {{0, 0, -1, -1}}}};
constexpr std::array<std::array<float, 3>, 8> vertices{{{{-1, -1, -1}},
                                                        {{1, -1, -1}},
                                                        {{-1, 1, -1}},
                                                        {{1, 1, -1}},
                                                        {{-1, -1, 1}},
                                                        {{1, -1, 1}},
                                                        {{-1, 1, 1}},
                                                        {{1, 1, 1}}}};
constexpr std::array<std::array<int, 4>, 6> faces{{{{5, 7, 3, 1}},
                                                   {{2, 6, 4, 0}},
                                                   {{3, 7, 6, 2}},
                                                   {{4, 5, 1, 0}},
                                                   {{6, 7, 5, 4}},
                                                   {{1, 3, 2, 0}}}};

struct Input {
    std::array<float, 3> a, b;
    float radius;
};

constexpr Input input(std::size_t kind, std::size_t i) {
    std::uint32_t state = std::uint32_t(i + 1) * 2654435761U;
    const auto random = [&]() {
        state = state * 1664525U + 1013904223U;
        return float(int((state >> 8) % 601) - 300) * .01f;
    };
    Input result{
        {random(), random(), random()}, {random(), random(), random()}, .05f + float(i % 13) * .1f};
    if (kind == 0 && i % 2) {
        result.b = {0, 0, 0};
    }
    return result;
}

struct BoxInput {
    std::array<float, 3> position, rotation, radii;
};

constexpr BoxInput box(std::size_t i) {
    return {input(3, i).a,
            {float(i % 13) * .1f, float(i % 17) * -.12f, float(i % 19) * .13f},
            {.1f + float(i % 7) * .15f, .1f + float(i % 11) * .1f, .1f + float(i % 5) * .2f}};
}
} // namespace world_collision_corpus
