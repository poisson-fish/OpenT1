#include "test_registry.hpp"
#include <ranges>
#include "ts_test_support.hpp"
#include <opent1/core/filename.hpp>
#include <opent1/game/player.hpp>
#include <opent1/game/player_manager.hpp>
#include <opent1/world/ts_static_shape.hpp>
#include <cstdlib>

namespace {
using namespace opent1;
using namespace opent1::game;

void modelRendering() {
    ResourceManager resources;
    auto shape = test::ts::load(test::ts::shape());
    CHECK(shape);
    // RPG weapon exports can retain a tiny sphere far from their actual mesh.
    shape->center = {100, 0, 100};
    shape->radius = .01f;
    CHECK(resources.add("stale-bounds.dts", std::move(shape)));
    CHECK(resources.add("test.bmp", gfx::Bitmap::create(4, 4)));
    const auto loaded = world::TsShapeResources::load(resources, "stale-bounds.dts");
    CHECK(loaded);
    world::TsStaticView view;
    view.shapeToCamera.p.y = 10;
    view.shapeToCamera.flags |= RMat3F::Matrix_HasTranslation;
    view.automaticDetail = true;
    const auto visibility =
        world::tsSphereVisibility(view, loaded->shape->center, loaded->shape->radius);
    CHECK(visibility && (*visibility & 0x80U));
    const auto rendered = world::TsStaticShapeFrame::prepare(*loaded, view);
    CHECK(rendered && rendered->polygonCount() == 1);
    const auto pose = world::TsShapePose::sample(loaded->shape);
    CHECK(pose);
    const auto retained = world::TsStaticShapeFrame::prepare(*loaded, view, *pose, true);
    gfx::Palette palette;
    CHECK(retained);
    const auto batch = retained->hardware(&palette);
    CHECK(batch && !batch->meshes().empty());
    const auto software = retained->softwarePolygons(&palette);
    CHECK(software && software->size() == rendered->polygonCount());
    auto litResources = *loaded;
    auto litMaterials = litResources.materials.data();
    litMaterials.list.materials[0].flags = 0x403;
    litResources.materials = world::TsMaterialResources(std::move(litMaterials));
    auto litView = view;
    litView.lighting.emplace();
    litView.lighting->ambient = {.1f, .1f, .1f};
    world::TsLight sun;
    sun.type = world::TsLight::Type::DirectionalWrap;
    sun.shadowMap = std::make_shared<const gfx::DirectionalShadowMap>(
        gfx::DirectionalShadowMap{4, {}, 1, std::vector<UInt8>(16, 96)});
    litView.lighting->lights = {sun};
    const auto litReference = world::TsStaticShapeFrame::prepare(litResources, litView, *pose);
    const auto litSnapshot = world::TsStaticShapeFrame::prepare(litResources, litView, *pose, true);
    CHECK(litReference && litSnapshot);
    litView.lighting->lights[0].shadowMap = std::make_shared<const gfx::DirectionalShadowMap>(
        gfx::DirectionalShadowMap{4, {}, 1, std::vector<UInt8>(16, 255)});
    const auto expectedLighting = litReference->softwarePolygons(&palette);
    const auto frozenLighting = litSnapshot->softwarePolygons(&palette);
    CHECK(expectedLighting && frozenLighting && expectedLighting->size() == frozenLighting->size());
    for (auto i : std::views::iota(std::size_t{0}) | std::views::take_while([&](std::size_t i) {
                      return i < expectedLighting->size();
                  }))
        for (auto j : std::views::iota(std::size_t{0}) | std::views::take_while([&](std::size_t j) {
                          return j < (*expectedLighting)[i].vertices.size();
                      })) {
            const auto& a = (*expectedLighting)[i].vertices[j];
            const auto& b = (*frozenLighting)[i].vertices[j];
            CHECK(a.position == b.position && a.color == b.color && a.texture == b.texture);
        }
    view.shapeToCamera.p.x += 1;
    auto overridePose = *pose;
    CHECK(overridePose.overrideParentTransform(1, TMat3F(EulerF{0, 0, .1f}, {})));
    const auto moved = world::TsStaticShapeFrame::prepare(*loaded, view, overridePose, true);
    CHECK(moved);
    const auto movedBatch = moved->hardware(&palette);
    CHECK(movedBatch && movedBatch->meshes()[0].mesh == batch->meshes()[0].mesh);
    CHECK(world::tsHardwareCacheStats().bytes > 0 && world::tsHardwareCacheStats().hits > 0);
    world::trimTsHardwareMeshes(1);
    const auto limited = world::TsStaticShapeFrame::prepare(*loaded, view, overridePose, true);
    CHECK(limited);
    const auto limitedBatch = limited->hardware(&palette);
    CHECK(limitedBatch && limitedBatch->meshes().empty());
    world::trimTsHardwareMeshes();
    world::clearTsHardwareMeshes();
    CHECK(world::tsHardwareCacheStats().bytes == 0 && !batch->meshes()[0].mesh->vertices().empty());

    sim::SimManager manager;
    auto ownedPlayers = std::make_unique<PlayerManager>(false);
    auto* players = ownedPlayers.get();
    players->setId(PlayerManagerId);
    CHECK(manager.addObject(std::move(ownedPlayers)));
    CHECK(players->clientAdded("Owner", "male1", "rpgbase", true, 2049) == 2049);
    CHECK(players->clientAdded("Controller", "male1", "base", true, 2050) == 2050);
    auto ownedPlayer = std::make_unique<Player>();
    auto* player = ownedPlayer.get();
    CHECK(manager.addObject(std::move(ownedPlayer)));
    player->setOwnerClient(2049);
    player->setControlClient(2050);
    CHECK(player->getSkinBase() == "rpgbase");
    player->setOwnerClient(0); // Released ownership retains the original skin.
    CHECK(player->getSkinBase() == "rpgbase");
}

void installedModels() {
    const auto* assets = std::getenv("OPENT1_TEST_ASSETS");
    if (!assets || !std::filesystem::is_regular_file(resolveFilePath(assets, "RPG/Axe.dts"))) {
        return;
    }
    auto resources = std::make_shared<ResourceManager>();
    CHECK(world::registerTsLoaders(*resources));
    CHECK(gfx::registerBitmapFontLoaders(*resources));
    const std::array roots{resolveFilePath(assets, "RPG"),
                           resolveFilePath(assets, "RPG/Skins"),
                           resolveFilePath(assets, "base")};
    CHECK(resources->setSearchPaths(roots));
    CHECK(resources->addVolume("entities.vol"));
    CHECK(resources->addVolume("rpgbase.vol"));
    DataBlockManager dbm(true, resources);
    auto* armor = dynamic_cast<PlayerData*>(
        dbm.createDataBlock("ModelArmor", DataBlockManager::PlayerDataType));
    CHECK(armor);
    armor->fileName = "rpgmalehuman";
    armor->shape = resources->load<world::TsShape>("rpgmalehuman.dts");
    armor->animData[Player::ANIM_STAND] = {"root", -1, 1, 7};
    armor->boxNormalHeight = 2.3f;
    CHECK(armor->shape);
    sim::SimManager manager;
    auto ownedPlayer = std::make_unique<Player>();
    auto* player = ownedPlayer.get();
    player->bindDataBlockManager(&dbm);
    CHECK(player->setNetFlags(player->netFlags() | sim::SimNetObject::IsGhost));
    player->setDatFileName("ModelArmor");
    CHECK(manager.addObject(std::move(ownedPlayer)));
    gfx::Palette palette;
    for (auto& color : palette.palettes.front().colors) {
        color = {21, 54, 87, 0};
    }
    CHECK(palette.valid());
    for (const auto name : {"dagger", "sword", "axe"}) {
        auto* imageData = dynamic_cast<ItemImageData*>(
            dbm.createDataBlock(std::string(name) + "Image", DataBlockManager::ItemImageDataType));
        auto* item =
            dynamic_cast<ItemData*>(dbm.createDataBlock(name, DataBlockManager::ItemDataType));
        CHECK(imageData && item);
        imageData->shapeFile = name;
        imageData->shape = resources->load<world::TsShape>(std::string(name) + ".dts");
        imageData->mountPoint = 0;
        CHECK(imageData->shape);
        Int32 imageId = -1, itemId = -1;
        CHECK(dbm.lookupDataBlock(
            std::string(name) + "Image", DataBlockManager::ItemImageDataType, &imageId));
        CHECK(dbm.lookupDataBlock(name, DataBlockManager::ItemDataType, &itemId));
        item->imageId = imageId;
        CHECK(player->mountItem(itemId, 0));
        for (const bool first : {true, false}) {
            player->setPlayerControl(first);
            player->updateAnimation(0);
            player->setImageState(0, Player::ItemImageEntry::Activate, true);
            world::WorldShapeQuery query;
            CHECK(player->processQuery(query) && query.attachments.size() == 1);
            player->updateImageAnimation(player->getItemImageEntry(0), .5f);
            if (std::string_view(name) == "dagger") {
                // Its authored resting pose is below the first-person view;
                // sample the actual slash instead of moving its mount point.
                player->setImageState(0, Player::ItemImageEntry::Fire, true);
                player->updateImageAnimation(player->getItemImageEntry(0), .25f);
            }
            query = {};
            CHECK(player->processQuery(query) && query.attachments.size() == 1);
            const auto& image = query.attachments.front();
            CHECK(image.firstPerson == first);
            auto camera = player->getEyeTransform();
            if (!first) {
                camera.p.y -= 4;
            }
            camera.inverse();
            world::TsStaticView view;
            m_mul(image.shapeToWorld, camera, view.shapeToCamera);
            view.nearDistance = .01f;
            view.automaticDetail = true;
            const auto prepared =
                world::TsStaticShapeFrame::prepare(image.resources, view, image.pose);
            CHECK(prepared);
            std::printf("RPG %s: %s view, %zu polygons\n",
                        name,
                        first ? "first-person" : "third-person",
                        prepared->polygonCount());
            CHECK(prepared->polygonCount() > 0);
            const auto hardware = prepared->hardware(&palette);
            CHECK(hardware && hardware->textures().size() > 1);
            const auto& rgba = hardware->textures()[1].rgba;
            // Embedded palettes in these weapons differ from the active
            // mission palette. Native OpenGL always uses the latter.
            CHECK(rgba[0] == std::byte{21} && rgba[1] == std::byte{54} && rgba[2] == std::byte{87});
        }
    }
}

void liveDatablockReplacement() {
    auto resources = std::make_shared<ResourceManager>();
    CHECK(resources->add("test.dts", test::ts::load(test::ts::shape())));
    CHECK(resources->add("test.bmp", gfx::Bitmap::create(4, 4)));
    DataBlockManager blocks(false, resources);
    constexpr auto group = DataBlockManager::StaticShapeDataType;
    auto* source = dynamic_cast<ShapeBaseData*>(blocks.createDataBlock("Original", group));
    CHECK(source);
    source->fileName = "test.dts";
    ShapeBase shape;
    shape.bindDataBlockManager(&blocks);
    CHECK(shape.initResources(source));
    std::weak_ptr<GameBaseData> owner = blocks.retainDataBlock(source);
    CHECK(!owner.expired());
    const auto snapshot = blocks.captureSnapshot();
    CHECK(blocks.createBlock(group, 0, 1));
    CHECK(!owner.expired() && shape.getDatPtr() == source);
    blocks.restoreSnapshot(snapshot);
    blocks.reset();
    CHECK(!owner.expired() && shape.getDatPtr() == source);
    CHECK(shape.initResources(source));
    CHECK(!owner.expired() && shape.getDatPtr() == source);
    // Force a new material preparation after the original registry owner leaves.
    world::WorldShapeQuery query;
    CHECK(shape.processQuery(query) && query.image);
    CHECK(query.image->resources.materials.textures().size() == 1);
    CHECK(source->fileName == "test.dts");
    CHECK(shape.initResources(nullptr));
    CHECK(owner.expired());
}
}

int opent1_test_model_rendering() {
    return opent1::test::run(modelRendering);
}

int opent1_test_installed_rpg_models() {
    return opent1::test::run(installedModels);
}

OPENT1_REGISTER_TEST("game", "model_rendering", opent1_test_model_rendering);
int opent1_test_live_datablock_replacement() {
    return opent1::test::run(liveDatablockReplacement);
}

OPENT1_REGISTER_TEST("game", "live_datablock_replacement", opent1_test_live_datablock_replacement);
OPENT1_REGISTER_TEST("fixpack", "installed_rpg_models", opent1_test_installed_rpg_models);
