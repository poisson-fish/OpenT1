#include "opent1/game/server_irc_controls.hpp"
#include <ranges>

namespace opent1::game {
namespace {
std::string folded(std::string_view value) {
    std::string result(value);
    std::ranges::transform(result, result.begin(), [](char c) {
        return c >= 'A' && c <= 'Z' ? char(c + 'a' - 'A') : c;
    });
    return result;
}
}

FGIRCChannelList::FGIRCChannelList() {
    columnInfo_ = {
        ColumnInfo{0, 10, 640, 0, 0, true, 140, "CHANNEL"},
        ColumnInfo{1, 10, 640, 1, 1, true, 60, "USERS"},
        ColumnInfo{2, 10, 640, 2, 2, true, 220, "TOPIC"},
    };
    setSize({3, 0});
}

void FGIRCChannelList::addChannelEntry(std::string_view name,
                                       Int32 userCount,
                                       std::string_view topic) {
    const auto key = folded(name);
    if (std::ranges::any_of(entries_, [&](const auto& e) {
            return folded(e.name) == key;
        })) {
        return;
    }
    entries_.push_back(ChannelListEntry{std::string(name), userCount, std::string(topic)});
    setSize({3, static_cast<Int32>(entries_.size())});
}

void FGIRCChannelList::clearChannelEntries() {
    entries_.clear();
    setSize({3, 0});
}

std::string FGIRCChannelList::getCellText(Point2I cell) const {
    if (cell.y < 0 || static_cast<std::size_t>(cell.y) >= entries_.size()) {
        return {};
    }
    const auto& entry = entries_[static_cast<std::size_t>(cell.y)];
    switch (cell.x) {
        case 0:
            return entry.name;
        case 1:
            return std::to_string(entry.userCount);
        case 2:
            return entry.topic;
        default:
            return {};
    }
}

void FGIRCChannelList::wake() {
    Parent::wake();
    auto* target = resolveClient ? resolveClient() : nullptr;
    const auto* manager = target ? target->getManager() : nullptr;
    if (manager && manager->console()) {
        showAll_ = script::toBool(manager->console()->getVariable("IRC::ShowAllChannels"));
    }
    setShowAll(showAll_);
}

void FGIRCChannelList::setShowAll(bool value) {
    showAll_ = value;
    clearChannelEntries();
    if (value) {
        if (auto* target = resolveClient ? resolveClient() : nullptr) {
            (void)target->sendInput("/list");
        }
    }
    tick(0);
}

void FGIRCChannelList::setFilter(std::string_view value) {
    filter_ = folded(value);
    tick(0);
}

void FGIRCChannelList::tick(double time) {
    Parent::tick(time);
    if (auto* target = resolveClient ? resolveClient() : nullptr) {
        const auto old = selected.y >= 0 && static_cast<std::size_t>(selected.y) < entries_.size()
                             ? entries_[static_cast<std::size_t>(selected.y)].name
                             : std::string{};
        clearChannelEntries();
        if (showAll_ && target->channelListReady()) {
            for (const auto& channel : target->channelList()) {
                addChannelEntry(channel.name, channel.personLimit, channel.topic);
            }
        }
        if (auto* manager = target->getManager(); manager && manager->console()) {
            for (auto i : std::views::iota(unsigned{0}, unsigned{100})) {
                const auto name =
                    manager->console()->getVariable("IRC::ChannelName" + std::to_string(i));
                if (name.empty()) {
                    break;
                }
                addChannelEntry(
                    name,
                    0,
                    manager->console()->getVariable("IRC::ChannelTopic" + std::to_string(i)));
            }
        }
        if (!filter_.empty()) {
            std::erase_if(entries_, [&](const auto& entry) {
                return folded(entry.name).find(filter_) == std::string::npos;
            });
        }
        std::ranges::sort(entries_, {}, [](const auto& entry) {
            return folded(entry.name);
        });
        setSize({3, static_cast<Int32>(entries_.size())});
        const auto found = std::ranges::find(entries_, old, &ChannelListEntry::name);
        selected = found == entries_.end()
                       ? Point2I{-1, -1}
                       : Point2I{0, static_cast<Int32>(found - entries_.begin())};
    }
}

bool FGIRCChannelList::event(const gui::Event& eventValue) {
    const bool handled = Parent::event(eventValue);
    if (handled && selected.y >= 0 && static_cast<std::size_t>(selected.y) < entries_.size()) {
        if (auto* edit = dynamic_cast<gui::TextEdit*>(findRootTag(141126))) {
            edit->setText(entries_[static_cast<std::size_t>(selected.y)].name);
        }
        if (eventValue.type == gui::EventType::Down && eventValue.clicks > 1) {
            if (auto* target = resolveClient ? resolveClient() : nullptr) {
                (void)target->sendInput("/join " +
                                        entries_[static_cast<std::size_t>(selected.y)].name);
            }
        }
    }
    return handled;
}
} // namespace opent1::game
