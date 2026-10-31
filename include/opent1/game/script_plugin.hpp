#pragma once
#include <opent1/math/matrix.hpp>
#include <opent1/math/point.hpp>
#include <opent1/sim/sim_world_game.hpp>
#include <functional>
#include <optional>
#include <string>
#include <unordered_map>

namespace opent1::game {

class ScriptPlugin final : public sim::SimConsolePlugin {
public:
    enum class ObserverMode : Int32 { None = 0, Fly = 1, OrbitObject = 2, OrbitPoint = 3 };

    struct ClientRepresentative {
        Int32 clientId{};
        sim::SimObjectId ownedObjectId{};
        sim::SimObjectId controlObjectId{};
        sim::SimObjectId observerCameraId{};
        ObserverMode observerMode{ObserverMode::None};
        sim::SimObjectId observerOrbitObjectId{};
        sim::SimObjectId damageObjectId{};
        Point3F observerPos{};
        Point3F observerRot{};
        Flt32 curOrbitDist{};
        Flt32 minOrbitDist{};
        Flt32 maxOrbitDist{};
        bool canMove{};
        bool canRotate{};
    };

    using SpawnPlayerFn =
        std::function<std::optional<sim::SimObjectId>(sim::SimManager* serverManager,
                                                      std::string_view armorName,
                                                      const Point3F& pos,
                                                      const Point3F& rot)>;

    ScriptPlugin() = default;
    ~ScriptPlugin() override = default;

    void init(Console& console, sim::SimManager* manager) override;
    void shutdown() noexcept override;

    void setManager(sim::SimManager* manager) override {
        manager_ = manager;
    }

    void setServerManager(sim::SimManager* serverManager) noexcept {
        serverManager_ = serverManager;
    }

    [[nodiscard]] sim::SimManager* serverManager() const noexcept {
        return serverManager_ ? serverManager_ : manager_;
    }

    void setSpawnPlayerCallback(SpawnPlayerFn callback) {
        spawnPlayer_ = std::move(callback);
    }

    ClientRepresentative& ensureClient(Int32 clientId);
    [[nodiscard]] const ClientRepresentative* findClient(Int32 clientId) const noexcept;
    [[nodiscard]] ClientRepresentative* findClientMutable(Int32 clientId) noexcept;

    void removeClient(Int32 clientId) noexcept {
        clients_.erase(clientId);
    }

    void clearClients() noexcept {
        clients_.clear();
    }

    void setCmdObserveAvailable(bool available) noexcept {
        cmdObserveAvailable_ = available;
    }

    [[nodiscard]] bool isCmdObserveAvailable() const noexcept {
        return cmdObserveAvailable_;
    }

    [[nodiscard]] bool isCmdObserveActive() const noexcept {
        return cmdObserveActive_;
    }

    [[nodiscard]] Int32 cmdObservedPlayer() const noexcept {
        return cmdObservedPlayer_;
    }

    [[nodiscard]] Int32 hrlmDetail() const noexcept {
        return hrlmDetail_;
    }

    void setHrlmDetail(Int32 value) noexcept {
        hrlmDetail_ = value;
    }

private:
    Console* console_{};
    sim::SimManager* manager_{};
    sim::SimManager* serverManager_{};
    Int32 hrlmDetail_{1};
    bool cmdObserveAvailable_{};
    bool cmdObserveActive_{};
    Int32 cmdObservedPlayer_{-1};
    std::unordered_map<Int32, ClientRepresentative> clients_;
    SpawnPlayerFn spawnPlayer_;
};

} // namespace opent1::game
