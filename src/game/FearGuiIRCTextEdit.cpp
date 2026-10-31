#include "opent1/game/server_irc_controls.hpp"

namespace opent1::game {

std::string FGIRCTextEdit::submitToClient(IRCClient& client) {
    const std::string line = text;
    setText({});
    return client.sendInput(line);
}

bool FGIRCTextEdit::event(const gui::Event& eventValue) {
    auto* target = resolveClient ? resolveClient() : nullptr;
    if (target && eventValue.type == gui::EventType::KeyDown) {
        if (eventValue.key == gui::Key::Return) {
            (void)submitToClient(*target);
            return true;
        }
        if (eventValue.key == gui::Key::Up) {
            setText(target->prevHistory());
            return true;
        }
        if (eventValue.key == gui::Key::Down) {
            setText(target->nextHistory());
            return true;
        }
    }
    return Parent::event(eventValue);
}
} // namespace opent1::game
