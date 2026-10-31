#include "ts_test_support.hpp"
#include <opent1/world/terrain_grid.hpp>
#include <opent1/world/world_collision_sim.hpp>
#include <opent1/world/world_scene.hpp>
#include <opent1/core/file_stream.hpp>
#include <opent1/core/vol_stream.hpp>
#include <opent1/core/timer.hpp>
#include <filesystem>
#include <limits>

namespace {
using namespace opent1;
using namespace opent1::world;
using opent1::test::ts::Bytes;

Bytes tagged(const Bytes& body, UInt32 tag, Int32 version) {
    Bytes result;
    result.put(tag);
    result.put(UInt32(body.data.size() + 4));
    result.integer(version);
    result.append(body);
    if (body.data.size() & 1) {
        result.put(UInt8{0xa5});
    }
    return result;
}

std::unique_ptr<TerrainGridBlock> loadBlock(const Bytes& bytes) {
    MemRWStream stream{std::span<const std::byte>(bytes.data)};
    return TerrainGridBlock::load(stream, UInt32(bytes.data.size()));
}

std::unique_ptr<TerrainGridFile> grid(const Bytes& bytes) {
    MemRWStream stream{std::span<const std::byte>(bytes.data)};
    return TerrainGridFile::load(stream, UInt32(bytes.data.size()));
}

void string(Bytes& bytes, std::string_view text) {
    bytes.integer(Int32(text.size()));
    bytes.text(text, text.size());
}

Bytes gridBody(Int32 version = 1) {
    Bytes b;
    string(b, "ground.pal");
    b.integer(1);
    b.integer(3);
    b.integer(1);
    b.point({0, 0, -8});
    b.point({24, 8, 16});
    b.integer(17);
    b.integer(-9);
    b.real(-8);
    b.real(16);
    b.integer(3);
    b.integer(1);
    if (version) {
        b.integer(0);
    }
    b.integer(0);
    b.integer(-1);
    b.integer(0);
    b.integer(1);
    b.integer(0);
    string(b, "first");
    return b;
}

Bytes oldBody(Int32 version) {
    Bytes b;
    b.text("old", 16);
    b.integer(2);
    b.integer(-1);
    b.real(-4);
    b.real(8);
    b.integer(2);
    b.integer(2);
    // 3x3 heights: first/last rows are always raw. The middle row uses signed
    // deltas in versions 1..3, so x=1 is 1 + (-2)*.5 = 0.
    for (auto h : {0.f, 1.f, 2.f}) {
        b.real(h);
    }
    if (version) {
        b.real(.5f);
        b.real(1);
        b.put(Int8{-2});
        b.real(3);
    } else {
        b.real(1);
        b.real(0);
        b.real(3);
    }
    for (auto h : {2.f, 3.f, 4.f}) {
        b.real(h);
    }
    for (auto i : std::views::iota(0, 4)) {
        b.put(UInt8(i == 0 ? 8 : i));
        b.put(UInt8(i));
    }
    if (version >= 2) {
        for (auto i : std::views::iota(0, 11)) {
            b.put(UInt16(i == 0 ? 1 : 0));
            if (i == 0) {
                b.put(UInt8{0x81});
            }
        }
    }
    if (version >= 3) {
        b.integer(0);
    }
    return b;
}

std::vector<std::byte> record(FileRStream& file) {
    UInt32 count{};
    CHECK(file.read(count) && count <= 65536);
    std::vector<std::byte> bytes(count);
    CHECK(file.readBytes(bytes));
    return bytes;
}

Bytes captured(unsigned select) {
    FileRStream file(OPENT1_TERRAIN_FIXTURE);
    UInt32 magic{}, version{}, count{};
    CHECK(file.read(magic) && file.read(version) && file.read(count));
    CHECK(magic == 0x4754314f && version == 1 && count == 8);
    Bytes selected;
    for (auto test : std::views::iota(0U, count)) {
        UInt32 number{};
        CHECK(file.read(number) && number == test);
        Bytes body;
        body.data = record(file);
        auto b = tagged(body, makeFourCC('G', 'B', 'L', 'K'), 5);
        auto block = loadBlock(b);
        CHECK(block);
        const auto heights = record(file), materials = record(file), lights = record(file);
        CHECK(block->version == 5 && block->width == 4 && block->name == "captured-grid");
        CHECK(block->detailCount == 3 &&
              block->lightScale == ((test & 1) ? Int32((test >> 1) & 1) : -1));
        Bytes h, m, l;
        for (auto value : block->heights) {
            h.real(value);
        }
        for (auto value : block->materials) {
            m.put(value.flags);
            m.put(value.index);
        }
        for (auto value : block->lights) {
            l.put(value);
        }
        CHECK(h.data == heights && m.data == materials && l.data == lights);
        CHECK(block->pins[0].size() == ((test & 2) ? 5U : 0U) &&
              block->pins[2].size() == ((test & 2) ? 1U : 0U));
        CHECK(block->hiresDelta.size() == ((test & 4) ? 3U : 0U) &&
              block->hiresMaps.size() == ((test & 4) ? 1U : 0U));
        if (test & 4) {
            CHECK(block->hiresMaps[0].color == 0xabcd &&
                  block->colorPool == std::vector<UInt16>{0x1234} &&
                  block->indexTable == std::vector<UInt8>{7} &&
                  block->treeTable == std::vector<UInt32>{16});
        }
        if (test == select) {
            selected = b;
        }
    }
    return selected;
}

void formatsAndBounds() {
    for (auto version : std::views::iota(0, 4)) {
        const auto body = oldBody(version);
        const auto tag = tagged(body, makeFourCC('G', 'B', 'L', 'K'), version);
        const auto b = loadBlock(tag);
        CHECK(b && b->heights == std::vector<Flt32>({0, 1, 2, 1, 0, 3, 2, 3, 4}));
        CHECK(loadBlock(body.pers("GridBlock", version)));
        auto collision = b->collision(2);
        CHECK(collision && !collision->surface({1, 1}) && collision->surface({1, 1}, true));
        for (auto size : std::views::iota(std::size_t{}, tag.data.size())) {
            auto bad = tag;
            bad.data.resize(size);
            CHECK(!loadBlock(bad));
        }
        for (auto size : std::views::iota(std::size_t{}, body.data.size())) {
            auto bad = body;
            bad.data.resize(size);
            CHECK(!loadBlock(tagged(bad, makeFourCC('G', 'B', 'L', 'K'), version)));
        }
    }
    for (auto version : {0, 1}) {
        const auto b = gridBody(version);
        auto g = grid(tagged(b, makeFourCC('G', 'F', 'I', 'L'), version));
        CHECK(g);
        CHECK(g->origin == Point2I(17, -9) && g->materialList == "ground.dml");
        CHECK(g->blockMap == std::vector<Int32>({0, -1, 0}) &&
              g->dependencies("path.with.dot/map.dtf") ==
                  std::vector<std::string>({"ground.dml", "path.with.dot/map#0.dtb"}));
        CHECK(grid(b.pers("GridFile", version)));
        const auto good = tagged(b, makeFourCC('G', 'F', 'I', 'L'), version);
        for (auto size : std::views::iota(std::size_t{}, good.data.size())) {
            auto bad = good;
            bad.data.resize(size);
            CHECK(!grid(bad));
        }
        for (auto size : std::views::iota(std::size_t{}, b.data.size())) {
            auto bad = b;
            bad.data.resize(size);
            CHECK(!grid(tagged(bad, makeFourCC('G', 'F', 'I', 'L'), version)));
        }
    }
    auto body = oldBody(0);
    auto good = tagged(body, makeFourCC('G', 'B', 'L', 'K'), 0);
    const auto corrupt = [&](std::size_t at, UInt32 value) {
        auto bad = good;
        Bytes word;
        word.put(value);
        std::ranges::copy(word.data, bad.data.begin() + std::ptrdiff_t(at));
        CHECK(!loadBlock(bad));
    };
    corrupt(8, 6);
    corrupt(28, 0);
    corrupt(32, 31);
    corrupt(44, 0x7fffffff);
    corrupt(40, 0x7fc00000U);
    corrupt(52, 0x7fc00000U);
    auto compressed = captured(7); // every byte prefix, including compressed maps/HRLM tails
    for (auto size : std::views::iota(std::size_t{}, compressed.data.size())) {
        auto bad = compressed;
        bad.data.resize(size);
        CHECK(!loadBlock(bad));
    }
    const auto payload = [](const Bytes& bytes) {
        MemRWStream header(std::span<const std::byte>(bytes.data).subspan(4));
        UInt32 count{};
        CHECK(header.read(count));
        count = (count & 0x7fffffffU) - 4;
        Bytes result;
        result.data.assign(bytes.data.begin() + 12, bytes.data.begin() + 12 + count);
        return result;
    };
    // Rewrap prefixes to exercise inner decompressor/count checks independently
    // of outer block extent checks.
    const auto compressedBody = payload(compressed);
    for (auto size : std::views::iota(std::size_t{}, compressedBody.data.size())) {
        auto bad = compressedBody;
        bad.data.resize(size);
        CHECK(!loadBlock(tagged(bad, makeFourCC('G', 'B', 'L', 'K'), 5)));
    }
    auto version4 = captured(0);
    auto v4body = payload(version4);
    // V4 has the same compressed height/material/light maps and HRLM v3 tail,
    // without V5's four-byte alternative-hires length immediately before it.
    v4body.data.erase(v4body.data.end() - 24, v4body.data.end() - 20);
    const auto v4 = loadBlock(tagged(v4body, makeFourCC('G', 'B', 'L', 'K'), 4));
    CHECK(v4 && v4->heights == loadBlock(version4)->heights);
    // Keep the outer block valid so malformed declarations reach inner checks.
    const auto badCompressed = [&](std::size_t at, UInt32 value) {
        auto bad = compressed;
        Bytes word;
        word.put(value);
        std::ranges::copy(word.data, bad.data.begin() + std::ptrdiff_t(at));
        CHECK(!loadBlock(bad));
    };
    badCompressed(52, 0xffffffffU);
    badCompressed(52, 96); // expected height bytes = 100
    auto field = gridBody();
    const auto badGrid = [&](std::size_t at, Int32 value) {
        auto bad = field;
        Bytes word;
        word.integer(value);
        std::ranges::copy(word.data, bad.data.begin() + std::ptrdiff_t(at));
        CHECK(!grid(tagged(bad, makeFourCC('G', 'F', 'I', 'L'), 1)));
    };
    badGrid(18, 0);
    badGrid(22, 32);
    badGrid(66, -1);
    badGrid(74, 3);
    badGrid(78, 71);
    badGrid(90, 0x7fffffff);
    auto g = gridBody();
    g.data[0] = std::byte{255};
    CHECK(!grid(tagged(g, makeFourCC('G', 'F', 'I', 'L'), 1)));
    auto trailing = good;
    trailing.put(UInt32{0xabcdef});
    CHECK(loadBlock(trailing));
    CHECK(!loadBlock(body.pers("SomethingElse", 0)));
}

void resourcesAndCollision() {
    std::vector<TerrainCollisionBlock> retained;
    {
        ResourceManager manager;
        CHECK(registerTerrainLoaders(manager));
        CHECK(registerTsLoaders(manager));
        auto body = gridBody();
        CHECK(manager.add("map.dtf", grid(tagged(body, makeFourCC('G', 'F', 'I', 'L'), 1))));
        CHECK(!TerrainGridResources::load(manager, "map.dtf"));
        auto list = std::make_unique<TsMaterialList>();
        list->version = 4;
        list->details = 1;
        list->materialsPerDetail = 3;
        list->materials.resize(3);
        for (auto& material : list->materials) {
            material.flags = 1;
            material.index = 7;
        }
        CHECK(manager.add("ground.dml", std::move(list)));
        CHECK(!TerrainGridResources::load(manager, "map.dtf"));
        CHECK(manager.add("map#0.dtb", loadBlock(captured(0))));
        const auto resource = TerrainGridResources::load(manager, "MAP.DTF");
        CHECK(resource);
        auto blocks = resource->collisionBlocks();
        CHECK(blocks && blocks->size() == 2);
        CHECK((*blocks)[0].origin == Point3F(0, 0, 0) && (*blocks)[1].origin == Point3F(16, 0, 0) &&
              (*blocks)[0].geometry == (*blocks)[1].geometry);
        retained = std::move(*blocks);
    }
    sim::SimWorld world;
    for (const auto& block : retained) {
        TMat3F at;
        at.postTranslate(block.origin);
        CHECK(world.manager().addObject(
            std::make_unique<SimWorldCollider>(SimTerrainCollisionImage(block.geometry), at)));
    }
    auto* root = dynamic_cast<sim::SimContainer*>(world.manager().findObject(sim::RootContainerId));
    CHECK(root);
    sim::SimContainerQuery query;
    query.box = {{16.2f, .2f, 100}, {16.2f, .2f, -100}};
    query.mask = ~UInt32{};
    sim::SimCollisionInfo hit;
    CHECK(root->findLOS(query, hit));
    Point3F worldHit;
    m_mul(hit.surfaces.surfaces[0].position, hit.surfaces.tWorld, worldHit);
    CHECK(hit.object.resolve(world.manager()) && std::abs(worldHit.x - 16.2f) < .001f);
}

void preloading() {
    const auto directory =
        std::filesystem::temp_directory_path() /
        std::format("opent1-grid-{}", Timer::Clock::now().time_since_epoch().count());

    struct Cleanup {
        std::filesystem::path path;

        ~Cleanup() {
            std::error_code e;
            std::filesystem::remove_all(path, e);
        }
    } cleanup{directory};

    CHECK(std::filesystem::create_directory(directory));
    const auto write = [&](std::string_view name, const Bytes& b) {
        FileWStream file(directory / name);
        CHECK(file.writeBytes(b.data) && file.flush());
    };
    auto body = gridBody();
    write("map.dtf", tagged(body, makeFourCC('G', 'F', 'I', 'L'), 1));
    write("map#0.dtb", captured(0));
    // A DML containing only palette materials has no texture dependencies.
    auto mat = opent1::test::ts::material();
    mat.data[38] = std::byte{1};
    write("ground.dml", mat);
    auto manager = std::make_shared<ResourceManager>();
    CHECK(manager->setSearchPaths(std::array{directory}));
    CHECK(registerTerrainLoaders(*manager) && registerTsLoaders(*manager));
    sim::SimWorld world(nullptr, manager);
    auto preloader = std::make_unique<sim::SimPreLoadManager>();
    auto* p = preloader.get();
    CHECK(registerWorldPreloaders(*p));
    CHECK(world.manager().addObject(std::move(preloader)));
    CHECK(p->preLoad("map.dtf") && p->isLoaded("map#0.dtb") && p->isLoaded("ground.dml"));
}

void assets(const std::filesystem::path& directory) {
    FileRStream fixture(OPENT1_TERRAIN_ASSET_FIXTURE);
    const auto word = [&] {
        UInt32 value{};
        CHECK(fixture.read(value));
        return value;
    };
    const auto text = [&] {
        auto bytes = record(fixture);
        return std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    };
    CHECK(word() == 0x4154314f && word() == 1);
    const auto count = word();
    CHECK(count > 0 && count <= 4096);

    struct Oracle {
        std::string archive, name;
        std::array<UInt32, 18> values;
        bool visited{};
    };

    std::vector<Oracle> oracles(count);
    for (auto& oracle : oracles) {
        oracle.archive = text();
        oracle.name = text();
        std::ranges::generate(oracle.values, word);
    }
    const auto hash = [](std::span<const std::byte> bytes) {
        UInt32 result = 2166136261U;
        for (auto b : bytes) {
            result = (result ^ std::to_integer<UInt8>(b)) * 16777619U;
        }
        return result;
    };
    UInt32 archives{}, grids{}, blocks{}, instances{}, selectedMaps{}, staleTrees{},
        nativeVisible{};
    for (const auto& entry : std::filesystem::recursive_directory_iterator(directory)) {
        if (entry.path().extension() != ".ted") {
            continue;
        }
        VolumeRStream volume;
        CHECK(volume.openVolume(entry.path()));
        auto owner = std::make_shared<ResourceManager>();
        auto& manager = *owner;
        CHECK(registerTerrainLoaders(manager) && registerTsLoaders(manager) &&
              gfx::registerBitmapFontLoaders(manager));
        CHECK(manager.setSearchPaths(std::array{entry.path().parent_path(), directory}));
        CHECK(manager.addVolume(entry.path().filename().string()));
        ++archives;
        for (const auto& item : volume.getVolumeItems()) {
            if (!item.name) {
                continue;
            }
            auto name = *item.name;
            std::ranges::transform(name, name.begin(), [](unsigned char c) {
                return char(std::tolower(c));
            });
            if (name.ends_with(".dtf")) {
                const auto g = manager.load<TerrainGridFile>(*item.name);
                if (!g) {
                    std::fprintf(stderr,
                                 "Rejected grid: %s / %s\n",
                                 entry.path().string().c_str(),
                                 item.name->c_str());
                }
                CHECK(g);
                ++grids;
                const auto stem = std::filesystem::path(g->materialList).stem().string();
                for (const auto suffix : {"DML.vol", "Terrain.vol", "World.vol"}) {
                    const auto archive = stem + suffix;
                    if (manager.contains(archive)) {
                        CHECK(manager.addVolume(archive));
                    }
                }
                auto resources = TerrainGridResources::load(manager, *item.name);
                if (!resources) {
                    std::fprintf(stderr,
                                 "Rejected terrain dependencies: %s / %s\n",
                                 item.name->c_str(),
                                 g->materialList.c_str());
                }
                CHECK(resources);
                const auto collision = resources->collisionBlocks();
                CHECK(collision);
                instances += UInt32(collision->size());
                const auto nativeCollision = TerrainCollisionSnapshot::prepare(*resources);
                CHECK(nativeCollision);
                const auto square = Flt32(1U << g->scale);
                const auto patch = WorldSurfaceMesh::terrain(
                    *resources, true, Box2F{0, 0, 16 * square, 16 * square});
                CHECK(patch && patch->triangles.size() <= 512);
                const auto lod =
                    WorldSurfaceMesh::terrainLod(*resources,
                                                 TerrainLodView{{0, 0, 4096}, 320, 2},
                                                 true,
                                                 Box2F{0, 0, 16 * square, 16 * square});
                CHECK(lod && lod->triangles.size() <= patch->triangles.size());
                TerrainLodView compositeView{{0, 0, 4096}, 320, 2};
                compositeView.compositeTextures = true;
                const auto composite = WorldSurfaceMesh::terrainLod(
                    *resources, compositeView, true, Box2F{0, 0, 16 * square, 16 * square});
                if (!composite) {
                    std::fprintf(stderr,
                                 "Rejected composite terrain: %s / %s\n",
                                 entry.path().string().c_str(),
                                 item.name->c_str());
                }
                CHECK(composite && composite->triangles.size() <= lod->triangles.size());
                TerrainLodState state(*resources);
                TerrainTextureCache cache(*resources);
                auto stream = TerrainResourceStream::open(owner, *item.name);
                CHECK(stream && stream->residentBlocks() == 0);
                compositeView.window = TerrainLodWindow{4, 1};
                const auto cells = Flt32(1U << (g->detailCount - 1));
                UInt32 viewIndex{};
                for (const auto center :
                     {Point2F{8.25f, 8.25f},
                      Point2F{8.75f, 8.75f},
                      Point2F{12.25f, 12.25f},
                      Point2F{g->size.x * cells - 8.25f, g->size.y * cells - 8.25f},
                      Point2F{8.25f, 8.25f},
                      Point2F{8.75f, 8.75f}}) {
                    constexpr std::array policies{TerrainLightMapDensity::Expanded,
                                                  TerrainLightMapDensity::Native,
                                                  TerrainLightMapDensity::NativeOpenGL};
                    compositeView.lightMapDensity = policies[viewIndex++ % policies.size()];
                    compositeView.nativeTextures =
                        (viewIndex % 3) ? std::optional{TerrainTextureView{320}} : std::nullopt;
                    compositeView.nativeGeometry =
                        (viewIndex % 2) ? std::optional{TerrainGeometryView{320}} : std::nullopt;
                    compositeView.nativeVisibility = compositeView.nativeGeometry
                                                         ? std::optional{TerrainVisibilityView{}}
                                                         : std::nullopt;
                    compositeView.nativeTraversal = bool(compositeView.nativeGeometry);
                    compositeView.textureTiming = TerrainTextureTiming::NativeSoftware;
                    if (compositeView.nativeVisibility) {
                        compositeView.nativeVisibility->visibleDistance =
                            std::max(128 * square, 8192.f);
                        compositeView.nativeVisibility->cameraToGrid.set(
                            EulerF{-1.4f, 0, Flt32(viewIndex) * .4f});
                    }
                    compositeView.camera = {center.x * square, center.y * square, 4096};
                    TsLighting lights;
                    TsLight point;
                    point.type = TsLight::Type::Point;
                    point.position = {center.x * square, center.y * square, g->minimumHeight};
                    point.range = 16 * square + std::abs(g->maximumHeight - g->minimumHeight);
                    point.intensity = {.9f, .3f, .1f};
                    lights.lights = {point};
                    const auto* scene =
                        compositeView.lightMapDensity != TerrainLightMapDensity::Expanded ? &lights
                                                                                          : nullptr;
                    auto window = WorldSurfaceMesh::terrainLod(
                        *resources, compositeView, true, {}, &cache, &state, scene);
                    CHECK(window && window->triangles.size() <= 242);
                    nativeVisible += compositeView.nativeVisibility && !window->triangles.empty();
                    const auto streamed = stream->prepare(compositeView, true, {}, scene);
                    CHECK(streamed && streamed->triangles.size() == window->triangles.size());
                    auto streamedCollision = stream->prepareCollision({center.x * square,
                                                                       center.y * square,
                                                                       center.x * square,
                                                                       center.y * square});
                    CHECK(streamedCollision);
                    CollisionSurfaceList eagerHits, streamHits;
                    const Point3F top{center.x * square, center.y * square, 100000},
                        bottom{center.x * square, center.y * square, -100000};
                    const auto eagerHit = nativeCollision->line(top, bottom, eagerHits),
                               streamHit = streamedCollision->line(top, bottom, streamHits);
                    CHECK(eagerHit == streamHit);
                    if (eagerHit) {
                        CHECK(eagerHits.surfaces[0].position == streamHits.surfaces[0].position &&
                              eagerHits.surfaces[0].normal == streamHits.surfaces[0].normal &&
                              eagerHits.surfaces[0].part == streamHits.surfaces[0].part);
                    }
                    for (std::size_t i = 0; i < window->triangles.size(); ++i) {
                        const auto& a = window->triangles[i];
                        const auto& b = streamed->triangles[i];
                        CHECK(a.material == b.material && a.lit == b.lit &&
                              a.repeatTexture == b.repeatTexture);
                        CHECK(bool(a.lightMap) == bool(b.lightMap));
                        if (a.lightMap) {
                            CHECK(a.lightMap->size == b.lightMap->size &&
                                  a.lightMap->samples == b.lightMap->samples);
                        }
                        for (std::size_t j = 0; j < 3; ++j) {
                            CHECK(a.vertices[j].position == b.vertices[j].position &&
                                  a.vertices[j].texture == b.vertices[j].texture &&
                                  a.vertices[j].intensity == b.vertices[j].intensity &&
                                  a.vertices[j].lightMap == b.vertices[j].lightMap);
                        }
                    }
                    CHECK(streamed->materials.textures.size() == window->materials.textures.size());
                    for (std::size_t i = 0; i < window->materials.textures.size(); ++i) {
                        const auto& a = window->materials.textures[i];
                        const auto& b = streamed->materials.textures[i];
                        CHECK(bool(a) == bool(b));
                        if (a) {
                            CHECK(a->width == b->width && a->height == b->height &&
                                  a->pixels == b->pixels);
                        }
                    }
                }
            } else if (name.ends_with(".dtb")) {
                const auto b = manager.load<TerrainGridBlock>(*item.name);
                if (!b) {
                    std::fprintf(stderr,
                                 "Rejected block: %s / %s\n",
                                 entry.path().string().c_str(),
                                 item.name->c_str());
                }
                CHECK(b && b->collision(8));
                ++blocks;
                CHECK(b->lightSamples(false) && b->lightSamples(true));
                const auto selected = b->hiresMapIndices(0, 0, b->width);
                CHECK(selected);
                selectedMaps += UInt32(selected->size());
                staleTrees += UInt32(b->hiresMaps.empty() && !b->treeTable.empty());
                for (const auto index : *selected) {
                    const auto& map = b->hiresMaps[index];
                    CHECK(b->hiresMapIndices(UInt32(map.x), UInt32(map.y), 1) ==
                          std::vector<UInt32>{index});
                }
                for (UInt32 i = 0; i < b->hiresMaps.size(); ++i) {
                    for (UInt32 resolution = 0; resolution <= 5; ++resolution) {
                        if (b->hiresMaps[i].resolutions & (1U << resolution)) {
                            CHECK(b->hiresSamples(i, resolution));
                        }
                    }
                }
                const auto archive =
                    std::filesystem::relative(entry.path(), directory).generic_string();
                auto oracle = std::ranges::find_if(oracles, [&](const auto& o) {
                    return o.archive == archive && o.name == *item.name;
                });
                CHECK(oracle != oracles.end());
                CHECK(volume.open(*item.name));
                Bytes range, heights, materials, lights, hrlm, colors, indices, tree;
                range.real(b->minimumHeight);
                range.real(b->maximumHeight);
                for (auto v : b->heights) {
                    heights.real(v);
                }
                for (auto v : b->materials) {
                    materials.put(v.flags);
                    materials.put(v.index);
                }
                for (auto v : b->lights) {
                    lights.put(v);
                }
                for (auto v : b->hiresMaps) {
                    hrlm.put(v.x);
                    hrlm.put(v.y);
                    hrlm.put(v.flags);
                    hrlm.put(v.compression);
                    hrlm.put(v.resolutions);
                    hrlm.put(v.colors);
                    hrlm.put(v.color);
                }
                for (auto v : b->colorPool) {
                    colors.put(v);
                }
                for (auto v : b->indexTable) {
                    indices.put(v);
                }
                for (auto v : b->treeTable) {
                    tree.put(v);
                }
                const std::array<UInt32, 18> actual{UInt32(volume.data().size()),
                                                    hash(volume.data()),
                                                    UInt32(b->version),
                                                    UInt32(b->detailCount),
                                                    UInt32(b->lightScale),
                                                    b->width,
                                                    hash(std::as_bytes(std::span(b->name))),
                                                    hash(range.data),
                                                    hash(heights.data),
                                                    hash(materials.data),
                                                    hash(lights.data),
                                                    UInt32(b->hiresDelta.size()),
                                                    hash(b->hiresDelta),
                                                    UInt32(b->hiresMaps.size()),
                                                    hash(hrlm.data),
                                                    hash(colors.data),
                                                    hash(indices.data),
                                                    hash(tree.data)};
                CHECK(actual == oracle->values);
                oracle->visited = true;
            }
        }
    }
    CHECK(archives > 0 && grids > 0 && nativeVisible > grids && blocks == count &&
          std::ranges::all_of(oracles, &Oracle::visited));
    std::printf("Terrain assets: %u archives, %u grids, %u blocks, %u collision instances\n",
                archives,
                grids,
                blocks,
                instances);
    std::printf("Terrain HRLM trees: %u selected maps, %u empty-map obsolete trees\n",
                selectedMaps,
                staleTrees);
    std::printf("Terrain visibility: %u nonempty native camera windows\n", nativeVisible);
}
}

int opent1_test_terrain_grid(int argc, char** argv) {
    return opent1::test::run([&] {
        if (argc > 1) {
            assets(argv[1]);
        } else {
            formatsAndBounds();
            resourcesAndCollision();
            preloading();
        }
    });
}

namespace {
int opent1_test_terrain_grid_default() {
    char program[] = "opent1_tests";
    char* args[] = {program, nullptr};
    return opent1_test_terrain_grid(1, args);
}
}

#include "test_registry.hpp"
OPENT1_REGISTER_TEST("world", "terrain_grid", opent1_test_terrain_grid_default);
