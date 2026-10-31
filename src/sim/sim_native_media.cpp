#include <opent1/sim/sim_native_media.hpp>
#include <charconv>
#include <cmath>

namespace opent1::sim {
namespace {
bool frameMember(SimObject& object) {
    if (!object.getManager()) {
        return false;
    }
    if (auto* set = dynamic_cast<SimSet*>(object.getManager()->findObject(FrameEndNotifySetId))) {
        return set->addObject(object);
    }
    return true;
}

template <class T> std::optional<T> parse(std::string_view text) {
    T value{};
    const auto r = std::from_chars(text.data(), text.data() + text.size(), value);
    return r.ec == std::errc{} && r.ptr == text.data() + text.size() ? std::optional<T>{value}
                                                                     : std::nullopt;
}

enum Command {
    NewRedbook,
    RbOpen,
    RbClose,
    RbEject,
    RbRetract,
    RbStatus,
    RbCount,
    RbInfo,
    RbPosition,
    RbPlay,
    RbStop,
    RbPause,
    RbResume,
    RbVolume,
    RbGetVolume,
    RbMode,
    NewMovie,
    OpenMovie,
    CloseMovie,
    PlayMovie,
    PlayMovieToComp,
    StopMovie,
    PauseMovie
};
}

Redbook::Redbook(std::unique_ptr<RedbookBackend> backend) : backend_(std::move(backend)) {}

Redbook::~Redbook() {
    close();
}

bool Redbook::onAdd() {
    return frameMember(*this);
}

bool Redbook::open(SimCanvas&) {
    close();
    return backend_->open();
}

void Redbook::close() {
    active_ = paused_ = false;
    backend_->close();
}

RedbookStatus Redbook::getStatus() {
    const auto state = backend_->status();
    return paused_ && state == RedbookStatus::Stopped ? RedbookStatus::Paused : state;
}

UInt32 Redbook::getTrackCount() {
    return backend_->trackCount();
}

bool Redbook::getTrackInfo(UInt32 track, Flt32& seconds) {
    UInt32 start{}, length{};
    if (!backend_->track(track, start, length)) {
        return false;
    }
    seconds = Flt32(length) * .001f;
    return true;
}

Flt32 Redbook::getTrackPosition() {
    const auto state = getStatus();
    if (state != RedbookStatus::Playing && state != RedbookStatus::Paused) {
        return 0;
    }
    const auto position = state == RedbookStatus::Paused ? pausedPosition_ : backend_->position();
    const auto seconds = Flt32(position - start_) * .001f;
    return seconds > 4380 ? 0 : seconds;
}

bool Redbook::play(UInt32 track) {
    UInt32 from{}, length{};
    if (!backend_->track(track, from, length) || length > UINT32_MAX - from ||
        !backend_->play(from, from + length)) {
        return false;
    }
    track_ = track;
    start_ = pausedPosition_ = from;
    end_ = from + length;
    paused_ = false;
    active_ = true;
    return true;
}

bool Redbook::playAll() {
    UInt32 from{}, length{};
    if (!backend_->track(1, from, length)) {
        return false;
    }
    UInt32 end = from;
    const auto count = getTrackCount();
    // Preserve the shipped range: track lengths [1, count), not the final track.
    for (UInt32 i = 1; i < count; ++i) {
        UInt32 unused{};
        if (!backend_->track(i, unused, length) || length > UINT32_MAX - end) {
            return false;
        }
        end += length;
    }
    if (!backend_->play(from, end)) {
        return false;
    }
    start_ = pausedPosition_ = from;
    end_ = end;
    paused_ = false;
    active_ = true;
    return true;
}

bool Redbook::stop() {
    active_ = paused_ = false;
    return backend_->stop();
}

bool Redbook::pause() {
    if (getStatus() != RedbookStatus::Playing) {
        return false;
    }
    pausedPosition_ = backend_->position();
    if (!backend_->pause()) {
        return false;
    }
    paused_ = true;
    return true;
}

bool Redbook::resume() {
    if (!paused_ || !backend_->play(pausedPosition_, end_)) {
        return false;
    }
    paused_ = false;
    active_ = true;
    return true;
}

bool Redbook::eject() {
    active_ = paused_ = false;
    return backend_->eject(true);
}

bool Redbook::retract() {
    return backend_->eject(false);
}

bool Redbook::setVolume(Flt32 value) {
    return std::isfinite(value) && value >= 0 && value <= 1 && backend_->setVolume(value);
}

bool Redbook::processEvent(const SimEvent& event) {
    if (event.type != EventTypeFirst + 6) {
        return SimObject::processEvent(event);
    }
    if (active_ && !paused_ && getStatus() == RedbookStatus::Stopped) {
        active_ = false;
        if (mode_ == 1) {
            (void)play(track_);
        } else if (mode_ == 2) {
            (void)playAll();
        }
    }
    return true;
}

MovPlay::MovPlay(std::unique_ptr<MovieBackend> backend) : backend_(std::move(backend)) {}

MovPlay::~MovPlay() {
    closeMovie();
}

bool MovPlay::onAdd() {
    return frameMember(*this);
}

bool MovPlay::processEvent(const SimEvent& event) {
    if (event.type != EventTypeFirst + 6) {
        return SimObject::processEvent(event);
    }
    (void)getStatus();
    return true;
}

bool MovPlay::playToCompletion() {
    if (!play()) {
        return false;
    }
    while (getStatus() == MovieStatus::Playing) {
        backend_->waitForEvent();
    }
    return getStatus() == MovieStatus::Completed;
}

struct NativeMediaPlugin::Binding {
    SimManager* manager{};

    std::optional<std::string>
    command(Console& console, Int32 id, std::span<const std::string> args) {
        if (!manager || args.size() < 2) {
            return "False";
        }
        if (id == NewRedbook || id == NewMovie) {
            if (manager->findObject(args[1])) {
                return "False";
            }
            if (id == NewRedbook && args.size() == 3) {
                auto* canvas = dynamic_cast<SimCanvas*>(manager->findObject(args[2]));
                if (!canvas) {
                    return "False";
                }
                auto object = std::make_unique<Redbook>();
                object->assignName(args[1]);
                if (!object->open(*canvas) || object->getStatus() != RedbookStatus::Stopped) {
                    console.print(object->error());
                    return "False";
                }
                return manager->addObject(std::move(object)) ? "True" : "False";
            }
            if (id == NewMovie && args.size() == 2) {
                auto object = std::make_unique<MovPlay>();
                object->assignName(args[1]);
                if (object->getStatus() == MovieStatus::NoDevice) {
                    console.print(object->error());
                    return "False";
                }
                return manager->addObject(std::move(object)) ? std::optional<std::string>{}
                                                             : std::optional<std::string>{"False"};
            }
            return "False";
        }
        bool ok = false;
        std::string output, error;
        if (id < NewMovie) {
            auto* object = dynamic_cast<Redbook*>(manager->findObject(args[1]));
            if (!object) {
                return "False";
            }
            if (id == RbOpen && args.size() == 3) {
                if (auto* canvas = dynamic_cast<SimCanvas*>(manager->findObject(args[2]))) {
                    ok = object->open(*canvas);
                }
            } else if (args.size() == 2) {
                switch (id) {
                    case RbClose:
                        object->close();
                        ok = true;
                        break;
                    case RbEject:
                        ok = object->eject();
                        break;
                    case RbRetract:
                        ok = object->retract();
                        break;
                    case RbStatus: {
                        static constexpr const char* names[]{"No device mapped to Redbook object",
                                                             "Device is not ready",
                                                             "CD is currently being played",
                                                             "CD is stopped",
                                                             "CD is paused",
                                                             "CD tray is opened"};
                        output = names[static_cast<unsigned>(object->getStatus())];
                        ok = true;
                        break;
                    }
                    case RbCount:
                        output = "Number of tracks: " + std::to_string(object->getTrackCount());
                        ok = true;
                        break;
                    case RbPosition:
                        output = "Current track position is " +
                                 std::to_string(object->getTrackPosition());
                        ok = true;
                        break;
                    case RbPlay:
                        object->setPlayMode(2);
                        ok = object->playAll();
                        break;
                    case RbStop:
                        ok = object->stop();
                        break;
                    case RbPause:
                        ok = object->pause();
                        break;
                    case RbResume:
                        ok = object->resume();
                        break;
                    case RbGetVolume:
                        output = "CD audio volume is " + std::to_string(object->getVolume());
                        ok = true;
                        break;
                    default:
                        break;
                }
            } else if (args.size() == 3) {
                if (id == RbVolume) {
                    if (const auto value = parse<Flt32>(args[2])) {
                        ok = object->setVolume(*value);
                    }
                } else if (id == RbMode) {
                    if (const auto value = parse<Int32>(args[2])) {
                        object->setPlayMode(*value);
                        ok = true;
                    }
                } else if (const auto value = parse<UInt32>(args[2])) {
                    if (id == RbInfo) {
                        Flt32 length{};
                        ok = object->getTrackInfo(*value, length);
                        if (ok) {
                            output = "Track is " + std::to_string(length) + " seconds long";
                        }
                    } else if (id == RbPlay) {
                        ok = object->getPlayMode() == 2 ? object->playAll() : object->play(*value);
                    }
                }
            }
            error = object->error();
        } else {
            auto* object = dynamic_cast<MovPlay*>(manager->findObject(args[1]));
            if (!object) {
                return "False";
            }
            if (id == OpenMovie && args.size() == 3) {
                ok = object->openMovie(args[2]);
            } else if (args.size() == 2) {
                switch (id) {
                    case CloseMovie:
                        object->closeMovie();
                        ok = true;
                        break;
                    case PlayMovie:
                        ok = object->play();
                        break;
                    case PlayMovieToComp:
                        ok = object->playToCompletion();
                        break;
                    case StopMovie:
                        ok = object->stop();
                        break;
                    case PauseMovie:
                        ok = object->pause();
                        break;
                    default:
                        break;
                }
            }
            error = object->error();
        }
        // Copy values before console sinks can delete the object or its world.
        if (!output.empty()) {
            console.print(output);
        }
        if (!ok && !error.empty()) {
            console.print(error);
        }
        if (id >= CloseMovie) {
            return "False";
        } // Shipped movie control commands always return False.
        if (id == RbOpen && ok) {
            return "True";
        }
        return ok ? std::optional<std::string>{} : std::optional<std::string>{"False"};
    }
};

NativeMediaPlugin::NativeMediaPlugin() : binding_(std::make_shared<Binding>()) {}

NativeMediaPlugin::~NativeMediaPlugin() {
    shutdown();
}

void NativeMediaPlugin::setManager(SimManager* manager) {
    binding_->manager = manager;
}

void NativeMediaPlugin::shutdown() noexcept {
    binding_->manager = nullptr;
}

void NativeMediaPlugin::init(Console& console, SimManager* manager) {
    setManager(manager);
    const std::weak_ptr<Binding> weak = binding_;
    for (const auto& [id, name] :
         std::to_array<std::pair<Int32, std::string_view>>({{NewRedbook, "newRedbook"},
                                                            {RbOpen, "rbOpen"},
                                                            {RbClose, "rbClose"},
                                                            {RbEject, "rbEject"},
                                                            {RbRetract, "rbRetract"},
                                                            {RbStatus, "rbGetStatus"},
                                                            {RbCount, "rbGetTrackCount"},
                                                            {RbInfo, "rbGetTrackInfo"},
                                                            {RbPosition, "rbGetTrackPosition"},
                                                            {RbPlay, "rbPlay"},
                                                            {RbStop, "rbStop"},
                                                            {RbPause, "rbPause"},
                                                            {RbResume, "rbResume"},
                                                            {RbVolume, "rbSetVolume"},
                                                            {RbGetVolume, "rbGetVolume"},
                                                            {RbMode, "rbSetPlayMode"},
                                                            {NewMovie, "newMovPlay"},
                                                            {OpenMovie, "openMovie"},
                                                            {CloseMovie, "closeMovie"},
                                                            {PlayMovie, "playMovie"},
                                                            {PlayMovieToComp, "playMovieToComp"},
                                                            {StopMovie, "stopMovie"},
                                                            {PauseMovie, "pauseMovie"}})) {
        console.addCommand(id,
                           name,
                           [weak](Console& c,
                                  Int32 command,
                                  std::span<const std::string> args) -> std::optional<std::string> {
                               const auto state = weak.lock();
                               return state ? state->command(c, command, args)
                                            : std::optional<std::string>{"False"};
                           });
    }
}
}
