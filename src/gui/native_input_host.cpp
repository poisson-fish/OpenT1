#include <opent1/gui/native_object_hosts.hpp>
#include <opent1/sim/sim_sdl_input.hpp>
#include <opent1/sim/sim_resource_volume.hpp>
#include <opent1/core/file_stream.hpp>
#include <charconv>

namespace opent1::gui {
struct NativeInputHost::State {
    platform::WindowId window{};
    sim::SimManager* manager{};
    sim::SimAddress input;
    std::shared_ptr<sim::SdlSimInput> adapter;
    std::shared_ptr<void> cleanup;
    bool changing{}, pendingRelease{};
    world::InputMaps maps;
    HostFileWriter write;

    void releasePending() {
        if (!pendingRelease) {
            return;
        }
        pendingRelease = false;
        if (auto* current = service()) {
            current->release();
        }
    }

    void close() {
        auto previous = std::move(adapter);
        if (previous) {
            previous->capture(false);
        }
        releasePending();
    }

    void change(sim::SimManager* next) {
        if (manager == next || changing) {
            return;
        }
        changing = true;

        struct Reset {
            bool& value;

            ~Reset() {
                value = false;
            }
        } reset{changing};

        close();
        manager = next;
        input = {};
        maps.reset();
    }

    sim::SimInputManager* service() const {
        return manager
                   ? dynamic_cast<sim::SimInputManager*>(manager->findObject(sim::InputManagerId))
                   : nullptr;
    }

    bool open() {
        auto* current = service();
        if (!window || !current) {
            return false;
        }
        if (adapter && input.resolve(*manager) == current) {
            return true;
        }
        close();
        auto staged = std::make_shared<sim::SdlSimInput>(*current, window);
        input = current->address();
        adapter = std::move(staged);
        return true;
    }

    std::vector<std::string> devices() const {
        if (!adapter) {
            return {};
        }
        std::vector<std::string> result{"keyboard0", "mouse0"};
        for (auto slot : adapter->gamepadSlots()) {
            result.push_back("joystick" + std::to_string(slot));
        }
        return result;
    }

    std::optional<std::pair<UInt8, UInt32>> device(std::string_view name) const {
        const auto known = devices();
        if (std::ranges::find(known, name) == known.end()) {
            return {};
        }
        const auto first = name.find_first_of("0123456789");
        UInt32 index{};
        const auto r = std::from_chars(name.data() + first, name.data() + name.size(), index);
        if (r.ec != std::errc{}) {
            return {};
        }
        return std::pair(name.starts_with("keyboard") ? sim::InputKeyboard
                         : name.starts_with("mouse")  ? sim::InputMouse
                                                      : sim::InputJoystick,
                         index);
    }

    std::optional<std::string> command(Console& console,
                                       sim::SimManager* current,
                                       std::string_view name,
                                       std::span<const std::string> args) {
        if (current != manager || !manager) {
            return "False";
        }
        const auto yes = [](bool v) -> std::optional<std::string> {
            return v ? "True" : "False";
        };
        if (name == "newInputManager") {
            if (service()) {
                return "False";
            }
            auto object = std::make_unique<sim::SimInputManager>();
            object->assignName("InputManager");
            return yes(manager->addObject(std::move(object)) != nullptr);
        }
        if (name == "editActionMap" || name == "newActionMap") {
            auto* resources = sim::SimResource::find(*manager);
            return yes(resources && maps.edit(args[0], resources->get(), name == "newActionMap"));
        }
        if (name == "bindAction" || name == "bindCommand" || name == "bind") {
            return yes(maps.bind(args, name != "bindAction", name == "bind"));
        }
        if (name == "saveActionMap") {
            auto script = maps.script(args.subspan(1));
            auto filename = args[0];
            if (!filename.ends_with(".cs")) {
                filename += ".cs";
            }
            return yes(script && write(filename, *script));
        }
        if (name == "inputOpen") {
            return yes(open());
        }
        if (name == "inputClose") {
            close();
            return "True";
        }
        if (!adapter || !service() || input.resolve(*manager) != service()) {
            return "False";
        }
        if (name == "inputCapture" || name == "inputRelease") {
            adapter->capture(name == "inputCapture");
            return "True";
        }
        if (name == "inputActivate" || name == "inputDeactivate") {
            const bool enabled = name == "inputActivate";
            if (args[0] == "all") {
                for (const auto& item : devices()) {
                    auto d = device(item);
                    adapter->activateDevice(d->first, d->second, enabled);
                }
                return "True";
            }
            const auto d = device(args[0]);
            if (!d) {
                return "False";
            }
            adapter->activateDevice(d->first, d->second, enabled);
            return "True";
        }
        if (name == "listInputDevices") {
            for (const auto& item : devices()) {
                console.print(item);
            }
            return "True";
        }
        if (name == "getInputDeviceInfo") {
            const auto d = device(args[0]);
            if (!d) {
                return "False";
            }
            console.print(args[0]);
            if (d->first == sim::InputKeyboard) {
                for (UInt32 i = 1; i < 256; ++i) {
                    if (const auto* key = sim::inputKeyDescription(i);
                        key && (!key->name.empty() || key->lower)) {
                        console.print(std::format("key {} {}",
                                                  i,
                                                  key->name.empty()
                                                      ? std::string(1, char(key->lower))
                                                      : std::string(key->name)));
                    }
                }
            } else {
                console.print(
                    d->first == sim::InputMouse
                        ? "xaxis0 yaxis0 button0 button1 button2"
                        : "xaxis0 yaxis0 zaxis0 rxaxis0 ryaxis0 rzaxis0 pov0 button0..31");
            }
            return "True";
        }
        if (name == "saveInputDeviceInfo") {
            std::string info;
            for (const auto& item : devices()) {
                info += item + "\r\n";
            }
            return yes(write(args[0], info));
        }
        return "False";
    }
};

NativeInputHost::NativeInputHost(platform::SdlWindow& window, HostFileWriter writer)
    : state_(std::make_shared<State>()) {
    state_->window = window.id();
    if (!writer) {
        writer = [](std::string_view path, std::string_view text) {
            FileWStream out{std::filesystem::path(path)};
            return out.getStatus() == STRM_OK && out.writeBytes(std::as_bytes(std::span(text))) &&
                   out.flush();
        };
    }
    state_->write = std::move(writer);
    const std::weak_ptr<State> weak = state_;
    state_->cleanup = window.beforeNativeDestroy([weak] {
        if (auto s = weak.lock()) {
            s->window = 0;
            s->adapter.reset();
            s->pendingRelease = true;
        }
    });
}

world::ObjectHost NativeInputHost::host() const {
    auto state = state_;
    world::ObjectHost result;
    result.managerChanged = [state](auto* manager) {
        state->change(manager);
    };
    result.command = [state](Console& console, auto* manager, auto name, auto args) {
        return state->command(console, manager, name, args);
    };
    result.frame = [state] {
        state->releasePending();
    };
    result.stop = [state] {
        state->change(nullptr);
    };
    return result;
}

bool NativeInputHost::process(const platform::Event& event) {
    auto state = state_;
    auto adapter = state->adapter;
    if (!adapter || !state->manager || !state->input.resolve(*state->manager)) {
        return false;
    }
    return adapter->process(event);
}

std::shared_ptr<const sim::SimActionMap> NativeInputHost::map(std::string_view name) const {
    return state_->maps.find(name);
}
}
