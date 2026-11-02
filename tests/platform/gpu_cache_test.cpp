#include "test_support.hpp"
#include <opent1/gfx/sdl_gpu_surface.hpp>
#include <opent1/world/world_surfaces.hpp>
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
    CHECK(warm.textureArrays > 0 && warm.physicalTextures < warm.textures);
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

void retainedMesh(platform::SdlWindow& window) {
    auto surface = gfx::SdlGpuSurface::create(window, 16, 16);
    CHECK(surface);
    std::vector<gfx::HardwareVertex> vertices(3);
    vertices[0].position = {-1, 4};
    vertices[0].inverseDepth = -1;
    vertices[1].position = {0, 4};
    vertices[1].inverseDepth = 1;
    vertices[2].position = {1, 4};
    vertices[2].inverseDepth = -1;
    const auto mesh = gfx::HardwareMesh::create(vertices, {0, 1, 2});
    CHECK(mesh && !gfx::HardwareMesh::create(vertices, {0, 1, 9}));
    auto invalid = vertices;
    invalid[0].position.x = 1e13f;
    CHECK(!gfx::HardwareMesh::create(std::move(invalid), {0, 1, 2}));
    const std::array<gfx::PolygonVertex, 3> placeholder{{{{0, 0}}, {{1, 0}}, {{0, 1}}}};
    gfx::HardwareMaterial material;
    material.color = {1, 0, 0};
    gfx::HardwarePolygon polygon{placeholder, material};
    polygon.mesh = mesh;
    polygon.indexCount = 3;
    polygon.transform.camera = {{{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}}};
    polygon.transform.optics = {16, 16, 8, 8};
    polygon.transform.clip = {.1f, 100, 1, 0};
    auto batch = gfx::HardwareBatch::prepare(std::span(&polygon, 1));
    CHECK(batch && batch->vertices().empty() && batch->meshes().size() == 1);
    CHECK(surface->render(*batch));
    std::vector<std::byte> pixels(16 * 16 * 4);
    CHECK(surface->readback(pixels));
    CHECK(pixels[(8 * 16 + 8) * 4] == std::byte{255});
    const auto warm = surface->resourceStats();
    CHECK(warm.meshUploads == 1 && warm.meshAllocations == 2);
    polygon.transform.camera[0][3] = .25f;
    batch = gfx::HardwareBatch::prepare(std::span(&polygon, 1));
    CHECK(batch && surface->render(*batch));
    CHECK(surface->resourceStats().meshUploads == warm.meshUploads);
    CHECK(surface->submissionStats().meshUploadBytes == 0);
    CHECK(surface->readback(pixels));
    const auto published = pixels;
    CHECK(surface->trimCaches());
    CHECK(surface->resourceStats().meshes == 0 && surface->resourceStats().uploadCapacity == 0);
    CHECK(surface->readback(pixels));
    CHECK(pixels == published);
    CHECK(surface->render(*batch) && surface->resourceStats().meshUploads == warm.meshUploads + 1);
    CHECK(surface->render(*batch));
    CHECK(surface->submissionStats().meshUploadBytes == 0);
    // Near/far clipping applies per fragment while retaining the shared inverse depth convention.
    polygon.transform.clip[0] = 5;
    batch = gfx::HardwareBatch::prepare(std::span(&polygon, 1));
    CHECK(batch && surface->render(*batch) && surface->readback(pixels));
    CHECK(pixels[(8 * 16 + 8) * 4] == std::byte{0});
    const auto empty = gfx::HardwareBatch::prepare({});
    CHECK(empty && surface->render(*empty));
    CHECK(surface->resourceStats().meshUploads == warm.meshUploads + 1);
    surface.reset();
    gfx::GpuSurfaceConfig bounded;
    bounded.meshCacheBytes = 0;
    bounded.stagingBytes = 4096;
    bounded.textureArrayBytes = bounded.lightMapPageBytes = 0;
    surface = gfx::SdlGpuSurface::create(window, 16, 16, bounded);
    CHECK(surface && surface->render(*empty) && surface->readback(pixels));
    const auto preserved = pixels;
    CHECK(!surface->render(*batch));
    CHECK(surface->resourceStats().meshAllocations == 0);
    CHECK(!surface->render(
        content(0))); // staging is bounded independently of resident texture payload
    CHECK(surface->readback(pixels) && pixels == preserved);
}

void interiorReadbacks(platform::SdlWindow& window) {
    ResourceManager manager;
    auto shape = std::make_unique<world::InteriorShape>();
    shape->lods.resize(1);
    shape->lightStateNames.resize(1);
    auto geometry = std::make_unique<world::InteriorGeometry>();
    geometry->points = {{-1, 0, -1}, {-1, 0, 1}, {1, 0, 1}, {1, 0, -1}};
    geometry->texturePoints = {{0, 1}, {0, 0}, {1, 0}, {1, 1}};
    geometry->vertices = {{0, 0}, {1, 1}, {2, 2}, {3, 3}};
    world::InteriorSurface face;
    face.vertexCount = 4;
    geometry->surfaces.push_back(face);
    auto lighting = std::make_unique<world::InteriorLighting>();
    lighting->surfaces = {{0x800007cdU, 0, 0, 1, 1, 0, 0}};
    auto bitmap = gfx::Bitmap::create(2, 2);
    CHECK(bitmap);
    bitmap->pixels[0] = std::byte{0};
    bitmap->pixels[1] = std::byte{1};
    bitmap->pixels[bitmap->stride()] = std::byte{2};
    bitmap->pixels[bitmap->stride() + 1] = std::byte{3};
    CHECK(manager.add("gpu.dis", std::move(shape)) && manager.add("gpu.dig", std::move(geometry)) &&
          manager.add("gpu.dil", std::move(lighting)) && manager.add("gpu.bmp", std::move(bitmap)));
    world::InteriorShapeResources resources;
    resources.shape = manager.load<world::InteriorShape>("gpu.dis");
    resources.geometry.push_back(manager.load<world::InteriorGeometry>("gpu.dig"));
    resources.lighting.push_back(manager.load<world::InteriorLighting>("gpu.dil"));
    resources.materials.list.materials.resize(1);
    resources.materials.list.materials[0].flags = 3;
    resources.materials.textures.push_back(manager.load<gfx::Bitmap>("gpu.bmp"));
    auto surface = gfx::SdlGpuSurface::create(window, 64, 64);
    CHECK(surface);
    gfx::Palette palette;
    palette.palettes[0].colors[0] = {0, 0, 0, 0};
    palette.palettes[0].colors[1] = {255, 0, 0, 0};
    palette.palettes[0].colors[2] = {0, 255, 0, 0};
    palette.palettes[0].colors[3] = {0, 0, 255, 0};
    for (bool cutout : {false, true}) {
        resources.materials.list.materials[0].flags = 3 | (cutout ? 0x1000U : 0);
        const auto mesh = world::WorldSurfaceMesh::interior(resources);
        CHECK(mesh);
        for (Flt32 gamma : {1.f, 1.7f}) {
            for (Flt32 distance : {.08f, .2f, 5.f, 19.8f, 21.f}) {
                for (Flt32 angle : {-.8f, 0.f, .8f, 3.1415927f}) {
                    world::TsStaticView view;
                    view.width = view.height = 64;
                    view.focal = {32, 32};
                    view.center = {32, 32};
                    view.nearDistance = .1f;
                    view.farDistance = 20;
                    view.shapeToCamera = TMat3F(EulerF{0, 0, angle}, {0, distance, 0});
                    const auto projected = mesh->prepare(view);
                    CHECK(projected);
                    const auto reference = projected->hardware(&palette, gamma);
                    const auto polygons = mesh->hardwareInterior(view, palette, gamma);
                    CHECK(reference && polygons);
                    const auto retained = gfx::HardwareBatch::prepare(*polygons, &palette);
                    CHECK(retained && surface->render(*reference));
                    std::vector<std::byte> a(64 * 64 * 4), b(a.size());
                    CHECK(surface->readback(a) && surface->render(*retained) &&
                          surface->readback(b));
                    unsigned largeErrors{};
                    for (std::size_t p = 0; p < a.size(); p += 4) {
                        bool large{};
                        for (std::size_t c = 0; c < 3; ++c)
                            large |= std::abs(int(a[p + c]) - int(b[p + c])) > 2;
                        largeErrors += large;
                    }
                    CHECK(largeErrors <= 20); // at most subpixel clipped/diagonal edge differences
                }
            }
            std::swap(palette.palettes[0].colors[1], palette.palettes[0].colors[2]);
        }
    }
}

gfx::HardwareBatch pageContent(bool changed) {
    std::array<std::array<gfx::PolygonVertex, 4>, 16> quads;
    std::array<gfx::LightMap, 16> maps;
    const std::array<Point2F, 4> uv{{{-1, -1}, {2, -1}, {2, 2}, {-1, 2}}};
    std::vector<gfx::HardwarePolygon> polygons;
    for (std::size_t i = 0; i < quads.size(); ++i) {
        const auto x = Flt32((i % 4) * 8), y = Flt32((i / 4) * 8);
        quads[i] = {{{{x, y}}, {{x + 8, y}}, {{x + 8, y + 8}}, {{x, y + 8}}}};
        const auto side = Int32(i == 15 ? 256 : i + 2);
        maps[i].size = {side, side};
        maps[i].dynamic = i == 0;
        for (Int32 p = 0; p < side * side; ++p) {
            const auto value = UInt16((p + i * 137 + (changed && i == 0 ? 123 : 0)) & 0xfff);
            maps[i].samples.push_back(value);
        }
        gfx::HardwareMaterial material;
        material.lightMap = &maps[i];
        material.perspective = false;
        material.depth = gfx::HardwareDepth::Disabled;
        polygons.push_back({quads[i], material, uv});
    }
    auto batch = gfx::HardwareBatch::prepare(polygons);
    CHECK(batch);
    return std::move(*batch);
}

void pageReadbacks(platform::SdlWindow& window) {
    const auto first = pageContent(false), changed = pageContent(true);
    const std::array<const gfx::HardwareBatch*, 4> frames{&first, &changed, &first, &changed};
    gfx::GpuSurfaceConfig config;
    config.lightMapPageBytes = 0;
    auto surface = gfx::SdlGpuSurface::create(window, 32, 32, config);
    CHECK(surface);
    std::array<std::vector<std::byte>, 4> reference;
    for (std::size_t i = 0; i < frames.size(); ++i) {
        reference[i].resize(32 * 32 * 4);
        CHECK(surface->render(*frames[i]) && surface->readback(reference[i]));
    }
    CHECK(surface->textureCacheStats().lightMapPages == 0);
    surface.reset();
    config.lightMapPageBytes = 1024 * 1024;
    surface = gfx::SdlGpuSurface::create(window, 32, 32, config);
    CHECK(surface);
    for (std::size_t i = 0; i < frames.size(); ++i) {
        std::vector<std::byte> actual(reference[i].size());
        CHECK(surface->render(*frames[i]) && surface->readback(actual));
        for (std::size_t p = 0; p < actual.size(); ++p)
            CHECK(std::abs(int(actual[p]) - int(reference[i][p])) <= 2);
    }
    const auto stats = surface->textureCacheStats();
    CHECK(stats.lightMapPages == 1 && stats.lightMapPageBytes <= config.lightMapPageBytes);
    CHECK(stats.physicalTextures < stats.textures && stats.reuses > 0);
    surface->clearTextureCache();
    CHECK(surface->textureCacheStats().physicalTextures == 0);
    CHECK(surface->render(first));
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
        retainedMesh(window);
        interiorReadbacks(window);
        pageReadbacks(window);
    });
}
}

#include "test_registry.hpp"
OPENT1_REGISTER_TEST("platform", "gpu_cache", gpuCache);
