#pragma once
#include <opent1/game/ai.hpp>
#include <opent1/game/ban_list.hpp>
#include <opent1/game/cpu_speed.hpp>
#include <opent1/game/fear_dynamic_data_plugin.hpp>
#include <opent1/game/fear_mission_editor.hpp>
#include <opent1/game/fear_plugin.hpp>
#include <opent1/game/file_plugin.hpp>
#include <opent1/game/script_plugin.hpp>
#include <opent1/game/version.hpp>
#include <opent1/gfx/bitmap_palette_font.hpp>
#include <opent1/gfx/palette.hpp>
#include <opent1/gfx/software_rasterizer.hpp>
#include <opent1/gfx/hardware_batch.hpp>
#include <opent1/gfx/render_stats.hpp>
#include <opent1/sim/sim_world_game.hpp>
#include <opent1/gui/runtime.hpp>
#include <opent1/gui/presentation.hpp>
#include <opent1/world/input_commands.hpp>
#include <opent1/world/ts_static_shape.hpp>
#include <filesystem>
#include <deque>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace opent1::game {

class SmackerCtrl;

class FearGameApp {
public:
    std::function<void(SmackerCtrl&)> bindMovieAudio;
    std::function<void(std::string_view)> playShellSound;

    struct RenderStats {
        double captureMs{}, worldMs{}, guiMs{}, totalMs{};
        bool lightingFallback{};
        gfx::RenderWorkStats work;
    };

    [[nodiscard]] const RenderStats& renderStats() const noexcept {
        return renderStats_;
    }

    struct Config {
        std::filesystem::path writeRoot{};
        std::filesystem::path assetRoot{};
        Int32 overrideCpuLoops{1290};
        bool registerDefaultPlugins{true};
        bool mountDiscoveredAssets{false};
    };

    FearGameApp();
    explicit FearGameApp(Config config);
    ~FearGameApp();

    FearGameApp(const FearGameApp&) = delete;
    FearGameApp& operator=(const FearGameApp&) = delete;

    [[nodiscard]] static std::filesystem::path
    discoverAssetRoot(const std::filesystem::path& startDir = {});

    bool init(std::span<const std::string_view> commandLineArgs = {});
    bool mountAssetRoot(const std::filesystem::path& assetRoot);
    bool executeConsoleCS();
    bool setupLiveDemoSession();
    bool loadMission(std::string_view missionNameOrFile, bool dedicated = false);

    void deferMissionLoading(bool enabled) {
        deferMissionLoading_ = enabled;
    }

    [[nodiscard]] bool missionPending() const {
        return pendingMission_.has_value();
    }

    bool loadPendingMission();
    bool stepFrame(double wallTimeSeconds);
    bool bindGameplayActions();
    [[nodiscard]] bool gameplayInputActive() const;
    [[nodiscard]] bool gameplayKeyboardActive() const;
    bool escapeGameplay();
    bool toggleConsole();
    [[nodiscard]] bool consoleVisible() const;
    bool handleGameplayAction(const sim::SimActionEvent&);
    bool setGameplayGuiMode(UInt32 mode);
    void showGameplayScores(bool visible);
    // Hardware palette/fill gamma is applied before lighting and blending;
    // submit the returned batch with final-frame gamma 1.
    bool renderFrame(gfx::SoftwareSurface& surface,
                     gfx::HardwareBatch* hardware = nullptr,
                     Flt32 paletteGamma = 1);
    [[nodiscard]] gui::Presentation guiPresentation(Point2I framebufferPixels) const;

    [[nodiscard]] Point3F hardwareHazeColor() const noexcept {
        return hardwareHazeColor_;
    }

    void shutdown() noexcept;

    [[nodiscard]] sim::SimGame& game() noexcept {
        return game_;
    }

    [[nodiscard]] const sim::SimGame& game() const noexcept {
        return game_;
    }

    [[nodiscard]] Console& console() noexcept {
        return game_.console();
    }

    [[nodiscard]] const Console& console() const noexcept {
        return const_cast<sim::SimGame&>(game_).console();
    }

    [[nodiscard]] ResourceManager& resources() noexcept {
        return *resources_;
    }

    [[nodiscard]] const std::shared_ptr<ResourceManager>& sharedResources() const noexcept {
        return resources_;
    }

    [[nodiscard]] BanList& banList() noexcept {
        return *banList_;
    }

    [[nodiscard]] FilePlugin& filePlugin() noexcept {
        return *filePlugin_;
    }

    [[nodiscard]] ScriptPlugin& scriptPlugin() noexcept {
        return *scriptPlugin_;
    }

    [[nodiscard]] FearDynamicDataPlugin& dynamicDataPlugin() noexcept {
        return *dynamicDataPlugin_;
    }

    [[nodiscard]] FearPlugin& fearPlugin() noexcept {
        return *fearPlugin_;
    }

    [[nodiscard]] AIPlugin& aiPlugin() noexcept {
        return *aiPlugin_;
    }

    [[nodiscard]] FearMissionPlugin& missionPlugin() noexcept {
        return *missionPlugin_;
    }

    [[nodiscard]] TimeDemoPlugin& timeDemoPlugin() noexcept {
        return *timeDemoPlugin_;
    }

    [[nodiscard]] const CpuMetrics& cpuMetrics() const noexcept {
        return cpuMetrics_;
    }

    [[nodiscard]] bool isInitialized() const noexcept {
        return initialized_;
    }

    [[nodiscard]] const std::filesystem::path& mountedAssetRoot() const noexcept {
        return mountedAssetRoot_;
    }

    [[nodiscard]] std::size_t mountedVolumeCount() const noexcept {
        return mountedVolumes_.size();
    }

    [[nodiscard]] std::span<const std::string> mountedVolumes() const noexcept {
        return mountedVolumes_;
    }

    [[nodiscard]] UInt32 steppedFrames() const noexcept {
        return steppedFrames_;
    }

    [[nodiscard]] UInt32 renderedFrames() const noexcept {
        return renderedFrames_;
    }

    [[nodiscard]] const std::shared_ptr<gfx::Palette>& defaultPalette() const noexcept {
        return defaultPalette_;
    }

    [[nodiscard]] const std::shared_ptr<gfx::BitmapFont>& defaultFont() const noexcept {
        return defaultFont_;
    }

    [[nodiscard]] const TMat3F& listenerTransform() const noexcept {
        return listenerTransform_;
    }

    [[nodiscard]] const Point3F& listenerVelocity() const noexcept {
        return listenerVelocity_;
    }

    [[nodiscard]] const std::shared_ptr<gui::Runtime>& guiRuntime() const noexcept {
        return guiRuntime_;
    }

    [[nodiscard]] const world::InputMaps& inputMaps() const noexcept {
        return inputMaps_;
    }

    [[nodiscard]] std::string_view loadedMissionName() const noexcept {
        return loadedMissionName_;
    }

    [[nodiscard]] std::size_t lastSceneSurfaceCount() const noexcept {
        return lastSceneSurfaceCount_;
    }

    [[nodiscard]] std::size_t lastSceneShapeCount() const noexcept {
        return lastSceneShapeCount_;
    }

    [[nodiscard]] std::size_t lastScenePolygonCount() const noexcept {
        return lastScenePolygonCount_;
    }

    [[nodiscard]] std::size_t lastSceneLightCount() const noexcept {
        return lastSceneLightCount_;
    }

private:
    void registerGameClassesAndFactories();
    void ensureFallbackRenderAssets();
    void setActivePalette(std::shared_ptr<gfx::Palette> pal);
    void updateGameplayPalette();
    void installRuntimeServices();
    void installShellCommands();
    void updateGameplayGui();
    [[nodiscard]] bool gameplayWorldVisible() const;
    void displayChatMessage(Int32 type, std::string_view message);

    Config config_{};
    std::shared_ptr<ResourceManager> resources_;
    std::shared_ptr<BanList> banList_;
    sim::SimGame game_;
    std::shared_ptr<FilePlugin> filePlugin_;
    std::shared_ptr<ScriptPlugin> scriptPlugin_;
    std::shared_ptr<FearDynamicDataPlugin> dynamicDataPlugin_;
    std::shared_ptr<FearPlugin> fearPlugin_;
    std::shared_ptr<AIPlugin> aiPlugin_;
    std::shared_ptr<FearMissionPlugin> missionPlugin_;
    std::shared_ptr<TimeDemoPlugin> timeDemoPlugin_;
    std::shared_ptr<gui::Runtime> guiRuntime_;
    world::InputMaps inputMaps_;
    CpuMetrics cpuMetrics_{};
    std::filesystem::path mountedAssetRoot_{};
    std::vector<std::string> mountedVolumes_{};
    std::shared_ptr<gfx::Palette> defaultPalette_{};
    std::shared_ptr<gfx::Palette> gameplayPalette_{};
    std::shared_ptr<gfx::BitmapFont> defaultFont_{};
    TMat3F listenerTransform_{};
    Point3F listenerVelocity_{0.0f, 0.0f, 0.0f};
    std::string loadedMissionName_{};
    std::size_t lastSceneSurfaceCount_{0};
    std::size_t lastSceneShapeCount_{0};
    std::size_t lastScenePolygonCount_{0};
    std::size_t lastSceneLightCount_{0};
    Point3F hardwareHazeColor_{};
    UInt32 steppedFrames_{0};
    UInt32 renderedFrames_{0};
    bool initialized_{};
    double lastWallTime_{-1.0};
    double simulationWallTime_{};
    RenderStats renderStats_{};
    UInt64 inputWorldRevision_{~UInt64{0}};
    sim::SimAddress inputTarget_{};
    std::array<std::shared_ptr<const sim::SimActionMap>, 3> boundInputMaps_{};

    struct PendingMission {
        std::string name;
        bool dedicated{};
    };

    std::optional<PendingMission> pendingMission_;
    bool deferMissionLoading_{};
    UInt32 gameplayGuiMode_{1};
    bool showingScores_{};
    std::vector<std::string> consoleHistory_;
    std::deque<std::pair<Int32, std::string>> chatMessages_;
    std::weak_ptr<const void> chatDisplayLifetime_;
    world::NativeSoftwareTextureCache softwareTextureCache_;
};

[[nodiscard]] int runFearMain(std::span<const std::string_view> commandLineArgs = {});

} // namespace opent1::game
