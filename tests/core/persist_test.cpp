#include "test_support.hpp"
#include <opent1/core/persist.hpp>
#include <opent1/core/mem_stream.hpp>
#include <opent1/core/block_stream.hpp>
#include <opent1/core/timer.hpp>

namespace {
using namespace opent1;
using namespace opent1::Persistent;

struct Record : VersionedBase {
    static inline int alive = 0;
    UInt32 value{0xDEADBEEF};
    UInt8 flag{7};
    std::string text{"default"};
    std::array<Int32, 3> array{1, 2, 3};

    Record() {
        ++alive;
    }

    ~Record() override {
        --alive;
    }

    Int32 version() const override {
        return 3;
    }

    Error read(StreamIO& s, Int32 version, Int32 user, LoadContext&) override {
        return version == 3 && user == 0 && s.read(value) && s.read(flag) ? Error::Ok
                                                                          : Error::ReadError;
    }

    Error write(StreamIO& s, Int32 version, Int32 user, StoreContext&) const override {
        return version == 3 && user == 0 && s.write(value) && s.write(flag) ? Error::Ok
                                                                            : Error::WriteError;
    }
};

struct Graph : Base {
    static inline int alive = 0;
    Base* next{};

    Graph() {
        ++alive;
    }

    ~Graph() override {
        --alive;
    }

    Error read(StreamIO& s, Int32, Int32, LoadContext& context) override {
        auto loaded = context.load(s);
        next = loaded.object;
        return loaded.error;
    }

    Error write(StreamIO& s, Int32, Int32, StoreContext& context) const override {
        return context.store(s, *next);
    }
};

struct Raw : Base {
    UInt32 magic{0xF00DCAFE};
    UInt8 value{6};

    Error read(StreamIO& s, Int32, Int32 user, LoadContext&) override {
        return user == 7 && s.read(magic) && magic == 0xF00DCAFE && s.read(value)
                   ? Error::Ok
                   : Error::ReadError;
    }

    Error write(StreamIO& s, Int32, Int32 user, StoreContext&) const override {
        return user == 7 && s.write(magic) && s.write(value) ? Error::Ok : Error::WriteError;
    }
};

struct FailingProbe : Base {};

struct Empty : Base {};

struct Failing : Base {
    Error read(StreamIO& s, Int32, Int32, LoadContext& context) override {
        (void)context.load(s);
        return Error::ReadError;
    }
};

struct Throwing : Base {
    Error read(StreamIO&, Int32, Int32, LoadContext&) override {
        throw std::runtime_error("read exception");
    }
};

void patch(std::span<std::byte> bytes, std::size_t at, UInt32 value) {
    std::ranges::for_each(std::views::iota(0U, 4U), [&](auto i) {
        bytes[at + i] = static_cast<std::byte>((value >> (i * 8)) & 255U);
    });
}

auto serialize(const ClassRegistry& registry,
               const Base& object,
               bool dictionary = false,
               Int32 user = 0) {
    std::vector<std::byte> bytes(2048);
    MemRWStream s{std::span(bytes)};
    StoreContext writer(registry, dictionary);
    CHECK(writer.store(s, object, user) == Error::Ok);
    bytes.resize(static_cast<std::size_t>(s.getPosition()));
    return bytes;
}

void tests() {
    ClassRegistry registry;
    const auto fields = std::vector{field("text", TypeString, &Record::text),
                                    arrayField("array", TypeInt, &Record::array)};
    CHECK(registry.registerClass<Record>("Record", kPersTag, 1001, fields));
    CHECK(!registry.registerClass<Record>("RecordAgain", 1002));
    CHECK(!registry.registerClass<Graph>("record", 1003));
    CHECK(!registry.registerClass<Graph>("Graph", kDictTag));
    CHECK(!registry.registerClass<Graph>(std::string(128, 'x'), 1004));
    CHECK(registry.registerClass<Graph>("Graph", 42, 1002));
    CHECK(registry.find(1001) == registry.find("RECORD") &&
          registry.find(42) == registry.find(1002));
    CHECK(!registry.find(0) && !registry.find(kPersTag));
    DataFieldManager manager;
    Record object;
    CHECK(registry.fields(object).size() == 2);
    const auto list = registry.fields(object);
    const std::array<std::string_view, 1> arg{"updated"};
    CHECK(list[0].set(object, 0, manager, arg) && list[0].get(object, 0, manager) == "updated");
    CHECK(!list[0].set(object, 1, manager, arg));
    const std::array<std::string_view, 1> number{"-123"};
    CHECK(list[1].set(object, 2, manager, number) && object.array[2] == -123);
    CHECK(!list[1].set(object, 3, manager, number));
    Graph other;
    CHECK(!list[0].set(other, 0, manager, arg) && !list[0].get(other, 0, manager));
    Int32 value = 9;
    CHECK(!manager.setData(TypeFloat, value, number) && value == 9);
    CHECK(!manager.registerType<Int32>(
        TypeInt,
        [](Int32) {
            return std::string{};
        },
        [](Int32&, auto) {
            return true;
        }));
    const std::array<std::string_view, 1> invalid{"12junk"};
    CHECK(!manager.setData(TypeInt, value, invalid) && value == 9);
    float f = 1;
    const std::array<std::string_view, 1> nan{"nan"};
    CHECK(!manager.setData(TypeFloat, f, nan) && f == 1);
    bool b = false;
    const std::array<std::string_view, 1> truth{"TRUE"};
    CHECK(manager.setData(TypeBool, b, truth) && b);
    auto bytes = serialize(registry, object);
    std::ranges::for_each(std::views::iota(std::size_t{0}, bytes.size()), [&](auto length) {
        MemRWStream s{std::span<const std::byte>(bytes).first(length)};
        LoadContext reader(registry, true);
        CHECK(!reader.load(s) && reader.size() == 0);
    });
    {
        MemRWStream s{std::span<const std::byte>(bytes)};
        LoadContext reader(registry);
        auto loaded = reader.load(s);
        CHECK(loaded && reader.size() == 1 && s.getPosition() == static_cast<Int32>(bytes.size()));
        const auto* record = dynamic_cast<Record*>(loaded.object);
        CHECK(record && record->value == object.value && record->flag == 7);
    }
    auto bad = bytes;
    bad[8] = std::byte{255};
    bad[9] = std::byte{255};
    MemRWStream badName{std::span<const std::byte>(bad)};
    LoadContext reader(registry, true);
    CHECK(reader.load(badName).error == Error::InvalidName && reader.size() == 0);
    std::array<std::byte, 12> dict{};
    patch(dict, 0, kDictTag);
    patch(dict, 4, 4);
    patch(dict, 8, 0xFFFFFFFFU);
    MemRWStream badDict{std::span<const std::byte>(dict)};
    CHECK(reader.load(badDict).error == Error::IncorrectTypeFound);
    std::array<std::byte, 8> tiny{};
    MemRWStream shortOutput{std::span(tiny)};
    StoreContext failedWriter(registry, true);
    CHECK(failedWriter.store(shortOutput, object) == Error::WriteError);
    std::array<std::byte, 2048> goodOutput{};
    MemRWStream good{std::span(goodOutput)};
    CHECK(failedWriter.store(good, object) == Error::Ok); // failed write did not poison DICT
    other.next = &other;
    bytes = serialize(registry, other, true);
    const auto beforeGraphs = Graph::alive;
    {
        MemRWStream s{std::span<const std::byte>(bytes)};
        LoadContext graphReader(registry, true);
        auto loaded = graphReader.load(s);
        auto* graph = dynamic_cast<Graph*>(loaded.object);
        CHECK(loaded && graph && graph->next == graph && graphReader.size() == 1);
        CHECK(Graph::alive == beforeGraphs + 1);
    }
    CHECK(Graph::alive == beforeGraphs);
    // A repeated root is a borrowed DICT reference, never a second owner.
    std::array<std::byte, 2048> output{};
    MemRWStream stream{std::span(output)};
    StoreContext writer(registry, true);
    CHECK(writer.store(stream, object) == Error::Ok && writer.store(stream, object) == Error::Ok);
    MemRWStream input{
        std::span<const std::byte>(output).first(static_cast<std::size_t>(stream.getPosition()))};
    auto first = reader.load(input);
    auto second = reader.load(input);
    CHECK(first && second && first.object == second.object && reader.size() == 1);
    ClassRegistry rawRegistry;
    CHECK(rawRegistry.registerUntagged<Raw>(
        "Raw",
        [](StreamIO& s) {
            UInt32 magic{};
            return s.read(magic) && magic == 0xF00DCAFE;
        },
        7));
    CHECK(rawRegistry.registerUntagged<FailingProbe>(
        "BadProbe",
        [](StreamIO& s) {
            std::array<std::byte, 100> buffer{};
            s.readBytes(buffer);
            return false;
        },
        3));
    CHECK(!rawRegistry.registerUntagged<Record>(
        "BadVersion",
        [](StreamIO&) {
            return true;
        },
        1));
    Raw raw;
    bytes = serialize(rawRegistry, raw, false, 7);
    CHECK(bytes.size() == 5);
    MemRWStream rawStream{std::span<const std::byte>(bytes)};
    LoadContext rawReader(rawRegistry);
    auto rawResult = rawReader.load(rawStream);
    CHECK(rawResult && dynamic_cast<Raw*>(rawResult.object)->value == 6 &&
          rawStream.getPosition() == 5);
    // Alias tags accepted for both small and FourCC values.
    ClassRegistry tagged;
    CHECK(tagged.registerClass<Record>("Tagged", 111, makeFourCC('A', 'L', 'I', 'S')));
    bytes = serialize(tagged, object);
    patch(bytes, 0, makeFourCC('A', 'L', 'I', 'S'));
    MemRWStream aliased{std::span<const std::byte>(bytes)};
    LoadContext aliasReader(tagged);
    CHECK(aliasReader.load(aliased));
    // Skipped unknown tail bytes must physically exist.
    ClassRegistry empty;
    CHECK(empty.registerClass<Empty>("Empty", 1));
    std::array<std::byte, 8> missingTail{};
    patch(missingTail, 0, 1);
    patch(missingTail, 4, 4);
    MemRWStream truncated{std::span<const std::byte>(missingTail)};
    LoadContext emptyReader(empty);
    CHECK(emptyReader.load(truncated).error == Error::ReadError && emptyReader.size() == 0);
    // Nested errors/exceptions roll back every object in the new subgraph.
    CHECK(registry.registerClass<Failing>("Failing", 51));
    CHECK(registry.registerClass<Throwing>("Throwing", 52));
    other.next = &object;
    bytes = serialize(registry, other, true);
    patch(bytes, 0, 51);
    const auto beforeRecords = Record::alive;
    MemRWStream nested{std::span<const std::byte>(bytes)};
    CHECK(!reader.load(nested) && reader.size() == 1 && Record::alive == beforeRecords);
    std::array<std::byte, 8> throwBytes{};
    patch(throwBytes, 0, 52);
    MemRWStream throwing{std::span<const std::byte>(throwBytes)};
    bool caught = false;
    try {
        (void)reader.load(throwing);
    } catch (const std::runtime_error&) {
        caught = true;
    }
    CHECK(caught && reader.size() == 1);
    const auto path =
        std::filesystem::temp_directory_path() /
        std::format("opent1-persist-{}", Timer::Clock::now().time_since_epoch().count());

    struct Cleanup {
        std::filesystem::path path;

        ~Cleanup() {
            std::error_code ec;
            std::filesystem::remove(path, ec);
        }
    } cleanup{path};

    StoreContext fileWriter(registry);
    CHECK(fileWriter.fileStore(path, object) == Error::Ok);
    LoadContext fileReader(registry);
    CHECK(fileReader.fileLoad(path));
}

void legacyScale() {
    ClassRegistry registry;
    CHECK(registry.registerClass<Graph>("Graph", 301));
    CHECK(registry.registerClass<Empty>("Empty", 302));
    std::vector<Graph> graph(160);
    Empty tail;
    for (std::size_t i = 0; i < graph.size(); ++i) {
        graph[i].next = i + 1 < graph.size() ? static_cast<Base*>(&graph[i + 1]) : &tail;
    }
    std::array<std::byte, 8192> bytes{};
    MemRWStream output{std::span(bytes)};
    StoreContext store(registry);
    CHECK(store.store(output, graph.front()) == Error::Ok);
    const auto size = static_cast<std::size_t>(output.getPosition());
    MemRWStream input{std::span<const std::byte>(bytes).first(size)};
    LoadContext load(registry);
    CHECK(load.load(input) && load.size() == 161);
    MemRWStream limitedInput{std::span<const std::byte>(bytes).first(size)};
    LoadContext limited(registry, false, {1000, 32, 8192});
    CHECK(limited.load(limitedInput).error == Error::LimitExceeded && limited.size() == 0);
    MemRWStream limitedOutput{std::span(bytes)};
    StoreContext limitedStore(registry, false, {8, 1000, 8192});
    CHECK(limitedStore.store(limitedOutput, graph.front()) == Error::LimitExceeded);
    MemRWStream bytesOutput{std::span(bytes)};
    StoreContext byteLimited(registry, false, {1000, 1000, 16});
    CHECK(byteLimited.store(bytesOutput, graph.front()) == Error::LimitExceeded);
    Graph cycle;
    cycle.next = &cycle;
    MemRWStream cyclic{std::span(bytes)};
    StoreContext cyclicStore(registry);
    CHECK(cyclicStore.store(cyclic, cycle) == Error::IncorrectTypeFound);
}

}

int opent1_test_core_persist() {
    return opent1::test::run([] {
        tests();
        legacyScale();
    });
}

#include "test_registry.hpp"
OPENT1_REGISTER_TEST("core", "core_persist", opent1_test_core_persist);
