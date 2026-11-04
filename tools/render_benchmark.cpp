// Ordinary optimized executable: intentionally no allocation-tracking overrides.
#include <opent1/game/game_app.hpp>
#include <ranges>
#include <opent1/game/observer_camera.hpp>
#include <opent1/game/player.hpp>
#include <opent1/gfx/sdl_gpu_surface.hpp>
#include <algorithm>
#include <chrono>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <sstream>

using namespace opent1;

namespace {
using Clock = std::chrono::steady_clock;

class BenchmarkShadowCaster : public world::SimTsShape {
public:
    bool processQuery(sim::SimQuery& query) override {
        const bool result = SimTsShape::processQuery(query);
        if (auto* shape = dynamic_cast<world::WorldShapeQuery*>(&query); shape && shape->image)
            shape->image->castShadow = true;
        return result;
    }
};

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
    out << '"' << name << "\":{\"mean\":" << sum / values.size() << ",\"p50\":" << percentile(.5)
        << ",\"p95\":" << percentile(.95) << ",\"p99\":" << percentile(.99) << '}';
}
}

int main(int argc, char** argv) {
    try {
        require(argc >= 4,
                "Usage: render_benchmark ASSETS MISSION OUTPUT "
                "[stationary|rotate|travel|follow-bot] [FRAMES] "
                "[DRIVER] [RPG]");
        const std::filesystem::path output = std::filesystem::absolute(argv[3]);
        std::filesystem::create_directories(output.parent_path());
        const std::string mode = argc > 4 ? argv[4] : "stationary";
        require(mode == "stationary" || mode == "rotate" || mode == "travel" ||
                    mode == "follow-bot",
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
        sim::SimAddress followed;
        const auto selectBot = [&] {
            for (auto* object : sg.manager->objects()) {
                if (auto* player = dynamic_cast<game::Player*>(object);
                    player && player->isAiControlled() && !player->isDead())
                    return player->address();
            }
            return sim::SimAddress{};
        };
        if (mode == "follow-bot") {
            followed = selectBot();
            require(bool(followed.resolve(*sg.manager)), "follow-bot requires a spawned bot");
        }
        auto origin = camera->getEyeTransform().p;
        auto rotation = camera->rotation();
        std::vector<BenchmarkShadowCaster*> shadowActors;
        Point3F crowdOrigin;
        const bool movingCrowd = std::getenv("OPENT1_SHADOW_BENCHMARK_MOVING") != nullptr;
        if (const auto* value = std::getenv("OPENT1_SHADOW_BENCHMARK_ORIGIN")) {
            std::istringstream position(value);
            require(bool(position >> crowdOrigin.x >> crowdOrigin.y >> crowdOrigin.z),
                    "invalid shadow benchmark origin");
            const auto* name = std::getenv("OPENT1_SHADOW_BENCHMARK_SHAPE");
            require(name && *name, "shadow benchmark requires OPENT1_SHADOW_BENCHMARK_SHAPE");
            for (unsigned i = 0; i < 32; ++i) {
                auto actor = std::make_unique<BenchmarkShadowCaster>();
                require(actor->setShapeName(name), "shadow benchmark shape failed");
                auto* caster = actor.get();
                require(bool(cg.manager->addObject(std::move(actor))),
                        "shadow benchmark actor failed");
                shadowActors.push_back(caster);
            }
            origin = crowdOrigin + Point3F{0, -16, 8};
            rotation = {-.35f, 0, 0};
        }
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
        std::vector<double> shadowIndex, shadowReceivers, shadowSilhouette, shadowFilter;
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
            for (std::size_t i = 0; i < shadowActors.size(); ++i) {
                const auto shift =
                    mode == "stationary" && !movingCrowd ? 0.f : std::sin(float(f) * .025f) * .5f;
                const auto position =
                    crowdOrigin +
                    Point3F{float(i % 8) * .5f - 1.75f + shift, float(i / 8) * .5f, 0};
                require(shadowActors[i]->setTransform(TMat3F(EulerF{}, position)),
                        "shadow benchmark placement failed");
            }
            auto pos = origin;
            auto rot = rotation;
            if (mode == "rotate" || mode == "travel")
                rot.z += Flt32(f % 120) * 6.2831853f / 120;
            if (mode == "travel") {
                pos.x += 64 * std::sin(Flt32(f) * .025f);
                pos.y += 64 * (1 - std::cos(Flt32(f) * .025f));
            }
            if (mode == "follow-bot") {
                auto* bot = dynamic_cast<const game::Player*>(followed.resolve(*sg.manager));
                if (!bot || bot->isDead()) {
                    followed = selectBot();
                    bot = dynamic_cast<const game::Player*>(followed.resolve(*sg.manager));
                }
                require(bot != nullptr, "follow-bot requires a living bot");
                const auto angle = Flt32(f % 120) * 6.2831853f / 120;
                pos = bot->getLinearPosition() +
                      Point3F{8 * std::sin(angle), -8 * std::cos(angle), 4};
                rot = {-.35f, 0, angle};
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
                shadowIndex.push_back(work.shadowIndexMs);
                shadowReceivers.push_back(work.shadowReceiversMs);
                shadowSilhouette.push_back(work.shadowSilhouetteMs);
                shadowFilter.push_back(work.shadowFilterMs);
                terrainWork.shadowCasters += work.shadowCasters;
                terrainWork.shadowRendered += work.shadowRendered;
                terrainWork.shadowCandidates += work.shadowCandidates;
                terrainWork.shadowPairTests += work.shadowPairTests;
                terrainWork.shadowClipCalls += work.shadowClipCalls;
                terrainWork.shadowIndexHits += work.shadowIndexHits;
                terrainWork.shadowGeometryHits += work.shadowGeometryHits;
                terrainWork.shadowSilhouetteHits += work.shadowSilhouetteHits;
                terrainWork.shadowBudgetExhaustions += work.shadowBudgetExhaustions;
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
            << surface->driver() << "\",\"frames\":" << count << ",\"load_ms\":" << loadMs
            << ",\"benchmark_shadow_casters\":" << shadowActors.size()
            << ",\"benchmark_shadow_moving\":" << (movingCrowd ? "true" : "false") << ',';
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
        distribution(out, "shadow_index_ms", shadowIndex);
        out << ',';
        distribution(out, "shadow_receivers_ms", shadowReceivers);
        out << ',';
        distribution(out, "shadow_silhouette_ms", shadowSilhouette);
        out << ',';
        distribution(out, "shadow_filter_ms", shadowFilter);
        out << ",\"mean_shadow_casters\":" << double(terrainWork.shadowCasters) / count
            << ",\"mean_shadow_rendered\":" << double(terrainWork.shadowRendered) / count
            << ",\"mean_shadow_candidates\":" << double(terrainWork.shadowCandidates) / count
            << ",\"mean_shadow_pairs\":" << double(terrainWork.shadowPairTests) / count
            << ",\"mean_shadow_clips\":" << double(terrainWork.shadowClipCalls) / count
            << ",\"mean_shadow_index_hits\":" << double(terrainWork.shadowIndexHits) / count
            << ",\"mean_shadow_geometry_hits\":" << double(terrainWork.shadowGeometryHits) / count
            << ",\"mean_shadow_silhouette_hits\":"
            << double(terrainWork.shadowSilhouetteHits) / count
            << ",\"shadow_budget_exhaustions\":" << terrainWork.shadowBudgetExhaustions;
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
