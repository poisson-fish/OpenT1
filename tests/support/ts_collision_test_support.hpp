#pragma once
#include "ts_test_support.hpp"
#include "ts_collision_corpus.hpp"
#include <opent1/world/ts_collision.hpp>

namespace opent1::test::ts_collision {
inline Point3F point(std::array<float, 3> p) {
    return {p[0], p[1], p[2]};
}

inline std::unique_ptr<world::TsShape> cube(std::size_t frame = 0) {
    auto s = std::make_unique<world::TsShape>();
    s->version = 8;
    s->radius = 3;
    const auto low = point(ts_collision_corpus::origin(frame)),
               scale = point(ts_collision_corpus::scale(frame));
    s->bounds = {low, low + scale * 200.f};
    s->nodes = {{0, -1, 0, 0, 0}, {1, 0, 0, 0, 1}};
    s->transforms.resize(2);
    s->names = {"bounds", "detail"};
    s->details = {{1, 1}};
    s->objects = {{1, 0, 0, 1, 0, 0, {}}};
    world::TsMesh mesh;
    mesh.version = 3;
    mesh.verticesPerFrame = 10;
    mesh.textureVerticesPerFrame = 1;
    mesh.textureVertices = {{}};
    mesh.frames = {
        {0, point(ts_collision_corpus::scale(frame)), point(ts_collision_corpus::origin(frame))}};
    for (const auto& v : ts_collision_corpus::vertices) {
        mesh.vertices.push_back({v[0], v[1], v[2], 0});
    }
    for (std::size_t i = 0; i < ts_collision_corpus::faces.size(); ++i) {
        const auto& f = ts_collision_corpus::faces[i];
        mesh.faces.push_back({{{{f[0], 0}, {f[1], 0}, {f[2], 0}}}, Int32(i % 3)});
    }
    s->meshes.push_back(std::move(mesh));
    return s;
}

inline Resource<world::TsShape> resource(std::unique_ptr<world::TsShape> shape) {
    ResourceManager manager;
    CHECK(world::registerTsLoaders(manager));
    CHECK(manager.add("collision.dts", std::move(shape)));
    return manager.load<world::TsShape>("collision.dts");
}

inline world::TsShapeCollision snapshot(Resource<world::TsShape> shape, Int32 detail = 0) {
    const auto pose = world::TsShapePose::sample(shape);
    CHECK(pose);
    auto geometry = world::TsShapeCollision::prepare(*pose, detail);
    CHECK(geometry);
    return std::move(*geometry);
}
} // namespace opent1::test::ts_collision
