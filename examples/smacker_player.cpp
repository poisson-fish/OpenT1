#define SDL_MAIN_USE_CALLBACKS 1
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <opent1/video/sdl_player.hpp>
#include <opent1/core/file_stream.hpp>
#include <chrono>
#include <stdexcept>

namespace {
using namespace opent1;
using namespace opent1::video;

UInt64 now() {
    return UInt64(std::chrono::duration_cast<std::chrono::microseconds>(
                      std::chrono::steady_clock::now().time_since_epoch())
                      .count());
}

class MovieApplication final : public platform::SdlApplication {
public:
    MovieApplication() : SdlApplication({false, 100, 120, 4096}) {}

private:
    std::shared_ptr<gfx::SdlGpuSurface> surface_;
    std::shared_ptr<audio::Device> device_;
    std::shared_ptr<SdlOutput> output_;
    std::unique_ptr<Player> player_;
    UInt64 duration_{};
    bool smoke_{};
    UInt32 frames_{};

    static void require(bool ok) {
        if (!ok) {
            throw std::runtime_error("Smacker playback failed");
        }
    }

    bool onInit(std::span<const std::string_view> args) override {
        if (args.size() < 2) {
            throw std::runtime_error(
                "usage: opent1_smacker_player file.smk [--loop] [--smoke] [--driver=name]");
        }
        FileRStream stream{std::filesystem::path(args[1])};
        std::shared_ptr<const Movie> movie = Movie::load(stream, stream.getSize());
        require(bool(movie));
        duration_ = movie->durationUs();
        PlayerOptions options;
        gfx::GpuSurfaceConfig gpu;
        for (std::size_t i = 2; i < args.size(); ++i) {
            if (args[i] == "--loop") {
                options.loop = true;
            } else if (args[i] == "--smoke") {
                smoke_ = true;
            } else if (args[i].starts_with("--driver=")) {
                gpu.driver = args[i].substr(9);
            } else {
                throw std::runtime_error("unknown movie option");
            }
        }
        platform::WindowConfig config;
        config.title = "OpenT1 Smacker - Space: pause, arrows: seek, Esc: close";
        config.width = Int32(movie->width());
        config.height = Int32(movie->height() * movie->verticalScale());
        auto& window = createWindow(config);
        surface_ =
            gfx::SdlGpuSurface::create(window, Int32(movie->width()), Int32(movie->height()), gpu);
        require(bool(surface_));
        device_ = std::make_shared<audio::Device>(audio::DeviceOptions{smoke_, 48000, {}, {}});
        output_ = std::make_shared<SdlOutput>(surface_, device_);
        player_ = std::make_unique<Player>(output_);
        require(player_->load(movie, now(), options));
        return true;
    }

    void onEvent(const platform::Event& event) override {
        const auto* key = std::get_if<platform::KeyEvent>(&event.data);
        if (!key || !key->down || !player_) {
            return;
        }
        const auto& state = player_->snapshot();
        if (key->key == SDLK_ESCAPE) {
            requestQuit();
        } else if (key->key == SDLK_SPACE) {
            require(state.state == PlayState::Playing ? player_->pause(now())
                                                      : player_->play(now()));
        } else if (key->key == SDLK_LEFT) {
            require(player_->seek(state.timeUs > 1000000 ? state.timeUs - 1000000 : 0, now()));
        } else if (key->key == SDLK_RIGHT) {
            require(player_->seek(std::min(state.timeUs + 1000000, duration_), now()));
        }
    }

    void onIdle() override {
        require(player_->tick(now()));
        if (smoke_) {
            static_cast<void>(device_->render(400));
        }
        require(surface_->presentFrame() != gfx::PresentResult::Error);
        if (player_->snapshot().state == PlayState::Ended || (smoke_ && ++frames_ == 8)) {
            requestQuit();
        }
    }

    void onExit(Int32 code) override {
        if (code) {
            std::fprintf(stderr, "Smacker: %s\n", error().c_str());
        }
        player_.reset();
        output_.reset();
        device_.reset();
        surface_.reset();
    }
};
}

SDL_AppResult SDL_AppInit(void** state, int argc, char** argv) {
    return opent1::platform::initApplication<MovieApplication>(state, argc, argv);
}

SDL_AppResult SDL_AppIterate(void* state) {
    return opent1::platform::iterateApplication(state);
}

SDL_AppResult SDL_AppEvent(void* state, SDL_Event* event) {
    return opent1::platform::handleApplicationEvent(state, event);
}

void SDL_AppQuit(void* state, SDL_AppResult result) {
    opent1::platform::quitApplication(state, result);
}
