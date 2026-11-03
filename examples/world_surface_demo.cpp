// Native interior/terrain surface and receiver comparison viewer.
#define SDL_MAIN_USE_CALLBACKS 1
#include <SDL3/SDL.h>
#include <iterator>
#include <SDL3/SDL_main.h>
#include <opent1/world/world_scene.hpp>
#include <opent1/gfx/sdl_gpu_surface.hpp>
#include <algorithm>
#include <cstdio>
#include <ranges>
#include <stdexcept>

namespace {
using namespace opent1;
using namespace opent1::world;
using namespace opent1::gfx;
using namespace opent1::platform;

void require(bool value, std::string_view text) {
    if (!value) {
        throw std::runtime_error(std::string(text));
    }
}

class WorldDemo : public SdlApplication {
    std::unique_ptr<SdlGpuSurface> surface_;
    std::unique_ptr<sim::SimWorld> world_;
    SimInteriorSurface* interior_{};
    SimSceneLight* lamp_{};
    UInt64 lastTicks_{};
    gfx::Palette palette_;
    Box3F bounds_;
    std::string snapshot_;
    bool smoke_{}, software_{}, shadows_{true}, nativeShadow_{}, sceneLights_{}, automaticDetail_{},
        containers_{};
    Flt32 yaw_{.4f}, distance_{2.7f};
    UInt32 frames_{};
    WindowId window_{};

    bool onInit(std::span<const std::string_view> args) override {
        std::filesystem::path directory = "PCRPG/base";
        std::string interior = "acommand.dis", terrain;
        bool baked = true, openGLLights = false, nativeGeometry = false, nativeVisibility = false,
             nativeTraversal = false, nativeCache = false;
        GpuSurfaceConfig gpu;
        for (auto argumentIt = std::ranges::next(args.begin(), 1, args.end());
             argumentIt != args.end();
             ++argumentIt) {
            if (*argumentIt == "--assets" && std::next(argumentIt) != args.end()) {
                directory = *++argumentIt;
            } else if (*argumentIt == "--interior" && std::next(argumentIt) != args.end()) {
                interior = *++argumentIt;
            } else if (*argumentIt == "--terrain" && std::next(argumentIt) != args.end()) {
                terrain = *++argumentIt;
            } else if (*argumentIt == "--driver" && std::next(argumentIt) != args.end()) {
                gpu.driver = *++argumentIt;
            } else if (*argumentIt == "--snapshot" && std::next(argumentIt) != args.end()) {
                snapshot_ = *++argumentIt;
            } else if (*argumentIt == "--unlit") {
                baked = false;
            } else if (*argumentIt == "--opengl-lightmaps") {
                openGLLights = true;
            } else if (*argumentIt == "--native-geometry") {
                nativeGeometry = true;
            } else if (*argumentIt == "--native-visibility") {
                nativeVisibility = true;
                nativeGeometry = true;
            } else if (*argumentIt == "--native-traversal") {
                nativeTraversal = true;
                nativeVisibility = true;
                nativeGeometry = true;
            } else if (*argumentIt == "--native-cache") {
                nativeCache = true;
            } else if (*argumentIt == "--native-shadow") {
                nativeShadow_ = true;
            } else if (*argumentIt == "--no-shadow") {
                shadows_ = false;
            } else if (*argumentIt == "--scene-light") {
                sceneLights_ = true;
            } else if (*argumentIt == "--automatic-detail") {
                automaticDetail_ = true;
            } else if (*argumentIt == "--containers") {
                containers_ = true;
            } else if (*argumentIt == "--software") {
                software_ = true;
            } else if (*argumentIt == "--smoke") {
                smoke_ = true;
            } else {
                throw std::runtime_error(
                    "Usage: opent1_world_surface_demo [--assets PCRPG/base] [--interior "
                    "acommand.dis | --terrain missions/Raindance.ted] [--unlit] "
                    "[--opengl-lightmaps] [--native-geometry] [--native-visibility] "
                    "[--native-traversal] [--native-cache] [--scene-light] [--automatic-detail] "
                    "[--containers] [--no-shadow] [--native-shadow] [--driver vulkan|direct3d12] "
                    "[--software] [--smoke] [--snapshot image.bmp]");
            }
        }
        auto manager = std::make_shared<ResourceManager>();
        require(registerTsLoaders(*manager) && registerInteriorLoaders(*manager) &&
                    registerTerrainLoaders(*manager) && registerBitmapFontLoaders(*manager) &&
                    registerPaletteLoaders(*manager),
                "Loader registration failed");
        require(manager->setSearchPaths(std::array{directory}),
                "Asset directory could not be indexed");
        for (const auto& name : manager->findMatches("*.vol")) {
            require(manager->addVolume(name), "Volume mount failed: " + name);
        }
        std::optional<WorldSurfaceMesh> mesh;
        std::unique_ptr<sim::SimObject> provider;
        if (terrain.empty()) {
            auto resources = InteriorShapeResources::load(*manager, interior);
            require(bool(resources), "Interior dependency loading failed: " + interior);
            mesh = WorldSurfaceMesh::interior(*resources, 0, 0, baked);
            auto animated =
                std::make_unique<SimInteriorSurface>(std::move(*resources), TMat3F{}, 0, 0, baked);
            if (automaticDetail_) {
                require(animated->setAutomaticDetail(0), "Invalid automatic interior state");
            }
            if (containers_) {
                require(animated->configureContainer(), "Interior spatial configuration failed");
            }
            interior_ = animated.get();
            provider = std::move(animated);
        } else {
            require(manager->addVolume(terrain), "Terrain archive mount failed");
            const auto grids = manager->findMatches("*.dtf");
            require(!grids.empty(), "No native grid in terrain archive");
            auto stream = TerrainResourceStream::open(manager, grids.front());
            require(bool(stream), "Terrain metadata loading failed");
            const auto square = Flt32(1U << stream->grid().scale);
            TerrainLodView policy;
            policy.compositeTextures = true;
            policy.lightMapDensity = openGLLights ? TerrainLightMapDensity::NativeOpenGL
                                                  : TerrainLightMapDensity::Native;
            policy.nativeTextures = TerrainTextureView{};
            policy.texturePixels = 256;
            if (nativeGeometry) {
                policy.nativeGeometry = TerrainGeometryView{};
            }
            if (nativeVisibility) {
                policy.nativeVisibility = TerrainVisibilityView{};
            }
            policy.nativeTraversal = nativeTraversal;
            if (nativeCache) {
                policy.textureTiming = openGLLights ? TerrainTextureTiming::NativeOpenGL
                                                    : TerrainTextureTiming::NativeSoftware;
            }
            policy.sceneLighting = sceneLights_ && baked;
            mesh = stream->prepare(policy, baked, Box2F{0, 0, 16 * square, 16 * square});
            provider = std::make_unique<SimTerrainSurface>(
                std::move(stream), TMat3F{}, baked, Box2F{0, 0, 16 * square, 16 * square}, policy);
        }
        require(bool(mesh) && !mesh->triangles.empty(), "Native surface preparation failed");
        bounds_ = mesh->bounds;
        auto palette = manager->load<Palette>("lush.day.ppl");
        require(bool(palette), "Missing palette");
        auto shadowPalette = sim::softwareShadowPalette(*palette);
        require(bool(shadowPalette), "Shadow palette preparation failed");
        palette_ = std::move(*shadowPalette);
        world_ = std::make_unique<sim::SimWorld>(nullptr, manager);
        const auto triangles = mesh->triangles.size();
        if (!provider) {
            provider = std::make_unique<SimWorldSurface>(WorldSurfaceInstance{
                std::make_shared<const WorldSurfaceMesh>(std::move(*mesh)), {}});
        }
        require(world_->manager().addObject(std::move(provider)) != nullptr,
                "Surface registration failed");
        if (sceneLights_) {
            auto lamp = std::make_unique<SimSceneLight>();
            lamp_ = lamp.get();
            require(world_->manager().addObject(std::move(lamp)) != nullptr,
                    "Scene light registration failed");
        }
        lastTicks_ = SDL_GetTicks();
        WindowConfig config;
        config.title = "OpenT1 native world / 1 GPU / 2 Software / Arrows Rotate/Zoom";
        config.width = 640;
        config.height = 480;
        config.hidden = smoke_;
        config.quitOnDestroy = true;
        auto& window = createWindow(config);
        window_ = window.id();
        gpu.debug = smoke_;
        surface_ = SdlGpuSurface::create(window, 640, 480, gpu);
        require(bool(surface_), SDL_GetError());
        require(surface_->software().setPalette(palette_), "Invalid palette");
        std::printf("Loaded %zu native surface triangles on %s.\n",
                    triangles,
                    std::string(surface_->driver()).c_str());
        return true;
    }

    void onEvent(const Event& event) override {
        if (event.window != window_) {
            return;
        }
        if (const auto* key = std::get_if<KeyEvent>(&event.data); key && key->down) {
            if (key->key == SDLK_ESCAPE) {
                requestQuit();
            }
            if (key->key == SDLK_1) {
                software_ = false;
            }
            if (key->key == SDLK_2) {
                software_ = true;
            }
            if (key->key == SDLK_LEFT) {
                yaw_ -= .15f;
            }
            if (key->key == SDLK_RIGHT) {
                yaw_ += .15f;
            }
            if (key->key == SDLK_UP) {
                distance_ = std::max(.25f, distance_ / 1.25f);
            }
            if (key->key == SDLK_DOWN) {
                distance_ = std::min(128.f, distance_ * 1.25f);
            }
        }
    }

    void onIdle() override {
        const auto ticks = SDL_GetTicks();
        if (interior_) {
            require(interior_->advanceAnimation(smoke_ ? .125f : Flt32(ticks - lastTicks_) / 1000),
                    "Interior playback failed");
        }
        lastTicks_ = ticks;
        const auto center = (bounds_.fMin + bounds_.fMax) * .5f;
        const auto radius = std::max((bounds_.fMax - bounds_.fMin).len() * .5f, 1.f);
        if (lamp_) {
            const auto time = smoke_ ? Flt32(frames_) * .125f : Flt32(ticks) / 1000;
            sim::SimLight point;
            point.type = sim::SimLight::Type::Point;
            point.position = center + Point3F{std::sin(time) * radius * .25f,
                                              std::cos(time) * radius * .25f,
                                              radius * .1f};
            point.range = radius;
            point.intensity = {.9f, .3f, .1f};
            sim::SimLight sun;
            sun.type = sim::SimLight::Type::Directional;
            sun.direction = {0, 0, -1};
            sun.intensity = {.15f, .2f, .3f};
            require(lamp_->setLighting({{.025f, .025f, .025f}, {point, sun}}),
                    "Scene light update failed");
        }
        const auto distance = distance_ * (smoke_ && automaticDetail_ && frames_ >= 2 ? 4.f : 1.f);
        sim::EnvironmentCamera camera;
        camera.viewport = {0, 0, 639, 479};
        camera.center = {320, 240};
        camera.focal = {420, 420};
        camera.nearDistance = .1f;
        camera.farDistance = std::min(65536.f, radius * std::max(8.f, distance * 2));
        camera.worldToCamera.set(EulerF{.6f, 0, yaw_}, {});
        m_mul(-center, static_cast<const RMat3F&>(camera.worldToCamera), camera.worldToCamera.p);
        camera.worldToCamera.p.y += radius * distance;
        camera.worldToCamera.flags |= RMat3F::Matrix_HasTranslation;
        auto frame = containers_
                         ? WorldSceneFrame::captureContainers(
                               *world_, camera, {center, radius * 8, radius * 4}, sceneLights_)
                         : WorldSceneFrame::capture(
                               *world_, camera, {center, radius * 8, radius * 4}, sceneLights_);
        require(bool(frame), "World scene capture failed");
        std::vector<sim::EnvironmentShadow> shadows;
        std::optional<sim::ShadowGeometry> shadowGeometry;
        sim::ShadowBitmap shadowBitmap;
        if (shadows_) {
            // A circular test silhouette makes receiver splitting easy to see.
            // Applications can pass the TS silhouette/filter output unchanged.
            const auto size = std::max(radius * .025f, .25f);
            const Point3F source{center.x, center.y, bounds_.fMax.z + size * 2};
            const auto window = sim::makeShadowSourceWindow(
                {0, 0, -1}, source, size, {{-size, -size, -size}, {size, size, size}}, {});
            shadowGeometry =
                frame->shadowGeometry(window, {0, 0, -1}, radius * 8, .1f, nativeShadow_);
            require(bool(shadowGeometry), "Receiver collection failed");
            shadowBitmap = {64, std::vector<UInt8>(64 * 64), true};
            for (auto y : std::views::iota(UInt32{0}, UInt32{64})) {
                for (auto x : std::views::iota(UInt32{0}, UInt32{64})) {
                    if (std::hypot(Flt32(x) - 31.5f, Flt32(y) - 31.5f) < 28) {
                        shadowBitmap.pixels[y * 64 + x] = 160;
                    }
                }
            }
            shadows.push_back({*shadowGeometry, shadowBitmap});
        }
        if (smoke_ && frames_ == 3) {
            world_.reset();
            interior_ = nullptr;
            lamp_ = nullptr;
        }
        if (software_) {
            surface_->software().clear(0);
            require(frame->draw(surface_->software(), shadows), "Software world draw failed");
            require(surface_->upload(), SDL_GetError());
        } else {
            auto batch = frame->hardware(palette_, shadows);
            require(bool(batch), "Hardware world preparation failed");
            GpuFrameConfig config;
            config.clearColor = {.035f, .045f, .06f};
            require(surface_->render(*batch, config), SDL_GetError());
        }
        if (smoke_ || (!snapshot_.empty() && frames_ == 0)) {
            std::vector<std::byte> rgba(640 * 480 * 4);
            require(surface_->readback(rgba), SDL_GetError());
            const auto visible = std::ranges::count_if(
                std::views::iota(std::size_t{}, std::size_t{640 * 480}), [&](auto i) {
                    return !std::ranges::equal(std::span(rgba).subspan(i * 4, 3),
                                               std::span(rgba).first(3));
                });
            require(visible > 500, "Native world produced too few visible pixels");
            if (!snapshot_.empty() && frames_ == 0) {
                auto* image =
                    SDL_CreateSurfaceFrom(640, 480, SDL_PIXELFORMAT_RGBA32, rgba.data(), 640 * 4);
                require(image != nullptr, SDL_GetError());
                const bool saved = SDL_SaveBMP(image, snapshot_.c_str());
                SDL_DestroySurface(image);
                require(saved, SDL_GetError());
            }
            std::printf("%s: %lld visible pixels, %zu receiver polygons.\n",
                        software_ ? "Software" : "GPU",
                        static_cast<long long>(visible),
                        shadows.empty() ? 0 : shadows[0].geometry.polygons.size());
        }
        if (!smoke_) {
            require(surface_->presentFrame() != PresentResult::Error, SDL_GetError());
        }
        ++frames_;
        if (smoke_) {
            software_ = !software_;
            if (frames_ == 4) {
                requestQuit();
            }
        }
    }

    void onExit(Int32 code) override {
        if (code) {
            std::fprintf(stderr, "World viewer: %s\n", error().c_str());
        }
        surface_.reset();
    }
};
}

SDL_AppResult SDL_AppInit(void** state, int argc, char** argv) {
    return initApplication<WorldDemo>(state, argc, argv);
}

SDL_AppResult SDL_AppIterate(void* state) {
    return iterateApplication(state);
}

SDL_AppResult SDL_AppEvent(void* state, SDL_Event* event) {
    return handleApplicationEvent(state, event);
}

void SDL_AppQuit(void* state, SDL_AppResult result) {
    quitApplication(state, result);
}
