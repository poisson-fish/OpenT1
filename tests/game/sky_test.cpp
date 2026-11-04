#include "test_registry.hpp"
#include "test_support.hpp"
#include <opent1/game/game_app.hpp>
#include <opent1/game/planet.hpp>
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

void planetBitmaps() {
    FearGameApp app;
    CHECK(app.init());
    CHECK(app.fearPlugin().createClientWorld());
    auto& manager = *app.fearPlugin().clientGlobals().manager;
    auto sky = std::make_unique<Sky>();
    CHECK(manager.addObject(std::move(sky)));
    auto surface = gfx::SoftwareSurface::create(320, 240);
    CHECK(surface && app.renderFrame(*surface));
    const auto background = surface->bitmap().pixels;

    auto owned = std::make_unique<Planet>();
    owned->set("", 0, 20, true, false, {.4f, .4f, .4f}, {.16f, .16f, .16f}, 400, 2000);
    auto* planet = owned.get();
    CHECK(manager.addObject(std::move(owned)));
    CHECK(app.renderFrame(*surface));
    CHECK(surface->bitmap().pixels == background);
    sim::SimLightQuery lighting;
    CHECK(planet->processQuery(lighting));
    CHECK(lighting.lights.size() == 1 && lighting.lights[0].intensity.red == .4f);
    CHECK(lighting.ambient.red == .16f && Planet::shadows);

    planet->planetImage().setTexture("missing-planet.bmp");
    CHECK(app.renderFrame(*surface));
    CHECK(surface->bitmap().pixels == background);

    auto bitmap = gfx::Bitmap::create(4, 4);
    CHECK(bitmap);
    bitmap->attributes |= gfx::BitmapTransparent;
    bitmap->row(1)[1] = bitmap->row(2)[2] = std::byte{80};
    bitmap->row(1)[2] = bitmap->row(2)[1] = std::byte{160};
    CHECK(app.resources().add("planet.bmp", std::move(bitmap)));
    planet->planetImage().setTexture("planet.bmp");
    CHECK(app.renderFrame(*surface));
    const auto pixel = [&](Int32 x, Int32 y) { return surface->bitmap().row(y)[x]; };
    CHECK(pixel(150, 52) == std::byte{80} && pixel(170, 52) == std::byte{160});
    CHECK(pixel(150, 72) == std::byte{160} && pixel(170, 72) == std::byte{80});
    CHECK(pixel(130, 32) == background[32 * surface->bitmap().stride() + 130]);
    const auto textured = surface->bitmap().pixels;
    gfx::HardwareBatch hardware;
    CHECK(app.renderFrame(*surface, &hardware));
    CHECK(!hardware.draws().empty());

    // Clipping a partially visible bitmap must preserve its texture coordinates.
    planet->planetImage().position.x -= planet->planetImage().position.y;
    CHECK(app.renderFrame(*surface));
    CHECK(pixel(5, 52) == std::byte{160} && pixel(5, 72) == std::byte{80});
    CHECK(pixel(25, 52) == background[52 * surface->bitmap().stride() + 25]);
    planet->planetImage().setTexture("");
    CHECK(app.renderFrame(*surface));
    CHECK(surface->bitmap().pixels == background && surface->bitmap().pixels != textured);
}
}

int opent1_test_sky_material_names() {
    return opent1::test::run(skyMaterialNames);
}

OPENT1_REGISTER_TEST("game", "sky_material_names", opent1_test_sky_material_names);

int opent1_test_planet_bitmaps() {
    return opent1::test::run(planetBitmaps);
}

OPENT1_REGISTER_TEST("game", "planet_bitmaps", opent1_test_planet_bitmaps);
