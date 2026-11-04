#include <opent1/game/fear_player_psc.hpp>
#include <iterator>
#include <ranges>
#include <opent1/game/game_app.hpp>
#include <opent1/game/sound.hpp>
#include <opent1/game/player.hpp>
#include <opent1/gfx/software_rasterizer.hpp>

#if defined(OPENT1_STANDALONE_EXECUTABLE)
#define SDL_MAIN_USE_CALLBACKS 1
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#else
#include <SDL3/SDL.h>
#endif

#include <opent1/gfx/sdl_gpu_surface.hpp>
#include <opent1/platform/sdl_window.hpp>
#include <opent1/platform/process_console.hpp>
#include <opent1/gui/sdl_gui.hpp>
#include <opent1/sim/sim_sdl_input.hpp>

#include <opent1/audio/audio.hpp>
#include <opent1/video/audio_player.hpp>
#include <opent1/game/shell_controls.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <chrono>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace opent1::game {

namespace {

struct RuntimeOptions {
    std::filesystem::path exePath{};
    std::filesystem::path assetRoot{};
    std::string gpuDriver{};
    std::string missionName{};
    std::string renderer{};
    std::optional<bool> fullscreen;
    bool verifyInput{}, renderStats{};
    Int32 maxFrames{-1};
    bool smoke{false};
    bool hidden{false};
    bool headless{false};
    bool sdlLoop{false};
    bool audioLoopback{false};
    bool noAudio{false};
    bool mountAssets{false};
    bool noAssets{false};
    bool execConsoleCs{false};
    bool spawnDemo{false};
    bool dedicated{false};

    [[nodiscard]] bool createsWindow() const noexcept {
        return !headless && (!dedicated || sdlLoop || smoke);
    }
};

[[nodiscard]] Int32 parsePositiveIntSv(std::string_view sv, Int32 fallback) noexcept {
    while (!sv.empty() && sv.front() == ' ') {
        sv.remove_prefix(1);
    }
    Int32 val = fallback;
    const auto [ptr, ec] = std::from_chars(sv.data(), sv.data() + sv.size(), val);
    (void)ptr;
    return (ec == std::errc{} && val >= 0) ? val : fallback;
}

[[nodiscard]] RuntimeOptions parseRuntimeOptions(std::span<const std::string_view> args,
                                                 bool standaloneDefault) {
    RuntimeOptions opts{};
    for (auto argumentIt = args.begin(); argumentIt != args.end(); ++argumentIt) {
        const std::string_view arg = *argumentIt;
        if (arg == "--smoke") {
            opts.smoke = true;
            opts.hidden = true;
            opts.audioLoopback = true;
        } else if (arg == "--hidden") {
            opts.hidden = true;
        } else if (arg == "--headless") {
            opts.headless = true;
        } else if (arg == "--renderer" && std::next(argumentIt) != args.end()) {
            opts.renderer = *++argumentIt;
        } else if (arg == "--fullscreen") {
            opts.fullscreen = true;
        } else if (arg == "--windowed") {
            opts.fullscreen = false;
        } else if (arg == "--verify-input") {
            opts.verifyInput = true;
        } else if (arg == "--render-stats") {
            opts.renderStats = true;
        } else if (arg == "--sdl-loop") {
            opts.sdlLoop = true;
        } else if (arg == "--audio-loopback") {
            opts.audioLoopback = true;
        } else if (arg == "--no-audio") {
            opts.noAudio = true;
        } else if (arg == "--mount-assets") {
            opts.mountAssets = true;
        } else if (arg == "--no-assets") {
            opts.noAssets = true;
        } else if (arg == "--exec-console-cs") {
            opts.execConsoleCs = true;
            opts.mountAssets = true;
        } else if (arg == "--spawn-demo") {
            opts.spawnDemo = true;
        } else if (arg == "-dedicated") {
            opts.dedicated = true;
        } else if (arg == "--frames" && std::next(argumentIt) != args.end()) {
            opts.maxFrames = parsePositiveIntSv(*++argumentIt, 2);
        } else if (arg == "--driver" && std::next(argumentIt) != args.end()) {
            opts.gpuDriver = std::string(*++argumentIt);
        } else if (arg == "--assets" && std::next(argumentIt) != args.end()) {
            opts.assetRoot = std::filesystem::path(*++argumentIt);
            opts.mountAssets = true;
        } else if (arg == "--mission" && std::next(argumentIt) != args.end()) {
            opts.missionName = std::string(*++argumentIt);
            opts.mountAssets = true;
        }
    }

    if (opts.maxFrames < 0) {
        if (opts.smoke) {
            opts.maxFrames = 4;
        } else if (!standaloneDefault && !opts.sdlLoop) {
            opts.maxFrames = 2;
        } else {
            opts.maxFrames = 0;
        }
    }
    return opts;
}

[[maybe_unused]] [[nodiscard]] std::size_t
countNonZeroSurfaceBytes(const gfx::SoftwareSurface& surface) noexcept {
    std::size_t count = 0;
    for (const std::byte b : surface.bitmap().pixels) {
        if (b != std::byte{0}) {
            ++count;
        }
    }
    return count;
}

[[maybe_unused]] [[nodiscard]] std::shared_ptr<const audio::Wave> makeFallbackWave() {
    auto wave = std::make_shared<audio::Wave>();
    wave->channels = 1;
    wave->bits = 16;
    wave->rate = 22050;
    constexpr std::size_t kSampleCount = 512;
    wave->samples.resize(kSampleCount * sizeof(Int16));
    for (auto i : std::views::iota(std::size_t{0}) | std::views::take_while([&](std::size_t i) {
                      return i < kSampleCount;
                  })) {
        const Int16 sample = (i % 32u < 16u) ? static_cast<Int16>(3000) : static_cast<Int16>(-3000);
        const auto u = static_cast<UInt16>(sample);
        wave->samples[i * 2] = static_cast<std::byte>(u & 0xFFu);
        wave->samples[i * 2 + 1] = static_cast<std::byte>((u >> 8u) & 0xFFu);
    }
    return wave;
}


class MovieAudioService : public std::enable_shared_from_this<MovieAudioService> {
    struct Binding {
        std::unique_ptr<video::AudioPlayer> player;
    };

    audio::Device* device_{};
    std::vector<std::weak_ptr<Binding>> bindings_;

public:
    ~MovieAudioService() {
        close();
    }

    void device(audio::Device& device) {
        device_ = &device;
    }

    void bind(SmackerCtrl& control) {
        auto binding = std::make_shared<Binding>();
        bindings_.push_back(binding);
        control.startAudio = [service = weak_from_this(),
                              binding](std::shared_ptr<const video::Movie> movie, bool loop) {
            const auto owner = service.lock();
            if (!owner || !owner->device_) {
                return;
            }
            auto player = std::make_unique<video::AudioPlayer>(*owner->device_);
            if (player->start(std::move(movie), loop)) {
                binding->player = std::move(player);
            }
        };
        control.stopAudio = [binding] {
            binding->player.reset();
        };
    }

    void update() {
        std::erase_if(bindings_, [](const auto& binding) {
            return binding.expired();
        });
        for (const auto& weak : bindings_) {
            if (const auto binding = weak.lock(); binding && binding->player) {
                (void)binding->player->update();
            }
        }
    }

    void close() noexcept {
        for (const auto& weak : bindings_) {
            if (const auto binding = weak.lock()) {
                binding->player.reset();
            }
        }
        device_ = nullptr;
    }
};

class FearMainApplication final : public platform::SdlApplication {
public:
    explicit FearMainApplication(bool includesProgramName = true)
        : platform::SdlApplication(platform::LoopConfig{
              .checkFocus = false,
              .nonfocusTimeoutMs = 16,
              .foregroundRateHz = 0,
              .maxPendingEvents = 4096,
          }),
          includesProgramName_(includesProgramName) {}

protected:
    SDL_InitFlags requiredSubsystems(std::span<const std::string_view> args) const override {
        return parseRuntimeOptions(args, includesProgramName_).createsWindow()
                   ? SDL_INIT_VIDEO | SDL_INIT_GAMEPAD
                   : SDL_INIT_EVENTS;
    }

    bool onInit(std::span<const std::string_view> rawArgs) override {
        std::span<const std::string_view> userArgs = rawArgs;
        std::filesystem::path exeDir{};
        if (includesProgramName_ && !rawArgs.empty()) {
            exeDir = std::filesystem::path(rawArgs.front()).parent_path();
            userArgs = rawArgs.subspan(1);
        }

        opts_ = parseRuntimeOptions(userArgs, includesProgramName_);
        opts_.exePath = exeDir;

        if (includesProgramName_ && (opts_.headless || opts_.dedicated)) {
            processConsole_ = std::make_unique<platform::ProcessConsole>();
            consoleOutput_ = app_.console().subscribeOutput([this](std::string_view line) {
                processConsole_->writeLine(line);
            });
        }

        (void)audio::registerWaveLoader(app_.resources());
        app_.bindMovieAudio = [service = movieAudio_](SmackerCtrl& control) {
            service->bind(control);
        };

        if (!opts_.noAssets && (opts_.mountAssets || includesProgramName_)) {
            const auto discovered =
                !opts_.assetRoot.empty() ? opts_.assetRoot : FearGameApp::discoverAssetRoot(exeDir);
            if (!discovered.empty()) {
                (void)app_.mountAssetRoot(discovered);
            }
        }

        // Startup --mission must leave a loading frame for the window to show.
        app_.deferMissionLoading(includesProgramName_ && !opts_.headless && !opts_.hidden &&
                                 !opts_.smoke && !opts_.dedicated);
        if (!app_.init(userArgs)) {
            return false;
        }

        if (opts_.createsWindow()) {
            platform::WindowConfig winCfg{};
            winCfg.title = "Starsiege: Tribes (OpenT1)";
            winCfg.width = 640;
            winCfg.height = 480;
            winCfg.hidden = opts_.hidden || opts_.smoke || opts_.dedicated || !includesProgramName_;
            winCfg.resizable = true;
            winCfg.quitOnDestroy = true;
            auto& win = createWindow(winCfg);
            mainWindow_ = win.id();
            if (app_.guiRuntime()) {
                guiInput_ = std::make_unique<gui::SdlInput>(win, app_.guiRuntime()->canvas);
                guiInput_->logicalExtent({640, 480});
            }

            const auto* rawDriver = SDL_GetCurrentVideoDriver();
            const std::string_view videoDriver =
                rawDriver ? std::string_view(rawDriver) : std::string_view{};
            if (videoDriver != "dummy" || !opts_.gpuDriver.empty()) {
                gfx::GpuSurfaceConfig gpuCfg{};
                gpuCfg.driver = opts_.gpuDriver;
                gpuCfg.presentMode = gfx::GpuPresentMode::Immediate;
                const auto cacheEntries =
                    app_.console().getVariable("pref::GPUTextureCacheEntries");
                if (!cacheEntries.empty()) {
                    std::size_t entries{};
                    const auto parsed = std::from_chars(
                        cacheEntries.data(), cacheEntries.data() + cacheEntries.size(), entries);
                    if (parsed.ec != std::errc{} ||
                        parsed.ptr != cacheEntries.data() + cacheEntries.size() || !entries) {
                        std::cerr << "Invalid pref::GPUTextureCacheEntries\n";
                        return false;
                    }
                    gpuCfg.textureCacheEntries = entries;
                }
                try {
                    gpuSurface_ = gfx::SdlGpuSurface::create(win, 640, 480, gpuCfg);
                } catch (...) {
                    gpuSurface_.reset();
                }
            }
        }

        if (!gpuSurface_) {
            softwareSurface_ = gfx::SoftwareSurface::create(640, 480);
            if (!softwareSurface_) {
                return false;
            }
        }

        // Legacy Options scripts address MainWindow. Route their presentation
        // commands to the SDL window that actually owns the game framebuffer.
        auto& console = app_.console();
        if (gpuSurface_) {
            // Native SimCanvas::surfaceChanged restores brightness after the
            // startup scripts have loaded ClientPrefs. Hardware consumes this
            // gamma in palette expansion; software consumes it on upload.
            const auto text = console.getVariable("pref::Display::gammaValue");
            float gamma{};
            const auto parsed = std::from_chars(text.data(), text.data() + text.size(), gamma);
            if (parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size()) {
                (void)gpuSurface_->setGamma(gamma);
            }
        }
        console.setVariable("OpenT1::HardwareAvailable", gpuSurface_ ? "True" : "False");
        if (!opts_.renderer.empty()) {
            if (opts_.renderer != "software" && opts_.renderer != "hardware") {
                return false;
            }
            if (opts_.renderer == "hardware" && !gpuSurface_) {
                std::cerr << "Hardware renderer unavailable: " << SDL_GetError() << '\n';
                return false;
            }
            for (const auto pref : {"pref::VideoWindowedDriver", "pref::VideoFullScreenDriver"}) {
                console.setVariable(pref, opts_.renderer == "hardware" ? "Hardware" : "Software");
            }
        }
        if (auto* window = findWindow(mainWindow_)) {
            const auto pref = console.getVariable("pref::VideoFullScreen");
            const bool fullscreen =
                opts_.fullscreen.value_or(pref == "True" || pref == "true" || pref == "1");
            if (!SDL_SetWindowFullscreen(window->nativeHandle(), fullscreen)) {
                return false;
            }
        }
        console.setVariable("OpenT1::AudioBackend", opts_.noAudio ? "Disabled" : "OpenAL");
        console.addCommand(0,
                           "OpenT1::videoModes",
                           [this](Console&, Int32, std::span<const std::string>) -> std::string {
                               auto* window = findWindow(mainWindow_);
                               if (!window) {
                                   return "640x480";
                               }
                               int count{};
                               auto** modes = SDL_GetFullscreenDisplayModes(
                                   SDL_GetDisplayForWindow(window->nativeHandle()), &count);
                               std::string result;
                               for (auto i :
                                    std::views::iota(0) | std::views::take_while([&](int i) {
                                        return modes && i < count;
                                    })) {
                                   if (modes[i]->w >= 640 && modes[i]->h >= 480) {
                                       result += std::to_string(modes[i]->w) + "x" +
                                                 std::to_string(modes[i]->h) + "\t";
                                   }
                               }
                               SDL_free(modes);
                               return result;
                           });
        console.addCommand(
            0,
            "setFullscreenMode",
            [this](Console&, Int32, std::span<const std::string> args) -> std::string {
                auto* window = findWindow(mainWindow_);
                if (!window || args.size() != 3) {
                    return "False";
                }
                const auto& value = args[2];
                const bool fullscreen =
                    value == "True" || value == "TRUE" || value == "true" || value == "1";
                return SDL_SetWindowFullscreen(window->nativeHandle(), fullscreen) ? "True"
                                                                                   : "False";
            });
        console.addCommand(0,
                           "isFullscreenMode",
                           [this](Console&, Int32, std::span<const std::string>) -> std::string {
                               auto* window = findWindow(mainWindow_);
                               return window && (SDL_GetWindowFlags(window->nativeHandle()) &
                                                 SDL_WINDOW_FULLSCREEN)
                                          ? "True"
                                          : "False";
                           });
        console.addCommand(
            0,
            "setFSResolution",
            [this](Console&, Int32, std::span<const std::string> args) -> std::string {
                auto* window = findWindow(mainWindow_);
                if (!window || args.size() != 3) {
                    return "False";
                }
                const auto& res = args[2];
                const auto x = res.find('x');
                if (x == std::string::npos) {
                    return "False";
                }
                int width{}, height{};
                auto a = std::from_chars(res.data(), res.data() + x, width);
                auto b = std::from_chars(res.data() + x + 1, res.data() + res.size(), height);
                if (a.ec != std::errc{} || b.ec != std::errc{} || a.ptr != res.data() + x ||
                    b.ptr != res.data() + res.size() || width < 640 || height < 480 ||
                    width > 16384 || height > 16384) {
                    return "False";
                }
                auto* native = window->nativeHandle();
                if (SDL_GetWindowFlags(native) & SDL_WINDOW_FULLSCREEN) {
                    SDL_DisplayMode mode{};
                    if (!SDL_GetClosestFullscreenDisplayMode(
                            SDL_GetDisplayForWindow(native), width, height, 0, true, &mode)) {
                        return "False";
                    }
                    return SDL_SetWindowFullscreenMode(native, &mode) ? "True" : "False";
                }
                return SDL_SetWindowSize(native, width, height) ? "True" : "False";
            });
        console.addCommand(
            0,
            "setGamma",
            [this](Console& c, Int32, std::span<const std::string> args) -> std::string {
                if (args.size() != 3) {
                    return "False";
                }
                float gamma{};
                const auto& text = args[2];
                auto r = std::from_chars(text.data(), text.data() + text.size(), gamma);
                if (r.ec != std::errc{} || r.ptr != text.data() + text.size()) {
                    return "False";
                }
                const bool ok = gpuSurface_ && gpuSurface_->setGamma(gamma);
                if (ok) {
                    c.setVariable("pref::Display::gammaValue", text);
                }
                return ok ? "True" : "False";
            });
        const auto savedResolution = console.getVariable("pref::VideoFullScreenRes");
        if (!savedResolution.empty()) {
            const std::array<std::string, 2> args{"MainWindow", savedResolution};
            (void)console.execute("setFSResolution", args);
        }

        if (!opts_.noAudio) {
            try {
                audio::DeviceOptions devOpts{};
                devOpts.loopback = opts_.audioLoopback || opts_.smoke || !includesProgramName_;
                devOpts.rate = 48000;
                audioLoopback_ = devOpts.loopback;
                std::optional<audio::Device> dev;
                try {
                    dev.emplace(devOpts);
                } catch (...) {
                    devOpts.loopback = true;
                    audioLoopback_ = true;
                    dev.emplace(devOpts);
                }
                audioRuntime_ = std::make_unique<audio::Runtime>(std::move(*dev));
                (void)audioRuntime_->setMaxBuffers(app_.legacySound.maxBuffers());
                app_.legacySound.setMaxBuffers = [this](UInt32 count) {
                    return audioRuntime_ && audioRuntime_->setMaxBuffers(count);
                };
                app_.legacySound.play = [this](UInt32 id,
                                               std::string_view file,
                                               const audio::Profile& profile,
                                               float priority) -> UInt64 {
                    auto wave = app_.resources().load<audio::Wave>(file);
                    const auto key = id | 0x40000000U;
                    return wave && audioRuntime_ &&
                                   audioRuntime_->define(key, std::move(wave), profile, priority)
                               ? audioRuntime_->play(
                                     key,
                                     {},
                                     (profile.flags & (audio::Software3D | audio::Hardware3D)) != 0)
                               : 0;
                };
                app_.legacySound.stop = [this](UInt64 handle) {
                    return audioRuntime_ && audioRuntime_->stop(handle);
                };
                movieAudio_->device(audioRuntime_->device());
                audioRuntime_->setHardware3D(
                    script::toBool(app_.console().getVariable("$pref::useDSound3D")));
                app_.playShellSound = [this](std::string_view name) {
                    if (!audioRuntime_) {
                        return;
                    }
                    auto wave = app_.resources().load<audio::Wave>(name);
                    audio::Profile profile;
                    profile.flags = 0;
                    profile.baseVolume = 0;
                    const UInt32 id = name == "Button2.wav" ? 0xFFFF0002u : 0xFFFF0001u;
                    if (wave && audioRuntime_->define(id, std::move(wave), profile)) {
                        (void)audioRuntime_->play(id, {}, false);
                    }
                };

                if (opts_.spawnDemo) {
                    audio::Profile profile{};
                    profile.flags = audio::Flag::Software3D;
                    profile.baseVolume = 0.85f;
                    profile.minDistance = 10.0f;
                    profile.maxDistance = 300.0f;

                    bool definedWave = false;
                    if (app_.resources().contains("button5.wav")) {
                        if (auto wavRes = app_.resources().load<audio::Wave>("button5.wav")) {
                            definedWave =
                                audioRuntime_->define(1u, std::move(wavRes), profile, 1.0f);
                        }
                    }
                    if (!definedWave) {
                        definedWave = audioRuntime_->define(1u, makeFallbackWave(), profile, 1.0f);
                    }
                    if (definedWave) {
                        activeVoice_ = audioRuntime_->play(
                            1u, app_.listenerTransform(), true, app_.listenerVelocity());
                    }
                }
            } catch (...) {
                movieAudio_->close();
                audioRuntime_.reset();
            }
        }

        if (app_.guiRuntime()) {
            app_.guiRuntime()->canvas.onFrame = [this] {
                (void)renderCurrentFrame();
            };
        }
        startTicksMs_ = SDL_GetTicks();
        if (processConsole_) {
            processConsole_->writeLine("OpenT1 script console ready. Enter quit(); to stop.");
            processConsole_->showPrompt(app_.console().getVariable("Console::Prompt"));
        }
        return true;
    }

    void bindGameplayAudio() {
        if (!audioRuntime_) {
            return;
        }
        for (auto* globals :
             {&app_.fearPlugin().clientGlobals(), &app_.fearPlugin().serverGlobals()}) {
            auto* manager = globals->manager;
            if (!manager || manager->findObject("GameSoundPlayback")) {
                continue;
            }
            auto playback = std::make_unique<SoundPlayback>(
                [this, manager](
                    UInt32 id, const TMat3F& transform, const Point3F& velocity) -> SoundHandle {
                    auto* dbm =
                        dynamic_cast<DataBlockManager*>(manager->findObject(DataBlockManagerId));
                    auto* sound = dbm ? dynamic_cast<SoundData*>(dbm->lookupDataBlock(
                                            Int32(id), DataBlockManager::SoundDataType))
                                      : nullptr;
                    if (!sound || !audioRuntime_) {
                        return 0;
                    }
                    auto wave = app_.resources().load<audio::Wave>(sound->wavFileName);
                    if (!wave) {
                        app_.console().print("Missing sound: " + sound->wavFileName);
                        return 0;
                    }
                    audio::Profile profile;
                    if (auto* source = dynamic_cast<SoundProfileData*>(dbm->lookupDataBlock(
                            sound->profileIndex, DataBlockManager::SoundProfileDataType))) {
                        profile.flags = source->profile.flags;
                        profile.baseVolume = source->profile.baseVolume;
                        profile.minDistance = source->profile.minDistance;
                        profile.maxDistance = source->profile.maxDistance;
                        profile.coneInsideAngle = UInt32(source->profile.coneInsideAngle);
                        profile.coneOutsideAngle = UInt32(source->profile.coneOutsideAngle);
                        profile.coneOutsideVolume = source->profile.coneOutsideVolume;
                        profile.coneVector = source->profile.coneVector;
                    }
                    if (!audioRuntime_->define(id, std::move(wave), profile, sound->priority)) {
                        return 0;
                    }
                    return SoundHandle(audioRuntime_->play(id, transform, true, velocity));
                });
            playback->playTagged = [this](UInt32 tag,
                                          const TMat3F& transform,
                                          const Point3F& velocity) -> SoundHandle {
                // Native base/scripts/sound.cs pairs (the stone/metal filenames
                // are intentionally crossed in the shipped sound table).
                const char* name = tag == 450100   ? "thump1.wav"
                                   : tag == 450101 ? "grenadeMetal.wav"
                                   : tag == 450102 ? "grenadeStone.wav"
                                                   : nullptr;
                if (!name || !audioRuntime_) {
                    return 0;
                }
                auto wave = app_.resources().load<audio::Wave>(name);
                audio::Profile profile;
                profile.flags = audio::Software3D;
                profile.minDistance = 1;
                profile.maxDistance = 100;
                if (!wave || !audioRuntime_->define(tag, std::move(wave), profile)) {
                    return 0;
                }
                return SoundHandle(audioRuntime_->play(tag, transform, true, velocity));
            };
            playback->move =
                [this](SoundHandle handle, const TMat3F& transform, const Point3F& velocity) {
                    if (audioRuntime_) {
                        (void)audioRuntime_->move(handle, transform, velocity);
                    }
                };
            playback->stop = [this](SoundHandle handle) {
                if (audioRuntime_) {
                    (void)audioRuntime_->stop(handle);
                }
            };
            playback->done = [this](SoundHandle handle) {
                return !audioRuntime_ ||
                       audioRuntime_->snapshot(handle).status == audio::Status::Stopped;
            };
            playback->playRelative =
                [this, play = playback->play](UInt32 id, const Point3F& relative) -> SoundHandle {
                TMat3F transform;
                transform.identity();
                transform.p = app_.listenerTransform().p + relative;
                return play(id, transform, {});
            };
            auto voices = std::make_shared<std::unordered_map<Int32, audio::Handle>>();
            playback->playVoice = [this, voices](Int32 speaker,
                                                 std::string_view filename,
                                                 const SoundProfileData& data,
                                                 const TMat3F& transform,
                                                 const Point3F& velocity) -> SoundHandle {
                if (!audioRuntime_) {
                    return 0;
                }
                if (const auto previous = voices->find(speaker); previous != voices->end()) {
                    (void)audioRuntime_->stop(previous->second);
                    voices->erase(previous);
                }
                auto wave = app_.resources().load<audio::Wave>(filename);
                if (!wave) {
                    return 0;
                }
                audio::Profile profile;
                profile.flags = data.profile.flags;
                profile.baseVolume = data.profile.baseVolume;
                profile.minDistance = data.profile.minDistance;
                profile.maxDistance = data.profile.maxDistance;
                profile.coneInsideAngle = UInt32(data.profile.coneInsideAngle);
                profile.coneOutsideAngle = UInt32(data.profile.coneOutsideAngle);
                profile.coneOutsideVolume = data.profile.coneOutsideVolume;
                profile.coneVector = data.profile.coneVector;
                const auto id = 0xf0000000U | UInt32(speaker);
                if (!audioRuntime_->define(id, std::move(wave), profile)) {
                    return 0;
                }
                const bool spatial = (profile.flags & (audio::Software3D | audio::Hardware3D)) != 0;
                const auto handle = audioRuntime_->play(id, transform, spatial, velocity);
                if (handle) {
                    (*voices)[speaker] = handle;
                }
                return SoundHandle(handle);
            };
            manager->addObject(std::move(playback));
        }
    }

    bool commanderInputActive() const {
        const auto* runtime = app_.guiRuntime().get();
        const auto* root = runtime ? runtime->canvas.top() : nullptr;
        return root && root->getName() == "commandGui";
    }

    void bindInput() {
        auto& cg = app_.fearPlugin().clientGlobals();
        (void)app_.bindGameplayActions();
        if (!cg.manager || !mainWindow_) {
            gameInput_.reset();
            inputManagerNumber_ = ~UInt64{0};
            return;
        }
        const auto revision = app_.game().worldRevision(sim::WorldType::Client);
        if (gameInput_ && inputManagerNumber_ == revision) {
            return;
        }
        gameInput_.reset();
        inputManagerNumber_ = revision;
        if (auto* input =
                dynamic_cast<sim::SimInputManager*>(cg.manager->findObject(sim::InputManagerId))) {
            gameInput_ = std::make_unique<sim::SdlSimInput>(*input, mainWindow_);
        }
        if (gameInput_) {
            gameInput_->capture(actionInputCaptured_);
        }
    }

    void onEvent(const platform::Event& ev) override {
        if (const auto* lifecycle = std::get_if<platform::AppEvent>(&ev.data);
            lifecycle && (lifecycle->type == SDL_EVENT_WILL_ENTER_BACKGROUND ||
                          lifecycle->type == SDL_EVENT_LOW_MEMORY)) {
            if (gpuSurface_) (void)gpuSurface_->trimCaches();
            app_.trimRenderCaches();
        }
        bindInput();
        if (const auto* key = std::get_if<platform::KeyEvent>(&ev.data);
            key && key->scancode == SDL_SCANCODE_GRAVE) {
            suppressConsoleText_ = key->down;
            if (key->down && !key->repeat) {
                (void)app_.toggleConsole();
                if (auto* psc = app_.fearPlugin().clientGlobals().playerPSC) {
                    psc->resetInput();
                }
                actionInputCaptured_ = app_.gameplayKeyboardActive() ||
                                       (commanderInputActive() && !app_.consoleVisible());
                if (gameInput_) {
                    gameInput_->capture(actionInputCaptured_);
                }
                if (guiInput_) {
                    (void)guiInput_->synchronize();
                }
            }
            return;
        }
        if (const auto* text = std::get_if<platform::TextEvent>(&ev.data);
            text && suppressConsoleText_) {
            suppressConsoleText_ = false;
            if (text->text == "`" || text->text == "~") {
                return;
            }
        }
        if (app_.consoleVisible()) {
            if (const auto* key = std::get_if<platform::KeyEvent>(&ev.data);
                key && key->down && key->key == SDLK_ESCAPE) {
                (void)app_.toggleConsole();
            } else if (guiInput_) {
                (void)guiInput_->process(ev);
            }
            return;
        }
        auto& cg = app_.fearPlugin().clientGlobals();
        const bool playing = app_.gameplayInputActive();
        const bool commanding = commanderInputActive();
        if (const auto* key = std::get_if<platform::KeyEvent>(&ev.data);
            key && key->down && !key->repeat && key->key == SDLK_ESCAPE) {
            if (cg.playerPSC) {
                (void)app_.escapeGameplay();
                actionInputCaptured_ = app_.gameplayKeyboardActive() || commanderInputActive();
                if (gameInput_) {
                    gameInput_->capture(actionInputCaptured_);
                }
            } else if (guiInput_) {
                (void)guiInput_->process(ev);
            }
            return;
        }
        const bool lifecycle = std::holds_alternative<platform::WindowEvent>(ev.data) ||
                               std::holds_alternative<platform::AppEvent>(ev.data) ||
                               std::holds_alternative<platform::GamepadDeviceEvent>(ev.data);
        const auto* key = std::get_if<platform::KeyEvent>(&ev.data);
        const auto* runtime = app_.guiRuntime().get();
        const bool editing = runtime && dynamic_cast<gui::TextEdit*>(runtime->canvas.focus());
        // PDA actions share the keyboard action map while the tactical screen
        // keeps an absolute mouse for selecting units and placing waypoints.
        const bool commandKey =
            (commanding || app_.gameplayKeyboardActive()) && key && (!editing || !key->down);
        if (gameInput_ && (playing || lifecycle || commandKey)) {
            (void)gameInput_->process(ev);
        }
        if (guiInput_ && (!playing || lifecycle)) {
            (void)guiInput_->process(ev);
        }
    }

    void onIdle() override {
        if (processConsole_) {
            const auto commands = processConsole_->poll();
            for (const auto& command : commands) {
                const auto result = app_.console().evaluate(command, "<stdin>");
                for (const auto& diagnostic : result.diagnostics) {
                    app_.console().print(diagnostic.format());
                }
                if (result.ok() && !result.value.empty()) {
                    app_.console().print(result.value);
                }
                if (app_.fearPlugin().quitRequested()) {
                    requestQuit(0);
                    return;
                }
            }
            if (!commands.empty()) {
                processConsole_->showPrompt(app_.console().getVariable("Console::Prompt"));
            }
        }
        if (loadingPresented_ && app_.missionPending()) {
            (void)app_.loadPendingMission();
            loadingPresented_ = false;
        }
        bindInput();
        bindGameplayAudio();
        auto& cg = app_.fearPlugin().clientGlobals();
        const bool capture = app_.gameplayInputActive() && (foreground() || opts_.verifyInput);
        const bool actionCapture =
            (app_.gameplayKeyboardActive() || (commanderInputActive() && !app_.consoleVisible())) &&
            (foreground() || opts_.verifyInput);
        if (actionCapture != actionInputCaptured_) {
            actionInputCaptured_ = actionCapture;
            if (gameInput_) {
                gameInput_->capture(actionCapture);
            }
        }
        if (auto* window = findWindow(mainWindow_)) {
            if (capture != mouseCaptured_) {
                mouseCaptured_ = capture;
                (void)SDL_SetWindowRelativeMouseMode(window->nativeHandle(), capture);
            }
        }
        if (guiInput_) {
            (void)guiInput_->synchronize();
        }
        if (opts_.verifyInput && framesRun_ == 0) {
            auto* player =
                cg.playerPSC ? dynamic_cast<Player*>(cg.playerPSC->getControlObject()) : nullptr;
            if (!player || !gameInput_) {
                std::cerr << "Input verification: no player adapter\n";
                requestQuit(1);
                return;
            }
            initialPitch_ = player->getViewPitch();
            initialPosition_ = player->getLinearPosition();
            SDL_Event event{};
            event.type = SDL_EVENT_WINDOW_FOCUS_GAINED;
            event.window.windowID = mainWindow_;
            SDL_PushEvent(&event);
            event = {};
            event.type = SDL_EVENT_KEY_DOWN;
            event.key.windowID = mainWindow_;
            event.key.key = SDLK_W;
            event.key.scancode = SDL_SCANCODE_W;
            event.key.down = true;
            SDL_PushEvent(&event);
            event = {};
            event.type = SDL_EVENT_MOUSE_MOTION;
            event.motion.windowID = mainWindow_;
            event.motion.yrel = 12;
            SDL_PushEvent(&event);
        }
        if (opts_.spawnDemo && cg.playerPSC) {
            PlayerMove move{};
            move.forwardAction = 1.0f;
            move.jetting = (framesRun_ % 2u) == 0u;
            move.turnRot = 0.03f;
            move.trigger = framesRun_ >= 1u;
            cg.playerPSC->pushMove(move);
        }

        const double wallTimeSeconds =
            (opts_.smoke || opts_.maxFrames > 0)
                ? (static_cast<double>(framesRun_ + 1u) * 0.032)
                : (static_cast<double>(SDL_GetTicks() - startTicksMs_) * 0.001);

        // The SDL host renders once below, after prediction, GUI and audio
        // updates. Keep the canvas callback for explicit script redraws.
        const bool keepGoing = app_.stepFrame(wallTimeSeconds, false);
        if (opts_.verifyInput && framesRun_ == 3) {
            const auto* player =
                cg.playerPSC ? dynamic_cast<Player*>(cg.playerPSC->getControlObject()) : nullptr;
            inputVerified_ = player && cg.playerPSC->currentMove().forwardAction > 0 &&
                             std::abs(player->getViewPitch() - initialPitch_) > .001f &&
                             (player->getLinearPosition() - initialPosition_).len() > .001f;
            if (!inputVerified_) {
                std::cerr << "Input verification: SDL movement/camera failed\n";
                requestQuit(1);
                return;
            }
            SDL_Event event{};
            event.type = SDL_EVENT_KEY_UP;
            event.key.windowID = mainWindow_;
            event.key.key = SDLK_W;
            event.key.scancode = SDL_SCANCODE_W;
            SDL_PushEvent(&event);
        }
        if (opts_.verifyInput && framesRun_ == 5 &&
            cg.playerPSC->currentMove().forwardAction != 0) {
            std::cerr << "Input verification: released key remained active\n";
            requestQuit(1);
            return;
        }

        if (audioRuntime_) {
            movieAudio_->update();
            const auto volume = [this](std::string_view name) {
                const auto value = app_.console().getVariable(name);
                return value.empty() ? 1.f : std::clamp(Flt32(script::toNumber(value)), 0.f, 1.f);
            };
            (void)audioRuntime_->setMaster(volume("pref::sfx2DVolume"),
                                           volume("pref::sfx3DVolume"));
            (void)audioRuntime_->setListener(audio::Listener{
                .transform = app_.listenerTransform(),
                .velocity = app_.listenerVelocity(),
            });
            if (activeVoice_ != 0) {
                (void)audioRuntime_->move(
                    activeVoice_, app_.listenerTransform(), app_.listenerVelocity());
            }
            const auto now = app_.game().simulationTime();
            (void)audioRuntime_->advance(std::clamp(now - audioSimulationTime_, 0.0, .256));
            // Finished effects otherwise retain backend sources and exhaust
            // the 128-voice limit after a short firefight.
            (void)audioRuntime_->purge();
            audioSimulationTime_ = now;
            if (audioLoopback_) {
                const auto mixed = audioRuntime_->device().render(1536);
                mixedAudioSamples_ += mixed.size();
            }
        }

        if (!renderCurrentFrame()) {
            return;
        }

        ++framesRun_;
        if (!keepGoing || app_.fearPlugin().quitRequested() ||
            (opts_.maxFrames > 0 && framesRun_ >= static_cast<UInt32>(opts_.maxFrames))) {
            requestQuit(0);
        }
    }

    bool renderCurrentFrame() {
        const auto frameStart = std::chrono::steady_clock::now();
        const auto* window = findWindow(mainWindow_);
        if (window) {
            int width{}, height{};
            if (!SDL_GetWindowSizeInPixels(window->nativeHandle(), &width, &height)) {
                return false;
            }
            if (width > 0 && height > 0) {
                if (gpuSurface_) {
                    if (!gpuSurface_->resize(width, height)) {
                        requestQuit(1);
                        return false;
                    }
                } else if (softwareSurface_ && (softwareSurface_->bitmap().width != width ||
                                                softwareSurface_->bitmap().height != height)) {
                    auto next = gfx::SoftwareSurface::create(width, height);
                    if (!next || (softwareSurface_->palette() &&
                                  !next->setPalette(*softwareSurface_->palette()))) {
                        requestQuit(1);
                        return false;
                    }
                    softwareSurface_ = std::move(next);
                }
            }
        }
        gfx::SoftwareSurface* targetSurface =
            gpuSurface_ ? &gpuSurface_->software() : softwareSurface_.get();
        const bool fullscreen =
            window && (SDL_GetWindowFlags(window->nativeHandle()) & SDL_WINDOW_FULLSCREEN);
        const bool hardware =
            gpuSurface_ &&
            app_.console().getVariable(fullscreen ? "pref::VideoFullScreenDriver"
                                                  : "pref::VideoWindowedDriver") == "Hardware";
        const auto displayGamma = gpuSurface_ ? gpuSurface_->gamma() : 1.f;
        gfx::HardwareBatch batch;
        bool rendered = targetSurface && app_.renderFrame(*targetSurface,
                                                          hardware ? &batch : nullptr,
                                                          hardware ? displayGamma : 1.f);
        if (rendered) {
            if (guiInput_) {
                guiInput_->presentation(app_.guiPresentation(
                    {targetSurface->bitmap().width, targetSurface->bitmap().height}));
            }
            if (opts_.smoke && !hardware && firstFrameNonZeroPixels_ == 0) {
                firstFrameNonZeroPixels_ = countNonZeroSurfaceBytes(*targetSurface);
            }
            if (gpuSurface_) {
                if (hardware) {
                    gfx::GpuFrameConfig frame;
                    frame.hazeColor = app_.hardwareHazeColor();
                    (void)gpuSurface_->setGamma(1);
                    rendered = gpuSurface_->render(batch, frame);
                    (void)gpuSurface_->setGamma(displayGamma);
                } else {
                    rendered = gpuSurface_->upload();
                }
                if (rendered) {
                    const auto presented = gpuSurface_->presentFrame();
                    rendered = presented != gfx::PresentResult::Error;
                    if (app_.missionPending() && presented == gfx::PresentResult::Presented) {
                        loadingPresented_ = true;
                    }
                    if (opts_.smoke && framesRun_ == 0u) {
                        std::vector<std::byte> readbackBuf(
                            static_cast<std::size_t>(targetSurface->bitmap().width) *
                            static_cast<std::size_t>(targetSurface->bitmap().height) * 4u);
                        if (gpuSurface_->readback(readbackBuf) && hardware) {
                            // Hardware HUDs can bypass the indexed framebuffer.
                            // Inspect actual presented RGB rather than alpha or
                            // an intentionally empty software staging surface.
                            firstFrameNonZeroPixels_ = 0;
                            for (std::size_t i = 0; i < readbackBuf.size(); i += 4) {
                                firstFrameNonZeroPixels_ += readbackBuf[i] != std::byte{} ||
                                                            readbackBuf[i + 1] != std::byte{} ||
                                                            readbackBuf[i + 2] != std::byte{};
                            }
                        }
                    }
                }
            } else if (window) {
                // Software rendering also works when no SDL_GPU device can be
                // created (remote desktop, unsupported driver, dummy tests).
                presentationPixels_.resize(std::size_t(targetSurface->bitmap().width) *
                                           targetSurface->bitmap().height * 4);
                rendered = targetSurface->rgba(presentationPixels_);
                auto* destination = SDL_GetWindowSurface(window->nativeHandle());
                auto* source = rendered ? SDL_CreateSurfaceFrom(targetSurface->bitmap().width,
                                                                targetSurface->bitmap().height,
                                                                SDL_PIXELFORMAT_RGBA32,
                                                                presentationPixels_.data(),
                                                                targetSurface->bitmap().width * 4)
                                        : nullptr;
                rendered = source && destination &&
                           SDL_BlitSurfaceScaled(
                               source, nullptr, destination, nullptr, SDL_SCALEMODE_NEAREST) &&
                           SDL_UpdateWindowSurface(window->nativeHandle());
                SDL_DestroySurface(source);
                if (rendered && app_.missionPending()) {
                    loadingPresented_ = true;
                }
            } else if (app_.missionPending()) {
                loadingPresented_ = true;
            }
        }
        if (!rendered) {
            const auto error = app_.console().getVariable("OpenT1::LastRenderError") +
                               std::string(" ") + SDL_GetError();
            if (error != lastRenderError_) {
                std::cerr << "Rendering failed: " << error << '\n';
                lastRenderError_ = error;
            }
            if (opts_.smoke) {
                requestQuit(1);
                return false;
            }
        } else {
            lastRenderError_.clear();
        }
        if (opts_.renderStats) {
            const auto& s = app_.renderStats();
            const auto total = std::chrono::duration<double, std::milli>(
                                   std::chrono::steady_clock::now() - frameStart)
                                   .count();
            std::cout << "FRAME " << framesRun_ << ' ' << (hardware ? "hardware" : "software")
                      << " capture=" << s.captureMs << " world=" << s.worldMs << " gui=" << s.guiMs
                      << " render=" << s.totalMs << " total=" << total
                      << " lightingFallback=" << s.lightingFallback
                      << " displayGamma=" << displayGamma;
            if (targetSurface) {
                std::cout << " framebuffer=" << targetSurface->bitmap().width << 'x'
                          << targetSurface->bitmap().height;
            }
            if (targetSurface) {
                const auto ui = app_.guiPresentation(
                    {targetSurface->bitmap().width, targetSurface->bitmap().height});
                std::cout << " ui=" << ui.extent.x << 'x' << ui.extent.y << " uiScale=" << ui.scale;
            }
            const auto& work = s.work;
            std::cout << " terrain=" << work.terrainLodMs
                      << " terrainCalls=" << work.terrainLodCalls
                      << " terrainNodes=" << work.terrainNodes
                      << " validatedHeights=" << work.terrainValidatedHeights
                      << " mergeHeights=" << work.terrainMergeHeights
                      << " materialChecks=" << work.terrainMaterialChecks
                      << " distanceRejects=" << work.terrainDistanceRejects
                      << " lightSamples=" << work.lightSamplesMs
                      << " lightCalls=" << work.lightSampleCalls
                      << " lightDecodes=" << work.lightDeltaDecodes
                      << " lightCopyBytes=" << work.lightSampleCopyBytes
                      << " bitmapCopyBytes=" << work.bitmapCopyBytes
                      << " layerScanPixels=" << work.layerScanPixels
                      << " surfaceInversions=" << work.surfaceInversions
                      << " tileBuilds=" << work.terrainTileBuilds
                      << " tileHits=" << work.terrainTileHits
                      << " tileEvictions=" << work.terrainTileEvictions
                      << " terrainCacheEntries=" << work.terrainCacheEntries
                      << " terrainCacheBytes=" << work.terrainCacheBytes << " sky=" << work.skyMs
                      << " hardwareSkyPolygons=" << work.hardwareSkyPolygons
                      << " shadowIndex=" << work.shadowIndexMs
                      << " shadowReceivers=" << work.shadowReceiversMs
                      << " shadowSilhouette=" << work.shadowSilhouetteMs
                      << " shadowFilter=" << work.shadowFilterMs
                      << " shadowCasters=" << work.shadowCasters
                      << " shadowRendered=" << work.shadowRendered
                      << " shadowCandidates=" << work.shadowCandidates
                      << " shadowPairs=" << work.shadowPairTests
                      << " shadowClips=" << work.shadowClipCalls
                      << " shadowIndexBuilds=" << work.shadowIndexBuilds
                      << " shadowIndexHits=" << work.shadowIndexHits
                      << " shadowGeometryHits=" << work.shadowGeometryHits
                      << " shadowSilhouetteHits=" << work.shadowSilhouetteHits
                      << " shadowWork=" << work.shadowWork
                      << " shadowBudgetExhaustions=" << work.shadowBudgetExhaustions
                      << " shadowSkipped=" << work.shadowSkipped;
            if (hardware) {
                const auto submitted = gpuSurface_->submissionStats();
                const auto textures = gpuSurface_->textureCacheStats();
                std::cout << " inputDraws=" << submitted.inputDraws << " draws=" << submitted.draws
                          << " pipelines=" << submitted.pipelineBinds
                          << " samplers=" << submitted.samplerBinds
                          << " vertexUniforms=" << submitted.vertexUniformPushes
                          << " fragmentUniforms=" << submitted.fragmentUniformPushes
                          << " fenceWait=" << submitted.fenceWaitMs
                          << " gpuUploads=" << textures.uploads << " gpuReuses=" << textures.reuses
                          << " gpuEvictions=" << textures.evictions
                          << " gpuTextures=" << textures.textures
                          << " gpuTextureBytes=" << textures.bytes;
            }
            std::cout << '\n';
        }

        return true;
    }

    void onExit(Int32 exitCode) override {
        if (exitCode != 0 && !error().empty()) {
            std::cerr << "Application failed: " << error() << '\n';
        }
        if ((opts_.verifyInput && !inputVerified_) ||
            (opts_.smoke &&
             (framesRun_ == 0u || app_.renderedFrames() == 0u || firstFrameNonZeroPixels_ == 0u))) {
            requestQuit(1);
        }
        if (app_.guiRuntime()) {
            app_.guiRuntime()->canvas.onFrame = {};
        }
        gameInput_.reset();
        guiInput_.reset();
        gpuSurface_.reset();
        softwareSurface_.reset();
        movieAudio_->close();
        if (audioRuntime_) {
            audioRuntime_->stopAll();
            audioRuntime_.reset();
        }
        app_.shutdown();
        consoleOutput_.reset();
    }

private:
    platform::WindowId mainWindow_{};
    UInt64 inputManagerNumber_{};
    std::unique_ptr<gui::SdlInput> guiInput_;
    std::unique_ptr<sim::SdlSimInput> gameInput_;
    bool mouseCaptured_{}, actionInputCaptured_{}, loadingPresented_{};
    bool suppressConsoleText_{};
    bool inputVerified_{};
    Flt32 initialPitch_{};
    Point3F initialPosition_{};
    std::string lastRenderError_;
    std::vector<std::byte> presentationPixels_;
    bool includesProgramName_{true};
    double audioSimulationTime_{};
    RuntimeOptions opts_{};
    FearGameApp app_{};
    std::unique_ptr<platform::ProcessConsole> processConsole_;
    std::shared_ptr<Console::Output> consoleOutput_;
    std::unique_ptr<gfx::SdlGpuSurface> gpuSurface_{};
    std::unique_ptr<gfx::SoftwareSurface> softwareSurface_{};
    std::unique_ptr<audio::Runtime> audioRuntime_{};
    std::shared_ptr<MovieAudioService> movieAudio_{std::make_shared<MovieAudioService>()};
    audio::Handle activeVoice_{0};
    std::size_t mixedAudioSamples_{0};
    bool audioLoopback_{false};
    UInt64 startTicksMs_{0};
    UInt32 framesRun_{0};
    std::size_t firstFrameNonZeroPixels_{0};
};

} // namespace

#if !defined(OPENT1_STANDALONE_EXECUTABLE)

int runFearMain(std::span<const std::string_view> commandLineArgs) {
    FearMainApplication app(false);
    SDL_AppResult status = app.initialize(commandLineArgs, false);
    while (status == SDL_APP_CONTINUE) {
        SDL_Event event{};
        while (SDL_PollEvent(&event)) {
            status = app.event(event);
            if (status != SDL_APP_CONTINUE) {
                break;
            }
        }
        if (status == SDL_APP_CONTINUE) {
            status = app.iterate();
        }
    }
    app.shutdown(status);
    return (status == SDL_APP_SUCCESS && app.exitCode() == 0) ? 0 : 1;
}

#endif // !OPENT1_STANDALONE_EXECUTABLE

} // namespace opent1::game

#if defined(OPENT1_STANDALONE_EXECUTABLE)

SDL_AppResult SDL_AppInit(void** state, int argc, char** argv) {
    return opent1::platform::initApplication<opent1::game::FearMainApplication>(state, argc, argv);
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

#endif // OPENT1_STANDALONE_EXECUTABLE
