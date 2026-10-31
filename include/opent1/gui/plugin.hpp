#pragma once
#include <opent1/gui/runtime.hpp>
#include <opent1/sim/sim_world_game.hpp>

namespace opent1::gui {
// Installation/removal through SimGame is explicit and repeatable. Retaining
// this plugin after removal does not retain active commands or a manager.
class Plugin final : public sim::SimConsolePlugin {
public:
    explicit Plugin(std::shared_ptr<const Persistent::ClassRegistry>, Resources = {});
    ~Plugin() override;
    void init(Console&, sim::SimManager*) override;
    void setManager(sim::SimManager*) override;
    void shutdown() noexcept override;
    void startFrame() override;
    void endFrame() override;

    [[nodiscard]] std::shared_ptr<Runtime> runtime() const {
        return runtime_;
    }

    [[nodiscard]] bool healthy() const {
        return healthy_;
    }

private:
    std::shared_ptr<const Persistent::ClassRegistry> registry_;
    Resources resources_;
    std::shared_ptr<Runtime> runtime_;
    sim::SimManager *manager_{}, *previousManager_{};
    double previous_{}, elapsed_{};
    bool healthy_{true};
};
} // namespace opent1::gui
