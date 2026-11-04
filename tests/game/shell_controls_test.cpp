#include "test_support.hpp"
#include "test_registry.hpp"
#include "gfx_test_support.hpp"
#include <opent1/game/game_app.hpp>
#include <opent1/game/shell_controls.hpp>
#include <opent1/gui/runtime.hpp>
#include <opent1/core/file_stream.hpp>
#include <cstdlib>
#include <fstream>
#ifdef OPENT1_SHELL_GPU
#include <opent1/gfx/sdl_gpu_surface.hpp>
#endif

namespace {
using namespace opent1;

void missionSelection() {
    Console console;
    Persistent::ClassRegistry registry;
    game::FGTextList list;
    auto runtime = std::make_shared<gui::Runtime>(console, registry);
    runtime->install();
    auto root = std::make_unique<gui::Control>();
    CHECK(root->resize({}, {320, 240}));
    auto fields = list.fields();
    fields.position = {10, 10};
    fields.extent = {160, 100};
    fields.variable = "pref::LastMission";
    CHECK(list.configure(fields));
    list.assignName("MissionTextList");
    list.font = std::make_shared<gfx::BitmapFont>(gfx_test::font(0));
    CHECK(list.insert({0, "Broadside"}) && list.insert({0, "Raindance"}) &&
          list.insert({0, "Stonehenge"}) && root->addObject(list));
    console.setVariable("pref::LastMission", "Broadside");
    CHECK(console
              .evaluate(R"(
        function MissionTextList::onAction() {
            $MissionDescription = Control::getValue(MissionTextList);
            $MissionActions++;
        }
    )")
              .ok());
    CHECK(runtime->show(gui::Graph::create(std::move(root))));
    CHECK(list.selection() && list.selection()->text == "Broadside");

    const auto point = list.localToGlobal({20, list.cellSize.y + 2});
    gui::Event mouse;
    mouse.type = gui::EventType::Move;
    mouse.point = point;
    CHECK(runtime->canvas.dispatch(mouse));
    mouse.type = gui::EventType::Down;
    CHECK(runtime->canvas.dispatch(mouse));
    CHECK(list.selection()->text == "Raindance");
    // A rendered frame while the button is held must not restore the old pref.
    CHECK(runtime->synchronizeVariables());
    CHECK(list.selection()->text == "Raindance");
    CHECK(console.getVariable("pref::LastMission") == "Broadside");
    mouse.type = gui::EventType::Up;
    CHECK(runtime->canvas.dispatch(mouse));
    CHECK(console.getVariable("pref::LastMission") == "Raindance");
    CHECK(console.getVariable("MissionDescription") == "Raindance");
    CHECK(console.getVariable("MissionActions") == "1");
    CHECK(runtime->synchronizeVariables());
    CHECK(list.selection()->text == "Raindance");

    gui::Event down;
    down.type = gui::EventType::KeyDown;
    down.key = gui::Key::Down;
    CHECK(runtime->canvas.dispatch(down));
    CHECK(console.getVariable("pref::LastMission") == "Stonehenge");
    CHECK(console.getVariable("MissionDescription") == "Stonehenge");
    CHECK(console.getVariable("MissionActions") == "2");
    CHECK(runtime->command("Control::setValue",
                           std::array<std::string, 2>{"MissionTextList", "Broadside"}) == "True");
    CHECK(list.selection()->text == "Broadside");
}

void selectedRowRendering() {
    game::FGTextList list;
    list.cellSize = {80, 16};
    list.font = std::make_shared<gfx::BitmapFont>(gfx_test::font(0));
    list.highlightFont = std::make_shared<gfx::BitmapFont>(gfx_test::font(1));
    CHECK(list.insert({0, "ABC"}));
    auto normal = gfx::SoftwareSurface::create(80, 16);
    auto hovered = gfx::SoftwareSurface::create(80, 16);
    auto selected = gfx::SoftwareSurface::create(80, 16);
    CHECK(normal && hovered && selected);
    normal->clear(7);
    hovered->clear(7);
    selected->clear(7);
    CHECK(list.drawCell(*normal, {}, {}, false, false));
    CHECK(list.drawCell(*hovered, {}, {}, false, true));
    CHECK(list.drawCell(*selected, {}, {}, true, false));
    CHECK(std::ranges::equal(normal->pixels(), hovered->pixels()));
    CHECK(selected->pixels().front() == std::byte{254});
    CHECK(selected->pixels().back() == std::byte{254});
}

void installedMissionSelection() {
    const auto* assets = std::getenv("OPENT1_TEST_ASSETS");
    if (!assets || !*assets) {
        return;
    }
    game::FearGameApp::Config config;
    config.assetRoot = assets;
    config.writeRoot =
        std::filesystem::temp_directory_path() /
        std::format("opent1-shell-{}", std::chrono::steady_clock::now().time_since_epoch().count());

    struct Cleanup {
        std::filesystem::path path;

        ~Cleanup() {
            std::error_code ec;
            std::filesystem::remove_all(path, ec);
        }
    } cleanup{config.writeRoot};

    game::FearGameApp app(config);
    CHECK(app.init());
    auto runtime = app.guiRuntime();
    CHECK(runtime && runtime->open("gui\\CreateServer.gui"));
    auto* list = dynamic_cast<game::FGTextList*>(runtime->findControl("MissionTextList"));
    CHECK(list && list->entries.size() >= 2);
    CHECK(list->variableChanged(list->entries.front().text));
    CHECK(app.console().execute("MissionTextList::onAction").ok());
    const auto previous = list->entries.front().text;
    const auto expected = list->entries[1].text;
    auto surface = gfx::SoftwareSurface::create(640, 480);
    CHECK(surface && app.renderFrame(*surface));
    gui::Event mouse;
    mouse.point = list->localToGlobal({20, list->cellSize.y + 2});
    mouse.type = gui::EventType::Move;
    CHECK(runtime->canvas.dispatch(mouse));
    mouse.type = gui::EventType::Down;
    CHECK(runtime->canvas.dispatch(mouse));
    CHECK(list->selection() && list->selection()->text == expected);
    CHECK(app.stepFrame(.032) && app.renderFrame(*surface));
    CHECK(list->selection()->text == expected);
    CHECK(app.console().getVariable("pref::LastMission") == previous);
    mouse.type = gui::EventType::Up;
    CHECK(runtime->canvas.dispatch(mouse));
    CHECK(app.console().getVariable("pref::LastMission") == expected);
    CHECK(app.stepFrame(.064) && app.renderFrame(*surface));
    CHECK(list->selection()->text == expected);

    // The original onAction must refresh the mission description as well.
    const auto count = std::stoi(app.console().getVariable("MLIST::Count"));
    bool described = false;
    for (int i = 0; i < count; ++i) {
        if (app.console().getVariable("MLIST::EName" + std::to_string(i)) == expected) {
            CHECK(runtime->command("Control::getValue",
                                   std::array<std::string, 1>{"MissionDescText"}) ==
                  app.console().getVariable("MLIST::EText" + std::to_string(i)));
            described = true;
            break;
        }
    }
    CHECK(described);
    std::printf("Installed host screen: %s -> %s across a rendered mouse-down frame\n",
                previous.c_str(),
                expected.c_str());
}

void installedShellArtwork() {
    const auto* assets = std::getenv("OPENT1_TEST_ASSETS");
    if (!assets || !*assets) {
        return;
    }
    const auto run = [&](bool rpg, bool overrideArtwork) {
        if (rpg && !std::filesystem::exists(std::filesystem::path(assets) / "RPG/scripts.vol")) {
            return;
        }
        game::FearGameApp::Config config;
        config.assetRoot = assets;
        config.writeRoot = std::filesystem::temp_directory_path() /
                           std::format("opent1-shell-art-{}",
                                       std::chrono::steady_clock::now().time_since_epoch().count());
        struct Cleanup {
            std::filesystem::path path;

            ~Cleanup() {
                std::error_code ec;
                std::filesystem::remove_all(path, ec);
            }
        } cleanup{config.writeRoot};
        // A disposable write overlay exercises a real loose mod replacement
        // without changing the external installation or copying its art.
        const auto replacementPath = config.writeRoot / "RPG/DarkLogo.bmp";
        if (overrideArtwork) {
            std::filesystem::create_directories(replacementPath.parent_path());
            auto replacement = gfx::Bitmap::create(640, 480);
            CHECK(replacement);
            std::fill(replacement->pixels.begin(), replacement->pixels.end(), std::byte{222});
            FileWStream file(replacementPath);
            CHECK(replacement->write(file) && file.flush());
        }
        game::FearGameApp app(config);
        CHECK(rpg ? app.init(std::array<std::string_view, 2>{"-mod", "RPG"}) : app.init());
        auto runtime = app.guiRuntime();
        CHECK(runtime);
        for (const auto name : {"DarkLogo.bmp",
                                "Background0.bmp",
                                "Background1.bmp",
                                "Background2.bmp",
                                "BackgroundGrid.bmp",
                                "Loading.bmp",
                                "CUR_Arrow.bmp",
                                "BTN_QuickStart_DF.bmp"}) {
            CHECK(app.resources().load<gfx::Bitmap>(name));
            const auto path = app.resources().getFullPath(name);
            CHECK(path);
            std::printf("%s %s: %s\n", rpg ? "RPG" : "base", name, path->string().c_str());
        }
        const auto frames = app.resources().load<gfx::BitmapArray>("ShellBorderCtrl.pba");
        CHECK(frames && frames->bitmaps.size() >= 15);
        CHECK(app.resources().load<gfx::Palette>("Shell.ppl"));
        if (overrideArtwork) {
            CHECK((app.resources().getFullPath("DarkLogo.bmp") == replacementPath) == rpg);
        }
        for (const auto screen :
             {"Startup",
              "gui/MainMenu.gui",
              "gui/Options.gui",
              "gui/CreateServer.gui",
              "gui/Loading.gui"}) {
            if (std::string_view(screen) == "gui/MainMenu.gui") {
                CHECK(app.console().execute("startMainMenuScreen").ok());
                const auto cursor = app.resources().load<gfx::Bitmap>("CUR_Arrow.bmp");
                CHECK(cursor && runtime->canvas.cursor.bitmap);
                CHECK(cursor->pixels == runtime->canvas.cursor.bitmap->pixels);
                CHECK(runtime->canvas.setCursorPosition({60, 150}));
            } else if (std::string_view(screen) != "Startup") {
                CHECK(runtime->open(screen));
            }
            CHECK(runtime->canvas.top());
            std::printf("%s screen %s\n", rpg ? "RPG" : "base", screen);
            auto surface = gfx::SoftwareSurface::create(1280, 720);
            CHECK(surface && app.renderFrame(*surface));
#ifdef OPENT1_SHELL_GPU
            if (const auto* driver = std::getenv("OPENT1_SHELL_GPU")) {
                platform::SdlApplication application;
                CHECK(application.initialize({}) == SDL_APP_CONTINUE);
                platform::WindowConfig wc;
                wc.hidden = true;
                wc.width = 1280;
                wc.height = 720;
                auto& window = application.createWindow(wc);
                gfx::GpuSurfaceConfig gc;
                gc.driver = driver;
                auto gpu = gfx::SdlGpuSurface::create(window, wc.width, wc.height, gc);
                CHECK(gpu);
                gfx::HardwareBatch batch;
                CHECK(app.renderFrame(gpu->software(), &batch));
                CHECK(gpu->render(batch));
                std::vector<std::byte> actual(1280 * 720 * 4), expected(actual.size());
                CHECK(gpu->readback(actual));
                CHECK(surface->rgba(expected));
                CHECK(actual == expected);
                std::printf("%s %s %s GPU matches software\n", rpg ? "RPG" : "base", screen, driver);
            }
#endif
            if (const auto* output = std::getenv("OPENT1_SHELL_CAPTURE"); output && !overrideArtwork) {
                const auto path = std::filesystem::path(output) /
                                  std::format("{}-{}.ppm",
                                              rpg ? "rpg" : "base",
                                              std::filesystem::path(screen).stem().string());
                std::ofstream file(path, std::ios::binary);
                file << "P6\n1280 720\n255\n";
                std::vector<std::byte> rgba(1280 * 720 * 4);
                CHECK(surface->rgba(rgba));
                for (std::size_t i = 0; i < rgba.size(); i += 4) {
                    file.write(reinterpret_cast<const char*>(rgba.data() + i), 3);
                }
                CHECK(file.good());
                if (std::string_view(screen) == "gui/MainMenu.gui") {
                    auto cursor = *runtime->canvas.cursor.bitmap;
                    cursor.palette = *app.defaultPalette();
                    FileWStream cursorFile(std::filesystem::path(output) /
                                           std::format("{}-cursor.bmp", rpg ? "rpg" : "base"));
                    CHECK(cursor.write(cursorFile, gfx::BitmapFormat::Microsoft));
                }
            }
        }

        CHECK(runtime->open("gui/MainMenu.gui"));
        auto* root = dynamic_cast<game::FGShellBorder*>(runtime->canvas.top());
        CHECK(root && root->bitmapName() == "DarkLogo.bmp");
        // Hide the labels, animation and buttons to inspect the bound background
        // across the menu interior, independently of a successful file decode.
        for (auto* child : root->objects()) {
            static_cast<gui::Control*>(child)->visible(false);
        }
        CHECK(runtime->canvas.setCursorPosition({0, 0}));
        auto actual = gfx::SoftwareSurface::create(640, 480);
        auto expected = gfx::SoftwareSurface::create(640, 480);
        CHECK(actual && expected && app.renderFrame(*actual));
        CHECK(expected->setPalette(*app.defaultPalette()));
        gui::Bitmap background;
        background.bitmap =
            std::make_shared<gfx::Bitmap>(*app.resources().load<gfx::Bitmap>("DarkLogo.bmp"));
        background.stretch = true;
        CHECK(background.resize({}, {640, 480}) && background.draw(*expected, {}, {0, 0, 639, 479}));
        bool nonzero = false;
        for (Int32 y = 40; y < 430; ++y) {
            const auto row = actual->bitmap().row(y).subspan(40, 550);
            CHECK(std::ranges::equal(row, expected->bitmap().row(y).subspan(40, 550)));
            nonzero |= std::ranges::any_of(row, [](std::byte b) {
                return b != std::byte{};
            });
        }
        CHECK(nonzero);
        if (rpg && overrideArtwork) {
            CHECK(actual->bitmap().row(240)[320] == std::byte{222});
        }
        std::printf("%s %s background reaches framebuffer\n",
                    rpg ? "RPG" : "base",
                    overrideArtwork ? "with mod replacement" : "stock");
    };
    for (const bool overrideArtwork : {false, true}) {
        for (const bool rpg : {false, true}) {
            run(rpg, overrideArtwork);
        }
    }
}

void installedRpgDeferredLoading() {
    const auto* assets = std::getenv("OPENT1_TEST_ASSETS");
    if (!assets || !std::filesystem::exists(std::filesystem::path(assets) / "RPG/scripts.vol")) {
        return;
    }
    game::FearGameApp::Config config;
    config.assetRoot = assets;
    config.writeRoot = std::filesystem::temp_directory_path() /
                       std::format("opent1-loading-{}",
                                   std::chrono::steady_clock::now().time_since_epoch().count());
    struct Cleanup {
        std::filesystem::path path;

        ~Cleanup() {
            std::error_code ec;
            std::filesystem::remove_all(path, ec);
        }
    } cleanup{config.writeRoot};
    game::FearGameApp app(config);
    app.deferMissionLoading(true);
    CHECK(app.init(std::array<std::string_view, 4>{"-mod", "RPG", "--mission", "rpgmap5"}));
    CHECK(app.missionPending() && app.loadedMissionName().empty());
    CHECK(app.guiRuntime()->canvas.top()->getName() == "LoadingGui");
    const auto image = app.resources().load<gfx::Bitmap>("Loading.bmp");
    const auto cursor = app.resources().load<gfx::Bitmap>("CUR_Arrow.bmp");
    CHECK(image && cursor && app.guiRuntime()->canvas.cursor.bitmap);
    CHECK(cursor->pixels == app.guiRuntime()->canvas.cursor.bitmap->pixels);
    auto surface = gfx::SoftwareSurface::create(640, 480);
    CHECK(surface && app.renderFrame(*surface));
    auto reference = gfx::SoftwareSurface::create(640, 480);
    CHECK(reference && reference->setPalette(*app.defaultPalette()));
    gui::Bitmap background;
    background.bitmap = std::make_shared<gfx::Bitmap>(*image);
    background.stretch = true;
    CHECK(background.resize({}, {640, 480}) && background.draw(*reference, {}, {0, 0, 639, 479}));
    CHECK(surface->bitmap().row(240)[320] == reference->bitmap().row(240)[320]);
    app.console().setVariable("Server::Port", "0");
    app.console().setVariable("Server::Heartbeats", "false");
    app.console().setVariable("PCFG::Name", "LoadingTest");
    app.console().setVariable("PCFG::Script", "");
    CHECK(app.loadPendingMission());
    CHECK(!app.missionPending() && app.loadedMissionName() == "rpgmap5");
    CHECK(!app.loadPendingMission());
}
}

int opent1_test_shell_mission_selection() {
    return test::run(missionSelection);
}

int opent1_test_shell_selected_row_rendering() {
    return test::run(selectedRowRendering);
}

int opent1_test_installed_shell_mission_selection() {
    return test::run(installedMissionSelection);
}

OPENT1_REGISTER_TEST("game", "shell_mission_selection", opent1_test_shell_mission_selection);
OPENT1_REGISTER_TEST("game",
                     "shell_selected_row_rendering",
                     opent1_test_shell_selected_row_rendering);
OPENT1_REGISTER_TEST("shell",
                     "installed_mission_selection",
                     opent1_test_installed_shell_mission_selection);

int opent1_test_installed_shell_artwork() {
    return test::run(installedShellArtwork);
}

OPENT1_REGISTER_TEST("shell_artwork", "installed_artwork", opent1_test_installed_shell_artwork);

int opent1_test_installed_rpg_loading() {
    return test::run(installedRpgDeferredLoading);
}

OPENT1_REGISTER_TEST("shell_loading", "installed_rpg_loading", opent1_test_installed_rpg_loading);
