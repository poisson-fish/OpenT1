#include "sim_net_test_support.hpp"
#undef CHECK
#include "ts_test_support.hpp"
#include <opent1/world/world_mission.hpp>
#include <opent1/core/file_stream.hpp>
#include <opent1/core/mem_stream.hpp>
#include <limits>
#undef CHECK
#define CHECK(...)                                                                                 \
    do {                                                                                           \
        if (!(__VA_ARGS__)) {                                                                      \
            std::fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #__VA_ARGS__);                 \
            std::exit(2);                                                                          \
        }                                                                                          \
    } while (false)

namespace {
using namespace opent1;
using namespace opent1::world;
using namespace sim_net_test;

template <class T> T scalar(StreamIO& s) {
    T v{};
    CHECK(s.read(v));
    return v;
}

Point3F point(StreamIO& s) {
    const auto x = scalar<Flt32>(s), y = scalar<Flt32>(s), z = scalar<Flt32>(s);
    return {x, y, z};
}

struct Record {
    UInt32 bits;
    std::vector<std::byte> bytes;
};

Record record(StreamIO& s) {
    Record r;
    r.bits = scalar<UInt32>(s);
    CHECK(r.bits < 65536 * 8);
    r.bytes.resize((r.bits + 7) / 8);
    CHECK(s.readBytes(r.bytes));
    return r;
}

template <class T> std::unique_ptr<T> asset(StreamIO& s) {
    const auto size = scalar<UInt32>(s);
    CHECK(size < 65536);
    std::vector<std::byte> bytes(size);
    CHECK(s.readBytes(bytes));
    MemRWStream in{std::span<const std::byte>(bytes)};
    auto result = T::load(in, size);
    CHECK(result);
    return result;
}

std::shared_ptr<ResourceManager> clockResources() {
    auto m = std::make_shared<ResourceManager>();
    CHECK(registerInteriorLoaders(*m));
    FileRStream file(OPENT1_INTERIOR_ASSET_FIXTURE);
    CHECK(file.setPosition(12));
    CHECK(m->add("room.dig", asset<InteriorGeometry>(file)));
    CHECK(m->add("room.dis", asset<InteriorShape>(file)));
    auto l = asset<InteriorLighting>(file);
    l->states = {{{0xff00, 0xff00, 0xff00, 0xff00}, 0, 0, 0},
                 {{0xff00, 0xff00, 0xff00, 0xff00}, 2, 0, 0},
                 {{0xff00, 0xff00, 0xff00, 0xff00}, 0, 0, 0},
                 {{0xff00, 0xff00, 0xff00, 0xff00}, 4, 0, 0}};
    l->lights = {{0, -1, 2, 0, 2, 1}, {1, -1, 2, 2, 4, 0}};
    CHECK(m->add("room.dil", std::move(l)));
    CHECK(m->add("room.dml", opent1::test::ts::loadMaterial(opent1::test::ts::material())));
    auto b = gfx::Bitmap::create(2, 2);
    CHECK(b);
    CHECK(m->add("test.bmp", std::move(b)));
    return m;
}

InteriorMissionState interior(UInt32 n) {
    InteriorMissionState s;
    s.filename = "room.dis";
    s.container = Int32(n % 3) - 1;
    s.position = {Flt32(n), -1.25f, 2};
    s.rotation = {.1f, .2f, -.3f};
    s.disableCollisions = n % 2 != 0;
    s.editMode = n % 2 != 0;
    s.checks = {123, 456, 789, 111};
    s.lights.resize((n / 3) % 8);
    for (UInt32 i = 0; i < s.lights.size(); ++i) {
        s.lights[i].time = Flt32(i) * .25f;
        s.lights[i].active = (i + n) % 2 != 0;
        s.lights[i].dummy1 = i + 77;
        s.lights[i].dummy2 = i + 99;
    }
    return s;
}

TerrainMissionState terrain(UInt32 n) {
    TerrainMissionState s;
    s.filename = "map.dtf";
    s.position = {Flt32(n), 2, 3};
    s.rotation = {.2f, -.1f, .3f};
    s.suspend = (n & 128) != 0;
    s.crc = 0x87654321;
    s.drag = .25f;
    return s;
}

template <class S> void packet(const S& state, UInt32 mask, const Record& expected) {
    std::vector<std::byte> bytes(65536);
    BitStream out{std::span(bytes)};
    CHECK(state.pack(out, mask));
    CHECK(UInt32(out.getCurPos()) == expected.bits);
    CHECK(std::ranges::equal(std::span(bytes).first(expected.bytes.size()), expected.bytes));
    // Every possible truncated packet must leave the caller's state unchanged.
    S initial;
    initial.filename = state.filename;
    if constexpr (std::same_as<S, InteriorMissionState>) {
        initial.lights.resize(state.lights.size());
    }
    for (UInt32 cut = 0; cut < expected.bits; ++cut) {
        auto candidate = initial;
        BitStream in{std::span<const std::byte>(expected.bytes)};
        in.setLimits(cut, 0);
        CHECK(!candidate.unpack(in));
        CHECK(candidate == initial);
    }
    BitStream in{std::span<const std::byte>(expected.bytes)};
    in.setLimits(expected.bits, 0);
    auto decoded = initial;
    CHECK(decoded.unpack(in) && UInt32(in.getCurPos()) == expected.bits);
    std::ranges::fill(bytes, std::byte{});
    BitStream again{std::span(bytes)};
    CHECK(decoded.pack(again, mask) && UInt32(again.getCurPos()) == expected.bits);
    CHECK(std::ranges::equal(std::span(bytes).first(expected.bytes.size()), expected.bytes));
}

void parity() {
    FileRStream file(OPENT1_WORLD_MISSION_FIXTURE);
    CHECK(scalar<UInt32>(file) == 0x4d57314f && scalar<UInt32>(file) == 1);
    CHECK(scalar<UInt32>(file) == 216);
    for (UInt32 n = 0; n < 216; ++n) {
        CHECK(scalar<UInt32>(file) == n);
        const auto mask = scalar<UInt32>(file);
        const auto expected = record(file);
        const auto state = interior(n);
        packet(state, mask, expected);
        const auto shutdown = scalar<UInt32>(file), moves = scalar<UInt32>(file);
        CHECK(shutdown == ((mask & 0x80000000U) ? 1U : 0U));
        CHECK(moves == ((mask & 1) ? 1U : 0U));
        const auto persist = record(file);
        std::vector<std::byte> bytes(65536);
        MemRWStream out{std::span(bytes)};
        CHECK(state.write(out));
        CHECK(UInt32(out.getPosition()) * 8 == persist.bits);
        CHECK(std::ranges::equal(std::span(bytes).first(persist.bytes.size()), persist.bytes));
        for (std::size_t cut = 0; cut < persist.bytes.size(); ++cut) {
            MemRWStream in{std::span<const std::byte>(persist.bytes).first(cut)};
            auto v = state;
            CHECK(!v.read(in) && v == state);
        }
        MemRWStream in{std::span<const std::byte>(persist.bytes)};
        InteriorMissionState v;
        CHECK(v.read(in));
        CHECK(v.filename == state.filename && v.lights == state.lights &&
              v.container == state.container && v.position == state.position);
        CHECK(!v.disableCollisions && !v.editMode &&
              v.checks.empty()); // native persistence omits these
    }
    CHECK(scalar<UInt32>(file) == 256);
    for (UInt32 n = 0; n < 256; ++n) {
        CHECK(scalar<UInt32>(file) == n);
        const auto mask = scalar<UInt32>(file);
        const auto expected = record(file);
        const auto state = terrain(n);
        packet(state, mask, expected);
        CHECK(scalar<UInt32>(file) == ((mask & 1) ? 1U : 0U));
        CHECK(scalar<UInt32>(file) == 1);
        for (Int32 version : {2, 3}) {
            const auto persist = record(file);
            std::vector<std::byte> bytes(65536);
            MemRWStream out{std::span(bytes)};
            CHECK(state.write(out, version));
            CHECK(UInt32(out.getPosition()) * 8 == persist.bits);
            CHECK(std::ranges::equal(std::span(bytes).first(persist.bytes.size()), persist.bytes));
            for (std::size_t cut = 0; cut < persist.bytes.size(); ++cut) {
                MemRWStream in{std::span<const std::byte>(persist.bytes).first(cut)};
                auto v = state;
                CHECK(!v.read(in, version) && v == state);
            }
            MemRWStream in{std::span<const std::byte>(persist.bytes)};
            TerrainMissionState v;
            CHECK(v.read(in, version));
            CHECK(v.position == state.position && v.filename == state.filename &&
                  v.drag == (version >= 3 ? .25f : 0.f));
        }
    }
    CHECK(scalar<UInt32>(file) == 126);
    for (UInt32 n = 0; n < 126; ++n) {
        InteriorGeometry g;
        g.expandBoundaryVertices = true;
        const auto a = point(file), b = point(file);
        g.bounds = {a, b};
        const auto input = point(file), expected = point(file);
        CHECK(g.boundaryPoint(input) ==
              expected); // exact binary32; includes zero-width/native sequential tests
    }
    CHECK(scalar<UInt32>(file) == 9);
    for (UInt32 n = 0; n < 9; ++n) {
        const auto extent = scalar<Flt32>(file);
        InteriorMissionState s;
        s.container = scalar<Int32>(file);
        CHECK(s.open({{0, 0, 0}, {extent, 1, 1}}) == (scalar<UInt32>(file) == 1));
        CHECK(scalar<UInt32>(file) == 0 && scalar<UInt32>(file) == 1);
    }
    CHECK(scalar<UInt32>(file) == 16);
    for (UInt32 n = 0; n < 16; ++n) {
        const auto r = record(file);
        MemRWStream in{std::span<const std::byte>(r.bytes)};
        TerrainMissionState s;
        CHECK(s.read(in, 1));
        CHECK(s.position == point(file));
        CHECK(s.visibleDistance == scalar<Flt32>(file) && s.hazeDistance == scalar<Flt32>(file));
        CHECK(s.hazeMin == scalar<Flt32>(file) && s.hazeMax == scalar<Flt32>(file) &&
              s.drag == scalar<Flt32>(file));
        CHECK(s.filename == "old.dtf");
        for (std::size_t cut = 0; cut < r.bytes.size(); ++cut) {
            MemRWStream truncated{std::span<const std::byte>(r.bytes).first(cut)};
            auto next = s;
            CHECK(!next.read(truncated, 1) && next == s);
        }
    }
    CHECK(scalar<UInt32>(file) == 27);
    for (UInt32 n = 0; n < 27; ++n) {
        TerrainMissionPreferences preferences{
            scalar<Flt32>(file), scalar<Flt32>(file), scalar<Flt32>(file)};
        TerrainMissionState s;
        s.filename = "map.dtf";
        const auto p = s.policy(preferences, 8);
        CHECK(p);
        CHECK(p->visibleDistance == scalar<Flt32>(file) && p->hazeDistance == scalar<Flt32>(file));
        CHECK(p->perspectiveDistance == scalar<Flt32>(file) &&
              p->screenSize == scalar<Flt32>(file));
        CHECK(p->containerFarDistance == scalar<Flt32>(file) &&
              p->textureDetail == scalar<Int32>(file));
    }
    CHECK(scalar<UInt32>(file) == 2);
    for (UInt32 n = 0; n < 2; ++n) {
        sim::SimWorld world(nullptr, clockResources());
        auto owned = std::make_unique<SimMissionInterior>();
        auto* room = owned.get();
        InteriorMissionState s;
        s.filename = "room.dis";
        if (n) {
            s.lights = {{4, true}, {2, true}};
        }
        CHECK(room->setState(s));
        CHECK(world.manager().addObject(std::move(owned)));
        std::array<Flt32, 2> scale{};
        for (Int32 i = 0; i < 2; ++i) {
            CHECK(room->state().lights[std::size_t(i)].time == scalar<Flt32>(file));
            CHECK(room->state().lights[std::size_t(i)].active == (scalar<UInt32>(file) != 0));
            scale[std::size_t(i)] = scalar<Flt32>(file);
            const auto playback = room->lights()->playback(i);
            CHECK(playback);
            CHECK(playback->timeScale == scale[std::size_t(i)] &&
                  playback->time == scalar<Flt32>(file));
        }
        CHECK(scalar<UInt32>(file) == 9);
        for (UInt32 j = 0; j < 9; ++j) {
            const auto now = scalar<UInt32>(file), expected = scalar<UInt32>(file);
            CHECK(room->advanceClientTime(now) && room->clientTime() == expected);
            for (Int32 i = 0; i < 2; ++i) {
                const auto elapsed = scalar<Flt32>(file);
                auto time = elapsed * scale[std::size_t(i)];
                const auto duration = i ? 4.f : 2.f;
                while (time > duration) {
                    time -= duration;
                }
                CHECK(std::abs(room->lights()->playback(i)->time - time) < .000002f);
            }
        }
    }
    CHECK(UInt32(file.getPosition()) == file.getSize());
    std::printf("world mission: 472 exact original packets, 744 persistence records, 126 boundary "
                "points, 36 policies, 18 clock events\n");
}

void malformed() {
    auto s = interior(0);
    s.container = 2;
    std::array<std::byte, 4096> bytes{};
    BitStream stream{std::span(bytes)};
    CHECK(!s.pack(stream, ~0U));
    CHECK(stream.getCurPos() == 0);
    auto t = terrain(0);
    t.screenSize = std::numeric_limits<Flt32>::quiet_NaN();
    CHECK(!t.pack(stream, ~0U));
    t = terrain(0);
    MemRWStream out{std::span(bytes)};
    CHECK(!t.write(out, 1));
    CHECK(out.getPosition() == 0);
}

void children() {
    FileRStream file(OPENT1_WORLD_CHILDREN_FIXTURE);
    CHECK(scalar<UInt32>(file) == 0x4348314f && scalar<UInt32>(file) == 1 &&
          scalar<UInt32>(file) == 768);
    UInt32 mismatches{}, accepted{}, scoped{};
    for (UInt32 n = 0; n < 768; ++n) {
        EnvironmentCamera c;
        c.viewport = {0, 0, 126, 94};
        c.focal = {63.5f, 47.5f / .75f};
        c.center = {63.5f - .00001f, 47.5f - .00001f};
        c.nearDistance = 1;
        c.farDistance = 1500;
        for (auto& row : c.worldToCamera.m) {
            for (auto& v : row) {
                v = scalar<Flt32>(file);
            }
        }
        c.worldToCamera.p = point(file);
        c.worldToCamera.flags = RMat3F::Matrix_HasRotation | RMat3F::Matrix_HasTranslation;
        const auto lo = point(file), hi = point(file);
        const Box3F box{lo, hi};
        TerrainMissionPolicy p;
        p.containerFarDistance = scalar<Flt32>(file);
        const auto render = scalar<UInt32>(file) != 0, scope = scalar<UInt32>(file) != 0;
        accepted += render;
        scoped += scope;
        if (p.renderChild(c, box) != render) {
            std::fprintf(stderr, "child %u visibility mismatch\n", n);
            ++mismatches;
        }
        auto inverse = c.worldToCamera;
        inverse.inverse();
        CHECK(p.scopeChild(inverse.p, p.containerFarDistance, box) == scope);
    }
    CHECK(!mismatches && accepted && scoped && UInt32(file.getPosition()) == file.getSize());
    std::printf("world children: 768 exact original camera/XYDB and scope admissions, %u rendered, "
                "%u scoped\n",
                accepted,
                scoped);
}
}

int opent1_test_world_mission() {
    parity();
    children();
    malformed();
    return 0;
}

#include "test_registry.hpp"
OPENT1_REGISTER_TEST("world", "world_mission", opent1_test_world_mission);
