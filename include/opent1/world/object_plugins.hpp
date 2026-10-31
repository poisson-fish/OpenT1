#pragma once
#include <opent1/sim/sim_world_game.hpp>

namespace opent1::world {
enum class ObjectAdapter { Console, Shell, Graphics, Network, Input, Tree, Tool };

struct ObjectCommand {
    std::string_view name;
    UInt16 minimum{}, maximum{};
};

[[nodiscard]] std::span<const ObjectCommand> objectCommands(ObjectAdapter);

// OS, device, transport and native-dialog capabilities are supplied explicitly.
// The adapter owns installation and argv validation; a missing capability fails
// without claiming that a platform action succeeded. argv excludes its name.
struct ObjectHost {
    std::function<std::optional<std::string>(
        Console&, sim::SimManager*, std::string_view, std::span<const std::string>)>
        command;
    std::function<void(sim::SimManager*)> managerChanged;
    std::function<bool(bool, std::string_view)> consoleEnabled;
    std::function<void(std::string_view)> output;
    std::function<std::vector<std::string>()> input;
    std::function<void()> stop;
    std::function<void()> frame;
};

class ObjectPlugin final : public sim::SimConsolePlugin {
public:
    ObjectPlugin(ObjectAdapter, ObjectHost);
    ~ObjectPlugin() override;
    void init(Console&, sim::SimManager*) override;
    void shutdown() noexcept override;
    void setManager(sim::SimManager*) override;
    void startFrame() override;
    [[nodiscard]] bool healthy() const;

private:
    struct State;
    ObjectAdapter kind_;
    ObjectHost host_;
    std::shared_ptr<State> state_;
    std::vector<std::shared_ptr<void>> commands_;
    std::shared_ptr<Console::Output> output_;
};
} // namespace opent1::world
