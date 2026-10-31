#include "opent1/game/server_irc_controls.hpp"

namespace opent1::game {

void FGIRCActiveTextFormat::appendChannelMessages(const IRCClient::Channel& channel) {
    lines_.clear();
    std::string combined{};
    for (const auto& msg : channel.messages) {
        lines_.push_back(msg.text);
        if (!combined.empty()) {
            combined.push_back('\n');
        }
        combined += msg.text;
    }
    setScriptValue(combined);
}

void FGIRCActiveTextFormat::tick(double time) {
    Parent::tick(time);
    if (auto* target = resolveClient ? resolveClient() : nullptr) {
        if (const auto* channel = target->currentChannel()) {
            appendChannelMessages(*channel);
        }
    }
}
} // namespace opent1::game
