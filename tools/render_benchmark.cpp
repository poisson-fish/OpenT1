// Ordinary optimized executable: intentionally no allocation-tracking overrides.
#include <opent1/game/game_app.hpp>
#include <ranges>
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
    if (!value)
        throw std::runtime_error(message);
}

void distribution(std::ostream& out, std::string_view name, std::vector<double> values) {
    double sum{};
    for (auto v : values)
        sum += v;
    std::ranges::sort(values);
    const auto percentile = [&](double p) {
        return values[std::size_t(p * (values.size() - 1))];
    };
    out << '"' << name << "\":{\"mean\":" << sum / values.size() << ",\"p95\":" << percentile(.95)
        << ",\"p99\":" << percentile(.99) << '}';
}
}

int main(int argc, char** argv) {
    try {
        require(argc >= 4,
                "Usage: render_benchmark ASSETS MISSION OUTPUT [stationary|rotate|travel] [FRAMES] "
                "[DRIVER] [RPG]");
        const std::filesystem::path output = std::filesystem::absolute(argv[3]);
        std::filesystem::create_directories(output.parent_path());
        const std::string mode = argc > 4 ? argv[4] : "stationary";
        require(mode == "stationary" || mode == "rotate" || mode == "travel",
                "invalid camera mode");
        const unsigned count = argc > 5 ? unsigned(std::stoul(argv[5])) : 240;
        require(count > 0 && count <= 100000, "invalid frame count");
        game::FearGameApp::Config config;
        config.assetRoot = argv[1];
        config.writeRoot = output.string() + "-runtime";
        game::FearGameApp game(config);
        const std::array<std::string_view, 2> rpg{"-mod", "RPG"};
        require(game.init(argc > 7 ? std::span<const std::string_view>(rpg)
                                   : std::span<const std::string_view>{}),
                "init failed");
        game.console().setVariable("Server::Port", "0");
        game.console().setVariable("Server::Heartbeats", "false");
        game.console().setVariable("Server::warmupTime", "0");
        game.console().setVariable("PCFG::Name", "RenderBenchmark");
        game.console().setVariable("PCFG::Script", "");
        const auto load = Clock::now();
        require(game.loadMission(argv[2]), "mission load failed");
        const auto loadMs = ms(load);
        for (auto i : std::views::iota(unsigned{0}, unsigned{8}))
            require(game.stepFrame((i + 1) * .032, false), "warmup failed");
        auto& cg = game.fearPlugin().clientGlobals();
        auto& sg = game.fearPlugin().serverGlobals();
        auto* camera = cg.playerPSC
                           ? dynamic_cast<game::ObserverCamera*>(cg.playerPSC->getControlObject())
                           : nullptr;
        auto* server = sg.playerPSC
                           ? dynamic_cast<game::ObserverCamera*>(sg.playerPSC->getControlObject())
                           : nullptr;
        require(camera && server, "benchmark requires local observer cameras");
        const auto origin = camera->getEyeTransform().p;
        const auto rotation = camera->rotation();
        platform::SdlApplication application;
        require(application.initialize({}) == SDL_APP_CONTINUE, "SDL init failed");
        platform::WindowConfig wc;
        wc.hidden = true;
        wc.width = 800;
        wc.height = 600;
        auto& window = application.createWindow(wc);
        gfx::GpuSurfaceConfig gc;
        if (argc > 6)
            gc.driver = argv[6];
        auto surface = gfx::SdlGpuSurface::create(window, 800, 600, gc);
        require(bool(surface), SDL_GetError());
        std::vector<double> frames, prepares, captures, submissions, simulations, terrain, lighting;
        std::vector<double> terrainVisibility, terrainSelect, terrainEdges, terrainEmit;
        gfx::RenderWorkStats terrainWork;
        UInt64 polygons{}, draws{}, binds{}, vertexBytes{}, meshBytes{}, textureBytes{},
            geometryBuilds{}, mapBuilds{}, terrainCacheHits{};
        UInt64 instanceBytes{};
        UInt64 inputDraws{}, pipelineBinds{}, vertexUniformPushes{}, fragmentUniformPushes{};
        gfx::HardwareBatch::MemoryStats batchMemory;
        // Warm the rotating working set as well as pipeline creation.
        constexpr unsigned warm = 120;
        for (auto f : std::views::iota(unsigned{0}) | std::views::take_while([&](unsigned f) {
                          return f < warm + count;
                      })) {
            const auto start = Clock::now();
            require(game.stepFrame((f + 9) * .032, false), "simulation failed");
            auto pos = origin;
            auto rot = rotation;
            if (mode != "stationary")
                rot.z += Flt32(f % 120) * 6.2831853f / 120;
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
                frames.push_back(ms(start));
                prepares.push_back(prepareMs);
                captures.push_back(game.renderStats().captureMs);
                submissions.push_back(submitMs);
                simulations.push_back(simMs);
                terrain.push_back(game.renderStats().work.terrainLodMs);
                const auto& work = game.renderStats().work;
                terrainVisibility.push_back(work.terrainVisibilityMs);
                terrainSelect.push_back(work.terrainSelectMs);
                terrainEdges.push_back(work.terrainEdgesMs);
                terrainEmit.push_back(work.terrainEmitMs);
                terrainWork.terrainNodes += work.terrainNodes;
                terrainWork.terrainValidatedHeights += work.terrainValidatedHeights;
                terrainWork.terrainMaterialChecks += work.terrainMaterialChecks;
                terrainWork.terrainPointRequests += work.terrainPointRequests;
                terrainWork.terrainPoints += work.terrainPoints;
                terrainWork.terrainEdgePoints += work.terrainEdgePoints;
                terrainWork.terrainScratchAllocations += work.terrainScratchAllocations;
                terrainWork.terrainScratchBytes += work.terrainScratchBytes;
                terrainWork.terrainScratchBufferBytes += work.terrainScratchBufferBytes;
                lighting.push_back(game.renderStats().work.lightSamplesMs);
                polygons += game.lastScenePolygonCount();
                draws += surface->submissionStats().draws;
                binds += surface->submissionStats().samplerBinds;
                inputDraws += surface->submissionStats().inputDraws;
                pipelineBinds += surface->submissionStats().pipelineBinds;
                vertexUniformPushes += surface->submissionStats().vertexUniformPushes;
                fragmentUniformPushes += surface->submissionStats().fragmentUniformPushes;
                vertexBytes += surface->submissionStats().vertexUploadBytes;
                meshBytes += surface->submissionStats().meshUploadBytes;
                textureBytes += surface->submissionStats().textureUploadBytes;
                geometryBuilds += game.renderStats().work.interiorGeometryBuilds;
                mapBuilds += game.renderStats().work.interiorLightMapBuilds;
                terrainCacheHits += game.renderStats().work.terrainMeshCacheHits;
                instanceBytes += surface->submissionStats().instanceUploadBytes;
            }
            if (f + 1 == warm + count)
                batchMemory = batch.memoryStats();
        }
        require(polygons > 0, "no world rendered");
        std::vector<std::byte> pixels(800 * 600 * 4);
        require(surface->readback(pixels), "readback failed");
        std::ofstream ppm(output.string() + ".ppm", std::ios::binary);
        ppm << "P6\n800 600\n255\n";
        for (std::size_t i = 0; i < pixels.size(); i += 4)
            ppm.write(reinterpret_cast<const char*>(pixels.data() + i), 3);
        const auto textures = surface->textureCacheStats();
        const auto resources = surface->resourceStats();
        const auto archives = game.resources().archiveMemoryStats();
        const auto ts = world::tsHardwareCacheStats();
        const auto sources = game.resources().residencyStats();
        const auto conversions = gfx::hardwareTextureConversionStats();
        std::ofstream out(output);
        out << "{\"mission\":\"" << argv[2] << "\",\"mode\":\"" << mode << "\",\"driver\":\""
            << surface->driver() << "\",\"frames\":" << count << ",\"load_ms\":" << loadMs << ',';
        distribution(out, "frame_ms", frames);
        out << ',';
        distribution(out, "prepare_ms", prepares);
        out << ',';
        distribution(out, "capture_ms", captures);
        out << ',';
        distribution(out, "submit_ms", submissions);
        out << ',';
        distribution(out, "simulation_ms", simulations);
        out << ',';
        distribution(out, "terrain_lod_ms", terrain);
        out << ',';
        distribution(out, "terrain_visibility_ms", terrainVisibility);
        out << ',';
        distribution(out, "terrain_select_ms", terrainSelect);
        out << ',';
        distribution(out, "terrain_edges_ms", terrainEdges);
        out << ',';
        distribution(out, "terrain_emit_ms", terrainEmit);
        out << ",\"mean_terrain_nodes\":" << double(terrainWork.terrainNodes) / count
            << ",\"mean_terrain_validated_heights\":"
            << double(terrainWork.terrainValidatedHeights) / count
            << ",\"mean_terrain_material_checks\":"
            << double(terrainWork.terrainMaterialChecks) / count
            << ",\"mean_terrain_point_requests\":"
            << double(terrainWork.terrainPointRequests) / count
            << ",\"mean_terrain_points\":" << double(terrainWork.terrainPoints) / count
            << ",\"mean_terrain_edge_points\":" << double(terrainWork.terrainEdgePoints) / count
            << ",\"mean_terrain_scratch_allocations\":"
            << double(terrainWork.terrainScratchAllocations) / count
            << ",\"mean_terrain_scratch_bytes\":" << double(terrainWork.terrainScratchBytes) / count
            << ",\"mean_terrain_scratch_buffer_bytes\":"
            << double(terrainWork.terrainScratchBufferBytes) / count;
        out << ',';
        distribution(out, "light_samples_ms", lighting);
        out << ",\"mean_polygons\":" << double(polygons) / count
            << ",\"mean_draws\":" << double(draws) / count
            << ",\"mean_input_draws\":" << double(inputDraws) / count
            << ",\"mean_pipeline_binds\":" << double(pipelineBinds) / count
            << ",\"mean_vertex_uniform_pushes\":" << double(vertexUniformPushes) / count
            << ",\"mean_fragment_uniform_pushes\":" << double(fragmentUniformPushes) / count
            << ",\"mean_binds\":" << double(binds) / count << ",\"textures\":" << textures.textures
            << ",\"texture_bytes\":" << textures.bytes
            << ",\"physical_textures\":" << textures.physicalTextures
            << ",\"texture_arrays\":" << textures.textureArrays
            << ",\"texture_array_bytes\":" << textures.textureArrayBytes
            << ",\"allocated_texture_payload_bytes\":" << textures.allocatedPayloadBytes
            << ",\"lightmap_pages\":" << textures.lightMapPages
            << ",\"lightmap_page_bytes\":" << textures.lightMapPageBytes
            << ",\"texture_uploads\":" << textures.uploads
            << ",\"texture_evictions\":" << textures.evictions
            << ",\"vertex_capacity\":" << resources.vertexCapacity
            << ",\"staging_capacity\":" << resources.uploadCapacity
            << ",\"instance_capacity\":" << resources.instanceCapacity
            << ",\"ts_mesh_cache_entries\":" << ts.entries
            << ",\"ts_mesh_cache_bytes\":" << ts.bytes << ",\"ts_mesh_cache_hits\":" << ts.hits
            << ",\"ts_mesh_cache_builds\":" << ts.builds
            << ",\"retained_meshes\":" << resources.meshes
            << ",\"retained_mesh_bytes\":" << resources.meshBytes
            << ",\"peak_retained_mesh_bytes\":" << resources.peakMeshBytes
            << ",\"mesh_uploads\":" << resources.meshUploads
            << ",\"mean_vertex_upload_bytes\":" << double(vertexBytes) / count
            << ",\"mean_mesh_upload_bytes\":" << double(meshBytes) / count
            << ",\"mean_instance_upload_bytes\":" << double(instanceBytes) / count
            << ",\"mean_texture_upload_bytes\":" << double(textureBytes) / count
            << ",\"geometry_builds\":" << geometryBuilds << ",\"map_builds\":" << mapBuilds
            << ",\"terrain_selection_cache_hits\":" << terrainCacheHits
            << ",\"archive_mapped_bytes\":" << archives.mappedBytes
            << ",\"archive_owned_bytes\":" << archives.ownedBufferBytes
            << ",\"archive_decode_staging_bytes\":" << archives.decodeStagingBytes
            << ",\"decoded_source_bytes\":" << sources.decodedBytes
            << ",\"live_source_bytes\":" << sources.liveBytes
            << ",\"unused_source_bytes\":" << sources.unusedBytes
            << ",\"source_budget_bytes\":" << sources.budgetBytes
            << ",\"source_budget_debt_bytes\":" << sources.overBudgetBytes
            << ",\"source_incomplete_entries\":" << sources.incompleteEntries
            << ",\"peak_decode_staging_bytes\":" << sources.peakDecodeStagingBytes
            << ",\"conversion_cache_bytes\":" << conversions.bytes
            << ",\"batch_owned_bytes\":" << batchMemory.batchBytes
            << ",\"batch_mesh_snapshot_bytes\":" << batchMemory.meshBytes
            << ",\"batch_texture_snapshot_bytes\":" << batchMemory.textureBytes << "}\n";
        // Optional acceptance work is deliberately outside all timing samples.
        if (std::getenv("OPENT1_VALIDATE_RENDER")) {
            gfx::HardwareBatch snapshot;
            require(game.renderFrame(surface->software(), &snapshot), "validation snapshot failed");
            gfx::GpuFrameConfig frame;
            frame.hazeColor = game.hardwareHazeColor();
            require(surface->render(snapshot, frame), "validation submission failed");
            std::vector<std::byte> before(pixels.size()), after(pixels.size());
            require(surface->readback(before), "validation readback failed");
            require(surface->trimCaches(), "cache trim failed");
            game.trimRenderCaches();
            require(surface->readback(after) && before == after, "trim changed published frame");
            require(surface->render(snapshot, frame) && surface->readback(after),
                    "trimmed batch submission failed");
            unsigned trimError{};
            for (auto i : std::views::iota(std::size_t{0}, before.size()))
                trimError = std::max(trimError, unsigned(std::abs(int(before[i]) - int(after[i]))));
            std::cout << "Trim readback maximum channel error: " << trimError << '\n';
            // A fresh page layout may change normalized UV rounding by a color level.
            require(trimError <= 2, "immutable batch did not survive trimming");
            require(game.renderFrame(surface->software()) && game.lastScenePolygonCount() > 0,
                    "software compatibility frame failed");
            bool transitioned{};
            if (const auto* mission = std::getenv("OPENT1_VALIDATE_TRANSITION")) {
                require(game.loadMission(mission), "transition mission load failed");
                for (auto i : std::views::iota(unsigned{0}, unsigned{36})) {
                    require(game.stepFrame((warm + count + 9 + i) * .032, false),
                            "transition simulation failed");
                    gfx::HardwareBatch next;
                    if (!game.renderFrame(surface->software(), &next)) {
                        throw std::runtime_error(
                            "transition preparation failed at frame " + std::to_string(i) + ": " +
                            std::string(game.console().getVariable("OpenT1::LastRenderError")));
                    }
                    frame.hazeColor = game.hardwareHazeColor();
                    require(surface->render(next, frame), "transition submission failed");
                }
                require(game.lastScenePolygonCount() > 0,
                        "transition did not render world geometry");
                transitioned = true;
            }
            std::ofstream validation(output.string() + ".validation.json");
            validation << "{\"trim_preserved_frame\":true,\"batch_survived_trim\":true,\"software_"
                          "rendered\":true,\"transition_rendered\":"
                       << (transitioned ? "true" : "false")
                       << ",\"trim_max_channel_error\":" << trimError << "}\n";
        }
        game.shutdown();
        std::cout << "Saved " << output.string() << '\n';
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
