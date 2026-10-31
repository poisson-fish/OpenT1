#include "opent1/game/server_irc_controls.hpp"

#include <algorithm>

namespace opent1::game {
void FGIRCBanCtrl::wake() {
    Parent::wake();
    ready_ = false;
    banMasks_.clear();
    if (auto* list = dynamic_cast<FGTextList*>(findTag(141113))) {
        list->clear();
        list->active = false;
    }
    for (UInt32 tag : {141110U, 141111U, 141112U}) {
        if (auto* value = dynamic_cast<gui::Active*>(findTag(tag))) {
            value->active = false;
        }
    }
    if (auto* edit = dynamic_cast<gui::TextEdit*>(findTag(141112))) {
        edit->setText({});
    }
    if (auto* target = resolveClient ? resolveClient() : nullptr) {
        if (const auto* channel = target->currentChannel()) {
            (void)target->requestBanList(channel->name);
        }
        if (auto* manager = target->getManager(); manager && manager->console()) {
            const auto nick = manager->console()->getVariable("IRCMsg::BanUser");
            if (!nick.empty()) {
                (void)target->sendInput("/WHO " + nick);
            }
        }
    }
}

void FGIRCBanCtrl::tick(double time) {
    Parent::tick(time);
    auto* target = resolveClient ? resolveClient() : nullptr;
    if (!target || ready_ || !target->banListReady()) {
        return;
    }
    banMasks_ = target->banList();
    if (auto* list = dynamic_cast<FGTextList*>(findTag(141113))) {
        list->clear();
        for (const auto& mask : banMasks_) {
            list->insert({0, mask});
        }
    }
    if (auto* manager = target->getManager(); manager && manager->console()) {
        const auto nick = manager->console()->getVariable("IRCMsg::BanUser");
        if (!nick.empty()) {
            const auto found = std::ranges::find(target->people(), nick, &IRCClient::Person::nick);
            if (found == target->people().end() || found->ident.empty()) {
                return;
            }
            const auto at = found->ident.find('@');
            if (auto* edit = dynamic_cast<gui::TextEdit*>(findTag(141112))) {
                edit->setText(at == std::string::npos ? found->ident
                                                      : "*!*" + found->ident.substr(at));
            }
        }
    }
    ready_ = true;
    for (UInt32 tag = 141110; tag <= 141113; ++tag) {
        if (auto* value = dynamic_cast<gui::Active*>(findTag(tag))) {
            value->active = true;
        }
    }
}

bool FGIRCBanCtrl::message(gui::Control& sender, UInt32 value) {
    auto* list = dynamic_cast<FGTextList*>(findTag(141113));
    auto* edit = dynamic_cast<gui::TextEdit*>(findTag(141112));
    if (value == 141113 && list && edit && list->selection()) {
        edit->setText(list->selection()->text);
    }
    if (ready_ && edit && !edit->text.empty() && (value == 141110 || value == 141111)) {
        if (auto* target = resolveClient ? resolveClient() : nullptr) {
            if (const auto* channel = target->currentChannel()) {
                const auto mask = edit->text;
                const auto found = std::ranges::find(banMasks_, mask);
                if (value == 141110 && found == banMasks_.end()) {
                    (void)target->sendInput("/MODE " + channel->name + " +b " + mask);
                    addBanMask(mask);
                }
                if (value == 141111 && found != banMasks_.end()) {
                    (void)target->sendInput("/MODE " + channel->name + " -b " + mask);
                    (void)removeBanMask(mask);
                }
                if (list) {
                    list->clear();
                    for (const auto& item : banMasks_) {
                        list->insert({0, item});
                    }
                }
            }
        }
    }
    return Parent::message(sender, value);
}

void FGIRCBanCtrl::addBanMask(std::string_view mask) {
    if (!mask.empty()) {
        banMasks_.emplace_back(mask);
    }
}

bool FGIRCBanCtrl::removeBanMask(std::string_view mask) {
    const auto erased = std::erase(banMasks_, std::string(mask));
    return erased > 0;
}

} // namespace opent1::game
