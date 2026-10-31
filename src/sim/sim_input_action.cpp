#include <opent1/sim/sim_input_action.hpp>
#include <opent1/core/res_manager.hpp>
#include <algorithm>
#include <cmath>

namespace opent1::sim {
namespace {
constexpr std::array<InputKeyDescription, 222> keys{{
#include "input_keys.inc"
}};

bool validInput(const SimInputEvent& e) {
    return e.deviceType < 32 && e.action >= InputMake && e.action <= InputMove && e.objType >= 1 &&
           e.objType <= 17 && (e.modifier & ~UInt8{63}) == 0 && std::isfinite(e.fValue);
}

bool validAction(const SimActionEvent& a, UInt32 flags) {
    return flags <= 255 && std::isfinite(a.fValue) && std::isfinite(a.deadZone) &&
           std::isfinite(a.scale) &&
           (!(flags & ActionDeadZone) || (a.deadZone >= 0 && a.deadZone < 1)) &&
           (!a.consoleCommand || (a.consoleCommand->size() <= 1024 * 1024 &&
                                  a.consoleCommand->find('\0') == std::string::npos));
}

bool genericModifier(const SimInputEvent& a) {
    return a.deviceType == InputKeyboard && a.objType == InputKey && a.objInst == 0 && a.modifier &&
           (a.action == InputMake || a.action == InputBreak);
}

bool systemMatch(const SimInputEvent& a, const SimInputEvent& b, bool action, bool bound) {
    if (a.deviceType != b.deviceType || a.deviceInst != b.deviceInst || a.objType != b.objType ||
        (action && a.action != b.action)) {
        return false;
    }
    if (bound) {
        return a.objInst == 0 && b.objInst == 0 && a.modifier == b.modifier;
    }
    switch (a.modifier) {
        case InputShift:
            return b.objInst == 0x2a || b.objInst == 0x36;
        case InputCtrl:
            return b.objInst == 0x1d || b.objInst == 0x9d;
        case InputAlt:
            return b.objInst == 0x38 || b.objInst == 0xb8;
        default:
            return false;
    }
}

bool inputMatch(const SimInputEvent& a, const SimInputEvent& b, bool bound) {
    if (genericModifier(a)) {
        return systemMatch(a, b, true, bound);
    }
    return a.deviceType == b.deviceType && a.deviceInst == b.deviceInst && a.objType == b.objType &&
           a.action == b.action && a.objInst == b.objInst &&
           (a.deviceType != InputKeyboard || SimActionMap::compareModifiers(a, b));
}

std::string lower(std::string_view text) {
    std::string value(text);
    std::ranges::transform(value, value.begin(), [](unsigned char c) {
        return char(c >= 'A' && c <= 'Z' ? c + 32 : c);
    });
    return value;
}

UInt8 modifierKey(UInt8 code) {
    switch (code) {
        case 0x2a:
            return InputLShift;
        case 0x36:
            return InputRShift;
        case 0x1d:
            return InputLCtrl;
        case 0x9d:
            return InputRCtrl;
        case 0x38:
            return InputLAlt;
        case 0xb8:
            return InputRAlt;
        default:
            return 0;
    }
}
} // namespace

const InputKeyDescription* inputKeyDescription(UInt32 code) {
    return code < keys.size() ? &keys[code] : nullptr;
}

UInt8 inputAscii(UInt32 code, UInt8 modifiers) {
    const auto* key = inputKeyDescription(code);
    return key ? ((modifiers & InputShift) ? key->upper : key->lower) : 0;
}

UInt8 inputKeyCode(std::string_view name) {
    const auto value = lower(name);
    // Legacy named/character lookup excludes DIK_LAST (the app key).
    const auto found = std::ranges::find_if(keys.begin(), keys.end() - 1, [&](const auto& key) {
        return !key.name.empty() && key.name == value;
    });
    return found == keys.end() - 1 ? 0 : static_cast<UInt8>(found - keys.begin());
}

UInt8 inputKeyCode(char ascii) {
    const auto code = static_cast<UInt8>(ascii);
    const auto found = std::ranges::find_if(keys.begin(), keys.end() - 1, [&](const auto& key) {
        return key.lower == code || key.upper == code;
    });
    return found == keys.end() - 1 ? 0 : static_cast<UInt8>(found - keys.begin());
}

std::optional<SimInputEvent> parseInputKey(std::string_view text) {
    if (text.size() > 255 || text.find('\0') != std::string_view::npos) {
        return {};
    }
    SimInputEvent event;
    const auto space = [](char c) {
        return c == ' ' || c == '\t' || c == '\r' || c == '\n';
    };
    while (!text.empty()) {
        text.remove_prefix(
            static_cast<std::size_t>(std::ranges::find_if_not(text, space) - text.begin()));
        if (text.empty()) {
            return {};
        }
        auto end = text.find_first_of(" \t\r\n-");
        // '-' itself can be the key, after optional modifiers.
        if (end == 0 && text.front() == '-') {
            end = 1;
        }
        if (end == std::string_view::npos) {
            end = text.size();
        }
        const auto token = text.substr(0, end);
        const auto value = lower(token);
        text.remove_prefix(end);
        if (value == "make") {
            event.action = InputMake;
            continue;
        }
        if (value == "break") {
            event.action = InputBreak;
            continue;
        }
        if (!text.empty() && text.front() == '-' &&
            (value == "shift" || value == "ctrl" || value == "alt")) {
            event.modifier |= value == "shift"  ? InputShift
                              : value == "ctrl" ? InputCtrl
                                                : InputAlt;
            text.remove_prefix(1);
            continue;
        }
        if (value == "shift" || value == "ctrl" || value == "alt") {
            event.modifier |= value == "shift"  ? InputShift
                              : value == "ctrl" ? InputCtrl
                                                : InputAlt;
        } else if (token.size() == 1) {
            event.objInst = inputKeyCode(token.front());
            event.ascii = static_cast<UInt8>(token.front());
            if (token.front() >= 'A' && token.front() <= 'Z') {
                event.modifier |= InputShift;
            }
            if (!event.objInst) {
                return {};
            }
        } else {
            event.objInst = inputKeyCode(token);
            if (!event.objInst) {
                return {};
            }
        }
        if (!std::ranges::all_of(text, space)) {
            return {};
        }
        return event;
    }
    return {};
}

Persistent::Error SimInputEvent::write(StreamIO& s, Int32, Int32, Persistent::StoreContext&) const {
    return validInput(*this) && s.write(deviceType) && s.write(deviceInst) && s.write(objType) &&
                   s.write(objInst) && s.write(action) && s.write(ascii) && s.write(modifier) &&
                   s.write(fValue)
               ? Persistent::Error::Ok
               : Persistent::Error::WriteError;
}

Persistent::Error SimInputEvent::read(StreamIO& s, Int32, Int32, Persistent::LoadContext&) {
    SimInputEvent value;
    if (!s.read(value.deviceType) || !s.read(value.deviceInst) || !s.read(value.objType) ||
        !s.read(value.objInst) || !s.read(value.action) || !s.read(value.ascii) ||
        !s.read(value.modifier) || !s.read(value.fValue) || !validInput(value)) {
        return Persistent::Error::ReadError;
    }
    *this = value;
    return Persistent::Error::Ok;
}

Persistent::Error
SimActionEvent::write(StreamIO& s, Int32, Int32, Persistent::StoreContext&) const {
    // Original ACEV persists only these two words, never native pointers/device.
    return std::isfinite(fValue) && s.write(action) && s.write(fValue)
               ? Persistent::Error::Ok
               : Persistent::Error::WriteError;
}

Persistent::Error SimActionEvent::read(StreamIO& s, Int32, Int32, Persistent::LoadContext&) {
    SimActionEvent value;
    if (!s.read(value.action) || !s.read(value.fValue) || !std::isfinite(value.fValue)) {
        return Persistent::Error::ReadError;
    }
    *this = std::move(value);
    return Persistent::Error::Ok;
}

bool SimActionMap::compareModifiers(const SimInputEvent& a, const SimInputEvent& b) {
    return std::ranges::all_of(std::array{0, 2, 4}, [&](int shift) {
        const auto x = (a.modifier >> shift) & 3;
        const auto y = (b.modifier >> shift) & 3;
        return x == y || (x != 0 && y != 0 && (x == 3 || y == 3));
    });
}

bool SimActionMap::isMakeBreakPair(const SimInputEvent& a, const SimInputEvent& b) {
    if (a.action != InputMake || b.action != InputBreak) {
        return false;
    }
    if (genericModifier(a)) {
        return systemMatch(a, b, false, false);
    }
    return a.deviceType == b.deviceType && a.deviceInst == b.deviceInst && a.objType == b.objType &&
           a.objInst == b.objInst;
}

const SimActionMap::Node* SimActionMap::findMatch(const SimInputEvent& event, bool bound) const {
    const auto found = std::ranges::find_if(nodes_, [&](const auto& node) {
        return inputMatch(node.event, event, bound);
    });
    return found == nodes_.end() ? nullptr : &*found;
}

bool SimActionMap::unmapEvent(const SimInputEvent& event) {
    const auto found = std::ranges::find_if(nodes_, [&](const auto& node) {
        return inputMatch(node.event, event, true);
    });
    if (found == nodes_.end()) {
        return false;
    }
    nodes_.erase(found);
    return true;
}

bool SimActionMap::mapEvent(const SimInputEvent& event,
                            const SimActionEvent& action,
                            UInt32 flags) {
    if (!validInput(event) || !validAction(action, flags)) {
        return false;
    }
    const auto* old = findMatch(event, true);
    if (nodes_.size() >= MaxBindings && !old) {
        return false;
    }
    Node replacement{flags, action, event}; // copy before changing live bindings
    nodes_.reserve(nodes_.size() + (old ? 0 : 1));
    unmapEvent(replacement.event);
    nodes_.push_back(std::move(replacement));
    return true;
}

std::optional<SimActionEvent> SimActionMap::transform(const Node& node,
                                                      const SimInputEvent& event) {
    if (!validInput(event) || !validAction(node.action, node.flags)) {
        return {};
    }
    auto result = node.action;
    result.device =
        event.action != InputMove        ? ActionDigital
        : event.deviceType == InputMouse ? ActionDelta
        : (event.objType == InputPov || event.objType == InputXPov || event.objType == InputYPov)
            ? ActionDigital | ActionPov
            : ActionAnalog;
    auto& value = result.fValue;
    if (!(node.flags & ActionValue)) {
        value = event.fValue;
        if ((node.flags & ActionCenter) && event.deviceType != InputMouse) {
            value = 1.f - value * 2.f;
        }
        if (node.flags & ActionDeadZone) {
            if (std::abs(value) < node.action.deadZone) {
                value = 0;
            } else {
                value += value > 0 ? -node.action.deadZone : node.action.deadZone;
                value = static_cast<float>(value * (1.0 / (1.0 - node.action.deadZone)));
            }
        }
        if (node.flags & ActionFlip) {
            value = !(node.flags & ActionCenter) && event.deviceType != InputMouse ? 1.f - value
                                                                                   : -value;
        }
        if (node.flags & ActionSquare) {
            const bool negative = value < 0;
            value *= value;
            if (negative) {
                value = -value;
            }
        }
        if (node.flags & ActionCube) {
            value = value * value * value;
        }
        if (node.flags & ActionScale) {
            value *= node.action.scale;
        }
    }
    if (!std::isfinite(value)) {
        return {};
    }
    return result;
}

Persistent::Error
SimActionMap::write(StreamIO& s, Int32 v, Int32 u, Persistent::StoreContext& context) const {
    if (nodes_.size() > MaxBindings || !s.write(static_cast<Int32>(nodes_.size()))) {
        return Persistent::Error::WriteError;
    }
    return std::ranges::all_of(
               nodes_,
               [&](const auto& node) {
                   return node.event.write(s, v, u, context) == Persistent::Error::Ok &&
                          node.action.write(s, v, u, context) == Persistent::Error::Ok &&
                          s.write(node.flags);
               })
               ? Persistent::Error::Ok
               : Persistent::Error::WriteError;
}

Persistent::Error
SimActionMap::read(StreamIO& s, Int32 v, Int32 u, Persistent::LoadContext& context) {
    Int32 count{};
    if (!s.read(count) || count < 0 || count > static_cast<Int32>(MaxBindings)) {
        return Persistent::Error::ReadError;
    }
    SimActionMap replacement;
    if (!std::ranges::all_of(std::views::iota(Int32{0}, count), [&](Int32) {
            Node node;
            return node.event.read(s, v, u, context) == Persistent::Error::Ok &&
                   node.action.read(s, v, u, context) == Persistent::Error::Ok &&
                   s.read(node.flags) && replacement.mapEvent(node.event, node.action, node.flags);
        })) {
        return Persistent::Error::ReadError;
    }
    nodes_ = std::move(replacement.nodes_);
    return Persistent::Error::Ok;
}

bool SimInputConsumerSet::processEvent(const SimEvent& event) {
    if (!dynamic_cast<const SimInputEvent*>(&event)) {
        return SimSet::processEvent(event);
    }
    bool handled = false;
    auto broadcast = [&] {
        struct Consumer {
            SimObject* object;
            Int32 priority;
        };
        std::vector<Consumer> consumers;
        const auto snapshot = objects_;
        std::ranges::for_each(snapshot, [&](auto* object) {
            if (isRemoved() || std::ranges::find(objects_, object) == objects_.end() ||
                object->isRemoved()) {
                return;
            }
            SimInputPriorityQuery query;
            object->processQuery(query);
            if (std::ranges::find(objects_, object) != objects_.end() && !object->isRemoved()) {
                consumers.push_back({object, query.priority});
            }
        });
        std::stable_sort(consumers.begin(), consumers.end(), [](const auto& a, const auto& b) {
            return a.priority > b.priority;
        });
        std::ranges::for_each(consumers, [&](const auto& consumer) {
            if (!handled && !isRemoved() &&
                std::ranges::find(objects_, consumer.object) != objects_.end() &&
                !consumer.object->isRemoved()) {
                handled = consumer.object->processEvent(event);
            }
        });
    };
    if (getManager()) {
        getManager()->dispatch(broadcast);
    } else {
        broadcast();
    }
    return handled;
}

bool SimActionHandler::onAdd() {
    addToSet(InputConsumerSetId);
    return true;
}

void SimActionHandler::onRemove() {
    bindings_.clear();
    held_.clear();
}

void SimActionHandler::onDeleteNotify(SimObject& object) {
    // Removed targets cannot receive releases; don't run callbacks from teardown.
    std::erase_if(held_, [&](const auto& make) {
        const auto owner = std::ranges::find_if(bindings_, [&](const auto& binding) {
            return binding.serial == make.owner;
        });
        return owner != bindings_.end() && owner->target.objectId == object.getId() &&
               owner->target.generation == object.generation();
    });
    std::erase_if(bindings_, [&](const auto& binding) {
        return binding.target.objectId == object.getId() &&
               binding.target.generation == object.generation();
    });
    if (bindings_.empty()) {
        held_.clear();
    }
}

bool SimActionHandler::processQuery(SimQuery& query) {
    return dynamic_cast<SimInputPriorityQuery*>(&query) != nullptr;
}

bool SimActionHandler::hasMake(const SimInputEvent& event) const {
    return std::ranges::any_of(held_, [&](const auto& make) {
        return SimActionMap::isMakeBreakPair(make.event, event);
    });
}

bool SimActionHandler::route(SimInputEvent event, UInt64* acceptedBinding) {
    const auto snapshot = bindings_;
    do {
        const bool handled = std::ranges::any_of(snapshot, [&](const auto& binding) {
            if (isRemoved()) {
                return false;
            }
            if (!std::ranges::any_of(bindings_, [&](const auto& live) {
                    return live.serial == binding.serial;
                })) {
                return false;
            }
            auto* target = binding.target.resolve(*getManager());
            if (!target) {
                return false;
            }
            const auto* node = binding.map->findMatch(event);
            if (!node) {
                return false;
            }
            auto action = SimActionMap::transform(*node, event);
            if (!action) {
                return false;
            }
            action->time = getManager()->getCurrentTime();
            if (action->consoleCommand) {
                auto* console = getManager()->console() ? getManager()->console() : console_;
                if (console) {
                    (void)console->evaluate(*action->consoleCommand);
                    if (acceptedBinding) {
                        *acceptedBinding = binding.serial;
                    }
                    return true;
                }
                return false;
            }
            const bool accepted = getManager()->sendEvent(binding.target, *action);
            if (accepted && acceptedBinding) {
                *acceptedBinding = binding.serial;
            }
            return accepted;
        });
        if (handled || isRemoved()) {
            return handled;
        }
        if (event.action == InputMove || event.modifier == 0) {
            break;
        }
        event.modifier = 0; // unmodified bindings are the legacy fallback
    } while (true);
    return false;
}

bool SimActionHandler::input(SimInputEvent event) {
    if (!validInput(event) || !getManager() || isRemoved()) {
        return false;
    }
    if (event.action == InputMake) {
        auto release = event;
        release.action = InputBreak;
        if (hasMake(release) || held_.size() >= 4096) {
            return false;
        } // SDL repeats don't make held duplicates
        held_.push_back({event, 0});
    } else if (event.action == InputBreak) {
        auto found = std::ranges::find_if(held_, [&](const auto& make) {
            return SimActionMap::isMakeBreakPair(make.event, event);
        });
        if (found == held_.end()) {
            return false;
        }
        event.modifier = found->event.modifier;
        held_.erase(found);
    }
    UInt64 accepted{};
    const bool handled = route(event, &accepted);
    if (event.action == InputMake && accepted) {
        auto release = event;
        release.action = InputBreak;
        const auto held = std::ranges::find_if(held_, [&](const auto& make) {
            return SimActionMap::isMakeBreakPair(make.event, release);
        });
        if (held != held_.end()) {
            if (std::ranges::any_of(bindings_, [&](const auto& binding) {
                    return binding.serial == accepted;
                })) {
                held->owner = accepted;
            } else {
                held_.erase(held);
            }
        }
    }
    return handled;
}

bool SimActionHandler::processEvent(const SimEvent& event) {
    const auto* inputEvent = dynamic_cast<const SimInputEvent*>(&event);
    if (!inputEvent || !getManager()) {
        return false;
    }
    bool result = false;
    getManager()->dispatch([&] {
        result = input(*inputEvent);
    });
    return result;
}

void SimActionHandler::breakKey(const SimInputEvent& make) {
    auto event = make;
    event.action = InputBreak;
    event.fValue = 0;
    input(event);
}

void SimActionHandler::releaseDevice(UInt8 type, UInt32 instance) {
    if (!getManager()) {
        return;
    }
    getManager()->dispatch([&] {
        const auto snapshot = held_;
        std::ranges::for_each(snapshot | std::views::reverse, [&](const auto& event) {
            if (!isRemoved() && event.event.deviceType == type &&
                event.event.deviceInst == instance) {
                breakKey(event.event);
            }
        });
    });
}

void SimActionHandler::breakAllMadeKeys() {
    if (!getManager()) {
        held_.clear();
        return;
    }
    getManager()->dispatch([&] {
        const auto snapshot = held_;
        std::ranges::for_each(snapshot | std::views::reverse, [&](const auto& event) {
            if (!isRemoved()) {
                breakKey(event.event);
            }
        });
    });
}

bool SimActionHandler::push(SimObject& target, std::shared_ptr<const SimActionMap> map) {
    if (!getManager() || target.getManager() != getManager() || target.isRemoved() || !map ||
        changing_ || bindings_.size() >= 4096) {
        return false;
    }
    bool success = false;
    getManager()->dispatch([&] {
        changing_ = true;
        try {
            const auto snapshot = held_;
            std::ranges::for_each(snapshot, [&](const auto& make) {
                if (!isRemoved() && map->findMatch(make.event)) {
                    breakKey(make.event);
                }
            });
            if (!isRemoved() && !target.isRemoved()) {
                std::erase_if(bindings_, [&](const auto& binding) {
                    return binding.target.objectId == target.getId() && binding.map == map;
                });
                bindings_.insert(bindings_.begin(),
                                 {++nextSerial_, target.address(), std::move(map)});
                deleteNotify(target);
                success = true;
            }
        } catch (...) {
            changing_ = false;
            throw;
        }
        changing_ = false;
    });
    return success;
}

void SimActionHandler::pop(SimObject& target, const SimActionMap* map) {
    if (!getManager() || target.getManager() != getManager() || changing_) {
        return;
    }
    const auto address = target.address();
    getManager()->dispatch([&] {
        changing_ = true;
        const auto matches = [&](const auto& binding) {
            return binding.target.objectId == address.objectId &&
                   binding.target.generation == address.generation &&
                   (!map || binding.map.get() == map);
        };
        try {
            const auto held = held_;
            std::ranges::for_each(held, [&](const auto& make) {
                if (isRemoved()) {
                    return;
                }
                const auto owner = std::ranges::find_if(bindings_, [&](const auto& binding) {
                    return make.owner ? binding.serial == make.owner
                                      : binding.map->findMatch(make.event) != nullptr;
                });
                if (owner != bindings_.end() && matches(*owner)) {
                    breakKey(make.event);
                }
            });
            std::erase_if(bindings_, matches);
            if (!std::ranges::any_of(bindings_, [&](const auto& binding) {
                    return binding.target.objectId == address.objectId &&
                           binding.target.generation == address.generation;
                })) {
                clearNotify(target);
            }
        } catch (...) {
            changing_ = false;
            throw;
        }
        changing_ = false;
    });
}

bool SimInputManager::submit(SimInputEvent event) {
    auto* manager = getManager();
    if (!manager || isRemoved() || !validInput(event)) {
        return false;
    }
    bool result = false;
    manager->dispatch([&] {
        UInt8 modifier = 0;
        if (event.deviceType == InputKeyboard && event.deviceInst == 0 &&
            event.objType == InputKey) {
            modifier = modifierKey(event.objInst);
            if (event.action == InputMake) {
                modifiers_ |= modifier;
            } else if (event.action == InputBreak) {
                modifiers_ &= static_cast<UInt8>(~modifier);
            }
        }
        event.modifier = modifier ? 0 : modifiers_;
        if (event.objType == InputKey) {
            event.ascii = inputAscii(event.objInst, modifiers_);
        }
        // A matching release bypasses filters even when UI focus changed.
        auto* handler = dynamic_cast<SimActionHandler*>(manager->findObject(ActionHandlerId));
        if (event.action == InputBreak && handler && handler->hasMake(event)) {
            result = manager->sendEvent(handler->address(), event);
        } else if (auto* consumer = manager->findObject(InputConsumerSetId)) {
            result = manager->sendEvent(consumer->address(), event);
        }
    });
    return result;
}

bool SimInputManager::processEvent(const SimEvent& event) {
    const auto* input = dynamic_cast<const SimInputEvent*>(&event);
    return input && submit(*input);
}

void SimInputManager::releaseDevice(UInt8 type, UInt32 instance) {
    if (type == InputKeyboard && instance == 0) {
        modifiers_ = 0;
    }
    if (auto* manager = getManager()) {
        manager->dispatch([&] {
            if (auto* handler =
                    dynamic_cast<SimActionHandler*>(manager->findObject(ActionHandlerId))) {
                handler->releaseDevice(type, instance);
            }
        });
    }
}

void SimInputManager::release() {
    modifiers_ = 0;
    if (auto* manager = getManager()) {
        manager->dispatch([&] {
            if (auto* handler =
                    dynamic_cast<SimActionHandler*>(manager->findObject(ActionHandlerId))) {
                handler->breakAllMadeKeys();
            }
        });
    }
}

void SimInputManager::onRemove() {
    release();
}

bool registerSimInputClasses(Persistent::ClassRegistry& registry) {
    return registry.registerClass<SimInputEvent>("SimInputEvent", makeFourCC('S', 'I', 'E', 'V')) &&
           registry.registerClass<SimActionEvent>(
               "SimActionEvent", makeFourCC('A', 'C', 'E', 'V'), ActionEventType) &&
           registry.registerClass<SimActionMap>("SimActionMap", makeFourCC('A', 'C', 'M', 'A'));
}

bool registerSimActionMapResource(ResourceManager& resources) {
    return resources.registerLoader<SimActionMap>(
        ".sae", [](StreamIO& stream, UInt32) -> std::unique_ptr<SimActionMap> {
            Persistent::ClassRegistry registry;
            registerSimInputClasses(registry);
            Persistent::LoadContext context(registry);
            auto loaded = context.load(stream);
            auto* map = loaded ? dynamic_cast<SimActionMap*>(loaded.object) : nullptr;
            return map ? std::make_unique<SimActionMap>(*map) : nullptr;
        });
}
} // namespace opent1::sim
