#include "test_registry.hpp"
#include "test_support.hpp"
#include <opent1/game/game_app.hpp>
#include <opent1/game/fear_cs_delegate.hpp>
#include <opent1/game/fear_player_psc.hpp>
#include <opent1/game/observer_camera.hpp>
#include <opent1/game/player.hpp>
#include <opent1/gfx/sdl_gpu_surface.hpp>
#include <cmath>

namespace {
using namespace opent1;
using namespace opent1::game;

class FlashHud final : public gui::Control {
public:
    FlashHud() {
        assignName("playGui");
    }

    bool draw(gfx::SoftwareSurface& surface, Point2I offset, RectI) const override {
        surface.fill({offset.x + 4, offset.y + 4, offset.x + 11, offset.y + 11}, 255);
        return true;
    }
};

struct FlashSession {
    FearGameApp server;
    std::unique_ptr<FearGameApp> remote;
    Player* player{};
    unsigned frame{};

    explicit FlashSession(bool network = false) {
        CHECK(server.init());
        CHECK(server.fearPlugin().createServerWorld({}, true));
        CHECK(server.console().evaluate("PlayerData FlashArmor { mass = 9; };").ok());
        if (network) {
            remote = std::make_unique<FearGameApp>();
            CHECK(remote->init());
            const auto address = net::parseIPv4Address(
                server.fearPlugin().serverGlobals().csDelegate->delegate()->localAddress());
            CHECK(address);
            CHECK(
                remote->console()
                    .execute("connect", std::array{"IP:127.0.0.1:" + std::to_string(address->port)})
                    .value == "True");
            advance(64);
        } else {
            CHECK(server.console().execute("localConnect").value == "True");
        }
        auto& app = client();
        const auto id = Int32(app.fearPlugin().clientGlobals().manager->getId());
        const auto object = server.fearPlugin().spawnPlayer("FlashArmor", {0, 0, 100}, {}, id);
        CHECK(object);
        player =
            dynamic_cast<Player*>(server.fearPlugin().serverGlobals().manager->findObject(*object));
        CHECK(player);
        advance(64);
        CHECK(app.fearPlugin().clientGlobals().playerPSC->getControlObject());
        auto surface = gfx::SoftwareSurface::create(320, 240);
        CHECK(surface && app.renderFrame(*surface));
        // Exact red and green ramps make software quantization deterministic.
        auto& palette = *app.defaultPalette();
        palette.allowedMatches.fill(0xffffffffU);
        for (unsigned i = 0; i < 128; ++i) {
            palette.palettes[0].colors[i] = {UInt8(i * 2), 0, 0, 0};
            palette.palettes[0].colors[128 + i] = {0, UInt8(i * 2), 0, 0};
        }
        CHECK(app.guiRuntime()->show(gui::Graph::create(std::make_unique<FlashHud>())));
    }

    FearGameApp& client() {
        return remote ? *remote : server;
    }

    void advance(unsigned count) {
        for (unsigned i = 0; i < count; ++i, ++frame) {
            CHECK(server.stepFrame(frame * .032, false));
            if (remote) {
                CHECK(remote->stepFrame(frame * .032, false));
            }
        }
    }

    Flt32 flash(Flt32 value) {
        if (remote) {
            // Keep the source steady while real UDP packets pass through both
            // apps. Assertions use the received quantized value, not the input.
            for (unsigned i = 0; i < 16; ++i) {
                player->setDamageFlash(value);
                advance(1);
            }
        } else {
            player->setDamageFlash(value);
            (void)server.fearPlugin().syncLoopbackState(false);
        }
        return client().fearPlugin().clientGlobals().playerPSC->damageFlash();
    }
};

void damageFlashFrames() {
    for (bool remote : {false, true}) {
        FlashSession session(remote);
        auto& app = session.client();
        auto surface = gfx::SoftwareSurface::create(320, 240);
        CHECK(surface);
        for (const auto* scale : {"1", "2"}) {
            app.console().setVariable("pref::UI::scale", scale);
            const auto pixel = [&](Int32 x, Int32 y) {
                return app.defaultPalette()->find().colors[UInt8(surface->bitmap().row(y)[x])];
            };
            CHECK(session.flash(0) == 0 && app.renderFrame(*surface));
            const auto baseline = surface->bitmap().pixels;
            const auto hud = pixel(8, 8);
            CHECK(hud.green == 254 && hud.red == 0);
            const auto received = session.flash(.5f);
            CHECK(received > .4f && received < .6f && app.renderFrame(*surface));
            CHECK(std::abs(int(pixel(160, 120).red) - int(received * 255)) <= 2);
            CHECK(pixel(8, 8) == hud);

            // A changed ghost value must not override the authoritative PSC tint.
            auto* ghost = dynamic_cast<Player*>(
                app.fearPlugin().clientGlobals().playerPSC->getControlObject());
            CHECK(ghost);
            ghost->setDamageFlash(0);
            CHECK(app.renderFrame(*surface) && pixel(160, 120).red > 100);

            CHECK(session.flash(1) > .9f && app.renderFrame(*surface));
            CHECK(std::abs(int(pixel(160, 120).red) - 194) <= 2);
            CHECK(pixel(8, 8) == hud);
            const auto partial = session.flash(.25f);
            CHECK(partial > .15f && partial < .3f && app.renderFrame(*surface));
            CHECK(std::abs(int(pixel(160, 120).red) - int(partial * 255)) <= 2);
            CHECK(session.flash(0) == 0 && app.renderFrame(*surface));
            CHECK(surface->bitmap().pixels == baseline);
        }

        // The existing simulation fade reaches the same untinted frame.
        (void)session.flash(.1f);
        session.advance(96);
        CHECK(app.fearPlugin().clientGlobals().playerPSC->damageFlash() == 0);
        CHECK(app.renderFrame(*surface));
        CHECK(surface->bitmap().row(120)[160] == std::byte{0});

        // Orbit cameras receive their subject's flash through the same PSC.
        auto& sg = session.server.fearPlugin().serverGlobals();
        const auto id = Int32(app.fearPlugin().clientGlobals().manager->getId());
        auto camera = std::make_unique<ObserverCamera>(id);
        auto* observer = camera.get();
        observer->setOrbitMode(session.player, {}, {}, 3, 3, 3);
        CHECK(sg.manager->addObject(std::move(camera)));
        CHECK(session.server.console()
                  .execute("Client::setControlObject",
                           std::array{std::to_string(id), std::to_string(observer->getId())})
                  .value == "True");
        session.advance(32);
        session.player->setDamageFlash(.5f);
        session.advance(8);
        CHECK(app.fearPlugin().clientGlobals().playerPSC->damageFlash() > .3f);
        CHECK(app.renderFrame(*surface));
        CHECK(surface->bitmap().row(120)[160] != std::byte{0});
        observer->setFlyMode(false, false, {}, {});
        session.advance(32);
        CHECK(app.fearPlugin().clientGlobals().playerPSC->damageFlash() == 0);
        CHECK(app.renderFrame(*surface));
        CHECK(surface->bitmap().row(120)[160] == std::byte{0});
    }
}

void damageFlashGpuFrames() {
    platform::SdlApplication sdl;
    CHECK(sdl.initialize({}) == SDL_APP_CONTINUE);
    platform::WindowConfig config;
    config.hidden = true;
    config.width = 320;
    config.height = 240;
    auto& window = sdl.createWindow(config);
    auto surface = gfx::SdlGpuSurface::create(window, 320, 240);
    CHECK(surface);
    for (bool remote : {false, true}) {
        FlashSession session(remote);
        auto& app = session.client();
        std::vector<std::byte> pixels(320 * 240 * 4);
        const auto render = [&] {
            gfx::HardwareBatch batch;
            CHECK(app.renderFrame(surface->software(), &batch));
            CHECK(surface->render(batch) && surface->readback(pixels));
        };
        const auto channel = [&](int x, int y, int c) {
            return int(pixels[(y * 320 + x) * 4 + c]);
        };
        for (const auto* scale : {"1", "2"}) {
            app.console().setVariable("pref::UI::scale", scale);
            CHECK(session.flash(0) == 0);
            render();
            const auto baseline = pixels;
            for (const auto value : {.5f, 1.f, .25f}) {
                const auto received = session.flash(value);
                render();
                CHECK(std::abs(channel(160, 120, 0) - int(std::min(received, .76f) * 255)) <= 1);
                CHECK(channel(160, 120, 1) == 0 && channel(160, 120, 2) == 0);
                CHECK(channel(8, 8, 0) == 0 && channel(8, 8, 1) == 254);
            }
            CHECK(session.flash(0) == 0);
            render();
            CHECK(pixels == baseline);
            (void)session.flash(.1f);
            session.advance(96);
            render();
            CHECK(pixels == baseline);
        }
    }
}

int damageFlash() {
    return test::run(damageFlashFrames);
}

int damageFlashGpu() {
    return test::run(damageFlashGpuFrames);
}
}

OPENT1_REGISTER_TEST("game", "damage_flash", damageFlash);
OPENT1_REGISTER_TEST("gpu", "damage_flash", damageFlashGpu);
