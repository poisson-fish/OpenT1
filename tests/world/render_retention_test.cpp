#include "test_support.hpp"
#include <ranges>
#include "test_registry.hpp"
#include <opent1/world/world_surfaces.hpp>
#include <opent1/world/world_scene.hpp>
#include <opent1/core/reusable_scratch.hpp>
#include <bit>

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
        for (auto i : std::views::iota(std::size_t{0}, scene.size())) {
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
        shape->states = {{"default", 0, 2}};
        shape->lods = {{100, 0, 0, ""}, {0, 1, 0, ""}};
        shape->lightStateNames.resize(1);
        auto geometry = std::make_unique<world::InteriorGeometry>();
        geometry->points = {{-1, 0, -1}, {1, 0, -1}, {0, 0, 1}};
        geometry->texturePoints = {{0, 0}, {1, 0}, {.5f, 1}};
        geometry->vertices = {{0, 0}, {1, 1}, {2, 2}};
        geometry->planes = {{0, 1, 0, 0}};
        geometry->textureScale = 1;
        geometry->bounds = {{-1, -1, -1}, {1, 1, 1}};
        world::InteriorSurface surface;
        surface.vertexCount = 3;
        geometry->surfaces = {surface, surface};
        auto lighting = std::make_unique<world::InteriorLighting>();
        lighting->mapSlots = 1;
        lighting->mapData = {std::byte{255}};
        lighting->surfaces = {{0x80000000U, 1, 0, 1, 1, 0, 0}, {0x80000fffU, 0, 0, 1, 1, 0, 0}};
        lighting->states = {{{}, 0, 1, 0}, {{0xff00, 0xff00, 0xff00, 0xff00}, 0, 1, 0}};
        lighting->stateData = {{0, 0, 0}};
        lighting->lights = {{7, -1, 2, 0, 0, 0}};
        auto lowGeometry = std::make_unique<world::InteriorGeometry>(*geometry);
        lowGeometry->surfaces.resize(1);
        auto lowLighting = std::make_unique<world::InteriorLighting>(*lighting);
        lowLighting->surfaces.resize(1);
        CHECK(manager.add("retain.dis", std::move(shape)));
        CHECK(manager.add("retain.dig", std::move(geometry)));
        CHECK(manager.add("retain.dil", std::move(lighting)));
        CHECK(manager.add("retain-low.dig", std::move(lowGeometry)));
        CHECK(manager.add("retain-low.dil", std::move(lowLighting)));
        world::InteriorShapeResources resources;
        resources.shape = manager.load<world::InteriorShape>("retain.dis");
        resources.geometry.push_back(manager.load<world::InteriorGeometry>("retain.dig"));
        resources.geometry.push_back(manager.load<world::InteriorGeometry>("retain-low.dig"));
        resources.lighting.push_back(manager.load<world::InteriorLighting>("retain.dil"));
        resources.lighting.push_back(manager.load<world::InteriorLighting>("retain-low.dil"));
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
        for (auto x : std::views::iota(-12) | std::views::take_while([&](int x) {
                          return x <= 12;
                      }))
            for (auto z : std::views::iota(-12) | std::views::take_while([&](int z) {
                              return z <= 12;
                          })) {
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

        sim::SimSceneLighting scene;
        sim::SimLight sun;
        sun.type = sim::SimLight::Type::Directional;
        sun.direction = {1, 0, 0}; // parallel to the surface, leaving its animated map unchanged
        sun.intensity = {1, 1, 1};
        scene.lights.push_back(sun);
        const auto sceneHandle = std::make_shared<const sim::SimSceneLighting>(scene);
        sim::SimSceneLighting ambient;
        ambient.ambient = {.2f, .2f, .2f};
        const auto ambientHandle = std::make_shared<const sim::SimSceneLighting>(ambient);
        for (const bool ambientOnly : {false, true}) {
            world::SimInteriorSurface provider(resources);
            provider.setVisibilityEnabled(false);
            CHECK(provider.setAutomaticDetail(0));
            const auto capture = [&](bool lit, bool lowDetail = false) {
                world::WorldSurfaceQuery query;
                if (lit) {
                    query.lighting = sceneHandle;
                } else if (ambientOnly) {
                    query.lighting = ambientHandle;
                }
                if (lowDetail) {
                    query.camera.emplace();
                    query.camera->worldToCamera = TMat3F(EulerF{}, {0, 100, 0});
                    query.camera->focal = {100, 100};
                }
                CHECK(provider.processQuery(query));
                test::check(!query.failed && query.image,
                            std::format("interior capture lit={} lowDetail={} ambientOnly={}",
                                        lit, lowDetail, ambientOnly));
                return query.image->mesh;
            };
            const auto old = capture(true);
            CHECK((*old->interiorMaps)[0]->samples[0] == 0);
            CHECK(provider.setLightState(7, 1));
            CHECK((*capture(false)->interiorMaps)[0]->samples[0] == 0xffff);
            const auto updated = capture(true);
            CHECK((*updated->interiorMaps)[0]->samples[0] == 0xffff);
            CHECK((*old->interiorMaps)[0]->samples[0] == 0); // old frames keep their maps
            CHECK(capture(false, true)->triangleCount() == 1);
            CHECK(capture(true, true)->triangleCount() == 1);
            CHECK(capture(false)->triangleCount() == 2);
            CHECK(capture(true)->triangleCount() == 2);
            CHECK((*updated->interiorMaps)[0]->samples[0] == 0xffff);
        }
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
        for (auto block : std::views::iota(0, 2)) {
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
            for (auto y : std::views::iota(0) | std::views::take_while([&](int y) {
                              return y <= 16;
                          }))
                for (auto x : std::views::iota(0) | std::views::take_while([&](int x) {
                                  return x <= 16;
                              }))
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
        UInt64 selectionHash = 14695981039346656037ULL;
        const auto hash = [&](UInt32 value) {
            selectionHash = (selectionHash ^ value) * 1099511628211ULL;
        };
        for (auto frame : std::views::iota(UInt32{0}, UInt32{27})) {
            const auto camera =
                frame < 3
                    ? std::array{Point3F{14, 4, 8}, Point3F{48, 12, 20}, Point3F{80, 24, 5}}[frame]
                    : Point3F{Flt32(frame * 7 % 100) - 4,
                              Flt32(frame * 11 % 40) - 4,
                              Flt32(frame % 3 * 8 + 5)};
            world::TerrainLodView view;
            view.camera = camera;
            view.compositeTextures = true;
            view.texturePixels = 256;
            view.nativeTraversal = true;
            view.nativeGeometry = world::TerrainGeometryView{32, 20, 1, .1f, 96, true};
            view.hazeDistance = 40.f;
            view.lightMapDensity = world::TerrainLightMapDensity::NativeOpenGL;
            std::optional<Box2F> region;
            if (frame >= 3) {
                view.nativeVisibility.emplace();
                view.nativeVisibility->visibleDistance = 96;
                view.nativeVisibility->cameraToGrid.set(
                    EulerF{Flt32(Int32(frame % 5) - 2) * .15f, 0, Flt32(frame) * .25f});
                if (frame % 4 == 0) {
                    view.nativeVisibility->perspective = false;
                    view.nativeVisibility->viewportCenter = {12, 0};
                    view.nativeVisibility->halfExtent = {30, 18};
                }
                if (frame % 3 == 0)
                    region = Box2F{7, 3, 87, 29};
                view.maximumLevel = frame % 5;
            }
            auto reference =
                world::WorldSurfaceMesh::terrainLod(resources, view, true, region, &cache);
            view.retainedPatches = true;
            auto compact =
                world::WorldSurfaceMesh::terrainLod(resources, view, true, region, &cache);
            CHECK(reference);
            CHECK(compact);
            CHECK(compact->terrainPatches && compact->triangles.empty());
            CHECK(compact->terrainSquareIds == reference->terrainSquareIds);
            CHECK(compact->triangleCount() == reference->triangleCount());
            if (frame < 3) {
                // Shadow broad-phase must retain the same local receivers
                // without expanding every retained terrain patch.
                const auto window = sim::makeShadowSourceWindow(
                    {0, 0, -1}, {14, 4, 12}, 2, {{-1, -1, -1}, {1, 1, 1}}, RMat3F{});
                const std::array<world::WorldSurfaceInstance, 1> eager{
                    {{std::make_shared<const world::WorldSurfaceMesh>(*reference), {}}}};
                const std::array<world::WorldSurfaceInstance, 1> retained{
                    {{std::make_shared<const world::WorldSurfaceMesh>(*compact), {}}}};
                const auto a = world::worldShadowGeometry(window, eager, {0, 0, -1});
                const auto b = world::worldShadowGeometry(window, retained, {0, 0, -1});
                CHECK(a && b && !a->polygons.empty() && a->polygons.size() == b->polygons.size());
                for (auto p : std::views::iota(std::size_t{}, a->polygons.size())) {
                    CHECK(a->polygons[p].vertices.size() == b->polygons[p].vertices.size());
                    for (auto v : std::views::iota(std::size_t{}, a->polygons[p].vertices.size())) {
                        CHECK(a->polygons[p].vertices[v].position ==
                              b->polygons[p].vertices[v].position);
                    }
                }
            }
            if (frame < 3)
                CHECK(compact->triangleCount() > 0);
            // Baseline selection/stitch fingerprint across moving, rotated,
            // orthographic and clipped views. Authored/target heights are exact
            // binary fractions, including unequal seams and pin ownership.
            hash(UInt32(compact->terrainSquareIds.size()));
            for (const auto& id : compact->terrainSquareIds)
                for (const auto value : id)
                    hash(value);
            for (const auto& patch : compact->terrainPatches->patches) {
                hash(patch.topologyKey);
                for (const auto& inputs : patch.heights) {
                    hash(std::bit_cast<UInt32>(inputs[0]));
                    hash(std::bit_cast<UInt32>(inputs[1]));
                }
            }
            std::size_t i{};
            for (const auto triangle : compact->surfaceTriangles()) {
                const auto& expected = reference->triangles[i++];
                CHECK(triangle.material == expected.material && triangle.lit == expected.lit);
                CHECK(triangle.lightMap->samples == expected.lightMap->samples);
                for (auto j : std::views::iota(std::size_t{0}, std::size_t{3})) {
                    const auto& a = triangle.vertices[j];
                    const auto& b = expected.vertices[j];
                    CHECK(a.position == b.position && a.texture == b.texture &&
                          a.lightMap == b.lightMap);
                    CHECK(a.farSphereDistance == b.farSphereDistance && a.haze == b.haze);
                }
            }
            snapshot = std::make_shared<const world::WorldSurfaceMesh>(std::move(*compact));
            view.textureTiming = world::TerrainTextureTiming::NativeSoftware;
            auto software =
                world::WorldSurfaceMesh::terrainLod(resources, view, true, region, &cache);
            CHECK(software && !software->terrainPatches);
            CHECK(software->terrainSquareIds == reference->terrainSquareIds);
            CHECK(software->triangles.size() == reference->triangles.size());
            for (auto j : std::views::iota(std::size_t{0}, software->triangles.size()))
                for (auto k : std::views::iota(std::size_t{0}, std::size_t{3})) {
                    const auto& a = software->triangles[j].vertices[k];
                    const auto& b = reference->triangles[j].vertices[k];
                    CHECK(a.position == b.position && a.farSphereDistance == b.farSphereDistance &&
                          a.haze == b.haze);
                }
            if (frame == 0) {
                // Fail after populating the arena, then reuse it on the next
                // view while the preceding published snapshot remains alive.
                auto invalid = resources;
                invalid.materials = {};
                view.compositeTextures = false;
                CHECK(!world::WorldSurfaceMesh::terrainLod(invalid, view, false));
            }
        }
        CHECK(selectionHash == 18022897737113234614ULL);
        // Unsupported composites recurse into projected output while the outer
        // arena is live. Exercise the independent nested scratch lease.
        auto custom = resources;
        auto customMaterials = resources.materials.data();
        customMaterials.list.materials[0].flags |= 0x1000U;
        custom.materials = world::TsMaterialResources(std::move(customMaterials));
        world::TerrainLodView fallback;
        fallback.camera = {14, 4, 8};
        fallback.nativeTraversal = fallback.compositeTextures = true;
        fallback.nativeGeometry = world::TerrainGeometryView{32, 20, 1, .1f, 96, true};
        const auto projected = world::WorldSurfaceMesh::terrainLod(custom, fallback, false);
        fallback.retainedPatches = true;
        const auto nested = world::WorldSurfaceMesh::terrainLod(custom, fallback, false);
        CHECK(projected && nested && !nested->terrainPatches && !nested->triangles.empty());
        CHECK(projected->terrainSquareIds == nested->terrainSquareIds);
        CHECK(projected->triangles.size() == nested->triangles.size());
        for (auto j : std::views::iota(std::size_t{0}, projected->triangles.size()))
            for (auto k : std::views::iota(std::size_t{0}, std::size_t{3}))
                CHECK(projected->triangles[j].vertices[k].position ==
                      nested->triangles[j].vertices[k].position);
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
