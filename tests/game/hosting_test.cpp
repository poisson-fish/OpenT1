#include "test_registry.hpp"
#include "test_support.hpp"
#include <opent1/core/mem_stream.hpp>
#include <opent1/game/fear_player_psc.hpp>
#include <opent1/game/game_app.hpp>
#include <opent1/game/shell_controls.hpp>
#include <algorithm>
#include <chrono>
#include <cstdlib>

namespace {
using namespace opent1;

struct TemporaryDirectory {
    std::filesystem::path path =
        std::filesystem::temp_directory_path() /
        std::format("opent1-hosting-{}",
                    std::chrono::steady_clock::now().time_since_epoch().count());

    ~TemporaryDirectory() {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }
};

bool clickHost(game::FearGameApp& app, gfx::SoftwareSurface& surface) {
    auto runtime = app.guiRuntime();
    auto* button = dynamic_cast<gui::Active*>(runtime->findControl("CreateServerNow"));
    CHECK(button && button->active);
    gui::Event mouse;
    mouse.point =
        button->localToGlobal({button->fields().extent.x / 2, button->fields().extent.y / 2});
    mouse.type = gui::EventType::Move;
    CHECK(runtime->canvas.dispatch(mouse));
    mouse.type = gui::EventType::Down;
    CHECK(runtime->canvas.dispatch(mouse));
    CHECK(app.renderFrame(surface));
    mouse.type = gui::EventType::Up;
    return runtime->canvas.dispatch(mouse);
}

void checkHostError(game::FearGameApp& app, gfx::SoftwareSurface& surface, UInt16 port) {
    auto runtime = app.guiRuntime();
    CHECK(!app.fearPlugin().serverGlobals().world && !app.fearPlugin().isConnected());
    const auto error = app.console().getVariable("OpenT1::LastGuiError");
    CHECK(error.find("UDP port " + std::to_string(port)) != std::string::npos);
    CHECK(error.find("retry") != std::string::npos);
    CHECK(app.renderFrame(surface));
    CHECK(runtime->canvas.dialogCount() == 1);
    CHECK(runtime->canvas.content()->getName() == "CreateServerGui");
    CHECK(runtime->command("Control::getValue",
                           std::array<std::string, 1>{"MessageDialogTextFormat"}) == error);
    CHECK(std::ranges::any_of(app.console().log(), [&](const auto& line) {
        return line.find(error) != std::string::npos;
    }));
}

void hostPortConflict() {
    game::FearGameApp app;
    CHECK(app.init());
    auto runtime = app.guiRuntime();
    CHECK(app.console()
              .evaluate(R"(
        function createServer(%mission, %dedicated) {
            GuiLoadContentCtrl("gui/Loading.gui");
            newServer(%mission, %dedicated);
            localConnect();
        }
    )")
              .ok());
    app.console().setVariable("OpenT1::ConsoleCSExecuted", "true");
    app.console().setVariable("pref::LastMission", "HostRegression");

    // Synthetic shell resources exercise the real delegate without game assets.
    gui::Control dialog;
    dialog.assignName("MessageDialog");
    gui::Text text;
    text.assignName("MessageDialogTextFormat");
    CHECK(dialog.resize({}, {320, 160}) && dialog.addObject(text));
    std::vector<std::byte> bytes(4096);
    MemRWStream stream{std::span<std::byte>(bytes)};
    Persistent::StoreContext store(app.game().classes());
    CHECK(store.store(stream, dialog) == Persistent::Error::Ok);
    bytes.resize(std::size_t(stream.getPosition()));
    runtime->loadFile = [bytes](std::string_view) {
        return std::optional(bytes);
    };

    auto root = std::make_unique<gui::Control>();
    root->assignName("CreateServerGui");
    game::FGUniversalButton host;
    host.assignName("CreateServerNow");
    host.gui::Active::message = 140120;
    CHECK(root->resize({}, {640, 480}) && host.resize({20, 20}, {140, 30}) &&
          root->addObject(host));
    CHECK(runtime->show(gui::Graph::create(std::move(root)),
                        false,
                        runtime->loadDelegate("SimGui::CreateServerDelegate")));
    auto surface = gfx::SoftwareSurface::create(640, 480);
    CHECK(surface && app.renderFrame(*surface));

    auto occupied = net::makeUdpTransport();
    const auto address = net::parseIPv4Address(occupied->localAddress());
    CHECK(address && address->port);
    app.console().setVariable("Server::Port", std::to_string(address->port));
    for (int attempt = 0; attempt < 2; ++attempt) {
        CHECK(!clickHost(app, *surface));
        checkHostError(app, *surface, address->port);
        CHECK(runtime->pop());
    }
    occupied.reset();
    CHECK(clickHost(app, *surface));
    CHECK(app.fearPlugin().serverGlobals().world && app.fearPlugin().isConnected());
    CHECK(app.console().getVariable("OpenT1::LastGuiError").empty());
    const auto bound = net::parseIPv4Address(
        app.fearPlugin().serverGlobals().csDelegate->delegate()->localAddress());
    CHECK(bound && bound->port == address->port);
}

void installedHostPortConflict() {
    const auto* assets = std::getenv("OPENT1_TEST_ASSETS");
    if (!assets || !*assets) {
        return;
    }
    for (const bool rpg : {false, true}) {
        const auto mod = std::filesystem::path(assets) / "RPG";
        if (rpg && !std::filesystem::exists(mod / "scripts.vol") &&
            !std::filesystem::exists(mod / "Server.cs")) {
            continue;
        }
        TemporaryDirectory directory;
        game::FearGameApp::Config config;
        config.assetRoot = assets;
        config.writeRoot = directory.path;
        game::FearGameApp app(config);
        CHECK(rpg ? app.init(std::array<std::string_view, 2>{"-mod", "RPG"}) : app.init());
        auto runtime = app.guiRuntime();
        CHECK(runtime->open("gui/CreateServer.gui"));
        app.console().setVariable("pref::LastMission", rpg ? "rpgmap5" : "Raindance");
        app.console().setVariable("PCFG::Name", "HostRegression");
        app.console().setVariable("PCFG::Script", "");
        app.console().setVariable("PCFG::Info", "hosting-test-password");
        app.console().setVariable("Server::Heartbeats", "false");
        auto occupied = net::makeUdpTransport();
        const auto address = net::parseIPv4Address(occupied->localAddress());
        CHECK(address && address->port);
        app.console().setVariable("Server::Port", std::to_string(address->port));
        auto surface = gfx::SoftwareSurface::create(1280, 720);
        CHECK(surface && app.renderFrame(*surface));
        CHECK(!clickHost(app, *surface));
        checkHostError(app, *surface, address->port);
        auto* close = dynamic_cast<gui::Active*>(runtime->findControl("DialogCloseButton"));
        CHECK(close && runtime->canvas.performClick(*close));
        CHECK(runtime->canvas.dialogCount() == 0);
        occupied.reset();
        CHECK(clickHost(app, *surface));
        for (int frame = 0; frame < 8; ++frame) {
            CHECK(app.stepFrame(frame * .032, false));
        }
        CHECK(app.renderFrame(*surface));
        CHECK(app.fearPlugin().serverGlobals().world && app.fearPlugin().isConnected());
        // RPG may put its ordinary startup information dialog above playGui.
        CHECK(runtime->canvas.content()->getName() == "playGui");
        const auto* psc = app.fearPlugin().clientGlobals().playerPSC;
        CHECK(psc && psc->getControlObject());
        CHECK(app.console().getVariable("OpenT1::LastGuiError").empty());
        std::printf("%s: occupied UDP port %u reports an error; same-port retry reaches playGui\n",
                    rpg ? "RPG" : "base",
                    unsigned(address->port));
    }
}
}

int opent1_test_host_port_conflict() {
    return test::run(hostPortConflict);
}

int opent1_test_installed_host_port_conflict() {
    return test::run(installedHostPortConflict);
}

OPENT1_REGISTER_TEST("game", "host_port_conflict", opent1_test_host_port_conflict);
OPENT1_REGISTER_TEST("hosting",
                     "installed_host_port_conflict",
                     opent1_test_installed_host_port_conflict);
