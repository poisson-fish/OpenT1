// Asset-free receiver workloads. No window, GPU, simulation, or audio is initialized.
#include <opent1/world/world_surfaces.hpp>
#include <opent1/world/shadow_receivers.hpp>
#include <opent1/gfx/render_stats.hpp>
#include <algorithm>
#include <chrono>
#include <fstream>
#include <iostream>
#include <numeric>
#include <filesystem>
#if defined(OPENT1_SHADOW_ALLOCATIONS)
#include <cstdlib>
#include <new>
#if defined(_WIN32)
#include <malloc.h>
#endif
namespace {
thread_local bool recordAllocations{};
thread_local std::size_t allocationCalls{};
}

void* operator new(std::size_t size) {
    if (auto* p = std::malloc(std::max(size, std::size_t{1}))) {
        if (recordAllocations)
            ++allocationCalls;
        return p;
    }
    throw std::bad_alloc();
}

void* operator new[](std::size_t size) {
    return ::operator new(size);
}

void operator delete(void* p) noexcept {
    std::free(p);
}

void operator delete[](void* p) noexcept {
    std::free(p);
}

void operator delete(void* p, std::size_t) noexcept {
    std::free(p);
}

void operator delete[](void* p, std::size_t) noexcept {
    std::free(p);
}

void* operator new(std::size_t size, std::align_val_t alignment) {
    const auto align = std::size_t(alignment);
#if defined(_WIN32)
    auto* p = _aligned_malloc(std::max(size, std::size_t{1}), align);
#else
    auto* p =
        std::aligned_alloc(align, (std::max(size, std::size_t{1}) + align - 1) / align * align);
#endif
    if (!p)
        throw std::bad_alloc();
    if (recordAllocations)
        ++allocationCalls;
    return p;
}

void* operator new[](std::size_t size, std::align_val_t alignment) {
    return ::operator new(size, alignment);
}

void operator delete(void* p, std::align_val_t) noexcept {
#if defined(_WIN32)
    _aligned_free(p);
#else
    std::free(p);
#endif
}

void operator delete[](void* p, std::align_val_t alignment) noexcept {
    ::operator delete(p, alignment);
}

void operator delete(void* p, std::size_t, std::align_val_t alignment) noexcept {
    ::operator delete(p, alignment);
}

void operator delete[](void* p, std::size_t, std::align_val_t alignment) noexcept {
    ::operator delete(p, alignment);
}
#endif

using namespace opent1;

namespace {
std::shared_ptr<world::WorldSurfaceMesh> mesh(unsigned cells, float spacing, unsigned layers) {
    auto result = std::make_shared<world::WorldSurfaceMesh>();
    world::TsMaterialResources::Data materials;
    materials.list.details = materials.list.materialsPerDetail = 1;
    materials.list.materials.push_back({.flags = 1, .index = 200, .mapFile = {}});
    materials.textures.resize(1);
    result->materials = world::TsMaterialResources(std::move(materials));
    for (unsigned layer = 0; layer < layers; ++layer)
        for (unsigned y = 0; y < cells; ++y)
            for (unsigned x = 0; x < cells; ++x) {
                const float px = (float(x) - cells * .5f) * spacing;
                const float py = (float(y) - cells * .5f) * spacing;
                const auto z = [&](float xx) {
                    return float(layer) * .5f + xx * float(layer % 2) * .1f;
                };
                const std::array<Point3F, 4> points{{{px, py, z(px)},
                                                     {px + spacing, py, z(px + spacing)},
                                                     {px + spacing, py + spacing, z(px + spacing)},
                                                     {px, py + spacing, z(px)}}};
                for (const auto ids : {std::array{0, 2, 1}, std::array{0, 3, 2}}) {
                    world::StaticSurfaceTriangle triangle;
                    for (unsigned i = 0; i < 3; ++i)
                        triangle.vertices[i].position = points[ids[i]];
                    result->triangles.push_back(std::move(triangle));
                }
            }
    return result;
}
}

int main(int argc, char** argv) {
    const unsigned frames = argc > 2 ? unsigned(std::stoul(argv[2])) : 60;
    if (!frames || frames > 10000)
        return 2;
    std::ofstream file;
    if (argc > 1) {
        const auto output = std::filesystem::absolute(argv[1]);
        std::filesystem::create_directories(output.parent_path());
        file.open(output);
        if (!file) {
            std::cerr << "Cannot open shadow benchmark output\n";
            return 1;
        }
    }
    std::ostream& out = file.is_open() ? file : std::cout;
    out << "{\"frames\":" << frames << ",\"allocation_audit\":"
#if defined(OPENT1_SHADOW_ALLOCATIONS)
        << "true"
#else
        << "false"
#endif
        << ",\"cases\":[";
    bool first = true;
    for (unsigned workload = 0; workload < 3; ++workload) {
        const unsigned cells = workload == 0 ? 64 : 16;
        const float spacing = workload == 0 ? 1.f : .25f;
        const unsigned layers = workload == 2 ? 3 : 1;
        const auto geometry = mesh(cells, spacing, layers);
        const std::array<world::WorldSurfaceInstance, 1> instances{{{geometry, {}}}};
        std::vector<double> timings;
        gfx::RenderWorkStats work;
        unsigned failures{}, polygons{};
        [[maybe_unused]] std::size_t allocations{};
        world::ShadowReceiverCache indexes;
        for (unsigned f = 0; f < frames + 4; ++f) {
            gfx::RenderWorkStats sample;
            gfx::ScopedRenderWorkStats record(sample);
            const auto start = std::chrono::steady_clock::now();
#if defined(OPENT1_SHADOW_ALLOCATIONS)
            allocationCalls = 0;
            recordAllocations = true;
#endif
            const world::ShadowReceiverScene receivers(instances, &indexes);
            Point3F light{.25f, .15f, -1};
            light.normalize();
            for (unsigned actor = 0; actor < 16; ++actor) {
                const auto window = sim::makeShadowSourceWindow(
                    light,
                    {float(actor % 4) * .05f, float(actor / 4) * .05f, 3},
                    1.5f,
                    {{-1, -1, -1}, {1, 1, 1}},
                    RMat3F{});
                const auto shadow = receivers.geometry(window, {0, 0, -1}, 100, .02f);
                if (f >= 4) {
                    failures += !shadow || shadow->polygons.empty();
                    if (shadow)
                        polygons += unsigned(shadow->polygons.size());
                }
            }
            if (f >= 4) {
#if defined(OPENT1_SHADOW_ALLOCATIONS)
                recordAllocations = false;
                allocations += allocationCalls;
#endif
                timings.push_back(std::chrono::duration<double, std::milli>(
                                      std::chrono::steady_clock::now() - start)
                                      .count());
                work.shadowTriangleTests += sample.shadowTriangleTests;
                work.shadowCandidates += sample.shadowCandidates;
                work.shadowPairTests += sample.shadowPairTests;
                work.shadowClipCalls += sample.shadowClipCalls;
            }
        }
        std::ranges::sort(timings);
        if (!first)
            out << ',';
        first = false;
        out << "{\"name\":\"" << std::array{"sparse", "dense", "stacked"}[workload]
            << "\",\"actors\":16,\"triangles\":" << geometry->triangles.size()
            << ",\"mean_ms\":" << std::accumulate(timings.begin(), timings.end(), 0.) / frames
            << ",\"p50_ms\":" << timings[std::size_t(.5 * (frames - 1))]
            << ",\"p95_ms\":" << timings[std::size_t(.95 * (frames - 1))]
            << ",\"p99_ms\":" << timings[std::size_t(.99 * (frames - 1))]
            << ",\"failures\":" << failures << ",\"polygons\":" << polygons
            << ",\"allocations_per_frame\":"
#if defined(OPENT1_SHADOW_ALLOCATIONS)
            << allocations / frames
#else
            << "null"
#endif
            << ",\"triangle_tests_per_frame\":" << work.shadowTriangleTests / frames
            << ",\"candidates_per_frame\":" << work.shadowCandidates / frames
            << ",\"pair_tests_per_frame\":" << work.shadowPairTests / frames
            << ",\"clip_calls_per_frame\":" << work.shadowClipCalls / frames << '}';
    }
    out << "]}\n";
}
