#include "opent1/game/server_irc_controls.hpp"

namespace opent1::game {

void FGIRCTopic::syncFromChannel(const IRCClient::Channel& channel) {
    text = channel.topic;
}

void FGIRCTopic::tick(double time) {
    Parent::tick(time);
    if (auto* target = resolveClient ? resolveClient() : nullptr) {
        if (const auto* channel = target->currentChannel()) {
            syncFromChannel(*channel);
        }
    }
}
} // namespace opent1::game
