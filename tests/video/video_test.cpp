#include <opent1/video/player.hpp>
#include <opent1/core/file_stream.hpp>
#include <opent1/core/mem_stream.hpp>
#include <chrono>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <limits>

using namespace opent1;
using namespace opent1::video;
#define CHECK(...)                                                                                 \
    do {                                                                                           \
        if (!(__VA_ARGS__)) {                                                                      \
            std::fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #__VA_ARGS__);                 \
            std::exit(2);                                                                          \
        }                                                                                          \
    } while (false)

namespace {
template <class T> T read(StreamIO& s) {
    T value{};
    CHECK(s.read(value));
    return value;
}

UInt64 hash(std::span<const std::byte> bytes) {
    UInt64 h = 14695981039346656037ULL;
    for (const auto b : bytes) {
        h = (h ^ std::to_integer<UInt8>(b)) * 1099511628211ULL;
    }
    return h;
}

UInt64 rgbaHash(const Frame& frame) {
    std::vector<std::byte> rgba;
    const auto& bitmap = frame.bitmap;
    const auto& pal = bitmap.palette->palettes[0].colors;
    for (Int32 y = 0; y < bitmap.height; ++y) {
        for (auto pixel : bitmap.row(y).first(std::size_t(bitmap.width))) {
            const auto color = pal[std::to_integer<UInt8>(pixel)];
            rgba.insert(rgba.end(),
                        {std::byte(color.red),
                         std::byte(color.green),
                         std::byte(color.blue),
                         std::byte{255}});
        }
    }
    return hash(rgba);
}

struct Expected {
    UInt64 rgba{};
    std::array<UInt32, 7> sizes{};
    std::array<UInt64, 7> hashes{};
};

struct Case {
    std::vector<std::byte> bytes;
    UInt32 width{}, height{};
    std::vector<Expected> frames;
};

std::shared_ptr<const Movie> load(const std::vector<std::byte>& bytes) {
    MemRWStream stream{std::span<const std::byte>(bytes)};
    return Movie::load(stream, UInt32(bytes.size()));
}

std::vector<Case> corpus(const char* path = OPENT1_VIDEO_FIXTURE) {
    FileRStream stream(path);
    CHECK(read<UInt32>(stream) == 0x314b4d53 && read<UInt32>(stream) == 1);
    const auto count = read<UInt32>(stream);
    CHECK(count > 0 && count <= 1000);
    std::vector<Case> cases(count);
    for (auto& c : cases) {
        c.bytes.resize(read<UInt32>(stream));
        CHECK(stream.readBytes(c.bytes));
        c.width = read<UInt32>(stream);
        c.height = read<UInt32>(stream);
        c.frames.resize(read<UInt32>(stream));
        for (auto& f : c.frames) {
            f.rgba = read<UInt64>(stream);
            for (std::size_t t = 0; t < 7; ++t) {
                f.sizes[t] = read<UInt32>(stream);
                f.hashes[t] = read<UInt64>(stream);
            }
        }
    }
    return cases;
}

void independent(const std::vector<Case>& cases) {
    UInt32 frames{}, chunks{};
    for (std::size_t ci = 0; ci < cases.size(); ++ci) {
        const auto& c = cases[ci];
        auto movie = load(c.bytes);
        CHECK(movie);
        CHECK(movie->width() == c.width && movie->height() == c.height &&
              movie->frameCount() == c.frames.size());
        const auto flags = std::to_integer<UInt8>(c.bytes[20]);
        CHECK(movie->ringFrame() == bool(flags & 1));
        CHECK(movie->verticalScale() == ((flags & 6) ? 2U : 1U));
        Decoder decoder(movie);
        std::array<UInt64, 7> samples{};
        std::array<std::vector<std::byte>, 7> concatenated;
        std::vector<std::shared_ptr<const Frame>> retained;
        for (const auto& expected : c.frames) {
            CHECK(decoder.next());
            const auto frame = decoder.frame();
            CHECK(frame->index == retained.size());
            CHECK(frame->timeUs == UInt64(frame->index) * movie->frameTimeUs());
            if (rgbaHash(*frame) != expected.rgba) {
                std::fprintf(stderr,
                             "RGBA case %zu frame %u: %llx != %llx\n",
                             ci,
                             frame->index,
                             static_cast<unsigned long long>(rgbaHash(*frame)),
                             static_cast<unsigned long long>(expected.rgba));
                CHECK(false);
            }
            for (UInt32 t = 0; t < 7; ++t) {
                const auto& pcm = frame->audio[t];
                CHECK(pcm.bytes.size() == expected.sizes[t]);
                if (hash(pcm.bytes) != expected.hashes[t]) {
                    std::fprintf(stderr, "PCM case %zu frame %u track %u\n", ci, frame->index, t);
                    CHECK(false);
                }
                if (!pcm.bytes.empty()) {
                    CHECK(pcm.firstSample == samples[t]);
                    samples[t] += pcm.bytes.size() / (pcm.format.channels * (pcm.format.bits / 8));
                    ++chunks;
                }
                concatenated[t].insert(concatenated[t].end(), pcm.bytes.begin(), pcm.bytes.end());
            }
            retained.push_back(frame);
            ++frames;
        }
        CHECK(!decoder.next());
        CHECK(decoder.frame() == retained.back());
        CHECK(!decoder.seek(movie->frameCount()));
        for (UInt32 t = 0; t < 7; ++t) {
            CHECK(movie->audioSamples(t) == samples[t]);
            const auto format = movie->audioFormat(t);
            if (!format.rate) {
                continue;
            }
            for (UInt32 at : {0U, 3U, 1799U, 1800U, 2200U, 3149U, 9000U}) {
                const auto part = movie->audio(t, at, 997);
                const auto block = format.channels * (format.bits / 8);
                const auto start = std::min(std::size_t(at) * block, concatenated[t].size());
                const auto end = std::min(start + 997 * block, concatenated[t].size());
                CHECK(part.bytes ==
                      std::vector<std::byte>(concatenated[t].begin() + std::ptrdiff_t(start),
                                             concatenated[t].begin() + std::ptrdiff_t(end)));
            }
        }
        for (UInt32 i : {7U, 0U, 3U, 3U, 1U, 6U, 2U, 7U}) {
            CHECK(decoder.seek(i));
            CHECK(rgbaHash(*decoder.frame()) == c.frames[i].rgba);
        }
        movie.reset();
        for (std::size_t i = 0; i < c.frames.size(); ++i) {
            CHECK(rgbaHash(*retained[i]) == c.frames[i].rgba);
        }
    }
    std::printf("independent FFmpeg corpus: %u RGBA frames, %u PCM chunks\n", frames, chunks);
}

UInt32 le32(const std::vector<std::byte>& b, std::size_t at) {
    UInt32 v{};
    for (UInt32 i = 0; i < 4; ++i) {
        v |= UInt32(std::to_integer<UInt8>(b[at + i])) << (i * 8);
    }
    return v;
}

void word(std::vector<std::byte>& b, std::size_t at, UInt32 value) {
    for (UInt32 i = 0; i < 4; ++i) {
        b[at + i] = std::byte((value >> (i * 8)) & 255);
    }
}

std::size_t packet(const std::vector<std::byte>& b, UInt32 frame) {
    const auto count = le32(b, 12) + (le32(b, 20) & 1);
    std::size_t at = 104 + count * 5 + le32(b, 52);
    for (UInt32 i = 0; i < frame; ++i) {
        at += le32(b, 104 + i * 4) & ~3U;
    }
    return at;
}

std::vector<std::byte> corrupt(const Case& c, UInt32 frame) {
    auto b = c.bytes;
    const auto at = packet(b, frame);
    b[at + 1] = std::byte{0};
    b[at + 2] = std::byte{255};
    return b;
}

void rejection(const std::vector<Case>& cases) {
    const auto& bytes = cases[0].bytes;
    for (std::size_t n = 0; n < bytes.size(); ++n) {
        MemRWStream cut{std::span<const std::byte>(bytes).first(n)};
        CHECK(!Movie::load(cut, UInt32(n)));
    }
    for (std::size_t at : {0U, 4U, 8U, 12U, 20U, 52U, 56U, 72U, 104U}) {
        auto bad = bytes;
        word(bad, at, 0xffffffff);
        CHECK(!load(bad));
    }
    auto bad = corrupt(cases[0], 2);
    auto movie = load(bad);
    CHECK(movie);
    Decoder decoder(movie);
    CHECK(decoder.next());
    const auto retained = decoder.frame();
    CHECK(!decoder.seek(4));
    CHECK(decoder.frame() == retained);
    CHECK(decoder.next());
    const auto second = decoder.frame();
    CHECK(!decoder.next() && decoder.frame() == second);
    // Bounded deterministic mutation sweep exercises malformed trees, palettes,
    // packet lengths, audio headers and compressed code paths under ASan too.
    UInt32 random = 13;
    for (UInt32 i = 0; i < 2000; ++i) {
        auto mutated = bytes;
        random = random * 1664525U + 1013904223U;
        const auto at = random % mutated.size();
        mutated[at] ^= std::byte(1U << (i % 8));
        if (auto m = load(mutated)) {
            Decoder d(m);
            for (UInt32 f = 0; f < m->frameCount() && f < 8; ++f) {
                if (!d.next()) {
                    break;
                }
            }
        }
    }
}

struct Sink final : Output {
    Presentation current;
    bool fail{}, throws{};
    UInt32 calls{};

    bool publish(const Presentation& p) override {
        ++calls;
        if (throws) {
            throws = false;
            throw std::runtime_error("sink failure");
        }
        if (fail) {
            fail = false;
            return false;
        }
        current = p;
        return true;
    }

    void clear() noexcept override {
        current = {};
    }
};

void playback(const std::vector<Case>& cases) {
    auto movie = load(cases[0].bytes);
    auto sink = std::make_shared<Sink>();
    Player p(sink);
    CHECK(!p.tick(0));
    CHECK(p.load(movie, 100));
    CHECK(p.snapshot().state == PlayState::Playing && p.snapshot().frame->index == 0);
    CHECK(p.tick(40099) && p.snapshot().frame->index == 0);
    CHECK(p.tick(40100) && p.snapshot().frame->index == 1);
    CHECK(p.tick(120100) && p.snapshot().frame->index == 3);
    CHECK(p.pause(140100) && p.snapshot().timeUs == 140000);
    CHECK(p.tick(900100) && p.snapshot().timeUs == 140000);
    CHECK(p.play(1000100));
    CHECK(p.tick(1040100) && p.snapshot().timeUs == 180000);
    const auto retained = p.snapshot();
    sink->fail = true;
    CHECK(!p.seek(0, 1040100));
    CHECK(p.error() == PlaybackError::Backend && p.snapshot().frame == retained.frame &&
          sink->current.frame == retained.frame);
    sink->throws = true;
    CHECK(!p.load(load(cases[1].bytes), 1040100));
    CHECK(p.snapshot().frame == retained.frame);
    CHECK(!p.load(std::shared_ptr<const Movie>{}, 1040100));
    CHECK(!p.seek(320001, 1040100));
    CHECK(!p.tick(1040099));
    CHECK(!p.load(load(corrupt(cases[0], 0)), 1040100));
    CHECK(p.error() == PlaybackError::Decode && p.snapshot().frame == retained.frame);
    CHECK(p.seek(80000, 1040100));
    CHECK(p.snapshot().frame->index == 2 && p.snapshot().audio[0].firstSample == 640);
    CHECK(p.tick(1280100) && p.snapshot().state == PlayState::Ended &&
          p.snapshot().frame->index == 7);
    CHECK(p.tick(std::numeric_limits<UInt64>::max()) && p.snapshot().state == PlayState::Ended);
    CHECK(p.play(std::numeric_limits<UInt64>::max()) && p.snapshot().timeUs == 0);
    CHECK(p.load(movie, 0, {true, 1}));
    CHECK(p.tick(1000000));
    CHECK(p.snapshot().loops == 3 && p.snapshot().timeUs == 40000 &&
          p.snapshot().frame->index == 1);
    CHECK(p.snapshot().audio[0].bytes.size() ==
          8000 * movie->audioFormat(0).channels * (movie->audioFormat(0).bits / 8));
    auto cycle = movie->audio(0, 0, 8000);
    const auto block = cycle.format.channels * (cycle.format.bits / 8);
    cycle.bytes.resize(std::size_t(movie->durationUs() * cycle.format.rate / 1000000) * block);
    const auto start = std::size_t(p.snapshot().timeUs * cycle.format.rate / 1000000) * block;
    for (std::size_t i = 0; i < p.snapshot().audio[0].bytes.size(); ++i) {
        CHECK(p.snapshot().audio[0].bytes[i] == cycle.bytes[(start + i) % cycle.bytes.size()]);
    }
    CHECK(p.seek(319999, 1000000));
    CHECK(p.tick(1000001));
    CHECK(p.snapshot().timeUs == 0 && p.snapshot().loops == 4 && p.snapshot().frame->index == 0);
    CHECK(p.seek(320000, 1000001) && p.snapshot().state == PlayState::Ended);
    CHECK(p.play(1000001));
    CHECK(p.snapshot().timeUs == 0);
    CHECK(p.tick(1080001));
    sink->fail = true;
    CHECK(!p.stop(1080001));
    CHECK(p.snapshot().timeUs == 80000);
    CHECK(p.stop(1080001));
    CHECK(p.snapshot().timeUs == 0 && p.snapshot().state == PlayState::Paused &&
          p.snapshot().loops == 0);
    auto old = p.snapshot().frame;
    p.close();
    CHECK(p.snapshot().state == PlayState::Empty && !sink->current.frame);
    CHECK(rgbaHash(*old) == cases[0].frames[0].rgba);
    const auto allTracks = load(cases[11].bytes);
    CHECK(p.load(allTracks, 0, {false, 127}));
    for (UInt32 t = 0; t < 7; ++t) {
        const auto format = allTracks->audioFormat(t);
        auto expected = allTracks->audio(t, 0, format.rate);
        expected.bytes.resize(std::size_t(allTracks->durationUs() * format.rate / 1000000) *
                                  format.channels * (format.bits / 8),
                              format.bits == 8 ? std::byte{128} : std::byte{});
        CHECK(p.snapshot().audio[t].bytes == expected.bytes);
    }
    p.close();
    // Resource handle, decoder and player survive cache eviction/manager death.
    std::optional<Decoder> owned;
    const auto directory =
        std::filesystem::temp_directory_path() /
        std::filesystem::path(
            "opent1-video-" +
            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    CHECK(std::filesystem::create_directory(directory));
    {
        FileWStream file(directory / "cutscene.smk");
        CHECK(file.writeBytes(cases[1].bytes));
    }
    {
        ResourceManager resources;
        CHECK(registerSmackerLoader(resources));
        const std::array roots{directory};
        CHECK(resources.setSearchPaths(roots));
        auto resource = resources.load<Movie>("cutscene.smk");
        CHECK(resource);
        owned.emplace(resource);
        CHECK(p.load(resource, 0));
        CHECK(!resources.evict("cutscene.smk"));
        resource.reset();
        CHECK(resources.setSearchPaths({}));
        resources.purge(true);
    }
    CHECK(std::filesystem::remove(directory / "cutscene.smk"));
    CHECK(std::filesystem::remove(directory));
    CHECK(owned->seek(7));
    CHECK(p.tick(33330) && p.snapshot().frame->index == 1);
    CHECK(rgbaHash(*owned->frame()) == cases[1].frames[7].rgba);
    p.close();
}
}

int opent1_test_video(int argc, char** argv) {
    if (argc == 3 && std::string_view(argv[1]) == "--reject") {
        FileRStream input(argv[2]);
        CHECK(!Movie::load(input, input.getSize()));
        return 0;
    }
    const auto cases = corpus(argc == 2 ? argv[1] : OPENT1_VIDEO_FIXTURE);
    independent(cases);
    if (argc == 1) {
        rejection(cases);
        playback(cases);
    }

    return 0;
}

namespace {
int opent1_test_video_default() {
    char program[] = "opent1_tests";
    char* args[] = {program, nullptr};
    return opent1_test_video(1, args);
}
}

#include "test_registry.hpp"
OPENT1_REGISTER_TEST("video", "video", opent1_test_video_default);
