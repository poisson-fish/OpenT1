#include "test_support.hpp"
#include "test_registry.hpp"
#include <opent1/world/world_surfaces.hpp>

using namespace opent1;

namespace {
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
        resources.materials.list.materials.resize(1);
        resources.materials.list.materials[0].flags = 2;
        resources.materials.textures.resize(1);
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
        options.slots = std::span<const world::InteriorLightSlot>{};
        CHECK(!world::WorldSurfaceMesh::interior(resources, 0, 0, true, options));
        CHECK(first->triangleCount() == 2); // a failed update leaves published geometry intact
    });
}
}

OPENT1_REGISTER_TEST("world", "retained_interior_lighting", retainedInteriorLighting);
