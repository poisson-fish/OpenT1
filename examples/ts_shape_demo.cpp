// Loaded TS default-pose viewer. Requires the user's original local assets.
#define SDL_MAIN_USE_CALLBACKS 1
#include <SDL3/SDL.h>
#include <iterator>
#include <SDL3/SDL_main.h>
#include <opent1/world/world_scene.hpp>
#include <opent1/sim/common_irc_shadow.hpp>
#include <opent1/gfx/sdl_gpu_surface.hpp>
#include <algorithm>
#include <charconv>
#include <cstdio>
#include <ranges>
#include <stdexcept>

namespace {
using namespace opent1;
using namespace opent1::world;
using namespace opent1::gfx;
using namespace opent1::platform;

void require(bool ok, std::string_view message) {
    if (!ok) {
        throw std::runtime_error(std::string(message));
    }
}

struct DemoShape : SimTsShape {
    UInt32 triggers{}, contacts{};

    bool processEvent(const sim::SimEvent& event) override {
        if (dynamic_cast<const SimTsAnimationTriggerEvent*>(&event)) {
            ++triggers;
            return true;
        }
        if (dynamic_cast<const sim::SimMovementCollisionEvent*>(&event)) {
            ++contacts;
            return true;
        }
        return SimTsShape::processEvent(event);
    }
};

class ShapeDemo : public SdlApplication {
    std::unique_ptr<SdlGpuSurface> surface_;
    TsShapeResources resources_;
    Resource<Palette> palette_;
    std::unique_ptr<sim::SimWorld> world_;
    DemoShape* model_{};
    TMat3F motion_;
    WindowId window_{};
    std::string snapshot_;
    bool smoke_{}, software_{}, rootMotion_{};
    Int32 sequence_{-1};
    Flt32 yaw_{.6f};
    unsigned frames_{};

    bool onInit(std::span<const std::string_view> args) override {
        std::filesystem::path directory = "PCRPG/base";
        std::string shapeName = "ammo1.dts", volumeName;
        GpuSurfaceConfig gpu;
        Int32 collisionDetail = -1;
        const auto integer = [](std::string_view value) {
            Int32 result{};
            const auto parsed = std::from_chars(value.data(), value.data() + value.size(), result);
            require(parsed.ec == std::errc{} && parsed.ptr == value.data() + value.size() &&
                        result >= 0,
                    "Expected a nonnegative integer");
            return result;
        };
        for (auto argumentIt = std::ranges::next(args.begin(), 1, args.end());
             argumentIt != args.end();
             ++argumentIt) {
            if (*argumentIt == "--assets" && std::next(argumentIt) != args.end()) {
                directory = *++argumentIt;
            } else if (*argumentIt == "--shape" && std::next(argumentIt) != args.end()) {
                shapeName = *++argumentIt;
            } else if (*argumentIt == "--volume" && std::next(argumentIt) != args.end()) {
                volumeName = *++argumentIt;
            } else if (*argumentIt == "--driver" && std::next(argumentIt) != args.end()) {
                gpu.driver = *++argumentIt;
            } else if (*argumentIt == "--snapshot" && std::next(argumentIt) != args.end()) {
                snapshot_ = *++argumentIt;
            } else if (*argumentIt == "--software") {
                software_ = true;
            } else if (*argumentIt == "--smoke") {
                smoke_ = true;
            } else if (*argumentIt == "--sequence" && std::next(argumentIt) != args.end()) {
                sequence_ = integer(*++argumentIt);
            } else if (*argumentIt == "--collision-detail" && std::next(argumentIt) != args.end()) {
                collisionDetail = integer(*++argumentIt);
            } else if (*argumentIt == "--root-motion") {
                rootMotion_ = true;
            } else {
                throw std::runtime_error(
                    "Usage: opent1_ts_shape_demo [--assets PCRPG/base] [--shape ammo1.dts] "
                    "[--volume water.dml] [--sequence N] [--root-motion] [--collision-detail N] "
                    "[--driver vulkan|direct3d12] [--software] [--smoke] [--snapshot image.bmp]");
            }
        }
        auto manager = std::make_shared<ResourceManager>();
        require(registerTsLoaders(*manager) && registerBitmapFontLoaders(*manager) &&
                    registerPaletteLoaders(*manager),
                "Loader registration failed");
        const std::array roots{directory};
        require(manager->setSearchPaths(roots), "Asset directory could not be indexed");
        const auto volumes = manager->findMatches("*.vol");
        require(!volumes.empty(), "No original asset volumes in the requested directory");
        std::ranges::for_each(volumes, [&](const auto& name) {
            require(manager->addVolume(name), "Volume mount failed: " + name);
        });
        auto loaded = TsShapeResources::load(*manager, shapeName);
        require(bool(loaded), "Shape or material dependency failed: " + shapeName);
        resources_ = std::move(*loaded);
        palette_ = manager->load<Palette>("lush.day.ppl");
        require(bool(palette_), "Missing lush.day.ppl palette");
        world_ = std::make_unique<sim::SimWorld>(nullptr, manager);
        auto preload = std::make_unique<sim::SimPreLoadManager>();
        auto* loader = preload.get();
        require(registerWorldPreloaders(*loader) && world_->manager().addObject(std::move(preload)),
                "Preloader registration failed");
        auto model = std::make_unique<DemoShape>();
        model_ = model.get();
        require(model->setShapeName(shapeName) && world_->manager().addObject(std::move(model)),
                "World shape registration failed");
        require(model_->setCollisionDetail(collisionDetail), "Invalid collision detail");
        if (sequence_ >= 0) {
            require(bool(model_->instance()->addThread(sequence_)), "Invalid animation sequence");
        }
        if (collisionDetail >= 0) {
            sim::SimCollisionImageQuery query;
            require(model_->processQuery(query) && query.images.size() == 1,
                    "Missing TS collision image");
        }
        model_->onPreLoad(*loader);
        if (!volumeName.empty()) {
            require(loader->preLoad(volumeName, true), "Volume preload queue failed");
        }
        loader->preLoadDelayed();
        require(loader->isLoaded(shapeName) && (volumeName.empty() || loader->isLoaded(volumeName)),
                "Typed preload failed");
        auto light = std::make_unique<SimSceneLight>();
        sim::SimLight directional;
        directional.type = sim::SimLight::Type::Directional;
        directional.direction = {0, 0, -1};
        directional.intensity = {.5f, .5f, .5f};
        require(light->setLight(directional, {.5f, .5f, .5f}) &&
                    world_->manager().addObject(std::move(light)),
                "World light registration failed");
        if (!volumeName.empty()) {
            auto volume = std::make_unique<sim::SimVolumetric>();
            auto state = volume->state();
            const auto radius = std::max(resources_.shape->radius, .25f);
            state.position = {-radius, radius * 4, -radius};
            state.dimensions = {radius * 2, radius, radius * 2};
            state.materialName = volumeName;
            state.transparency = .4f;
            require(volume->setState(state) && world_->manager().addObject(std::move(volume)),
                    "World volume registration failed");
        }
        WindowConfig config;
        config.title = "OpenT1 TS - " + shapeName + " / 1 Hardware / 2 Software / Arrows Rotate";
        config.width = 640;
        config.height = 480;
        config.hidden = smoke_;
        config.quitOnDestroy = true;
        auto& window = createWindow(config);
        window_ = window.id();
        gpu.debug = smoke_;
        surface_ = SdlGpuSurface::create(window, 640, 480, gpu);
        require(bool(surface_), SDL_GetError());
        require(surface_->software().setPalette(*palette_), "Invalid shape palette");
        std::printf("Loaded %s: %zu meshes, %zu materials, %zu bitmap dependencies on %s.\n",
                    shapeName.c_str(),
                    resources_.shape->meshes.size(),
                    resources_.materials.list().materials.size(),
                    resources_.shape->dependencies().size(),
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
        }
    }

    void onIdle() override {
        const auto& shape = *resources_.shape;
        TMat3F transform(EulerF(.25f, 0, yaw_), {});
        m_mul(-shape.center, static_cast<const RMat3F&>(transform), transform.p);
        transform.p.y += std::max(shape.radius * 3, .5f);
        TMat3F target;
        m_mul(motion_, transform, target);
        require(model_->setTransform(target), "World transform failed");
        if (sequence_ >= 0) {
            TsAdvanceOptions options;
            options.applyRootMotion = rootMotion_;
            const auto advanced = model_->advanceAnimation(1. / 30, options);
            require(bool(advanced), "World animation advance failed");
            auto inverse = transform;
            inverse.inverse();
            m_mul(advanced->appliedTransform, inverse, motion_);
        }
        world_->advanceToTime(Flt32(frames_) / 30);
        sim::EnvironmentCamera camera;
        camera.viewport = {0, 0, 639, 479};
        camera.center = {320, 240};
        camera.focal = {320, 320};
        camera.nearDistance = .1f;
        camera.farDistance = 10000;
        auto frame = WorldSceneFrame::capture(*world_, camera, {{}, 10000, 5000});
        require(bool(frame), "World scene capture failed");
        require(frame->shapes().size() == 1 && frame->shapes()[0].polygonCount() > 0,
                "No visible shape polygons");
        // The final smoke snapshot must render after the entire world, resource
        // service, preload manager, providers and original shape handles die.
        if (smoke_ && frames_ == 5) {
            std::printf("World playback: %u triggers, %u contacts, position %.3f %.3f %.3f.\n",
                        model_->triggers,
                        model_->contacts,
                        model_->getTransform().p.x,
                        model_->getTransform().p.y,
                        model_->getTransform().p.z);
            world_.reset();
            model_ = nullptr;
            resources_ = {};
        }
        if (software_) {
            surface_->software().clear(0);
            require(frame->draw(surface_->software()), "Software shape draw failed");
            require(surface_->upload(), SDL_GetError());
        } else {
            auto batch = frame->hardware(*palette_);
            require(bool(batch), "Hardware material preparation failed");
            GpuFrameConfig config;
            config.clearColor = {.035f, .045f, .06f};
            require(surface_->render(*batch, config), SDL_GetError());
        }
        if (smoke_ || (!snapshot_.empty() && frames_ == 0)) {
            std::vector<std::byte> rgba(640 * 480 * 4);
            require(surface_->readback(rgba), SDL_GetError());
            const auto visible = std::ranges::count_if(
                std::views::iota(std::size_t{0}, std::size_t{640 * 480}), [&](auto i) {
                    return !std::ranges::equal(std::span(rgba).subspan(i * 4, 3),
                                               std::span(rgba).first(3));
                });
            require(visible > 500, "Loaded shape did not produce visible textured pixels");
            if (frames_ == 0 && !snapshot_.empty()) {
                auto* image =
                    SDL_CreateSurfaceFrom(640, 480, SDL_PIXELFORMAT_RGBA32, rgba.data(), 640 * 4);
                require(image != nullptr, SDL_GetError());
                const bool saved = SDL_SaveBMP(image, snapshot_.c_str());
                SDL_DestroySurface(image);
                require(saved, SDL_GetError());
            }
            if (smoke_) {
                std::printf(
                    "%s world: %zu visible triangles, %zu volumes, %lld non-background pixels.\n",
                    software_ ? "Software" : "Hardware",
                    frame->shapes()[0].polygonCount(),
                    frame->volumes().size(),
                    static_cast<long long>(visible));
            }
        }
        if (!smoke_) {
            require(surface_->presentFrame() != PresentResult::Error, SDL_GetError());
        }
        ++frames_;
        if (smoke_) {
            software_ = !software_;
            yaw_ += .15f;
            if (frames_ == 6) {
                requestQuit();
            }
        }
    }

    void onExit(Int32 code) override {
        if (code) {
            std::fprintf(stderr, "TS viewer: %s\n", error().c_str());
        }
        surface_.reset();
    }
};
}

SDL_AppResult SDL_AppInit(void** state, int argc, char** argv) {
    return initApplication<ShapeDemo>(state, argc, argv);
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
