#include <opent1/game/commander_huds.hpp>

namespace opent1::game {

void FGCurrentCommand::setCommandDisplay(std::string_view cmdText, Int32 cmdStatus) {
    text = std::string(cmdText);
    cmdStatus_ = cmdStatus;
}

} // namespace opent1::game
