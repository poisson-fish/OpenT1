#include "test_registry.hpp"
#include "ts_test_support.hpp"
#include <opent1/game/player.hpp>
#include <opent1/game/interior_shape.hpp>
#include <opent1/game/static_base.hpp>
#include <opent1/core/file_stream.hpp>
#include <opent1/gfx/sdl_gpu_surface.hpp>
#include <opent1/world/world_scene.hpp>
#include <cmath>
#include <cstdlib>

namespace {
using namespace opent1;
using namespace opent1::game;

struct MountedLightScene {
    std::shared_ptr<ResourceManager> resources = std::make_shared<ResourceManager>();
    DataBlockManager blocks{false, resources};
    sim::SimWorld world{nullptr, resources};
    Player* player{};
    ItemImageData* image{};
    Int32 itemId{};

    MountedLightScene() {
        auto body = test::ts::load(test::ts::shape());
        auto orb = test::ts::load(test::ts::shape());
        CHECK(body && orb);
        body->transforms[0].translation = {};
        body->names[1] = "dummy midback";
        orb->transforms[0].translation = {0, .25f, 0};
        orb->transforms[1].translation = {0, .5f, 0};
        orb->names[1] = "dummyalways muzzle";
        CHECK(resources->add("light-body.dts", std::move(body)));
        CHECK(resources->add("light-orb.dts", std::move(orb)));
        CHECK(resources->add("test.bmp", gfx::Bitmap::create(4, 4)));
        auto* armor = dynamic_cast<PlayerData*>(
            blocks.createDataBlock("LightArmor", DataBlockManager::PlayerDataType));
        image = dynamic_cast<ItemImageData*>(
            blocks.createDataBlock("LightImage", DataBlockManager::ItemImageDataType));
        auto* item = dynamic_cast<ItemData*>(
            blocks.createDataBlock("LightItem", DataBlockManager::ItemDataType));
        CHECK(armor && image && item);
        armor->fileName = "light-body.dts";
        image->shape = resources->load<world::TsShape>("light-orb.dts");
        image->mountPoint = 2;
        image->mountOffset = {0, 0, 1.8f};
        image->firstPerson = false;
        image->lightType = Player::PulsingLight;
        image->lightRadius = 15;
        image->lightColor = {.95f, .85f, .55f};
        image->lightTime = 2;
        item->imageId = image->dbmIndex;
        itemId = item->dbmIndex;
        auto owned = std::make_unique<Player>();
        player = owned.get();
        CHECK(player->setNetFlags(player->netFlags() | sim::SimNetObject::IsGhost));
        player->bindDataBlockManager(&blocks);
        player->setDatFileName("LightArmor");
        CHECK(world.manager().addObject(std::move(owned)));
        player->setPos({0, 4, 0});
        CHECK(player->mountItem(itemId, 0));
    }

    sim::SimSceneLighting lighting() {
        auto* lights = dynamic_cast<sim::SimLightSet*>(world.manager().findObject(sim::LightSetId));
        CHECK(lights);
        return lights->getSceneLighting();
    }
};

void mountedImageLights() {
    MountedLightScene scene;
    auto& manager = scene.world.manager();
    auto serverPlayer = std::make_unique<Player>();
    serverPlayer->bindDataBlockManager(&scene.blocks);
    serverPlayer->setDatFileName("LightArmor");
    auto* authoritative = serverPlayer.get();
    CHECK(manager.addObject(std::move(serverPlayer)));
    CHECK(authoritative->mountItem(scene.itemId, 0));
    // Hosted server objects must not double the client ghost's illumination.
    const auto checkIntensity = [&](Flt32 factor) {
        const auto lighting = scene.lighting();
        CHECK(lighting.lights.size() == 1);
        const auto& light = lighting.lights[0];
        CHECK(light.type == sim::SimLight::Type::Point && light.range == 15);
        CHECK(std::abs(light.intensity.red - .95f * factor) < .0001f);
        CHECK(std::abs(light.intensity.green - .85f * factor) < .0001f);
        CHECK(std::abs(light.intensity.blue - .55f * factor) < .0001f);
        return light.position;
    };
    CHECK((checkIntensity(.575f) - Point3F{0, 4.75f, 1.8f}).len() < .0001f);
    manager.advanceToTime(1);
    checkIntensity(1);
    manager.advanceToTime(3);
    checkIntensity(.15f);

    // Hidden first-person images still cast light; camera switches must not duplicate it.
    for (const bool firstPerson : {true, false, true}) {
        scene.player->setPlayerControl(firstPerson);
        checkIntensity(.15f);
    }
    scene.image->lightType = Player::ConstantLight;
    scene.player->setPos({10, 20, 3});
    scene.player->setRot({0, 0, std::numbers::pi_v<Flt32> / 2});
    const auto position = checkIntensity(1);
    TMat3F local, expected;
    CHECK(scene.player->getMuzzleTransform(0, &local));
    m_mul(local, scene.player->getTransform(), expected);
    CHECK((position - expected.p).len() < .0001f);
    CHECK((position - Point3F{0, 4.75f, 1.8f}).len() > 10);

    // A shape without a muzzle uses its authored image origin.
    scene.image->shape = scene.resources->load<world::TsShape>("light-body.dts");
    TMat3F mount;
    CHECK(scene.player->getImageTransform(0, &mount));
    m_mul(mount, scene.player->getTransform(), expected);
    CHECK((checkIntensity(1) - expected.p).len() < .0001f);

    // All supported mount slots must survive scene-light collection.
    for (Int32 slot = 1; slot < Player::MaxImageSlots; ++slot)
        CHECK(scene.player->mountItem(scene.itemId, slot));
    CHECK(scene.lighting().lights.size() == Player::MaxImageSlots);
    for (Int32 slot = 0; slot < Player::MaxImageSlots; ++slot)
        CHECK(scene.player->unmountItem(slot));
    CHECK(scene.lighting().lights.empty());

    scene.image->lightType = Player::WeaponFireLight;
    scene.image->lightTime = 2;
    CHECK(scene.player->mountItem(scene.itemId, 0));
    CHECK(scene.lighting().lights.empty());
    scene.player->setImageState(0, Player::ItemImageEntry::Fire);
    checkIntensity(1);
    manager.advanceToTime(4);
    checkIntensity(.5f);
    scene.player->setImageState(0, Player::ItemImageEntry::Fire);
    checkIntensity(1); // repeated fire restarts the fade
    manager.advanceToTime(6);
    CHECK(scene.lighting().lights.empty());
    scene.player->setImageState(0, Player::ItemImageEntry::Fire);
    CHECK(scene.player->unmountItem(0));
    CHECK(scene.player->mountItem(scene.itemId, 0));
    CHECK(scene.lighting().lights.empty()); // remount cannot retain a prior muzzle flash
    for (const auto type : {Player::PulsingLight, Player::WeaponFireLight}) {
        scene.image->lightType = type;
        scene.image->lightTime = 0;
        scene.player->setImageState(0, Player::ItemImageEntry::Fire);
        CHECK(scene.lighting().lights.empty());
    }
    scene.image->lightType = Player::NoLight;
    CHECK(scene.lighting().lights.empty());
    scene.image->lightType = Player::ConstantLight;
    checkIntensity(1);
    CHECK(manager.deleteObject(*scene.player));
    CHECK(scene.lighting().lights.empty());
}

void mountedLightReceiver() {
    MountedLightScene scene;
    scene.player->setPlayerControl(true);
    scene.image->lightType = Player::ConstantLight;
    auto shape = test::ts::load(test::ts::shape());
    CHECK(shape);
    shape->transforms[0].translation = {};
    shape->materials->materials[0].flags = 0x403; // lit texture
    shape->materials->materials[0].mapFile = "receiver.bmp";
    UInt8 normal = 0;
    for (unsigned i = 1; i < 256; ++i)
        if (world::decodeTsNormal(UInt8(i)).y < world::decodeTsNormal(normal).y)
            normal = UInt8(i);
    for (auto& mesh : shape->meshes)
        for (auto& vertex : mesh.vertices)
            vertex.normal = normal;
    CHECK(scene.resources->add("receiver.dts", std::move(shape)));
    auto bitmap = gfx::Bitmap::create(4, 4);
    CHECK(bitmap);
    std::ranges::fill(bitmap->pixels, std::byte{200});
    CHECK(scene.resources->add("receiver.bmp", std::move(bitmap)));
    auto* data = dynamic_cast<StaticShapeData*>(
        scene.blocks.createDataBlock("LightReceiver", DataBlockManager::StaticShapeDataType));
    CHECK(data);
    data->fileName = "receiver.dts";
    auto receiver = std::make_unique<StaticShape>();
    receiver->bindDataBlockManager(&scene.blocks);
    receiver->setDatFileName("LightReceiver");
    CHECK(receiver->setNetFlags(receiver->netFlags() | sim::SimNetObject::IsGhost));
    receiver->setPos({0, 8, 1.8f});
    CHECK(scene.world.manager().addObject(std::move(receiver)));

    sim::EnvironmentCamera camera;
    camera.viewport = {0, 0, 63, 63};
    camera.center = camera.focal = {32, 32};
    camera.farDistance = 100;
    camera.worldToCamera.p.z = -1.8f;
    camera.worldToCamera.flags |= RMat3F::Matrix_HasTranslation;
    gfx::Palette palette;
    for (unsigned i = 0; i < 256; ++i)
        palette.palettes[0].colors[i] = {UInt8(i), UInt8(i), UInt8(i), 0};
    CHECK(palette.buildRemapTables({.shadeShift = 4, .hazeLevels = 1}));
    const auto render = [&] {
        const auto frame = world::WorldSceneFrame::capture(scene.world, camera, {});
        CHECK(frame && frame->shapes().size() == 2); // hidden player body + receiver
        auto surface = gfx::SoftwareSurface::create(64, 64);
        CHECK(surface && surface->setPalette(palette));
        surface->clear(0);
        CHECK(frame->draw(*surface));
        CHECK(frame->hardware(palette));
        unsigned sum = 0;
        for (const auto pixel : surface->pixels())
            sum += std::to_integer<unsigned>(pixel);
        return sum;
    };
    const auto lit = render();
    CHECK(scene.player->unmountItem(0));
    const auto dark = render();
    CHECK(lit > dark);
    CHECK(scene.player->mountItem(scene.itemId, 0));
    CHECK(render() == lit);
    scene.player->setPos({100, 4, 0});
    CHECK(render() == dark); // moving the light beyond its radius restores darkness
}

template <class T> std::unique_ptr<T> interiorAsset(StreamIO& stream) {
    UInt32 size{};
    CHECK(stream.read(size) && size < 65536);
    std::vector<std::byte> bytes(size);
    CHECK(stream.readBytes(bytes));
    MemRWStream input{std::span<const std::byte>(bytes)};
    auto result = T::load(input, size);
    CHECK(result);
    return result;
}

UInt64 mapBrightness(const world::WorldSurfaceMesh& mesh) {
    CHECK(mesh.interiorMaps);
    UInt64 sum{};
    for (const auto& map : *mesh.interiorMaps)
        if (map)
            for (const auto color : map->samples)
                sum += color >> 12;
    return sum;
}

void mountedInteriorReceiver() {
    MountedLightScene scene;
    FileRStream file(OPENT1_INTERIOR_ASSET_FIXTURE);
    CHECK(file.setPosition(12));
    CHECK(scene.resources->add("room.dig", interiorAsset<world::InteriorGeometry>(file)));
    CHECK(scene.resources->add("room.dis", interiorAsset<world::InteriorShape>(file)));
    auto lighting = interiorAsset<world::InteriorLighting>(file);
    for (auto& surface : lighting->surfaces) {
        surface.map = 0x80001111U; // dark constant baked lighting
        surface.lightCount = surface.lightIndex = 0;
    }
    lighting->mapSlots = 0;
    lighting->states.clear();
    lighting->stateData.clear();
    lighting->lights.clear();
    CHECK(scene.resources->add("room.dil", std::move(lighting)));
    CHECK(scene.resources->add("room.dml", test::ts::loadMaterial(test::ts::material())));
    auto loaded = world::InteriorShapeResources::load(*scene.resources, "room.dis");
    CHECK(loaded);
    auto building = std::make_unique<InteriorShape>();
    auto* interior = building.get();
    interior->setFileName("room.dis");
    interior->setShapeResources(std::make_shared<world::InteriorShapeResources>(*loaded));
    interior->setShapePositionAndRotation({10, 20, 3}, {0, 0, .6f});
    CHECK(scene.world.manager().addObject(std::move(building)));
    const auto& bounds = loaded->geometry[0]->bounds;
    Point3F center;
    m_mul((bounds.fMin + bounds.fMax) * .5f, interior->getTransform(), center);
    scene.player->setPos(center - Point3F{0, .75f, 1.8f});
    scene.image->lightType = Player::ConstantLight;
    scene.player->setPlayerControl(true);
    const auto query = [&](bool enabled) {
        world::WorldSurfaceQuery request;
        if (enabled)
            request.lighting = std::make_shared<const sim::SimSceneLighting>(scene.lighting());
        CHECK(interior->processQuery(request) && !request.failed && request.image);
        return request.image->mesh;
    };
    const auto dark = query(false);
    const auto lit = query(true);
    CHECK(mapBrightness(*lit) > mapBrightness(*dark));
    CHECK(lit->interiorMesh == dark->interiorMesh); // maps change, geometry stays shared
    CHECK(query(true) == lit);                      // unchanged lights reuse the prepared maps
    interior->invalidateLightMaps();
    CHECK(mapBrightness(*query(true)) == mapBrightness(*lit)); // never accumulate light twice
    scene.player->setPos(center + Point3F{100, 100, 100});
    CHECK(mapBrightness(*query(true)) == mapBrightness(*dark));
    CHECK(scene.player->unmountItem(0));
    const auto removed = query(true);
    CHECK(mapBrightness(*removed) == mapBrightness(*dark));
    CHECK(!removed->interiorSceneLighting);
    CHECK(mapBrightness(*lit) > mapBrightness(*removed)); // old snapshots remain immutable
    scene.player->setPos(center - Point3F{0, .75f, 1.8f});
    CHECK(scene.player->mountItem(scene.itemId, 0));
    CHECK(mapBrightness(*query(true)) == mapBrightness(*lit));
    scene.image->lightType = Player::PulsingLight;
    CHECK(mapBrightness(*query(true)) < mapBrightness(*lit));
    scene.world.manager().advanceToTime(1);
    CHECK(mapBrightness(*query(true)) == mapBrightness(*lit));   // pulse updates cached maps
    CHECK(mapBrightness(*query(false)) == mapBrightness(*dark)); // lighting-disabled capture
}

void installedCryptReceivers(bool hardware) {
    const auto* assets = std::getenv("OPENT1_TEST_ASSETS");
    if (!assets ||
        !std::filesystem::is_regular_file(std::filesystem::path(assets) / "RPG" / "crypt0.vol")) {
        std::puts("SKIP: Ancient Crypt check requires the Wasteland installation");
        return;
    }
    MountedLightScene scene;
    CHECK(world::registerInteriorLoaders(*scene.resources));
    CHECK(world::registerTsLoaders(*scene.resources));
    CHECK(gfx::registerBitmapFontLoaders(*scene.resources));
    const auto root = std::filesystem::path(assets);
    const std::array paths{root / "RPG", root / "base"};
    CHECK(scene.resources->setSearchPaths(paths));
    for (const auto& directory : {root / "base", root / "RPG"}) {
        std::vector<std::filesystem::path> volumes;
        for (const auto& entry : std::filesystem::directory_iterator(directory))
            if (entry.path().extension() == ".vol")
                volumes.push_back(entry.path());
        std::ranges::sort(volumes);
        for (const auto& volume : volumes)
            CHECK(scene.resources->addVolume(volume.filename().string()));
    }
    scene.player->setPlayerControl(true);
    gfx::Palette palette;
    for (unsigned i = 0; i < 256; ++i)
        palette.palettes[0].colors[i] = {UInt8(i), UInt8(i), UInt8(i), 0};
    CHECK(palette.buildRemapTables({.shadeShift = 4, .hazeLevels = 1}));
    std::unique_ptr<platform::SdlApplication> application;
    std::unique_ptr<gfx::SdlGpuSurface> gpu;
    if (hardware) {
        application = std::make_unique<platform::SdlApplication>();
        CHECK(application->initialize({}) == SDL_APP_CONTINUE);
        platform::WindowConfig window;
        window.width = window.height = 128;
        window.hidden = true;
        gpu = gfx::SdlGpuSurface::create(application->createWindow(window), 128, 128);
        CHECK(gpu);
    }
    for (const auto* name : {"crypt0.0.dis", "lrock6.0.dis"}) {
        auto building = std::make_unique<InteriorShape>();
        auto* interior = building.get();
        interior->setFileName(name);
        interior->setShapePositionAndRotation({10, 20, 3}, {0, 0, .6f});
        CHECK(scene.world.manager().addObject(std::move(building)));
        const auto& geometry = *interior->shapeResources()->geometry[0];
        world::WorldSurfaceQuery base;
        CHECK(interior->processQuery(base) && !base.failed && base.image);
        const auto dark = base.image->mesh;
        bool illuminated = false;
        for (const auto& surface : geometry.surfaces) {
            if (!surface.renderable() || surface.vertexCount < 3)
                continue;
            Point3F center{};
            for (UInt32 i = 0; i < surface.vertexCount; ++i)
                center += geometry.boundaryPoint(
                    geometry.points[geometry.vertices[surface.firstVertex + i].point]);
            center /= Flt32(surface.vertexCount);
            const auto normal =
                Point3F(geometry.planes[surface.plane]) * (surface.planeFront() ? 1.f : -1.f);
            Point3F position;
            m_mul(center + normal, interior->getTransform(), position);
            scene.player->setPos(position - Point3F{0, .75f, 1.8f});
            world::WorldSurfaceQuery request;
            request.lighting = std::make_shared<const sim::SimSceneLighting>(scene.lighting());
            CHECK(interior->processQuery(request) && !request.failed && request.image);
            if (mapBrightness(*request.image->mesh) > mapBrightness(*dark)) {
                illuminated = true;
                CHECK(request.image->mesh->interiorMesh == dark->interiorMesh);
                TMat3F localCamera;
                const auto forward = -normal;
                const auto axis = std::abs(normal.z) > .9f ? Point3F{0, 1, 0} : Point3F{0, 0, 1};
                Point3F right, up;
                m_cross(forward, axis, right);
                right.normalize();
                localCamera.setRow(0, right);
                localCamera.setRow(1, forward);
                m_cross(right, forward, up);
                localCamera.setRow(2, up);
                localCamera.p = center + normal * 3;
                localCamera.flags = RMat3F::Matrix_HasRotation | RMat3F::Matrix_HasTranslation;
                sim::EnvironmentCamera camera;
                m_mul(localCamera, interior->getTransform(), camera.worldToCamera);
                camera.worldToCamera.inverse();
                camera.viewport = {0, 0, 127, 127};
                camera.center = camera.focal = {64, 64};
                camera.nearDistance = .1f;
                camera.farDistance = 1000;
                const auto render = [&] {
                    const auto frame = world::WorldSceneFrame::captureContainers(
                        scene.world, camera, {}, true, {}, hardware);
                    CHECK(frame && !frame->surfaces().empty());
                    auto software = gfx::SoftwareSurface::create(128, 128);
                    CHECK(software && software->setPalette(palette));
                    software->clear(0);
                    CHECK(frame->draw(*software));
                    const auto batch = frame->hardware(palette);
                    CHECK(batch);
                    UInt64 sum{};
                    if (gpu) {
                        std::vector<std::byte> rgba(128 * 128 * 4);
                        CHECK(gpu->render(*batch) && gpu->readback(rgba));
                        for (std::size_t i = 0; i < rgba.size(); ++i)
                            if (i % 4 != 3)
                                sum += std::to_integer<UInt8>(rgba[i]);
                    } else {
                        for (const auto pixel : software->pixels())
                            sum += std::to_integer<UInt8>(pixel);
                    }
                    return sum;
                };
                const auto litPixels = render();
                CHECK(scene.player->unmountItem(0));
                const auto darkPixels = render();
                CHECK(litPixels > darkPixels);
                CHECK(scene.player->mountItem(scene.itemId, 0));
                CHECK(render() == litPixels); // revisiting the same geometry refreshes GPU maps
                std::printf("%s: baked brightness %llu -> Lumos brightness %llu\n",
                            name,
                            static_cast<unsigned long long>(mapBrightness(*dark)),
                            static_cast<unsigned long long>(mapBrightness(*request.image->mesh)));
                break;
            }
        }
        CHECK(illuminated);
        CHECK(scene.world.manager().deleteObject(*interior));
    }
}
}

int testMountedImageLights() {
    return opent1::test::run(mountedImageLights);
}

int testMountedLightReceiver() {
    return opent1::test::run(mountedLightReceiver);
}

OPENT1_REGISTER_TEST("game", "mounted_image_lights", testMountedImageLights);
OPENT1_REGISTER_TEST("game", "mounted_light_receiver", testMountedLightReceiver);

int testMountedInteriorReceiver() {
    return opent1::test::run(mountedInteriorReceiver);
}

OPENT1_REGISTER_TEST("game", "mounted_interior_receiver", testMountedInteriorReceiver);

int testInstalledCryptReceivers() {
    return opent1::test::run([] {
        installedCryptReceivers(false);
    });
}

OPENT1_REGISTER_TEST("lumos", "ancient_crypt_receivers", testInstalledCryptReceivers);

int testInstalledCryptGpuReceivers() {
    return opent1::test::run([] {
        installedCryptReceivers(true);
    });
}

OPENT1_REGISTER_TEST("lumos_gpu", "ancient_crypt_gpu_receivers", testInstalledCryptGpuReceivers);
