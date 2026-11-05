#pragma once
#include <opent1/core/res_manager.hpp>
#include <opent1/math/matrix.hpp>
#include <array>
#include <deque>
#include <map>
#include <condition_variable>
#include <mutex>
#include <thread>

namespace opent1::audio {
// Only interleaved PCM WAV, mono/stereo, unsigned 8 or signed little-endian 16.
// Decoders own bytes and never retain a stream or a ResourceManager pointer.
struct Wave {
    UInt16 channels{}, bits{};
    UInt32 rate{};
    std::vector<std::byte> samples;
    [[nodiscard]] UInt32 frames() const;
    bool readFrames(UInt64 frame, std::span<std::byte> output, bool loop) const;
    static std::unique_ptr<Wave> load(StreamIO&, UInt32 size);
};

bool registerWaveLoader(ResourceManager&);

inline std::size_t resourceResidentBytes(const Wave& v) {
    return sizeof(v) + v.samples.capacity();
}

enum Flag : UInt32 {
    Loop = 1U << 1,
    Stream = 1U << 2,
    Pitch = 1U << 3,
    Software3D = 1U << 4,
    Hardware3D = 1U << 5
};

struct Profile {
    UInt32 flags{};
    float baseVolume{}, coneOutsideVolume{}, minDistance{1}, maxDistance{100};
    UInt32 coneInsideAngle{360}, coneOutsideAngle{360};
    Point3F coneVector{0, 1, 0};
    bool operator==(const Profile&) const = default;
};

struct Listener {
    TMat3F transform;
    Point3F velocity;
};

struct Spatial {
    float attenuation{}, pan{};
    Int32 millibels{};
    float gain{1};
};

bool valid(const Profile&);
bool valid(const TMat3F&);
float convertFromLinear(float);
// Native software-3D equations. The runtime can separately select positional 3D.
Spatial evaluate(const Profile&, const TMat3F&, const Listener&, float master = 1);
UInt32 nativeBufferBytes(const Wave&, bool streaming);

enum class Failure { None, Device, Context, Buffer, Upload, Source, Configure, Play, Listener };

// Optional one-shot fault injection and live native object counts. Access with
// the same external synchronization as the device; no process-global state.
struct BackendControl {
    Failure fail{Failure::None};
    std::size_t skip{}, devices{}, contexts{}, buffers{}, sources{};
};

struct DeviceOptions {
    bool loopback{};
    UInt32 rate{48000};
    std::string name;
    std::shared_ptr<BackendControl> control;
};
struct BackendState;

class Buffer {
public:
    ~Buffer();
    Buffer(const Buffer&) = delete;
    Buffer& operator=(const Buffer&) = delete;

private:
    friend class Device;
    friend class Source;
    friend class QueuedSource;
    explicit Buffer(std::shared_ptr<BackendState>);
    std::shared_ptr<BackendState> state_;
    unsigned id_{};
    std::size_t bytes_{};
};

struct SourceParameters {
    float gain{1}, pan{}, pitch{1};
    bool loop{};
    bool positional{};
    Point3F position{}, velocity{}, direction{0, 1, 0};
    float minDistance{1}, maxDistance{100}, coneOutsideGain{1};
    UInt32 coneInsideAngle{360}, coneOutsideAngle{360};
};

class Source {
public:
    Source(Source&&) noexcept;
    Source& operator=(Source&&) noexcept;
    ~Source();
    Source(const Source&) = delete;
    Source& operator=(const Source&) = delete;
    void configure(const SourceParameters&); // restores old properties on error
    void play();
    void stop();
    [[nodiscard]] bool playing() const;
    [[nodiscard]] SourceParameters parameters() const;

private:
    friend class Device;
    explicit Source(std::shared_ptr<const Buffer>);
    void release() noexcept;
    std::shared_ptr<const Buffer> buffer_;
    unsigned id_{};
    SourceParameters parameters_;
};

class Device {
public:
    explicit Device(DeviceOptions = {});
    Device(Device&&) noexcept = default;
    Device& operator=(Device&&) noexcept = default;
    Device(const Device&) = delete;
    Device& operator=(const Device&) = delete;
    [[nodiscard]] std::shared_ptr<const Buffer> upload(const Wave&);
    [[nodiscard]] Source source(std::shared_ptr<const Buffer>);
    // Bounded native streaming queue. Owns every buffer and the device/context;
    // destroying Device first is supported. New queues are initially stopped.
    [[nodiscard]] std::shared_ptr<class QueuedSource>
        queue(std::span<const std::shared_ptr<const Buffer>>);
    void listener(const Listener&);
    // Loopback is real OpenAL Soft mixing, stereo signed 16; no sound card needed.
    [[nodiscard]] std::vector<Int16> render(UInt32 frames);
    [[nodiscard]] std::string renderer() const;
    [[nodiscard]] bool loopback() const;

private:
    std::shared_ptr<BackendState> state_;
};

class QueuedSource {
public:
    ~QueuedSource();
    QueuedSource(const QueuedSource&) = delete;
    QueuedSource& operator=(const QueuedSource&) = delete;
    void play();
    void prepare();           // starts muted; all injected/fallible start work happens here
    void activate() noexcept; // publication: rewind to the window's clock origin
    void stop() noexcept;
    [[nodiscard]] bool playing() const;
    [[nodiscard]] UInt32 processed() const;
    // Replaces this many completed queue entries, retaining a single source.
    // Foreign buffers, format changes and insufficient processed entries fail
    // before any queue mutation. Caller provides at most the existing count.
    void recycle(std::span<const std::shared_ptr<const Buffer>>);
    [[nodiscard]] UInt32 sampleOffset() const;
    void configure(const SourceParameters&);

    [[nodiscard]] SourceParameters parameters() const {
        return parameters_;
    }

private:
    friend class Device;
    QueuedSource(std::shared_ptr<BackendState>, std::vector<std::shared_ptr<const Buffer>>);
    std::shared_ptr<BackendState> state_;
    std::vector<std::shared_ptr<const Buffer>> buffers_;
    unsigned id_{};
    SourceParameters parameters_;
};

using Handle = UInt64;
enum class Status { Playing, Suspended, Stopped };

struct VoiceSnapshot {
    Handle handle{};
    UInt32 sound{};
    Status status{Status::Stopped};
    double frame{};
    Spatial spatial;
};

// Simulation operations are synchronized with the device's streaming worker.
// Handles never recycle. Suspended voices retain no native source; the native
// device limit is separate from the number of logical looping voices.
// Limits: 256 definitions, 256 queued entries per channel.
// Each device also caps resident PCM at 128 MiB and buffers at 512. Streaming
// definitions retain their PCM resource, but only a two-second window resides in
// the device. Completed half-second chunks are refilled using its play cursor.
class Runtime {
public:
    explicit Runtime(Device device);
    ~Runtime();

    bool define(UInt32, Resource<Wave>, Profile, float priority = 1);
    bool define(UInt32, std::shared_ptr<const Wave>, Profile, float priority = 1);
    bool removeDefinition(UInt32);

    // Native preference is off by default. Newly defined hardware profiles fall
    // back to software when disabled; active voices retain their selected mode.
    void setHardware3D(bool enabled) {
        std::lock_guard lock(mutex_);
        hardware3D_ = enabled;
    }

    Handle
    play(UInt32, const TMat3F& transform = {}, bool spatial = false, const Point3F& velocity = {});
    bool stop(Handle);
    bool restart(Handle);
    bool suspend(Handle);
    void stopAll();
    void mute(bool);
    bool move(Handle, const TMat3F&, const Point3F& velocity = {});
    bool setListener(const Listener&);
    bool setMaster(float twoD, float threeD);
    bool setMaxBuffers(UInt32); // native inclusive admission; 0 means one voice
    [[nodiscard]] UInt32 maxBuffers() const;
    bool prioritize();
    bool advance(double seconds); // bounded <= 1 day; no partial publication
    bool queue(Int32 channel, UInt32, const TMat3F& transform = {}, bool spatial = false);
    bool pollSequences(); // one dequeue/channel, native two-second scheduler
    void clearSequences();
    std::size_t purge();
    [[nodiscard]] VoiceSnapshot snapshot(Handle) const;
    [[nodiscard]] std::optional<SourceParameters> backendParameters(Handle) const;
    [[nodiscard]] Handle current(std::size_t channel) const;
    [[nodiscard]] std::size_t queued(std::size_t channel) const;

    [[nodiscard]] std::size_t voiceCount() const {
        std::lock_guard lock(mutex_);
        return voices_.size();
    }

    Device& device() {
        return device_;
    }

private:
    struct Definition {
        std::shared_ptr<const Buffer> buffer;
        UInt32 frames{}, rate{};
        Profile profile;
        float priority{};
        std::shared_ptr<const Wave> wave;
        std::weak_ptr<const Wave> original;
    };

    struct Voice {
        Definition definition;
        std::optional<Source> source;
        VoiceSnapshot state;
        TMat3F transform;
        Point3F velocity;
        bool spatial{};
        std::shared_ptr<QueuedSource> stream;
        SourceParameters properties;
        UInt64 readFrame{}, playedFrames{};
        UInt32 writeCursor{};
        std::vector<UInt32> chunks;

        void configure(const SourceParameters& value) {
            if (source) {
                source->configure(value);
            } else if (stream) {
                stream->configure(value);
            }
            properties = value;
        }

        SourceParameters parameters() const {
            return source ? source->parameters() : properties;
        }

        void stop() {
            if (source) {
                source->stop();
            } else if (stream) {
                stream->stop();
            }
            source.reset();
            stream.reset();
        }
    };

    struct Entry {
        UInt32 sound{};
        TMat3F transform;
        bool spatial{};
    };

    struct Channel {
        Handle current{};
        std::deque<Entry> queue;
    };

    bool present(const Listener&,
                 float twoD,
                 float threeD,
                 Handle moved = 0,
                 const TMat3F* transform = nullptr,
                 const Point3F* velocity = nullptr);
    Spatial spatialFor(const Voice&, const Listener&, float, float, const TMat3F* = nullptr) const;
    SourceParameters parametersFor(const Voice&,
                                   const Spatial&,
                                   const TMat3F* = nullptr,
                                   const Point3F* = nullptr) const;
    void startVoice(Voice&);
    bool serviceStream(Voice&);
    mutable std::recursive_mutex mutex_;
    Device device_;
    std::map<UInt32, Definition> definitions_;
    std::map<Handle, Voice> voices_;
    std::array<Channel, 32> channels_;
    Listener listener_;
    float twoD_{1}, threeD_{1};
    Handle next_{1};
    UInt32 maxBuffers_{16};
    bool muted_{}, hardware3D_{};
    std::condition_variable_any wake_;
    std::jthread worker_;
};
} // namespace opent1::audio
