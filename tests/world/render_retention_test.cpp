#include "test_support.hpp"
#include "test_registry.hpp"
#include <opent1/world/world_surfaces.hpp>
#include <opent1/world/world_scene.hpp>
#include <opent1/core/reusable_scratch.hpp>

using namespace opent1;

namespace {
int sharedRenderInputs() {
    return test::run([] {
        world::TsMaterialResources retained;
        std::weak_ptr<const gfx::DirectionalShadowMap> mapLifetime;
        std::optional<world::TsLighting> lighting;
        {
            ResourceManager manager;
            CHECK(manager.add("shared.bmp", gfx::Bitmap::create(2, 2)));
            world::TsMaterialList list;
            list.details = list.materialsPerDetail = 1;
            list.materials.push_back({.flags = 3, .mapFile = "shared.bmp"});
            auto source = world::TsMaterialResources::load(manager, list);
            CHECK(source);
            retained = *source;
            CHECK(&retained.data() == &source->data());
            auto replacement = source->data();
            replacement.list.materials[0].flags = 1;
            *source = world::TsMaterialResources(std::move(replacement));
            CHECK(retained.list().materials[0].flags == 3);
            CHECK(source->list().materials[0].flags == 1);
            sim::SimSceneLighting scene;
            sim::SimLight sun;
            sun.type = sim::SimLight::Type::Directional;
            sun.direction = {0, 0, -1};
            sun.intensity = {1, 1, 1};
            sun.shadowMap = std::make_shared<const gfx::DirectionalShadowMap>(
                gfx::DirectionalShadowMap{2, {}, 1, {64, 64, 64, 64}});
            mapLifetime = sun.shadowMap;
            scene.lights.push_back(sun);
            lighting = world::tsSceneLighting(scene, {});
            CHECK(lighting && lighting->lights[0].shadowMap == sun.shadowMap);
            auto sameMap = *lighting;
            sameMap.lights[0].shadowMap =
                std::make_shared<const gfx::DirectionalShadowMap>(*sun.shadowMap);
            CHECK(sameMap == *lighting); // cache equality still compares map contents
        }
        CHECK(retained.textures()[0] && retained.textures()[0]->width == 2);
        CHECK(!mapLifetime.expired());
        const auto prepared = world::TsPreparedLighting::prepare(*lighting, {}, 1);
        CHECK(prepared && !prepared->lighting().lights[0].shadowMap);
        lighting.reset();
        CHECK(mapLifetime.expired()); // prepared scalars do not retain the source map
    });
}

int renderScratchLifetimes() {
    return test::run([] {
        struct Scratch {
            std::vector<std::shared_ptr<const int>> inputs;
            void clear() {
                inputs.clear();
            }
        };
        std::weak_ptr<const int> lifetime;
        {
            detail::ScratchLease<Scratch> outer;
            outer.get().inputs.push_back(std::make_shared<const int>(7));
            lifetime = outer.get().inputs.front();
            {
                detail::ScratchLease<Scratch> nested;
                CHECK(nested.get().inputs.empty());
                nested.get().inputs.push_back(std::make_shared<const int>(9));
                CHECK(*outer.get().inputs[0] == 7);
            }
            CHECK(!lifetime.expired());
        }
        CHECK(lifetime.expired());
        try {
            detail::ScratchLease<Scratch> failing;
            failing.get().inputs.push_back(std::make_shared<const int>(11));
            lifetime = failing.get().inputs.front();
            throw std::runtime_error("failed capture");
        } catch (const std::runtime_error&) {
        }
        detail::ScratchLease<Scratch> next;
        CHECK(next.get().inputs.empty() && lifetime.expired());
    });
}

int environmentScratchOwnership() {
    return test::run([] {
        sim::EnvironmentCamera camera;
        camera.viewport = {0, 0, 31, 31};
        gfx::Palette palette;
        std::array<gfx::PolygonVertex, 3> vertices{{{{1, 1}}, {{10, 1}}, {{1, 10}}}};
        std::array<gfx::HardwarePolygon, 3> scene;
        for (std::size_t i = 0; i < scene.size(); ++i) {
            scene[i].vertices = vertices;
            scene[i].material.blend = gfx::HardwareBlend::Alpha;
            scene[i].material.depth = gfx::HardwareDepth::TestOnly;
            scene[i].material.color = {Flt32(i) * .25f, 1, 1};
            scene[i].imageSortValue = i == 0 ? .7f : .2f;
        }
        const auto first = sim::prepareHardwareEnvironment(camera, {}, {}, palette, {}, scene);
        CHECK(first && first->draws().size() == 3 && first->vertices().size() == 9);
        CHECK(first->draws()[0].color.x == .25f && first->draws()[1].color.x == .5f &&
              first->draws()[2].color.x == 0); // stable order for equal image keys
        vertices[0].position.x = 2;
        scene[0].material.opacity = -1;
        CHECK(!sim::prepareHardwareEnvironment(camera, {}, {}, palette, {}, scene));
        scene[0].material.opacity = 1;
        const auto next = sim::prepareHardwareEnvironment(camera, {}, {}, palette, {}, scene);
        CHECK(next && next->draws().size() == 3 && next->vertices()[0].position.x == 2);
        CHECK(first->vertices()[0].position.x == 1); // published batches own their geometry
    });
}

int retainedInteriorLighting() {
    return test::run([] {
        ResourceManager manager;
        auto shape = std::make_unique<world::InteriorShape>();
        shape->lods.resize(1);
        shape->lightStateNames.resize(1);
        auto geometry = std::make_unique<world::InteriorGeometry>();
        geometry->points = {{-1, 0, -1}, {1, 0, -1}, {0, 0, 1}};
        geometry->texturePoints = {{0, 0}, {1, 0}, {.5f, 1}};
        geometry->vertices = {{0, 0}, {1, 1}, {2, 2}};
        world::InteriorSurface surface;
        surface.vertexCount = 3;
        geometry->surfaces = {surface, surface};
        auto lighting = std::make_unique<world::InteriorLighting>();
        lighting->mapSlots = 1;
        lighting->mapData = {std::byte{255}};
        lighting->surfaces = {{0x80000000U, 1, 0, 1, 1, 0, 0}, {0x80000fffU, 0, 0, 1, 1, 0, 0}};
        CHECK(manager.add("retain.dis", std::move(shape)));
        CHECK(manager.add("retain.dig", std::move(geometry)));
        CHECK(manager.add("retain.dil", std::move(lighting)));
        world::InteriorShapeResources resources;
        resources.shape = manager.load<world::InteriorShape>("retain.dis");
        resources.geometry.push_back(manager.load<world::InteriorGeometry>("retain.dig"));
        resources.lighting.push_back(manager.load<world::InteriorLighting>("retain.dil"));
        world::TsMaterialResources::Data materialData;
        materialData.list.materials.resize(1);
        materialData.list.materials[0].flags = 2;
        materialData.textures.resize(1);
        resources.materials = world::TsMaterialResources(std::move(materialData));
        const std::array<world::InteriorLightSlot, 1> dark{{{-1, {}}}},
            bright{{{0, {255, 255, 255, 255}}}};
        world::InteriorSurfaceLighting options;
        options.slots = dark;
        auto first = world::WorldSurfaceMesh::interior(resources, 0, 0, true, options);
        CHECK(first && first->triangleCount() == 2);
        CHECK(first->triangles.empty()); // compact indexed storage is the retained representation
        options.previous = &*first;
        options.slots = bright;
        auto next = world::WorldSurfaceMesh::interior(resources, 0, 0, true, options);
        CHECK(next && next->interiorMesh == first->interiorMesh);
        CHECK((*next->interiorMaps)[0] != (*first->interiorMaps)[0]);
        CHECK((*next->interiorMaps)[1] == (*first->interiorMaps)[1]);
        CHECK((*next->interiorMaps)[0]->samples[0] == 0xffff);
        world::WorldSurfaceMesh expanded;
        for (auto triangle : next->surfaceTriangles())
            expanded.triangles.push_back(std::move(triangle));
        for (int x = -12; x <= 12; ++x)
            for (int z = -12; z <= 12; ++z) {
                for (Flt32 depth : {-.051f, -.05f, 0.f, .05f, .051f}) {
                    const Point3F point{Flt32(x) * .1f, depth, Flt32(z) * .1f};
                    CHECK(next->sampleLighting(point) == expanded.sampleLighting(point));
                }
            }
        CHECK((*first->interiorMaps)[0]->samples[0] == 0);
        const std::array<UInt32, 1> visible{1};
        auto selection = next->visibleInterior(visible);
        CHECK(selection && selection->triangleCount() == 1 &&
              selection->interiorMesh == first->interiorMesh);
        CHECK(selection->interiorMaps == next->interiorMaps);
        world::TsStaticView view;
        view.twoSided = true;
        view.shapeToCamera.p = {0, 5, 0};
        CHECK(next->prepare(view));
        gfx::Palette palette;
        std::vector<gfx::HardwarePolygon> inputs;
        CHECK(next->appendHardwareInterior(inputs, view, palette));
        const auto count = inputs.size();
        CHECK(count > 0 && next->appendHardwareInterior(inputs, view, palette));
        CHECK(inputs.size() == count * 2);
        CHECK(!next->appendHardwareInterior(inputs, view, palette, -1));
        CHECK(inputs.size() == count * 2);
        options.slots = std::span<const world::InteriorLightSlot>{};
        CHECK(!world::WorldSurfaceMesh::interior(resources, 0, 0, true, options));
        CHECK(first->triangleCount() == 2); // a failed update leaves published geometry intact
    });
}

int retainedTerrainPatches() {
    return test::run([] {
        ResourceManager manager;
        auto grid = std::make_unique<world::TerrainGridFile>();
        grid->scale = 1;
        grid->detailCount = 5;
        grid->size = {3, 1};
        grid->blocks = {{0, "a"}, {1, "b"}};
        grid->blockMap = {0, 1, 0}; // repeated source and a deliberately unequal seam
        CHECK(manager.add("patch.dtf", std::move(grid)));
        for (int block = 0; block < 2; ++block) {
            auto source = std::make_unique<world::TerrainGridBlock>();
            source->width = 16;
            source->detailCount = 5;
            source->lightScale = 0;
            source->minimumHeight = 0;
            source->maximumHeight = 8;
            source->materials.resize(16 * 16);
            source->materials[17].flags = 8; // authored hole / coarse empty level
            source->heights.resize(17 * 17);
            source->lights.resize(17 * 17, 0xffff);
            source->pins[0].resize((17 * 17 + 7) / 8);
            source->pins[0][3] = std::byte{0x40};
            for (int y = 0; y <= 16; ++y)
                for (int x = 0; x <= 16; ++x)
                    source->heights[std::size_t(y) * 17 + x] = Flt32((x ^ y) + block * 8) * .25f;
            CHECK(manager.add(std::format("patch#{}.dtb", block), std::move(source)));
        }
        auto bitmap = gfx::Bitmap::create(16, 16);
        CHECK(bitmap && manager.add("patch.bmp", std::move(bitmap)));
        world::TerrainGridResources resources;
        resources.grid = manager.load<world::TerrainGridFile>("patch.dtf");
        resources.blocks = {manager.load<world::TerrainGridBlock>("patch#0.dtb"),
                            manager.load<world::TerrainGridBlock>("patch#1.dtb")};
        world::TsMaterialResources::Data materialData;
        materialData.list.materialsPerDetail = 1;
        materialData.list.details = 1;
        materialData.list.materials.resize(1);
        materialData.list.materials[0].flags = 0x103;
        materialData.textures = {manager.load<gfx::Bitmap>("patch.bmp")};
        resources.materials = world::TsMaterialResources(std::move(materialData));
        world::TerrainTextureCache cache(resources);
        std::shared_ptr<const world::WorldSurfaceMesh> snapshot;
        for (const auto camera : {Point3F{14, 4, 8}, Point3F{48, 12, 20}, Point3F{80, 24, 5}}) {
            world::TerrainLodView view;
            view.camera = camera;
            view.compositeTextures = true;
            view.texturePixels = 256;
            view.nativeTraversal = true;
            view.nativeGeometry = world::TerrainGeometryView{32, 20, 1, .1f, 96, true};
            view.hazeDistance = 40.f;
            view.lightMapDensity = world::TerrainLightMapDensity::NativeOpenGL;
            auto reference = world::WorldSurfaceMesh::terrainLod(resources, view, true, {}, &cache);
            view.retainedPatches = true;
            auto compact = world::WorldSurfaceMesh::terrainLod(resources, view, true, {}, &cache);
            CHECK(reference);
            CHECK(compact);
            CHECK(compact->terrainPatches && compact->triangles.empty());
            CHECK(compact->terrainSquareIds == reference->terrainSquareIds);
            CHECK(compact->triangleCount() == reference->triangleCount() &&
                  compact->triangleCount() > 0);
            std::size_t i{};
            for (const auto triangle : compact->surfaceTriangles()) {
                const auto& expected = reference->triangles[i++];
                CHECK(triangle.material == expected.material && triangle.lit == expected.lit);
                CHECK(triangle.lightMap->samples == expected.lightMap->samples);
                for (std::size_t j = 0; j < 3; ++j) {
                    const auto& a = triangle.vertices[j];
                    const auto& b = expected.vertices[j];
                    CHECK(a.position == b.position && a.texture == b.texture &&
                          a.lightMap == b.lightMap);
                    CHECK(a.farSphereDistance == b.farSphereDistance && a.haze == b.haze);
                }
            }
            snapshot = std::make_shared<const world::WorldSurfaceMesh>(std::move(*compact));
            view.textureTiming = world::TerrainTextureTiming::NativeSoftware;
            auto software = world::WorldSurfaceMesh::terrainLod(resources, view, true, {}, &cache);
            CHECK(software && !software->terrainPatches && !software->triangles.empty());
        }
        cache.clear();
        CHECK(snapshot && snapshot->retainedHardwareCompatible());
        world::TsStaticView view;
        view.shapeToCamera.p.y = 50;
        CHECK(snapshot->prepare(view));
    });
}
}

OPENT1_REGISTER_TEST("world", "retained_interior_lighting", retainedInteriorLighting);
OPENT1_REGISTER_TEST("world", "retained_terrain_patches", retainedTerrainPatches);
OPENT1_REGISTER_TEST("world", "shared_render_inputs", sharedRenderInputs);
OPENT1_REGISTER_TEST("world", "render_scratch_lifetimes", renderScratchLifetimes);
OPENT1_REGISTER_TEST("world", "environment_scratch_ownership", environmentScratchOwnership);
