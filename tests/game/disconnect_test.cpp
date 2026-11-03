#include "test_registry.hpp"
#include <ranges>
#include "test_support.hpp"
#include <opent1/game/game_app.hpp>
#include <opent1/game/shell_controls.hpp>
#include <opent1/core/vol_stream.hpp>
#include <chrono>
#include <cstdlib>

namespace {
using namespace opent1;
using namespace opent1::game;

struct TemporaryDirectory {
    std::filesystem::path path =
        std::filesystem::temp_directory_path() /
        std::format("opent1-disconnect-{}",
                    std::chrono::steady_clock::now().time_since_epoch().count());

    ~TemporaryDirectory() {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }
};

void disconnectShellResources() {
    TemporaryDirectory directory;
    CHECK(std::filesystem::create_directories(directory.path));
    for (const auto* name : {"shell", "mission", "ghost"}) {
        VolumeRWStream volume;
        const std::string contents = name;
        CHECK(volume.put(contents + ".txt", std::as_bytes(std::span(contents))));
        CHECK(volume.save(directory.path / (contents + ".vol")));
    }
    FearGameApp app;
    CHECK(app.init());
    CHECK(app.resources().setSearchPaths(std::array{directory.path}));
    CHECK(app.fearPlugin().createClientWorld());
    CHECK(app.fearPlugin().focusClient());
    CHECK(app.console()
              .evaluate(R"(
        newObject(ShellVolume, SimVolume, "shell.vol");
        newObject(MainWindow, "SimGui::Canvas");
        function EndGame() {
            $ConnectedToServer = false;
            $DisconnectResult = disconnect();
        }
    )")
              .ok());
    auto* shell = app.fearPlugin().clientGlobals().manager->findObject("ShellVolume");
    auto* canvas = app.fearPlugin().clientGlobals().manager->findObject("MainWindow");
    CHECK(shell && canvas);
    auto button = std::make_unique<FGUniversalButton>();
    auto* click = button.get();
    auto fields = click->fields();
    fields.command = "EndGame();";
    CHECK(click->configure(fields));
    CHECK(app.guiRuntime()->show(gui::Graph::create(std::move(button))));
    for ([[maybe_unused]] auto attempt : std::views::iota(0, 2)) {
        CHECK(app.fearPlugin().createServerWorld());
        CHECK(app.console().execute("localConnect").value == "True");
        CHECK(app.fearPlugin().focusClient());
        CHECK(app.console()
                  .evaluate(R"(
            newObject(MissionGroup, SimGroup);
            newObject(MissionVolume, SimVolume, "mission.vol");
            addToSet(MissionGroup, MissionVolume);
        )")
                  .ok());
        auto& manager = *app.fearPlugin().clientGlobals().manager;
        auto ghost = std::make_unique<sim::SimVolume>();
        ghost->assignName("GhostVolume");
        CHECK(ghost->setFileName("ghost.vol"));
        CHECK(ghost->setNetFlags(ghost->netFlags() | sim::SimNetObject::IsGhost));
        CHECK(manager.addObject(std::move(ghost)));
        CHECK(app.resources().contains("shell.txt") && app.resources().contains("mission.txt") &&
              app.resources().contains("ghost.txt"));
        CHECK(app.guiRuntime()->canvas.performClick(*click));
        CHECK(app.console().getVariable("DisconnectResult") == "True");
        CHECK(!app.fearPlugin().isConnected() && !app.fearPlugin().serverGlobals().world);
        const auto* replacement = app.fearPlugin().clientGlobals().manager;
        CHECK(replacement && replacement->findObject("ShellVolume") == shell &&
              replacement->findObject("MainWindow") == canvas);
        CHECK(!replacement->findObject("MissionGroup") && !replacement->findObject("GhostVolume"));
        CHECK(app.resources().contains("shell.txt"));
        CHECK(!app.resources().contains("mission.txt") && !app.resources().contains("ghost.txt"));
    }
}

gui::Active* disconnectButton(gui::Control& root) {
    if (auto* button = dynamic_cast<gui::Active*>(&root);
        button && button->fields().command == "EndGame();") {
        return button;
    }
    for (auto* object : root.objects()) {
        if (auto* control = dynamic_cast<gui::Control*>(object)) {
            if (auto* button = disconnectButton(*control)) {
                return button;
            }
        }
    }
    return nullptr;
}

void installedDisconnect() {
    const auto* assets = std::getenv("OPENT1_TEST_ASSETS");
    if (!assets ||
        !std::filesystem::is_regular_file(std::filesystem::path(assets) / "RPG/scripts.vol")) {
        return;
    }
    FearGameApp::Config config;
    config.assetRoot = assets;
    TemporaryDirectory directory;
    config.writeRoot = directory.path;
    FearGameApp app(config);
    const std::array<std::string_view, 2> args{"-mod", "RPG"};
    CHECK(app.init(args));
    app.console().setVariable("PCFG::Name", "DisconnectTest");
    app.console().setVariable("PCFG::Info", "disconnect-test");
    CHECK(app.loadMission("rpgmap5"));
    for (auto frame : std::views::iota(0, 8)) {
        CHECK(app.stepFrame(frame * .032));
    }
    CHECK(app.fearPlugin().focusServer());
    CHECK(app.console()
              .evaluate("processMenupickgroup(2049, 3); processMenupickclass(2049, 1);")
              .ok());
    for (auto frame : std::views::iota(8, 16)) {
        CHECK(app.stepFrame(frame * .032));
    }
    CHECK(app.setGameplayGuiMode(1));
    CHECK(app.guiRuntime()->canvas.top()->getName() == "playGui");
    CHECK(app.escapeGameplay());
    CHECK(app.guiRuntime()->canvas.top()->getName() == "LobbyGui");
    auto* button = disconnectButton(*app.guiRuntime()->canvas.top());
    CHECK(button && button->active && app.fearPlugin().isConnected());
    app.console().setVariable("pref::PlayGameMode", "HOST");
    CHECK(app.resources().contains("gui/MainMenu.gui"));
    CHECK(app.guiRuntime()->canvas.performClick(*button));
    CHECK(!app.fearPlugin().isConnected());
    CHECK(!app.fearPlugin().serverGlobals().world);
    CHECK(app.resources().contains("gui/MainMenu.gui"));
    CHECK(app.guiRuntime()->canvas.top()->getName() == "CreateServerGui");
    CHECK(std::ranges::none_of(app.console().log(), [](const auto& line) {
        return line.starts_with("GUI resource missing:");
    }));
}
}

int opent1_test_disconnect_shell_resources() {
    return opent1::test::run(disconnectShellResources);
}

int opent1_test_installed_disconnect() {
    return opent1::test::run(installedDisconnect);
}

OPENT1_REGISTER_TEST("game", "disconnect_shell_resources", opent1_test_disconnect_shell_resources);
OPENT1_REGISTER_TEST("fixpack", "installed_disconnect", opent1_test_installed_disconnect);
