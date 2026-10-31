#include <opent1/audio/audio.hpp>
#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace opent1::audio {
namespace {
bool finite(const Point3F& p) {
    return std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z);
}

float doppler(const Point3F& position, const Point3F& velocity, const Listener& listener) {
    const auto direction = listener.transform.p - position;
    const auto distance = std::sqrt(m_dot(direction, direction));
    if (distance < 1e-6f) {
        return 1;
    }
    constexpr float speed = 343.3f;
    const auto source = std::min(m_dot(velocity, direction) / distance, speed - .001f);
    const auto receiver = std::min(m_dot(listener.velocity, direction) / distance, speed - .001f);
    return (speed - receiver) / (speed - source);
}
}

bool Runtime::define(UInt32 id, Resource<Wave> w, Profile p, float priority) {
    if (!w) {
        return false;
    }
    // Upload copies PCM into the device; no borrowed manager, resource or profile.
    return define(id, std::make_shared<Wave>(*w), p, priority);
}

bool Runtime::define(UInt32 id, std::shared_ptr<const Wave> w, Profile p, float priority) {
    if (!w || !w->frames() || w->rate < 1000 || w->rate > 192000 ||
        w->samples.size() % (w->channels * (w->bits / 8)) || !valid(p) ||
        !std::isfinite(priority) || priority < 0 || priority > 1 ||
        (!definitions_.contains(id) && definitions_.size() >= 256)) {
        return false;
    }
    if ((p.flags & Hardware3D) && (!hardware3D_ || w->channels != 1)) {
        p.flags = (p.flags & ~UInt32(Hardware3D)) | Software3D;
    }
    try {
        if ((p.flags & Stream) &&
            UInt64(w->samples.size()) * 2 < UInt64(w->rate) * w->channels * (w->bits / 8) * 2) {
            p.flags &= ~UInt32(Stream);
        }
        Definition d{{}, w->frames(), w->rate, p, priority, {}};
        if (p.flags & Stream) {
            d.wave = std::move(w);
        } else {
            d.buffer = device_.upload(*w);
        }
        definitions_.insert_or_assign(id, std::move(d));
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

bool Runtime::removeDefinition(UInt32 id) {
    return definitions_.erase(id) != 0;
}

Spatial Runtime::spatialFor(
    const Voice& v, const Listener& l, float twoD, float threeD, const TMat3F* moved) const {
    auto p = v.definition.profile;
    if (!v.spatial) {
        // Native play(id) calls setVolume, which adds the 2D master to baseVolume.
        p.flags &= ~UInt32(Software3D | Hardware3D);
    }
    auto result = evaluate(p, moved ? *moved : v.transform, l, v.spatial ? threeD : twoD);
    if (v.spatial && (p.flags & Hardware3D)) {
        // DirectSound applies distance and cone attenuation in its spatial
        // source. The native software volume remains a priority/clipping input.
        p.flags &= ~UInt32(Software3D | Hardware3D);
        result.gain = evaluate(p, {}, l, threeD).gain;
    }
    return result;
}

SourceParameters Runtime::parametersFor(const Voice& v,
                                        const Spatial& spatial,
                                        const TMat3F* moved,
                                        const Point3F* velocity) const {
    const auto& p = v.definition.profile;
    SourceParameters result{
        spatial.gain, spatial.pan, 1, bool(p.flags & Loop) && !(p.flags & Stream)};
    if (v.spatial && (p.flags & Hardware3D)) {
        const auto& transform = moved ? *moved : v.transform;
        result.positional = true;
        result.position = transform.p;
        result.velocity = velocity ? *velocity : v.velocity;
        m_mul(p.coneVector, static_cast<const RMat3F&>(transform), result.direction);
        result.minDistance = p.minDistance;
        result.maxDistance = p.maxDistance;
        result.coneInsideAngle = p.coneInsideAngle;
        result.coneOutsideAngle = p.coneOutsideAngle;
        // Preserve native integer conversion before the dB multiplication.
        result.coneOutsideGain =
            std::pow(10.f, float(Int32(p.coneOutsideVolume) * -10000) / 2000.f);
    }
    return result;
}

Handle Runtime::play(UInt32 id, const TMat3F& t, bool spatial, const Point3F& velocity) {
    const auto d = definitions_.find(id);
    if (muted_ || d == definitions_.end() || !valid(t) || !finite(velocity) ||
        voices_.size() >= 128 || next_ == std::numeric_limits<Handle>::max()) {
        return 0;
    }
    const auto& p = d->second.profile;
    if (spatial && (p.flags & (Software3D | Hardware3D)) && !(p.flags & Loop) &&
        (t.p - listener_.transform.p).lenf() > p.maxDistance) {
        return 0;
    }
    try {
        Voice v;
        v.definition = d->second;
        v.state = {next_, id, Status::Suspended, 0, {}};
        v.transform = t;
        v.velocity = velocity;
        v.spatial = spatial;
        if (!(p.flags & Stream)) {
            v.source.emplace(device_.source(d->second.buffer));
        }
        v.state.spatial = spatialFor(v, listener_, twoD_, threeD_);
        v.configure(parametersFor(v, v.state.spatial));
        if (std::abs(v.state.spatial.attenuation - 1.f) >= .000001f) {
            startVoice(v);
            v.state.status = Status::Playing;
        }
        voices_.emplace(next_, std::move(v));
        return next_++;
    } catch (const std::exception&) {
        return 0;
    }
}

void Runtime::startVoice(Voice& voice) {
    if (voice.source) {
        voice.source->play();
        return;
    }
    const auto& wave = *voice.definition.wave;
    std::array<std::shared_ptr<const Buffer>, 4> buffers;
    std::vector<UInt32> chunks;
    chunks.reserve(buffers.size());
    UInt64 readFrame{};
    // Native SFX_BUFFER_LENGTH=2000ms and SFX_TIMER_PERIOD=500ms. Alternating
    // half-second extents also preserve the exact ring size at odd PCM rates.
    for (std::size_t i = 0; i < buffers.size(); ++i) {
        const auto end = UInt64(wave.rate) * (i + 1) / 2;
        const auto frames = static_cast<UInt32>(end - readFrame);
        chunks.push_back(frames);
        Wave chunk{wave.channels, wave.bits, wave.rate, {}};
        chunk.samples.resize(static_cast<std::size_t>(frames) * wave.channels * (wave.bits / 8));
        if (!wave.readFrames(
                readFrame, chunk.samples, (voice.definition.profile.flags & Loop) != 0)) {
            throw std::runtime_error("invalid streaming PCM");
        }
        buffers[i] = device_.upload(chunk);
        readFrame = end;
    }
    auto stream = device_.queue(buffers);
    stream->configure(voice.properties);
    stream->play();
    voice.stream = std::move(stream);
    voice.chunks = std::move(chunks);
    voice.readFrame = readFrame;
    voice.playedFrames = 0;
    voice.writeCursor = 0;
}

bool Runtime::serviceStream(Voice& voice) {
    if (!voice.stream) {
        return false;
    }
    try {
        const auto processed = voice.stream->processed();
        if (processed > voice.chunks.size()) {
            return false;
        }
        if (processed) {
            const auto& wave = *voice.definition.wave;
            const bool loop = (voice.definition.profile.flags & Loop) != 0;
            // handleFill stops only on a subsequent fill after wavePosition is
            // strictly beyond waveSize+bufferSize. Until then it queues native
            // 8/16-bit silence, preserving the original drain/sequence timing.
            if (!loop && voice.readFrame >
                             UInt64(voice.definition.frames) + UInt64(voice.definition.rate) * 2) {
                voice.stop();
                voice.state.status = Status::Stopped;
                voice.state.frame = voice.definition.frames;
                return true;
            }
            std::vector<std::shared_ptr<const Buffer>> replacements;
            replacements.reserve(processed);
            UInt64 readFrame = voice.readFrame, consumed{};
            for (UInt32 i = 0; i < processed; ++i) {
                const auto frames = voice.chunks[i];
                consumed += frames;
                Wave chunk{wave.channels, wave.bits, wave.rate, {}};
                chunk.samples.resize(static_cast<std::size_t>(frames) * wave.channels *
                                     (wave.bits / 8));
                if (!wave.readFrames(readFrame, chunk.samples, loop)) {
                    return false;
                }
                replacements.push_back(device_.upload(chunk));
                readFrame += frames;
            }
            voice.stream->recycle(replacements);
            std::rotate(voice.chunks.begin(), voice.chunks.begin() + processed, voice.chunks.end());
            const auto ring = UInt64(voice.definition.rate) * 2;
            const auto first = std::min(consumed, ring - voice.writeCursor);
            // Preserve the shipped split-lock EOF cursor update. In this one
            // branch handleFill adds the first segment once before copying the
            // second and again in its final total, affecting drain completion.
            if (!loop && voice.readFrame <= voice.definition.frames &&
                readFrame > voice.definition.frames &&
                voice.definition.frames - voice.readFrame > first) {
                readFrame += first;
            }
            voice.writeCursor = static_cast<UInt32>((voice.writeCursor + consumed) % ring);
            voice.readFrame = readFrame;
            voice.playedFrames += consumed;
            // Recover from a delayed host tick which let the bounded queue run
            // dry. No whole-file upload or guessed wall-clock cursor is used.
            if (!voice.stream->playing()) {
                voice.stream->play();
            }
        }
        const auto frame = voice.playedFrames + voice.stream->sampleOffset();
        voice.state.frame =
            (voice.definition.profile.flags & Loop)
                ? static_cast<double>(frame % voice.definition.frames)
                : static_cast<double>(std::min<UInt64>(frame, voice.definition.frames));
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

bool Runtime::stop(Handle h) {
    const auto i = voices_.find(h);
    if (i == voices_.end()) {
        return false;
    }
    i->second.stop();
    i->second.state.status = Status::Stopped;
    return true;
}

bool Runtime::suspend(Handle h) {
    const auto i = voices_.find(h);
    if (i == voices_.end()) {
        return false;
    }
    auto& v = i->second;
    v.stop();
    if (v.state.status != Status::Suspended) {
        v.state.status = (v.definition.profile.flags & Loop) ? Status::Suspended : Status::Stopped;
    }
    return true;
}

bool Runtime::restart(Handle h) {
    const auto i = voices_.find(h);
    if (i == voices_.end()) {
        return false;
    }
    auto& v = i->second;
    if (std::abs(v.state.spatial.attenuation - 1.f) < .000001f) {
        return true;
    }
    try {
        startVoice(v);
    } catch (const std::exception&) {
        return false;
    }
    v.state.status = Status::Playing;
    v.state.frame = 0;
    return true;
}

void Runtime::stopAll() {
    for (auto& [h, v] : voices_) {
        (void)h;
        v.stop();
        v.state.status = Status::Stopped;
    }
}

void Runtime::mute(bool value) {
    muted_ = value;
    if (value) {
        stopAll();
    }
}

bool Runtime::present(const Listener& listener,
                      float twoD,
                      float threeD,
                      Handle moved,
                      const TMat3F* transform,
                      const Point3F* velocity) {
    struct Change {
        Voice* voice;
        Spatial value;
        SourceParameters previous, next;
    };

    std::vector<Change> changes;
    changes.reserve(voices_.size());
    try {
        for (auto& [h, v] : voices_) {
            const auto* position = h == moved ? transform : nullptr;
            const auto spatial = spatialFor(v, listener, twoD, threeD, position);
            changes.push_back(
                {&v,
                 spatial,
                 v.parameters(),
                 parametersFor(v, spatial, position, h == moved ? velocity : nullptr)});
        }
        device_.listener(listener);
    } catch (const std::exception&) {
        return false;
    }
    std::size_t applied{};
    try {
        for (auto& change : changes) {
            change.voice->configure(change.next);
            ++applied;
        }
    } catch (const std::exception&) {
        for (std::size_t i = 0; i < applied; ++i) {
            changes[i].voice->configure(changes[i].previous);
        }
        device_.listener(listener_);
        return false;
    }
    for (auto& change : changes) {
        change.voice->state.spatial = change.value;
    }
    listener_ = listener;
    twoD_ = twoD;
    threeD_ = threeD;
    return true;
}

bool Runtime::move(Handle h, const TMat3F& t, const Point3F& velocity) {
    const auto i = voices_.find(h);
    if (i == voices_.end() || !valid(t) || !finite(velocity) ||
        !present(listener_, twoD_, threeD_, h, &t, &velocity)) {
        return false;
    }
    i->second.transform = t;
    i->second.velocity = velocity;
    return true;
}

bool Runtime::setListener(const Listener& l) {
    return present(l, twoD_, threeD_);
}

bool Runtime::setMaster(float twoD, float threeD) {
    if (!std::isfinite(twoD) || !std::isfinite(threeD) || twoD < 0 || twoD > 1 || threeD < 0 ||
        threeD > 1) {
        return false;
    }
    return present(listener_, twoD, threeD);
}

bool Runtime::setMaxBuffers(UInt32 n) {
    if (n > 127) {
        return false;
    }
    const auto old = maxBuffers_;
    maxBuffers_ = n;
    if (!prioritize()) {
        maxBuffers_ = old;
        return false;
    }
    return true;
}

bool Runtime::prioritize() {
    struct Change {
        Voice* voice;
        Status status;
        bool start{};
    };

    std::vector<Change> order;
    order.reserve(voices_.size());
    for (auto& [h, v] : voices_) {
        (void)h;
        order.push_back({&v, v.state.status, false});
    }
    const auto priority = [](const Voice& v) {
        const float p = v.definition.priority;
        return p * p < 1e-12f ? 1.f - v.state.spatial.attenuation : p;
    };
    std::ranges::sort(order, [&](const auto& a, const auto& b) {
        const float av = priority(*a.voice), bv = priority(*b.voice);
        return av == bv ? a.voice->state.handle > b.voice->state.handle : av > bv;
    });
    UInt32 playing{};
    for (auto& c : order) {
        auto& v = *c.voice;
        if (c.status == Status::Playing) {
            const float p = priority(v);
            if (p * p < 1e-12f || playing > maxBuffers_) {
                c.status =
                    (v.definition.profile.flags & Loop) ? Status::Suspended : Status::Stopped;
            } else {
                ++playing;
            }
        } else if (c.status == Status::Suspended && playing <= maxBuffers_) {
            c.start = std::abs(v.state.spatial.attenuation - 1.f) >= .000001f;
            if (c.start) {
                c.status = Status::Playing;
            }
            ++playing;
        }
    }
    std::vector<Voice*> started;
    started.reserve(order.size());
    try {
        for (auto& c : order) {
            if (c.start) {
                startVoice(*c.voice);
                started.push_back(c.voice);
            }
        }
    } catch (const std::exception&) {
        for (auto* v : started) {
            v->stop();
        }
        return false;
    }
    for (auto& c : order) {
        if (c.voice->state.status == Status::Playing && c.status != Status::Playing) {
            c.voice->stop();
        }
        c.voice->state.status = c.status;
        if (c.start) {
            c.voice->state.frame = 0;
        }
    }
    return true;
}

bool Runtime::advance(double seconds) {
    if (!std::isfinite(seconds) || seconds < 0 || seconds > 86400) {
        return false;
    }
    if (!prioritize()) {
        return false;
    }
    for (auto& [h, v] : voices_) {
        (void)h;
        auto& s = v.state;
        if (s.status == Status::Stopped) {
            continue;
        }
        if (s.status == Status::Suspended) {
            continue;
        }
        if (v.definition.profile.flags & Stream) {
            if (!serviceStream(v)) {
                return false;
            }
            continue;
        }
        const float pitch = v.spatial && (v.definition.profile.flags & Hardware3D)
                                ? doppler(v.transform.p, v.velocity, listener_)
                                : 1.f;
        s.frame += seconds * v.definition.rate * pitch;
        if (v.definition.profile.flags & Loop) {
            s.frame = std::fmod(s.frame, v.definition.frames);
        } else if (s.frame >= v.definition.frames) {
            s.frame = v.definition.frames;
            stop(s.handle);
        }
    }
    return true;
}

bool Runtime::queue(Int32 channel, UInt32 id, const TMat3F& t, bool spatial) {
    if (channel < -1 || channel >= 32 || !valid(t)) {
        return false;
    }
    if (channel == -1) {
        return play(id, t, spatial) != 0;
    }
    auto& q = channels_[std::size_t(channel)].queue;
    if (q.size() >= 256) {
        return false;
    }
    q.push_back({id, t, spatial});
    return true;
}

bool Runtime::pollSequences() {
    bool success = true;
    for (auto& c : channels_) {
        if (c.current && snapshot(c.current).status == Status::Stopped) {
            c.current = 0;
        }
        if (!c.current && !c.queue.empty()) {
            const auto e = c.queue.front();
            const auto h = play(e.sound, e.transform, e.spatial);
            // Native failed/unknown IDs consume one entry per poll. Backend
            // faults retain valid entries so a transient device failure retries.
            const auto d = definitions_.find(e.sound);
            const bool clipped =
                d != definitions_.end() && e.spatial &&
                (d->second.profile.flags & (Software3D | Hardware3D)) &&
                !(d->second.profile.flags & Loop) &&
                (e.transform.p - listener_.transform.p).lenf() > d->second.profile.maxDistance;
            if (h || d == definitions_.end() || clipped || muted_) {
                c.queue.pop_front();
                c.current = h;
            } else {
                success = false;
            }
        }
    }
    return success;
}

void Runtime::clearSequences() {
    for (auto& c : channels_) {
        c.current = 0;
        c.queue.clear();
    }
}

std::size_t Runtime::purge() {
    const auto n = voices_.size();
    std::erase_if(voices_, [](const auto& e) {
        return e.second.state.status == Status::Stopped;
    });
    return n - voices_.size();
}

VoiceSnapshot Runtime::snapshot(Handle h) const {
    const auto i = voices_.find(h);
    return i == voices_.end() ? VoiceSnapshot{} : i->second.state;
}

std::optional<SourceParameters> Runtime::backendParameters(Handle h) const {
    const auto i = voices_.find(h);
    return i == voices_.end() ? std::nullopt : std::optional(i->second.parameters());
}

Handle Runtime::current(std::size_t c) const {
    return c < channels_.size() ? channels_[c].current : 0;
}

std::size_t Runtime::queued(std::size_t c) const {
    return c < channels_.size() ? channels_[c].queue.size() : 0;
}
} // namespace opent1::audio
