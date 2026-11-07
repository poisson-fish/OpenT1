#include <opent1/sim/sim_world_game.hpp>
#include <opent1/sim/sim_editor_events.hpp>
#include <opent1/sim/common_irc_shadow.hpp>
#include <opent1/sim/sim_irc.hpp>
#include <opent1/sim/sim_palette.hpp>
#include <opent1/sim/sim_native_media.hpp>
#include <opent1/sim/sim_interior_grouping.hpp>
#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <limits>
#include <stdexcept>

namespace opent1::sim {
std::shared_ptr<ConsoleDataManager> makeSimConsoleDataManager(SimGame&);

namespace {
struct FlagGuard {
    bool& flag;

    explicit FlagGuard(bool& value) : flag(value) {
        if (flag) {
            throw std::logic_error("recursive world/game drive");
        }
        flag = true;
    }

    ~FlagGuard() {
        flag = false;
    }
};

struct OperationGuard {
    bool& flag;
    bool previous;

    explicit OperationGuard(bool& value) : flag(value), previous(value) {
        flag = true;
    }

    ~OperationGuard() {
        flag = previous;
    }
};

bool textValid(std::string_view text, std::size_t max) {
    return text.size() <= max && text.find('\0') == std::string_view::npos;
}

std::string canonical(std::string_view text) {
    std::string value(text);
    std::ranges::transform(value, value.begin(), [](unsigned char c) {
        return char(c >= 'A' && c <= 'Z' ? c + 32 : c);
    });
    return value;
}

std::string number(double value) {
    std::array<char, 64> result{};
    std::snprintf(result.data(), result.size(), "%g", value);
    return result.data();
}

std::optional<double> real(std::string_view text) {
    double value{};
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    return result.ec == std::errc{} && result.ptr == text.data() + text.size() &&
                   std::isfinite(value)
               ? std::optional(value)
               : std::nullopt;
}

bool finite(const ColorF& c) {
    return std::isfinite(c.red) && std::isfinite(c.green) && std::isfinite(c.blue);
}

bool finite(const Point3F& p) {
    return std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z);
}
} // namespace

bool SimObject::processEvent(const SimEvent& event) {
    const auto* command = dynamic_cast<const SimConsoleEvent*>(&event);
    if (!command || !getManager() || !getManager()->console()) {
        return false;
    }
    return command->execute(*getManager()->console());
}

SimTimerSet::SimTimerSet(SimTime interval, SimObjectId id) : interval_(interval) {
    if (!std::isfinite(interval) || interval <= 0 || !id) {
        throw std::invalid_argument("invalid simulation timer interval/id");
    }
    setId(id);
    setStable(true);
}

bool SimTimerSet::scheduleNext() {
    if (!getManager() || isRemoved()) {
        return running_ = false;
    }
    const auto now = getManager()->getCurrentTime();
    const auto next = now + interval_;
    if (!std::isfinite(next) || next <= now) {
        return running_ = false;
    }
    auto event = std::make_unique<SimEvent>();
    event->time = next;
    return running_ = postEvent(std::move(event));
}

bool SimTimerSet::onAdd() {
    return scheduleNext();
}

bool SimTimerSet::processEvent(const SimEvent& event) {
    if (event.type != EventTypeFirst || !getManager()) {
        return SimSet::processEvent(event);
    }
    auto* manager = getManager();
    manager->dispatch([&] {
        SimTimerEvent tick;
        tick.time = manager->getCurrentTime();
        tick.timerId = getId();
        tick.timerInterval = interval_;
        try {
            SimSet::processEvent(tick);
        } catch (...) {
            scheduleNext();
            throw;
        }
        scheduleNext();
    });
    return true;
}

bool SimConsoleEvent::valid() const {
    if (script) {
        return arguments.empty() && textValid(*script, MaxCommandBytes - 1);
    }
    const bool remote = (flags & Remote) != 0;
    if (arguments.empty() || arguments.size() > MaxArgs + (remote ? 1 : 0)) {
        return false;
    }
    std::size_t bytes = 0;
    return std::ranges::all_of(arguments, [&](const auto& arg) {
        bytes += arg.size() + 1;
        return textValid(arg, remote ? 261 : 255) && bytes <= MaxCommandBytes + (remote ? 17 : 0);
    });
}

bool SimConsoleEvent::execute(Console& console) const {
    if (!valid()) {
        return false;
    }
    if (echo && script) {
        console.print(*script);
    }
    if (script) {
        (void)console.evaluate(*script, "<scheduled>");
    } else {
        (void)console.execute(arguments.front(), std::span(arguments).subspan(1));
    }
    return true;
}

bool SimConsoleEvent::packCommand(BitStream& stream) const {
    if (script || (flags & Remote) || !valid()) {
        return false;
    }
    stream.writeInt(static_cast<Int32>(arguments.size()), 5);
    if (!stream.isValid()) {
        return false;
    }
    return std::ranges::all_of(arguments, [&](const auto& arg) {
        return stream.writeString(arg);
    });
}

bool SimConsoleEvent::unpackCommand(BitStream& stream, SimObjectId source) {
    const auto count = static_cast<UInt32>(stream.readInt(5));
    if (!stream.isValid() || !count || count > MaxArgs) {
        return false;
    }
    std::vector<std::string> decoded;
    if (!std::ranges::all_of(std::views::iota(UInt32{0}, count), [&](UInt32) {
            std::string arg;
            if (!stream.readString(arg)) {
                return false;
            }
            decoded.push_back(std::move(arg));
            return true;
        })) {
        return false;
    }
    // Remote delivery adds one argument and prefixes the command. Bound the
    // original 512-byte payload before adding these owned routing fields.
    SimConsoleEvent payload;
    payload.arguments = decoded;
    if (!payload.valid()) {
        return false;
    }
    decoded.front().insert(0, "remote");
    decoded.insert(decoded.begin() + 1, std::to_string(source));
    arguments = std::move(decoded);
    script.reset();
    sourceManagerId = source;
    flags |= Remote;
    address = {};
    echo = false;
    return true;
}

bool SimConsoleEvent::schedule(SimObject& destination,
                               std::string_view text,
                               SimTime delay,
                               bool echo) {
    if (!destination.getManager() || destination.isRemoved() || !std::isfinite(delay) ||
        !textValid(text, MaxCommandBytes - 1)) {
        return false;
    }
    const auto when = destination.getManager()->getCurrentTime() + std::max(0.f, delay);
    if (!std::isfinite(when)) {
        return false;
    }
    auto event = std::make_unique<SimConsoleEvent>();
    event->script = text;
    event->time = when;
    event->echo = echo;
    return destination.postEvent(std::move(event));
}

SimSceneLighting SimLightSet::getSceneLighting(std::optional<Point3F> camera, Flt32 distance) {
    if (!std::isfinite(distance) || distance < 0 || (camera && !finite(*camera))) {
        throw std::invalid_argument("invalid scene lighting query");
    }
    SimSceneLighting result;
    auto gather = [&] {
        const auto snapshot = objects_;
        std::ranges::for_each(snapshot, [&](auto* object) {
            if (isRemoved() || std::ranges::find(objects_, object) == objects_.end() ||
                object->isRemoved()) {
                return;
            }
            SimLightQuery query;
            if (!object->processQuery(query)) {
                return;
            }
            if (query.lights.size() > SimLightQuery::MaxLights || !finite(query.ambient)) {
                throw std::invalid_argument("invalid scene light provider");
            }
            result.ambient += query.ambient;
            if (!finite(result.ambient)) {
                throw std::overflow_error("ambient intensity overflow");
            }
            std::ranges::for_each(query.lights, [&](const auto& light) {
                if (!finite(light.position) || !finite(light.direction) ||
                    !finite(light.intensity) || !std::isfinite(light.range) || light.range < 0 ||
                    !finite(light.attenuation) || light.attenuation.x < 0 ||
                    light.attenuation.y < 0 || light.attenuation.z < 0 ||
                    !std::isfinite(light.shadowAttenuation) || light.shadowAttenuation < 0 ||
                    light.shadowAttenuation > 1 || (light.shadowMap && !light.shadowMap->valid()) ||
                    (light.type != SimLight::Type::Directional &&
                     light.type != SimLight::Type::Point &&
                     light.type != SimLight::Type::DirectionalNonlinear &&
                     light.type != SimLight::Type::DirectionalWrap)) {
                    throw std::invalid_argument("invalid scene light");
                }
                if (camera && light.type == SimLight::Type::Point) {
                    // Original light-set admission uses float squared distance,
                    // includes the boundary and does not add the light range.
                    const auto delta = *camera - light.position;
                    if (m_dot(delta, delta) > distance * distance) {
                        return;
                    }
                }
                if (result.lights.size() >= 65536) {
                    throw std::length_error("scene light limit");
                }
                result.lights.push_back(light);
            });
        });
    };
    if (getManager()) {
        getManager()->dispatch(gather);
    } else {
        gather();
    }
    return result;
}

SimWorld::SimWorld(Console* console, std::shared_ptr<ResourceManager> resources) {
    manager_.setConsole(console);
    const auto install = [&](auto object) {
        if (!manager_.addObject(std::move(object))) {
            throw std::runtime_error("failed to install world service");
        }
    };
    install(std::make_unique<SimResource>(std::move(resources)));
    install(std::make_unique<SimDefaultOpenContainer>(RootContainerId));
    install(std::make_unique<SimInputConsumerSet>());
    install(std::make_unique<SimActionHandler>(console));
    install(std::make_unique<SimInputManager>());
    install(std::make_unique<SimConsoleScheduler>());
    install(std::make_unique<SimCanvasSet>());
    std::ranges::for_each(std::array{PacketStreamSetId,
                                     GhostManagerSetId,
                                     GhostAlwaysSetId,
                                     CameraSetId,
                                     CameraMountSetId,
                                     RenderSetId,
                                     FrameEndNotifySetId,
                                     NamedGuiSetId,
                                     TaggedGuiSetId},
                          [&](auto id) {
                              auto set = std::make_unique<SimSet>();
                              set->setId(id);
                              set->setStable(true);
                              if (id == NamedGuiSetId) {
                                  set->assignName("NamedGuiSet");
                              }
                              install(std::move(set));
                          });
    install(std::make_unique<SimLightSet>());
    install(std::make_unique<SimTimerSet>());
}

std::size_t SimWorld::advanceToTime(SimTime time, std::size_t budget) {
    FlagGuard guard(advancing_);
    if (!std::isfinite(time) || time < manager_.getTargetTime()) {
        throw std::invalid_argument("invalid world time");
    }
    const auto postMessage = [&](SimObjectId id, SimTime when, bool current) {
        if (auto* target = manager_.findObject(id)) {
            auto event = std::make_unique<SimMessageEvent>();
            event->time = when;
            event->address = target->address();
            if (!manager_.postEvent(std::move(event), current)) {
                throw std::length_error("world event queue full");
            }
        }
    };
    postMessage(CSDelegateId, manager_.getCurrentTime(), true);
    postMessage(PacketStreamSetId, time, false);
    return manager_.advanceToTime(time, budget);
}

void SimWorld::render() {
    if (!renderEnabled_) {
        return;
    }
    FlagGuard guard(rendering_);
    if (auto* canvases = dynamic_cast<SimCanvasSet*>(manager_.findObject(CanvasSetId))) {
        canvases->render();
    }
    const auto render = render_;
    if (render) {
        render();
    }
    if (auto* set = manager_.findObject(FrameEndNotifySetId)) {
        SimEvent event;
        event.type = EventTypeFirst + 6;
        event.time = manager_.getCurrentTime();
        manager_.sendEvent(set->address(), event);
    }
}

namespace {
enum GameCommand {
    Schedule,
    GetTime,
    NewObject,
    DeleteObject,
    IsObject,
    AddToSet,
    RenameObject,
    SetObjectId,
    GetGroup,
    IsMember,
    GetNextObject,
    FocusClient,
    FocusServer,
    Unfocus,
    PostAction,
    IsFile,
    ListObjects,
    LoadObject,
    StoreObject,
    RemoteEval,
    PurgeResources,
    GetPathOf,
    ListVolumes,
    NewClient,
    NewServer,
    DeleteClient,
    DeleteServer,
    ObjectFocus,
    SetWindowTitle,
    InspectObject
};
}

SimGame::SimGame(std::shared_ptr<ResourceManager> resources)
    : resources_(resources ? std::move(resources) : std::make_shared<ResourceManager>()),
      console_(resources_.get()) {
    if (!registerSimClasses(classes_) || !registerSimResourceClasses(classes_) ||
        !registerCommonClasses(classes_) || !registerSimInputClasses(classes_) ||
        !registerSimPaletteClass(classes_)) {
        throw std::logic_error("simulation class registration failed");
    }
    console_.setDataManager(makeSimConsoleDataManager(*this));
    registerInteriorGroupingResource(*resources_);
    classes_.registerClass<SimInteriorGrouping>("SimInteriorGrouping");
    registerFactory("SimInteriorGrouping", [this] {
        return std::make_unique<SimInteriorGrouping>(this);
    });
    console_.bindVariable(
        "SimTimeScale",
        [this] {
            return number(timeScale_);
        },
        [this](auto text) {
            if (const auto value = real(text)) {
                setTimeScale(*value);
            }
        });
    registerFactory("SimObject", [] {
        return std::make_unique<SimObject>();
    });
    registerFactory("SimSet", [] {
        return std::make_unique<SimSet>();
    });
    registerFactory("SimGroup", [] {
        return std::make_unique<SimGroup>();
    });
    registerFactory("SimConsoleScheduler", [] {
        return std::make_unique<SimConsoleScheduler>();
    });
    registerFactory("SimContainer", [] {
        return std::make_unique<SimContainer>();
    });
    registerFactory("SimDefaultOpenContainer", [] {
        return std::make_unique<SimDefaultOpenContainer>();
    });
    registerFactory("SimMovement", [] {
        return std::make_unique<SimMovement>();
    });
    registerFactory("SimDynamicLight", [] {
        return std::make_unique<SimDynamicLight>();
    });
    registerFactory("SimVolumetric", [] {
        return std::make_unique<SimVolumetric>();
    });
    registerFactory("SimVolume", [] {
        return std::make_unique<SimVolume>();
    });
    registerFactory("IRCClient", [] {
        return std::make_unique<IRCClient>();
    });
    registerFactory("Redbook", [] {
        return std::make_unique<Redbook>();
    });
    registerFactory("MovPlay", [] {
        return std::make_unique<MovPlay>();
    });
    console_.addCommand(0,
                        "Object::getName",
                        [this](Console&, Int32, std::span<const std::string> args) -> std::string {
                            auto* object = args.size() == 2 ? findObject(args[1]) : nullptr;
                            return object ? std::string(object->getName()) : std::string{};
                        });
    const auto commands =
        std::to_array<std::pair<Int32, std::string_view>>({{Schedule, "schedule"},
                                                           {GetTime, "getSimTime"},
                                                           {NewObject, "newObject"},
                                                           {DeleteObject, "deleteObject"},
                                                           {IsObject, "isObject"},
                                                           {AddToSet, "addToSet"},
                                                           {RenameObject, "renameObject"},
                                                           {SetObjectId, "setObjectId"},
                                                           {GetGroup, "getGroup"},
                                                           {IsMember, "isMember"},
                                                           {GetNextObject, "getNextObject"},
                                                           {FocusClient, "focusClient"},
                                                           {FocusServer, "focusServer"},
                                                           {Unfocus, "unfocus"},
                                                           {PostAction, "postAction"},
                                                           {IsFile, "isFile"},
                                                           {ListObjects, "listObjects"},
                                                           {LoadObject, "loadObject"},
                                                           {StoreObject, "storeObject"},
                                                           {RemoteEval, "remoteEval"},
                                                           {PurgeResources, "purgeResources"},
                                                           {GetPathOf, "getPathOf"},
                                                           {ListVolumes, "listVolumes"},
                                                           {NewClient, "newClient"},
                                                           {NewServer, "newServer"},
                                                           {DeleteClient, "deleteClient"},
                                                           {DeleteServer, "deleteServer"},
                                                           {ObjectFocus, "focus"},
                                                           {SetWindowTitle, "setWindowTitle"},
                                                           {InspectObject, "inspectObject"}});
    std::ranges::for_each(commands, [&](auto item) {
        console_.addCommand(item.first, item.second, [this](auto& console, auto id, auto args) {
            return command(console, id, args);
        });
    });
}

SimGame::~SimGame() {
    // Plugins are detached while all worlds/console still exist.
    driving_ = true;
    switching_ = true;
    try {
        setPluginManagers(nullptr);
    } catch (...) {
    }
    auto retiring = std::move(plugins_);
    for (const auto& plugin : retiring) {
        plugin->shutdown();
    }
    retiring.clear();
    console_.setDataManager({});
    plugins_.clear();
    worlds_ = {};
}

SimWorld* SimGame::world(WorldType type) const {
    const auto index = static_cast<std::size_t>(type);
    return index < worlds_.size() ? worlds_[index].get() : nullptr;
}

SimManager* SimGame::manager() const {
    if (executionManager_) {
        return executionManager_;
    }
    auto* value = focused_ ? world(*focused_) : nullptr;
    return value ? &value->manager() : nullptr;
}

void SimGame::withWorld(WorldType type, const std::function<void()>& run) {
    auto* selected = world(type);
    if (!selected) {
        return;
    }
    OperationGuard guard(driving_);
    auto* previous = executionManager_;
    executionManager_ = &selected->manager();
    try {
        setPluginManagers(executionManager_);
        selected->manager().dispatch(run);
    } catch (...) {
        executionManager_ = previous;
        try {
            setPluginManagers(manager());
        } catch (...) {
        }
        throw;
    }
    executionManager_ = previous;
    setPluginManagers(manager());
}

void SimGame::setPluginManagers(SimManager* manager) {
    OperationGuard guard(driving_);
    const auto snapshot = plugins_;
    std::ranges::for_each(snapshot, [&](const auto& plugin) {
        if (std::ranges::find(plugins_, plugin) != plugins_.end()) {
            plugin->setManager(manager);
        }
    });
}

bool SimGame::setWorld(WorldType type, std::unique_ptr<SimWorld> replacement) {
    const auto index = static_cast<std::size_t>(type);
    if (index >= worlds_.size() || driving_ || switching_ || declarationDepth_ ||
        std::ranges::any_of(worlds_, [](const auto& world) {
            return world && world->manager().isDispatching();
        })) {
        return false;
    }
    FlagGuard guard(switching_);
    if (replacement && replacement->manager().getTargetTime() > simTime_) {
        return false;
    }
    if (worldRevisions_[index] == std::numeric_limits<UInt64>::max()) {
        throw std::overflow_error("world revision exhausted");
    }
    if (focused_ == type && worlds_[index]) {
        if (auto* input = dynamic_cast<SimInputManager*>(
                worlds_[index]->manager().findObject(InputManagerId))) {
            input->release();
        }
        focused_.reset();
        setPluginManagers(nullptr);
    }
    ++worldRevisions_[index];
    worlds_[index] = std::move(replacement);
    if (worlds_[index]) {
        worlds_[index]->manager().setConsole(&console_);
        worlds_[index]->manager().setClassRegistry(&classes_);
    }
    return true;
}

bool SimGame::focus(std::optional<WorldType> type) {
    if (switching_) {
        return false;
    }
    if (type && !world(*type)) {
        return false;
    }
    if (focused_ == type) {
        return true;
    }
    FlagGuard guard(switching_);
    if (auto* old = manager()) {
        if (auto* input = dynamic_cast<SimInputManager*>(old->findObject(InputManagerId))) {
            input->release();
        }
    }
    focused_ = type;
    setPluginManagers(manager());
    return true;
}

bool SimGame::registerFactory(std::string_view name, Factory factory) {
    return !name.empty() && textValid(name, 255) && factory &&
           factories_.emplace(canonical(name), std::move(factory)).second;
}

std::shared_ptr<void> SimGame::scopedFactory(std::string_view name, Factory factory) {
    const auto key = canonical(name);
    if (!registerFactory(name, std::move(factory))) {
        throw std::runtime_error("Object factory namespace in use");
    }
    try {
        const std::weak_ptr<const void> weak = factoryLifetime_;
        return std::shared_ptr<void>(new int(0), [this, weak, key](void* value) {
            delete static_cast<int*>(value);
            if (!weak.expired()) {
                factories_.erase(key);
            }
        });
    } catch (...) {
        factories_.erase(key);
        throw;
    }
}

std::shared_ptr<void> SimGame::scopedLoadPreparation(std::string_view name,
                                                     LoadPreparation prepare) {
    const auto key = canonical(name);
    if (name.empty() || !textValid(name, 255) || !prepare ||
        !loadPreparations_.emplace(key, std::move(prepare)).second) {
        throw std::runtime_error("Load preparation namespace in use");
    }
    try {
        const std::weak_ptr<const void> weak = factoryLifetime_;
        return std::shared_ptr<void>(new int(0), [this, weak, key](void* value) {
            delete static_cast<int*>(value);
            if (!weak.expired()) {
                loadPreparations_.erase(key);
            }
        });
    } catch (...) {
        loadPreparations_.erase(key);
        throw;
    }
}

bool SimGame::registerPlugin(std::shared_ptr<SimConsolePlugin> plugin) {
    if (!plugin || plugins_.size() >= 1024 ||
        std::ranges::find(plugins_, plugin) != plugins_.end()) {
        return false;
    }
    plugins_.push_back(plugin);
    OperationGuard guard(driving_);
    try {
        plugin->init(console_, manager());
    } catch (...) {
        std::erase(plugins_, plugin);
        plugin->shutdown();
        throw;
    }
    return true;
}

bool SimGame::removePlugin(const std::shared_ptr<SimConsolePlugin>& plugin) {
    auto retained = plugin;
    if (std::erase(plugins_, retained) == 0) {
        return false;
    }
    try {
        retained->setManager(nullptr);
    } catch (...) {
    }
    retained->shutdown();
    return true;
}

bool SimGame::setTimeScale(double scale) {
    if (!std::isfinite(scale) || scale < 0 || scale > 1000) {
        return false;
    }
    timeScale_ = scale;
    return true;
}

void SimGame::frame(double wallTime, bool renderCanvases) {
    if (switching_) {
        throw std::logic_error("cannot drive a game during world/focus replacement");
    }
    if (!std::isfinite(wallTime) || wallTime < 0 || (started_ && wallTime < wallTime_)) {
        throw std::invalid_argument("invalid game wall time");
    }
    const auto target = simTime_ + (started_ ? wallTime - wallTime_ : 0) * timeScale_;
    if (!std::isfinite(target) || target > std::numeric_limits<float>::max()) {
        throw std::overflow_error("game time overflow");
    }
    FlagGuard guard(driving_);
    started_ = true;
    wallTime_ = wallTime;
    simTime_ = target;
    const auto plugins = plugins_;
    const auto visit = [&](auto callback) {
        std::ranges::for_each(plugins, [&](const auto& plugin) {
            if (std::ranges::find(plugins_, plugin) != plugins_.end()) {
                callback(*plugin);
            }
        });
    };
    try {
        visit([](auto& plugin) {
            plugin.startFrame();
        });
        for (const auto type : {WorldType::Server, WorldType::Client}) {
            withWorld(type, [&] {
                world(type)->advanceToTime(static_cast<float>(target));
            });
        }
        setPluginManagers(manager());
        if (renderCanvases) {
            std::ranges::for_each(worlds_, [](const auto& world) {
                if (world) {
                    world->render();
                }
            });
        }
        visit([](auto& plugin) {
            plugin.endFrame();
        });
        if (console_.isFunction("Game::EndFrame")) {
            (void)console_.execute("Game::EndFrame");
        }
    } catch (...) {
        try {
            setPluginManagers(manager());
        } catch (...) {
        }
        throw;
    }
}

SimObject* SimGame::findObject(std::string_view path, SimManager* preferred) const {
    path.remove_prefix(static_cast<std::size_t>(std::ranges::find_if_not(path,
                                                                         [](char c) {
                                                                             return c == ' ' ||
                                                                                    c == '\t';
                                                                         }) -
                                                path.begin()));
    auto* destination = preferred ? preferred : manager();
    if (path.size() >= 2 && (path[0] == '/' || path[0] == '\\') &&
        (path[1] == '/' || path[1] == '\\')) {
        path.remove_prefix(2);
        const auto end = path.find_first_of("/\\");
        const auto name = canonical(path.substr(0, end));
        auto* selected = world(name == "server" ? WorldType::Server : WorldType::Client);
        destination = selected ? &selected->manager() : nullptr;
        if (!destination) {
            return nullptr;
        }
        path = end == path.npos ? std::string_view{} : path.substr(end);
    }
    if (!destination) {
        if (auto* client = world(WorldType::Client)) {
            destination = &client->manager();
        }
    }
    if (auto* object = destination ? destination->findObject(path) : nullptr) {
        return object;
    }
    const auto* client = world(WorldType::Client);
    return client && destination == &client->manager() && findClientObject
               ? findClientObject(path)
               : nullptr;
}

SimObject* SimGame::createObject(std::string_view className,
                                 std::string_view name,
                                 SimGroup* parent,
                                 std::span<const std::string> arguments) {
    auto* destination = parent ? parent->getManager() : manager();
    if (!destination || !textValid(name, 255) || arguments.size() > 127 ||
        !std::ranges::all_of(arguments, [](const auto& a) {
            return textValid(a, 4096);
        })) {
        return nullptr;
    }
    OperationGuard guard(driving_);
    SimObject* result{};
    destination->dispatch([&] {
        auto object = makeObject(className);
        if (!object || (parent && parent->isRemoved())) {
            return;
        }
        object->assignName(name);
        if (!object->prepareArguments(arguments) || (parent && parent->isRemoved())) {
            return;
        }
        result = destination->addObject(std::move(object), parent);
        if (result) {
            auto* initialized = result;
            try {
                if (!initialized->processArguments(arguments) || initialized->isRemoved() ||
                    (initializeObject && !initializeObject(*initialized))) {
                    if (!initialized->isRemoved()) {
                        initialized->deleteObject();
                    }
                    result = nullptr;
                }
            } catch (...) {
                if (!initialized->isRemoved()) {
                    initialized->deleteObject();
                }
                result = nullptr;
                throw;
            }
        }
    });
    return result;
}

std::optional<std::string>
SimGame::command(Console& console, Int32 id, std::span<const std::string> args) {
    if (id == NewClient || id == NewServer || id == DeleteClient || id == DeleteServer) {
        if (args.size() != 1 || driving_ || switching_ || declarationDepth_) {
            return "False";
        }
        const auto type =
            id == NewClient || id == DeleteClient ? WorldType::Client : WorldType::Server;
        auto replacement = id == NewClient || id == NewServer
                               ? std::make_unique<SimWorld>(&console_, resources_)
                               : nullptr;
        return setWorld(type, std::move(replacement)) ? "True" : "False";
    }
    std::optional<std::string> result;
    withWorlds([&] {
        result = invokeCommand(console, id, args);
    });
    return result;
}

void SimGame::withWorlds(const std::function<void()>& run) {
    OperationGuard guard(driving_);
    // Keep borrowed targets in either world alive across engine/output callbacks.
    const auto client = [&] {
        if (worlds_[1]) {
            worlds_[1]->manager().dispatch(run);
        } else {
            run();
        }
    };
    if (worlds_[0]) {
        worlds_[0]->manager().dispatch(client);
    } else {
        client();
    }
}

std::optional<std::string>
SimGame::invokeCommand(Console& console, Int32 id, std::span<const std::string> args) {
    const auto yes = [](bool value) -> std::optional<std::string> {
        return value ? "True" : "False";
    };
    const auto size = args.size();
    auto* destination = manager();
    if (id == FocusClient || id == FocusServer) {
        return yes(size == 1 && focus(id == FocusClient ? WorldType::Client : WorldType::Server));
    }
    if (id == Unfocus && size == 1) {
        return yes(focus({}));
    }
    if (id == IsFile) {
        if (size < 2) {
            return "False";
        }
        return yes(std::ranges::all_of(args.subspan(1), [&](const auto& name) {
            std::ifstream file(console.filePath(name), std::ios::binary);
            return file.is_open();
        }));
    }
    if (!destination) {
        return "False";
    }
    if (id == InspectObject) {
        if (size != 2) {
            return "False";
        }
        const auto presenter = inspector_;
        if (!presenter) {
            return "False";
        }
        auto inspection = inspectObject(args[1]);
        return yes(inspection && presenter(std::move(*inspection)));
    }
    if (id == SetWindowTitle) {
        if ((size != 2 && size != 3) || !textValid(args.back(), 4096)) {
            return "False";
        }
        if (size == 3) {
            auto* canvas = dynamic_cast<SimCanvas*>(findObject(args[1]));
            return yes(canvas && canvas->setTitle(args[2]));
        }
        auto* set = dynamic_cast<SimCanvasSet*>(destination->findObject(CanvasSetId));
        if (!set) {
            return "False";
        }
        const std::vector<SimObject*> canvases(set->objects().begin(), set->objects().end());
        bool updated = false;
        std::ranges::for_each(canvases, [&](auto* object) {
            if (!object->isRemoved()) {
                updated = static_cast<SimCanvas*>(object)->setTitle(args[1]) || updated;
            }
        });
        return yes(updated);
    }
    if ((id == ObjectFocus || id == Unfocus) && size == 2) {
        auto* object = findObject(args[1]);
        if (!object || !object->getManager()) {
            return "False";
        }
        std::unique_ptr<SimEvent> event;
        if (id == ObjectFocus)
            event = std::make_unique<SimGainFocusEvent>();
        else
            event = std::make_unique<SimLoseFocusEvent>();
        return yes(object->postEvent(std::move(event), true));
    }
    if (id == LoadObject) {
        if (size != 3) {
            return "0";
        }
        auto* object = loadObject(args[1], args[2]);
        return object ? std::to_string(object->getId()) : "0";
    }
    if (id == StoreObject) {
        if (size != 3 && size != 4) {
            return "False";
        }
        auto* object = findObject(args[1]);
        Int32 user{};
        if (size == 4) {
            const auto r = std::from_chars(args[3].data(), args[3].data() + args[3].size(), user);
            if (r.ec != std::errc{} || r.ptr != args[3].data() + args[3].size()) {
                return "False";
            }
        }
        return yes(object && object != object->getManager() &&
                   storeObject(*object, console.filePath(args[2]), user));
    }
    if (id == RemoteEval) {
        if (size < 3 || size > 33) {
            return "False";
        }
        UInt32 peer{};
        const auto r = std::from_chars(args[1].data(), args[1].data() + args[1].size(), peer);
        if (r.ec != std::errc{} || r.ptr != args[1].data() + args[1].size() || !peer ||
            peer == destination->getId()) {
            return "False";
        }
        auto* stream = dynamic_cast<network::PacketStream*>(destination->findObject(peer));
        if (!stream) {
            return "False";
        }
        auto event = std::make_unique<SimConsoleEvent>();
        event->arguments.assign(args.begin() + 2, args.end());
        event->flags = SimEvent::Guaranteed | SimEvent::Ordered;
        return yes(event->valid() && stream->sendRemoteEvent(std::move(event)));
    }
    if (id == PurgeResources || id == GetPathOf || id == ListVolumes) {
        auto* resource = SimResource::find(*destination);
        if (!resource) {
            return "False";
        }
        if (id == PurgeResources) {
            if (size != 1) {
                return "False";
            }
            resource->get().purge(true);
            return "True";
        }
        if (id == GetPathOf) {
            if (size != 2) {
                return "";
            }
            const auto path = resource->get().getFullPath(args[1]);
            return path ? path->parent_path().string() : "";
        }
        if (size != 1) {
            return "False";
        }
        std::vector<std::string> lines;
        std::ranges::for_each(resource->objects(), [&](auto* p) {
            if (auto* v = dynamic_cast<SimVolume*>(p)) {
                lines.push_back("   \"" + std::string(v->getName()) + "\": \"" +
                                std::string(v->getFileName()) + "\"");
            }
        });
        std::ranges::for_each(lines, [&](const auto& line) {
            console.print(line);
        });
        return "True";
    }
    if (id == GetTime) {
        return size == 1 ? number(std::max(0.f, destination->getCurrentTime() - scriptTimeBase_))
                         : "False";
    }
    if (id == Schedule) {
        if (size != 3 && size != 4) {
            return "False";
        }
        auto delay = real(args[2]);
        auto* target =
            size == 4 ? findObject(args[3]) : destination->findObject(ConsoleSchedulerId);
        if (!delay || *delay > std::numeric_limits<float>::max() || !target ||
            !SimConsoleEvent::schedule(
                *target, args[1], static_cast<float>(std::max(0.0, *delay)))) {
            return "False";
        }
        return number(std::trunc(target->getManager()->getCurrentTime() + std::max(0.0, *delay)));
    }
    if (id == NewObject) {
        if (size < 3) {
            return "False";
        }
        auto* object = createObject(args[2], args[1], nullptr, args.subspan(3));
        return object ? std::to_string(object->getId()) : "False";
    }
    if (size < 2) {
        return "False";
    }
    auto* object = findObject(args[1]);
    if (id == IsObject) {
        return yes(size == 2 && object);
    }
    if (!object || object == object->getManager()) {
        return "False";
    }
    if (id == DeleteObject) {
        return yes(size == 2 && object->deleteObject());
    }
    if (id == RenameObject) {
        if (size != 3 || !textValid(args[2], 255)) {
            return "False";
        }
        object->assignName(args[2]);
        return "True";
    }
    if (id == SetObjectId) {
        if (size != 3 || !object->getManager()) {
            return "False";
        }
        UInt32 value{};
        const auto result = std::from_chars(args[2].data(), args[2].data() + args[2].size(), value);
        return yes(result.ec == std::errc{} && result.ptr == args[2].data() + args[2].size() &&
                   object->getManager()->assignId(*object, value));
    }
    if (id == GetGroup) {
        return size == 2 && object->getGroup() ? std::to_string(object->getGroup()->getId()) : "-1";
    }
    if (id == PostAction) {
        if (size != 4 || !object->getManager()) {
            return "False";
        }
        Int32 action{};
        const auto result =
            std::from_chars(args[2].data(), args[2].data() + args[2].size(), action);
        auto value = real(args[3]);
        if (result.ec != std::errc{} || result.ptr != args[2].data() + args[2].size()) {
            const auto name = canonical(args[2]);
            if (name == "attach") action = 103;
            else if (name == "nextcamera") action = 101;
            else return "False";
        }
        if (!value) {
            if (auto* reference = object->getManager()->findObject(args[3])) {
                value = double(reference->getId());
            }
        }
        if (!value ||
            std::abs(*value) > std::numeric_limits<float>::max()) {
            return "False";
        }
        SimActionEvent event;
        event.action = action;
        event.fValue = static_cast<float>(*value);
        event.device = ActionDigital;
        event.time = object->getManager()->getCurrentTime();
        return yes(object->getManager()->sendEvent(object->address(), event));
    }
    auto* set = dynamic_cast<SimSet*>(object);
    if (!set) {
        return "False";
    }
    if (id == AddToSet) {
        if (size < 3) {
            return "False";
        }
        bool success = true;
        std::ranges::for_each(args.subspan(2), [&](const auto& name) {
            auto* child = findObject(name);
            success &= !set->isRemoved() && child && set->addObject(*child);
        });
        return yes(success);
    }
    if (id == IsMember || id == GetNextObject) {
        if (size != 3) {
            return "False";
        }
        const auto child = findObject(args[2]);
        const auto members = set->objects();
        const auto name = canonical(args[2]);
        const auto found = std::ranges::find_if(members, [&](const auto* member) {
            return member == child || canonical(member->getName()) == name;
        });
        if (id == IsMember) {
            return yes(found != members.end());
        }
        if (found == members.end()) {
            return members.empty() ? "0" : std::to_string(members.front()->getId());
        }
        const auto next = found + 1;
        return next == members.end() ? "0" : std::to_string((*next)->getId());
    }
    if (id == ListObjects && size == 2) {
        std::vector<std::string> lines;
        std::ranges::transform(set->objects(), std::back_inserter(lines), [](const auto* child) {
            return std::to_string(child->getId()) + ": " + std::string(child->getName());
        });
        std::ranges::for_each(lines, [&](const auto& line) {
            console.print(line);
        });
        return "True";
    }
    return "False";
}
} // namespace opent1::sim
