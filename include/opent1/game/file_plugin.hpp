#pragma once
#include <opent1/core/res_manager.hpp>
#include <opent1/game/ban_list.hpp>
#include <opent1/sim/sim_world_game.hpp>
#include <filesystem>
#include <memory>

namespace opent1::game {

class FilePlugin final : public sim::SimConsolePlugin {
public:
    explicit FilePlugin(std::shared_ptr<ResourceManager> resources = {},
                        std::shared_ptr<BanList> banList = {},
                        std::filesystem::path writeRoot = {});
    ~FilePlugin() override = default;

    void init(Console& console, sim::SimManager* manager) override;
    void shutdown() noexcept override;

    void setManager(sim::SimManager* manager) override {
        manager_ = manager;
    }

    [[nodiscard]] ResourceManager& resources() noexcept {
        return *resources_;
    }

    [[nodiscard]] const ResourceManager& resources() const noexcept {
        return *resources_;
    }

    [[nodiscard]] BanList& banList() noexcept {
        return *banList_;
    }

    [[nodiscard]] const BanList& banList() const noexcept {
        return *banList_;
    }

    void setWriteRoot(std::filesystem::path root) {
        writeRoot_ = std::move(root);
    }

    [[nodiscard]] const std::filesystem::path& writeRoot() const noexcept {
        return writeRoot_;
    }

private:
    std::shared_ptr<ResourceManager> resources_;
    std::shared_ptr<BanList> banList_;
    std::filesystem::path writeRoot_;
    Console* console_{};
    sim::SimManager* manager_{};
    std::vector<std::string> findMatches_;
    std::size_t findIndex_{};
};

} // namespace opent1::game
