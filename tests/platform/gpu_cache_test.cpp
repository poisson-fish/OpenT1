#include "test_support.hpp"
#include "ts_test_support.hpp"
#include <opent1/gfx/sdl_gpu_surface.hpp>
#include <opent1/world/world_surfaces.hpp>
#include <opent1/sim/sim_hardware_environment.hpp>
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
    const auto memory = batch->memoryStats();
    CHECK(memory.batchBytes > 0 &&
          memory.meshBytes >= mesh->vertices().size_bytes() + mesh->indices().size_bytes());
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
    CHECK(surface->resourceStats().meshes == 0 && surface->resourceStats().uploadCapacity == 0 &&
          surface->resourceStats().instanceCapacity == 0);
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
    world::TsMaterialResources::Data materialData;
    materialData.list.materials.resize(1);
    materialData.list.materials[0].flags = 3;
    materialData.textures.push_back(manager.load<gfx::Bitmap>("gpu.bmp"));
    resources.materials = world::TsMaterialResources(std::move(materialData));
    auto surface = gfx::SdlGpuSurface::create(window, 64, 64);
    CHECK(surface);
    gfx::Palette palette;
    palette.palettes[0].colors[0] = {0, 0, 0, 0};
    palette.palettes[0].colors[1] = {255, 0, 0, 0};
    palette.palettes[0].colors[2] = {0, 255, 0, 0};
    palette.palettes[0].colors[3] = {0, 0, 255, 0};
    for (bool cutout : {false, true}) {
        auto replacementMaterials = resources.materials.data();
        replacementMaterials.list.materials[0].flags = 3 | (cutout ? 0x1000U : 0);
        resources.materials = world::TsMaterialResources(std::move(replacementMaterials));
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

gfx::HardwareBatch arrayContent(bool changedPalette) {
    gfx::Palette palette;
    for (unsigned i = 0; i < 256; ++i) {
        auto& c = palette.palettes[0].colors[i];
        c = {UInt8(i), UInt8((i * 23) & 255), UInt8((i * 61) & 255), UInt8((i * 37) & 255)};
        if (changedPalette)
            std::swap(c.red, c.green);
    }
    std::array<std::array<gfx::PolygonVertex, 4>, 32> quads;
    std::vector<std::unique_ptr<gfx::Bitmap>> sources;
    std::vector<gfx::HardwarePolygon> polygons;
    for (std::size_t i = 0; i < quads.size(); ++i) {
        // Authored mip chains require power-of-two source dimensions; single
        // mip images also exercise the supported non-power-of-two array layout.
        const auto levels = Int32(i % 3 + 1);
        auto bitmap = gfx::Bitmap::create(levels == 1 && (i & 1U) ? 6 : 8, 4);
        CHECK(bitmap);
        bitmap->detailLevels = levels;
        if (bitmap->detailLevels > 1) {
            std::size_t bytes{};
            for (Int32 level = 0; level < bitmap->detailLevels; ++level)
                bytes += std::size_t(std::max(1, bitmap->width >> level)) *
                         std::max(1, bitmap->height >> level);
            bitmap->pixels.resize(bytes);
        }
        for (std::size_t j = 0; j < bitmap->pixels.size(); ++j)
            bitmap->pixels[j] = std::byte((j * 17 + i * 7) % 255 + 1);
        const auto x = Flt32((i % 8) * 16), y = Flt32((i / 8) * 16);
        quads[i] = {{{{x, y}, {-.5f, -.5f}},
                     {{x + 16, y}, {2.5f, -.5f}},
                     {{x + 16, y + 16}, {2.5f, 2.5f}},
                     {{x, y + 16}, {-.5f, 2.5f}}}};
        gfx::HardwareMaterial material;
        material.texture = bitmap.get();
        material.perspective = false;
        material.depth = gfx::HardwareDepth::Disabled;
        material.linear = (i & 1U) != 0;
        material.wrap = (i & 2U) != 0;
        material.mipFilter = i % 3 == 0 ? gfx::HardwareMipFilter::None
                                        : (i % 3 == 1 ? gfx::HardwareMipFilter::Nearest
                                                      : gfx::HardwareMipFilter::Linear);
        material.lodBias = Flt32(i % 5) * .75f;
        material.paletteAlpha = true;
        material.alphaCutoff = i % 4 == 0 ? .3f : 0;
        material.blend = i % 4 == 1 ? gfx::HardwareBlend::Alpha : gfx::HardwareBlend::Opaque;
        polygons.push_back({quads[i], material});
        sources.push_back(std::move(bitmap));
    }
    auto batch = gfx::HardwareBatch::prepare(polygons, &palette);
    CHECK(batch);
    return std::move(*batch);
}

void arrayReadbacks(platform::SdlWindow& window) {
    const auto first = arrayContent(false), changed = arrayContent(true);
    const std::array<const gfx::HardwareBatch*, 3> frames{&first, &changed, &first};
    gfx::GpuSurfaceConfig config;
    config.textureArrayBytes = 0;
    auto surface = gfx::SdlGpuSurface::create(window, 128, 64, config);
    CHECK(surface);
    std::array<std::vector<std::byte>, 3> reference;
    for (std::size_t i = 0; i < frames.size(); ++i) {
        reference[i].resize(128 * 64 * 4);
        CHECK(surface->render(*frames[i]) && surface->readback(reference[i]));
    }
    CHECK(surface->textureCacheStats().textureArrays == 0);
    surface.reset();
    config.textureArrayBytes = 1024 * 1024;
    surface = gfx::SdlGpuSurface::create(window, 128, 64, config);
    CHECK(surface);
    for (std::size_t i = 0; i < frames.size(); ++i) {
        std::vector<std::byte> actual(reference[i].size());
        CHECK(surface->render(*frames[i]) && surface->readback(actual));
        for (std::size_t p = 0; p < actual.size(); ++p)
            CHECK(std::abs(int(actual[p]) - int(reference[i][p])) <= 2);
        if (i == 1)
            CHECK(surface->trimCaches());
    }
    const auto stats = surface->textureCacheStats();
    CHECK(stats.textureArrays >= 3 && stats.textureArrayBytes <= config.textureArrayBytes);
    CHECK(stats.physicalTextures < stats.textures);
}

void terrainReadbacks(platform::SdlWindow& window) {
    ResourceManager manager;
    auto bitmap = gfx::Bitmap::create(4, 4);
    CHECK(bitmap);
    std::ranges::fill(bitmap->pixels, std::byte{1});
    CHECK(manager.add("terrain.bmp", std::move(bitmap)));
    auto lightMap = std::make_shared<gfx::LightMap>();
    lightMap->size = {2, 2};
    lightMap->samples = {0xffff, 0xf88f, 0xff88, 0xf8f8};
    gfx::Palette palette;
    palette.palettes[0].colors[1] = {180, 110, 60, 255};
    auto surface = gfx::SdlGpuSurface::create(window, 64, 64);
    CHECK(surface);
    for (UInt32 key = 0; key < 32; ++key) {
        const std::array<Point2F, 8> ring{
            {{0, 0}, {0, .5f}, {0, 1}, {.5f, 1}, {1, 1}, {1, .5f}, {1, 0}, {.5f, 0}}};
        std::vector<gfx::HardwareVertex> vertices(8);
        std::vector<UInt32> indices;
        const auto emit = [&](UInt32 a, UInt32 b, UInt32 c) {
            indices.insert(indices.end(), {a & 7U, b & 7U, c & 7U});
        };
        for (const auto start : key & 16U ? std::array{6U, 2U} : std::array{0U, 4U}) {
            const bool a = key & (1U << (((start + 1) & 7U) / 2)),
                       b = key & (1U << (((start + 3) & 7U) / 2));
            if (a && b) {
                emit(start, start + 3, start + 4);
                emit(start, start + 1, start + 3);
                emit(start + 1, start + 2, start + 3);
            } else if (a) {
                emit(start, start + 1, start + 4);
                emit(start + 1, start + 2, start + 4);
            } else if (b) {
                emit(start, start + 2, start + 3);
                emit(start, start + 3, start + 4);
            } else
                emit(start, start + 2, start + 4);
        }
        for (std::size_t i = 0; i < 8; ++i) {
            vertices[i].position = ring[i];
            vertices[i].texture = {ring[i].x, 1 - ring[i].y};
        }
        world::WorldSurfaceMesh mesh;
        world::TsMaterialResources::Data materialData;
        materialData.list.materials.resize(1);
        materialData.list.materials[0].flags = 0x103;
        materialData.list.materialsPerDetail = 1;
        materialData.textures = {manager.load<gfx::Bitmap>("terrain.bmp")};
        mesh.materials = world::TsMaterialResources(std::move(materialData));
        auto patches = std::make_shared<world::WorldSurfaceMesh::TerrainPatches>();
        patches->farDistance = 8;
        patches->hazeDistance = 3;
        world::WorldSurfaceMesh::TerrainPatch patch;
        patch.topology = gfx::HardwareMesh::create(std::move(vertices), indices);
        patch.topologyKey = key;
        patch.extent = 2;
        patch.origin = {-2, 4, -1};
        patch.lightMap = lightMap;
        patch.mapOrigin = {.1f, .9f};
        patch.mapScale = {.8f, -.8f};
        for (std::size_t i = 0; i < 8; ++i)
            patch.heights[i] = {
                Flt32(i % 3) * .1f, Flt32(i % 2) * .2f, Flt32(i % 3) * .6f, Flt32(i) * .7f + 3.5f};
        patches->patches.push_back(patch);
        patch.firstTriangle = indices.size() / 3;
        patch.origin.x = .5f;
        for (auto& h : patch.heights) {
            h[0] += .2f;
            h[3] -= 1.5f;
        }
        patches->patches.push_back(patch);
        patches->triangles = indices.size() / 3 * 2;
        mesh.terrainPatches = patches;
        world::TsStaticView view;
        view.width = view.height = 64;
        view.focal = {32, 32};
        view.center = {32, 32};
        view.nearDistance = .1f;
        view.farDistance =
            key & 1U ? 7.f : 8.f; // draw clipping can differ from the terrain haze limit
        view.shapeToCamera.p.z = -1;
        const auto projectedFrame = mesh.prepare(view);
        CHECK(projectedFrame);
        const auto reference = projectedFrame->hardware(&palette);
        const auto polygons = mesh.hardwareInterior(view, palette);
        CHECK(reference && polygons);
        const auto retained = gfx::HardwareBatch::prepare(*polygons, &palette);
        CHECK(retained);
        gfx::GpuFrameConfig frame;
        frame.hazeColor = {.2f, .3f, .4f};
        std::vector<std::byte> a(64 * 64 * 4), b(a.size());
        CHECK(surface->render(*reference, frame) && surface->readback(a));
        CHECK(surface->render(*retained, frame) && surface->readback(b));
        unsigned errors{};
        for (std::size_t i = 0; i < a.size(); i += 4)
            if (std::abs(int(a[i]) - int(b[i])) > 2 ||
                std::abs(int(a[i + 1]) - int(b[i + 1])) > 2 ||
                std::abs(int(a[i + 2]) - int(b[i + 2])) > 2)
                ++errors;
        CHECK(errors <= 8);
        CHECK(surface->resourceStats().meshUploads == 1);
    }
}

void shapeReadbacks(platform::SdlWindow& window) {
    ResourceManager manager;
    CHECK(manager.add("retained.dts", test::ts::load(test::ts::shape())));
    auto bitmap = gfx::Bitmap::create(4, 4);
    CHECK(bitmap);
    for (std::size_t i = 0; i < bitmap->pixels.size(); ++i)
        bitmap->pixels[i] = std::byte(i % 4);
    CHECK(manager.add("test.bmp", std::move(bitmap)));
    auto resources = world::TsShapeResources::load(manager, "retained.dts");
    CHECK(resources);
    const auto pose = world::TsShapePose::sample(resources->shape);
    CHECK(pose);
    gfx::Palette palette;
    palette.palettes[0].colors[1] = {230, 80, 30, 128};
    palette.palettes[0].colors[2] = {30, 210, 70, 160};
    palette.palettes[0].colors[3] = {40, 60, 240, 200};
    auto surface = gfx::SdlGpuSurface::create(window, 64, 64);
    CHECK(surface);
    gfx::GpuFrameConfig frame;
    frame.hazeColor = {.2f, .3f, .4f};
    std::optional<gfx::HardwareBatch> earlier;
    std::vector<std::byte> earlierPixels;
    for (const UInt32 shading : {0x100U, 0x200U, 0x400U})
        for (const UInt32 alpha : {0U, 0x1000U, 0x2000U})
            for (const auto [type, pointMode] :
                 std::array{std::pair{world::TsLight::Type::Directional, 0},
                            std::pair{world::TsLight::Type::DirectionalNonlinear, 0},
                            std::pair{world::TsLight::Type::DirectionalWrap, 0},
                            std::pair{world::TsLight::Type::Point, 0},
                            std::pair{world::TsLight::Type::Point, 1},
                            std::pair{world::TsLight::Type::Point, 2},
                            std::pair{world::TsLight::Type::Point, 3}})
                for (Flt32 depth : {.08f, 1.f, 5.f, 19.8f, 21.f}) {
                    auto replacementMaterials = resources->materials.data();
                    replacementMaterials.list.materials[0].flags = shading | alpha | 3;
                    resources->materials = world::TsMaterialResources(std::move(replacementMaterials));
                    world::TsStaticView view;
                    view.width = view.height = 64;
                    view.focal = {32, 32};
                    view.center = {32, 32};
                    view.nearDistance = .1f;
                    view.farDistance = 20;
                    view.shapeToCamera.p.y = depth;
                    if (pointMode == 3 && alpha == 0x1000U) {
                        view.shapeToCamera.m[0][0] = -1;
                        view.shapeToCamera.flags |= RMat3F::Matrix_HasRotation;
                    }
                    view.haze = .35f;
                    view.opacity = alpha == 0x2000U ? .6f : 1.f;
                    view.lighting.emplace();
                    view.lighting->ambient = {.1f, .2f, .15f};
                    world::TsLight light;
                    light.type = type;
                    light.direction = {0, -1, 0};
                    light.position = {0, -4, 1};
                    light.range = 20;
                    light.attenuation = {1, .01f, .001f};
                    light.intensity = {.6f, .5f, .7f};
                    if (pointMode) {
                        light.position = {0, -100, 0};
                        light.range = pointMode == 1   ? 200.f
                                      : pointMode == 2 ? light.position.lenf()
                                                       : 1.f;
                    }
                    view.lighting->lights = {light};
                    if (pointMode == 1) {
                        auto second = light;
                        second.type = world::TsLight::Type::DirectionalWrap;
                        second.shadowAttenuation = .5f;
                        view.lighting->lights.push_back(second);
                    }
                    auto evaluated = *pose;
                    if (pointMode && shading == 0x400U)
                        CHECK(evaluated.overrideParentTransform(1, TMat3F(EulerF{0, 0, .2f}, {})));
                    const auto referenceFrame =
                        world::TsStaticShapeFrame::prepare(*resources, view, evaluated);
                    const auto retainedFrame =
                        world::TsStaticShapeFrame::prepare(*resources, view, evaluated, true);
                    CHECK(referenceFrame && retainedFrame);
                    const auto reference = referenceFrame->hardware(&palette);
                    auto inputs = retainedFrame->hardwarePolygons(&palette);
                    CHECK(reference && inputs);
                    sim::EnvironmentCamera camera;
                    camera.viewport = {0, 0, 63, 63};
                    camera.center = {32, 32};
                    camera.focal = {32, 32};
                    camera.nearDistance = .1f;
                    camera.farDistance = 20;
                    for (auto& input : *inputs)
                        input.imageSortValue = .5f;
                    const auto retained =
                        sim::prepareHardwareEnvironment(camera, {}, {}, palette, {}, *inputs);
                    CHECK(retained && surface->render(*reference, frame));
                    std::vector<std::byte> a(64 * 64 * 4), b(a.size());
                    CHECK(surface->readback(a) && surface->render(*retained, frame) &&
                          surface->readback(b));
                    unsigned largeErrors{};
                    for (std::size_t i = 0; i < a.size(); i += 4)
                        if (std::abs(int(a[i]) - int(b[i])) > 2 ||
                            std::abs(int(a[i + 1]) - int(b[i + 1])) > 2 ||
                            std::abs(int(a[i + 2]) - int(b[i + 2])) > 2)
                            ++largeErrors;
                    CHECK(largeErrors <= 8);
                    if (shading == 0x400U && alpha == 0 &&
                        type == world::TsLight::Type::Directional && depth == 5) {
                        CHECK(!retained->meshes().empty());
                        earlier = *retained;
                        earlierPixels = b;
                    }
                }
    CHECK(earlier && surface->trimCaches());
    world::clearTsHardwareMeshes();
    const auto conversionBudget = gfx::hardwareTextureConversionStats();
    gfx::setHardwareTextureConversionBudget(0);
    CHECK(gfx::hardwareTextureConversionStats().bytes == 0);
    CHECK(surface->render(*earlier, frame));
    std::vector<std::byte> actual(earlierPixels.size());
    CHECK(surface->readback(actual) && actual == earlierPixels);
    gfx::setHardwareTextureConversionBudget(conversionBudget.budgetBytes,
                                            conversionBudget.budgetEntries);
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
        arrayReadbacks(window);
        terrainReadbacks(window);
        shapeReadbacks(window);
    });
}
}

#include "test_registry.hpp"
OPENT1_REGISTER_TEST("platform", "gpu_cache", gpuCache);
