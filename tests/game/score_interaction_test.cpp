#include "test_registry.hpp"
#include "test_support.hpp"
#include <opent1/game/chat_inventory_huds.hpp>
#include <opent1/game/game_app.hpp>
#include <opent1/game/shell_controls.hpp>
#include <algorithm>
#include <chrono>
#include <cstdlib>

namespace {
using namespace opent1;

template <class T> T* find(gui::Control& control) {
    if (auto* result = dynamic_cast<T*>(&control)) {
        return result;
    }
    for (auto* child : control.objects()) {
        if (auto* result = find<T>(*static_cast<gui::Control*>(child))) {
            return result;
        }
    }
    return nullptr;
}

struct TemporaryDirectory {
    std::filesystem::path path =
        std::filesystem::temp_directory_path() /
        std::format("opent1-score-menu-{}",
                    std::chrono::steady_clock::now().time_since_epoch().count());

    ~TemporaryDirectory() {
        std::error_code error;
        std::filesystem::remove_all(path, error);
    }
};

void populate(game::PlayerManager& players) {
    players.setClientScoreHeading("Player\t\x70Score");
    players.setTeamScoreHeading("Team\t\x70Score");
    players.teamAdded("Team");
    for (Int32 i = 0; i < 48; ++i) {
        const auto id = 2049 + i;
        CHECK(players.clientAdded("Player" + std::to_string(i), "male1", "base", true, id) == id);
        players.setClientScore(id, "%n\t7", 48 - i);
    }
}

void click(gui::Canvas& canvas, Point2I point) {
    gui::Event event;
    event.point = point;
    event.type = gui::EventType::Move;
    (void)canvas.dispatch(event);
    event.type = gui::EventType::Down;
    CHECK(canvas.dispatch(event));
    event.type = gui::EventType::Up;
    CHECK(canvas.dispatch(event));
}

void clickRow(gui::Canvas& canvas, game::ScoreListCtrl& list, Int32 row) {
    // Select a visible point after either scrollbar has moved the list.
    const auto x = std::max(0, -list.fields().position.x) + 8;
    click(canvas, list.localToGlobal({x, row * list.cellSize.y + list.cellSize.y / 2}));
}

Point2I center(gui::RectI rect) {
    return {(rect.left + rect.right) / 2, (rect.top + rect.bottom) / 2};
}

struct Fixture {
    game::FearGameApp app;
    game::FearGuiScrollCtrl scroll;
    game::ScoreListCtrl list;
    std::unique_ptr<gfx::SoftwareSurface> surface;
    game::PlayerManager* players{};

    Fixture() {
        CHECK(app.init());
        CHECK(app.fearPlugin().createServerWorld({}, true));
        CHECK(app.console().execute("localConnect").value == "True");
        players = app.fearPlugin().clientGlobals().playerManager;
        CHECK(players);
        players->reset();
        populate(*players);
        auto root = std::make_unique<gui::Control>();
        CHECK(root->resize({}, {640, 480}));
        scroll.horizontalPolicy = scroll.verticalPolicy = gui::Scroll::Dynamic;
        CHECK(scroll.resize({20, 20}, {262, 262}));
        CHECK(root->addObject(scroll) && scroll.addObject(list));
        CHECK(app.guiRuntime()->show(gui::Graph::create(std::move(root))));
        surface = gfx::SoftwareSurface::create(640, 480);
        CHECK(surface);
        refresh();
    }

    void refresh() {
        CHECK(app.renderFrame(*surface));
        CHECK(scroll.computeSizes());
    }

    gui::Canvas& canvas() {
        return app.guiRuntime()->canvas;
    }
};

void scoreScrolling() {
    Fixture fixture;
    auto& list = fixture.list;
    auto& scroll = fixture.scroll;
    CHECK(list.cellSize.x == 640 && list.cells.x == 1);
    CHECK(scroll.geometry().horizontalEnabled && scroll.geometry().verticalEnabled);

    gui::Event wheel;
    wheel.type = gui::EventType::Wheel;
    wheel.point = scroll.content()->localToGlobal({10, 40});
    wheel.wheel = -1;
    CHECK(fixture.canvas().dispatch(wheel));
    CHECK(list.fields().position.y == -list.cellSize.y);
    click(fixture.canvas(), scroll.localToGlobal(center(scroll.geometry().right)));
    CHECK(list.fields().position.x < 0);

    // Drag each native scrollbar thumb through Canvas, not by changing offsets.
    const auto drag = [&](bool horizontal) {
        const auto geometry = scroll.geometry();
        const auto track = horizontal ? geometry.horizontalTrack : geometry.verticalTrack;
        const auto thumb = horizontal ? geometry.horizontalThumb : geometry.verticalThumb;
        const auto size = horizontal ? geometry.horizontalSize : geometry.verticalSize;
        gui::Event event;
        event.type = gui::EventType::Down;
        event.point = scroll.localToGlobal(horizontal ? Point2I{thumb + size / 2, center(track).y}
                                                      : Point2I{center(track).x, thumb + size / 2});
        CHECK(fixture.canvas().dispatch(event));
        event.type = gui::EventType::Drag;
        event.point =
            scroll.localToGlobal(horizontal ? Point2I{track.right - size / 2, center(track).y}
                                            : Point2I{center(track).x, track.bottom - size / 2});
        CHECK(fixture.canvas().dispatch(event));
        event.type = gui::EventType::Up;
        CHECK(fixture.canvas().dispatch(event));
    };
    drag(true);
    drag(false);
    CHECK(scroll.geometry().x > .9f && scroll.geometry().y > .9f);
    const auto position = list.fields().position;
    fixture.players->setClientScore(2049, "%n\t8", 48);
    fixture.refresh();
    CHECK(list.fields().position == position && list.cellSize.x == 640);

    // Content shrink clamps the viewport without changing the native row width.
    for (Int32 id = 2050; id < 2097; ++id) {
        fixture.players->clientDropped(id);
    }
    fixture.refresh();
    CHECK(scroll.geometry().horizontalEnabled && !scroll.geometry().verticalEnabled);
    CHECK(list.fields().position.y == 0);
}

void scoreSelection() {
    Fixture fixture;
    std::vector<std::vector<std::string>> requests;
    fixture.app.console().addCommand(
        0, "remoteEval", [&](Console&, Int32, std::span<const std::string> args) {
            requests.emplace_back(args.begin() + 1, args.end());
            return "True";
        });
    // A player row follows the team heading, team row and player heading.
    clickRow(fixture.canvas(), fixture.list, 3);
    CHECK(requests == std::vector<std::vector<std::string>>{{"2048", "SelectClient", "2049"}});
    CHECK(fixture.list.lines()[fixture.list.selected.y].id == 2049);
    CHECK(fixture.canvas().focus() != &fixture.list);
    const auto row = fixture.list.selected.y;
    fixture.refresh();
    const auto sample = fixture.list.localToGlobal({10, row * fixture.list.cellSize.y + 10});
    CHECK(fixture.surface->bitmap().row(sample.y)[sample.x] == std::byte(game::GREEN_78));

    fixture.players->setClientScore(2055, "%n\t99", 999);
    fixture.refresh();
    CHECK(fixture.list.selected.y != row);
    CHECK(fixture.list.lines()[fixture.list.selected.y].id == 2049);
    CHECK(requests.size() == 1);
    for (Int32 headingOrTeam = 0; headingOrTeam < 3; ++headingOrTeam) {
        clickRow(fixture.canvas(), fixture.list, headingOrTeam);
    }
    CHECK(requests.size() == 1);
    CHECK(fixture.list.lines()[fixture.list.selected.y].id == 2049);

    // Clicking while horizontally scrolled must keep that column in view.
    CHECK(fixture.scroll.scrollTo(1, 0));
    const auto position = fixture.list.fields().position;
    clickRow(fixture.canvas(), fixture.list, fixture.list.selected.y);
    CHECK(fixture.list.fields().position == position);
    CHECK(requests.size() == 2 && requests.back().back() == "2049");
    fixture.players->clientDropped(2049);
    fixture.refresh();
    CHECK(fixture.list.selected == Point2I(-1, -1));
}

void scoreSelectionRequest() {
    game::FearGameApp app;
    CHECK(app.init());
    auto& plugin = app.fearPlugin();
    CHECK(plugin.createServerWorld({}, true));
    CHECK(app.console().execute("localConnect").value == "True");
    auto* server = plugin.serverGlobals().playerManager;
    CHECK(server->clientAdded("Peer", "male1", "base", true, 2050) == 2050);
    server->setClientScoreHeading("Player");
    (void)plugin.syncLoopbackState();
    CHECK(app.console()
              .evaluate(R"(
        function remoteSelectClient(%sender, %target) {
            $ScoreTest::Sender = %sender;
            $ScoreTest::Target = %target;
            remoteEval(%sender, "ScoreOptions", %target);
        }
        function remoteScoreOptions(%server, %target) {
            $ScoreTest::OptionTarget = %target;
        }
    )")
              .ok());
    auto root = std::make_unique<gui::Control>();
    game::ScoreListCtrl list;
    CHECK(root->resize({}, {640, 480}) && root->addObject(list));
    CHECK(app.guiRuntime()->show(gui::Graph::create(std::move(root))));
    auto surface = gfx::SoftwareSurface::create(640, 480);
    CHECK(surface && app.renderFrame(*surface));
    const auto target =
        std::ranges::find(list.lines(), 2050, &game::ScoreListCtrl::FormattedScoreLine::id);
    CHECK(target != list.lines().end());
    clickRow(app.guiRuntime()->canvas, list, Int32(target - list.lines().begin()));
    // Keep the real remoteEval implementation: the UI request must reach the
    // server with the viewer's ID, then return the selected target to the client.
    app.game().withWorld(sim::WorldType::Server, [&] {
        CHECK(app.console().getVariable("ScoreTest::Sender") == "2049");
        CHECK(app.console().getVariable("ScoreTest::Target") == "2050");
    });
    app.game().withWorld(sim::WorldType::Client, [&] {
        CHECK(app.console().getVariable("ScoreTest::OptionTarget") == "2050");
    });
}

void installedScoreInteraction() {
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
        auto* players = app.fearPlugin().clientGlobals().playerManager;
        CHECK(players);
        populate(*players);
        std::vector<std::string> request;
        app.console().addCommand(
            0, "remoteEval", [&](Console&, Int32, std::span<const std::string> args) {
                request.assign(args.begin() + 1, args.end());
                return "True";
            });
        CHECK(runtime->open("gui/score.gui"));
        auto* list = find<game::ScoreListCtrl>(*runtime->canvas.top());
        CHECK(list && list->parent());
        auto* scroll = dynamic_cast<gui::Scroll*>(list->parent()->parent());
        CHECK(scroll);
        auto surface = gfx::SoftwareSurface::create(640, 480);
        CHECK(surface && app.renderFrame(*surface) && scroll->computeSizes());
        CHECK(list->cellSize.x == 640);
        CHECK(list->shellFont && list->shellHighlightFont);
        CHECK(list->cellSize.y == list->shellFont->info.height + 2);
        CHECK(scroll->geometry().horizontalEnabled && scroll->geometry().verticalEnabled);
        click(runtime->canvas, scroll->localToGlobal(center(scroll->geometry().right)));
        click(runtime->canvas, scroll->localToGlobal(center(scroll->geometry().down)));
        CHECK(list->fields().position.x < 0 && list->fields().position.y < 0);
        const auto position = list->fields().position;
        clickRow(runtime->canvas, *list, 3);
        CHECK(request == std::vector<std::string>({"2048", "SelectClient", "2049"}));
        CHECK(list->fields().position == position);
        CHECK(app.renderFrame(*surface));
        CHECK(list->fields().position == position);
        std::printf(
            "%s score.gui: native 640px rows, both scrollbars and SelectClient click passed\n",
            rpg ? "RPG" : "base");
    }
}
}

int opent1_test_score_scrolling() {
    return opent1::test::run(scoreScrolling);
}

int opent1_test_score_selection() {
    return opent1::test::run(scoreSelection);
}

int opent1_test_score_selection_request() {
    return opent1::test::run(scoreSelectionRequest);
}

int opent1_test_installed_score_interaction() {
    return opent1::test::run(installedScoreInteraction);
}

OPENT1_REGISTER_TEST("game", "score_menu_scrolling", opent1_test_score_scrolling);
OPENT1_REGISTER_TEST("game", "score_menu_selection", opent1_test_score_selection);
OPENT1_REGISTER_TEST("game", "score_menu_selection_request", opent1_test_score_selection_request);
OPENT1_REGISTER_TEST("scores_assets",
                     "installed_score_interaction",
                     opent1_test_installed_score_interaction);
