#pragma once
#include <opent1/game/ban_list.hpp>
#include <opent1/game/cpu_speed.hpp>
#include <opent1/game/data_block_manager.hpp>
#include <opent1/game/fear_cs_delegate.hpp>
#include <opent1/game/fear_dynamic_data_plugin.hpp>
#include <opent1/sim/sim_world_game.hpp>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>

namespace opent1::world {
class TerrainEditor;
}

namespace opent1::game {

class AIManager;
class ChatMenu;
class DecalManager;
class FearMissionEditor;
class HudManager;
class Player;
class PlayerManager;
class PlayerPSC;
class ScriptPlugin;
class SensorManager;

struct WorldGlobals {
    UInt32 currentTime{};
    UInt32 lastTime{};
    UInt32 timeBase{};
    Int32 interpolateTime{};
    Int32 predictForwardTime{64};
    Flt32 clockTime{};
    sim::SimWorld* world{};
    sim::SimManager* manager{};
    DataBlockManager* dbm{};
    FearCSDelegate* csDelegate{};
    PlayerManager* playerManager{};
    SensorManager* sensorManager{};
    DecalManager* decalManager{};
    AIManager* aiManager{};
    PlayerPSC* playerPSC{};
    ChatMenu* chatMenu{};
    HudManager* hudManager{};
    sim::network::PacketStream* packetStream{};
    sim::network::GhostManager* ghostManager{};
    sim::network::EventManager* eventManager{};
    ResourceManager* resManager{};

    void clear() noexcept {
        const auto interpolation = interpolateTime, prediction = predictForwardTime;
        *this = WorldGlobals{};
        interpolateTime = interpolation;
        predictForwardTime = prediction;
    }
};

class TimeDemoPlugin final : public sim::SimConsolePlugin {
public:
    TimeDemoPlugin() = default;
    ~TimeDemoPlugin() override = default;

    void init(Console& console, sim::SimManager* manager) override;
    void shutdown() noexcept override;
    void startFrame() override;
    void endFrame() override;

    void beginTimeDemo(std::string demoFile);
    void finishTimeDemo();

    [[nodiscard]] bool isActive() const noexcept {
        return active_;
    }

    [[nodiscard]] Int32 framesRendered() const noexcept {
        return frames_;
    }

private:
    Console* console_{};
    bool active_{};
    std::string demoFile_;
    Int32 frames_{};
};

class FearPlugin final : public sim::SimConsolePlugin {
public:
    explicit FearPlugin(sim::SimGame& game,
                        std::shared_ptr<ResourceManager> resources = {},
                        std::shared_ptr<BanList> banList = {},
                        std::shared_ptr<FearDynamicDataPlugin> dynamicData = {},
                        std::filesystem::path writeRoot = {},
                        std::shared_ptr<ScriptPlugin> scriptPlugin = {});
    ~FearPlugin() override = default;

    void init(Console& console, sim::SimManager* manager) override;
    void shutdown() noexcept override;
    void setManager(sim::SimManager* manager) override;
    void startFrame() override;
    void endFrame() override;

    bool createServerWorld(std::string_view missionName = {}, bool dedicated = false);
    bool createClientWorld();
    void destroyServerWorld();
    void destroyClientWorld();
    bool focusServer();
    bool focusClient();

    std::optional<sim::SimObjectId> spawnPlayer(std::string_view armorName,
                                                const Point3F& pos,
                                                const Point3F& rot,
                                                Int32 clientId = 2049,
                                                Int32 teamId = 0);
    std::size_t syncLoopbackState(bool deliverMoves = true);
    [[nodiscard]] sim::SimObject*
    findGhostForServerObject(sim::SimObjectId serverId) const noexcept;

    [[nodiscard]] WorldGlobals& serverGlobals() noexcept {
        return sg_;
    }

    [[nodiscard]] const WorldGlobals& serverGlobals() const noexcept {
        return sg_;
    }

    [[nodiscard]] WorldGlobals& clientGlobals() noexcept {
        return cg_;
    }

    [[nodiscard]] const WorldGlobals& clientGlobals() const noexcept {
        return cg_;
    }

    [[nodiscard]] WorldGlobals* activeGlobals() noexcept {
        return wg_;
    }

    [[nodiscard]] const WorldGlobals* activeGlobals() const noexcept {
        return wg_;
    }

    [[nodiscard]] const std::shared_ptr<world::TerrainEditor>& terrainEditor() const noexcept {
        return terrainEditor_;
    }

    [[nodiscard]] const std::shared_ptr<sim::network::Registry>& networkRegistry() const noexcept {
        return netRegistry_;
    }

    [[nodiscard]] bool quitRequested() const noexcept {
        return quitRequested_;
    }

    [[nodiscard]] bool cursorVisible() const noexcept {
        return cursorVisible_;
    }

    [[nodiscard]] const std::string& sadPassword() const noexcept {
        return sadPassword_;
    }

    [[nodiscard]] bool isConnected() const noexcept {
        return !connectAddress_.empty() && (connectAddress_ == "LOOPBACK:28001" ||
                                            (cg_.csDelegate && cg_.csDelegate->delegate() &&
                                             cg_.csDelegate->delegate()->stream(2048)));
    }

    [[nodiscard]] const std::string& connectAddress() const noexcept {
        return connectAddress_;
    }

    [[nodiscard]] Int32 downloadedDataBlocks() const noexcept {
        return downloadedBlocks_;
    }

    [[nodiscard]] bool isDataFinished() const noexcept {
        return dataFinishedFlag_;
    }

    [[nodiscard]] std::size_t ghostCount() const noexcept {
        return serverToClientGhosts_.size();
    }

    [[nodiscard]] const std::string& centerPrintText() const noexcept {
        return centerPrintText_;
    }

    [[nodiscard]] Int32 centerPrintPosition() const noexcept {
        return centerPrintPosition_;
    }

    std::function<void()> onConnecting;
    std::function<void()> onConnectionAccepted;
    std::function<void()> onPlayerControl;
    std::function<void(UInt32)> onGuiMode;
    std::function<void(bool)> onScoresVisible;
    std::function<void(Int32, std::string_view)> onChatMessage;

private:
    void installLegacyUtilityCommands(Console&);
    void refreshActiveGlobals();
    void stepServerSimulation();
    void stepClientSimulation();
    bool configureRemoteStream(sim::network::PacketStream&);
    void updateRemoteControls();
    void installRemoteCommands(Console&);
    void installNetworkCommands(Console&);
    void installGameplayCommands(Console&);
    void installPlayerCommands(Console&);
    void installTerrainEditing(Console&);

    sim::SimGame& game_;
    std::shared_ptr<ResourceManager> resources_;
    std::shared_ptr<BanList> banList_;
    std::shared_ptr<FearDynamicDataPlugin> dynamicData_;
    std::shared_ptr<ScriptPlugin> scriptPlugin_;
    std::shared_ptr<sim::network::Registry> netRegistry_;
    std::filesystem::path writeRoot_;
    Console* console_{};
    sim::SimManager* manager_{};
    std::shared_ptr<world::TerrainEditor> terrainEditor_;
    sim::SimAddress editedTerrain_{};
    sim::WorldType editedTerrainWorld_{sim::WorldType::Server};
    UInt64 editedTerrainWorldRevision_{};

    WorldGlobals sg_{};
    WorldGlobals cg_{};
    WorldGlobals* wg_{&cg_};

    std::unordered_map<sim::SimObjectId, sim::SimObjectId> serverToClientGhosts_;
    std::unordered_map<sim::SimObjectId, UInt64> loopbackSourceGenerations_;
    std::unordered_map<sim::SimObjectId, sim::SimObjectId> pendingRemoteControls_;
    std::string sadPassword_;
    std::string connectAddress_;
    bool quitRequested_{};
    bool cursorVisible_{true};
    bool fullscreenMode_{};
    Int32 resolutionIndex_{};
    Int32 downloadedBlocks_{};
    bool dataFinishedFlag_{};
    UInt32 serverSimulationTime_{};
    std::string centerPrintText_;
    Int32 centerPrintPosition_{};
};

} // namespace opent1::game
