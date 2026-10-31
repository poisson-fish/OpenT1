#include "opent1/game/server_irc_controls.hpp"

#include <algorithm>

namespace opent1::game {

void FGIRCChannelCtrl::syncFromClient(const IRCClient& client) {
    tabs_.clear();
    for (const auto& ch : client.channels()) {
        TabEntry tab{};
        tab.name = ch.name;
        tab.flags = ch.flags;
        tab.width = std::max(64, static_cast<Int32>(ch.name.size()) * 8 + 20);
        tabs_.push_back(std::move(tab));
    }
    if (const auto* current = client.currentChannel()) {
        const auto selectedTab = std::ranges::find(tabs_, current->name, &TabEntry::name);
        selectedChannel_ =
            selectedTab == tabs_.end() ? 0 : static_cast<Int32>(selectedTab - tabs_.begin());
    }
    if (firstChannel_ >= static_cast<Int32>(tabs_.size())) {
        firstChannel_ = std::max(0, static_cast<Int32>(tabs_.size()) - 1);
    }
}

bool FGIRCChannelCtrl::canScrollRight() const noexcept {
    Int32 totalWidth = 0;
    for (std::size_t i = static_cast<std::size_t>(std::max(0, firstChannel_)); i < tabs_.size();
         ++i) {
        totalWidth += tabs_[i].width;
    }
    const Int32 availWidth = std::max(240, fields_.extent.x);
    return totalWidth > availWidth && (firstChannel_ + 1) < static_cast<Int32>(tabs_.size());
}

bool FGIRCChannelCtrl::msgWaitingLeft() const noexcept {
    for (Int32 i = 0; i < firstChannel_ && static_cast<std::size_t>(i) < tabs_.size(); ++i) {
        if ((tabs_[static_cast<std::size_t>(i)].flags & IRCClient::CHANNEL_NEW_MESSAGE) != 0U) {
            return true;
        }
    }
    return false;
}

bool FGIRCChannelCtrl::msgWaitingRight() const noexcept {
    for (std::size_t i = static_cast<std::size_t>(std::max(0, firstChannel_ + 1)); i < tabs_.size();
         ++i) {
        if ((tabs_[i].flags & IRCClient::CHANNEL_NEW_MESSAGE) != 0U) {
            return true;
        }
    }
    return false;
}

void FGIRCChannelCtrl::scrollLeft() noexcept {
    if (canScrollLeft()) {
        --firstChannel_;
    }
}

void FGIRCChannelCtrl::scrollRight() noexcept {
    if (canScrollRight()) {
        ++firstChannel_;
    }
}

void FGIRCChannelCtrl::tick(double time) {
    Parent::tick(time);
    if (auto* target = resolveClient ? resolveClient() : nullptr) {
        syncFromClient(*target);
    }
}

void FGIRCChannelCtrl::selectChannel(Int32 index) {
    if (index < 0 || static_cast<std::size_t>(index) >= tabs_.size()) {
        return;
    }
    selectedChannel_ = index;
    if (auto* target = resolveClient ? resolveClient() : nullptr) {
        target->setCurrentChannel(tabs_[static_cast<std::size_t>(index)].name);
    }
}

bool FGIRCChannelCtrl::event(const gui::Event& eventValue) {
    if (eventValue.type == gui::EventType::Down && eventValue.button == 1) {
        Int32 x = globalToLocal(eventValue.point).x;
        for (Int32 index = firstChannel_; index < static_cast<Int32>(tabs_.size()); ++index) {
            const auto width = tabs_[static_cast<std::size_t>(index)].width;
            if (x >= 0 && x < width) {
                selectChannel(index);
                return true;
            }
            x -= width;
        }
    }
    return Parent::event(eventValue);
}

bool FGIRCChannelCtrl::draw(gfx::SoftwareSurface& surface, Point2I offset, gui::RectI clip) const {
    if (!Parent::draw(surface, offset, clip)) {
        return false;
    }
    Int32 x = offset.x;
    for (Int32 index = firstChannel_;
         index < static_cast<Int32>(tabs_.size()) && x < offset.x + fields_.extent.x;
         ++index) {
        const auto& tab = tabs_[static_cast<std::size_t>(index)];
        surface.rectangle({x, offset.y, x + tab.width - 1, offset.y + fields_.extent.y - 1},
                          index == selectedChannel_ ? 255 : 100);
        if (tabFont && !surface.text(*tabFont, {x + 6, offset.y + 4}, tab.name)) {
            return false;
        }
        if (tab.flags & IRCClient::CHANNEL_NEW_MESSAGE) {
            surface.fill({x + 2, offset.y + 2, x + 4, offset.y + 4}, 255);
        }
        x += tab.width;
    }
    return true;
}
} // namespace opent1::game
