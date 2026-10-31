#pragma once
#include <opent1/sim/sim_world_game.hpp>

namespace opent1::sim {
enum class RedbookStatus { NoDevice, DeviceNotReady, Playing, Stopped, Paused, TrayOpened };

class RedbookBackend {
public:
    virtual ~RedbookBackend() = default;
    virtual bool open() = 0;
    virtual void close() = 0;
    virtual RedbookStatus status() = 0;
    virtual UInt32 trackCount() = 0;
    virtual bool track(UInt32, UInt32& startMs, UInt32& lengthMs) = 0;
    virtual UInt32 position() = 0;
    virtual bool play(UInt32 fromMs, UInt32 toMs) = 0;
    virtual bool stop() = 0;
    virtual bool pause() = 0;
    virtual bool eject(bool openTray) = 0;
    // Native Redbook uses attenuation: zero is full volume, one is silence.
    virtual bool setVolume(Flt32 attenuation) = 0;
    virtual Flt32 volume() = 0;
    virtual std::string error() const = 0;
};

std::unique_ptr<RedbookBackend> makeNativeRedbookBackend();

class Redbook final : public SimObject {
public:
    explicit Redbook(std::unique_ptr<RedbookBackend> = makeNativeRedbookBackend());
    ~Redbook() override;
    bool onAdd() override;

    void onRemove() override {
        close();
    }

    bool processEvent(const SimEvent&) override;
    bool open(SimCanvas&);
    void close();
    RedbookStatus getStatus();
    UInt32 getTrackCount();
    bool getTrackInfo(UInt32, Flt32& seconds);
    Flt32 getTrackPosition();
    bool play(UInt32 track);
    bool playAll();
    bool stop();
    bool pause();
    bool resume();
    bool eject();
    bool retract();

    void setPlayMode(Int32 mode) {
        mode_ = mode;
    }

    Int32 getPlayMode() const {
        return mode_;
    }

    bool setVolume(Flt32 value);

    Flt32 getVolume() {
        return backend_->volume();
    }

    std::string error() const {
        return backend_->error();
    }

private:
    std::unique_ptr<RedbookBackend> backend_;
    UInt32 start_{}, pausedPosition_{}, end_{}, track_{};
    Int32 mode_{};
    bool active_{}, paused_{};
};

enum class MovieStatus { NoDevice, NoMovie, Stopped, Playing, Paused, Completed };

class MovieBackend {
public:
    virtual ~MovieBackend() = default;
    virtual MovieStatus status() = 0;
    virtual bool open(std::string_view path) = 0;
    virtual void close() = 0;
    virtual bool play() = 0;
    virtual bool pause() = 0;
    virtual bool stop() = 0;
    virtual void waitForEvent() = 0;
    virtual std::string error() const = 0;
};

// Full-screen native renderer is the production default. Hidden rendering is
// an explicit host/test option; decoding and graph events remain real.
std::unique_ptr<MovieBackend> makeNativeMovieBackend(bool fullScreen = true);

class MovPlay final : public SimObject {
public:
    explicit MovPlay(std::unique_ptr<MovieBackend> = makeNativeMovieBackend());
    ~MovPlay() override;
    bool onAdd() override;

    void onRemove() override {
        closeMovie();
    }

    bool processEvent(const SimEvent&) override;

    MovieStatus getStatus() {
        return backend_->status();
    }

    bool openMovie(std::string_view path) {
        return backend_->open(path);
    }

    void closeMovie() {
        backend_->close();
    }

    bool play() {
        return backend_->play();
    }

    bool playToCompletion();

    bool stop() {
        return backend_->stop();
    }

    bool pause() {
        return backend_->pause();
    }

    std::string error() const {
        return backend_->error();
    }

private:
    std::unique_ptr<MovieBackend> backend_;
};

class NativeMediaPlugin final : public SimConsolePlugin {
public:
    NativeMediaPlugin();
    ~NativeMediaPlugin() override;
    void init(Console&, SimManager*) override;
    void setManager(SimManager*) override;
    void shutdown() noexcept override;

private:
    struct Binding;
    std::shared_ptr<Binding> binding_;
};
}
