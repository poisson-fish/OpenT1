#include "test_registry.hpp"
#include "test_support.hpp"
#include <opent1/world/shadow_receivers.hpp>
#include <opent1/gfx/render_stats.hpp>
#include <random>

namespace {
using namespace opent1;

std::shared_ptr<world::WorldSurfaceMesh> receiverMesh() {
    auto mesh = std::make_shared<world::WorldSurfaceMesh>();
    world::TsMaterialResources::Data materials;
    materials.list.details = materials.list.materialsPerDetail = 1;
    materials.list.materials.push_back({.flags = 1, .index = 200, .mapFile = {}});
    materials.textures.resize(1);
    mesh->materials = world::TsMaterialResources(std::move(materials));
    return mesh;
}

void quad(
    world::WorldSurfaceMesh& mesh, float x, float y, float size, float height, float slope = 0) {
    const std::array<Point3F, 4> points{{{x, y, height + x * slope},
                                         {x + size, y, height + (x + size) * slope},
                                         {x + size, y + size, height + (x + size) * slope},
                                         {x, y + size, height + x * slope}}};
    for (const auto ids : {std::array{0, 2, 1}, std::array{0, 3, 2}}) {
        world::StaticSurfaceTriangle t;
        for (std::size_t i = 0; i < 3; ++i)
            t.vertices[i].position = points[ids[i]];
        mesh.triangles.push_back(t);
    }
}

// Independent ray/triangle oracle. It does not clip or merge polygons and
// verifies both coverage and nearest depth, rather than a particular tessellation.
std::optional<float> ray(const world::WorldSurfaceMesh& mesh, Point3F origin, Point3F direction) {
    std::optional<float> nearest;
    for (const auto& t : mesh.triangles) {
        const auto a = t.vertices[0].position, b = t.vertices[1].position,
                   c = t.vertices[2].position;
        const auto u = b - a, v = c - a;
        Point3F n;
        m_cross(u, v, n);
        const auto denominator = m_dot(n, direction);
        if (std::abs(denominator) < 1e-8f)
            continue;
        const auto distance = m_dot(n, a - origin) / denominator;
        if (distance < 0 || distance > 100)
            continue;
        const auto p = origin + direction * distance - a;
        const double uu = m_dot(u, u), uv = m_dot(u, v), vv = m_dot(v, v), pu = m_dot(p, u),
                     pv = m_dot(p, v);
        const auto determinant = uu * vv - uv * uv;
        if (std::abs(determinant) < 1e-14)
            continue;
        const auto s = (pu * vv - pv * uv) / determinant, tv = (pv * uu - pu * uv) / determinant;
        if (s >= -1e-5 && tv >= -1e-5 && s + tv <= 1.00001 && (!nearest || distance < *nearest))
            nearest = distance;
    }
    return nearest;
}

bool inside(const sim::ShadowReceiver& receiver, Point3F point) {
    bool positive = false, negative = false;
    for (std::size_t i = 0; i < receiver.vertices.size(); ++i) {
        const auto a = receiver.vertices[i],
                   b = receiver.vertices[(i + 1) % receiver.vertices.size()];
        Point3F cross;
        m_cross(b - a, point - a, cross);
        const auto side = m_dot(cross, receiver.normal);
        positive |= side > 1e-6f;
        negative |= side < -1e-6f;
    }
    return !(positive && negative);
}

void receiverCoverage() {
    std::mt19937 random(0x531ad0);
    for (unsigned scene = 0; scene < 5; ++scene) {
        auto mesh = receiverMesh();
        for (unsigned y = 0; y < 16; ++y)
            for (unsigned x = 0; x < 16; ++x) {
                if (scene == 1 && x >= 6 && x < 10 && y >= 6 && y < 10)
                    continue; // A real hole must survive merging.
                quad(*mesh, float(x) * .25f - 2, float(y) * .25f - 2, .25f, 0);
            }
        if (scene >= 2)
            quad(*mesh, -1, -1, 2, 1, .25f);
        if (scene >= 3)
            quad(*mesh, -.75f, -.75f, 1.5f, 1, -.3f); // Crossing depth planes.
        if (scene == 4)
            quad(*mesh, -1, -1, 2, 1, .25f); // Duplicate coplanar input.
        const Point3F translation = scene == 4 ? Point3F{-2548.25f, -262.375f, 120} : Point3F{};
        const EulerF rotation = scene == 4 ? EulerF{.1f, 0, .3f} : EulerF{};
        TMat3F rotationInverse(rotation, {});
        rotationInverse.inverse();
        const std::array<world::WorldSurfaceInstance, 1> instances{
            {{mesh, TMat3F(rotation, translation)}}};
        const world::ShadowReceiverScene receivers(instances);
        for (unsigned view = 0; view < 8; ++view) {
            Point3F light{
                float(int(random() % 60) - 30) * .01f, float(int(random() % 60) - 30) * .01f, -1};
            light.normalize();
            const auto window = sim::makeShadowSourceWindow(
                light, {0, 0, 4}, 2, {{-1, -1, -1}, {1, 1, 1}}, RMat3F{});
            const auto result = receivers.collect(window, 100, translation);
            CHECK(result && !result->empty() && result->size() <= 100);
            for (unsigned y = 0; y < 17; ++y)
                for (unsigned x = 0; x < 17; ++x) {
                    const auto origin =
                        window.points[0] +
                        (window.points[1] - window.points[0]) * ((float(x) + .371f) / 17) +
                        (window.points[3] - window.points[0]) * ((float(y) + .619f) / 17);
                    Point3F localOrigin, localLight;
                    m_mul(origin, rotationInverse, localOrigin);
                    m_mul(light, static_cast<const RMat3F&>(rotationInverse), localLight);
                    const auto expected = ray(*mesh, localOrigin, localLight);
                    std::optional<float> actual;
                    unsigned coverage{};
                    for (const auto& receiver : *result) {
                        const auto depth = (receiver.constant - m_dot(receiver.normal, origin)) /
                                           m_dot(receiver.normal, light);
                        if (inside(receiver, origin + light * depth)) {
                            actual = depth;
                            ++coverage;
                        }
                    }
                    CHECK(bool(actual) == bool(expected));
                    CHECK(coverage <= 1);
                    if (expected)
                        CHECK(std::abs(*actual - *expected) < .002f);
                }
        }
    }
}

void receiverCacheAndBudget() {
    auto mesh = receiverMesh();
    quad(*mesh, -4, -4, 8, 0);
    const auto window = sim::makeShadowSourceWindow(
        {0, 0, -1}, {0, 0, 3}, 1, {{-.5f, -.5f, -.5f}, {.5f, .5f, .5f}}, RMat3F{});
    world::ShadowReceiverCache cache;
    std::array<world::WorldSurfaceInstance, 1> sources{{{mesh, {}}}};
    const world::ShadowReceiverScene first(sources, &cache);
    CHECK(first.collect(window));
    gfx::RenderWorkStats stats;
    {
        gfx::ScopedRenderWorkStats record(stats);
        const world::ShadowReceiverScene reused(sources, &cache);
        CHECK(reused.collect(window));
    }
    CHECK(stats.shadowIndexHits == 1 && stats.shadowIndexBuilds == 0);
    std::size_t budget = 0;
    CHECK(!first.collect(window, 100, {}, &budget) && budget == 0);
    auto invalid = window;
    invalid.right *= 2;
    CHECK(!first.collect(invalid));
    auto replacement = receiverMesh();
    quad(*replacement, -4, -4, 8, 1);
    sources[0].mesh = replacement;
    const world::ShadowReceiverScene changed(sources, &cache);
    const auto result = changed.collect(window);
    CHECK(result && !result->empty());
    for (const auto& receiver : *result)
        for (auto p : receiver.vertices)
            CHECK(std::abs(p.z - 1) < 1e-6f);
    std::weak_ptr<const world::WorldSurfaceMesh> lifetime;
    {
        auto temporary = receiverMesh();
        quad(*temporary, -1, -1, 2, 0);
        lifetime = temporary;
        const std::array<world::WorldSurfaceInstance, 1> source{{{temporary, {}}}};
        CHECK(world::ShadowReceiverScene(source, &cache).collect(window));
    }
    CHECK(lifetime.expired());
    cache.clear();
}
}

int opent1_test_shadow_receiver_coverage() {
    return opent1::test::run(receiverCoverage);
}

int opent1_test_shadow_receiver_cache() {
    return opent1::test::run(receiverCacheAndBudget);
}

OPENT1_REGISTER_TEST("world", "shadow_receiver_coverage", opent1_test_shadow_receiver_coverage);
OPENT1_REGISTER_TEST("world", "shadow_receiver_cache", opent1_test_shadow_receiver_cache);
