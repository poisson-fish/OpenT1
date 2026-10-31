#include "test_support.hpp"
#include "test_registry.hpp"
#include <opent1/game/fear_mission_editor.hpp>

using namespace opent1;

namespace {
void consoleOptions() {
    Console console;
    sim::SimManager manager;
    manager.setConsole(&console);
    auto* editor = static_cast<game::FearMissionEditor*>(
        manager.addObject(std::make_unique<game::FearMissionEditor>()));
    CHECK(editor);
    constexpr auto mask = game::FearMissionEditor::SnapToGrid |
                          game::FearMissionEditor::ConstrainX |
                          game::FearMissionEditor::ShowHandles;
    for (const auto truth : {"1", "true", "True", "TRUE"}) {
        for (const auto name : {"ME::SnapToGrid", "ME::ConstrainX", "ME::ShowHandles"}) {
            console.setVariable(name, truth);
        }
        editor->getConsoleOptions();
        CHECK((editor->flags() & mask) == mask);
        console.setVariable("ME::SnapToGrid", "false");
        console.setVariable("ME::ConstrainX", "0");
        console.setVariable("ME::ShowHandles", "");
        editor->getConsoleOptions();
        CHECK((editor->flags() & mask) == 0);
    }
}

int missionEditorOptions() {
    return test::run(consoleOptions);
}
}

OPENT1_REGISTER_TEST("game", "mission_editor_options", missionEditorOptions);
