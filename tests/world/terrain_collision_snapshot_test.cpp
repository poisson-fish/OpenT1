#include "test_registry.hpp"
#include "test_support.hpp"
#include <opent1/world/terrain_collision_snapshot.hpp>

namespace {
using namespace opent1;
using namespace opent1::world;

TerrainCollisionSnapshot terrain(std::array<Int32, 4> placements = {0, 0, 0, 0},
                                  bool raisedCorner = false) {
    ResourceManager manager;
    auto grid = std::make_unique<TerrainGridFile>();
    grid->scale = 2;
    grid->detailCount = 2;
    grid->size = {2, 2};
    grid->blockMap.assign(placements.begin(), placements.end());
    grid->blocks = {{0, "low"}, {1, "high"}};
    CHECK(manager.add("test.dtf", std::move(grid)));
    TerrainGridResources resources;
    resources.grid = manager.load<TerrainGridFile>("test.dtf");
    for (Int32 id = 0; id < 2; ++id) {
        auto block = std::make_unique<TerrainGridBlock>();
        block->width = 2;
        block->detailCount = 2;
        block->heights.assign(9, id == 0 ? 0.f : 64.f);
        block->materials.assign(4, TerrainGridMaterial{0, UInt8(id == 0 ? 3 : 7)});
        if (raisedCorner && id == 0)
            block->heights[5] = 10;
        const auto name = std::format("test#{}.dtb", id);
        CHECK(manager.add(name, std::move(block)));
        resources.blocks.push_back(manager.load<TerrainGridBlock>(name));
    }
    auto snapshot = TerrainCollisionSnapshot::prepare(std::move(resources));
    CHECK(snapshot);
    return std::move(*snapshot);
}

void checkHit(const TerrainCollisionSnapshot& snapshot,
               Point3F start,
               Point3F end,
               Point3F position,
               Int32 part,
               Int32 material,
               float time) {
    CollisionSurfaceList hits;
    test::check(snapshot.line(start, end, hits),
                std::format("terrain miss from ({},{},{}) to ({},{},{})",
                            start.x,
                            start.y,
                            start.z,
                            end.x,
                            end.y,
                            end.z));
    CHECK(hits.surfaces.size() == 1);
    const auto& hit = hits.surfaces.front();
    CHECK((hit.position - position).len() < .001f);
    CHECK(hit.part == part && hit.material == material);
    CHECK(std::abs(hit.time - time) < .001f);
    CHECK(std::abs(hit.normal.x) < .001f && std::abs(hit.normal.y) < .001f && hit.normal.z > 0);
}

void voidEntry() {
    const auto snapshot = terrain();
    // Each segment crosses z=0 in the void, then enters terrain above it.
    // The old walker carried the outside start into the first resident block.
    const std::array<std::pair<Point3F, Point3F>, 5> misses{{
        {{-8, 2, -1}, {6, 2, 1}},
        {{24, 2, -1}, {10, 2, 1}},
        {{2, -8, -1}, {2, 6, 1}},
        {{2, 24, -1}, {2, 10, 1}},
        {{-16000, -16000, -1}, {6, 6, 1}},
    }};
    for (const auto& [start, end] : misses) {
        CollisionSurfaceList hits;
        CollisionSurface sentinel;
        sentinel.part = 123;
        hits.surfaces.push_back(sentinel);
        CHECK(!snapshot.line(start, end, hits));
        CHECK(hits.surfaces.size() == 1 && hits.surfaces.front().part == 123);
        CHECK(!snapshot.tube(start, end, .25f, hits));
        CHECK(hits.surfaces.size() == 1 && hits.surfaces.front().part == 123);
    }
    // Entry must still find real contacts inside the terrain in each direction.
    checkHit(snapshot, {-8, 2, 2}, {10, 2, -1}, {4, 2, 0}, 0, 3, 2.f / 3);
    checkHit(snapshot, {24, 2, 2}, {6, 2, -1}, {12, 2, 0}, 0, 3, 2.f / 3);
    checkHit(snapshot, {2, -8, 2}, {2, 10, -1}, {2, 4, 0}, 0, 3, 2.f / 3);
    checkHit(snapshot, {2, 24, 2}, {2, 6, -1}, {2, 12, 0}, 0, 3, 2.f / 3);
    CollisionSurfaceList hits;
    CHECK(!snapshot.line({-16000, -16000, 2}, {-15900, -15900, -2}, hits));
}

void seamEndpoints() {
    const auto xSeam = terrain({0, 1, 0, 1});
    // A final endpoint on a seam belongs to the interval just traversed, even
    // when the next block has a different authored height or is absent.
    checkHit(xSeam, {2, 2, 4}, {8, 2, -4}, {5, 2, 0}, 0, 3, .5f);
    checkHit(xSeam, {14, 2, 68}, {8, 2, 60}, {11, 2, 64}, 1, 7, .5f);
    checkHit(xSeam, {2, 2, 4}, {0, 2, -4}, {1, 2, 0}, 0, 3, .5f);
    checkHit(xSeam, {14, 2, 68}, {16, 2, 60}, {15, 2, 64}, 1, 7, .5f);
    const auto ySeam = terrain({0, 0, 1, 1});
    checkHit(ySeam, {2, 2, 4}, {2, 8, -4}, {2, 5, 0}, 0, 3, .5f);
    checkHit(ySeam, {2, 14, 68}, {2, 8, 60}, {2, 11, 64}, 1, 7, .5f);
    checkHit(ySeam, {2, 2, 4}, {8, 8, -4}, {5, 5, 0}, 0, 3, .5f);
    const auto absent = terrain({0, -1, 0, -1});
    checkHit(absent, {2, 2, 4}, {8, 2, -4}, {5, 2, 0}, 0, 3, .5f);
    CollisionSurfaceList hits;
    CHECK(!xSeam.line({2, 2, 32}, {14, 2, 32}, hits));
}

void boundaryTriangles() {
    const auto snapshot = terrain({0, 0, 0, 0}, true);
    // The raised vertex is (8,4,10). These rays stay over the flat triangle
    // while starting at (8,0), where multiple crossing streams meet at t=0.
    CollisionSurfaceList hits;
    CHECK(!snapshot.line({8, 0, 5}, {4, 2, 5}, hits));
    CHECK(!snapshot.tube({8, 0, 5}, {4, 2, 5}, .25f, hits));
    CHECK(!snapshot.line({16, 0, 5}, {12, 2, 5}, hits));
    checkHit(snapshot, {8, 0, 5}, {4, 2, -5}, {6, 1, 0}, 0, 3, .5f);
}

int testVoidEntry() {
    return test::run(voidEntry);
}

int testSeamEndpoints() {
    return test::run(seamEndpoints);
}

int testBoundaryTriangles() {
    return test::run(boundaryTriangles);
}
}

OPENT1_REGISTER_TEST("world", "terrain_void_entry", testVoidEntry);
OPENT1_REGISTER_TEST("world", "terrain_seam_endpoints", testSeamEndpoints);
OPENT1_REGISTER_TEST("world", "terrain_boundary_triangles", testBoundaryTriangles);
