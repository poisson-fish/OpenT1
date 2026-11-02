// Ordinary optimized executable: intentionally no allocation-tracking overrides.
#include <opent1/game/game_app.hpp>
#include <opent1/game/observer_camera.hpp>
#include <opent1/gfx/sdl_gpu_surface.hpp>
#include <algorithm>
#include <chrono>
#include <fstream>
#include <iostream>
#include <stdexcept>

using namespace opent1;
namespace {
using Clock = std::chrono::steady_clock;
double ms(Clock::time_point start) {
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}
void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
void distribution(std::ostream& out, std::string_view name, std::vector<double> values) {
    double sum{};
    for (auto v : values) sum += v;
    std::ranges::sort(values);
    const auto percentile = [&](double p) { return values[std::size_t(p * (values.size() - 1))]; };
    out << '"' << name << "\":{\"mean\":" << sum / values.size()
        << ",\"p95\":" << percentile(.95) << ",\"p99\":" << percentile(.99) << '}';
}
}
int main(int argc, char** argv) {
    try {
        require(argc >= 4, "Usage: render_benchmark ASSETS MISSION OUTPUT [stationary|rotate|travel] [FRAMES] [DRIVER] [RPG]");
        const std::filesystem::path output = std::filesystem::absolute(argv[3]);
        std::filesystem::create_directories(output.parent_path());
        const std::string mode = argc > 4 ? argv[4] : "stationary";
        require(mode == "stationary" || mode == "rotate" || mode == "travel", "invalid camera mode");
        const unsigned count = argc > 5 ? unsigned(std::stoul(argv[5])) : 240;
        require(count > 0 && count <= 100000, "invalid frame count");
        game::FearGameApp::Config config;
        config.assetRoot = argv[1];
        config.writeRoot = output.string() + "-runtime";
        game::FearGameApp game(config);
        const std::array<std::string_view, 2> rpg{"-mod", "RPG"};
        require(game.init(argc > 7 ? std::span<const std::string_view>(rpg) : std::span<const std::string_view>{}), "init failed");
        game.console().setVariable("Server::Port", "0");
        game.console().setVariable("Server::Heartbeats", "false");
        game.console().setVariable("Server::warmupTime", "0");
        game.console().setVariable("PCFG::Name", "RenderBenchmark");
        game.console().setVariable("PCFG::Script", "");
        const auto load = Clock::now();
        require(game.loadMission(argv[2]), "mission load failed");
        const auto loadMs = ms(load);
        for (unsigned i = 0; i < 8; ++i) require(game.stepFrame((i + 1) * .032, false), "warmup failed");
        auto& cg = game.fearPlugin().clientGlobals();
        auto& sg = game.fearPlugin().serverGlobals();
        auto* camera = cg.playerPSC ? dynamic_cast<game::ObserverCamera*>(cg.playerPSC->getControlObject()) : nullptr;
        auto* server = sg.playerPSC ? dynamic_cast<game::ObserverCamera*>(sg.playerPSC->getControlObject()) : nullptr;
        require(camera && server, "benchmark requires local observer cameras");
        const auto origin = camera->getEyeTransform().p;
        const auto rotation = camera->rotation();
        platform::SdlApplication application;
        require(application.initialize({}) == SDL_APP_CONTINUE, "SDL init failed");
        platform::WindowConfig wc;
        wc.hidden = true;
        wc.width = 800; wc.height = 600;
        auto& window = application.createWindow(wc);
        gfx::GpuSurfaceConfig gc;
        if (argc > 6) gc.driver = argv[6];
        auto surface = gfx::SdlGpuSurface::create(window, 800, 600, gc);
        require(bool(surface), SDL_GetError());
        std::vector<double> frames, prepares, captures, submissions, simulations;
        UInt64 polygons{}, draws{}, binds{};
        // Warm the rotating working set as well as pipeline creation.
        constexpr unsigned warm = 120;
        for (unsigned f = 0; f < warm + count; ++f) {
            const auto start = Clock::now();
            require(game.stepFrame((f + 9) * .032, false), "simulation failed");
            auto pos = origin;
            auto rot = rotation;
            if (mode != "stationary") rot.z += Flt32(f % 120) * 6.2831853f / 120;
            if (mode == "travel") {
                pos.x += 64 * std::sin(Flt32(f) * .025f);
                pos.y += 64 * (1 - std::cos(Flt32(f) * .025f));
            }
            camera->setFlyMode(false, true, pos, rot);
            server->setFlyMode(false, true, pos, rot);
            const auto simMs = ms(start);
            gfx::HardwareBatch batch;
            const auto prepare = Clock::now();
            require(game.renderFrame(surface->software(), &batch), "render preparation failed");
            const auto prepareMs = ms(prepare);
            gfx::GpuFrameConfig frame;
            frame.hazeColor = game.hardwareHazeColor();
            const auto submit = Clock::now();
            require(surface->render(batch, frame), SDL_GetError());
            const auto submitMs = ms(submit);
            if (f >= warm) {
                frames.push_back(ms(start)); prepares.push_back(prepareMs);
                captures.push_back(game.renderStats().captureMs);
                submissions.push_back(submitMs); simulations.push_back(simMs);
                polygons += game.lastScenePolygonCount();
                draws += surface->submissionStats().draws;
                binds += surface->submissionStats().samplerBinds;
            }
        }
        require(polygons > 0, "no world rendered");
        std::vector<std::byte> pixels(800 * 600 * 4);
        require(surface->readback(pixels), "readback failed");
        std::ofstream ppm(output.string() + ".ppm", std::ios::binary);
        ppm << "P6\n800 600\n255\n";
        for (std::size_t i = 0; i < pixels.size(); i += 4) ppm.write(reinterpret_cast<const char*>(pixels.data() + i), 3);
        const auto textures = surface->textureCacheStats();
        const auto resources = surface->resourceStats();
        std::ofstream out(output);
        out << "{\"mission\":\"" << argv[2] << "\",\"mode\":\"" << mode
            << "\",\"driver\":\"" << surface->driver() << "\",\"frames\":" << count
            << ",\"load_ms\":" << loadMs << ',';
        distribution(out, "frame_ms", frames); out << ',';
        distribution(out, "prepare_ms", prepares); out << ',';
        distribution(out, "capture_ms", captures); out << ',';
        distribution(out, "submit_ms", submissions); out << ',';
        distribution(out, "simulation_ms", simulations);
        out << ",\"mean_polygons\":" << double(polygons) / count
            << ",\"mean_draws\":" << double(draws) / count << ",\"mean_binds\":" << double(binds) / count
            << ",\"textures\":" << textures.textures << ",\"texture_bytes\":" << textures.bytes
            << ",\"texture_uploads\":" << textures.uploads << ",\"texture_evictions\":" << textures.evictions
            << ",\"vertex_capacity\":" << resources.vertexCapacity << ",\"staging_capacity\":" << resources.uploadCapacity << "}\n";
        game.shutdown();
        std::cout << "Saved " << output.string() << '\n';
        return 0;
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
