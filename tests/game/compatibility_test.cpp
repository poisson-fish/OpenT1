#include "test_support.hpp"
#include <ranges>
#include "test_registry.hpp"
#include "ts_test_support.hpp"
#include <opent1/game/game_app.hpp>
#include <opent1/game/interior_shape.hpp>
#include <opent1/game/chat_menu.hpp>
#include <opent1/game/chat_inventory_huds.hpp>
#include <opent1/game/fear_player_psc.hpp>
#include <opent1/game/player.hpp>
#include <opent1/game/observer_camera.hpp>
#include <opent1/game/marker.hpp>
#include <opent1/sim/sim_input_action.hpp>
#include <opent1/core/file_stream.hpp>
#include <opent1/core/vol_stream.hpp>
#include <opent1/core/filename.hpp>
#include <opent1/sim/sim_interior_grouping.hpp>
#include <opent1/sim/sim_editor_events.hpp>
#include <opent1/world/interior_shape.hpp>
#include <opent1/world/interior_links.hpp>
#include <opent1/world/world_surfaces.hpp>
#include <fstream>
#include <cstdlib>
#include <cmath>
#include <numbers>

namespace {
using namespace opent1;

void write(const std::filesystem::path& path, std::string_view text) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream file(path, std::ios::binary);
    file << text;
    CHECK(bool(file));
}

void volume(const std::filesystem::path& path, std::string_view name, std::string_view data) {
    std::filesystem::create_directories(path.parent_path());
    VolumeRWStream archive;
    CHECK(archive.put(name, std::as_bytes(std::span(data))) && archive.save(path));
}

std::string read(ResourceManager& resources, std::string_view name) {
    std::vector<std::byte> bytes;
    CHECK(resources.read(name, bytes));
    return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}

void compatibility() {
    const auto cwd = std::filesystem::current_path();
    const auto root = std::filesystem::temp_directory_path() /
                      std::format("opent1-compat-{}",
                                  std::chrono::steady_clock::now().time_since_epoch().count());

    struct Cleanup {
        std::filesystem::path root;

        ~Cleanup() {
            std::error_code ec;
            std::filesystem::remove_all(root, ec);
        }
    } cleanup{root};

    write(root / "config/override.dat", "config");
    write(root / "override.dat", "inactive root");
    write(root / "Inactive/secret.dat", "inactive mod");
    write(root / "base/fallback.dat", "base");
    write(root / "RPG/MISSIONS/modmission.mis", R"(
        instant SimGroup "MissionGroup" {
            instant SimGroup "Volumes" {
                instant SimVolume "Assets" { fileName = $MissionArchive; };
            };
            instant NeedsArchive "Consumer";
        };
    )");
    write(root / "RPG/override.dat", "rpg");
    write(root / "Extra/override.dat", "extra");
    write(root / "recordings/demo.dat", "recording");
    write(root / "temp/reload.cs", "$Reloaded = true;");
    volume(root / "base/scripts.vol", "server.cs", "$WrongServer = true;");
    volume(root / "RPG/scripts.vol", "server.cs", R"(
        $ModServer = true;
        function createServer(%mission, %dedicated) {
            $CustomBefore = true;
            deleteObject(MissionGroup);
            $MissionArchive = "mission.vol";
            exec(%mission @ ".mis");
            $CustomAfter = true;
            return "True";
        }
    )");
    volume(root / "RPG/mission.vol", "required.dat", "mission data");
    volume(root / "base/other.vol", "unused.dat", "unused");
    write(root / "console.cs", R"(
        $dedicated = true;
        newServer(); focusServer();
        $ConsoleWorld::DefaultSearchPath = "config;RPG;RPG\\missions;base;recordings;temp";
        newObject(ScriptsVolume, SimVolume, "scripts.vol");
        exec("server.cs");
    )");
    game::FearGameApp::Config config;
    config.assetRoot = root;
    game::FearGameApp app(config);

    struct NeedsArchive : sim::SimObject {
        ResourceManager* resources{};

        explicit NeedsArchive(ResourceManager& r) : resources(&r) {}

        bool onAdd() override {
            return sim::SimObject::onAdd() && resources->contains("required.dat");
        }
    };

    app.game().registerFactory("NeedsArchive", [&] {
        return std::make_unique<NeedsArchive>(app.resources());
    });
    CHECK(app.mountAssetRoot(root));
    CHECK(!app.resources().contains("server.cs") && !app.resources().contains("unused.dat"));
    CHECK(!app.resources().contains("Inactive/secret.dat"));
    CHECK(app.init());
    CHECK(std::filesystem::current_path() == cwd);
    CHECK(script::toBool(app.console().getVariable("ModServer")) &&
          app.console().getVariable("WrongServer").empty());
    CHECK(read(app.resources(), "override.dat") == "config");
    CHECK(read(app.resources(), "fallback.dat") == "base");
    CHECK(read(app.resources(), "demo.dat") == "recording");
    CHECK(app.console().executeFile("reload.cs").ok());
    CHECK(app.loadMission("modmission", true));
    CHECK(script::toBool(app.console().getVariable("CustomBefore")) &&
          script::toBool(app.console().getVariable("CustomAfter")));
    CHECK(read(app.resources(), "required.dat") == "mission data");
    CHECK(app.console().evaluate("deleteObject(MissionGroup);").ok());
    CHECK(!app.resources().contains("required.dat"));
    CHECK(app.console().evaluate("export(\"Custom*\",\"config\\\\saved.cs\");").value == "True");
    CHECK(std::filesystem::is_regular_file(root / "config/saved.cs"));
    CHECK(!app.resources().contains("saved.cs"));
    const auto path = app.console().getVariable("ConsoleWorld::DefaultSearchPath");
    app.console().setVariable("ConsoleWorld::DefaultSearchPath", path);
    CHECK(app.resources().contains("saved.cs"));
    CHECK(app.console().execute("unfocus").value == "True");
    const std::array<std::string, 2> physical{"console.cs", "RPG\\scripts.vol"};
    CHECK(app.console().execute("isFile", physical).value == "True");
    const std::array<std::string, 1> virtualOnly{"server.cs"};
    CHECK(app.console().execute("isFile", virtualOnly).value == "False");
    app.console().setVariable("ConsoleWorld::DefaultSearchPath",
                              "Extra;RPG;base;RPG\\missions;temp");
    CHECK(read(app.resources(), "override.dat") == "extra");
    // Already mounted scripts stay owned until their SimVolume is destroyed.
    CHECK(read(app.resources(), "server.cs").find("CustomBefore") != std::string::npos);
    CHECK(app.console().execute("focusServer").value == "True");
    CHECK(app.console().evaluate("deleteObject(ScriptsVolume);").ok());
    CHECK(!app.resources().contains("server.cs"));
    app.console().setVariable("ConsoleWorld::DefaultSearchPath", "base");
    CHECK(app.console().evaluate("newObject(ScriptsVolume,SimVolume,\"scripts.vol\");").ok());
    CHECK(read(app.resources(), "server.cs").find("WrongServer") != std::string::npos);
    CHECK(app.resources().add("tail.dat", std::make_unique<RawData>()));
    CHECK(app.resources().add("middle.cs", std::make_unique<RawData>()));
    CHECK(app.resources().add("head.dat", std::make_unique<RawData>()));
    CHECK(app.console()
              .evaluate(R"(
        $First = File::findFirst("head.dat");
        $NextScript = File::findNext("*.cs");
        $NextData = File::findNext("*.dat");
    )")
              .ok());
    CHECK(app.console().getVariable("First") == "head.dat" &&
          app.console().getVariable("NextScript") == "middle.cs" &&
          app.console().getVariable("NextData") == "tail.dat");
    // StringTableEntry values must compare with the console's first spelling.
    CHECK(app.console()
              .evaluate(R"(
        $Spelling = weapon;
        ItemData CompatWeapon { className = "Weapon"; };
        $ClassMatches = CompatWeapon.className == Weapon;
    )")
              .ok());
    CHECK(app.console().getVariable("ClassMatches") == "1");
    const std::array<std::string, 1> missingInterior{"missing.dis"};
    CHECK(!app.game().createObject("SimInterior", "MissingInterior", nullptr, missingInterior));
    write(root / "base/assembly.emf",
          "[EMF]\r\nbase.dis\r\nGROUP "
          "SimGroup\r\nParts\r\nOBJECT\r\nParts\r\npiece\r\nPart\r\narg\r\n1 2 3\r\n0 0 "
          "0\r\n[END]\r\n");
    app.console().setVariable("ConsoleWorld::DefaultSearchPath", "base");

    struct Part : sim::SimObject {
        Point3F position;

        bool processArguments(std::span<const std::string> args) override {
            return args.size() == 1;
        }

        bool processEvent(const sim::SimEvent& event) override {
            if (const auto* transform = dynamic_cast<const sim::SimObjectTransformEvent*>(&event)) {
                position = transform->transform.p;
                return true;
            }
            return false;
        }
    };

    sim::SimGame emfGame(app.sharedResources());
    CHECK(emfGame.setWorld(
        sim::WorldType::Server,
        std::make_unique<sim::SimWorld>(&emfGame.console(), app.sharedResources())));
    CHECK(emfGame.focus(sim::WorldType::Server));
    CHECK(emfGame.registerFactory("SimInterior", [] {
        return std::make_unique<Part>();
    }));
    CHECK(emfGame.registerFactory("Part", [] {
        return std::make_unique<Part>();
    }));
    const std::array<std::string, 1> emf{"assembly.emf"};
    auto* assembly = emfGame.createObject("SimInteriorGrouping", "Assembly", nullptr, emf);
    CHECK(assembly);
    auto* group = dynamic_cast<sim::SimGroup*>(assembly);
    auto* part = group ? dynamic_cast<Part*>(group->findObject("Parts/piece")) : nullptr;
    CHECK(part && part->position == Point3F(1, 2, 3));
    UInt32 played{};
    app.legacySound.play = [&](UInt32 id,
                               std::string_view file,
                               const audio::Profile& profile,
                               float priority) -> UInt64 {
        CHECK(id == 42 && file == "UI.wav" && profile.baseVolume == -.25f &&
              (profile.flags & audio::Loop) && priority == .3f);
        ++played;
        return 99;
    };
    app.legacySound.stop = [](UInt64 handle) {
        return handle == 99;
    };
    CHECK(app.console()
              .evaluate(R"(
        newSfx(); sfxSetFormat(22050,16,true);
        sfxAdd2DProfile(100,-0.25,SFX_IS_LOOPING);
        sfxAddPair(42,100,"UI.wav",0.3);
        $SoundHandle = sfxPlay(42);
        $SoundStopped = sfxStop($SoundHandle);
    )")
              .ok());
    CHECK(played == 1 && app.console().getVariable("SoundHandle") == "99" &&
          app.console().getVariable("SoundStopped") == "True");
    app.shutdown();
    CHECK(!app.resources().contains("server.cs"));
}

void largeInteriorMeshes() {
    for (const auto count : std::array<UInt16, 4>{38, 47, 51, 255}) {
        ResourceManager manager;
        auto shape = std::make_unique<world::InteriorShape>();
        shape->lods.resize(1);
        auto geometry = std::make_unique<world::InteriorGeometry>();
        world::InteriorSurface surface;
        surface.vertexCount = UInt8(count);
        geometry->surfaces.push_back(surface);
        for (auto i : std::views::iota(UInt16{0}) | std::views::take_while([&](UInt16 i) {
                          return i < count;
                      })) {
            const auto angle = Flt32(i) * 2.f * std::numbers::pi_v<Flt32> / Flt32(count);
            geometry->points.push_back({std::cos(angle), 0, std::sin(angle)});
            geometry->texturePoints.push_back({Flt32(i) / count, 0});
            geometry->vertices.push_back({i, i});
        }
        CHECK(manager.add("ring.dis", std::move(shape)));
        CHECK(manager.add("ring.dig", std::move(geometry)));
        world::InteriorShapeResources resources;
        resources.shape = manager.load<world::InteriorShape>("ring.dis");
        resources.geometry.push_back(manager.load<world::InteriorGeometry>("ring.dig"));
        world::TsMaterialResources::Data materialData;
        materialData.list.materials.resize(1);
        materialData.list.materials[0].flags = 1;
        materialData.textures.resize(1);
        resources.materials = world::TsMaterialResources(std::move(materialData));
        const auto mesh = world::WorldSurfaceMesh::interior(resources, 0, 0, false);
        CHECK(mesh && mesh->triangleCount() == std::size_t(count - 2) && mesh->interiorMesh &&
              mesh->interiorMesh->surfaceIds.size() == mesh->triangleCount());
        world::TsStaticView view;
        view.shapeToCamera.identity();
        view.shapeToCamera.p = {0, 5, 0};
        view.twoSided = true;
        const auto frame = mesh->prepare(view);
        CHECK(frame && frame->polygonCount() == std::size_t(count - 2));
    }
}

void emptyMissionLighting() {
    test::ts::Bytes body;
    body.integer(7); // build id
    body.integer(4); // scale
    body.integer(0); // slots
    for ([[maybe_unused]] auto i : std::views::iota(0, 5)) {
        body.integer(0);
    } // empty arrays
    body.integer(0);    // names
    body.put(UInt8{1}); // compressed lightmaps
    body.integer(1);
    body.integer(2); // Huffman counts
    body.integer(-1);
    body.integer(-2); // root
    body.put(UInt32{0});
    body.put(UInt32{65535}); // colors
    body.integer(0);         // no mission pairs
    body.put(UInt8{0});      // legacy LZH empty-stream terminator
    const auto encoded = body.pers("ITRMissionLighting", 7).data;
    MemRWStream stream{std::span<const std::byte>(encoded)};
    const auto lighting = world::InteriorLighting::load(stream, UInt32(encoded.size()));
    CHECK(lighting && lighting->mission && lighting->missionEntries.empty());
    // Extra bytes and nonzero terminators must still be rejected.
    auto corrupt = encoded;
    corrupt.back() = std::byte{1};
    MemRWStream invalid{std::span<const std::byte>(corrupt)};
    CHECK(!world::InteriorLighting::load(invalid, UInt32(corrupt.size())));
}

void legacyInteriorFaceMasks() {
    ResourceManager manager;
    auto geometry = std::make_unique<world::InteriorGeometry>();
    geometry->bounds = {{-1, -1, -1}, {1, 1, 1}};
    CHECK(manager.add("links.dig", std::move(geometry)));
    std::array<world::InteriorLinkInstance, 2> instances;
    for (auto& instance : instances) {
        instance.geometry = manager.load<world::InteriorGeometry>("links.dig");
        instance.placement.identity();
    }
    instances[1].placement.p.x = 2;
    instances[1].placement.flags |= RMat3F::Matrix_HasTranslation;
    const auto canonical = world::InteriorLinkGraph::prepare(instances);
    CHECK(canonical && canonical->links(0).size() == 1 && canonical->links(1).size() == 1);
    for (auto& instance : instances) {
        instance.linkableFaces = 255; // Shipped RPG DIS files also set the two unused low bits.
    }
    const auto legacy = world::InteriorLinkGraph::prepare(instances);
    CHECK(legacy && std::ranges::equal(legacy->links(0), canonical->links(0)) &&
          std::ranges::equal(legacy->links(1), canonical->links(1)));
    instances[0].linkableFaces = 3; // Unused bits alone must not enable any links.
    const auto disabled = world::InteriorLinkGraph::prepare(instances);
    CHECK(disabled && disabled->links(0).empty() && disabled->links(1).empty());
    instances[0].linkableFaces = 256;
    CHECK(!world::InteriorLinkGraph::prepare(instances));
}
}

int opent1_test_asset_compatibility() {
    return opent1::test::run([] {
        compatibility();
        largeInteriorMeshes();
        emptyMissionLighting();
        legacyInteriorFaceMasks();
    });
}

OPENT1_REGISTER_TEST("game", "asset_compatibility", opent1_test_asset_compatibility);

int opent1_test_installed_rpg_compatibility() {
    return opent1::test::run([] {
        const auto* assets = std::getenv("OPENT1_TEST_ASSETS");
        if (!assets) {
            return;
        }
        if (!std::filesystem::is_regular_file(resolveFilePath(assets, "RPG/scripts.vol"))) {
            return;
        }
        game::FearGameApp::Config config;
        config.assetRoot = assets;
        config.writeRoot = std::filesystem::temp_directory_path() /
                           std::format("opent1-rpg-acceptance-{}",
                                       std::chrono::steady_clock::now().time_since_epoch().count());
        struct Cleanup {
            std::filesystem::path path;
            ~Cleanup() {
                std::error_code ec;
                std::filesystem::remove_all(path, ec);
            }
        } cleanup{config.writeRoot};
        game::FearGameApp app(config);
        const std::array<std::string_view, 2> args{"-mod", "RPG"};
        CHECK(app.init(args));
        CHECK(app.resources().contains("Axe.dts") && app.resources().contains("rpgmap5.mis"));
        app.console().setVariable("PCFG::Name", "CompatRpg");
        app.console().setVariable("PCFG::Script", "");
        app.console().setVariable("PCFG::Info", "compat-dummy-password");
        CHECK(app.loadMission("rpgmap5"));
        CHECK(app.loadedMissionName() == "rpgmap5");
        CHECK(app.fearPlugin().serverGlobals().manager->findObject("MissionGroup"));
        CHECK(app.fearPlugin().serverGlobals().manager->findObject(sim::TerrainId));
        CHECK(app.console().evaluate("return $Client::info[2049, 5];").value ==
              "compat-dummy-password");
        auto surface = gfx::SoftwareSurface::create(320, 240);
        CHECK(surface);
        for (auto frame : std::views::iota(0, 8)) {
            CHECK(app.stepFrame(frame * .032));
            CHECK(app.renderFrame(*surface));
            CHECK(app.console().getVariable("OpenT1::LastRenderError").empty());
            CHECK(app.lastSceneSurfaceCount() > 0 && app.lastScenePolygonCount() > 0);
        }
        CHECK(app.console().evaluate("return fetchData(2049, \"password\");").value ==
              "compat-dummy-password");
        app.game().withWorld(sim::WorldType::Server, [&] {
            CHECK(!script::toBool(app.console().evaluate("return 2049.IsInvalid;").value));
        });
        // A successful profile handshake alone does not establish that new
        // characters can see and complete RPG's mandatory server menus.
        auto& cg = app.fearPlugin().clientGlobals();
        const auto serverMenu = [&]() {
            return dynamic_cast<game::ChatMenu*>(cg.manager->findObject("CurServerMenu"));
        };
        CHECK(cg.playerPSC->scoresVisible() && app.guiRuntime()->canvas.dialogCount() == 1);
        auto* observer = dynamic_cast<game::ObserverCamera*>(cg.playerPSC->getControlObject());
        CHECK(observer && observer->mode() == game::ObserverCamera::FreeRotateMode);
        auto* observers = dynamic_cast<sim::SimGroup*>(
            app.fearPlugin().serverGlobals().manager->findObject("MissionGroup/ObserverDropPoints"));
        CHECK(observers && !observers->objects().empty());
        auto* marker = dynamic_cast<game::Marker*>(observers->objects().front());
        CHECK(marker && marker->getLinearPosition() == Point3F(-2548.25f, -262.375f, 120));
        auto* serverObserver = dynamic_cast<game::ObserverCamera*>(
            app.fearPlugin().serverGlobals().playerPSC->getControlObject());
        CHECK(serverObserver);
        for (const auto* camera : {serverObserver, observer}) {
            const auto& eye = camera->getEyeTransform();
            CHECK((eye.p - marker->getLinearPosition()).len() < .001f);
            for (auto row : std::views::iota(0, 3)) {
                for (auto col : std::views::iota(0, 3)) {
                    CHECK(std::abs(eye.m[row][col] - marker->getTransform().m[row][col]) < .00001f);
                }
            }
        }
        CHECK(app.console().evaluate("return Client::getOwnedObject(2049);").value == "-1");
        CHECK(serverMenu() && serverMenu()->getHeading() == "Pick a group:");
        const auto display = [&](auto&& self, gui::Control* root) -> game::MenuDisplayCtrl* {
            if (auto* menu = dynamic_cast<game::MenuDisplayCtrl*>(root); menu && menu->visible()) {
                return menu;
            }
            if (root) {
                for (auto* child : root->objects()) {
                    if (auto* menu = self(self, dynamic_cast<gui::Control*>(child))) {
                        return menu;
                    }
                }
            }
            return nullptr;
        };
        auto* visibleMenu = display(display, app.guiRuntime()->canvas.top());
        CHECK(visibleMenu && visibleMenu->heading == "Pick a group:" && visibleMenu->entries.size() == 4);
        const auto key = [&](UInt8 scanCode) {
            auto* input = dynamic_cast<sim::SimInputManager*>(cg.manager->findObject(sim::InputManagerId));
            CHECK(input);
            sim::SimInputEvent event;
            event.objInst = scanCode;
            event.fValue = 1;
            CHECK(input->submit(event));
            event.action = sim::InputBreak;
            event.fValue = 0;
            (void)input->submit(event);
        };
        CHECK(app.bindGameplayActions());
        key(15); // Tab closes the menu, retaining the pending character setup.
        CHECK(app.stepFrame(.256) && app.renderFrame(*surface));
        CHECK(!serverMenu() && !cg.playerPSC->scoresVisible());
        key(15); // Tab must reopen the group menu through the installed action map.
        CHECK(app.stepFrame(.288) && app.renderFrame(*surface));
        CHECK(serverMenu() && serverMenu()->getHeading() == "Pick a group:");
        key(4); // 3: Warrior
        CHECK(app.stepFrame(.320) && app.renderFrame(*surface));
        CHECK(serverMenu() && serverMenu()->getHeading() == "Pick a class:");
        CHECK(serverMenu()->curMenu() && serverMenu()->curMenu()->heading == "Fighter");
        key(45); // x: back to group selection
        CHECK(app.stepFrame(.352) && app.renderFrame(*surface));
        CHECK(serverMenu() && serverMenu()->getHeading() == "Pick a group:");
        key(4);
        CHECK(app.stepFrame(.384));
        key(2); // 1: Fighter
        CHECK(app.stepFrame(.416) && app.renderFrame(*surface));
        CHECK(!serverMenu() && !cg.playerPSC->scoresVisible());
        CHECK(app.guiRuntime()->canvas.dialogCount() == 0);
        CHECK(dynamic_cast<game::Player*>(cg.playerPSC->getControlObject()));
        key(15);
        CHECK(app.stepFrame(.448) && app.renderFrame(*surface));
        CHECK(serverMenu() && serverMenu()->getHeading() == "Options");
        key(15);
        CHECK(app.stepFrame(.480));
        CHECK(!serverMenu() && !cg.playerPSC->scoresVisible());
        CHECK(app.console().evaluate("return fetchData(2049, \"CLASS\");").value == "Fighter");
        CHECK(app.console().evaluate("return fetchData(2049, \"HasLoadedAndSpawned\");").value == "true");
        CHECK(app.fearPlugin().focusServer());
        CHECK(app.console().evaluate("return fetchData(2049, \"RACE\");").value == "MaleHuman");
        CHECK(app.fearPlugin().serverGlobals().playerManager->findClient(2049)->skinBase == "rpgbase");
        CHECK(cg.playerManager->findClient(2049)->skinBase == "rpgbase");
        cg.playerPSC->setFirstPerson(false);
        world::WorldShapeQuery playerImage;
        CHECK(cg.playerPSC->getControlObject()->processQuery(playerImage) && playerImage.image);
        CHECK(playerImage.image->resources.materials.list().materials.front().mapFile ==
              "rpgbase.larmor.BMP");
        cg.playerPSC->setFirstPerson(true);
        CHECK(app.console().evaluate("return SaveCharacter(2049);").value == "true");
        CHECK(std::filesystem::is_regular_file(config.writeRoot / "temp/CompatRpg.cs"));
        CHECK(app.console().evaluate("storeData(2049, \"CLASS\", \"\"); LoadCharacter(2049);").ok());
        CHECK(app.console().evaluate("return fetchData(2049, \"CLASS\");").value == "Fighter");
        CHECK(app.console().evaluate("return fetchData(2049, \"password\");").value == "compat-dummy-password");
        CHECK(app.fearPlugin().focusClient());
        gfx::HardwareBatch hardware;
        CHECK(app.renderFrame(*surface, &hardware));
        CHECK(!hardware.draws().empty());
        CHECK(app.fearPlugin().focusServer());
        auto* mission = dynamic_cast<sim::SimGroup*>(
            app.fearPlugin().serverGlobals().manager->findObject("MissionGroup"));
        const auto interior = [](auto&& self, sim::SimGroup& group) -> game::InteriorShape* {
            for (auto* object : group.objects()) {
                if (auto* shape = dynamic_cast<game::InteriorShape*>(object)) {
                    return shape;
                }
                if (auto* child = dynamic_cast<sim::SimGroup*>(object)) {
                    if (auto* found = self(self, *child)) {
                        return found;
                    }
                }
            }
            return nullptr;
        };
        auto* source = mission ? interior(interior, *mission) : nullptr;
        CHECK(source && source->shapeResources());
        auto emf = std::make_unique<sim::InteriorGroupingScript>();
        emf->lines = {"[EMF]", std::string(source->getFileName()), "[END]"};
        CHECK(app.resources().add("compat.emf", std::move(emf)));
        const std::array<std::string, 1> emfArgs{"compat.emf"};
        auto* assembly = dynamic_cast<sim::SimGroup*>(
            app.game().createObject("SimInteriorGrouping", "CompatGrouping", nullptr, emfArgs));
        CHECK(assembly && assembly->objects().size() == 1);
        auto* grouped = dynamic_cast<game::InteriorShape*>(assembly->objects().front());
        CHECK(grouped && grouped->shapeResources());
        CHECK(assembly->deleteObject());
        CHECK(app.fearPlugin().focusClient());
        CHECK(app.console()
                  .evaluate(R"(
            $modList = "base"; EvalSearchPath();
            deleteObject(ScriptsVolume);
            newObject(ScriptsVolume, SimVolume, "scripts.vol");
            exec("server.cs");
        )")
                  .ok());
        CHECK(!app.resources().contains("Axe.dts"));
        CHECK(app.loadMission("Raindance"));
        CHECK(app.loadedMissionName() == "Raindance");
        CHECK(app.console().getVariable("OpenT1::LastMissionError").empty());
        app.shutdown();
        for (const std::string password : {"compat-dummy-password", "compat-wrong-password", ""}) {
            game::FearGameApp returning(config);
            CHECK(returning.init(args));
            returning.console().setVariable("PCFG::Name", "CompatRpg");
            returning.console().setVariable("PCFG::Script", "");
            returning.console().setVariable("PCFG::Info", password);
            CHECK(returning.loadMission("rpgmap5"));
            for (auto frame : std::views::iota(0, 8)) {
                CHECK(returning.stepFrame(frame * .032));
            }
            const bool rejected = password != "compat-dummy-password";
            returning.game().withWorld(sim::WorldType::Server, [&] {
                CHECK(script::toBool(returning.console().evaluate("return 2049.IsInvalid;").value) == rejected);
            });
            auto& returningClient = returning.fearPlugin().clientGlobals();
            CHECK(!returningClient.manager->findObject("CurServerMenu"));
            if (rejected) {
                CHECK(dynamic_cast<game::ObserverCamera*>(returningClient.playerPSC->getControlObject()));
                CHECK(!returning.fearPlugin().centerPrintText().empty());
                for (auto frame : std::views::iota(1) | std::views::take_while([&](int frame) {
                                      return frame <= 81;
                                  })) {
                    CHECK(returning.stepFrame(.224 + frame * .256));
                }
                CHECK(!returning.fearPlugin().isConnected());
                CHECK(returning.console().getVariable("errorString").find("Other info") != std::string::npos);
            } else {
                CHECK(dynamic_cast<game::Player*>(returningClient.playerPSC->getControlObject()));
                CHECK(returning.console().evaluate("return fetchData(2049, \"CLASS\");").value == "Fighter");
            }
            returning.shutdown();
        }
    });
}

OPENT1_REGISTER_TEST("fixpack",
                     "installed_rpg_compatibility",
                     opent1_test_installed_rpg_compatibility);
