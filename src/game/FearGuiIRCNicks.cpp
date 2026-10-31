#include "opent1/game/server_irc_controls.hpp"

#include <algorithm>
#include <cctype>

namespace opent1::game {

namespace {

bool caseInsensitiveNickLess(std::string_view a, std::string_view b) noexcept {
    const std::size_t n = std::min(a.size(), b.size());
    for (std::size_t i = 0; i < n; ++i) {
        const auto ca = static_cast<unsigned char>(std::tolower(static_cast<unsigned char>(a[i])));
        const auto cb = static_cast<unsigned char>(std::tolower(static_cast<unsigned char>(b[i])));
        if (ca != cb) {
            return ca < cb;
        }
    }
    return a.size() < b.size();
}

} // namespace

void FGIRCNicks::syncFromChannel(const IRCClient& client, const IRCClient::Channel& channel) {
    nicks_.clear();
    clear();
    const auto people = client.people();
    for (const auto& member : channel.members) {
        if (member.personIndex < people.size()) {
            auto p = people[member.personIndex];
            p.flags |= member.flags;
            nicks_.push_back(std::move(p));
        }
    }
    std::stable_sort(
        nicks_.begin(), nicks_.end(), [](const IRCClient::Person& a, const IRCClient::Person& b) {
            const bool aOp = (a.flags & IRCClient::PERSON_OPERATOR) != 0U;
            const bool bOp = (b.flags & IRCClient::PERSON_OPERATOR) != 0U;
            if (aOp != bOp) {
                return aOp;
            }
            return caseInsensitiveNickLess(a.nick, b.nick);
        });
    for (const auto& p : nicks_) {
        insert(gui::ListEntry{0, p.nick});
    }
}

void FGIRCNicks::tick(double time) {
    Parent::tick(time);
    if (auto* target = resolveClient ? resolveClient() : nullptr) {
        if (const auto* channel = target->currentChannel()) {
            syncFromChannel(*target, *channel);
        }
    }
}
} // namespace opent1::game
