#include "world_collision_corpus.hpp"
#include "ts_collision_test_support.hpp"
#include <opent1/world/world_collision_sim.hpp>
#include <opent1/world/world_scene.hpp>
#include <opent1/core/file_stream.hpp>
#include <limits>
#include <numbers>

namespace {
using namespace opent1;
using namespace opent1::world;
using namespace opent1::sim;
using namespace opent1::test::ts_collision;

Point3F point(std::array<float, 3> p) {
    return {p[0], p[1], p[2]};
}

bool close(Flt32 a, Flt32 b) {
    return std::abs(a - b) <= .0005f * std::max({1.f, std::abs(a), std::abs(b)});
}

void near(Point3F a, Point3F b, std::source_location at = std::source_location::current()) {
    test::check(close(a.x, b.x) && close(a.y, b.y) && close(a.z, b.z),
                std::format("({},{},{}) != ({},{},{})", a.x, a.y, a.z, b.x, b.y, b.z),
                at);
}

TMat3F at(Point3F p) {
    TMat3F t;
    t.p = p;
    t.flags |= RMat3F::Matrix_HasTranslation;
    return t;
}

template <class F> void throws(F&& f) {
    bool caught = false;
    try {
        f();
    } catch (const std::exception&) {
        caught = true;
    }
    CHECK(caught);
}

std::shared_ptr<const TerrainCollision> ground(bool hole = false) {
    const std::array<Flt32, 9> heights{};
    std::array<TerrainCollisionCell, 4> cells{};
    cells[0].hole = hole;
    const auto result = TerrainCollision::prepare(2, 2, 4, heights, cells);
    CHECK(result);
    return std::make_shared<const TerrainCollision>(*result);
}

struct InteriorData {
    std::vector<PlaneF> planes;
    std::vector<InteriorCollisionNode> nodes;
    std::vector<InteriorCollisionLeaf> leaves{{false, {}}, {true, {}}};
    std::vector<InteriorCollisionSurface> surfaces;

    InteriorData() {
        for (std::size_t i = 0; i < 6; ++i) {
            const auto p = world_collision_corpus::planes[i];
            planes.emplace_back(p[0], p[1], p[2], p[3]);
            nodes.push_back({UInt32(i), -1, i == 5 ? -2 : Int32(i + 1)});
            InteriorCollisionSurface s;
            s.plane = UInt32(i);
            for (auto v : world_collision_corpus::faces[i]) {
                s.vertices.push_back(point(world_collision_corpus::vertices[std::size_t(v)]));
            }
            surfaces.push_back(std::move(s));
            leaves[1].surfaces.push_back(UInt32(i));
        }
    }

    std::optional<InteriorCollision> prepare() const {
        return InteriorCollision::prepare(planes, nodes, leaves, surfaces);
    }
};

std::shared_ptr<const InteriorCollision> interior(Int32 material = -1) {
    InteriorData data;
    for (auto& s : data.surfaces) {
        s.material = material;
    }
    auto result = data.prepare();
    CHECK(result);
    return std::make_shared<const InteriorCollision>(std::move(*result));
}

void parity() {
    FileRStream file(OPENT1_WORLD_COLLISION_FIXTURE);
    std::array<char, 8> magic{};
    CHECK(file.readBytes(std::as_writable_bytes(std::span(magic))) &&
          std::string_view(magic.data(), 6) == "WCOLL1" && !magic[6] && !magic[7]);
    for (const auto& corners : world_collision_corpus::heights) {
        for (UInt32 split = 0; split < 2; ++split) {
            std::array<Flt32, 6> heights{};
            heights[split] = corners[0];
            heights[split + 1] = corners[1];
            heights[3 + split] = corners[2];
            heights[4 + split] = corners[3];
            const std::array<TerrainCollisionCell, 2> cells{};
            auto terrain = TerrainCollision::prepare(2, 1, 4, heights, cells);
            CHECK(terrain);
            for (int y = 0; y < 8; ++y) {
                for (int x = 0; x < 8; ++x) {
                    Flt32 height{};
                    Point3F normal;
                    CHECK(file.read(height) && file.read(normal.x) && file.read(normal.y) &&
                          file.read(normal.z));
                    const auto surface = terrain->surface({split * 4 + x * .5f, y * .5f});
                    CHECK(surface);
                    CHECK(close(surface->position.z, height));
                    near(surface->normal, normal);
                    CollisionSurfaceList list;
                    const auto lineHit =
                        terrain->line({surface->position.x, surface->position.y, 10},
                                      {surface->position.x, surface->position.y, -10},
                                      list);
                    if (!lineHit) {
                        std::fprintf(stderr,
                                     "terrain line split=%u x=%d y=%d height=%g\n",
                                     split,
                                     x,
                                     y,
                                     height);
                    }
                    CHECK(lineHit);
                    CHECK(
                        close(list.surfaces[0].position.z, height)); // both authored diagonal types
                }
            }
        }
    }
    const auto geometry = interior();
    std::size_t contacts{};
    for (std::size_t kind = 0; kind < 3; ++kind) {
        for (std::size_t i = 0; i < world_collision_corpus::samples; ++i) {
            const auto sample = world_collision_corpus::input(kind, i);
            CollisionSurfaceList list;
            const bool hit =
                kind == 0   ? geometry->line(point(sample.a), point(sample.b), list)
                : kind == 1 ? geometry->tube(point(sample.a), point(sample.b), sample.radius, list)
                            : geometry->sphere(point(sample.a), sample.radius, list);
            UInt32 count{};
            CHECK(file.read(count));
            if (list.surfaces.size() != count) {
                std::fprintf(stderr,
                             "world parity kind=%zu i=%zu contacts=%zu/%u\n",
                             kind,
                             i,
                             list.surfaces.size(),
                             count);
            }
            CHECK(hit == (count != 0) && list.surfaces.size() == count);
            contacts += count;
            for (const auto& s : list.surfaces) {
                CollisionSurface expected;
                CHECK(file.read(expected.time) && file.read(expected.position.x) &&
                      file.read(expected.position.y) && file.read(expected.position.z) &&
                      file.read(expected.normal.x) && file.read(expected.normal.y) &&
                      file.read(expected.normal.z) && file.read(expected.distance) &&
                      file.read(expected.surface) && file.read(expected.part));
                near(s.position, expected.position);
                near(s.normal, expected.normal);
                CHECK(close(s.distance, expected.distance));
                CHECK(s.surface == expected.surface && s.part == expected.part && s.material == -1);
                if (kind != 0) {
                    CHECK(close(s.time, expected.time));
                } // original line does not write time
            }
        }
    }
    std::size_t boxContacts{};
    for (std::size_t i = 0; i < world_collision_corpus::samples; ++i) {
        const auto sample = world_collision_corpus::box(i);
        const auto radii = point(sample.radii);
        const TMat3F transform(EulerF(sample.rotation[0], sample.rotation[1], sample.rotation[2]),
                               point(sample.position));
        CollisionSurfaceList list;
        const auto hit = geometry->box({-radii, radii}, transform, list);
        UInt32 expectedHit{}, contained{}, count{};
        Point3F step;
        CHECK(file.read(expectedHit) && file.read(contained) && file.read(step.x) &&
              file.read(step.y) && file.read(step.z) && file.read(count));
        if (hit != (expectedHit != 0) || list.surfaces.size() != count) {
            std::fprintf(stderr,
                         "BSP box case=%zu hit=%d/%u contacts=%zu/%u\n",
                         i,
                         hit,
                         expectedHit,
                         list.surfaces.size(),
                         count);
        }
        CHECK(hit == (expectedHit != 0) && list.contained == (contained != 0) &&
              list.surfaces.size() == count);
        boxContacts += count;
        if (hit) {
            near(list.stepVector, step);
        }
        for (UInt32 j = 0; j < count; ++j) {
            CollisionSurface expected;
            CHECK(file.read(expected.normal.x) && file.read(expected.normal.y) &&
                  file.read(expected.normal.z) && file.read(expected.distance) &&
                  file.read(expected.surface));
            const auto found =
                std::ranges::find(list.surfaces, expected.surface, &CollisionSurface::surface);
            CHECK(found != list.surfaces.end());
            near(found->normal, expected.normal);
            CHECK(close(found->distance, expected.distance));
        }
    }
    CHECK(contacts > 200 && UInt32(file.getPosition()) == file.getSize());
    std::printf("World collision: 384 terrain samples, 1028 interior queries, %zu primitive and "
                "%zu box contacts\n",
                contacts,
                boxContacts);
}

void terrainGeometry() {
    auto terrain = ground();
    CollisionSurfaceList list;
    list.surfaces.resize(1);
    list.surfaces[0].part = 123;
    CHECK(!terrain->line({1, 1, 2}, {2, 2, 1}, list) && list.surfaces[0].part == 123);
    CHECK(terrain->line({1, 1, 2}, {1, 1, -2}, list));
    CHECK(close(list.surfaces[0].time, .5f));
    near(list.surfaces[0].normal, {0, 0, 1});
    CHECK(terrain->line({1, 1, -2}, {1, 1, 2}, list));
    CHECK(close(list.surfaces[0].time, .5f));
    CHECK(terrain->tube({1, 1, 2}, {1, 1, -2}, .5f, list));
    CHECK(close(list.surfaces[0].time, .375f));
    CHECK(terrain->sphere({1, 1, -.25f}, .5f, list));
    CHECK(close(list.surfaces[0].distance, .25f));
    CHECK(!terrain->sphere({1, 1, .5f}, .5f, list));
    CHECK(!terrain->surface({8, 1}));
    CHECK(!terrain->surface({-.1f, 1}));
    CHECK(!ground(true)->line({1, 1, 2}, {1, 1, -2}, list));
    CHECK(ground(true)->line({1, 1, 2}, {1, 1, -2}, list, true));
    CHECK(!ground(true)->sphere({1, 1, 0}, 1, list));
    CHECK(!ground(true)->box({{-1, -1, -1}, {1, 1, 1}}, at({1, 1, 0}), list));
    CHECK(terrain->box({{-1, -1, -1}, {1, 1, 1}}, at({2, 2, .5f}), list));
    CHECK(close(list.surfaces[0].distance, .5f));
    CHECK(!terrain->box({{-1, -1, -1}, {1, 1, 1}}, at({2, 2, 1}), list));
    CHECK(!TerrainCollision::prepare(0, 1, 1, {}, {}));
    CHECK(!TerrainCollision::prepare(4097, 1, 1, {}, {}));
    std::array<Flt32, 4> heights{};
    std::array<TerrainCollisionCell, 1> cells{};
    CHECK(!TerrainCollision::prepare(1, 1, 0, heights, cells));
    cells[0].material = -1;
    CHECK(!TerrainCollision::prepare(1, 1, 1, heights, cells));
    CHECK(!TerrainCollision::prepare(1, 1, 1e-30f, heights, cells));
    cells[0].material = 0;
    heights[0] = std::numeric_limits<Flt32>::quiet_NaN();
    CHECK(!TerrainCollision::prepare(1, 1, 1, heights, cells));
    throws([&] {
        terrain->sphere({}, -1, list);
    });
    throws([&] {
        terrain->line({std::numeric_limits<Flt32>::infinity(), 0, 0}, {}, list);
    });
}

void interiorBoxesAndValidation() {
    auto geometry = interior(0);
    CollisionSurfaceList list;
    CHECK(geometry->box({{-.1f, -.1f, -.1f}, {.1f, .1f, .1f}}, {}, list));
    CHECK(list.contained && list.surfaces.empty());
    CHECK(geometry->box({{-.5f, -.5f, -.5f}, {.5f, .5f, .5f}}, at({1.2f, 0, 0}), list));
    CHECK(!list.contained && list.surfaces.size() == 1 && list.surfaces[0].surface == 0 &&
          list.surfaces[0].material == 0);
    CHECK(close(list.surfaces[0].position.x, 1));
    near(list.surfaces[0].normal, {1, 0, 0});
    CHECK(close(list.stepVector.z, .5f));
    CHECK(!geometry->box({{-.5f, -.5f, -.5f}, {.5f, .5f, .5f}}, at({1.5f, 0, 0}), list)); // tangent
    TMat3F rotated(EulerF(0, 0, std::numbers::pi_v<Flt32> * .25f), {1.6f, 1.6f, 0});
    CHECK(!geometry->box({{-.5f, -.5f, -.5f}, {.5f, .5f, .5f}},
                         rotated,
                         list)); // two plane overlaps, no solid intersection
    rotated.p = {1.1f, 1.1f, 0};
    CHECK(geometry->box({{-.5f, -.5f, -.5f}, {.5f, .5f, .5f}}, rotated, list));
    CHECK(list.surfaces.size() == 2);
    InteriorData data;
    data.nodes[5].back = 0;
    CHECK(!data.prepare());
    data = InteriorData{};
    data.nodes[0].front = 1;
    CHECK(!data.prepare());
    data = InteriorData{};
    data.leaves[1].surfaces.push_back(100);
    CHECK(!data.prepare());
    data = InteriorData{};
    data.planes[0].x = 2;
    CHECK(!data.prepare());
    data = InteriorData{};
    std::reverse(data.surfaces[0].vertices.begin(), data.surfaces[0].vertices.end());
    CHECK(!data.prepare());
    data = InteriorData{};
    data.surfaces[0].vertices[0].x = 9;
    CHECK(!data.prepare());
    data = InteriorData{};
    data.surfaces[0].material = -2;
    CHECK(!data.prepare());
    // A front-side solid leaf exercises reversed surface normals and the
    // original negative-distance sphere projection, beyond the cube fixture.
    InteriorCollisionSurface wall;
    wall.plane = 0;
    wall.planeFront = false;
    wall.material = 0;
    wall.vertices = {{0, -2, -2}, {0, 2, -2}, {0, 2, 2}, {0, -2, 2}};
    const std::array<PlaneF, 1> planes{{{1, 0, 0, 0}}};
    const std::array<InteriorCollisionNode, 1> nodes{{{0, -2, -1}}};
    const std::array<InteriorCollisionLeaf, 2> leaves{{{false, {}}, {true, {0}}}};
    auto halfSpace = InteriorCollision::prepare(planes, nodes, leaves, std::span(&wall, 1));
    CHECK(halfSpace);
    // RPG castle surfaces use the complete serialized UInt8 vertex range.
    auto polygon = wall;
    polygon.vertices.clear();
    for (int i = 0; i < 51; ++i) {
        const auto angle = Flt32(i) * 2.f * std::numbers::pi_v<Flt32> / 51.f;
        polygon.vertices.push_back({0, 2.f * std::cos(angle), 2.f * std::sin(angle)});
    }
    const auto castle = InteriorCollision::prepare(
        planes, nodes, leaves, std::span(&polygon, 1), InteriorSurfacePolicy::Native);
    CHECK(castle && castle->line({-1, 0, 0}, {1, 0, 0}, list));
    polygon.vertices.resize(256, polygon.vertices.front());
    CHECK(!InteriorCollision::prepare(
        planes, nodes, leaves, std::span(&polygon, 1), InteriorSurfacePolicy::Native));

    CHECK(halfSpace->line({-1, 0, 0}, {1, 0, 0}, list));
    near(list.surfaces[0].normal, {-1, 0, 0});
    CHECK(halfSpace->sphere({-.25f, 0, 0}, .5f, list));
    near(list.surfaces[0].position, {-.5f, 0, 0});
    CHECK(halfSpace->box({{-.5f, -.5f, -.5f}, {.5f, .5f, .5f}}, {}, list));
    near(list.surfaces[0].normal, {-1, 0, 0});
    data = InteriorData{};
    const auto repeated = data.surfaces[0];
    data.surfaces.assign(4097, repeated);
    data.leaves[1].surfaces.clear();
    for (UInt32 i = 0; i < 4097; ++i) {
        data.leaves[1].surfaces.push_back(i);
    }
    const auto crowded = data.prepare();
    CHECK(crowded);
    list.surfaces.resize(1);
    list.surfaces[0].part = 123;
    throws([&] {
        crowded->sphere({1.1f, 0, 0}, .5f, list);
    });
    CHECK(list.surfaces.size() == 1 && list.surfaces[0].part == 123);
    TMat3F bad;
    bad.m[0][0] = 2;
    throws([&] {
        geometry->box({{-1, -1, -1}, {1, 1, 1}}, bad, list);
    });
}

void dispatchAndLifetime() {
    SimTerrainCollisionImage terrain(ground(), std::array<SimCollisionMaterial, 1>{{{.25f, .5f}}});
    SimInteriorCollisionImage room(interior(5),
                                   std::array<SimCollisionMaterial, 6>{
                                       {{1, 1}, {1, 1}, {1, 1}, {1, 1}, {1, 1}, {.75f, .25f}}});
    auto ts =
        SimTsCollisionImage(std::make_shared<const TsShapeCollision>(snapshot(resource(cube()))));
    ts.transform = at({2, 2, .5f});
    terrain.transform = TMat3F(EulerF(.2f, 0, 0), {3, 4, 5});
    TMat3F placement;
    m_mul(ts.transform, terrain.transform, placement);
    ts.transform = placement;
    CollisionSurfaceList list;
    CHECK(SimCollisionImage::test(ts, terrain, list));
    CHECK(list.surfaces[0].material == 0 &&
          close(ts.getContactMaterial(terrain, 0)->friction, .25f));
    Point3F normal;
    m_mul(list.surfaces[0].normal, static_cast<const RMat3F&>(list.tWorld), normal);
    Point3F expected;
    m_mul(Point3F{0, 0, 1}, static_cast<const RMat3F&>(terrain.transform), expected);
    near(normal, expected);
    Point3F hit;
    m_mul(list.surfaces[0].position, list.tLocal, hit);
    CHECK(close(hit.z, -.5f)); // moving TS: tLocal identity
    CHECK(SimCollisionImage::test(terrain, ts, list));
    m_mul(list.surfaces[0].position, list.tLocal, hit);
    CHECK(close(hit.z, 0));
    ts.transform = at({1.2f, 0, 0});
    CHECK(SimCollisionImage::test(ts, room, list));
    CHECK(ts.getContactMaterial(room, 5)->friction == .75f);
    ts.transform = {}; // full cube shares boundary: no volume caps, contained
    CHECK(SimCollisionImage::test(ts, room, list) && list.contained && list.surfaces.empty());
    SimCollisionSphereImage sphere;
    sphere.center = {1.25f, 0, 0};
    sphere.radius = .5f;
    CHECK(SimCollisionImage::test(sphere, room, list));
    SimCollisionLineImage line;
    line.start = {3, 0, 0};
    line.end = {0, 0, 0};
    CHECK(SimCollisionImage::test(line, room, list));
    SimCollisionBackendImage broken(SimCollisionImage::Interior,
                                    std::make_shared<const SimCollisionBackendImage::Collider>(
                                        [](const auto&, const auto&, auto& result) {
                                            result.contained = true;
                                            return true;
                                        }));
    throws([&] {
        SimCollisionImage::test(line, broken, list);
    }); // LOS/sweeps always need a position
    SimWorld world;
    auto object = std::make_unique<SimWorldCollider>(SimTerrainCollisionImage(ground(true)));
    auto* provider = object.get();
    CHECK(world.manager().addObject(std::move(object)));
    CHECK(provider->getType() == TerrainObjectType && provider->getContainer());
    auto* root = dynamic_cast<SimContainer*>(world.manager().findObject(RootContainerId));
    CHECK(root);
    SimContainerQuery query;
    query.box = {{1, 1, 2}, {1, 1, -2}};
    SimCollisionInfo info;
    CHECK(!root->findLOS(query, info));
    CHECK(root->findLOS(query, info, SimCollisionImageQuery::Editor));
    auto retained = info.image;
    CHECK(provider->deleteObject());
    line.start = query.box.fMin;
    line.end = query.box.fMax;
    CHECK(SimCollisionImage::test(line, *retained, list));
    auto roomObject = std::make_unique<SimWorldCollider>(SimInteriorCollisionImage(interior()),
                                                         Box3F({-1, -1, -1}, {1, 1, 1}));
    CHECK(world.manager().addObject(std::move(roomObject)));
    query.box = {{3, 0, 0}, {0, 0, 0}};
    CHECK(root->findLOS(query, info));
}

SimTsShape* movingShape(SimWorld& world, Point3F displacement, Point3F placement = {}) {
    auto shape = cube();
    shape->defaultMaterials = 3;
    shape->sequences = {{0, 1, 1, 0, 0, 0, 0, 0}};
    shape->transforms.push_back({{}, displacement});
    shape->subSequences = {{0, 2, 0}};
    shape->keyframes = {{0, 0, 0x8000}, {1, 2, 0x8000}};
    shape->nodes[0].firstSubSequence = 0;
    shape->nodes[0].subSequenceCount = 1;
    TsShapeResources resources;
    resources.shape = resource(std::move(shape));
    resources.materials.list.details = 1;
    resources.materials.list.materialsPerDetail = 3;
    resources.materials.list.materials.resize(3);
    resources.materials.textures.resize(3);
    for (auto& m : resources.materials.list.materials) {
        m.flags = 1;
        m.index = 2;
    }
    auto object = std::make_unique<SimTsShape>();
    CHECK(object->setResources(std::move(resources)) && object->setCollisionDetail(0) &&
          object->setTransform(at(placement)));
    auto* result = object.get();
    CHECK(world.manager().addObject(std::move(object)));
    CHECK(result->instance()->addThread(0));
    return result;
}

struct TsMover : SimMovement {
    std::shared_ptr<SimTsCollisionImage> image;

    bool processQuery(SimQuery& query) override {
        if (auto* collision = dynamic_cast<SimCollisionImageQuery*>(&query)) {
            auto snapshot = std::make_shared<SimTsCollisionImage>(*image);
            snapshot->transform = getTransform();
            collision->images.push_back(std::move(snapshot));
            return true;
        }
        return SimMovement::processQuery(query);
    }
};

void movementIntegration() {
    {
        SimWorld world;
        CHECK(world.manager().addObject(
            std::make_unique<SimWorldCollider>(SimTerrainCollisionImage(ground()))));
        auto* shape = movingShape(world, {0, 0, -4}, {2, 2, 3});
        const auto result = shape->advanceAnimation(1);
        CHECK(result && !result->movementComplete && close(result->requestedTransform.p.z, -1));
        CHECK(shape->getTransform().p.z >= .99f && shape->getTransform().p.z <= 1.11f);
    }
    {
        SimWorld world;
        CHECK(world.manager().addObject(std::make_unique<SimWorldCollider>(
            SimInteriorCollisionImage(interior()), Box3F({-1, -1, -1}, {1, 1, 1}), at({3, 0, 0}))));
        auto* shape = movingShape(world, {4, 0, 0});
        CHECK(!shape->advanceAnimation(1)->movementComplete);
        CHECK(shape->getTransform().p.x >= .89f && shape->getTransform().p.x <= 1.01f);
    }
    {
        SimWorld world;
        // TS has only three material slots; room contacts use slot five. This
        // exercises material selection inside the ordinary movement response.
        std::array<SimCollisionMaterial, 6> materials{};
        materials[5] = {.5f, 0};
        CHECK(world.manager().addObject(std::make_unique<SimWorldCollider>(
            SimInteriorCollisionImage(interior(5), materials), Box3F({-1, -1, -1}, {1, 1, 1}))));
        auto mover = std::make_unique<TsMover>();
        mover->image = std::make_shared<SimTsCollisionImage>(
            std::make_shared<const TsShapeCollision>(snapshot(resource(cube()))),
            TsCollisionLevel::Faces,
            std::array<TsMaterial, 3>{});
        CHECK(mover->setLocalBoundingBox({{-1, -1, -1}, {1, 1, 1}}) &&
              mover->setPosition(at({3, 0, 0}), true));
        CHECK(mover->setLinearVelocity({-1, 1, 0}) && mover->setTimeSlice(1));
        auto* object = mover.get();
        CHECK(world.manager().addObject(std::move(mover)));
        auto* root = dynamic_cast<SimContainer*>(world.manager().findObject(RootContainerId));
        CHECK(root && root->addObject(*object));
        CHECK(object->setPosition(at({1.9f, 0, 0})));
        CHECK(object->getLinearVelocity().x >= 0 && object->getLinearVelocity().y < 1);
    }
}
}

int opent1_test_world_collision() {
    return opent1::test::run([] {
        parity();
        terrainGeometry();
        interiorBoxesAndValidation();
        dispatchAndLifetime();
        movementIntegration();
    });
}

#include "test_registry.hpp"
OPENT1_REGISTER_TEST("world", "world_collision", opent1_test_world_collision);
