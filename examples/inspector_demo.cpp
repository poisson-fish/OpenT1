// A real simulation inspector using original fonts and the SDL callback loop.
#define SDL_MAIN_USE_CALLBACKS 1
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <opent1/sim/sim_sdl_inspector.hpp>
#include <opent1/sim/common_irc_shadow.hpp>
#include <opent1/gfx/sdl_gpu_surface.hpp>
#include <filesystem>
#include <ranges>
#include <stdexcept>

namespace {
using namespace opent1;
using namespace opent1::sim;
using namespace opent1::platform;

void require(bool ok, std::string_view message) {
    if (!ok) {
        throw std::runtime_error(std::string(message));
    }
}

class InspectorDemo : public SdlApplication {
public:
    InspectorDemo() : SdlApplication(LoopConfig{false, 100, 60, 4096}) {}

private:
    SimGame game_;
    SimInspector inspector_;
    gfx::BitmapFont font_;
    std::unique_ptr<gfx::SdlGpuSurface> surface_;
    std::unique_ptr<SdlInspectorInput> input_;
    WindowId window_{};
    bool smoke_{};
    unsigned frames_{};

    bool onInit(std::span<const std::string_view> args) override {
        require(args.size() >= 2,
                "Usage: opent1_inspector_demo <game base directory> [GPU driver] [--smoke]");
        smoke_ = std::ranges::find(args, "--smoke") != args.end();
        require(game_.console().evaluate("newClient();focusClient();").ok(),
                "Unable to create client world");
        auto& resources = SimResource::find(*game_.manager())->get();
        const std::array roots{std::filesystem::path(args[1])};
        require(resources.setSearchPaths(roots) && gfx::registerBitmapFontLoaders(resources),
                "Unable to configure asset lookup");
        for (const auto& volume : resources.findMatches("*.vol")) {
            require(resources.addVolume(volume), "Unable to mount asset volume");
        }
        const auto font = resources.load<gfx::BitmapFont>("console.pft");
        require(font && font->info.height >= 1 && font->info.height <= 14,
                "console.pft with height 1..14 is required");
        font_ = *font;
        font_.info.scale = {65536, 65536};
        // The inspector accepts an explicit dictionary, including application labels.
        auto& tags = game_.manager()->tags();
        const std::array<std::pair<Int32, std::string_view>, 16> labels{
            {{110066, "Name"},
             {1010500, "Position"},
             {1010501, "Dimensions"},
             {1010502, "Texture scale"},
             {1010503, "Transparency"},
             {1010504, "Animation scale"},
             {1010505, "Visible faces (uppercase enables)"},
             {1010515, "Volume type"},
             {1010510, "Current velocity"},
             {1010511, "Current drag"},
             {1010512, "Density"},
             {1010513, "Damage per second"},
             {1010516, "Container drag"},
             {1010509, "Extra bounds"},
             {1010517, "Subdivisions"},
             {1010514, "Material name"}}};
        std::ranges::for_each(labels, [&](const auto& entry) {
            require(
                tags.addEntry(entry.first, std::format("ID_DEMO_{}", entry.first), entry.second),
                "Duplicate inspector label");
        });
        require(tags.addEntry(1010520, "IDITG_SV_TYPE_WATER", "Water") &&
                    tags.addEntry(1010521, "IDITG_SV_TYPE_AIR", "Air Current") &&
                    tags.addEntry(1010522, "IDITG_SV_TYPE_ACID", "Acid"),
                "Duplicate volume type");
        require(game_.createObject("SimVolumetric", "DemoVolume") != nullptr,
                "Unable to create demo volume");
        WindowConfig config;
        config.title = "OpenT1 Layer 5 Inspector";
        config.width = 640;
        config.height = 800;
        config.minWidth = 320;
        config.minHeight = 400;
        config.hidden = smoke_;
        config.quitOnDestroy = true;
        auto& window = createWindow(config);
        window_ = window.id();
        require(inspector_.resize(640, 800), "Invalid inspector size");
        gfx::GpuSurfaceConfig gpu;
        if (args.size() > 2 && args[2] != "--smoke") {
            gpu.driver = args[2];
        }
        surface_ = gfx::SdlGpuSurface::create(window, 640, 800, gpu);
        require(bool(surface_), SDL_GetError());
        gfx::Palette palette;
        for (auto i : std::views::iota(0U, 256U)) {
            palette.palettes[0].colors[i] = {UInt8(i), UInt8(i), UInt8(i), 0};
        }
        if (font_.palette) {
            palette = *font_.palette;
        }
        require(surface_->software().setPalette(palette), "Invalid inspector palette");
        input_ = std::make_unique<SdlInspectorInput>(window, inspector_);
        game_.setInspector([this](SimInspection fields) {
            const bool ok = inspector_.open(std::move(fields), game_.manager()->tags());
            input_->syncTextInput();
            return ok;
        });
        require(game_.console().evaluate("inspectObject(\"DemoVolume\");").value == "True",
                "Unable to open inspector");
        return true;
    }

    void onEvent(const Event& event) override {
        if (input_) {
            input_->process(event, game_);
        }
    }

    void onIdle() override {
        if (!surface_->ready()) {
            requestQuit();
            return;
        }
        require(inspector_.draw(surface_->software(), font_), "Inspector drawing failed");
        if (smoke_) {
            require(surface_->upload(), SDL_GetError());
            if (++frames_ == 2) {
                require(inspector_.setText(1, "10 20 30") && inspector_.apply(game_),
                        "Inspector application failed");
                const auto* volume = dynamic_cast<SimVolumetric*>(game_.findObject("DemoVolume"));
                require(volume && volume->state().position == Point3F{10, 20, 30},
                        "Inspector did not update the volume");
                requestQuit();
            }
        } else {
            require(surface_->present() != gfx::PresentResult::Error, SDL_GetError());
        }
    }

    void onExit(Int32) override {
        game_.setInspector({});
        input_.reset();
        surface_.reset();
    }
};
}

SDL_AppResult SDL_AppInit(void** state, int argc, char** argv) {
    return initApplication<InspectorDemo>(state, argc, argv);
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
