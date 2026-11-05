#include <opent1/audio/audio.hpp>
#include <ranges>
#include <AL/al.h>
#include <AL/alc.h>
#include <AL/alext.h>
#include <algorithm>
#include <bit>
#include <cmath>
#include <numbers>
#include <stdexcept>
#include <utility>

namespace opent1::audio {
namespace {
void check() {
    if (alGetError() != AL_NO_ERROR) {
        throw std::runtime_error("OpenAL operation failed");
    }
}

void fault(const std::shared_ptr<BackendControl>& c, Failure f) {
    if (c && c->fail == f && c->skip) {
        --c->skip;
        return;
    }
    if (c && c->fail == f) {
        c->fail = Failure::None;
        throw std::runtime_error("injected OpenAL failure");
    }
}

bool finite(const Point3F& v) {
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}
}

struct BackendState {
    mutable std::recursive_mutex mutex;
    ALCdevice* device{};
    ALCcontext* context{};
    PFNALCSETTHREADCONTEXTPROC setThread{};
    PFNALCGETTHREADCONTEXTPROC getThread{};
    LPALCRENDERSAMPLESSOFT render{};
    std::shared_ptr<BackendControl> control;
    std::size_t bytes{}, buffers{}, sources{};

    ~BackendState() {
        if (context) {
            alcDestroyContext(context);
            if (control) {
                --control->contexts;
            }
        }
        if (device) {
            alcCloseDevice(device);
            if (control) {
                --control->devices;
            }
        }
    }
};

namespace {
class Context {
public:
    explicit Context(const BackendState& s) : lock_(s.mutex), s_(s), previous_(s.getThread()) {
        if (!s_.setThread(s_.context)) {
            throw std::runtime_error("cannot select OpenAL context");
        }
        alGetError();
    }

    ~Context() {
        s_.setThread(previous_);
    }

private:
    std::unique_lock<std::recursive_mutex> lock_;
    const BackendState& s_;
    ALCcontext* previous_{};
};

void apply(unsigned id, const SourceParameters& p) {
    alSourcef(id, AL_GAIN, p.gain);
    alSourcef(id, AL_PITCH, p.pitch);
    alSourcei(id, AL_LOOPING, p.loop ? AL_TRUE : AL_FALSE);
    alSourcei(id, AL_SOURCE_RELATIVE, p.positional ? AL_FALSE : AL_TRUE);
    alSourcef(id, AL_ROLLOFF_FACTOR, p.positional ? 1.f : 0.f);
    alSource3f(id,
               AL_VELOCITY,
               p.positional ? p.velocity.x : 0,
               p.positional ? p.velocity.y : 0,
               p.positional ? p.velocity.z : 0);
    alSource3f(id,
               AL_DIRECTION,
               p.positional ? p.direction.x : 0,
               p.positional ? p.direction.y : 0,
               p.positional ? p.direction.z : 0);
    alSourcef(id, AL_REFERENCE_DISTANCE, p.minDistance);
    alSourcef(id, AL_MAX_DISTANCE, p.maxDistance);
    alSourcef(id, AL_CONE_INNER_ANGLE, p.positional ? float(p.coneInsideAngle) : 360);
    alSourcef(id, AL_CONE_OUTER_ANGLE, p.positional ? float(p.coneOutsideAngle) : 360);
    alSourcef(id, AL_CONE_OUTER_GAIN, p.positional ? p.coneOutsideGain : 1);
    // Native software pan is a millibel channel balance. OpenAL's unit circle
    // supplies stereo placement; distance/cone attenuation is already applied.
    if (p.positional) {
        alSource3f(id, AL_POSITION, p.position.x, p.position.y, p.position.z);
    } else {
        alSource3f(id, AL_POSITION, p.pan, 0, -std::sqrt(std::max(0.f, 1.f - p.pan * p.pan)));
    }
    // Stereo buffers ignore AL_POSITION unless forced to mono. Rotate both
    // authored channels instead, retaining their separation at centered pan.
    const float turn = std::asin(p.pan), halfWidth = std::numbers::pi_v<float> / 6.f;
    const float angles[] = {halfWidth - turn, -halfWidth - turn};
    alSourcefv(id, AL_STEREO_ANGLES, angles);
    check();
}
}

Device::Device(DeviceOptions o) {
    if (o.rate < 8000 || o.rate > 192000 || o.name.size() > 256) {
        throw std::invalid_argument("invalid audio device options");
    }
    auto s = std::make_shared<BackendState>();
    s->control = std::move(o.control);
    fault(s->control, Failure::Device);
    if (o.loopback) {
        auto open = reinterpret_cast<LPALCLOOPBACKOPENDEVICESOFT>(
            alcGetProcAddress(nullptr, "alcLoopbackOpenDeviceSOFT"));
        if (!open) {
            throw std::runtime_error("OpenAL Soft loopback unavailable");
        }
        s->device = open(nullptr);
        s->render = reinterpret_cast<LPALCRENDERSAMPLESSOFT>(
            alcGetProcAddress(s->device, "alcRenderSamplesSOFT"));
    } else {
        s->device = alcOpenDevice(o.name.empty() ? nullptr : o.name.c_str());
    }
    if (!s->device) {
        throw std::runtime_error("cannot open OpenAL device");
    }
    if (s->control) {
        ++s->control->devices;
    }
    s->setThread = reinterpret_cast<PFNALCSETTHREADCONTEXTPROC>(
        alcGetProcAddress(s->device, "alcSetThreadContext"));
    s->getThread = reinterpret_cast<PFNALCGETTHREADCONTEXTPROC>(
        alcGetProcAddress(s->device, "alcGetThreadContext"));
    if (!s->setThread || !s->getThread ||
        !alcIsExtensionPresent(s->device, "ALC_EXT_thread_local_context")) {
        throw std::runtime_error("OpenAL Soft thread context unavailable");
    }
    fault(s->control, Failure::Context);
    const ALCint attrs[] = {ALC_FREQUENCY,
                            ALCint(o.rate),
                            ALC_FORMAT_CHANNELS_SOFT,
                            ALC_STEREO_SOFT,
                            ALC_FORMAT_TYPE_SOFT,
                            ALC_SHORT_SOFT,
                            0};
    const ALCint normal[] = {ALC_FREQUENCY, ALCint(o.rate), 0};
    s->context = alcCreateContext(s->device, o.loopback ? attrs : normal);
    if (!s->context) {
        throw std::runtime_error("cannot create OpenAL context");
    }
    if (s->control) {
        ++s->control->contexts;
    }
    Context context(*s);
    alDistanceModel(AL_INVERSE_DISTANCE_CLAMPED);
    alDopplerFactor(1);
    alSpeedOfSound(343.3f);
    check();
    state_ = std::move(s);
}

Buffer::Buffer(std::shared_ptr<BackendState> s) : state_(std::move(s)) {}

Buffer::~Buffer() {
    if (id_) {
        Context c(*state_);
        alDeleteBuffers(1, &id_);
        --state_->buffers;
        state_->bytes -= bytes_;
        if (state_->control) {
            --state_->control->buffers;
        }
    }
}

std::shared_ptr<const Buffer> Device::upload(const Wave& w) {
    if (!state_ || (w.channels != 1 && w.channels != 2) || (w.bits != 8 && w.bits != 16) ||
        w.rate < 1000 || w.rate > 192000 || w.samples.empty() ||
        w.samples.size() > 64U * 1024 * 1024 || w.samples.size() % (w.channels * (w.bits / 8))) {
        throw std::invalid_argument("invalid PCM buffer");
    }
    auto b = std::shared_ptr<Buffer>(new Buffer(state_));
    Context c(*state_);
    fault(state_->control, Failure::Buffer);
    if (state_->buffers >= 512 || w.samples.size() > 128U * 1024 * 1024 - state_->bytes) {
        throw std::length_error("OpenAL PCM budget exceeded");
    }
    alGenBuffers(1, &b->id_);
    check();
    ++state_->buffers;
    if (state_->control) {
        ++state_->control->buffers;
    }
    fault(state_->control, Failure::Upload);
    const auto format = w.bits == 8 ? (w.channels == 1 ? AL_FORMAT_MONO8 : AL_FORMAT_STEREO8)
                                    : (w.channels == 1 ? AL_FORMAT_MONO16 : AL_FORMAT_STEREO16);
    std::vector<std::byte> swapped;
    const auto* data = w.samples.data();
    if constexpr (std::endian::native == std::endian::big) {
        if (w.bits == 16) {
            swapped = w.samples;
            for (std::size_t i = 0; i < swapped.size(); i += 2) {
                std::swap(swapped[i], swapped[i + 1]);
            }
            data = swapped.data();
        }
    }
    alBufferData(b->id_, format, data, ALsizei(w.samples.size()), ALsizei(w.rate));
    check();
    b->bytes_ = w.samples.size();
    state_->bytes += b->bytes_;
    return b;
}

Source::Source(std::shared_ptr<const Buffer> b) : buffer_(std::move(b)) {
    auto& s = *buffer_->state_;
    Context c(s);
    fault(s.control, Failure::Source);
    if (s.sources >= 128) {
        throw std::length_error("OpenAL source budget exceeded");
    }
    alGenSources(1, &id_);
    check();
    ++s.sources;
    if (s.control) {
        ++s.control->sources;
    }
    try {
        alSourcei(id_, AL_BUFFER, ALint(buffer_->id_));
        alSourcei(id_, AL_SOURCE_RELATIVE, AL_TRUE);
        alSourcef(id_, AL_ROLLOFF_FACTOR, 0);
        check();
    } catch (...) {
        release();
        throw;
    }
}

Source Device::source(std::shared_ptr<const Buffer> b) {
    if (!b || b->state_ != state_) {
        throw std::invalid_argument("foreign OpenAL buffer");
    }
    return Source(std::move(b));
}

Source::Source(Source&& s) noexcept
    : buffer_(std::move(s.buffer_)), id_(std::exchange(s.id_, 0)), parameters_(s.parameters_) {}

Source& Source::operator=(Source&& s) noexcept {
    if (this != &s) {
        release();
        buffer_ = std::move(s.buffer_);
        id_ = std::exchange(s.id_, 0);
        parameters_ = s.parameters_;
    }
    return *this;
}

void Source::release() noexcept {
    if (id_) {
        Context c(*buffer_->state_);
        alSourceStop(id_);
        alSourcei(id_, AL_BUFFER, 0);
        alDeleteSources(1, &id_);
        --buffer_->state_->sources;
        if (buffer_->state_->control) {
            --buffer_->state_->control->sources;
        }
        id_ = 0;
    }
    buffer_.reset();
}

Source::~Source() {
    release();
}

void Source::configure(const SourceParameters& p) {
    if (!std::isfinite(p.gain) || p.gain < 0 || p.gain > 1 || !std::isfinite(p.pan) ||
        std::abs(p.pan) > 1 || !std::isfinite(p.pitch) || p.pitch < .01f || p.pitch > 4) {
        throw std::invalid_argument("invalid source properties");
    }
    if (!finite(p.position) || !finite(p.velocity) || !finite(p.direction) ||
        !std::isfinite(p.minDistance) || p.minDistance < 0 || !std::isfinite(p.maxDistance) ||
        p.maxDistance <= p.minDistance || !std::isfinite(p.coneOutsideGain) ||
        p.coneOutsideGain < 0 || p.coneOutsideGain > 1 || p.coneInsideAngle > p.coneOutsideAngle ||
        p.coneOutsideAngle > 360) {
        throw std::invalid_argument("invalid spatial source properties");
    }
    Context c(*buffer_->state_);
    fault(buffer_->state_->control, Failure::Configure);
    try {
        apply(id_, p);
    } catch (...) {
        apply(id_, parameters_);
        throw;
    }
    parameters_ = p;
}

SourceParameters Source::parameters() const {
    Context c(*buffer_->state_);
    SourceParameters p = parameters_;
    ALint loop{};
    alGetSourcef(id_, AL_GAIN, &p.gain);
    alGetSourcef(id_, AL_PITCH, &p.pitch);
    alGetSourcei(id_, AL_LOOPING, &loop);
    if (p.positional) {
        alGetSource3f(id_, AL_POSITION, &p.position.x, &p.position.y, &p.position.z);
        alGetSource3f(id_, AL_VELOCITY, &p.velocity.x, &p.velocity.y, &p.velocity.z);
    } else {
        float y{}, z{};
        alGetSource3f(id_, AL_POSITION, &p.pan, &y, &z);
    }
    check();
    p.loop = loop != 0;
    return p;
}

void Source::play() {
    Context c(*buffer_->state_);
    fault(buffer_->state_->control, Failure::Play);
    alSourceRewind(id_);
    alSourcePlay(id_);
    check();
}

void Source::stop() {
    Context c(*buffer_->state_);
    alSourceStop(id_);
    check();
}

bool Source::playing() const {
    Context c(*buffer_->state_);
    ALint v{};
    alGetSourcei(id_, AL_SOURCE_STATE, &v);
    check();
    return v == AL_PLAYING;
}

void Device::listener(const Listener& l) {
    if (!valid(l.transform) || !finite(l.velocity)) {
        throw std::invalid_argument("invalid listener");
    }
    Context c(*state_);
    fault(state_->control, Failure::Listener);
    // Relative sources retain their software pan. Positional sources use the
    // original Manager::setListenerTransform row order (front row 2, top row 1).
    alListener3f(AL_POSITION, l.transform.p.x, l.transform.p.y, l.transform.p.z);
    alListener3f(AL_VELOCITY, l.velocity.x, l.velocity.y, l.velocity.z);
    const float orientation[] = {l.transform.m[2][0],
                                 l.transform.m[2][1],
                                 l.transform.m[2][2],
                                 l.transform.m[1][0],
                                 l.transform.m[1][1],
                                 l.transform.m[1][2]};
    alListenerfv(AL_ORIENTATION, orientation);
    check();
}

std::vector<Int16> Device::render(UInt32 frames) {
    if (!state_->render || frames > 1920000) {
        throw std::invalid_argument("invalid loopback render");
    }
    std::vector<Int16> pcm(std::size_t(frames) * 2);
    Context c(*state_);
    state_->render(state_->device, pcm.data(), ALCsizei(frames));
    if (alcGetError(state_->device) != ALC_NO_ERROR) {
        throw std::runtime_error("OpenAL loopback failed");
    }
    return pcm;
}

std::string Device::renderer() const {
    Context c(*state_);
    const auto* s = alGetString(AL_RENDERER);
    check();
    return s ? s : "";
}

bool Device::loopback() const {
    return state_ && state_->render;
}

QueuedSource::QueuedSource(std::shared_ptr<BackendState> state,
                           std::vector<std::shared_ptr<const Buffer>> buffers)
    : state_(std::move(state)), buffers_(std::move(buffers)) {}

std::shared_ptr<QueuedSource>
Device::queue(std::span<const std::shared_ptr<const Buffer>> buffers) {
    if (!state_ || buffers.empty() || buffers.size() > 16) {
        throw std::invalid_argument("invalid audio queue");
    }
    std::array<ALuint, 16> ids{};
    for (auto i : std::views::iota(std::size_t{0}, buffers.size())) {
        if (!buffers[i] || buffers[i]->state_ != state_) {
            throw std::invalid_argument("foreign audio queue buffer");
        }
        ids[i] = buffers[i]->id_;
    }
    auto source =
        std::shared_ptr<QueuedSource>(new QueuedSource(state_, {buffers.begin(), buffers.end()}));
    Context c(*state_);
    fault(state_->control, Failure::Source);
    if (state_->sources >= 128) {
        throw std::length_error("OpenAL source budget exceeded");
    }
    alGenSources(1, &source->id_);
    check();
    ++state_->sources;
    if (state_->control) {
        ++state_->control->sources;
    }
    fault(state_->control, Failure::Configure);
    alSourcei(source->id_, AL_SOURCE_RELATIVE, AL_TRUE);
    alSourcef(source->id_, AL_ROLLOFF_FACTOR, 0);
    alSourceQueueBuffers(source->id_, ALsizei(buffers.size()), ids.data());
    check();
    return source;
}

QueuedSource::~QueuedSource() {
    if (id_) {
        Context c(*state_);
        alSourceStop(id_);
        alSourcei(id_, AL_BUFFER, 0);
        alDeleteSources(1, &id_);
        --state_->sources;
        if (state_->control) {
            --state_->control->sources;
        }
    }
}

void QueuedSource::play() {
    Context c(*state_);
    fault(state_->control, Failure::Play);
    alSourcePlay(id_);
    check();
}

void QueuedSource::prepare() {
    Context c(*state_);
    alSourcef(id_, AL_GAIN, 0);
    check();
    fault(state_->control, Failure::Play);
    alSourcePlay(id_);
    check();
}

void QueuedSource::activate() noexcept {
    try {
        Context c(*state_);
        alSourceRewind(id_);
        alSourcef(id_, AL_GAIN, 1);
        alSourcePlay(id_);
    } catch (const std::exception&) {
    }
}

void QueuedSource::stop() noexcept {
    try {
        Context c(*state_);
        alSourceStop(id_);
    } catch (const std::exception&) {
    }
}

bool QueuedSource::playing() const {
    Context c(*state_);
    ALint value{};
    alGetSourcei(id_, AL_SOURCE_STATE, &value);
    check();
    return value == AL_PLAYING;
}

UInt32 QueuedSource::processed() const {
    Context c(*state_);
    ALint value{};
    alGetSourcei(id_, AL_BUFFERS_PROCESSED, &value);
    check();
    return UInt32(value);
}

UInt32 QueuedSource::sampleOffset() const {
    Context c(*state_);
    ALint value{};
    alGetSourcei(id_, AL_SAMPLE_OFFSET, &value);
    check();
    return static_cast<UInt32>(std::max(0, value));
}

void QueuedSource::configure(const SourceParameters& p) {
    if (!std::isfinite(p.gain) || p.gain < 0 || p.gain > 1 || !std::isfinite(p.pan) ||
        std::abs(p.pan) > 1 || !std::isfinite(p.pitch) || p.pitch < .01f || p.pitch > 4 || p.loop) {
        throw std::invalid_argument("invalid queued source properties");
    }
    if (!finite(p.position) || !finite(p.velocity) || !finite(p.direction) ||
        !std::isfinite(p.minDistance) || p.minDistance < 0 || !std::isfinite(p.maxDistance) ||
        p.maxDistance <= p.minDistance || !std::isfinite(p.coneOutsideGain) ||
        p.coneOutsideGain < 0 || p.coneOutsideGain > 1 || p.coneInsideAngle > p.coneOutsideAngle ||
        p.coneOutsideAngle > 360) {
        throw std::invalid_argument("invalid spatial queue properties");
    }
    Context c(*state_);
    fault(state_->control, Failure::Configure);
    try {
        apply(id_, p);
    } catch (...) {
        apply(id_, parameters_);
        throw;
    }
    parameters_ = p;
}

void QueuedSource::recycle(std::span<const std::shared_ptr<const Buffer>> replacements) {
    if (replacements.empty()) {
        return;
    }
    if (replacements.size() > buffers_.size()) {
        throw std::invalid_argument("too many audio queue replacements");
    }
    Context c(*state_);
    ALint count{};
    alGetSourcei(id_, AL_BUFFERS_PROCESSED, &count);
    check();
    if (replacements.size() > static_cast<std::size_t>(count)) {
        throw std::logic_error("audio queue entries have not completed");
    }
    std::array<ALuint, 16> incoming{}, outgoing{};
    ALint rate{}, channels{}, bits{};
    alGetBufferi(buffers_.front()->id_, AL_FREQUENCY, &rate);
    alGetBufferi(buffers_.front()->id_, AL_CHANNELS, &channels);
    alGetBufferi(buffers_.front()->id_, AL_BITS, &bits);
    check();
    for (auto i : std::views::iota(std::size_t{0}, replacements.size())) {
        const auto& buffer = replacements[i];
        if (!buffer || buffer->state_ != state_) {
            throw std::invalid_argument("foreign audio queue replacement");
        }
        ALint r{}, ch{}, b{};
        alGetBufferi(buffer->id_, AL_FREQUENCY, &r);
        alGetBufferi(buffer->id_, AL_CHANNELS, &ch);
        alGetBufferi(buffer->id_, AL_BITS, &b);
        check();
        if (r != rate || ch != channels || b != bits) {
            throw std::invalid_argument("audio queue replacement format differs");
        }
        incoming[i] = buffer->id_;
    }
    auto next = std::vector<std::shared_ptr<const Buffer>>(
        buffers_.begin() + static_cast<std::ptrdiff_t>(replacements.size()), buffers_.end());
    next.insert(next.end(), replacements.begin(), replacements.end());
    alSourceUnqueueBuffers(id_, static_cast<ALsizei>(replacements.size()), outgoing.data());
    check();
    alSourceQueueBuffers(id_, static_cast<ALsizei>(replacements.size()), incoming.data());
    check();
    buffers_ = std::move(next);
}
} // namespace opent1::audio
