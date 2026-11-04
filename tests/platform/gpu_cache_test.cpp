#include "test_support.hpp"
#include <ranges>
#include "ts_test_support.hpp"
#include <opent1/gfx/sdl_gpu_surface.hpp>
#include <opent1/world/world_surfaces.hpp>
#include <opent1/sim/sim_hardware_environment.hpp>
#include <array>
#include <cmath>
#include <string_view>

using namespace opent1;

namespace {
void patternedTransfers(platform::SdlWindow& window) {
    gfx::Palette palette;
    for (auto i : std::views::iota(0U, 256U))
        palette.palettes[0].colors[i] = {UInt8(i), UInt8(255 - i), UInt8((i * 37) & 255), 0};
    auto surface = gfx::SdlGpuSurface::create(window, 1, 8);
    CHECK(surface);
    const auto verify = [&](const auto& expected) {
        std::vector<std::byte> actual(expected.size());
        CHECK(surface->readback(actual));
        CHECK(actual == expected);
    };
    const auto pattern = [&](gfx::Bitmap& bitmap, Int32 level) {
        const auto width = std::max(1, bitmap.width >> level);
        const auto height = std::max(1, bitmap.height >> level);
        std::vector<std::byte> expected(std::size_t(width) * height * 4);
        for (auto y : std::views::iota(0, height)) {
            auto row = bitmap.row(y, level);
            for (auto x : std::views::iota(0, width)) {
                const auto index = UInt8((x * 3 + y * 19 + level * 43) & 255);
                row[x] = std::byte(index);
                const auto& color = palette.palettes[0].colors[index];
                const auto offset = (std::size_t(y) * width + x) * 4;
                expected[offset] = std::byte(color.red);
                expected[offset + 1] = std::byte(color.green);
                expected[offset + 2] = std::byte(color.blue);
                expected[offset + 3] = std::byte{255};
            }
        }
        return expected;
    };
    const auto render = [&](const gfx::Bitmap& bitmap, Int32 width, Int32 height, bool dynamic) {
        const Flt32 w = Flt32(width), h = Flt32(height);
        const std::array<gfx::PolygonVertex, 4> quad{
            {{{0, 0}, {0, 0}}, {{w, 0}, {1, 0}}, {{w, h}, {1, 1}}, {{0, h}, {0, 1}}}};
        gfx::HardwareMaterial material;
        material.texture = &bitmap;
        material.depth = gfx::HardwareDepth::Disabled;
        material.perspective = false;
        material.dynamicTexture = dynamic;
        material.mipFilter = gfx::HardwareMipFilter::Nearest;
        const gfx::HardwarePolygon polygon{quad, material};
        const auto batch = gfx::HardwareBatch::prepare(std::span(&polygon, 1), &palette);
        CHECK(batch && surface->render(*batch));
    };
    // Narrow, odd and aligned widths catch padding read as texels. Compare
    // against known colors, rather than another potentially corrupt GPU path.
    for (const auto width : {1, 4, 17, 63, 64, 65, 127, 128, 129}) {
        auto bitmap = gfx::Bitmap::create(width, 8);
        CHECK(bitmap && surface->resize(width, 8));
        bitmap->palette = palette;
        const auto expected = pattern(*bitmap, 0);
        CHECK(surface->software().setPalette(palette));
        CHECK(surface->software().blit(*bitmap, {0, 0}) && surface->upload());
        verify(expected);
        CHECK(surface->uploadBitmap(*bitmap));
        verify(expected);
        render(*bitmap, width, 8, false); // texture-array layer
        verify(expected);
        render(*bitmap, width, 8, true); // standalone UI/dynamic texture
        verify(expected);
    }
    auto mipmapped = gfx::Bitmap::create(128, 32);
    CHECK(mipmapped);
    mipmapped->detailLevels = 3;
    mipmapped->pixels.resize(128 * 32 + 64 * 16 + 32 * 8);
    (void)pattern(*mipmapped, 0);
    (void)pattern(*mipmapped, 1);
    const auto expected = pattern(*mipmapped, 2);
    CHECK(surface->resize(32, 8));
    render(*mipmapped, 32, 8, false);
    verify(expected);
    render(*mipmapped, 32, 8, true);
    verify(expected);
}

gfx::HardwareBatch content(unsigned first) {
    const std::array<gfx::PolygonVertex, 4> vertices{
        {{{0, 0}, {0, 0}}, {{16, 0}, {1, 0}}, {{16, 16}, {1, 1}}, {{0, 16}, {0, 1}}}};
    std::vector<std::unique_ptr<gfx::Bitmap>> sources;
    std::vector<gfx::HardwarePolygon> polygons;
    for (auto i :
         std::views::iota(static_cast<unsigned>(first)) | std::views::take_while([&](unsigned i) {
             return i < first + 1200;
         })) {
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

void retainedInstanceSubmission(platform::SdlWindow& window) {
    std::vector<gfx::HardwareVertex> vertices(4);
    vertices[0].position = vertices[3].position = {-1, 4};
    vertices[1].position = vertices[2].position = {1, 4};
    vertices[0].inverseDepth = vertices[1].inverseDepth = -1;
    vertices[2].inverseDepth = vertices[3].inverseDepth = 1;
    vertices[0].color = {1, 0, 0};
    vertices[1].color = {0, 1, 0};
    vertices[2].color = {0, 0, 1};
    const auto mesh = gfx::HardwareMesh::create(std::move(vertices), {0, 1, 2, 0, 2, 3});
    CHECK(mesh);
    const std::array<gfx::PolygonVertex, 3> placeholder{{{{0, 0}}, {{1, 0}}, {{0, 1}}}};
    std::array<gfx::HardwarePolygon, 5> polygons;
    for (auto i : std::views::iota(std::size_t{0}, polygons.size())) {
        auto& p = polygons[i];
        p.vertices = placeholder;
        p.mesh = mesh;
        p.indexCount = 6;
        p.material.blend = gfx::HardwareBlend::Alpha;
        p.material.depth = gfx::HardwareDepth::TestOnly;
        p.material.opacity = .4f;
        const Flt32 shift = i == 2 ? .5f : i == 3 ? -.5f : 0;
        p.transform.camera = {{{1, 0, 0, shift}, {0, 1, 0, 0}, {0, 0, 1, 0}}};
        p.transform.optics = {16, 16, 8, 8};
        p.transform.clip = {.1f, 100, 1, 0};
    }
    polygons[0].indexCount = polygons[1].indexCount = 3;
    polygons[1].firstIndex = 3;
    auto batch = gfx::HardwareBatch::prepare(polygons);
    CHECK(batch && batch->meshes().size() == 3);
    std::array<gfx::HardwareBatch, 5> layers;
    std::array<const gfx::HardwareBatch*, 5> layerRefs;
    for (auto i : std::views::iota(std::size_t{0}, layers.size())) {
        auto layer = gfx::HardwareBatch::prepare(std::span(&polygons[i], 1));
        CHECK(layer);
        layers[i] = std::move(*layer);
        layerRefs[i] = &layers[i];
    }
    auto reference = gfx::HardwareBatch::compose(layerRefs);
    auto surface = gfx::SdlGpuSurface::create(window, 16, 16);
    CHECK(reference && surface);
    std::vector<std::byte> a(16 * 16 * 4), b(a.size());
    CHECK(surface->render(*reference) && surface->readback(a));
    CHECK(surface->submissionStats().draws == 5);
    CHECK(surface->render(*batch) && surface->readback(b) && a == b);
    // Adjacent index ranges coalesce before instancing; a later reuse of the
    // first transform must start another run instead of selecting instance 3.
    CHECK(surface->submissionStats().inputDraws == 5 && surface->submissionStats().draws == 2);
    CHECK(surface->submissionStats().vertexUniformPushes == 1);
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
                        for (auto c : std::views::iota(std::size_t{0}, std::size_t{3}))
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
    for (auto i : std::views::iota(std::size_t{0}) | std::views::take_while([&](std::size_t i) {
                      return i < quads.size();
                  })) {
        const auto x = Flt32((i % 4) * 8), y = Flt32((i / 4) * 8);
        quads[i] = {{{{x, y}}, {{x + 8, y}}, {{x + 8, y + 8}}, {{x, y + 8}}}};
        const auto side = Int32(i == 15 ? 256 : i + 2);
        maps[i].size = {side, side};
        maps[i].dynamic = i == 0;
        for (auto p : std::views::iota(Int32{0}) | std::views::take_while([&](Int32 p) {
                          return p < side * side;
                      })) {
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
    for (auto i : std::views::iota(std::size_t{0}) | std::views::take_while([&](std::size_t i) {
                      return i < frames.size();
                  })) {
        reference[i].resize(32 * 32 * 4);
        CHECK(surface->render(*frames[i]) && surface->readback(reference[i]));
    }
    CHECK(surface->textureCacheStats().lightMapPages == 0);
    surface.reset();
    config.lightMapPageBytes = 1024 * 1024;
    surface = gfx::SdlGpuSurface::create(window, 32, 32, config);
    CHECK(surface);
    for (auto i : std::views::iota(std::size_t{0}, frames.size())) {
        std::vector<std::byte> actual(reference[i].size());
        CHECK(surface->render(*frames[i]) && surface->readback(actual));
        for (auto p : std::views::iota(std::size_t{0}, actual.size()))
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
    for (auto i : std::views::iota(unsigned{0}, unsigned{256})) {
        auto& c = palette.palettes[0].colors[i];
        c = {UInt8(i), UInt8((i * 23) & 255), UInt8((i * 61) & 255), UInt8((i * 37) & 255)};
        if (changedPalette)
            std::swap(c.red, c.green);
    }
    std::array<std::array<gfx::PolygonVertex, 4>, 32> quads;
    std::vector<std::unique_ptr<gfx::Bitmap>> sources;
    std::vector<gfx::HardwarePolygon> polygons;
    for (auto i : std::views::iota(std::size_t{0}) | std::views::take_while([&](std::size_t i) {
                      return i < quads.size();
                  })) {
        // Authored mip chains require power-of-two source dimensions; single
        // mip images also exercise the supported non-power-of-two array layout.
        const auto levels = Int32(i % 3 + 1);
        auto bitmap = gfx::Bitmap::create(levels == 1 && (i & 1U) ? 6 : 8, 4);
        CHECK(bitmap);
        bitmap->detailLevels = levels;
        if (bitmap->detailLevels > 1) {
            std::size_t bytes{};
            for (auto level : std::views::iota(Int32{0}) | std::views::take_while([&](Int32 level) {
                                  return level < bitmap->detailLevels;
                              }))
                bytes += std::size_t(std::max(1, bitmap->width >> level)) *
                         std::max(1, bitmap->height >> level);
            bitmap->pixels.resize(bytes);
        }
        for (auto j : std::views::iota(std::size_t{0}, bitmap->pixels.size()))
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
    for (auto i : std::views::iota(std::size_t{0}) | std::views::take_while([&](std::size_t i) {
                      return i < frames.size();
                  })) {
        reference[i].resize(128 * 64 * 4);
        CHECK(surface->render(*frames[i]) && surface->readback(reference[i]));
    }
    CHECK(surface->textureCacheStats().textureArrays == 0);
    surface.reset();
    config.textureArrayBytes = 1024 * 1024;
    surface = gfx::SdlGpuSurface::create(window, 128, 64, config);
    CHECK(surface);
    for (auto i : std::views::iota(std::size_t{0}, frames.size())) {
        std::vector<std::byte> actual(reference[i].size());
        CHECK(surface->render(*frames[i]) && surface->readback(actual));
        for (auto p : std::views::iota(std::size_t{0}, actual.size()))
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
    for (auto key : std::views::iota(UInt32{0}, UInt32{32})) {
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
        for (auto i : std::views::iota(std::size_t{0}, std::size_t{8})) {
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
        for (auto i : std::views::iota(std::size_t{0}, std::size_t{8}))
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

void terrainSeamCoverage(platform::SdlWindow& window) {
    ResourceManager manager;
    auto bitmap = gfx::Bitmap::create(2, 2);
    CHECK(bitmap);
    std::ranges::fill(bitmap->pixels, std::byte{1});
    CHECK(manager.add("seam.bmp", std::move(bitmap)));
    world::WorldSurfaceMesh mesh;
    world::TsMaterialResources::Data materials;
    materials.list.materials.resize(1);
    materials.list.materials[0].flags = 0x103;
    materials.list.materialsPerDetail = 1;
    materials.textures = {manager.load<gfx::Bitmap>("seam.bmp")};
    mesh.materials = world::TsMaterialResources(std::move(materials));
    const std::array<Point2F, 8> ring{
        {{0, 0}, {0, .5f}, {0, 1}, {.5f, 1}, {1, 1}, {1, .5f}, {1, 0}, {.5f, 0}}};
    std::vector<gfx::HardwareVertex> vertices(8);
    for (std::size_t i = 0; i < ring.size(); ++i)
        vertices[i].position = ring[i];
    const auto topology = gfx::HardwareMesh::create(std::move(vertices), {0, 2, 4, 4, 6, 0});
    auto patches = std::make_shared<world::WorldSurfaceMesh::TerrainPatches>();
    patches->farDistance = patches->hazeDistance = 4096;
    for (int y = 0; y < 64; ++y)
        for (int x = 0; x < 64; ++x) {
            world::WorldSurfaceMesh::TerrainPatch patch;
            patch.topology = topology;
            patch.extent = 8;
            patch.origin = {Flt32(-2560 + x * 8), Flt32(-704 + y * 8), 0};
            patch.firstTriangle = patches->triangles;
            for (auto& h : patch.heights)
                h = {90, 90, 0, 0};
            patches->patches.push_back(std::move(patch));
            patches->triangles += 2;
        }
    mesh.terrainPatches = patches;
    gfx::Palette palette;
    palette.palettes[0].colors[1] = {100, 60, 30, 255};
    auto surface = gfx::SdlGpuSurface::create(window, 800, 600);
    CHECK(surface);
    world::TsStaticView view;
    view.width = 800;
    view.height = 600;
    view.focal = {400, 400};
    view.center = {400, 300};
    view.nearDistance = .1f;
    view.farDistance = 4096;
    gfx::GpuFrameConfig frame;
    frame.clearColor = {1, 1, 1};
    // A continuous opaque plane must cover the frame at these downward views,
    // even at large world coordinates and across independently drawn patches.
    for (int angle = 0; angle < 12; ++angle) {
        view.shapeToCamera.set(EulerF{-.9f, 0, Flt32(angle) * .173f},
                               {-2293.5f, -455, 102.5f});
        view.shapeToCamera.inverse();
        const auto polygons = mesh.hardwareInterior(view, palette);
        CHECK(polygons);
        const auto batch = gfx::HardwareBatch::prepare(*polygons, &palette);
        CHECK(batch && surface->render(*batch, frame));
        std::vector<std::byte> pixels(800 * 600 * 4);
        CHECK(surface->readback(pixels));
        unsigned holes{};
        for (std::size_t i = 0; i < pixels.size(); i += 4)
            holes += pixels[i] == std::byte{255};
        if (holes)
            std::printf("Terrain seam coverage: angle %d, %u uncovered pixels\n", angle, holes);
        CHECK(holes == 0);
    }
}

void terrainSubmission(platform::SdlWindow& window) {
    std::vector<gfx::HardwareVertex> vertices(18);
    std::vector<UInt32> indices;
    for (auto i : std::views::iota(UInt32{0}) | std::views::take_while([&](UInt32 i) {
                      return i < vertices.size();
                  })) {
        vertices[i].inverseDepth = Flt32(i);
        indices.push_back(i);
    }
    const auto mesh = gfx::HardwareMesh::create(std::move(vertices), std::move(indices));
    CHECK(mesh);
    std::array<std::unique_ptr<gfx::Bitmap>, 16> images;
    std::array<gfx::LightMap, 16> maps;
    std::array<gfx::HardwarePolygon, 16> polygons;
    const std::array<gfx::PolygonVertex, 3> placeholder{{{{0, 0}}, {{1, 0}}, {{0, 1}}}};
    const std::array<Point2F, 3> lightUV{};
    gfx::Palette palette;
    for (auto i : std::views::iota(unsigned{0}, unsigned{256}))
        palette.palettes[0].colors[i] = {UInt8(i), UInt8(i * 17), UInt8(i * 31), 255};
    for (auto i : std::views::iota(std::size_t{0}, polygons.size())) {
        images[i] = gfx::Bitmap::create(4, 4);
        CHECK(images[i]);
        for (auto p : std::views::iota(std::size_t{0}, images[i]->pixels.size()))
            images[i]->pixels[p] = std::byte((i * 17 + p * 7) & 255);
        maps[i].size = {2, 2};
        maps[i].samples = {UInt16(0xf000U | (i * 53)), 0xff8f, 0xf8ff, 0xfff8};
        auto& polygon = polygons[i];
        polygon.vertices = placeholder;
        polygon.lightMapCoordinates = lightUV;
        polygon.mesh = mesh;
        polygon.indexCount = 18;
        polygon.material.texture = images[i].get();
        polygon.material.lightMap = &maps[i];
        auto& t = polygon.transform;
        t.camera = {
            {{1, 0, 0, Flt32(i % 4) * 2 - 4}, {0, 0, 0, 8}, {0, 1, 0, Flt32(i / 4) * 2 - 4}}};
        t.optics = {32, 32, 32, 32};
        t.clip = {.1f, 20, 1, 0};
        t.geometry = {2, 20, 1, Flt32(i)};
        t.ambient = {20, 0, 0, 5};
        t.mapping = {-.5f, -.5f, 2, 2}; // clamp before applying the page rectangle
        for (auto& h : t.parameters)
            h = {0, 0, 0, Flt32(i) * .5f + 6};
    }
    gfx::GpuFrameConfig frame;
    frame.hazeColor = {.2f, .3f, .4f};
    gfx::GpuSurfaceConfig standalone;
    standalone.textureArrayBytes = standalone.lightMapPageBytes = 0;
    platform::WindowConfig referenceConfig;
    referenceConfig.hidden = true;
    referenceConfig.width = referenceConfig.height = 64;
    platform::SdlWindow referenceWindow(referenceConfig);
    auto reference = gfx::SdlGpuSurface::create(referenceWindow, 64, 64, standalone);
    auto batched = gfx::SdlGpuSurface::create(window, 64, 64);
    CHECK(reference && batched);
    for (auto mode : std::views::iota(unsigned{0}, unsigned{5})) {
        if (mode == 1) {
            // Same-state alpha patches overlap: instance order must retain the
            // original texture/blend order, including at equal depth.
            for (auto& p : polygons) {
                p.transform.camera[0][3] = -1;
                p.transform.camera[2][3] = -1;
                p.material.blend = gfx::HardwareBlend::Alpha;
                p.material.depth = gfx::HardwareDepth::TestOnly;
                p.material.opacity = .4f;
            }
        }
        if (mode == 3) {
            // Cutouts with overlapping, interleaved near/far patches exercise
            // discard before both fog color and depth writes.
            for (auto i : std::views::iota(std::size_t{0}, polygons.size())) {
                auto& p = polygons[i];
                p.transform.camera[1][3] = 5 + Flt32((i * 7) % 16) * .25f;
                p.material.blend = gfx::HardwareBlend::Opaque;
                p.material.depth = gfx::HardwareDepth::TestAndWrite;
                p.material.opacity = 1;
                p.material.transparent = true;
                for (std::size_t j = 0; j < images[i]->pixels.size(); j += 2)
                    images[i]->pixels[j] = std::byte{0};
            }
        }
        if (mode == 4) {
            for (auto i : std::views::iota(std::size_t{0}, polygons.size()))
                polygons[i].material.depth =
                    i % 2 ? gfx::HardwareDepth::Disabled : gfx::HardwareDepth::TestAndWrite;
        }
        auto batch = gfx::HardwareBatch::prepare(polygons, &palette);
        CHECK(batch);
        if (mode == 2) {
            // Composed layers retain their draw boundaries even when their
            // bindings and ranges would otherwise be compatible.
            const std::array<const gfx::HardwareBatch*, 2> layers{&*batch, &*batch};
            batch = gfx::HardwareBatch::compose(layers);
            CHECK(batch);
        }
        std::vector<std::byte> a(64 * 64 * 4), b(a.size());
        CHECK(reference->render(*batch, frame) && reference->readback(a));
        CHECK(batched->render(*batch, frame) && batched->readback(b));
        for (auto i : std::views::iota(std::size_t{0}, a.size()))
            CHECK(std::abs(int(a[i]) - int(b[i])) <= 2);
        const auto stats = batched->submissionStats();
        CHECK(stats.inputDraws == (mode == 2 ? 32U : 16U));
        const std::array expectedDraws{1U, 1U, 2U, 1U, 16U};
        const std::array expectedVertexPushes{1U, 1U, 2U, 1U, 16U};
        const std::array expectedFragmentPushes{1U, 1U, 1U, 1U, 1U};
        CHECK(stats.draws == expectedDraws[mode]);
        CHECK(stats.fragmentUniformPushes == expectedFragmentPushes[mode]);
        CHECK(stats.vertexUniformPushes == expectedVertexPushes[mode]);
        CHECK(reference->submissionStats().draws >= stats.draws);
        // Immutable snapshots and per-patch sampling survive cache recreation.
        CHECK(batched->trimCaches() && batched->render(*batch, frame) && batched->readback(a));
        CHECK(a == b);
    }
}

void fogReadbacks(platform::SdlWindow& window) {
    ResourceManager manager;
    CHECK(world::registerTsLoaders(manager) && gfx::registerBitmapFontLoaders(manager));
    auto leaf = gfx::Bitmap::create(2, 1);
    auto terrain = gfx::Bitmap::create(1, 1);
    CHECK(leaf && terrain);
    leaf->pixels[0] = std::byte{0};
    leaf->pixels[1] = std::byte{1};
    terrain->pixels[0] = std::byte{2};
    CHECK(manager.add("test.bmp", std::move(leaf)) &&
          manager.add("fog-terrain.bmp", std::move(terrain)));
    std::array<world::TsShapeResources, 2> leaves;
    for (auto i : std::views::iota(std::size_t{0}, leaves.size())) {
        auto shape = test::ts::load(test::ts::shape());
        CHECK(shape);
        for (auto& mesh : shape->meshes)
            std::ranges::fill(mesh.textureVertices, Point2F{i ? .25f : .75f, .5f});
        const auto name = i ? "leaf-hole.dts" : "leaf-solid.dts";
        CHECK(manager.add(name, std::move(shape)));
        const auto resources = world::TsShapeResources::load(manager, name);
        CHECK(resources);
        leaves[i] = *resources;
    }
    const std::array<Point2F, 8> ring{
        {{0, 0}, {0, .5f}, {0, 1}, {.5f, 1}, {1, 1}, {1, .5f}, {1, 0}, {.5f, 0}}};
    std::vector<gfx::HardwareVertex> vertices(8);
    for (auto i : std::views::iota(std::size_t{0}, ring.size()))
        vertices[i].position = ring[i];
    world::WorldSurfaceMesh::TerrainPatch patch;
    patch.extent = 16;
    patch.topology = gfx::HardwareMesh::create(std::move(vertices), {0, 2, 4, 4, 6, 0});
    CHECK(patch.topology);
    world::WorldSurfaceMesh ground;
    world::TsMaterialResources::Data materials;
    materials.list.materials.resize(1);
    materials.list.materials[0].flags = 0x103;
    materials.list.materialsPerDetail = 1;
    materials.textures = {manager.load<gfx::Bitmap>("fog-terrain.bmp")};
    ground.materials = world::TsMaterialResources(std::move(materials));
    world::TsStaticView terrainView;
    terrainView.width = terrainView.height = 64;
    terrainView.focal = terrainView.center = {32, 32};
    terrainView.farDistance = 32;
    // Face a flat native terrain patch toward the camera to cover the viewport.
    terrainView.shapeToCamera.m = {{{1, 0, 0}, {0, 0, 1}, {0, -1, 0}}};
    terrainView.shapeToCamera.p = {-8, 8, -8};
    terrainView.shapeToCamera.flags = RMat3F::Matrix_HasRotation | RMat3F::Matrix_HasTranslation;
    sim::EnvironmentCamera camera;
    camera.viewport = {0, 0, 63, 63};
    camera.center = camera.focal = {32, 32};
    camera.nearDistance = .1f;
    camera.farDistance = 32;
    auto surface = gfx::SdlGpuSurface::create(window, 64, 64);
    CHECK(surface);
    gfx::GpuFrameConfig frame;
    frame.hazeColor = {.2f, .3f, .4f};
    frame.clearColor = {.1f, .15f, .2f};
    std::array<gfx::PolygonVertex, 4> foreground{{{{0, 0}}, {{64, 0}}, {{64, 64}}, {{0, 64}}}};
    for (auto& v : foreground)
        v.inverseDepth = .5f;
    gfx::HardwareMaterial foregroundMaterial;
    foregroundMaterial.color = {0, 0, 1};

    struct LeafCase {
        UInt32 flags;
        UInt8 alpha;
        Flt32 haze;
        bool hole{};
    };

    const std::array cases{LeafCase{0x2403, 255, 0},
                           LeafCase{0x2403, 128, 0},
                           LeafCase{0x2403, 0, 0, true},
                           LeafCase{0x2403, 128, .5f},
                           LeafCase{0x2403, 128, 1},
                           LeafCase{0x1403, 255, .5f},
                           LeafCase{0x1403, 255, .5f, true},
                           LeafCase{0x403, 255, .5f}};
    gfx::Palette palette;
    palette.palettes[0].colors[0] = {0, 255, 0, 0};
    palette.palettes[0].colors[2] = {255, 0, 0, 255};
    for (bool retainedTerrain : {false, true})
        for (bool retainedShape : {false, true})
            for (Flt32 terrainHaze : {-1.f, 0.f, .5f, 1.f}) {
                auto patches = std::make_shared<world::WorldSurfaceMesh::TerrainPatches>();
                patches->farDistance = 32;
                patches->hazeDistance = 8;
                patches->triangles = 2;
                for (auto& h : patch.heights)
                    h = {0, 0, 0, 8 + 24 * std::max(terrainHaze, 0.f)};
                patches->patches = {patch};
                ground.terrainPatches = patches;
                for (const auto& c : cases) {
                    palette.palettes[0].colors[1] = {0, 255, 0, c.alpha};
                    auto resources = leaves[c.hole ? 1 : 0];
                    auto data = resources.materials.data();
                    data.list.materials[0].flags = c.flags;
                    resources.materials = world::TsMaterialResources(std::move(data));
                    auto view = terrainView;
                    view.shapeToCamera.identity();
                    view.shapeToCamera.p.y = 4;
                    view.haze = c.haze;
                    const auto pose = world::TsShapePose::sample(resources.shape);
                    CHECK(pose);
                    const auto shapeFrame =
                        world::TsStaticShapeFrame::prepare(resources, view, *pose, retainedShape);
                    CHECK(shapeFrame);
                    const auto shapePolygons = shapeFrame->hardwarePolygons(&palette);
                    CHECK(shapePolygons && !shapePolygons->empty());
                    auto polygons = *shapePolygons;
                    // Submit cutouts before terrain: holes must discard both fog and Z.
                    const auto terrainFrame = ground.prepare(terrainView);
                    CHECK(terrainFrame);
                    const auto terrainPolygons = retainedTerrain
                                                     ? ground.hardwareInterior(terrainView, palette)
                                                     : terrainFrame->hardwarePolygons(&palette);
                    CHECK(terrainPolygons && !terrainPolygons->empty());
                    if (terrainHaze >= 0)
                        polygons.insert(
                            polygons.end(), terrainPolygons->begin(), terrainPolygons->end());
                    for (bool occluded : {false, true}) {
                        auto scene = polygons;
                        if (occluded)
                            scene.push_back({foreground, foregroundMaterial});
                        const auto batch =
                            sim::prepareHardwareEnvironment(camera, {}, {}, palette, {}, scene);
                        CHECK(batch && surface->render(*batch, frame));
                        std::vector<std::byte> pixels(64 * 64 * 4);
                        CHECK(surface->readback(pixels));
                        const auto fog = frame.hazeColor;
                        const auto background =
                            terrainHaze < 0
                                ? frame.clearColor
                                : Point3F(1, 0, 0) * (1 - terrainHaze) + fog * terrainHaze;
                        const auto source = Point3F(0, 1, 0) * (1 - c.haze) + fog * c.haze;
                        const auto alpha =
                            c.hole ? 0.f : (c.flags & 0x2000U ? c.alpha / 255.f : 1.f);
                        const auto expected =
                            occluded ? Point3F(0, 0, 1) : source * alpha + background * (1 - alpha);
                        for (auto channel : std::views::iota(std::size_t{0}, std::size_t{3}))
                            CHECK(std::abs(int(pixels[(33 * 64 + 32) * 4 + channel]) -
                                           int(std::lround(expected[channel] * 255))) <= 2);
                    }
                }
            }
}

void fogBlendReadbacks(platform::SdlWindow& window) {
    auto surface = gfx::SdlGpuSurface::create(window, 16, 16);
    CHECK(surface);
    std::array<gfx::PolygonVertex, 4> quad{{{{0, 0}}, {{16, 0}}, {{16, 16}}, {{0, 16}}}};
    gfx::GpuFrameConfig frame;
    frame.clearColor = {.25f, .5f, .75f};
    frame.hazeColor = {.7f, .2f, .1f};
    for (const auto blend : {gfx::HardwareBlend::Opaque,
                             gfx::HardwareBlend::Alpha,
                             gfx::HardwareBlend::Additive,
                             gfx::HardwareBlend::Subtractive,
                             gfx::HardwareBlend::Multiply})
        for (const Flt32 haze : {0.f, .5f, 1.f}) {
            gfx::HardwareMaterial material;
            material.color = {.2f, .4f, .6f};
            material.opacity = .5f;
            material.alphaSource = gfx::HardwareAlphaSource::Constant;
            material.blend = blend;
            material.hazeSource = gfx::PolygonSource::Constant;
            material.haze = haze;
            const gfx::HardwarePolygon polygon{quad, material};
            const auto batch = gfx::HardwareBatch::prepare(std::span(&polygon, 1));
            CHECK(batch && surface->render(*batch, frame));
            std::vector<std::byte> pixels(16 * 16 * 4);
            CHECK(surface->readback(pixels));
            for (auto channel : std::views::iota(std::size_t{0}, std::size_t{3})) {
                const auto background = frame.clearColor[channel];
                const auto source = material.color[channel] * (1 - haze);
                Flt32 expected{};
                switch (blend) {
                    case gfx::HardwareBlend::Opaque:
                        expected = source + frame.hazeColor[channel] * haze;
                        break;
                    case gfx::HardwareBlend::Alpha:
                        expected =
                            (source + frame.hazeColor[channel] * haze) * .5f + background * .5f;
                        break;
                    case gfx::HardwareBlend::Additive:
                        expected = background + source * .5f;
                        break;
                    case gfx::HardwareBlend::Subtractive:
                        expected = background * (1 - source);
                        break;
                    case gfx::HardwareBlend::Multiply:
                        expected = background * (source + haze);
                        break;
                }
                CHECK(std::abs(int(pixels[(8 * 16 + 8) * 4 + channel]) -
                               int(std::lround(std::clamp(expected, 0.f, 1.f) * 255))) <= 2);
            }
        }
}

void shadowReadbacks(platform::SdlWindow& window) {
    auto surface = gfx::SdlGpuSurface::create(window, 64, 64);
    CHECK(surface);
    gfx::Palette palette;
    sim::EnvironmentCamera camera;
    camera.viewport = {0, 0, 63, 63};
    camera.center = camera.focal = {32, 32};
    camera.nearDistance = .1f;
    camera.farDistance = 20;
    std::array<gfx::PolygonVertex, 4> ground{{{{0, 0}}, {{64, 0}}, {{64, 64}}, {{0, 64}}}};
    for (auto& vertex : ground)
        vertex.inverseDepth = .25f;
    gfx::HardwareMaterial material;
    material.color = {1, 1, 1};
    const std::array<gfx::HardwarePolygon, 1> scene{{{ground, material}}};
    sim::ShadowGeometry geometry;
    geometry.polygons.push_back({0,
                                 {{{-1, 3.98f, 1}, {0, 0}},
                                  {{1, 3.98f, 1}, {1, 0}},
                                  {{1, 3.98f, -1}, {1, 1}},
                                  {{-1, 3.98f, -1}, {0, 1}}}});
    const std::vector<UInt8> silhouette(66 * 66, 255);
    auto bitmap = sim::filterShadowBitmap(silhouette, 66, 64, sim::ShadowBlur::Box2, .5f);
    const std::array<sim::EnvironmentShadow, 1> shadows{{{geometry, bitmap}}};
    const auto plain = sim::prepareHardwareEnvironment(camera, {}, {}, palette, {}, scene);
    const auto shaded = sim::prepareHardwareEnvironment(camera, {}, {}, palette, shadows, scene);
    CHECK(plain && shaded && surface->render(*plain));
    std::vector<std::byte> a(64 * 64 * 4), b(a.size());
    CHECK(surface->readback(a) && surface->render(*shaded) && surface->readback(b));
    const auto center = (32 * 64 + 32) * 4, outside = (8 * 64 + 8) * 4;
    CHECK(a[center] == std::byte{255});
    CHECK(int(b[center]) >= 120 && int(b[center]) <= 135);
    CHECK(b[outside] == a[outside]);
    // A projected shadow behind the receiver must fail the depth test.
    for (auto& vertex : geometry.polygons[0].vertices)
        vertex.position.y = 4.02f;
    const auto occluded = sim::prepareHardwareEnvironment(camera, {}, {}, palette, shadows, scene);
    CHECK(occluded && surface->render(*occluded) && surface->readback(b));
    CHECK(b == a);
    CHECK(surface->trimCaches() && surface->render(*shaded) && surface->readback(b));
    CHECK(int(b[center]) >= 120 && int(b[center]) <= 135);
}

void shapeReadbacks(platform::SdlWindow& window) {
    ResourceManager manager;
    CHECK(manager.add("retained.dts", test::ts::load(test::ts::shape())));
    auto bitmap = gfx::Bitmap::create(4, 4);
    CHECK(bitmap);
    for (auto i : std::views::iota(std::size_t{0}, bitmap->pixels.size()))
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
            for (const auto& [type, pointMode] :
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

int runGpuCache(bool requireGpu) {
    return test::run([&] {
        platform::SdlApplication app;
        CHECK(app.initialize({}) == SDL_APP_CONTINUE);
        const auto* driver = SDL_GetCurrentVideoDriver();
        if (driver && std::string_view(driver) == "dummy") {
            CHECK(!requireGpu);
            std::puts("GPU cache test skipped: SDL dummy driver has no GPU device");
            return;
        }
        platform::WindowConfig config;
        config.hidden = true;
        config.width = config.height = 16;
        auto& window = app.createWindow(config);
        patternedTransfers(window);
        retainedWorkingSet(window);
        retainedMesh(window);
        retainedInstanceSubmission(window);
        interiorReadbacks(window);
        pageReadbacks(window);
        arrayReadbacks(window);
        terrainReadbacks(window);
        terrainSeamCoverage(window);
        terrainSubmission(window);
        fogReadbacks(window);
        fogBlendReadbacks(window);
        shadowReadbacks(window);
        shapeReadbacks(window);
    });
}

int gpuCache() {
    return runGpuCache(false);
}

int requiredGpuCache() {
    return runGpuCache(true);
}
}

#include "test_registry.hpp"
OPENT1_REGISTER_TEST("platform", "gpu_cache", gpuCache);
OPENT1_REGISTER_TEST("gpu", "gpu_cache", requiredGpuCache);
