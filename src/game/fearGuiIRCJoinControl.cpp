#include "opent1/game/server_irc_controls.hpp"

namespace opent1::game {

// FGIRCJoinCtrl manages channel join name and optional key.

void FGIRCJoinCtrl::wake() {
    Parent::wake();
    if (auto* target = resolveClient ? resolveClient() : nullptr) {
        const auto* manager = target->getManager();
        const auto* console = manager ? manager->console() : nullptr;
        if (console) {
            if (auto* button = dynamic_cast<FGUniversalButton*>(findTag(141128))) {
                button->setState(script::toBool(console->getVariable("IRC::ShowAllChannels")));
            }
        }
    }
}

void FGIRCJoinCtrl::tick(double time) {
    Parent::tick(time);
    auto* list = dynamic_cast<FGIRCChannelList*>(findTag(141125));
    if (list) {
        if (auto* edit = dynamic_cast<gui::TextEdit*>(findTag(141126))) {
            list->setFilter(edit->text);
        }
    }
    if (auto* target = resolveClient ? resolveClient() : nullptr) {
        if (auto* button = dynamic_cast<FGUniversalButton*>(findTag(141128))) {
            button->active = !button->isSet() || target->channelListReady();
        }
    }
}

bool FGIRCJoinCtrl::message(gui::Control& sender, UInt32 value) {
    if (value == 141128) {
        auto* button = dynamic_cast<FGUniversalButton*>(findTag(141128));
        if (button) {
            if (auto* target = resolveClient ? resolveClient() : nullptr) {
                if (auto* manager = target->getManager(); manager && manager->console()) {
                    manager->console()->setVariable("IRC::ShowAllChannels",
                                                    button->isSet() ? "True" : "False");
                }
            }
            if (auto* list = dynamic_cast<FGIRCChannelList*>(findTag(141125))) {
                list->setShowAll(button->isSet());
            }
        }
    }
    if (value == 110001) {
        if (auto* edit = dynamic_cast<gui::TextEdit*>(findTag(141126))) {
            channelToJoin_ = edit->text;
        }
        if (auto* edit = dynamic_cast<gui::TextEdit*>(findTag(141127))) {
            password_ = edit->text;
        }
        if (channelToJoin_.empty()) {
            return false;
        }
        if (channelToJoin_.front() != '#' && channelToJoin_.front() != '&') {
            channelToJoin_.insert(channelToJoin_.begin(), '#');
        }
        if (auto* target = resolveClient ? resolveClient() : nullptr) {
            if (auto* manager = target->getManager(); manager && manager->console()) {
                manager->console()->setVariable("IRCMsg::Channel", channelToJoin_);
                if (!password_.empty()) {
                    manager->console()->setVariable("IRCMsg::Key", password_);
                }
            }
            (void)target->sendInput("/join " + channelToJoin_ +
                                    (password_.empty() ? "" : " " + password_));
        }
    }
    return Parent::message(sender, value);
}
} // namespace opent1::game
