#include "test_support.hpp"
#include "test_registry.hpp"
#include "ts_test_support.hpp"
#include <opent1/game/game_app.hpp>
#include <opent1/game/shell_controls.hpp>
#include <opent1/gui/runtime.hpp>
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iterator>

namespace {
using namespace opent1;

void previewArmor() {
    game::SkinView view;
    CHECK(view.currentArmor() == game::SkinView::Light);
    CHECK(view.shapeName() == "larmor.dts");
    for (const bool male : {true, false}) {
        view.setSkin("storm", male);
        CHECK(view.resolvedShapeFilename() == (male ? "larmor.dts" : "lfemale.dts"));
        view.cycleArmor();
        CHECK(view.resolvedShapeFilename() == (male ? "marmor.dts" : "mfemale.dts"));
        view.cycleArmor();
        CHECK(view.resolvedShapeFilename() == "harmor.dts");
        CHECK(view.skinName() == "storm");
        view.cycleArmor();
    }
}

void previewMaterials() {
    auto resources = std::make_shared<ResourceManager>();
    for (const auto name : {"larmor.dts", "marmor.dts", "harmor.dts", "lfemale.dts", "mfemale.dts"}) {
        auto shape = test::ts::load(test::ts::shape());
        CHECK(shape && shape->materials);
        // The native preview replaces even maps without gameplay's base.* prefix.
        shape->materials->materials[0].mapFile = "authored.bmp";
        auto list = std::make_unique<world::TsMaterialList>(*shape->materials);
        const auto base = std::string_view(name).substr(0, std::string_view(name).find('.'));
        CHECK(resources->add(std::format("listed.{}.dml", base), std::move(list)));
        CHECK(resources->add(name, std::move(shape)));
    }
    for (const auto [name, color] :
         {std::pair{"authored.bmp", 20}, {"base.larmor.bmp", 40}, {"storm.larmor.bmp", 80},
          {"base.marmor.bmp", 40}, {"storm.marmor.bmp", 80},
          {"base.harmor.bmp", 40}, {"storm.harmor.bmp", 80},
          {"base.lfemale.bmp", 40}, {"storm.lfemale.bmp", 80},
          {"base.mfemale.bmp", 40}, {"storm.mfemale.bmp", 80}}) {
        auto bitmap = gfx::Bitmap::create(4, 4);
        CHECK(bitmap);
        std::ranges::fill(bitmap->pixels, std::byte(color));
        CHECK(resources->add(name, std::move(bitmap)));
    }
    game::SkinView view;
    view.bindResources(resources);
    CHECK(view.resize({}, {160, 200}));
    view.setRotation({0, 0, 0});
    const auto render = [&] {
        auto surface = gfx::SoftwareSurface::create(160, 200);
        CHECK(surface);
        surface->clear(0);
        CHECK(view.draw(*surface, {}, {0, 0, 159, 199}));
        const auto pixels = surface->bitmap().pixels;
        CHECK(std::ranges::any_of(pixels, [](auto pixel) { return pixel != std::byte{}; }));
        return pixels;
    };
    for (const bool male : {true, false}) {
        for (int armor = 0; armor < 3; ++armor) {
            view.setSkin("base", male);
            const auto base = render();
            view.setSkin("storm", male);
            const auto storm = render();
            CHECK(storm != base);
            view.setSkin("missing", male);
            CHECK(render() == base);
            // An explicit DML has precedence over generated preview materials.
            view.setSkin("listed", male);
            const auto listed = render();
            CHECK(listed != base && listed != storm);
            CHECK(resources->load<world::TsShape>(view.resolvedShapeFilename())
                      ->materials->materials[0].mapFile == "authored.bmp");
            view.cycleArmor();
        }
    }
}

void installedProfileSkins() {
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
        game::FearGameApp::Config config;
        config.assetRoot = assets;
        config.writeRoot = std::filesystem::temp_directory_path() /
                           std::format("opent1-profile-skin-{}",
                                       std::chrono::steady_clock::now().time_since_epoch().count());
        struct Cleanup {
            std::filesystem::path path;
            ~Cleanup() {
                std::error_code ec;
                std::filesystem::remove_all(path, ec);
            }
        } cleanup{config.writeRoot};
        game::FearGameApp app(config);
        CHECK(rpg ? app.init(std::array<std::string_view, 2>{"-mod", "RPG"}) : app.init());
        auto runtime = app.guiRuntime();
        auto& console = app.console();
        CHECK(console.execute("startMainMenuScreen").ok());
        CHECK(console
                  .evaluate(R"(
            $PCFG::Name[0] = "SkinTest";
            $PCFG::Gender[0] = "MALE";
            $PCFG::SkinBase[0] = "base";
            $PCFG::Name[1] = "";
            $PCFG::CurrentPlayer = 0;
        )")
                  .ok());
        CHECK(runtime && runtime->open("gui/PlayerSetup.gui"));
        auto* combo = dynamic_cast<game::FGComboBox*>(runtime->findControl("140311"));
        auto* skin = dynamic_cast<game::SkinView*>(runtime->findControl("140003"));
        CHECK(combo && skin);
        CHECK(skin->resolvedShapeFilename() == "larmor.dts");
        const auto render = [&] {
            const auto extent = skin->fields().extent;
            auto surface = gfx::SoftwareSurface::create(extent.x, extent.y);
            CHECK(surface && surface->setPalette(*app.defaultPalette()));
            surface->clear(0);
            CHECK(skin->draw(*surface, {}, {0, 0, extent.x - 1, extent.y - 1}));
            const auto pixels = surface->bitmap().pixels;
            CHECK(std::ranges::any_of(pixels, [](auto pixel) { return pixel != std::byte{}; }));
            return pixels;
        };
        const auto select = [&](std::string_view name) {
            CHECK(combo->open());
            auto& list = combo->list();
            const auto at = std::ranges::find_if(list.entries, [&](const auto& entry) {
                return entry.text == name;
            });
            CHECK(at != list.entries.end());
            const auto row = Int32(at - list.entries.begin());
            // Scroll the requested row into view before actual mouse dispatch.
            const auto& geometry = combo->popupScroll().geometry();
            const auto range = list.fields().extent.y - geometry.viewport.y;
            const auto position = range > 0
                                      ? std::min(1.f, Flt32(row * list.cellSize.y) / range) : 0.f;
            CHECK(combo->popupScroll().scrollTo(0, position));
            gui::Event click;
            click.type = gui::EventType::Down;
            click.button = 1;
            click.point = list.localToGlobal({10, row * list.cellSize.y + 2});
            CHECK(runtime->canvas.dispatch(click));
            CHECK(!combo->isOpen());
            CHECK(combo->getSelectedEntry() == combo->findEntry(name));
            CHECK(console.getVariable("PCFG::SkinBase") == name);
            CHECK(skin->skinName() == name);
        };
        auto previous = render();
        const auto names = rpg ? std::array{"robeblack", "robeblue"}
                               : std::array{"beagle", "cphoenix"};
        for (const auto name : names) {
            if (rpg) {
                CHECK(app.resources().contains(std::string(name) + ".larmor.bmp"));
                CHECK(!app.resources().contains(std::string(name) + ".marmor.bmp"));
            }
            select(name);
            const auto next = render();
            CHECK(next != previous);
            previous = next;
        }
        CHECK(runtime->open("gui/MainMenu.gui"));
        CHECK(console.getVariable("PCFG::SkinBase0") == names.back());
        CHECK(std::filesystem::exists(config.writeRoot / "config/Players.cs"));
        // Reload the exported script, then reopen the screen from that saved state.
        std::ifstream savedConfig(config.writeRoot / "config/Players.cs");
        const std::string saved{std::istreambuf_iterator<char>(savedConfig),
                                std::istreambuf_iterator<char>()};
        CHECK(!saved.empty());
        console.setVariable("PCFG::SkinBase0", "base");
        CHECK(console.evaluate(saved).ok());
        CHECK(console.getVariable("PCFG::SkinBase0") == names.back());
        CHECK(runtime->open("gui/PlayerSetup.gui"));
        combo = dynamic_cast<game::FGComboBox*>(runtime->findControl("140311"));
        skin = dynamic_cast<game::SkinView*>(runtime->findControl("140003"));
        CHECK(combo && skin && combo->text == names.back());
        CHECK(skin->resolvedShapeFilename() == "larmor.dts");
        CHECK(render() == previous);
        CHECK(console.evaluate("SwitchGender(\"FEMALE\");").ok());
        CHECK(!skin->isMale() && skin->resolvedShapeFilename() == "lfemale.dts");
        CHECK(skin->skinName() == console.getVariable("PCFG::SkinBase"));
        CHECK(render() != previous);
        auto surface = gfx::SoftwareSurface::create(640, 480);
        CHECK(surface && app.renderFrame(*surface));
        std::printf("%s profile: skin pixels change, survive reopening, and switch gender\n",
                    rpg ? "RPG" : "base");
    }
}
}

int opent1_test_profile_preview_armor() {
    return test::run(previewArmor);
}

int opent1_test_profile_preview_materials() {
    return test::run(previewMaterials);
}

int opent1_test_installed_profile_skins() {
    return test::run(installedProfileSkins);
}

OPENT1_REGISTER_TEST("game", "profile_preview_armor", opent1_test_profile_preview_armor);
OPENT1_REGISTER_TEST("game", "profile_preview_materials", opent1_test_profile_preview_materials);
OPENT1_REGISTER_TEST("profile_skins", "installed_profile_skins", opent1_test_installed_profile_skins);
