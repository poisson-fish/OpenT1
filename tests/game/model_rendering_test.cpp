#include "test_registry.hpp"
#include <ranges>
#include "ts_test_support.hpp"
#include <opent1/core/filename.hpp>
#include <opent1/gfx/render_limits.hpp>
#include <opent1/gfx/render_stats.hpp>
#include <opent1/game/player.hpp>
#include <opent1/game/player_manager.hpp>
#include <opent1/world/ts_static_shape.hpp>
#include <opent1/world/world_surfaces.hpp>
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

class ShadowActor : public sim::SimObject {
public:
    world::WorldShapeImage image;

    bool onAdd() override {
        return SimObject::onAdd() && addToSet(sim::RenderSetId);
    }

    bool processQuery(sim::SimQuery& query) override {
        if (auto* shapes = dynamic_cast<world::WorldShapeQuery*>(&query)) {
            shapes->image = image;
            return true;
        }
        return false;
    }
};

void playerShadows() {
    struct Preferences {
        bool players = Player::prefPlayerShadows;
        Int32 mask = ShapeBase::prefShadowDetailMask;

        ~Preferences() {
            Player::prefPlayerShadows = players;
            ShapeBase::prefShadowDetailMask = mask;
        }
    } preferences;

    Player::prefPlayerShadows = true;
    ShapeBase::prefShadowDetailMask = 0; // Saved vanilla preferences must not disable players.
    auto resources = std::make_shared<ResourceManager>();
    auto shape = test::ts::load(test::ts::shape());
    CHECK(shape);
    // A horizontal triangle gives a measurable downward silhouette.
    shape->bounds = {{-1, -1, 0}, {1, 1, 0}};
    for (auto& mesh : shape->meshes) {
        for (auto& vertex : mesh.vertices)
            std::swap(vertex.y, vertex.z);
        for (auto& frame : mesh.frames) {
            std::swap(frame.scale.y, frame.scale.z);
            std::swap(frame.origin.y, frame.origin.z);
        }
    }
    CHECK(resources->add("shadow-player.dts", std::move(shape)));
    CHECK(resources->add("test.bmp", gfx::Bitmap::create(4, 4)));
    DataBlockManager blocks(false, resources);
    auto* armor = dynamic_cast<PlayerData*>(
        blocks.createDataBlock("ShadowArmor", DataBlockManager::PlayerDataType));
    CHECK(armor);
    armor->fileName = "shadow-player.dts";
    sim::SimWorld world(nullptr, resources);
    auto owned = std::make_unique<Player>();
    auto* player = owned.get();
    CHECK(player->setNetFlags(player->netFlags() | sim::SimNetObject::IsGhost));
    player->bindDataBlockManager(&blocks);
    player->setDatFileName("ShadowArmor");
    CHECK(world.manager().addObject(std::move(owned)));
    player->setPos({0, 8, 2});

    auto floor = std::make_shared<world::WorldSurfaceMesh>();
    world::TsMaterialResources::Data materials;
    materials.list.details = materials.list.materialsPerDetail = 1;
    materials.list.materials.push_back({.flags = 1, .index = 200, .mapFile = {}});
    materials.textures.resize(1);
    floor->materials = world::TsMaterialResources(std::move(materials));
    const std::array<Point3F, 4> corners{{{-8, 0, 0}, {8, 0, 0}, {8, 16, 0}, {-8, 16, 0}}};
    for (const auto indices : {std::array{0, 2, 1}, std::array{0, 3, 2}}) {
        world::StaticSurfaceTriangle triangle;
        for (auto j : std::views::iota(0, 3))
            triangle.vertices[j].position = corners[indices[j]];
        floor->triangles.push_back(triangle);
    }
    const auto window =
        sim::makeShadowSourceWindow({0, 0, -1}, {0, 8, 2}, 2, {{-1, -1, 0}, {1, 1, 0}}, RMat3F{});
    auto slope = std::make_shared<world::WorldSurfaceMesh>(*floor);
    for (auto& triangle : slope->triangles)
        for (auto& vertex : triangle.vertices)
            vertex.position.z = vertex.position.x * .25f + 1;
    const std::array<world::WorldSurfaceInstance, 2> stacked{{{floor, {}}, {slope, {}}}};
    const auto projected = world::worldShadowGeometry(window, stacked, {0, 0, -1}, 100, .02f);
    CHECK(projected && !projected->polygons.empty());
    for (const auto& polygon : projected->polygons)
        for (const auto& vertex : polygon.vertices)
            CHECK(std::abs(vertex.position.z - vertex.position.x * .25f - 1.02f) < .0001f);
    // No shadow can spill past the authored edge of a narrow ledge.
    for (auto& triangle : slope->triangles)
        for (auto& vertex : triangle.vertices)
            vertex.position.x = std::min(vertex.position.x, 0.f);
    const std::array<world::WorldSurfaceInstance, 1> ledge{{{slope, {}}}};
    const auto clipped = world::worldShadowGeometry(window, ledge, {0, 0, -1}, 100, .02f);
    CHECK(clipped && !clipped->polygons.empty());
    for (const auto& polygon : clipped->polygons)
        for (const auto& vertex : polygon.vertices)
            CHECK(vertex.position.x <= .0001f);
    CHECK(world.manager().addObject(
        std::make_unique<world::SimWorldSurface>(world::WorldSurfaceInstance{floor, {}})));
    sim::EnvironmentCamera camera;
    camera.viewport = {0, 0, 63, 63};
    camera.center = camera.focal = {32, 32};
    camera.farDistance = 100;
    TMat3F placement;
    placement.setRow(0, {1, 0, 0});
    placement.setRow(1, {0, 0, -1});
    placement.setRow(2, {0, 1, 0});
    placement.p = {0, 8, 15};
    placement.flags = RMat3F::Matrix_HasRotation | RMat3F::Matrix_HasTranslation;
    placement.inverse();
    camera.worldToCamera = placement;
    gfx::Palette palette;
    for (auto i : std::views::iota(0, 256))
        palette.palettes[0].colors[i] = {UInt8(i), UInt8(i), UInt8(i), 0};

    world::WorldShapeImage body;
    for (const bool first : {false, true}) {
        player->setPlayerControl(first);
        world::WorldShapeQuery query;
        CHECK(player->processQuery(query) && query.image);
        CHECK(query.image->castShadow && query.image->drawShape == !first);
        body = *query.image;
        const auto scene = world::WorldSceneFrame::capture(world, camera, {}, false);
        CHECK(scene);
        const auto shadows = scene->shadows({.5f, 0, -1}); // No Planet object required.
        CHECK(shadows && shadows->size() == 1);
        const auto& shadow = shadows->front();
        CHECK(!shadow.geometry.polygons.empty());
        CHECK(std::ranges::any_of(shadow.bitmap.pixels, [](auto p) {
            return p > 0;
        }));
        for (const auto& polygon : shadow.geometry.polygons)
            for (const auto& vertex : polygon.vertices)
                CHECK(std::abs(vertex.position.z - .02f / std::sqrt(1.25f)) < .0001f);
        const std::array<sim::EnvironmentShadow, 1> inputs{{{shadow.geometry, shadow.bitmap}}};
        const auto plain = scene->hardware(palette);
        const auto shaded = scene->hardware(palette, inputs);
        CHECK(plain && shaded && shaded->vertices().size() > plain->vertices().size());
        auto a = gfx::SoftwareSurface::create(64, 64);
        auto b = gfx::SoftwareSurface::create(64, 64);
        CHECK(a && b && a->setPalette(palette) && b->setPalette(palette));
        CHECK(scene->draw(*a) && scene->draw(*b, inputs));
        unsigned darkened = 0;
        for (auto y : std::views::iota(0, 64))
            for (auto x : std::views::iota(0, 64))
                darkened += b->bitmap().row(y)[x] < a->bitmap().row(y)[x];
        CHECK(darkened > 0);
        if (first) {
            CHECK(scene->shapes().size() == 1 && scene->shapes()[0].polygonCount() == 0);
            auto resized = camera;
            resized.viewport = {0, 0, 127, 127};
            resized.center = resized.focal = {64, 64};
            const auto reframed = scene->reframed(resized);
            CHECK(reframed && reframed->shapes()[0].polygonCount() == 0);
            CHECK(reframed->shadows({0, 0, -1})->size() == 1);
        }
    }
    // RPG missions place actors thousands of units from the origin. Oblique
    // shadows must keep working while they move through those coordinates.
    sim::SimWorld translated(nullptr, resources);
    const Point3F offset{-2548.25f, -262.375f, 120};
    auto shiftedActor = std::make_unique<ShadowActor>();
    shiftedActor->image = body;
    shiftedActor->image.shapeToWorld.p += offset;
    auto* movingActor = shiftedActor.get();
    CHECK(translated.manager().addObject(std::move(shiftedActor)));
    CHECK(translated.manager().addObject(std::make_unique<world::SimWorldSurface>(
        world::WorldSurfaceInstance{floor, TMat3F(EulerF{}, offset)})));
    auto shiftedCamera = camera;
    auto shiftedPlacement = camera.worldToCamera;
    shiftedPlacement.inverse();
    shiftedPlacement.p += offset;
    shiftedPlacement.inverse();
    shiftedCamera.worldToCamera = shiftedPlacement;
    for (auto frame : std::views::iota(0, 64)) {
        movingActor->image.shapeToWorld.p.x = offset.x + Flt32(frame) / 64;
        const auto scene = world::WorldSceneFrame::capture(translated, shiftedCamera, {}, false);
        CHECK(scene);
        const auto shadows = scene->shadows({.6f, .45f, -.7f});
        CHECK(shadows && shadows->size() == 1 && !shadows->front().geometry.polygons.empty());
    }
    // A narrow receiver clips a valid local shadow fragment that collapses
    // when translated to RPG world coordinates. Keep the other fragments and
    // render the complete frame through both backends.
    sim::SimWorld sliverWorld(nullptr, resources);
    auto sliverActor = std::make_unique<ShadowActor>();
    sliverActor->image = body;
    sliverActor->image.shapeToWorld.p += offset;
    CHECK(sliverWorld.manager().addObject(std::move(sliverActor)));
    CHECK(sliverWorld.manager().addObject(std::make_unique<world::SimWorldSurface>(
        world::WorldSurfaceInstance{floor, TMat3F(EulerF{}, offset)})));
    auto sliver = std::make_shared<world::WorldSurfaceMesh>(*floor);
    for (auto& triangle : sliver->triangles) {
        for (auto& vertex : triangle.vertices) {
            vertex.position.y = 8 + vertex.position.y * 1e-7f;
            vertex.position.z = 1;
        }
    }
    CHECK(sliverWorld.manager().addObject(std::make_unique<world::SimWorldSurface>(
        world::WorldSurfaceInstance{sliver, TMat3F(EulerF{}, offset)})));
    const auto sliverScene = world::WorldSceneFrame::capture(sliverWorld, shiftedCamera, {}, false);
    CHECK(sliverScene);
    const auto sliverShadows = sliverScene->shadows({0, 0, -1});
    CHECK(sliverShadows && sliverShadows->size() == 1);
    CHECK(!sliverShadows->front().geometry.polygons.empty());
    const std::array<sim::EnvironmentShadow, 1> sliverInputs{
        {{sliverShadows->front().geometry, sliverShadows->front().bitmap}}};
    CHECK(sliverScene->hardware(palette, sliverInputs));
    auto sliverSurface = gfx::SoftwareSurface::create(64, 64);
    CHECK(sliverSurface && sliverSurface->setPalette(palette));
    CHECK(sliverScene->draw(*sliverSurface, sliverInputs));
    auto unsupported = std::make_unique<ShadowActor>();
    unsupported->image = movingActor->image;
    unsupported->image.pose = {}; // Hidden caster can fail silhouette preparation independently.
    auto* badCaster = unsupported.get();
    CHECK(translated.manager().addObject(std::move(unsupported)));
    for (const auto x : {0.f, 3.f}) {
        badCaster->image.shapeToWorld.p.x = offset.x + x;
        const auto scene = world::WorldSceneFrame::capture(translated, shiftedCamera, {}, false);
        CHECK(scene);
        const auto shadows = scene->shadows({.6f, .45f, -.7f});
        CHECK(shadows && shadows->size() == 1); // Preserve valid casters before/after a failure.
        CHECK(scene->hardware(palette));
    }
    Player::prefPlayerShadows = false;
    world::WorldShapeQuery disabled;
    CHECK(player->processQuery(disabled) && disabled.image && !disabled.image->castShadow);
    const auto noShadows = world::WorldSceneFrame::capture(world, camera, {}, false);
    CHECK(noShadows && noShadows->shadows({0, 0, -1})->empty());

    // Poses remain owned by the captured frame, and a crowd stays in budget.
    auto actor = std::make_unique<ShadowActor>();
    actor->image = body;
    auto* animated = actor.get();
    CHECK(world.manager().addObject(std::move(actor)));
    const auto old = world::WorldSceneFrame::capture(world, camera, {}, false);
    CHECK(old);
    const auto oldShadows = old->shadows({0, 0, -1});
    CHECK(oldShadows && oldShadows->size() == 1);
    auto cacheCamera = camera;
    cacheCamera.focal = {640, 640};
    const auto cachedScene = world::WorldSceneFrame::capture(world, cacheCamera, {}, false);
    CHECK(cachedScene);
    world::WorldShadowContext context;
    const auto firstCached = cachedScene->shadows({0, 0, -1}, .5f, &context, 1);
    CHECK(firstCached && firstCached->size() == 1);
    const auto fullFootprint = cachedScene->shadows({0, 0, -1});
    CHECK(fullFootprint && fullFootprint->size() == 1);
    const std::array<sim::EnvironmentShadow, 1> fullInput{
        {{fullFootprint->front().geometry, fullFootprint->front().bitmap}}};
    const std::array<sim::EnvironmentShadow, 1> croppedInput{
        {{firstCached->front().geometry, firstCached->front().bitmap}}};
    auto fullSurface = gfx::SoftwareSurface::create(64, 64);
    auto croppedSurface = gfx::SoftwareSurface::create(64, 64);
    CHECK(fullSurface && croppedSurface && fullSurface->setPalette(palette) &&
          croppedSurface->setPalette(palette));
    CHECK(cachedScene->draw(*fullSurface, fullInput) &&
          cachedScene->draw(*croppedSurface, croppedInput));
    // Cropping transparent support must preserve the original silhouette UVs.
    for (Int32 y = 0; y < 64; ++y)
        for (Int32 x = 0; x < 64; ++x)
            CHECK(std::abs(int(fullSurface->bitmap().row(y)[x]) -
                           int(croppedSurface->bitmap().row(y)[x])) <= 1);
    gfx::RenderWorkStats cacheStats;
    {
        gfx::ScopedRenderWorkStats record(cacheStats);
        const auto repeated = cachedScene->shadows({0, 0, -1}, .5f, &context, 1.016);
        CHECK(repeated && repeated->size() == 1);
        CHECK(repeated->front().bitmap.pixels == firstCached->front().bitmap.pixels);
    }
    CHECK(cacheStats.shadowGeometryHits == 1 && cacheStats.shadowSilhouetteHits == 1);
    {
        gfx::ScopedRenderWorkStats record(cacheStats);
        const auto changedLight = cachedScene->shadows({.1f, 0, -1}, .5f, &context, 1.032);
        CHECK(changedLight && changedLight->size() == 1);
    }
    CHECK(cacheStats.shadowGeometryHits == 1 && cacheStats.shadowSilhouetteHits == 1);
    CHECK(cachedScene->shadows({0, 0, -1}, .5f, &context, 1.04)->size() == 1);
    CHECK(animated->image.pose.overrideParentTransform(1, TMat3F(EulerF{0, 0, .7f}, {})));
    const auto next = world::WorldSceneFrame::capture(world, camera, {}, false);
    CHECK(next);
    const auto nextShadows = next->shadows({0, 0, -1});
    CHECK(nextShadows && nextShadows->size() == 1);
    const auto animatedScene = world::WorldSceneFrame::capture(world, cacheCamera, {}, false);
    CHECK(animatedScene);
    cacheStats = {};
    std::optional<std::vector<world::WorldShadow>> animatedCached;
    {
        gfx::ScopedRenderWorkStats record(cacheStats);
        animatedCached = animatedScene->shadows({0, 0, -1}, .5f, &context, 1.064);
    }
    CHECK(animatedCached && animatedCached->size() == 1);
    // The animated mask changed its occupied bounds, invalidating its cropped
    // receiver footprint as well as the silhouette.
    CHECK(cacheStats.shadowGeometryHits == 0 && cacheStats.shadowSilhouetteHits == 0);
    CHECK(animatedCached->front().bitmap.pixels != firstCached->front().bitmap.pixels);
    CHECK(nextShadows->front().bitmap.pixels != oldShadows->front().bitmap.pixels);
    CHECK(old->shadows({0, 0, -1})->front().bitmap.pixels == oldShadows->front().bitmap.pixels);
    auto distantCamera = camera;
    const auto distance = (animated->image.shapeToWorld.p - Point3F{0, 8, 15}).len();
    const auto focal = 20 * distance / (2 * animated->image.resources.shape->radius);
    distantCamera.focal = {focal, focal};
    const auto distant = world::WorldSceneFrame::capture(world, distantCamera, {}, false);
    CHECK(distant);
    world::WorldShadowContext distantContext;
    const auto held = distant->shadows({0, 0, -1}, .5f, &distantContext, 2);
    CHECK(held && held->size() == 1);
    CHECK(animated->image.pose.overrideParentTransform(1, TMat3F(EulerF{0, 0, -.5f}, {})));
    const auto distantNext = world::WorldSceneFrame::capture(world, distantCamera, {}, false);
    CHECK(distantNext);
    cacheStats = {};
    {
        gfx::ScopedRenderWorkStats record(cacheStats);
        const auto throttled = distantNext->shadows({0, 0, -1}, .5f, &distantContext, 2.016);
        CHECK(throttled && throttled->size() == 1);
        CHECK(throttled->front().bitmap.pixels == held->front().bitmap.pixels);
    }
    CHECK(cacheStats.shadowSilhouetteHits == 1);
    cacheStats = {};
    {
        gfx::ScopedRenderWorkStats record(cacheStats);
        CHECK(distantNext->shadows({0, 0, -1}, .5f, &distantContext, 2.15)->size() == 1);
    }
    CHECK(cacheStats.shadowSilhouetteHits == 0);
    distantCamera.focal = {focal * .1f, focal * .1f};
    const auto tiny = world::WorldSceneFrame::capture(world, distantCamera, {}, false);
    CHECK(tiny && tiny->shadows({0, 0, -1}, .5f, &distantContext, 2.2)->empty());
    auto ceiling = std::make_shared<world::WorldSurfaceMesh>(*floor);
    for (auto& triangle : ceiling->triangles)
        for (auto& vertex : triangle.vertices)
            vertex.position.z = 3;
    CHECK(world.manager().addObject(
        std::make_unique<world::SimWorldSurface>(world::WorldSurfaceInstance{ceiling, {}})));
    for (auto i : std::views::iota(0, 35)) {
        auto extra = std::make_unique<ShadowActor>();
        extra->image = body;
        extra->image.shapeToWorld.p.x += Flt32(i) * .01f;
        CHECK(world.manager().addObject(std::move(extra)));
    }
    const auto crowd = world::WorldSceneFrame::capture(world, camera, {}, false);
    CHECK(crowd);
    const auto crowdShadows = crowd->shadows({0, 0, -1});
    CHECK(crowdShadows && crowdShadows->size() == 32);
    for (const auto& shadow : *crowdShadows)
        for (const auto& polygon : shadow.geometry.polygons)
            for (const auto& vertex : polygon.vertices)
                CHECK(std::abs(vertex.position.z - .02f) < .0001f);
    auto limits = gfx::RenderLimits::dynamic();
    limits.shadows = 4;
    gfx::ScopedRenderLimits limited(limits);
    CHECK(crowd->shadows({0, 0, -1})->size() == 4);
}
}

int opent1_test_model_rendering() {
    return opent1::test::run(modelRendering);
}

int opent1_test_installed_rpg_models() {
    return opent1::test::run(installedModels);
}

OPENT1_REGISTER_TEST("game", "model_rendering", opent1_test_model_rendering);

int opent1_test_player_shadows() {
    return opent1::test::run(playerShadows);
}

OPENT1_REGISTER_TEST("game", "player_shadows", opent1_test_player_shadows);

int opent1_test_live_datablock_replacement() {
    return opent1::test::run(liveDatablockReplacement);
}

OPENT1_REGISTER_TEST("game", "live_datablock_replacement", opent1_test_live_datablock_replacement);
OPENT1_REGISTER_TEST("fixpack", "installed_rpg_models", opent1_test_installed_rpg_models);
