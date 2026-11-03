#include <opent1/game/chat_menu.hpp>
#include <ranges>
#include <algorithm>
#include <charconv>

namespace opent1::game {

namespace {
[[nodiscard]] std::string truncateHeading(std::string_view text) {
    const std::size_t len =
        std::min<std::size_t>(text.size(), static_cast<std::size_t>(ChatMenu::MaxHeadingLen));
    return std::string(text.substr(0, len));
}
} // namespace

ChatMenu::ChatMenu() = default;

ChatMenu::ChatMenu(std::string_view initialHeading) {
    setHeading(initialHeading);
}

ChatMenu::~ChatMenu() {
    clearMenu();
}

void ChatMenu::clearMenu() {
    chatTree_.reset();
    curMenu_ = nullptr;
    setMode(Inactive);
}

void ChatMenu::setHeading(std::string_view newHeading) {
    heading_ = truncateHeading(newHeading);
}

bool ChatMenu::addMenuString(std::string_view menuString, std::span<const std::string_view> argv) {
    std::unique_ptr<Node>* curTreePtr = &chatTree_;
    std::string_view remaining = menuString;

    for (;;) {
        if (remaining.empty()) {
            curMenu_ = chatTree_.get();
            return false;
        }
        const char key = remaining.front();
        remaining.remove_prefix(1);

        const std::size_t slashPos = remaining.find('\\');
        const bool isLeaf = (slashPos == std::string_view::npos);
        const std::string_view rawHeading = isLeaf ? remaining : remaining.substr(0, slashPos);
        if (rawHeading.empty()) {
            curMenu_ = chatTree_.get();
            return false;
        }

        Node* walk = curTreePtr->get();
        while (walk != nullptr) {
            if (walk->key == key) {
                break;
            }
            walk = walk->nextSibling.get();
        }

        if (isLeaf) {
            if (walk != nullptr) {
                curMenu_ = chatTree_.get();
                return false;
            }
            auto newNode = std::make_unique<Node>();
            newNode->key = key;
            newNode->heading = truncateHeading(rawHeading);
            const Int32 count = static_cast<Int32>(
                std::min<std::size_t>(argv.size(), static_cast<std::size_t>(MaxArgs)));
            newNode->argc = count;
            for (auto i : std::views::iota(Int32{0}) | std::views::take_while([&](Int32 i) {
                              return i < count;
                          })) {
                newNode->argv[static_cast<std::size_t>(i)] =
                    std::string(argv[static_cast<std::size_t>(i)]);
            }

            while (*curTreePtr != nullptr) {
                curTreePtr = &((*curTreePtr)->nextSibling);
            }
            *curTreePtr = std::move(newNode);
            curMenu_ = chatTree_.get();
            return true;
        }

        if (walk != nullptr) {
            if (walk->argc > 0) {
                curMenu_ = chatTree_.get();
                return false;
            }
            curTreePtr = &(walk->child);
        } else {
            auto newNode = std::make_unique<Node>();
            newNode->key = key;
            newNode->heading = truncateHeading(rawHeading);
            Node* newNodeRaw = newNode.get();

            while (*curTreePtr != nullptr) {
                curTreePtr = &((*curTreePtr)->nextSibling);
            }
            *curTreePtr = std::move(newNode);
            curTreePtr = &(newNodeRaw->child);
        }

        remaining.remove_prefix(slashPos + 1);
    }
}

bool ChatMenu::addMenuString(std::string_view menuString, std::span<const std::string> argv) {
    std::vector<std::string_view> views;
    views.reserve(argv.size());
    for (const auto& s : argv) {
        views.push_back(s);
    }
    return addMenuString(menuString, views);
}

bool ChatMenu::processKey(char key) {
    bool ret = false;
    Node* walk = curMenu_;
    while (walk != nullptr) {
        if (walk->key == key) {
            if (walk->argc > 0) {
                std::vector<std::string> cmdArgs;
                cmdArgs.reserve(static_cast<std::size_t>(walk->argc));
                for (auto i : std::views::iota(Int32{0}) | std::views::take_while([&](Int32 i) {
                                  return i < walk->argc;
                              })) {
                    const auto& arg = walk->argv[static_cast<std::size_t>(i)];
                    cmdArgs.push_back(arg);
                }
                executedCommands_.push_back(cmdArgs);
                curMenu_ = chatTree_.get();
                if (curMode_ == OneShot) {
                    setMode(Inactive);
                }
                // A server-menu command may delete this ChatMenu. Finish all
                // local state changes before entering the script callback.
                if (boundConsole_ && !cmdArgs.empty()) {
                    (void)boundConsole_->execute(cmdArgs.front(),
                                                 std::span<const std::string>(cmdArgs).subspan(1));
                }
                return true;
            }
            curMenu_ = walk->child.get();
            ret = true;
            break;
        }
        walk = walk->nextSibling.get();
    }
    if (!curMenu_) {
        curMenu_ = chatTree_.get();
        if (curMode_ == OneShot) {
            setMode(Inactive);
        }
    }
    return ret;
}

void ChatMenu::setMode(Int32 mode) {
    if (mode < Inactive || mode > OneShot) {
        return;
    }
    curMode_ = mode;
    curMenu_ = chatTree_.get();
}

bool ChatMenu::processArguments(std::span<const std::string> arguments) {
    if (arguments.size() == 1) {
        setHeading(arguments.front());
        return true;
    }
    return false;
}

bool ChatMenu::onAdd() {
    if (!Parent::onAdd()) {
        return false;
    }
    addToSet(sim::InputConsumerSetId);
    return true;
}

bool ChatMenu::processEvent(const sim::SimEvent& event) {
    if (event.type != sim::InputEventType) {
        return false;
    }
    const auto* ev = dynamic_cast<const sim::SimInputEvent*>(&event);
    if (!ev || curMode_ == Inactive) {
        return false;
    }
    if (ev->deviceType == sim::InputKeyboard && ev->deviceInst == 0 &&
        ev->objType == sim::InputKey && ev->modifier == 0) {
        if (ev->action == sim::InputMake) {
            if (ev->objInst == kEscapeScanCode && curMode_ != Inactive) {
                if (curMode_ == OneShot) {
                    curMenu_ = chatTree_.get();
                    setMode(Inactive);
                    lastKey_ = ev->objInst;
                    return true;
                }
                if (curMenu_ != chatTree_.get()) {
                    curMenu_ = chatTree_.get();
                    lastKey_ = ev->objInst;
                    return true;
                }
            } else {
                lastKey_ = ev->objInst;
                return processKey(static_cast<char>(ev->ascii));
            }
        } else if (ev->action == sim::InputBreak && ev->objInst == lastKey_) {
            lastKey_ = 0;
            return true;
        }
    }
    return false;
}

bool ChatMenu::processQuery(sim::SimQuery& query) {
    if (query.type == sim::InputPriorityQueryType) {
        if (auto* pq = dynamic_cast<sim::SimInputPriorityQuery*>(&query)) {
            pq->priority = kChatMenuInputPriority;
            return true;
        }
    }
    return false;
}

bool registerChatMenuClass(Persistent::ClassRegistry& registry) {
    return registry.registerClass<ChatMenu>("ChatMenu");
}

} // namespace opent1::game
