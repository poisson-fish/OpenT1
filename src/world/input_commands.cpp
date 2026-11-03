#include <opent1/world/input_commands.hpp>
#include <iterator>
#include <charconv>
#include <cmath>

namespace opent1::world {
namespace {
std::string lower(std::string_view text) {
    std::string value(text);
    for (auto& c : value) {
        if (c >= 'A' && c <= 'Z') {
            c = char(c - 'A' + 'a');
        }
    }
    return value;
}

template <class T> bool number(std::string_view text, T& result) {
    const auto r = std::from_chars(text.data(), text.data() + text.size(), result);
    return r.ec == std::errc{} && r.ptr == text.data() + text.size();
}

std::optional<std::pair<std::string, UInt32>> instance(std::string_view text) {
    const auto first = text.find_first_of("0123456789");
    UInt32 index{};
    if (first != text.npos && !number(text.substr(first), index)) {
        return {};
    }
    return std::pair(lower(text.substr(0, first)), index);
}

std::string mapName(std::string_view text) {
    if (text.empty() || text.size() > 255 || text.find('\0') != text.npos) {
        return {};
    }
    auto name = lower(text);
    if (!name.ends_with(".sae")) {
        name += ".sae";
    }
    return name;
}

std::string quote(std::string_view text) {
    std::string result = "\"";
    for (char c : text) {
        switch (c) {
            case '\\':
                result += "\\\\";
                break;
            case '"':
                result += "\\\"";
                break;
            case '\n':
                result += "\\n";
                break;
            case '\r':
                result += "\\r";
                break;
            case '\t':
                result += "\\t";
                break;
            default:
                result += c;
                break;
        }
    }
    return result + '"';
}

constexpr std::array<std::pair<std::string_view, UInt8>, 8> modifiers{
    {{"shift", sim::InputShift},
     {"control", sim::InputCtrl},
     {"alt", sim::InputAlt},
     {"l_shift", sim::InputLShift},
     {"r_shift", sim::InputRShift},
     {"l_control", sim::InputLCtrl},
     {"r_control", sim::InputRCtrl},
     {"l_alt", sim::InputLAlt}}};
constexpr std::array<std::pair<std::string_view, UInt8>, 14> objects{
    {{"xaxis", sim::InputXAxis},
     {"yaxis", sim::InputYAxis},
     {"zaxis", sim::InputZAxis},
     {"rxaxis", sim::InputRxAxis},
     {"ryaxis", sim::InputRyAxis},
     {"rzaxis", sim::InputRzAxis},
     {"pov", sim::InputPov},
     {"xpov", sim::InputXPov},
     {"ypov", sim::InputYPov},
     {"upov", sim::InputUpPov},
     {"dpov", sim::InputDownPov},
     {"lpov", sim::InputLeftPov},
     {"rpov", sim::InputRightPov},
     {"button", sim::InputButton}}};
constexpr std::array<std::pair<std::string_view, UInt32>, 8> flags{
    {{"None", sim::ActionNone},
     {"Value", sim::ActionValue},
     {"Flip", sim::ActionFlip},
     {"Center", sim::ActionCenter},
     {"Square", sim::ActionSquare},
     {"Cube", sim::ActionCube},
     {"DeadZone", sim::ActionDeadZone},
     {"Scale", sim::ActionScale}}};
}

std::optional<sim::SimInputEvent> parseInputArguments(std::span<const std::string> args) {
    if (args.size() < 2 || args.size() > 16) {
        return {};
    }
    const auto device = instance(args[0]);
    if (!device || device->second > 255) {
        return {};
    }
    sim::SimInputEvent event;
    event.deviceInst = device->second;
    event.deviceType = device->first == "keyboard"   ? sim::InputKeyboard
                       : device->first == "mouse"    ? sim::InputMouse
                       : device->first == "joystick" ? sim::InputJoystick
                       : device->first == "device"   ? sim::InputDevice
                                                     : 0;
    if (!event.deviceType) {
        return {};
    }
    auto input = std::next(args.begin());
    const auto action = lower(*input);
    event.action = action == "make"    ? sim::InputMake
                   : action == "break" ? sim::InputBreak
                                       : sim::InputMove;
    if (event.action != sim::InputMove) {
        ++input;
    }
    if (input == args.end()) {
        return {};
    }
    for (; input != args.end(); ++input) {
        const auto key = lower(*input);
        UInt8 modifier{};
        if (key == "r_alt") {
            modifier = sim::InputRAlt;
        } else if (key == "ctrl") {
            modifier = sim::InputCtrl;
        } else {
            const auto found =
                std::ranges::find(modifiers, key, &decltype(modifiers)::value_type::first);
            if (found != modifiers.end()) {
                modifier = found->second;
            }
        }
        if (!modifier) {
            break;
        }
        event.modifier |= modifier;
    }
    if (input == args.end()) {
        if (event.deviceType == sim::InputKeyboard && event.action != sim::InputMove &&
            (event.modifier == sim::InputShift || event.modifier == sim::InputCtrl ||
             event.modifier == sim::InputAlt)) {
            event.objType = sim::InputKey;
            return event;
        }
        return {};
    }
    if (std::next(input) != args.end()) {
        return {};
    }
    const auto object = instance(*input);
    if (!object || object->second > 255) {
        return {};
    }
    const auto found =
        std::ranges::find(objects, object->first, &decltype(objects)::value_type::first);
    if (found != objects.end()) {
        event.objType = found->second;
        event.objInst = UInt8(object->second);
        if (event.action == sim::InputMove && event.objType == sim::InputButton) {
            return {};
        }
    } else {
        if (event.action == sim::InputMove) {
            return {};
        }
        event.objType = sim::InputKey;
        event.objInst =
            input->size() == 1 ? sim::inputKeyCode(input->front()) : sim::inputKeyCode(*input);
        if (!event.objInst) {
            return {};
        }
    }
    return event;
}

std::optional<InputAction> parseActionArguments(std::span<const std::string> args) {
    if (args.empty() || args.size() > 24) {
        return {};
    }
    InputAction result;
    if (!number(args[0], result.action.action) || !result.action.action) {
        return {};
    }
    auto input = std::next(args.begin());
    float value{};
    if (input != args.end() && number(*input, value)) {
        if (!std::isfinite(value)) {
            return {};
        }
        result.flags |= sim::ActionValue;
        result.action.fValue = value;
        ++input;
    }
    for (; input != args.end(); ++input) {
        const auto found = std::ranges::find_if(flags, [&](const auto& f) {
            return lower(f.first) == lower(*input);
        });
        if (found == flags.end()) {
            return {};
        }
        result.flags |= found->second;
        if (found->second == sim::ActionDeadZone || found->second == sim::ActionScale) {
            if (++input == args.end() || !number(*input, value) || !std::isfinite(value)) {
                return {};
            }
            value = std::clamp(value, 0.f, 1.f);
            if (found->second == sim::ActionDeadZone) {
                result.action.deadZone = value;
            } else {
                result.action.scale = value;
            }
        }
    }
    return result;
}

std::optional<std::string> actionMapScript(const sim::SimActionMap& map) {
    std::string output;
    for (const auto& node : map.nodes()) {
        const auto& e = node.event;
        const char* device = e.deviceType == sim::InputKeyboard   ? "keyboard"
                             : e.deviceType == sim::InputMouse    ? "mouse"
                             : e.deviceType == sim::InputJoystick ? "joystick"
                             : e.deviceType == sim::InputDevice   ? "device"
                                                                  : nullptr;
        if (!device) {
            return {};
        }
        output += node.action.consoleCommand ? "bindCommand(" : "bindAction(";
        output += quote(std::string(device) + std::to_string(e.deviceInst)) + ", ";
        if (e.action == sim::InputMake || e.action == sim::InputBreak) {
            output += e.action == sim::InputMake ? "make, " : "break, ";
        }
        auto remaining = e.modifier;
        for (const auto& [name, mask] : modifiers) {
            if ((remaining & mask) == mask) {
                output += std::string(name) + ", ";
                remaining &= UInt8(~mask);
            }
        }
        if (remaining & sim::InputRAlt) {
            output += "r_alt, ";
            remaining &= UInt8(~sim::InputRAlt);
        }
        if (remaining) {
            return {};
        }
        if (e.objType == sim::InputKey) {
            if (e.objInst) {
                const auto* key = sim::inputKeyDescription(e.objInst);
                if (!key) {
                    return {};
                }
                const auto name =
                    key->name.empty() ? std::string(1, char(key->lower)) : std::string(key->name);
                if ((name.size() == 1 ? sim::inputKeyCode(name[0]) : sim::inputKeyCode(name)) !=
                    e.objInst) {
                    return {};
                }
                output += quote(name) + ", ";
            } else if (e.deviceType != sim::InputKeyboard || e.action == sim::InputMove ||
                       (e.modifier != sim::InputShift && e.modifier != sim::InputCtrl &&
                        e.modifier != sim::InputAlt)) {
                return {};
            }
        } else {
            const auto found =
                std::ranges::find(objects, e.objType, &decltype(objects)::value_type::second);
            if (found == objects.end()) {
                return {};
            }
            output += quote(std::string(found->first) + std::to_string(e.objInst)) + ", ";
        }
        output += "TO, ";
        if (node.action.consoleCommand) {
            output += quote(*node.action.consoleCommand);
        } else {
            output += std::to_string(node.action.action);
            if (node.flags & sim::ActionValue) {
                output += ", " + std::format("{:.9g}", node.action.fValue);
            }
            for (const auto& [name, flag] : flags) {
                if (flag == sim::ActionValue || !(node.flags & flag)) {
                    continue;
                }
                output += ", " + std::string(name);
                if (flag == sim::ActionDeadZone) {
                    output += ", " + std::format("{:.9g}", node.action.deadZone);
                }
                if (flag == sim::ActionScale) {
                    output += ", " + std::format("{:.9g}", node.action.scale);
                }
            }
        }
        output += ");\r\n";
    }
    return output;
}

bool InputMaps::edit(std::string_view text, ResourceManager& resources, bool clear) {
    auto name = mapName(text);
    if (name.empty()) {
        return false;
    }
    if (maps_.size() >= 256 && !maps_.contains(name)) {
        return false;
    }
    auto found = maps_.find(name);
    if (clear) {
        maps_[name] = std::make_shared<const sim::SimActionMap>();
    } else if (found == maps_.end()) {
        auto loaded = resources.load<sim::SimActionMap>(name);
        if (!loaded && resources.contains(name)) {
            return false;
        }
        maps_[name] = loaded ? std::make_shared<const sim::SimActionMap>(*loaded)
                             : std::make_shared<const sim::SimActionMap>();
    }
    current_ = std::move(name);
    return true;
}

bool InputMaps::bind(std::span<const std::string> args, bool command, bool gameMap) {
    const auto name = gameMap ? std::string("simgame.sae") : current_;
    const auto found = maps_.find(name);
    if (found == maps_.end() && (!gameMap || maps_.size() >= 256)) {
        return false;
    }
    const auto to = std::ranges::find_if(args, [](const auto& s) {
        return lower(s) == "to";
    });
    if (to == args.end()) {
        return false;
    }
    const auto index = std::size_t(to - args.begin());
    auto event = parseInputArguments(args.first(index));
    if (!event || index + 1 == args.size()) {
        return false;
    }
    InputAction action;
    if (command) {
        if (index + 2 != args.size() || args[index + 1].size() > 65536 ||
            args[index + 1].find('\0') != std::string::npos) {
            return false;
        }
        action.action.consoleCommand = args[index + 1];
    } else {
        auto parsed = parseActionArguments(args.subspan(index + 1));
        if (!parsed) {
            return false;
        }
        action = std::move(*parsed);
    }
    auto candidate = found == maps_.end() ? std::make_shared<sim::SimActionMap>()
                                          : std::make_shared<sim::SimActionMap>(*found->second);
    if (!candidate->mapEvent(*event, action.action, action.flags)) {
        return false;
    }
    maps_[name] = std::move(candidate);
    return true;
}

std::shared_ptr<const sim::SimActionMap> InputMaps::find(std::string_view text) const {
    const auto found = maps_.find(mapName(text));
    return found == maps_.end() ? nullptr : found->second;
}

bool InputMaps::replace(std::string_view text, const sim::SimActionMap& map) {
    auto name = mapName(text);
    if (name.empty() || (maps_.size() >= 256 && !maps_.contains(name))) {
        return false;
    }
    maps_[name] = std::make_shared<const sim::SimActionMap>(map);
    return true;
}

std::optional<std::string> InputMaps::script(std::span<const std::string> names) const {
    if (names.empty()) {
        return {};
    }
    std::string result;
    for (const auto& name : names) {
        auto map = find(name);
        if (!map) {
            return {};
        }
        auto body = actionMapScript(*map);
        if (!body) {
            return {};
        }
        result += "newActionMap(" + quote(mapName(name)) + ");\r\n" + *body;
    }
    return result;
}
}
