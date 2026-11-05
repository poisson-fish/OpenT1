#include "test_registry.hpp"
#include "test_support.hpp"
#include "ts_test_support.hpp"
#include <opent1/game/game_app.hpp>
#include <opent1/game/fear_player_psc.hpp>
#include <opent1/game/observer_camera.hpp>
#include <opent1/game/interior_shape.hpp>
#include <opent1/game/player.hpp>
#include <opent1/world/world_collision_sim.hpp>
#include <opent1/world/world_scene.hpp>
#include <opent1/world/ts_collision_sim.hpp>
#include <opent1/gfx/sdl_gpu_surface.hpp>
#include <cmath>
#include <cstdlib>
#include <filesystem>

namespace {
using namespace opent1;
using namespace opent1::game;

class CameraWall final : public sim::SimContainer {
public:
    explicit CameraWall(Box3F box, bool trigger = false) {
        setType(StaticObjectType);
        auto collision = std::make_shared<sim::SimCollisionBoxImage>();
        collision->box = box;
        image = std::move(collision);
        image->trigger = trigger;
        CHECK(setBoundingBox(box));
    }

    CameraWall(std::shared_ptr<sim::SimCollisionImage> collision, Box3F bounds)
        : image(std::move(collision)) {
        setType(StaticObjectType);
        CHECK(setBoundingBox(bounds));
    }

    bool onAdd() override {
        return SimContainer::onAdd() &&
               dynamic_cast<sim::SimContainer*>(getManager()->findObject(sim::RootContainerId))
                   ->addObject(*this);
    }

    bool processQuery(sim::SimQuery& query) override {
        if (auto* collision = dynamic_cast<sim::SimCollisionImageQuery*>(&query)) {
            collision->images = {image};
            return true;
        }
        return SimContainer::processQuery(query);
    }

    std::shared_ptr<sim::SimCollisionImage> image;
};

void cameraCollision() {
    sim::SimWorld world;
    auto& manager = world.manager();
    auto owned = std::make_unique<GameBase>();
    auto* subject = owned.get();
    CHECK(manager.addObject(std::move(owned)));
    const auto place = [&](Box3F box, bool trigger = false) {
        auto wall = std::make_unique<CameraWall>(box, trigger);
        auto* result = wall.get();
        CHECK(manager.addObject(std::move(wall)));
        return result;
    };
    TMat3F camera;
    CHECK(subject->validateEyePoint(&camera, 4) == 4 && camera.p == Point3F(0, -4, 0));

    // A corner beside the center ray still intersects the camera's clearance.
    auto* wall = place({{.01f, -3, -1}, {1, -2, 1}});
    camera.identity();
    const auto cornerDistance = subject->validateEyePoint(&camera, 4);
    CHECK(cornerDistance > 1.9f && cornerDistance < 2);
    CHECK(camera.p.y > -2 && camera.p.x == 0 && camera.p.z == 0);
    wall->deleteObject();

    // Ordinary walls preserve the established 0.025 clearance.
    wall = place({{-2, -3, -1}, {2, -2, 1}});
    camera.identity();
    CHECK(std::abs(subject->validateEyePoint(&camera, 4) - 1.975f) < .001f);
    // Orbit cameras use the same collision path as the player chase camera.
    auto observer = std::make_unique<ObserverCamera>();
    auto* orbit = observer.get();
    CHECK(manager.addObject(std::move(observer)));
    orbit->setOrbitMode(nullptr, {}, {}, 4, 4, 4);
    orbit->update(0, 32, nullptr);
    CHECK(std::abs(orbit->getEyeTransform().p.y + 1.975f) < .001f);
    wall->deleteObject();

    // Legacy sphere line hits do not populate time, and their normals are not unit length.
    auto sphere = std::make_shared<sim::SimCollisionSphereImage>();
    sphere->center = {0, -2, 0};
    sphere->radius = .5f;
    auto sphericalWall = std::make_unique<CameraWall>(sphere, Box3F{{-1, -3, -1}, {1, -1, 1}});
    auto* spherical = sphericalWall.get();
    CHECK(manager.addObject(std::move(sphericalWall)));
    camera.identity();
    CHECK(std::abs(subject->validateEyePoint(&camera, 4) - 1.475f) < .001f);
    spherical->deleteObject();

    // Native TS faces report their surface position for both line and tube hits.
    ResourceManager resources;
    auto shape = test::ts::load(test::ts::shape());
    CHECK(shape);
    shape->transforms[0].translation = {};
    // Face the anchor, which sweeps backward along -Y.
    for (auto& mesh : shape->meshes)
        for (auto& face : mesh.faces)
            std::swap(face.corners[1], face.corners[2]);
    CHECK(resources.add("camera-face.dts", std::move(shape)));
    const auto pose = world::TsShapePose::sample(resources.load<world::TsShape>("camera-face.dts"));
    CHECK(pose);
    const auto geometry = world::TsShapeCollision::shared(*pose, 0);
    CHECK(geometry);
    auto face = std::make_shared<world::SimTsCollisionImage>(geometry);
    face->transform = TMat3F(EulerF{}, {0, -2, 0});
    auto faceWall = std::make_unique<CameraWall>(face, Box3F{{-1, -2, -1}, {1, -2, 1}});
    auto* native = faceWall.get();
    CHECK(manager.addObject(std::move(faceWall)));
    camera.identity();
    const auto nativeDistance = subject->validateEyePoint(&camera, 4);
    test::check(std::abs(nativeDistance - 1.975f) < .001f,
                std::format("TS camera distance {} != 1.975", nativeDistance));
    native->deleteObject();

    // Starting in contact cannot allow the camera to pass through the wall.
    wall = place({{-.01f, -.01f, -1}, {1, 1, 1}});
    camera.identity();
    CHECK(subject->validateEyePoint(&camera, 4) == 0 && camera.p == Point3F{});
    wall->deleteObject();

    wall = place({{-2, -3, -1}, {2, -2, 1}}, true);
    camera.identity();
    CHECK(subject->validateEyePoint(&camera, 4) == 4);
    wall->deleteObject();

    // Real heightfield collision at grazing incidence (dot < the old .01 cutoff).
    const std::array<Flt32, 4> heights{};
    const std::array<world::TerrainCollisionCell, 1> cells{};
    auto terrain = world::TerrainCollision::prepare(1, 1, 16, heights, cells);
    CHECK(terrain);
    CHECK(
        manager.addObject(std::make_unique<world::SimWorldCollider>(world::SimTerrainCollisionImage(
            std::make_shared<const world::TerrainCollision>(std::move(*terrain))))));
    camera = TMat3F(EulerF{.005f, 0, 0}, {8, 8, .03f});
    const auto grazingDistance = subject->validateEyePoint(&camera, 8);
    CHECK(grazingDistance > .9f && grazingDistance < 1.1f);
    CHECK(camera.p.z >= .0249f);

    const std::array<Flt32, 4> slopeHeights{0, 0, 4, 4};
    auto slope = world::TerrainCollision::prepare(1, 1, 16, slopeHeights, cells);
    CHECK(slope);
    CHECK(manager.addObject(std::make_unique<world::SimWorldCollider>(
        world::SimTerrainCollisionImage(
            std::make_shared<const world::TerrainCollision>(std::move(*slope))),
        TMat3F(EulerF{}, {20, 0, 0}))));
    camera = TMat3F(EulerF{.5f, 0, 0}, {28, 8, 2.1f});
    const auto slopeDistance = subject->validateEyePoint(&camera, 4);
    CHECK(slopeDistance > 0 && slopeDistance < .5f);
    CHECK((camera.p.z - camera.p.y * .25f) / std::sqrt(1.0625f) >= .0249f);
}

class NearWall final : public world::SimWorldSurface {
public:
    explicit NearWall(world::WorldSurfaceInstance image) : SimWorldSurface(std::move(image)) {}

    bool processQuery(sim::SimQuery& query) override {
        if (auto* surface = dynamic_cast<world::WorldSurfaceQuery*>(&query);
            surface && surface->camera) {
            camera = *surface->camera;
        }
        return SimWorldSurface::processQuery(query);
    }

    sim::EnvironmentCamera camera;
};

void cameraNearPlane() {
    FearGameApp app;
    CHECK(app.init() && app.fearPlugin().createClientWorld());
    auto& cg = app.fearPlugin().clientGlobals();
    auto owned = std::make_unique<GameBase>();
    auto* subject = owned.get();
    CHECK(cg.manager->addObject(std::move(owned)));
    cg.playerPSC->setControlObject(subject);
    cg.playerPSC->setFirstPerson(true);
    auto bitmap = gfx::Bitmap::create(2, 2);
    CHECK(bitmap);
    std::ranges::fill(bitmap->pixels, std::byte{200});
    CHECK(app.resources().add("camera-wall.bmp", std::move(bitmap)));
    world::TsMaterialList list;
    list.details = list.materialsPerDetail = 1;
    list.materials.push_back({.flags = 3, .mapFile = "camera-wall.bmp"});
    auto materials = world::TsMaterialResources::load(app.resources(), list);
    CHECK(materials);
    auto mesh = std::make_shared<world::WorldSurfaceMesh>();
    mesh->materials = *materials;
    mesh->bounds = {{-1, .3f, -1}, {1, .3f, 1}};
    const std::array<Point3F, 4> points{{{-1, .3f, -1}, {-1, .3f, 1}, {1, .3f, 1}, {1, .3f, -1}}};
    for (const auto indices : {std::array{0, 1, 2}, std::array{0, 2, 3}}) {
        world::StaticSurfaceTriangle triangle;
        for (unsigned i = 0; i < 3; ++i)
            triangle.vertices[i].position = points[indices[i]];
        mesh->triangles.push_back(triangle);
    }
    auto wall = std::make_unique<NearWall>(world::WorldSurfaceInstance{mesh, {}});
    auto* probe = wall.get();
    CHECK(cg.manager->addObject(std::move(wall)));
    auto surface = gfx::SoftwareSurface::create(320, 240);
    CHECK(surface && app.renderFrame(*surface));
    CHECK(probe->camera.nearDistance == .2f);
    CHECK(surface->bitmap().row(120)[160] == std::byte{200});
    gfx::HardwareBatch hardware;
    CHECK(app.renderFrame(*surface, &hardware) && !hardware.draws().empty());
    CHECK(probe->camera.nearDistance == .2f);

    cg.playerPSC->setFirstPerson(false);
    CHECK(app.renderFrame(*surface));
    CHECK(probe->camera.nearDistance == .5f);
    CHECK(surface->bitmap().row(120)[160] == std::byte{0});
    auto observer = std::make_unique<ObserverCamera>();
    auto* camera = observer.get();
    CHECK(cg.manager->addObject(std::move(observer)));
    cg.playerPSC->setControlObject(camera);
    cg.playerPSC->setFirstPerson(true);
    CHECK(app.renderFrame(*surface));
    CHECK(probe->camera.nearDistance == .5f);

    // Third-person projection must also keep a close solid wall opaque.
    cg.playerPSC->setControlObject(subject);
    cg.playerPSC->setFirstPerson(false);
    auto collision = std::make_unique<CameraWall>(Box3F{{-1, .3f, -1}, {1, 1, 1}});
    CHECK(cg.manager->addObject(std::move(collision)));
    CHECK(app.renderFrame(*surface));
    CHECK(probe->camera.nearDistance > 0 && probe->camera.nearDistance < .3f);
    CHECK(surface->bitmap().row(120)[160] == std::byte{200});
}

int collisionTest() {
    return test::run(cameraCollision);
}

int nearPlaneTest() {
    return test::run(cameraNearPlane);
}

class MineCameraProbe final : public InteriorShape {
public:
    bool processQuery(sim::SimQuery& query) override {
        if (auto* surface = dynamic_cast<world::WorldSurfaceQuery*>(&query);
            surface && surface->camera)
            camera = *surface->camera;
        return InteriorShape::processQuery(query);
    }

    std::optional<sim::EnvironmentCamera> camera;
};

void installedKeldrinCamera(bool hardware = false) {
    const auto* assets = std::getenv("OPENT1_TEST_ASSETS");
    if (!assets || !*assets) {
        std::puts("SKIP: Keldrin camera check requires OPENT1_TEST_ASSETS");
        return;
    }
    FearGameApp app;
    CHECK(app.init() && app.fearPlugin().createClientWorld());
    const auto root = std::filesystem::path(assets);
    const std::array paths{root / "RPG", root / "base"};
    CHECK(app.resources().setSearchPaths(paths));
    for (const auto& directory : {root / "base", root / "RPG"}) {
        std::vector<std::filesystem::path> volumes;
        for (const auto& entry : std::filesystem::directory_iterator(directory))
            if (entry.path().extension() == ".vol")
                volumes.push_back(entry.path());
        std::ranges::sort(volumes);
        for (const auto& volume : volumes)
            CHECK(app.resources().addVolume(volume.filename().string()));
    }
    auto& manager = *app.fearPlugin().clientGlobals().manager;
    auto owned = std::make_unique<GameBase>();
    auto* subject = owned.get();
    CHECK(manager.addObject(std::move(owned)));
    auto playerObject = std::make_unique<Player>();
    auto* player = playerObject.get();
    CHECK(manager.addObject(std::move(playerObject)));
    auto& psc = *app.fearPlugin().clientGlobals().playerPSC;
    psc.setControlObject(player);
    psc.setFirstPerson(false);
    psc.setCamDist(4);
    psc.setFov(90);
    std::unique_ptr<platform::SdlApplication> application;
    std::unique_ptr<gfx::SdlGpuSurface> gpu;
    if (hardware) {
        application = std::make_unique<platform::SdlApplication>();
        CHECK(application->initialize({}) == SDL_APP_CONTINUE);
        platform::WindowConfig window;
        window.width = 320;
        window.height = 240;
        window.hidden = true;
        gpu = gfx::SdlGpuSurface::create(application->createWindow(window), 320, 240);
        CHECK(gpu);
    }
    unsigned checked = 0, crossed = 0, clipped = 0, baselineClipped = 0, rendered = 0;
    for (const auto* name : {"M1.0.dis", "M2.0.dis", "M3.0.dis", "M4.0.dis"}) {
        auto building = std::make_unique<MineCameraProbe>();
        auto* mine = building.get();
        mine->setFileName(name);
        mine->setShapePositionAndRotation({-1649.5f, -1128.75f, 253.125f}, {0, 0, 1.57018f});
        CHECK(manager.addObject(std::move(building)));
        const auto& geometry = *mine->shapeResources()->geometry[0];
        auto inverse = mine->getTransform();
        inverse.inverse();
        sim::SimCollisionImageQuery collision;
        CHECK(mine->processQuery(collision) && !collision.images.empty());
        const auto solid = [&](Point3F world) {
            Point3F local;
            m_mul(world, inverse, local);
            const auto leaf = geometry.leafAt(local);
            CHECK(leaf);
            return geometry.leaves[*leaf].solid;
        };
        bool renderedMine = false;
        for (unsigned s = 0; s < geometry.surfaces.size(); ++s) {
            const auto& surface = geometry.surfaces[s];
            if (!surface.renderable() || surface.vertexCount < 3)
                continue;
            const auto localNormal =
                Point3F(geometry.planes[surface.plane]) * (surface.planeFront() ? 1.f : -1.f);
            if (std::abs(localNormal.z) > .25f)
                continue;
            Point3F center;
            for (UInt32 i = 0; i < surface.vertexCount; ++i)
                center += geometry.points[geometry.vertices[surface.firstVertex + i].point];
            center /= Flt32(surface.vertexCount);
            Point3F anchor, normal;
            m_mul(center + localNormal * .75f, mine->getTransform(), anchor);
            m_mul(localNormal, static_cast<const RMat3F&>(mine->getTransform()), normal);
            if (solid(anchor))
                continue;
            // Only sample anchors with enough room for a player's head. A
            // polygon centroid can lie directly on an adjoining solid wall.
            sim::SimCollisionSphereImage head;
            head.center = anchor;
            head.radius = .3f;
            CollisionSurfaceList headContact;
            if (sim::SimCollisionImage::test(head, *collision.images.front(), headContact))
                continue;
            for (const auto angle : {0.f, 1.f, 1.4f}) {
                Point3F tangent;
                m_cross(normal, Point3F{0, 0, 1}, tangent);
                tangent.normalize();
                const auto forward = normal * std::cos(angle) + tangent * std::sin(angle);
                Point3F right, up;
                m_cross(forward, Point3F{0, 0, 1}, right);
                right.normalize();
                m_cross(right, forward, up);
                TMat3F camera;
                camera.setRow(0, right);
                camera.setRow(1, forward);
                camera.setRow(2, up);
                camera.p = anchor;
                camera.flags = RMat3F::Matrix_HasRotation | RMat3F::Matrix_HasTranslation;
                const auto distance = subject->validateEyePoint(&camera, 4);
                const auto near = subject->validateCameraNearPlane(camera.p, .5f, {.5f, .375f});
                ++checked;
                crossed += solid(camera.p);
                bool nearClipped = false, oldClipped = false;
                for (const auto x : {-1.f, 1.f})
                    for (const auto y : {-1.f, 1.f}) {
                        nearClipped |= solid(camera.p + forward * near + right * (x * near) +
                                             up * (y * near * .75f));
                        oldClipped |=
                            solid(camera.p + forward * .5f + right * (x * .5f) + up * (y * .375f));
                    }
                baselineClipped += oldClipped;
                if (nearClipped && clipped++ == 0)
                    std::printf("Keldrin repro: %s surface=%u angle=%g distance=%g near=%g "
                                "anchor=%g %g %g\n",
                                name,
                                s,
                                angle,
                                distance,
                                near,
                                anchor.x,
                                anchor.y,
                                anchor.z);
                if (oldClipped && !renderedMine) {
                    EulerF rotation;
                    camera.angles(rotation);
                    player->setPos(anchor - Point3F{0, 0, 1.8f});
                    player->setRot({0, 0, rotation.z});
                    player->setViewPitch(rotation.x);
                    TMat3F playerCamera;
                    player->getCameraTransform(4, &playerCamera);
                    CHECK((playerCamera.p - camera.p).len() < .005f);
                    auto frame = gfx::SoftwareSurface::create(320, 240);
                    CHECK(frame && app.renderFrame(*frame));
                    CHECK(mine->camera && mine->camera->nearDistance < .5f);
                    CHECK(std::abs(mine->camera->nearDistance - near) < .002f);
                    gfx::HardwareBatch batch;
                    CHECK(app.renderFrame(*frame, &batch) && !batch.draws().empty());
                    CHECK(std::abs(mine->camera->nearDistance - near) < .002f);
                    if (gpu) {
                        std::vector<std::byte> pixels(320 * 240 * 4);
                        CHECK(gpu->render(batch) && gpu->readback(pixels));
                        unsigned colored = 0;
                        for (std::size_t i = 0; i < pixels.size(); i += 4)
                            colored += pixels[i] != std::byte{0} || pixels[i + 1] != std::byte{0} ||
                                       pixels[i + 2] != std::byte{0};
                        CHECK(colored > 0);
                    }
                    ++rendered;
                    renderedMine = true;
                }
            }
        }
        mine->deleteObject();
    }
    std::printf("Keldrin cameras: %u checked, %u centers in solid, %u near planes in solid (fixed "
                ".5 near: %u), %u rendered mines\n",
                checked,
                crossed,
                clipped,
                baselineClipped,
                rendered);
    CHECK(checked > 0 && crossed == 0 && clipped == 0 && baselineClipped > 0 && rendered == 4);
}

int installedCameraTest() {
    return test::run([] {
        installedKeldrinCamera();
    });
}

int installedCameraGpuTest() {
    return test::run([] {
        installedKeldrinCamera(true);
    });
}
}

OPENT1_REGISTER_TEST("game", "camera_collision", collisionTest);
OPENT1_REGISTER_TEST("game", "camera_near_plane", nearPlaneTest);
OPENT1_REGISTER_TEST("camera_assets", "keldrin_camera", installedCameraTest);
OPENT1_REGISTER_TEST("camera_gpu", "keldrin_camera_gpu", installedCameraGpuTest);
