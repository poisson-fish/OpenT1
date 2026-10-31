#include <opent1/world/object_plugins.hpp>
#include <array>

namespace opent1::world {
namespace {
constexpr std::array<ObjectCommand, 5> shell{{{"shellOpen", 1, 2},
                                              {"shellOpenAndGoWin", 2, 3},
                                              {"HTMLOpen", 1, 1},
                                              {"HTMLOpenAndGoWin", 2, 2},
                                              {"Notepad", 1, 1}}};
constexpr std::array<ObjectCommand, 21> graphics{{{"swapSurfaces", 1, 1},
                                                  {"isFullscreenMode", 1, 1},
                                                  {"setFullscreenMode", 2, 2},
                                                  {"setFSResolution", 2, 2},
                                                  {"isVirtualFS", 1, 1},
                                                  {"nextRes", 1, 1},
                                                  {"prevRes", 1, 1},
                                                  {"screenShot", 1, 2},
                                                  {"setScreenShotSeq", 1, 1},
                                                  {"flushTextureCache", 0, 1},
                                                  {"resetUpdateRegion", 1, 1},
                                                  {"setGamma", 2, 2},
                                                  {"setFullscreenDevice", 2, 2},
                                                  {"setWindowedDevice", 2, 2},
                                                  {"listDevices", 0, 0},
                                                  {"testDevice", 1, 1},
                                                  {"messageCanvasDevice", 2, 32},
                                                  {"setWindowSize", 3, 3},
                                                  {"lockWindowSize", 1, 1},
                                                  {"unlockWindowSize", 1, 1},
                                                  {"isGfxDriver", 0, 2}}};
constexpr std::array<ObjectCommand, 8> network{{{"netStats", 0, 0},
                                                {"logPacketStats", 0, 1},
                                                {"DNet::TranslateAddress", 2, 2},
                                                {"playDemo", 1, 1},
                                                {"timeDemo", 1, 1},
                                                {"connect", 1, 1},
                                                {"disconnect", 0, 0},
                                                {"net::kick", 1, 2}}};
constexpr std::array<ObjectCommand, 16> input{{{"newInputManager", 0, 0},
                                               {"listInputDevices", 0, 0},
                                               {"getInputDeviceInfo", 1, 1},
                                               {"saveInputDeviceInfo", 1, 1},
                                               {"inputOpen", 0, 0},
                                               {"inputClose", 0, 0},
                                               {"inputCapture", 0, 0},
                                               {"inputRelease", 0, 0},
                                               {"inputActivate", 1, 1},
                                               {"inputDeactivate", 1, 1},
                                               {"editActionMap", 1, 1},
                                               {"newActionMap", 1, 1},
                                               {"bindAction", 4, 32},
                                               {"bindCommand", 4, 32},
                                               {"bind", 4, 32},
                                               {"saveActionMap", 2, 64}}};
constexpr std::array<ObjectCommand, 5> tree{{{"simTreeCreate", 2, 3},
                                             {"simTreeAddSet", 2, 64},
                                             {"simTreeRegBitmaps", 2, 64},
                                             {"simTreeRegClass", 4, 4},
                                             {"simTreeRegScript", 2, 3}}};
constexpr std::array<ObjectCommand, 27> tool{
    {{"newToolWindow", 2, 3},   {"newToolStrip", 3, 4},     {"listToolButtons", 1, 1},
     {"listToolWindows", 0, 0}, {"hideToolWin", 1, 1},      {"showToolWin", 1, 1},
     {"showToolWinAll", 0, 0},  {"hideToolWinAll", 0, 0},   {"setToolWinPos", 3, 3},
     {"addToolButton", 3, 5},   {"delToolButton", 2, 2},    {"addToolGap", 2, 2},
     {"setToolCommand", 3, 4},  {"setButtonHelp", 3, 3},    {"isButtonDown", 2, 3},
     {"addStatusBar", 2, 32},   {"delStatusBar", 1, 1},     {"setStatusField", 4, 4},
     {"getStatusField", 2, 2},  {"clearStatusField", 2, 2}, {"setMainWindow", 1, 1},
     {"editBox", 3, 3},         {"browseBox", 5, 63},       {"edit2Box", 6, 6},
     {"confirmBox", 2, 2},      {"openFile", 3, 63},        {"saveFileAs", 3, 63}}};

bool truth(std::string_view s) {
    return s == "true" || s == "True" || s == "1";
}

bool shellValid(std::string_view name, std::span<const std::string> args) {
    if (name == "HTMLOpen" || name == "HTMLOpenAndGoWin") {
        auto prefix = args.back().substr(0, 8);
        for (auto& c : prefix) {
            if (c >= 'A' && c <= 'Z') {
                c = char(c - 'A' + 'a');
            }
        }
        return prefix.starts_with("http://") || prefix.starts_with("https://");
    }
    if (name == "Notepad") {
        auto text = args.front();
        if (text.find_first_of(" \t\r\n") != text.npos || text.size() < 4) {
            return false;
        }
        auto suffix = text.substr(text.size() - 4);
        for (auto& c : suffix) {
            if (c >= 'A' && c <= 'Z') {
                c = char(c - 'A' + 'a');
            }
        }
        return suffix == ".txt";
    }
    return true;
}
}

std::span<const ObjectCommand> objectCommands(ObjectAdapter kind) {
    switch (kind) {
        case ObjectAdapter::Console:
            return {};
        case ObjectAdapter::Shell:
            return shell;
        case ObjectAdapter::Graphics:
            return graphics;
        case ObjectAdapter::Network:
            return network;
        case ObjectAdapter::Input:
            return input;
        case ObjectAdapter::Tree:
            return tree;
        case ObjectAdapter::Tool:
            return tool;
    }
    return {};
}

struct ObjectPlugin::State {
    Console* console{};
    sim::SimManager* manager{};
    ObjectHost host;
    bool active{}, enabled{}, healthy{true}, dispatching{};
};

ObjectPlugin::ObjectPlugin(ObjectAdapter kind, ObjectHost host)
    : kind_(kind), host_(std::move(host)) {}

ObjectPlugin::~ObjectPlugin() {
    shutdown();
}

bool ObjectPlugin::healthy() const {
    return state_ && state_->healthy;
}

void ObjectPlugin::init(Console& console, sim::SimManager* manager) {
    if (state_) {
        throw std::logic_error("Object plugin already installed");
    }
    auto state = std::make_shared<State>();
    state->console = &console;
    state->manager = manager;
    state->host = host_;
    state->active = true;
    std::vector<std::shared_ptr<void>> commands;
    const std::weak_ptr<State> weak = state;
    for (const auto spec : objectCommands(kind_)) {
        const auto kind = kind_;
        commands.push_back(console.scopedCommand(
            0,
            spec.name,
            [weak, spec, kind](Console& c, Int32, std::span<const std::string> args)
                -> std::optional<std::string> {
                auto s = weak.lock();
                if (!s || !s->active || s->dispatching || args.empty()) {
                    return "False";
                }
                args = args.subspan(1);
                if (args.size() < spec.minimum || args.size() > spec.maximum || !s->host.command) {
                    return "False";
                }
                std::size_t bytes = 0;
                for (const auto& arg : args) {
                    bytes += arg.size();
                    if (arg.size() > 65536 || arg.find('\0') != arg.npos || bytes > 1048576) {
                        return "False";
                    }
                }
                if (kind == ObjectAdapter::Shell && !shellValid(spec.name, args)) {
                    return "False";
                }
                if (kind == ObjectAdapter::Tool &&
                    (spec.name == "openFile" || spec.name == "saveFileAs" ||
                     spec.name == "browseBox") &&
                    args.size() % 2 == 0) {
                    return "False";
                }
                const auto callback = s->host.command;
                s->dispatching = true;
                struct Reset {
                    bool& value;
                    ~Reset() {
                        value = false;
                    }
                } reset{s->dispatching};
                try {
                    auto result = callback(c, s->manager, spec.name, args);
                    return result ? std::move(result) : std::optional<std::string>("False");
                } catch (...) {
                    s->healthy = false;
                    return "False";
                }
            }));
    }
    if (kind_ == ObjectAdapter::Console) {
        output_ = console.subscribeOutput([weak](std::string_view text) {
            auto s = weak.lock();
            if (s && s->active && s->enabled && s->host.output) {
                const auto callback = s->host.output;
                try {
                    callback(text);
                } catch (...) {
                    s->healthy = false;
                }
            }
        });
    }
    try {
        if (state->host.managerChanged) {
            state->host.managerChanged(manager);
        }
    } catch (...) {
        if (state->host.stop) {
            state->host.stop();
        }
        output_.reset();
        throw;
    }
    commands_ = std::move(commands);
    state_ = std::move(state);
}

void ObjectPlugin::shutdown() noexcept {
    if (!state_) {
        return;
    }
    auto old = std::move(state_);
    old->active = false;
    old->manager = nullptr;
    old->console = nullptr;
    commands_.clear();
    output_.reset();
    try {
        if (old->enabled && old->host.consoleEnabled) {
            old->host.consoleEnabled(false, {});
        }
        if (old->host.stop) {
            old->host.stop();
        }
    } catch (...) {
    }
    old->host = {};
}

void ObjectPlugin::setManager(sim::SimManager* manager) {
    if (!state_) {
        return;
    }
    state_->manager = manager;
    if (state_->host.managerChanged) {
        try {
            state_->host.managerChanged(manager);
        } catch (...) {
            state_->healthy = false;
        }
    }
}

void ObjectPlugin::startFrame() {
    auto s = state_;
    if (!s || !s->active || s->dispatching) {
        return;
    }
    if (s->host.frame) {
        try {
            const auto frame = s->host.frame;
            frame();
        } catch (...) {
            s->healthy = false;
        }
    }
    if (!s->active || kind_ != ObjectAdapter::Console) {
        return;
    }
    try {
        const bool enabled = truth(s->console->getVariable("WinConsoleEnabled"));
        if (enabled != s->enabled) {
            if (s->host.consoleEnabled &&
                !s->host.consoleEnabled(enabled, s->console->getVariable("Console::WindowTitle"))) {
                s->healthy = false;
                return;
            }
            s->enabled = enabled;
        }
        if (!enabled || !s->host.input) {
            return;
        }
        auto lines = s->host.input();
        if (lines.size() > 64) {
            s->healthy = false;
            return;
        }
        for (const auto& line : lines) {
            if (!s->active) {
                break;
            }
            if (line.size() > 65536 || line.find('\0') != line.npos ||
                !s->console->evaluate(line, "<native console>").ok()) {
                s->healthy = false;
            }
        }
    } catch (...) {
        s->healthy = false;
    }
}
} // namespace opent1::world
