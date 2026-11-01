#include "test_support.hpp"
#include <opent1/core/res_manager.hpp>
#include <opent1/core/timer.hpp>
#include <opent1/core/file_stream.hpp>
#include <opent1/core/rle_stream.hpp>
#include <opent1/core/lzh_stream.hpp>
#include <opent1/core/mem_stream.hpp>
#include <opent1/core/vol_stream.hpp>

namespace {
using namespace opent1;
using namespace std::chrono_literals;

void put(std::vector<std::byte>& bytes, UInt32 word) {
    std::ranges::for_each(std::views::iota(0, 4), [&](int i) {
        bytes.push_back(static_cast<std::byte>((word >> (i * 8)) & 255U));
    });
}

void patch(std::vector<std::byte>& bytes, std::size_t at, UInt32 word) {
    std::ranges::for_each(std::views::iota(0, 4), [&](int i) {
        bytes[at + i] = static_cast<std::byte>((word >> (i * 8)) & 255U);
    });
}

void block(std::vector<std::byte>& bytes, UInt32 id, std::span<const std::byte> data) {
    put(bytes, id);
    put(bytes, static_cast<UInt32>(data.size()));
    std::ranges::copy(data, std::back_inserter(bytes));
    if (data.size() % 2) {
        bytes.push_back(std::byte{});
    }
}

auto archive(StreamCompressType codec, std::string_view text) {
    const auto original = std::as_bytes(std::span(text));
    std::vector<std::byte> payload(original.begin(), original.end());
    if (codec != STRM_COMPRESS_NONE) {
        payload.resize(text.size() * 2 + 256);
        MemRWStream stream{std::span(payload)};
        RLEWStream rle;
        LZHWStream lzh;
        StreamIO* writer;
        if (codec == STRM_COMPRESS_RLE) {
            rle.attach(stream);
            writer = &rle;
        } else {
            lzh.attach(stream);
            writer = &lzh;
        }
        CHECK(writer->writeBytes(original) && writer->flush());
        const auto size = static_cast<std::size_t>(stream.getPosition());
        rle.detach();
        payload.resize(size);
    }
    std::vector<std::byte> bytes;
    put(bytes, kVolFileId);
    put(bytes, 0);
    block(bytes, kVolFileBlockTag, payload);
    patch(bytes, 4, static_cast<UInt32>(bytes.size()));
    constexpr char name[] = "sub/Asset.dat";
    block(bytes, kVolStringTag, std::as_bytes(std::span(name)));
    std::vector<std::byte> dict;
    put(dict, 1);
    put(dict, 0);
    put(dict, 8);
    put(dict, static_cast<UInt32>(original.size()));
    dict.push_back(static_cast<std::byte>(codec));
    block(bytes, kVolIdsTag, dict);
    // The legacy compressed CRC range may extend past the compressed payload.
    // Give it defined backing bytes, independently of decoder output.
    bytes.resize(std::max(bytes.size(), std::size_t{16} + original.size()), std::byte{0x5a});
    return bytes;
}

void save(const std::filesystem::path& path, std::span<const std::byte> data) {
    FileWStream file(path);
    CHECK(file() == STRM_OK && file.writeBytes(data) && file.flush());
}

std::string text(std::span<const std::byte> data) {
    return {reinterpret_cast<const char*>(data.data()), data.size()};
}

struct Counted {
    static inline int alive = 0;

    Counted() {
        ++alive;
    }

    ~Counted() {
        --alive;
    }
};

void tests() {
    const auto root = std::filesystem::temp_directory_path() /
                      std::format("opent1-res-{}", Timer::Clock::now().time_since_epoch().count());
    CHECK(std::filesystem::create_directories(root / "first/sub") &&
          std::filesystem::create_directories(root / "second/sub"));

    struct Cleanup {
        std::filesystem::path path;

        ~Cleanup() {
            std::error_code ec;
            std::filesystem::remove_all(path, ec);
        }
    } cleanup{root};

    const std::string disk = "disk", second = "second";
    save(root / "first/sub/Asset.dat", std::as_bytes(std::span(disk)));
    save(root / "second/sub/Asset.dat", std::as_bytes(std::span(second)));
    save(root / "second/text.txt", std::as_bytes(std::span(second)));
    save(root / "second/retry.bad", std::as_bytes(std::span(second)));
    std::array roots{root / "first", root / "second"};
    Resource<RawData> survivor;
    std::unique_ptr<StreamIO> ownedStream;
    {
        ResourceManager manager;
        CHECK(manager.setSearchPaths(roots));
        CHECK(manager.contains("SUB\\ASSET.DAT") && !manager.contains("../sub/Asset.dat") &&
              !manager.contains("/sub/Asset.dat"));
        CHECK(manager.getSize("sub/asset.dat") == 4 &&
              manager.getFullPath("sub/asset.dat") ==
                  std::filesystem::absolute(root / "first/sub/Asset.dat"));
        CHECK(manager.findMatches("SUB/*.D?T") ==
              std::vector<std::string>({"sub\\Asset.dat", "sub\\Asset.dat"}));
        auto a = manager.load<RawData>("sub/asset.dat");
        CHECK(a && text(a->bytes) == disk && a.name() == "sub/asset.dat");
        auto copy = a;
        auto moved = std::move(copy);
        CHECK(!copy && moved.get() == a.get() &&
              manager.load<RawData>("SUB/ASSET.DAT").get() == a.get());
        CHECK(!manager.load<std::string>("sub/asset.dat"));
        CHECK(manager.purge(true) == 0);
        std::vector<std::byte> unchanged{std::byte{7}};
        CHECK(!manager.read("sub/asset.dat", unchanged, 3) &&
              unchanged == std::vector{std::byte{7}});
        UInt32 crc{};
        CHECK(manager.getCrc("sub/asset.dat", crc) &&
              crc == CRC::calculate(std::as_bytes(std::span(disk))));
        CHECK(!manager.getCrc("missing", crc));
        survivor = a;
        ownedStream = manager.openStream("sub/asset.dat");
        std::array invalid{root / "missing"};
        CHECK(!manager.setSearchPaths(invalid) && manager.contains("sub/asset.dat"));
        CHECK(manager.setSearchPaths(std::span(roots).subspan(1)));
        CHECK(text(survivor->bytes) == disk &&
              text(manager.load<RawData>("sub/asset.dat")->bytes) == second);
        CHECK(manager.add("owned.custom", std::make_unique<Counted>(), 0ms));
        CHECK(Counted::alive == 1 && !manager.add("owned.custom", std::make_unique<Counted>()));
        auto custom = manager.load<Counted>("owned.custom");
        CHECK(custom && manager.purge() == 0);
        custom.reset();
        CHECK(manager.purge() == 1 && Counted::alive == 0);
        int calls = 0;
        CHECK(manager.registerLoader<std::string>(
            ".txt",
            [&](StreamIO& stream, UInt32 size) {
                ++calls;
                std::string s(size, ' ');
                if (!stream.readBytes(std::as_writable_bytes(std::span(s)))) {
                    return std::unique_ptr<std::string>{};
                }
                return std::make_unique<std::string>(s);
            },
            0ms));
        CHECK(!manager.registerLoader<std::string>(".TXT", [](StreamIO&, UInt32) {
            return std::make_unique<std::string>();
        }));
        CHECK(!manager.load<std::string>("missing.txt") && calls == 0);
        auto loadedText = manager.load<std::string>("text.txt");
        CHECK(loadedText && *loadedText == second && calls == 1);
        CHECK(manager.load<std::string>("TEXT.TXT").get() == loadedText.get() && calls == 1);
        loadedText.reset();
        CHECK(manager.purge() == 1);
        CHECK(manager.load<std::string>("text.txt") && calls == 2);
        int attempts = 0;
        CHECK(manager.registerLoader<Counted>(
            ".bad", [&](StreamIO&, UInt32) -> std::unique_ptr<Counted> {
                CHECK(!manager.load<Counted>("retry.bad")); // dependency cycle fails cleanly
                if (++attempts == 1) {
                    return {};
                }
                if (attempts == 2) {
                    throw std::runtime_error("retry");
                }
                return std::make_unique<Counted>();
            }));
        CHECK(!manager.load<Counted>("retry.bad"));
        bool caught = false;
        try {
            (void)manager.load<Counted>("retry.bad");
        } catch (const std::runtime_error&) {
            caught = true;
        }
        CHECK(caught && manager.load<Counted>("retry.bad") && attempts == 3);
    }
    CHECK(Counted::alive == 0);
    CHECK(survivor && text(survivor->bytes) == disk);
    std::array<std::byte, 4> read{};
    CHECK(ownedStream->readBytes(read) && text(read) == disk);
    CHECK(!ownedStream->readBytes(read) && ownedStream->getLastSize() == 0);
    CHECK(ownedStream->setPosition(0) && ownedStream->readBytes(read));
    ownedStream->close();
    ownedStream->clrStatus();
    CHECK(!ownedStream->readBytes(read));
    std::ranges::for_each(
        std::array{STRM_COMPRESS_NONE, STRM_COMPRESS_RLE, STRM_COMPRESS_LZH}, [&](auto codec) {
            const std::string payload(200, 'V');
            save(root / "second/test.vol", archive(codec, payload));
            ResourceManager manager;
            CHECK(manager.setSearchPaths(roots) && manager.addVolume("TEST.VOL"));
            CHECK(manager.addVolume("test.vol") && !manager.addVolume("missing.vol"));
            CHECK(text(manager.load<RawData>("sub/asset.dat")->bytes) == disk);
            std::filesystem::remove(root / "first/sub/Asset.dat");
            std::filesystem::remove(root / "second/sub/Asset.dat");
            CHECK(manager.setSearchPaths(roots));
            CHECK(manager.getSize("sub/asset.dat") == payload.size());
            auto held = manager.load<RawData>("sub/asset.dat");
            CHECK(held && text(held->bytes) == payload);
            std::vector<std::byte> output;
            CHECK(manager.read("sub/asset.dat", output) && text(output) == payload);
            UInt32 crc{};
            const auto encoded = archive(codec, payload);
            // Vanilla reads the declared decoded size directly from the raw
            // archive payload, crossing into following blocks when compressed.
            CHECK(manager.getCrc("sub/asset.dat", crc) &&
                  crc == CRC::calculate(std::span(encoded).subspan(16, payload.size())));
            const UInt32 seed = 0x12345678U;
            CHECK(manager.getCrc("sub/asset.dat", crc, seed) &&
                  crc == CRC::calculate(std::span(encoded).subspan(16, payload.size()), seed));
            save(root / "second/newer.vol", archive(codec, "newer"));
            CHECK(manager.setSearchPaths(roots) && manager.addVolume("newer.vol"));
            CHECK(text(manager.load<RawData>("sub/asset.dat")->bytes) == "newer" &&
                  text(held->bytes) == payload);
            CHECK(manager.removeVolume("newer.vol") &&
                  text(manager.load<RawData>("sub/asset.dat")->bytes) == payload);
            CHECK(manager.removeVolume("TEST.VOL") && manager.contains("sub/asset.dat"));
            CHECK(manager.removeVolume("test.vol") && !manager.contains("sub/asset.dat"));
            CHECK(!manager.removeVolume("test.vol") && text(held->bytes) == payload);
            save(root / "first/sub/Asset.dat", std::as_bytes(std::span(disk)));
            save(root / "second/sub/Asset.dat", std::as_bytes(std::span(second)));
        });
    CHECK(resourceType("a.DAT") == 0xFF746164U && resourceType("noext") == kInvalidResourceId);
    const std::string known = "123456789";
    CHECK(CRC::calculate(std::as_bytes(std::span(known))) == 0x340BC6D9U);
}
}

int opent1_test_core_resource() {
    return opent1::test::run(tests);
}

#include "test_registry.hpp"
OPENT1_REGISTER_TEST("core", "core_resource", opent1_test_core_resource);
