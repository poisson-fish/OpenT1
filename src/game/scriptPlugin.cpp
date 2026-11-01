#include <opent1/game/script_plugin.hpp>
#include <opent1/game/observer_camera.hpp>
#include <cctype>
#include <charconv>

namespace opent1::game {
namespace {

[[nodiscard]] Int32 parseIntAscii(std::string_view sv) noexcept {
    while (!sv.empty() && std::isspace(static_cast<unsigned char>(sv.front())) != 0) {
        sv.remove_prefix(1);
    }
    Int32 out = 0;
    (void)std::from_chars(sv.data(), sv.data() + sv.size(), out);
    return out;
}

[[nodiscard]] Flt32 parseFloatAscii(std::string_view sv) noexcept {
    while (!sv.empty() && std::isspace(static_cast<unsigned char>(sv.front())) != 0) {
        sv.remove_prefix(1);
    }
    Flt32 out = 0.0f;
    (void)std::from_chars(sv.data(), sv.data() + sv.size(), out);
    return out;
}

Point3F parsePoint3F(std::string_view text) noexcept {
    Point3F pt{0.0f, 0.0f, 0.0f};
    Flt32* coords[3] = {&pt.x, &pt.y, &pt.z};
    for (Flt32* dst : coords) {
        while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front())) != 0) {
            text.remove_prefix(1);
        }
        if (text.empty()) {
            break;
        }
        const auto [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), *dst);
        if (ec != std::errc{}) {
            break;
        }
        text.remove_prefix(static_cast<std::size_t>(ptr - text.data()));
    }
    return pt;
}

bool parseConsoleBool(std::string_view text) noexcept {
    if (text.size() == 4) {
        if ((text[0] == 't' || text[0] == 'T') && (text[1] == 'r' || text[1] == 'R') &&
            (text[2] == 'u' || text[2] == 'U') && (text[3] == 'e' || text[3] == 'E')) {
            return true;
        }
    }
    if (text.size() == 5) {
        if ((text[0] == 'f' || text[0] == 'F') && (text[1] == 'a' || text[1] == 'A') &&
            (text[2] == 'l' || text[2] == 'L') && (text[3] == 's' || text[3] == 'S') &&
            (text[4] == 'e' || text[4] == 'E')) {
            return false;
        }
    }
    return parseIntAscii(text) != 0;
}

sim::SimObject* resolveGameObject(sim::SimManager* mgr, std::string_view nameOrId) noexcept {
    if (!mgr || nameOrId.empty()) {
        return nullptr;
    }
    if (auto* byName = mgr->findObject(nameOrId)) {
        return byName;
    }
    const auto id = static_cast<sim::SimObjectId>(parseIntAscii(nameOrId));
    if (id != 0) {
        return mgr->findObject(id);
    }
    return nullptr;
}

} // namespace

ScriptPlugin::ClientRepresentative& ScriptPlugin::ensureClient(Int32 clientId) {
    auto [it, inserted] = clients_.try_emplace(clientId);
    if (inserted) {
        it->second.clientId = clientId;
    }
    return it->second;
}

const ScriptPlugin::ClientRepresentative* ScriptPlugin::findClient(Int32 clientId) const noexcept {
    const auto it = clients_.find(clientId);
    return it != clients_.end() ? &it->second : nullptr;
}

ScriptPlugin::ClientRepresentative* ScriptPlugin::findClientMutable(Int32 clientId) noexcept {
    const auto it = clients_.find(clientId);
    return it != clients_.end() ? &it->second : nullptr;
}

void ScriptPlugin::init(Console& console, sim::SimManager* manager) {
    console_ = &console;
    manager_ = manager;

    console.print("ScriptPlugin");

    console.addCommand(0,
                       "spawnPlayer",
                       [this](Console&, Int32, std::span<const std::string> argv) -> std::string {
                           auto* srv = serverManager();
                           if (!srv || argv.size() != 4) {
                               return "-1";
                           }
                           const Point3F pos = parsePoint3F(argv[2]);
                           const Point3F rot = parsePoint3F(argv[3]);
                           if (spawnPlayer_) {
                               if (const auto id = spawnPlayer_(srv, argv[1], pos, rot)) {
                                   return std::to_string(*id);
                               }
                               return "-1";
                           }
                           if (clients_.empty()) {
                               return "-1";
                           }
                           auto obj = std::make_unique<sim::SimObject>();
                           obj->setFieldValue("armor", argv[1]);
                           obj->setFieldValue("position", argv[2]);
                           obj->setFieldValue("rotation", argv[3]);
                           sim::SimGroup* cleanup =
                               dynamic_cast<sim::SimGroup*>(srv->findObject("MissionCleanup"));
                           auto* added = srv->addObject(std::move(obj), cleanup);
                           if (!added) {
                               return "-1";
                           }
                           return std::to_string(added->getId());
                       });

    console.addCommand(0,
                       "Client::getObserverCamera",
                       [this](Console&, Int32, std::span<const std::string> argv) -> std::string {
                           auto* srv = serverManager();
                           if (!srv || argv.size() != 2) {
                               return "-1";
                           }
                           const Int32 clientId = parseIntAscii(argv[1]);
                           auto* cl = findClient(clientId);
                           if (!cl) {
                               return "-1";
                           }
                           const auto* camera =
                               dynamic_cast<ObserverCamera*>(srv->findObject(cl->observerCameraId));
                           if (!camera || camera->clientId() != clientId) {
                               auto* cleanup =
                                   dynamic_cast<sim::SimGroup*>(srv->findObject("MissionCleanup"));
                               auto replacement = std::make_unique<ObserverCamera>(clientId);
                               auto* added = srv->addObject(std::move(replacement), cleanup);
                               ensureClient(clientId).observerCameraId = added ? added->getId() : 0;
                           }
                           if (!cl->observerCameraId) {
                               return "-1";
                           }
                           return std::to_string(cl->observerCameraId);
                       });

    console.addCommand(0,
                       "Observer::setOrbitObject",
                       [this](Console&, Int32, std::span<const std::string> argv) -> std::string {
                           auto* srv = serverManager();
                           if (!srv || argv.size() < 6) {
                               return "false";
                           }
                           const Int32 clientId = parseIntAscii(argv[1]);
                           auto* cl = findClientMutable(clientId);
                           if (!cl || cl->observerCameraId == 0) {
                               return "false";
                           }
                           auto* target = resolveGameObject(srv, argv[2]);
                           if (!target) {
                               return "false";
                           }
                           cl->observerMode = ObserverMode::OrbitObject;
                           cl->observerOrbitObjectId = target->getId();
                           cl->observerPos = Point3F{0.0f, 0.0f, 0.0f};
                           cl->curOrbitDist = parseFloatAscii(argv[3]);
                           cl->minOrbitDist = parseFloatAscii(argv[4]);
                           cl->maxOrbitDist = parseFloatAscii(argv[5]);
                           cl->observerRot = (argv.size() == 7) ? parsePoint3F(argv[6])
                                                                : Point3F{0.0f, 0.0f, 0.0f};
                           if (auto* camera = dynamic_cast<ObserverCamera*>(
                                   srv->findObject(cl->observerCameraId))) {
                               auto* object = dynamic_cast<GameBase*>(target);
                               if (!object) {
                                   return "false";
                               }
                               camera->setOrbitMode(
                                   object,
                                   cl->observerPos,
                                   EulerF{cl->observerRot.x, cl->observerRot.y, cl->observerRot.z},
                                   cl->minOrbitDist,
                                   cl->maxOrbitDist,
                                   cl->curOrbitDist);
                           }
                           return "True";
                       });

    console.addCommand(0,
                       "Observer::setOrbitPoint",
                       [this](Console&, Int32, std::span<const std::string> argv) -> std::string {
                           auto* srv = serverManager();
                           if (!srv || argv.size() < 6) {
                               return "false";
                           }
                           const Int32 clientId = parseIntAscii(argv[1]);
                           auto* cl = findClientMutable(clientId);
                           if (!cl || cl->observerCameraId == 0) {
                               return "false";
                           }
                           cl->observerMode = ObserverMode::OrbitPoint;
                           cl->observerOrbitObjectId = 0;
                           cl->observerPos = parsePoint3F(argv[2]);
                           cl->curOrbitDist = parseFloatAscii(argv[3]);
                           cl->minOrbitDist = parseFloatAscii(argv[4]);
                           cl->maxOrbitDist = parseFloatAscii(argv[5]);
                           cl->observerRot = (argv.size() == 7) ? parsePoint3F(argv[6])
                                                                : Point3F{0.0f, 0.0f, 0.0f};
                           return "True";
                       });

    console.addCommand(0,
                       "Observer::setFlyMode",
                       [this](Console&, Int32, std::span<const std::string> argv) -> std::string {
                           auto* srv = serverManager();
                           if (!srv || argv.size() != 6) {
                               return "false";
                           }
                           const Int32 clientId = parseIntAscii(argv[1]);
                           auto* cl = findClientMutable(clientId);
                           if (!cl || cl->observerCameraId == 0) {
                               return "false";
                           }
                           cl->observerMode = ObserverMode::Fly;
                           cl->observerPos = parsePoint3F(argv[2]);
                           cl->observerRot = parsePoint3F(argv[3]);
                           cl->canMove = parseConsoleBool(argv[4]);
                           cl->canRotate = parseConsoleBool(argv[5]);
                           return "True";
                       });

    console.addCommand(0,
                       "Observer::setDamageObject",
                       [this](Console&, Int32, std::span<const std::string> argv) -> std::string {
                           auto* srv = serverManager();
                           if (!srv || argv.size() != 3) {
                               return "false";
                           }
                           const Int32 clientId = parseIntAscii(argv[1]);
                           auto* cl = findClientMutable(clientId);
                           if (!cl || cl->observerCameraId == 0) {
                               return "false";
                           }
                           auto* target = resolveGameObject(srv, argv[2]);
                           if (!target) {
                               return "false";
                           }
                           cl->damageObjectId = target->getId();
                           return "true";
                       });

    console.addCommand(0,
                       "Client::toggleCMDObserver",
                       [this](Console&, Int32, std::span<const std::string> argv) -> std::string {
                           if (cmdObserveAvailable_ && argv.size() == 2) {
                               cmdObserveActive_ = parseConsoleBool(argv[1]);
                               return "True";
                           }
                           return "False";
                       });

    console.addCommand(0,
                       "Client::cmdObservePlayer",
                       [this](Console&, Int32, std::span<const std::string> argv) -> std::string {
                           if (cmdObserveAvailable_ && argv.size() == 2) {
                               const Int32 playerId = parseIntAscii(argv[1]);
                               if (findClient(playerId) != nullptr) {
                                   cmdObservedPlayer_ = playerId;
                                   return "True";
                               }
                           }
                           return "False";
                       });

    console.bindVariable("pref::HrlmDetail", hrlmDetail_);
}

void ScriptPlugin::shutdown() noexcept {
    if (console_) {
        console_->removeCommand("spawnPlayer");
        console_->removeCommand("Client::getObserverCamera");
        console_->removeCommand("Observer::setOrbitObject");
        console_->removeCommand("Observer::setOrbitPoint");
        console_->removeCommand("Observer::setFlyMode");
        console_->removeCommand("Observer::setDamageObject");
        console_->removeCommand("Client::toggleCMDObserver");
        console_->removeCommand("Client::cmdObservePlayer");
        console_->removeVariable("pref::HrlmDetail");
    }
    console_ = nullptr;
    manager_ = nullptr;
}

} // namespace opent1::game
