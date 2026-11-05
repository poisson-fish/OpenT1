#include "test_registry.hpp"
#include "ts_test_support.hpp"
#include <opent1/game/player.hpp>
#include <opent1/world/world_scene.hpp>
#include <cmath>

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
    const auto source = world::TsShapeResources::load(*scene.resources, "receiver.dts");
    CHECK(source);
    auto receiverResources = *source;
    auto materials = receiverResources.materials.data();
    materials.textures[0] = scene.resources->load<gfx::Bitmap>("receiver.bmp");
    receiverResources.materials = world::TsMaterialResources(std::move(materials));
    auto receiver = std::make_unique<world::SimTsShape>();
    CHECK(receiver->setResources(std::move(receiverResources)));
    TMat3F placement;
    placement.p = {0, 8, 1.8f};
    placement.flags |= RMat3F::Matrix_HasTranslation;
    CHECK(receiver->setTransform(placement));
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
}

int testMountedImageLights() {
    return opent1::test::run(mountedImageLights);
}

int testMountedLightReceiver() {
    return opent1::test::run(mountedLightReceiver);
}

OPENT1_REGISTER_TEST("game", "mounted_image_lights", testMountedImageLights);
OPENT1_REGISTER_TEST("game", "mounted_light_receiver", testMountedLightReceiver);
