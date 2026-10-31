#pragma once
#include <opent1/sim/sim_input_action.hpp>
#include <opent1/core/res_manager.hpp>

namespace opent1::world {
[[nodiscard]] std::optional<sim::SimInputEvent> parseInputArguments(std::span<const std::string>);

struct InputAction {
    sim::SimActionEvent action;
    UInt32 flags{};
};

[[nodiscard]] std::optional<InputAction> parseActionArguments(std::span<const std::string>);
// Emits executable native bindAction/bindCommand script with escaped strings.
[[nodiscard]] std::optional<std::string> actionMapScript(const sim::SimActionMap&);

class InputMaps {
public:
    bool edit(std::string_view, ResourceManager&, bool clear = false);
    bool bind(std::span<const std::string>, bool command, bool gameMap = false);
    [[nodiscard]] std::shared_ptr<const sim::SimActionMap> find(std::string_view) const;
    bool replace(std::string_view, const sim::SimActionMap&);
    [[nodiscard]] std::optional<std::string> script(std::span<const std::string>) const;

    void reset() {
        maps_.clear();
        current_.clear();
    }

private:
    std::map<std::string, std::shared_ptr<const sim::SimActionMap>> maps_;
    std::string current_;
};
}
