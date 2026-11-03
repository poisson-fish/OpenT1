#include "opent1/game/server_irc_controls.hpp"
#include <ranges>
#include <tuple>

namespace opent1::game {
namespace {
bool checked(gui::Control& root, UInt32 tag) {
    const auto* button = dynamic_cast<FGUniversalButton*>(root.findTag(tag));
    return button && button->isSet();
}

void check(gui::Control& root, UInt32 tag, bool value) {
    if (auto* button = dynamic_cast<FGUniversalButton*>(root.findTag(tag))) {
        button->setState(value);
    }
}

std::string editText(gui::Control& root, UInt32 tag) {
    const auto* edit = dynamic_cast<gui::TextEdit*>(root.findTag(tag));
    return edit ? edit->text : std::string{};
}

void edit(gui::Control& root, UInt32 tag, std::string_view text) {
    if (auto* value = dynamic_cast<gui::TextEdit*>(root.findTag(tag))) {
        value->setText(std::string(text));
    }
}

void enable(gui::Control& root, UInt32 tag, bool value) {
    if (auto* control = dynamic_cast<gui::Active*>(root.findTag(tag))) {
        control->active = value;
    }
}
}

void FGIRCChannelPropsCtrl::wake() {
    Parent::wake();
    canSetTopic_ = canSetProps_ = false;
    auto* target = resolveClient ? resolveClient() : nullptr;
    const auto* channel = target ? target->currentChannel() : nullptr;
    if (!channel) {
        return;
    }
    loadFromChannel(*channel);
    for (const auto& member : channel->members) {
        if (member.personIndex < target->people().size() &&
            target->people()[member.personIndex].nick == target->nickName()) {
            canSetProps_ = (member.flags & IRCClient::PERSON_OPERATOR) != 0;
            canSetTopic_ = canSetProps_ || !(channel->flags & IRCClient::CHANNEL_TOPIC_LIMITED);
            break;
        }
    }
    edit(*this, 141132, topic_);
    edit(*this, 141134, std::to_string(userLimit_));
    edit(*this, 141136, key_);
    check(*this, 141130, (flags_ & IRCClient::CHANNEL_SECRET) != 0);
    check(*this, 141131, (flags_ & IRCClient::CHANNEL_PRIVATE) != 0);
    check(*this, 141133, userLimit_ > 0);
    check(*this, 141135, !key_.empty());
    check(*this, 141137, (flags_ & IRCClient::CHANNEL_MODERATED) != 0);
    check(*this, 141138, !(flags_ & IRCClient::CHANNEL_TOPIC_LIMITED));
    check(*this, 141139, (flags_ & IRCClient::CHANNEL_INVITE) != 0);
    for (auto tag : std::views::iota(UInt32{141130}) | std::views::take_while([&](UInt32 tag) {
                        return tag <= 141139;
                    })) {
        enable(*this, tag, canSetProps_);
    }
    enable(*this, 141132, canSetTopic_);
    enable(*this, 141134, canSetProps_ && userLimit_ > 0);
    enable(*this, 141136, canSetProps_ && !key_.empty());
}

bool FGIRCChannelPropsCtrl::message(gui::Control& sender, UInt32 value) {
    if (value == 141133) {
        enable(*this, 141134, canSetProps_ && checked(*this, 141133));
    }
    if (value == 141135) {
        enable(*this, 141136, canSetProps_ && checked(*this, 141135));
    }
    if (value == 141130 && checked(*this, 141130)) {
        check(*this, 141131, false);
    }
    if (value == 141131 && checked(*this, 141131)) {
        check(*this, 141130, false);
    }
    if (value == 110001) {
        auto* target = resolveClient ? resolveClient() : nullptr;
        const auto* channel = target ? target->currentChannel() : nullptr;
        if (!channel || channel->name.empty() ||
            (channel->name.front() != '#' && channel->name.front() != '&')) {
            return false;
        }
        const std::string name = channel->name;
        const auto oldFlags = channel->flags;
        const auto oldKey = channel->key;
        const auto oldLimit = channel->personLimit;
        const auto topic = editText(*this, 141132);
        if (canSetTopic_ && topic != channel->topic) {
            (void)target->sendInput("/TOPIC " + name + " :" + topic);
        }
        if (canSetProps_) {
            const auto number = script::toInteger(editText(*this, 141134));
            const auto limit =
                checked(*this, 141133) && number > 0 ? static_cast<UInt32>(number) : 0;
            if (limit != oldLimit) {
                (void)target->sendInput("/MODE " + name +
                                        (limit ? " +l " + std::to_string(limit) : " -l"));
            }
            const auto key = checked(*this, 141135) ? editText(*this, 141136) : std::string{};
            if (oldKey != key) {
                if (!oldKey.empty()) {
                    (void)target->sendInput("/MODE " + name + " -k " + oldKey);
                }
                if (!key.empty()) {
                    (void)target->sendInput("/MODE " + name + " +k " + key);
                }
            }
            std::string modes;
            for (const auto& [tag, flag, letter, invert] :
                 std::to_array<std::tuple<UInt32, UInt32, char, bool>>(
                     {{141137, IRCClient::CHANNEL_MODERATED, 'm', false},
                      {141138, IRCClient::CHANNEL_TOPIC_LIMITED, 't', true},
                      {141139, IRCClient::CHANNEL_INVITE, 'i', false},
                      {141130, IRCClient::CHANNEL_SECRET, 's', false},
                      {141131, IRCClient::CHANNEL_PRIVATE, 'p', false}})) {
                const bool desired = checked(*this, tag) != invert;
                if (desired != ((oldFlags & flag) != 0)) {
                    modes += desired ? '+' : '-';
                    modes += letter;
                }
            }
            if (!modes.empty()) {
                (void)target->sendInput("/MODE " + name + " " + modes);
            }
        }
    }
    return Parent::message(sender, value);
}

void FGIRCChannelPropsCtrl::loadFromChannel(const IRCClient::Channel& channel) {
    topic_ = channel.topic;
    flags_ = channel.flags;
    userLimit_ = channel.personLimit;
    key_ = channel.key;
}

void FGIRCChannelPropsCtrl::applyToChannel(IRCClient::Channel& channel) const {
    channel.topic = topic_;
    channel.flags = flags_;
    channel.personLimit = userLimit_;
    channel.key = key_;
}

} // namespace opent1::game
