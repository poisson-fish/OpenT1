#include <opent1/audio/audio.hpp>
#include <opent1/core/file_stream.hpp>
#include <opent1/core/mem_stream.hpp>
#include <opent1/sim/sim_audio.hpp>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>

#define CHECK(...)                                                                                 \
    do {                                                                                           \
        if (!(__VA_ARGS__)) {                                                                      \
            std::fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #__VA_ARGS__);                 \
            std::exit(2);                                                                          \
        }                                                                                          \
    } while (false)
using namespace opent1;
using namespace opent1::audio;

namespace {
template <class T> T read(StreamIO& s) {
    T v{};
    CHECK(s.read(v));
    return v;
}

Point3F point(StreamIO& s) {
    const auto x = read<float>(s), y = read<float>(s), z = read<float>(s);
    return {x, y, z};
}

TMat3F matrix(StreamIO& s) {
    TMat3F t;
    t.flags = read<Int32>(s);
    for (auto& row : t.m) {
        for (auto& v : row) {
            v = read<float>(s);
        }
    }
    t.p = point(s);
    return t;
}

Profile profile(StreamIO& s) {
    Profile p;
    p.flags = read<UInt32>(s);
    p.baseVolume = read<float>(s);
    p.coneOutsideVolume = read<float>(s);
    p.minDistance = read<float>(s);
    p.maxDistance = read<float>(s);
    p.coneInsideAngle = read<UInt32>(s);
    p.coneOutsideAngle = read<UInt32>(s);
    p.coneVector = point(s);
    return p;
}

std::shared_ptr<Wave> tone(UInt16 channels = 1, UInt16 bits = 16) {
    auto w = std::make_shared<Wave>();
    w->rate = 48000;
    w->channels = channels;
    w->bits = bits;
    w->samples.resize(4800 * channels * (bits / 8));
    for (std::size_t i = 0; i < 4800; ++i) {
        const auto sample = Int16(std::sin(double(i) * .06) * 10000);
        for (UInt16 c = 0; c < channels; ++c) {
            const auto at = (i * channels + c) * (bits / 8);
            w->samples[at] = std::byte(bits == 8 ? 128 + (sample / 256) : sample & 255);
            if (bits == 16) {
                w->samples[at + 1] = std::byte((UInt16(sample) >> 8) & 255);
            }
        }
    }
    return w;
}

void nativeCorpus() {
    FileRStream f(OPENT1_AUDIO_FIXTURE);
    CHECK(read<UInt32>(f) == 0x31445541 && read<UInt32>(f) == 1);
    const auto n = read<UInt32>(f);
    CHECK(n == 864);
    float largest{};
    for (UInt32 i = 0; i < n; ++i) {
        const auto p = profile(f);
        const auto t = matrix(f);
        Listener l;
        l.transform = matrix(f);
        const auto master = read<float>(f);
        const auto volume = read<float>(f), pan = read<float>(f);
        const auto db = read<Int32>(f), nativePan = read<Int32>(f);
        const auto priority = read<float>(f), expectedPriority = read<float>(f);
        const auto s = evaluate(p, t, l, master);
        largest = std::max(largest, std::abs(s.attenuation - volume));
        if (std::abs(s.attenuation - volume) > 2e-6f || std::abs(s.pan - pan) > 2e-6f ||
            s.millibels != db) {
            std::fprintf(stderr,
                         "spatial %u attenuation %.9g / %.9g pan %.9g / %.9g db %d / %d\n",
                         i,
                         s.attenuation,
                         volume,
                         s.pan,
                         pan,
                         s.millibels,
                         db);
            std::exit(2);
        }
        CHECK(static_cast<Int32>(s.pan * 10000) == nativePan);
        CHECK(std::abs((priority == 0 ? 1.f - s.attenuation : priority) - expectedPriority) <
              2e-6f);
    }
    CHECK(read<UInt32>(f) == 16);
    for (UInt32 i = 0; i < 16; ++i) {
        const auto size = read<UInt32>(f);
        std::vector<std::byte> bytes(size);
        CHECK(f.readBytes(bytes));
        MemRWStream input{std::span<const std::byte>(bytes)};
        auto w = Wave::load(input, size);
        CHECK(w);
        CHECK(w->rate == read<UInt32>(f));
        CHECK(w->samples.size() == read<UInt32>(f));
        CHECK((w->bits == 8 ? 128U : 0U) == read<UInt32>(f));
        CHECK(w->frames() == ((i & 8) ? w->rate * 3 : 16));
        CHECK(nativeBufferBytes(*w, true) ==
              ((i & 8) ? w->rate * w->channels * (w->bits / 8) * 2 : w->samples.size()));
        // Reject every truncated prefix, without publishing any partial Wave.
        for (std::size_t end = 0; end < std::min<std::size_t>(bytes.size(), 80); ++end) {
            MemRWStream cut{std::span<const std::byte>(bytes.data(), end)};
            CHECK(!Wave::load(cut, UInt32(end)));
        }
        for (std::size_t at : {std::size_t(4),
                               std::size_t(16),
                               std::size_t(20),
                               std::size_t(22),
                               std::size_t(32),
                               std::size_t(34),
                               std::size_t(40)}) {
            auto bad = bytes;
            bad[at] = std::byte{255};
            MemRWStream invalid{std::span<const std::byte>(bad)};
            CHECK(!Wave::load(invalid, UInt32(bad.size())));
        }
    }
    auto control = std::make_shared<BackendControl>();
    Runtime r(Device({true, 48000, {}, control}));
    CHECK(read<UInt32>(f) == 16);
    for (UInt32 i = 0; i < 16; ++i) {
        const auto index = read<UInt32>(f);
        CHECK(index == i);
        Profile p;
        p.flags = (i & 1) ? UInt32(Loop) : 0U;
        p.baseVolume = (i & 2) ? 1.f : 0.f;
        CHECK(r.define(1, tone(), p));
        const auto h = r.play(1);
        CHECK(h);
        const auto compare = [&](UInt32 flags) {
            constexpr UInt32 playing = 1U << 23, suspended = 1U << 25;
            const auto expected = (flags & playing)
                                      ? Status::Playing
                                      : ((flags & suspended) ? Status::Suspended : Status::Stopped);
            CHECK(r.snapshot(h).status == expected);
        };
        compare(read<UInt32>(f));
        CHECK(r.suspend(h));
        compare(read<UInt32>(f));
        CHECK(r.restart(h));
        compare(read<UInt32>(f));
        CHECK(r.stop(h));
        compare(read<UInt32>(f));
        r.purge();
    }
    CHECK(read<UInt32>(f) == 160);
    std::size_t played{};
    for (UInt32 id = 1; id <= 160; ++id) {
        CHECK(r.define(id, tone(), {}));
    }
    for (UInt32 i = 0; i < 160; ++i) {
        const auto op = read<UInt32>(f);
        const auto c = read<Int32>(f);
        const auto id = read<UInt32>(f);
        const auto spatial = read<UInt32>(f) != 0;
        if (!op) {
            const bool accepted = r.queue(c, id, {}, spatial);
            if (c == -1) {
                CHECK(accepted == (id != 0));
                if (accepted) {
                    ++played;
                }
            } else {
                CHECK(accepted);
            }
        } else {
            // Completion is supplied by the original manager shim at these polls.
            for (Handle h = 1; h < 1000; ++h) {
                if (r.snapshot(h).handle) {
                    CHECK(r.stop(h));
                }
            }
            r.purge();
            CHECK(r.pollSequences());
        }
        for (std::size_t channel = 0; channel < 32; ++channel) {
            const auto expected = read<UInt32>(f), queued = read<UInt32>(f);
            const auto actual = r.snapshot(r.current(channel)).sound;
            CHECK(actual == expected && r.queued(channel) == queued);
            if (op && actual) {
                ++played;
            }
        }
        CHECK(played == read<UInt32>(f));
    }
    CHECK(read<UInt32>(f) == 66);
    for (UInt32 i = 0; i < 66; ++i) {
        sim::SimSoundSequenceEvent e;
        e.channel = read<Int32>(f);
        e.sound = read<UInt32>(f);
        e.spatial = read<UInt32>(f) != 0;
        e.transform = matrix(f);
        const auto bits = read<UInt32>(f);
        std::vector<std::byte> expected((bits + 7) / 8);
        CHECK(f.readBytes(expected));
        std::array<std::byte, 64> bytes{};
        BitStream packed{std::span<std::byte>(bytes)};
        CHECK(e.pack(packed));
        CHECK(UInt32(packed.getCurPos()) == bits);
        CHECK(std::equal(expected.begin(), expected.end(), bytes.begin()));
        sim::SimSoundSequenceEvent decoded;
        BitStream input{std::span<const std::byte>(expected)};
        CHECK(decoded.unpack(input));
        CHECK(decoded.sound == e.sound && decoded.channel == e.channel &&
              decoded.spatial == e.spatial);
        for (UInt32 end = 0; end < bits; ++end) {
            BitStream cut{std::span<const std::byte>(expected)};
            cut.setLimits(end, 0);
            decoded.sound = 77;
            CHECK(!decoded.unpack(cut));
            CHECK(decoded.sound == 77);
        }
    }
    CHECK(read<UInt32>(f) == 128);
    for (UInt32 i = 0; i < 128; ++i) {
        Wave w;
        w.bits = UInt16(read<UInt32>(f));
        w.channels = 1;
        w.rate = 8000;
        const bool loop = read<UInt32>(f) != 0;
        const auto at = read<UInt32>(f), cursor = read<UInt32>(f);
        w.samples.resize(64);
        CHECK(f.readBytes(w.samples));
        std::array<std::byte, 32> expected{}, output{};
        CHECK(f.readBytes(expected));
        CHECK(w.readFrames(at / (w.bits / 8), output, loop));
        for (std::size_t j = 0; j < 32; ++j) {
            CHECK(output[j] == expected[(cursor + j) % 32]);
        }
        CHECK(w.readFrames(std::numeric_limits<UInt64>::max(), output, loop));
    }
    CHECK(read<UInt32>(f) == 32);
    for (UInt32 i = 0; i < 32; ++i) {
        Runtime runtime(Device({true, 48000, {}, nullptr}));
        const auto max = read<UInt32>(f);
        std::array<Handle, 8> handles{};
        for (UInt32 j = 0; j < 8; ++j) {
            Profile p;
            p.flags = read<UInt32>(f);
            p.baseVolume = read<float>(f);
            const auto priority = read<float>(f);
            CHECK(runtime.define(j + 1, tone(), p, priority));
            handles[j] = runtime.play(j + 1);
            CHECK(handles[j]);
            if (j % 3 == 0) {
                CHECK(runtime.suspend(handles[j]));
            }
        }
        CHECK(runtime.setMaxBuffers(max));
        for (auto h : handles) {
            const auto flags = read<UInt32>(f);
            const auto status = flags & (1U << 23)
                                    ? Status::Playing
                                    : (flags & (1U << 25) ? Status::Suspended : Status::Stopped);
            CHECK(runtime.snapshot(h).status == status);
        }
    }
    std::printf("native: 864 spatial, 16 WAVs, 64 lifecycle states, 160 sequence snapshots, 66 "
                "exact packets, 128 ring fills, 256 priority states; max float error %.9g\n",
                largest);
}

template <class F> bool throws(F&& f) {
    try {
        f();
    } catch (const std::exception&) {
        return true;
    }
    return false;
}

void backend() {
    auto c = std::make_shared<BackendControl>();
    for (auto failure : {Failure::Device, Failure::Context}) {
        c->fail = failure;
        CHECK(throws([&] {
            Device d({true, 48000, {}, c});
        }));
        CHECK(!c->devices && !c->contexts);
    }
    CHECK(throws([&] {
        Device missing({false, 48000, "OpenT1 nonexistent device", c});
    }));
    CHECK(!c->devices && !c->contexts);
    std::optional<Source> retained;
    {
        Device d({true, 48000, {}, c});
        CHECK(d.renderer().find("OpenAL Soft") != std::string::npos);
        for (auto failure : {Failure::Buffer, Failure::Upload}) {
            c->fail = failure;
            CHECK(throws([&] {
                (void)d.upload(*tone());
            }));
            CHECK(!c->buffers);
        }
        auto b = d.upload(*tone());
        c->fail = Failure::Source;
        CHECK(throws([&] {
            (void)d.source(b);
        }));
        CHECK(!c->sources);
        retained.emplace(d.source(b));
        retained->configure({.5f, .2f, 1, true});
        const auto p = retained->parameters();
        CHECK(p.gain == .5f && p.pan == .2f && p.loop);
        c->fail = Failure::Configure;
        CHECK(throws([&] {
            retained->configure({.2f, -.2f, 1, false});
        }));
        CHECK(retained->parameters().gain == .5f);
        c->fail = Failure::Play;
        CHECK(throws([&] {
            retained->play();
        }));
        CHECK(!retained->playing());
        retained->play();
        CHECK(retained->playing());
        const auto pcm = d.render(9600);
        CHECK(std::ranges::any_of(pcm, [](auto v) {
            return v != 0;
        }));
        CHECK(retained->playing());
        const auto energy = [](const auto& samples, std::size_t channel) {
            double e{};
            for (std::size_t i = 4800 + channel; i < samples.size(); i += 2) {
                e += double(samples[i]) * samples[i];
            }
            return e;
        };
        retained->configure({1, -.8f, 1, true});
        retained->play();
        const auto left = d.render(9600);
        CHECK(energy(left, 0) > energy(left, 1) * 2);
        retained->configure({1, .8f, 1, true});
        retained->play();
        const auto right = d.render(9600);
        CHECK(energy(right, 1) > energy(right, 0) * 2);
        retained->configure({0, 0, 1, true});
        retained->play();
        const auto muted = d.render(9600);
        CHECK(energy(muted, 0) < energy(left, 0) * .001);
        retained->stop();
        CHECK(!retained->playing());
        // Exercise every supported AL PCM format, one-shot end and moved handles.
        for (UInt16 channels : {UInt16(1), UInt16(2)}) {
            for (UInt16 bits : {UInt16(8), UInt16(16)}) {
                auto buffer = d.upload(*tone(channels, bits));
                auto a = d.source(buffer);
                auto moved = std::move(a);
                moved.play();
                (void)d.render(9600);
                CHECK(!moved.playing());
                if (channels == 2) {
                    moved.configure({1, -.8f, 1, true});
                    moved.play();
                    const auto stereoLeft = d.render(9600);
                    CHECK(energy(stereoLeft, 0) > energy(stereoLeft, 1) * 2);
                    moved.configure({1, .8f, 1, true});
                    moved.play();
                    const auto stereoRight = d.render(9600);
                    CHECK(energy(stereoRight, 1) > energy(stereoRight, 0) * 2);
                }
            }
        }
        {
            std::vector<Source> sources;
            for (unsigned i = 0; i < 127; ++i) {
                sources.push_back(d.source(b));
            }
            CHECK(c->sources == 128 && throws([&] {
                      (void)d.source(b);
                  }));
        }
        {
            std::vector<std::shared_ptr<const Buffer>> buffers;
            auto tiny = tone();
            tiny->samples.resize(2);
            for (unsigned i = 0; i < 511; ++i) {
                buffers.push_back(d.upload(*tiny));
            }
            CHECK(c->buffers == 512 && throws([&] {
                      (void)d.upload(*tiny);
                  }));
        }
        CHECK(c->buffers == 1 && c->sources == 1);
        Device moved = std::move(d);
        CHECK(!moved.renderer().empty());
    }
    CHECK(c->devices == 1 && c->contexts == 1 && c->buffers == 1 && c->sources == 1);
    retained->play();
    CHECK(retained->playing());
    retained.reset();
    CHECK(!c->sources && !c->buffers && !c->contexts && !c->devices);
    std::puts("backend: real Soft loopback, four PCM formats, one-shot/loop playback, failures and "
              "retained source/device lifetimes");
}

void rollback() {
    auto c = std::make_shared<BackendControl>();
    Runtime r(Device({true, 48000, {}, c}));
    Profile p;
    p.flags = Software3D | Loop;
    auto wav = tone();
    CHECK(r.define(1, wav, p, 0));
    const auto h = r.play(1, {}, true);
    CHECK(h && r.advance(.03));
    const auto old = r.snapshot(h);
    for (auto failure : {Failure::Buffer, Failure::Upload}) {
        c->fail = failure;
        CHECK(!r.define(1, wav, {}));
        CHECK(c->buffers == 1);
    }
    for (auto failure : {Failure::Source, Failure::Configure, Failure::Play}) {
        c->fail = failure;
        CHECK(!r.play(1));
        CHECK(r.voiceCount() == 1 && c->sources == 1);
    }
    Listener l;
    l.transform.p = {20, 0, 0};
    c->fail = Failure::Listener;
    CHECK(!r.setListener(l));
    CHECK(r.snapshot(h).spatial.attenuation == old.spatial.attenuation);
    c->fail = Failure::Configure;
    CHECK(!r.setListener(l));
    CHECK(r.snapshot(h).spatial.attenuation == old.spatial.attenuation);
    const auto second = r.play(1, {}, true);
    CHECK(second);
    const auto before = r.backendParameters(h);
    CHECK(before);
    c->fail = Failure::Configure;
    c->skip = 1;
    CHECK(!r.setListener(l));
    CHECK(r.backendParameters(h)->gain == before->gain &&
          r.backendParameters(second)->gain == before->gain);
    CHECK(r.stop(second));
    CHECK(r.purge() == 1);
    TMat3F far;
    far.p = {100, 0, 0};
    CHECK(r.move(h, far));
    CHECK(r.advance(.01));
    CHECK(r.snapshot(h).status == Status::Suspended);
    CHECK(r.move(h, {}));
    c->fail = Failure::Play;
    CHECK(!r.advance(.02));
    CHECK(r.snapshot(h).status == Status::Suspended);
    CHECK(r.advance(.02));
    CHECK(std::abs(r.snapshot(h).frame - 960) < .001);
    CHECK(r.removeDefinition(1));
    wav.reset();
    CHECK(c->buffers == 1);
    CHECK(r.device().render(2400).size() == 4800);
    CHECK(r.stop(h));
    CHECK(r.purge() == 1 && !c->buffers && !c->sources);
    CHECK(r.define(1, tone(), {}));
    CHECK(r.queue(0, 1));
    c->fail = Failure::Play;
    CHECK(!r.pollSequences());
    CHECK(r.queued(0) == 1 && !r.current(0));
    CHECK(r.pollSequences());
    CHECK(r.current(0));
    Profile full;
    full.baseVolume = 1;
    CHECK(r.define(2, tone(), full));
    CHECK(r.setMaster(0, 1));
    const auto silent = r.play(2);
    CHECK(silent && r.snapshot(silent).status == Status::Suspended);
    CHECK(r.stop(silent));
    CHECK(r.purge() == 1);
    CHECK(r.setMaster(1, 1));
    CHECK(!r.advance(-1) && !r.advance(std::numeric_limits<double>::quiet_NaN()));
    CHECK(!r.queue(32, 1));
    CHECK(!r.queue(-2, 1));
    for (int i = 0; i < 256; ++i) {
        CHECK(r.queue(31, 1));
    }
    CHECK(!r.queue(31, 1));
    for (int i = 1; i < 128; ++i) {
        CHECK(r.play(1));
    }
    CHECK(!r.play(1));
    r.stopAll();
    CHECK(r.purge() == 128);
    r.clearSequences();
    CHECK(r.queue(0, 1));
    r.mute(true);
    CHECK(!r.play(1));
    CHECK(r.pollSequences() && !r.queued(0));
    r.mute(false);
    CHECK(r.play(1));
    std::puts("runtime: load/source/listener/restart rollback, loop restart, stale handles, limits "
              "and definition lifetimes");
}

void positionalAudio() {
    const auto crossingCount = [](const std::vector<Int16>& pcm) {
        int count{};
        for (std::size_t i = 1602; i < pcm.size(); i += 2) {
            if (pcm[i - 2] < 0 && pcm[i] >= 0) {
                ++count;
            }
        }
        return count;
    };
    const auto render = [&](float sourceSpeed, float listenerSpeed, bool hardware) {
        auto control = std::make_shared<BackendControl>();
        Runtime runtime(Device({true, 48000, {}, control}));
        runtime.setHardware3D(hardware);
        Listener listener;
        listener.velocity = {0, 0, listenerSpeed};
        CHECK(runtime.setListener(listener));
        Profile p;
        p.flags = Hardware3D | Loop;
        p.minDistance = 100;
        p.maxDistance = 1000;
        CHECK(runtime.define(1, tone(), p));
        TMat3F position;
        position.p = {0, 0, 10};
        const auto handle = runtime.play(1, position, true, {0, 0, sourceSpeed});
        CHECK(handle);
        CHECK(runtime.backendParameters(handle)->positional == hardware);
        const auto count = crossingCount(runtime.device().render(4800));
        CHECK(runtime.advance(.01));
        const double factor = hardware ? (343.3 + listenerSpeed) / (343.3 + sourceSpeed) : 1.;
        CHECK(std::abs(runtime.snapshot(handle).frame - 480 * factor) < .02);
        if (hardware) {
            CHECK(runtime.backendParameters(handle)->velocity.z == sourceSpeed);
            control->fail = Failure::Configure;
            CHECK(!runtime.move(handle, position, {0, 0, 25}));
            CHECK(runtime.backendParameters(handle)->velocity.z == sourceSpeed);
            CHECK(runtime.move(handle, position, {0, 0, 25}));
            CHECK(runtime.backendParameters(handle)->velocity.z == 25);
        }
        return count;
    };
    const auto stationary = render(0, 0, true), approaching = render(-100, 0, true),
               receding = render(100, 0, true);
    const auto receiver = render(0, 100, true), together = render(100, 100, true),
               software = render(-100, 0, false);
    CHECK(approaching > stationary * 1.25 && receding < stationary * .9 &&
          receiver > stationary * 1.15);
    CHECK(std::abs(together - stationary) <= 1 && std::abs(software - stationary) <= 1);
    std::puts("positional: real moving source/listener Doppler, matching playback clock, "
              "preference fallback and velocity rollback");
}

void streamingAudio() {
    auto control = std::make_shared<BackendControl>();
    {
        Device device({true, 48000, {}, control});
        auto buffer = device.upload(*tone());
        const std::array buffers{buffer, buffer};
        auto queue = device.queue(buffers);
        CHECK(throws([&] {
            queue->recycle(std::span(buffers).first(1));
        }));
        queue->configure({.5f, 0, 1, false});
        queue->play();
        (void)device.render(5000);
        CHECK(queue->processed() == 1);
        const auto before = queue->sampleOffset();
        CHECK(throws([&] {
                  queue->recycle(buffers);
              }) &&
              queue->processed() == 1);
        auto wrong = device.upload(*tone(2));
        CHECK(throws([&] {
                  queue->recycle(std::span(&wrong, 1));
              }) &&
              queue->processed() == 1);
        queue->recycle(std::span(buffers).first(1));
        CHECK(queue->processed() == 0 && queue->sampleOffset() < before);
        CHECK(control->sources == 1 && queue->playing());
        control->fail = Failure::Configure;
        CHECK(throws([&] {
            queue->configure({.2f, 0, 1, false});
        }));
        CHECK(queue->parameters().gain == .5f);
    }
    CHECK(!control->sources && !control->buffers);
    for (UInt16 bits : {UInt16(8), UInt16(16)}) {
        auto wave = std::make_shared<Wave>();
        wave->rate = 8000;
        wave->channels = 1;
        wave->bits = bits;
        wave->samples.resize(3 * wave->rate * (bits / 8));
        for (std::size_t frame = 0; frame < 3 * wave->rate; ++frame) {
            const Int16 value =
                static_cast<Int16>(std::sin(static_cast<double>(frame) * .17) * 12000);
            wave->samples[frame * (bits / 8)] =
                std::byte(bits == 8 ? 128 + value / 256 : value & 255);
            if (bits == 16) {
                wave->samples[frame * 2 + 1] = std::byte((UInt16(value) >> 8) & 255);
            }
        }
        Runtime runtime(Device({true, 8000, {}, control}));
        Profile profile;
        profile.flags = Stream;
        CHECK(runtime.define(77, wave, profile) && control->buffers == 0);
        const auto handle = runtime.play(77);
        CHECK(handle && control->buffers == 4 && control->sources == 1);
        // The script clock alone cannot advance a native playback cursor.
        CHECK(runtime.advance(100) && runtime.snapshot(handle).frame == 0);
        for (unsigned tick = 1; tick <= 8; ++tick) {
            const auto pcm = runtime.device().render(4000);
            CHECK(runtime.advance(.5));
            CHECK(control->buffers == 4 && control->sources == 1);
            if (tick <= 5) {
                CHECK(std::ranges::any_of(pcm, [](auto value) {
                    return value != 0;
                }));
            }
            // Native handleFill checks wavePosition > waveSize+bufferSize
            // before filling, so an exact three-second sample stops at the
            // eighth 500ms poll after its initial two-second fill.
            CHECK(runtime.snapshot(handle).status ==
                  (tick < 8 ? Status::Playing : Status::Stopped));
        }
        CHECK(runtime.snapshot(handle).frame == 24000);
        CHECK(runtime.restart(handle));
        CHECK(runtime.snapshot(handle).frame == 0 && control->buffers == 4 &&
              control->sources == 1);
        CHECK(runtime.stop(handle) && runtime.purge() == 1 && !control->buffers &&
              !control->sources);
        profile.flags = Stream | Loop;
        CHECK(runtime.define(78, wave, profile));
        const auto loop = runtime.play(78);
        CHECK(loop);
        for (unsigned tick = 0; tick < 18; ++tick) {
            (void)runtime.device().render(4000);
            CHECK(runtime.advance(.5));
            CHECK(runtime.snapshot(loop).status == Status::Playing &&
                  runtime.snapshot(loop).frame < 24000);
        }
        const auto old = runtime.snapshot(loop);
        control->fail = Failure::Upload;
        (void)runtime.device().render(4000);
        CHECK(!runtime.advance(.5));
        CHECK(runtime.snapshot(loop).frame == old.frame && control->buffers == 4);
        CHECK(runtime.advance(0));
        CHECK(runtime.stop(loop) && runtime.purge() == 1);
        auto shortWave = tone();
        profile.flags = Stream;
        CHECK(runtime.define(79, shortWave, profile) && control->buffers == 1);
        const auto shortHandle = runtime.play(79);
        CHECK(shortHandle);
        CHECK(runtime.advance(.2) && runtime.snapshot(shortHandle).status == Status::Stopped);
    }
    CHECK(!control->sources && !control->buffers);
    std::puts("streaming: live processed-buffer refill, native two-second ring and half-second "
              "drain, loop wraps, short-wave fallback and upload retry");
}

void zeroSoundIdAndRepeatedEffects() {
    auto control = std::make_shared<BackendControl>();
    Runtime runtime(Device({true, 48000, {}, control}));
    CHECK(runtime.define(0, tone(), {}));
    for (int shot = 0; shot < 300; ++shot) {
        const auto handle = runtime.play(0);
        CHECK(handle && runtime.snapshot(handle).sound == 0);
        CHECK(runtime.advance(.11));
        CHECK(runtime.snapshot(handle).status == Status::Stopped);
        CHECK(runtime.purge() == 1);
        CHECK(runtime.voiceCount() == 0 && control->sources == 0);
    }
}
}

int opent1_test_audio() {
    nativeCorpus();
    backend();
    rollback();
    positionalAudio();
    streamingAudio();
    zeroSoundIdAndRepeatedEffects();
    return 0;
}

#include "test_registry.hpp"
OPENT1_REGISTER_TEST("audio", "audio", opent1_test_audio);
