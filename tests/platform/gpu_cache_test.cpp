#include "test_support.hpp"
#include <opent1/gfx/sdl_gpu_surface.hpp>
#include <array>
#include <string_view>

using namespace opent1;

namespace {
gfx::HardwareBatch content(unsigned first) {
    const std::array<gfx::PolygonVertex, 4> vertices{
        {{{0, 0}, {0, 0}}, {{16, 0}, {1, 0}}, {{16, 16}, {1, 1}}, {{0, 16}, {0, 1}}}};
    std::vector<std::unique_ptr<gfx::Bitmap>> sources;
    std::vector<gfx::HardwarePolygon> polygons;
    for (unsigned i = first; i < first + 1200; ++i) {
        auto bitmap = gfx::Bitmap::create(1, 1, 24);
        CHECK(bitmap);
        bitmap->pixels[0] = std::byte{64};
        bitmap->pixels[1] = std::byte((i >> 8) & 255);
        bitmap->pixels[2] = std::byte(i & 255);
        gfx::HardwareMaterial material;
        material.texture = bitmap.get();
        material.depth = gfx::HardwareDepth::Disabled;
        material.perspective = false;
        polygons.push_back({vertices, material});
        sources.push_back(std::move(bitmap));
    }
    auto batch = gfx::HardwareBatch::prepare(polygons);
    CHECK(batch);
    return std::move(*batch);
}

void retainedWorkingSet(platform::SdlWindow& window) {
    const auto first = content(0), second = content(1200);
    auto surface = gfx::SdlGpuSurface::create(window, 16, 16);
    CHECK(surface);
    CHECK(surface->render(first) && surface->render(second));
    const auto warm = surface->textureCacheStats();
    CHECK(warm.textures == 2401 && warm.evictions == 0);
    CHECK(surface->render(first) && surface->render(second));
    const auto stable = surface->textureCacheStats();
    CHECK(stable.uploads == warm.uploads && stable.evictions == 0);
    CHECK(stable.hits > warm.hits);
    std::vector<std::byte> pixels(16 * 16 * 4);
    CHECK(surface->readback(pixels));
    CHECK(pixels[0] == std::byte(2399 & 255) && pixels[1] == std::byte(2399 >> 8) &&
          pixels[2] == std::byte{64});
    surface.reset();

    // A smaller retention budget still admits a complete frame and evicts
    // unpinned older content, with the same rendered pixels on a later revisit.
    gfx::GpuSurfaceConfig config;
    config.textureCacheEntries = 1024;
    surface = gfx::SdlGpuSurface::create(window, 16, 16, config);
    CHECK(surface && surface->render(first) && surface->render(second));
    const auto evicted = surface->textureCacheStats();
    CHECK(evicted.textures == 1201 && evicted.evictions > 0);
    CHECK(surface->render(first) && surface->render(second));
    CHECK(surface->textureCacheStats().uploads > evicted.uploads);
    CHECK(surface->readback(pixels));
    CHECK(pixels[0] == std::byte(2399 & 255) && pixels[1] == std::byte(2399 >> 8) &&
          pixels[2] == std::byte{64});
}

int gpuCache() {
    return test::run([] {
        platform::SdlApplication app;
        CHECK(app.initialize({}) == SDL_APP_CONTINUE);
        const auto* driver = SDL_GetCurrentVideoDriver();
        if (driver && std::string_view(driver) == "dummy") {
            std::puts("GPU cache test skipped: SDL dummy driver has no GPU device");
            return;
        }
        platform::WindowConfig config;
        config.hidden = true;
        config.width = config.height = 16;
        auto& window = app.createWindow(config);
        retainedWorkingSet(window);
    });
}
}

#include "test_registry.hpp"
OPENT1_REGISTER_TEST("platform", "gpu_cache", gpuCache);
