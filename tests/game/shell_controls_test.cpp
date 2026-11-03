#include "test_support.hpp"
#include "test_registry.hpp"
#include "gfx_test_support.hpp"
#include <opent1/game/game_app.hpp>
#include <opent1/game/shell_controls.hpp>
#include <opent1/gui/runtime.hpp>
#include <cstdlib>

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
