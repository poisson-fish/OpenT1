#include <opent1/sim/sim_native_media.hpp>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <mutex>
#ifdef _WIN32
#include <windows.h>
#include <mmsystem.h>
#include <dshow.h>
#endif

namespace opent1::sim {
namespace {
#ifdef _WIN32
// A shared lease restores the original CD mixer value only after the final
// open Redbook device is closed, matching the original global mixer lifetime.
class CdVolume {
public:
    CdVolume() {
        for (UINT i = mixerGetNumDevs(); i > 0; --i) {
            if (mixerOpen(&mixer_, i - 1, 0, 0, 0) != MMSYSERR_NOERROR) {
                continue;
            }
            MIXERLINEW line{};
            line.cbStruct = sizeof(line);
            line.dwComponentType = MIXERLINE_COMPONENTTYPE_SRC_COMPACTDISC;
            MIXERCONTROLW control{};
            control.cbStruct = sizeof(control);
            MIXERLINECONTROLSW controls{};
            controls.cbStruct = sizeof(controls);
            controls.dwControlType = MIXERCONTROL_CONTROLTYPE_VOLUME;
            controls.cControls = 1;
            controls.cbmxctrl = sizeof(control);
            controls.pamxctrl = &control;
            if (mixerGetLineInfoW(reinterpret_cast<HMIXEROBJ>(mixer_),
                                  &line,
                                  MIXER_GETLINEINFOF_COMPONENTTYPE) == MMSYSERR_NOERROR) {
                controls.dwLineID = line.dwLineID;
                if (mixerGetLineControlsW(reinterpret_cast<HMIXEROBJ>(mixer_),
                                          &controls,
                                          MIXER_GETLINECONTROLSF_ONEBYTYPE) == MMSYSERR_NOERROR) {
                    details_.cbStruct = sizeof(details_);
                    details_.dwControlID = control.dwControlID;
                    details_.cChannels = 1;
                    details_.cbDetails = sizeof(value_);
                    details_.paDetails = &value_;
                    if (mixerGetControlDetailsW(reinterpret_cast<HMIXEROBJ>(mixer_),
                                                &details_,
                                                MIXER_GETCONTROLDETAILSF_VALUE) ==
                        MMSYSERR_NOERROR) {
                        original_ = value_.dwValue;
                        available_ = true;
                        return;
                    }
                }
            }
            mixerClose(mixer_);
            mixer_ = nullptr;
        }
        for (UINT i = auxGetNumDevs(); i > 0; --i) {
            AUXCAPSW caps{};
            if (auxGetDevCapsW(i - 1, &caps, sizeof(caps)) == MMSYSERR_NOERROR &&
                caps.wTechnology == AUXCAPS_CDAUDIO && (caps.dwSupport & AUXCAPS_VOLUME) &&
                auxGetVolume(i - 1, &original_) == MMSYSERR_NOERROR) {
                aux_ = i - 1;
                available_ = true;
                return;
            }
        }
    }

    ~CdVolume() {
        if (!available_) {
            return;
        }
        if (mixer_) {
            value_.dwValue = original_;
            mixerSetControlDetails(
                reinterpret_cast<HMIXEROBJ>(mixer_), &details_, MIXER_SETCONTROLDETAILSF_VALUE);
            mixerClose(mixer_);
        } else {
            auxSetVolume(aux_, original_);
        }
    }

    bool set(Flt32 attenuation) {
        if (!available_) {
            return false;
        }
        DWORD value = static_cast<DWORD>((1 - attenuation) * 127 * 516);
        if (mixer_) {
            value_.dwValue = value;
            return mixerSetControlDetails(reinterpret_cast<HMIXEROBJ>(mixer_),
                                          &details_,
                                          MIXER_SETCONTROLDETAILSF_VALUE) == MMSYSERR_NOERROR;
        }
        return auxSetVolume(aux_, value | (value << 16)) == MMSYSERR_NOERROR;
    }

    Flt32 get() {
        if (!available_) {
            return 1;
        }
        DWORD value{};
        if (mixer_) {
            if (mixerGetControlDetailsW(reinterpret_cast<HMIXEROBJ>(mixer_),
                                        &details_,
                                        MIXER_GETCONTROLDETAILSF_VALUE) != MMSYSERR_NOERROR) {
                return 1;
            }
            value = value_.dwValue;
        } else if (auxGetVolume(aux_, &value) != MMSYSERR_NOERROR) {
            return 1;
        }
        return std::clamp(1 - Flt32((value & 0xffff) >> 9) / 127, 0.0f, 1.0f);
    }

private:
    HMIXER mixer_{};
    UINT aux_{};
    bool available_{};
    DWORD original_{};
    MIXERCONTROLDETAILS details_{};
    MIXERCONTROLDETAILS_UNSIGNED value_{};
};

std::shared_ptr<CdVolume> cdVolume() {
    static std::mutex mutex;
    static std::weak_ptr<CdVolume> shared;
    std::scoped_lock lock(mutex);
    auto value = shared.lock();
    if (!value) {
        value = std::make_shared<CdVolume>();
        shared = value;
    }
    return value;
}

class NativeRedbook final : public RedbookBackend {
public:
    ~NativeRedbook() override {
        close();
    }

    bool open() override {
        close();
        MCI_OPEN_PARMSW p{};
        p.lpstrDeviceType = L"cdaudio";
        auto result = mciSendCommandW(0, MCI_OPEN, MCI_OPEN_TYPE, reinterpret_cast<DWORD_PTR>(&p));
        if (result) {
            result = mciSendCommandW(
                0, MCI_OPEN, MCI_OPEN_TYPE | MCI_OPEN_SHAREABLE, reinterpret_cast<DWORD_PTR>(&p));
        }
        if (!checked(result)) {
            return false;
        }
        device_ = p.wDeviceID;
        MCI_SET_PARMS format{};
        format.dwTimeFormat = MCI_FORMAT_MILLISECONDS;
        if (!command(MCI_SET, MCI_SET_TIME_FORMAT, reinterpret_cast<DWORD_PTR>(&format))) {
            close();
            return false;
        }
        volume_ = cdVolume();
        return true;
    }

    void close() override {
        if (device_) {
            mciSendCommandW(device_, MCI_CLOSE, MCI_WAIT, 0);
            device_ = 0;
        }
        volume_.reset();
    }

    RedbookStatus status() override {
        if (!device_) {
            return RedbookStatus::NoDevice;
        }
        DWORD_PTR mode{};
        if (!query(MCI_STATUS_MODE, 0, mode)) {
            return RedbookStatus::DeviceNotReady;
        }
        switch (mode) {
            case MCI_MODE_PLAY:
                return RedbookStatus::Playing;
            case MCI_MODE_PAUSE:
                return RedbookStatus::Paused;
            case MCI_MODE_STOP:
                return RedbookStatus::Stopped;
            case MCI_MODE_OPEN:
                return RedbookStatus::TrayOpened;
            default:
                return RedbookStatus::DeviceNotReady;
        }
    }

    UInt32 trackCount() override {
        DWORD_PTR count{};
        return query(MCI_STATUS_NUMBER_OF_TRACKS, 0, count) ? static_cast<UInt32>(count) : 0;
    }

    bool track(UInt32 track, UInt32& from, UInt32& length) override {
        if (!track) {
            return false;
        }
        DWORD_PTR a{}, b{};
        if (!query(MCI_STATUS_POSITION, track, a) || !query(MCI_STATUS_LENGTH, track, b)) {
            return false;
        }
        from = static_cast<UInt32>(a);
        length = static_cast<UInt32>(b);
        return true;
    }

    UInt32 position() override {
        DWORD_PTR value{};
        return query(MCI_STATUS_POSITION, 0, value) ? static_cast<UInt32>(value) : 0;
    }

    bool play(UInt32 from, UInt32 to) override {
        MCI_PLAY_PARMS p{};
        p.dwFrom = from;
        p.dwTo = to;
        return command(MCI_PLAY, MCI_FROM | MCI_TO, reinterpret_cast<DWORD_PTR>(&p));
    }

    bool stop() override {
        return command(MCI_STOP, MCI_WAIT, 0);
    }

    bool pause() override {
        return command(MCI_PAUSE, MCI_WAIT, 0);
    }

    bool eject(bool opened) override {
        return command(MCI_SET, opened ? MCI_SET_DOOR_OPEN : MCI_SET_DOOR_CLOSED, 0);
    }

    bool setVolume(Flt32 value) override {
        if (!std::isfinite(value) || value < 0 || value > 1 || !volume_ || !volume_->set(value)) {
            error_ = "CD audio mixer/aux volume control is unavailable";
            return false;
        }
        return true;
    }

    Flt32 volume() override {
        return volume_ ? volume_->get() : 1;
    }

    std::string error() const override {
        return error_;
    }

private:
    bool checked(MCIERROR result) {
        if (!result) {
            error_.clear();
            return true;
        }
        char message[256]{};
        mciGetErrorStringA(result, message, sizeof(message));
        error_ = "CD audio: " + std::string(message) + " (MCI " + std::to_string(result) + ")";
        return false;
    }

    bool command(UINT message, DWORD_PTR flags, DWORD_PTR data) {
        if (!device_) {
            error_ = "No CD audio device is open";
            return false;
        }
        return checked(mciSendCommandW(device_, message, flags, data));
    }

    bool query(DWORD item, UInt32 track, DWORD_PTR& value) {
        MCI_STATUS_PARMS p{};
        p.dwItem = item;
        p.dwTrack = track;
        if (!command(MCI_STATUS,
                     MCI_STATUS_ITEM | MCI_WAIT | (track ? MCI_TRACK : 0),
                     reinterpret_cast<DWORD_PTR>(&p))) {
            return false;
        }
        value = p.dwReturn;
        return true;
    }

    MCIDEVICEID device_{};
    std::shared_ptr<CdVolume> volume_;
    std::string error_;
};

template <class T> struct ComPointer {
    T* value{};

    ~ComPointer() {
        reset();
    }

    void reset() {
        if (value) {
            value->Release();
            value = nullptr;
        }
    }

    T* operator->() const {
        return value;
    }
};

class NativeMovie final : public MovieBackend {
public:
    explicit NativeMovie(bool fullscreen) : fullScreen_(fullscreen) {
        const auto hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        initialized_ = SUCCEEDED(hr);
        if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) {
            checked(hr, "COM initialization");
            return;
        }
        ComPointer<IGraphBuilder> probe;
        if (checked(CoCreateInstance(CLSID_FilterGraph,
                                     nullptr,
                                     CLSCTX_INPROC_SERVER,
                                     IID_IGraphBuilder,
                                     reinterpret_cast<void**>(&probe.value)),
                    "DirectShow graph creation")) {
            state_ = MovieStatus::NoMovie;
        }
    }

    ~NativeMovie() override {
        close();
        if (initialized_) {
            CoUninitialize();
        }
    }

    bool open(std::string_view path) override {
        if (state_ != MovieStatus::NoMovie || path.empty() || path.find('\0') != path.npos ||
            path.size() > static_cast<std::size_t>(INT_MAX)) {
            return false;
        }
        if (!checked(CoCreateInstance(CLSID_FilterGraph,
                                      nullptr,
                                      CLSCTX_INPROC_SERVER,
                                      IID_IGraphBuilder,
                                      reinterpret_cast<void**>(&graph_.value)),
                     "DirectShow graph creation")) {
            return false;
        }
        const auto fail = [&] {
            close();
            return false;
        };
        if (!checked(
                graph_->QueryInterface(IID_IMediaEvent, reinterpret_cast<void**>(&events_.value)),
                "DirectShow event interface") ||
            !checked(graph_->QueryInterface(IID_IMediaControl,
                                            reinterpret_cast<void**>(&control_.value)),
                     "DirectShow control interface")) {
            return fail();
        }
        const auto count = MultiByteToWideChar(
            CP_UTF8, MB_ERR_INVALID_CHARS, path.data(), static_cast<int>(path.size()), nullptr, 0);
        if (!count) {
            error_ = "Movie path is not valid UTF-8";
            return fail();
        }
        std::wstring name(static_cast<std::size_t>(count), L'\0');
        MultiByteToWideChar(CP_UTF8,
                            MB_ERR_INVALID_CHARS,
                            path.data(),
                            static_cast<int>(path.size()),
                            name.data(),
                            count);
        foreground_ = fullScreen_ ? GetForegroundWindow() : nullptr;
        if (!checked(graph_->RenderFile(name.c_str(), nullptr), "DirectShow RenderFile")) {
            return fail();
        }
        if (!checked(
                graph_->QueryInterface(IID_IVideoWindow, reinterpret_cast<void**>(&window_.value)),
                "DirectShow video window")) {
            return fail();
        }
        window_->put_AutoShow(fullScreen_ ? OATRUE : OAFALSE);
        if (fullScreen_) {
            if (!checked(window_->put_FullScreenMode(OATRUE), "DirectShow full screen")) {
                return fail();
            }
        } else {
            window_->put_Visible(OAFALSE);
        }
        state_ = MovieStatus::Stopped;
        return true;
    }

    void close() override {
        if (control_.value) {
            control_->Stop();
        }
        if (window_.value) {
            window_->put_FullScreenMode(OAFALSE);
            window_->put_Visible(OAFALSE);
            window_->put_Owner(0);
        }
        window_.reset();
        events_.reset();
        control_.reset();
        graph_.reset();
        if (state_ != MovieStatus::NoDevice) {
            state_ = MovieStatus::NoMovie;
        }
        if (foreground_) {
            SetForegroundWindow(foreground_);
            foreground_ = nullptr;
        }
    }

    MovieStatus status() override {
        if (!events_.value || state_ == MovieStatus::Completed) {
            return state_;
        }
        long code{};
        LONG_PTR first{}, second{};
        bool completed = false;
        while (SUCCEEDED(events_->GetEvent(&code, &first, &second, 0))) {
            if (code == EC_COMPLETE || code == EC_USERABORT || code == EC_ERRORABORT ||
                (fullScreen_ && code == EC_FULLSCREEN_LOST)) {
                completed = true;
            }
            if (code == EC_ERRORABORT) {
                checked(static_cast<HRESULT>(first), "DirectShow playback");
            }
            events_->FreeEventParams(code, first, second);
        }
        if (fullScreen_ && state_ == MovieStatus::Playing && GetAsyncKeyState(VK_ESCAPE) != 0) {
            completed = true;
        }
        if (completed) {
            (void)stop();
            state_ = MovieStatus::Completed;
        }
        return state_;
    }

    bool play() override {
        const auto current = status();
        if (current != MovieStatus::Stopped && current != MovieStatus::Paused) {
            return false;
        }
        if (!checked(control_->Run(), "DirectShow play")) {
            state_ = MovieStatus::Completed;
            return false;
        }
        state_ = MovieStatus::Playing;
        return true;
    }

    bool pause() override {
        if (status() != MovieStatus::Playing || !checked(control_->Pause(), "DirectShow pause")) {
            return false;
        }
        state_ = MovieStatus::Paused;
        return true;
    }

    bool stop() override {
        if (!control_.value || !checked(control_->Stop(), "DirectShow stop")) {
            return false;
        }
        ComPointer<IMediaPosition> position;
        if (SUCCEEDED(graph_->QueryInterface(IID_IMediaPosition,
                                             reinterpret_cast<void**>(&position.value)))) {
            position->put_CurrentPosition(0);
        }
        state_ = MovieStatus::Stopped;
        return true;
    }

    void waitForEvent() override {
        // The original synchronous call blocks the script until completion.
        // Pump Windows messages so the renderer still handles Esc/focus loss.
        MsgWaitForMultipleObjects(0, nullptr, FALSE, 10, QS_ALLINPUT);
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            if (message.message == WM_QUIT) {
                (void)stop();
                state_ = MovieStatus::Completed;
                PostQuitMessage(static_cast<int>(message.wParam));
                break;
            }
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }

    std::string error() const override {
        return error_;
    }

private:
    bool checked(HRESULT result, std::string_view operation) {
        if (SUCCEEDED(result)) {
            return true;
        }
        char code[16]{};
        std::snprintf(code, sizeof(code), "0x%08lx", static_cast<unsigned long>(result));
        error_ = std::string(operation) + " failed (" + code + ")";
        return false;
    }

    bool initialized_{}, fullScreen_{};
    MovieStatus state_{MovieStatus::NoDevice};
    HWND foreground_{};
    ComPointer<IGraphBuilder> graph_;
    ComPointer<IMediaEvent> events_;
    ComPointer<IMediaControl> control_;
    ComPointer<IVideoWindow> window_;
    std::string error_;
};
#else
class NativeRedbook final : public RedbookBackend {
public:
    bool open() override {
        return false;
    }

    void close() override {}

    RedbookStatus status() override {
        return RedbookStatus::NoDevice;
    }

    UInt32 trackCount() override {
        return 0;
    }

    bool track(UInt32, UInt32&, UInt32&) override {
        return false;
    }

    UInt32 position() override {
        return 0;
    }

    bool play(UInt32, UInt32) override {
        return false;
    }

    bool stop() override {
        return false;
    }

    bool pause() override {
        return false;
    }

    bool eject(bool) override {
        return false;
    }

    bool setVolume(Flt32) override {
        return false;
    }

    Flt32 volume() override {
        return 1;
    }

    std::string error() const override {
        return "Native Redbook CD audio is unavailable on this platform";
    }
};

class NativeMovie final : public MovieBackend {
public:
    explicit NativeMovie(bool) {}

    MovieStatus status() override {
        return MovieStatus::NoDevice;
    }

    bool open(std::string_view) override {
        return false;
    }

    void close() override {}

    bool play() override {
        return false;
    }

    bool pause() override {
        return false;
    }

    bool stop() override {
        return false;
    }

    void waitForEvent() override {}

    std::string error() const override {
        return "Native DirectShow movie playback is unavailable on this platform";
    }
};
#endif
}

std::unique_ptr<RedbookBackend> makeNativeRedbookBackend() {
    return std::make_unique<NativeRedbook>();
}

std::unique_ptr<MovieBackend> makeNativeMovieBackend(bool fullscreen) {
    return std::make_unique<NativeMovie>(fullscreen);
}
}
