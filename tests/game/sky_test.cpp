#include "test_registry.hpp"
#include "test_support.hpp"
#include <opent1/game/game_app.hpp>
#include <opent1/game/sky.hpp>

namespace {
using namespace opent1;
using namespace opent1::game;

void skyMaterialNames() {
    FearGameApp app;
    CHECK(app.init());
    for (const auto& [name, bitmap] :
         {std::pair{"lushsky_night.dml", "night.bmp"},
          std::pair{"lushsky_night.day.dml", "day.bmp"}}) {
        auto list = std::make_unique<world::TsMaterialList>();
        list->details = list->materialsPerDetail = 1;
        world::TsMaterial material;
        material.flags = 3;
        material.mapFile = bitmap;
        list->materials.push_back(material);
        CHECK(app.resources().add(name, std::move(list)));
        CHECK(app.resources().add(bitmap, gfx::Bitmap::create(4, 4)));
    }
    auto& plugin = app.fearPlugin();
    CHECK(plugin.createServerWorld({}, true));
    CHECK(plugin.focusServer());
    CHECK(app.console()
              .evaluate(R"(
                  instant Sky "MissionSky" { dmlName = "lushsky_night.dml"; };
              )")
              .ok());
    auto* server = dynamic_cast<Sky*>(plugin.serverGlobals().manager->findObject(TribesSkyId));
    CHECK(server && server->dmlName() == "lushsky_night.dml");
    CHECK(app.console().execute("localConnect").value == "True");

    const auto checkSky = [&](const Sky* sky, std::string_view name, std::string_view bitmap) {
        CHECK(sky && sky->dmlName() == name);
        const auto* materials = sky->materials();
        CHECK(materials && materials->textures().size() == 1);
        CHECK(materials->textures()[0]);
        CHECK(materials->textures()[0].get() == app.resources().load<gfx::Bitmap>(bitmap).get());
        CHECK(materials->list().materials[0].mapFile == bitmap);
    };
    for (unsigned frame = 0; frame < 8; ++frame)
        CHECK(app.stepFrame(frame * .032, false));
    const auto* client = dynamic_cast<Sky*>(plugin.findGhostForServerObject(server->getId()));
    checkSky(server, "lushsky_night.dml", "night.bmp");
    checkSky(client, "lushsky_night.dml", "night.bmp");

    // These names share their first 15 characters. A changed suffix must replace
    // the cached materials on both the server and the replicated client sky.
    server->setDmlName("lushsky_night.day.dml");
    for (unsigned frame = 8; frame < 16; ++frame)
        CHECK(app.stepFrame(frame * .032, false));
    checkSky(server, "lushsky_night.day.dml", "day.bmp");
    checkSky(client, "lushsky_night.day.dml", "day.bmp");
}
}

int opent1_test_sky_material_names() {
    return opent1::test::run(skyMaterialNames);
}

OPENT1_REGISTER_TEST("game", "sky_material_names", opent1_test_sky_material_names);
