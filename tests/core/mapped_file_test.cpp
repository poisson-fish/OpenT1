#include "test_support.hpp"
#include <ranges>
#include "test_registry.hpp"
#include <opent1/core/mapped_file.hpp>
#include <opent1/core/vol_stream.hpp>
#include <chrono>
#include <algorithm>

using namespace opent1;

namespace {
int mappedArchives() {
    return test::run([] {
        // Four UTF-16 filename lengths cover every alignment of the variable
        // rename payload within its UInt64 storage. The terminator must exist
        // even when there is no spare allocation padding after the name.
        for (auto padding : std::views::iota(unsigned{0}, unsigned{4})) {
            const auto path = std::filesystem::absolute(
                "mapped-archive-" +
                std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
                std::string(padding, 'x') + ".vol");
            VolumeRWStream writer;
            const std::string first = "first", second = "second";
            CHECK(writer.put("entry", std::as_bytes(std::span(first))) && writer.save(path));
            ReadOnlyMappedFile mapping;
            const auto size = std::filesystem::file_size(path);
            CHECK(!mapping.open(path, std::size_t(size) - 1));
            const bool mapped = mapping.open(path, std::size_t(size));
            CHECK(!mapped || (mapping.bytes().size() == size && mapping.unchanged()));
            std::printf("Archive backing: %s\n", mapped ? "mapped" : "buffered fallback");
            auto moved = std::move(mapping);
            CHECK(mapping.bytes().empty() && (!mapped || moved.bytes().size() == size));
            VolumeRStream reader;
            CHECK(reader.openVolume(path) &&
                  (reader.mappedBytes() == 0 || reader.mappedBytes() == size) &&
                  reader.open("entry"));
            CHECK(std::ranges::equal(reader.data(), std::as_bytes(std::span(first))));
            CHECK(writer.put("entry", std::as_bytes(std::span(second))) && writer.save(path));
            CHECK((!mapped || moved.unchanged()) && reader.open("entry"));
            CHECK(std::ranges::equal(reader.data(), std::as_bytes(std::span(first))));
            VolumeRStream replacement;
            CHECK(replacement.openVolume(path) && replacement.open("entry"));
            CHECK(std::ranges::equal(replacement.data(), std::as_bytes(std::span(second))));
            auto transferred = std::move(reader);
            CHECK(transferred.open("entry") &&
                  std::ranges::equal(transferred.data(), std::as_bytes(std::span(first))));
            transferred.closeVolume();
            CHECK(transferred.mappedBytes() == 0 && transferred.ownedBackingBytes() == 0);
            replacement.closeVolume();
            moved = ReadOnlyMappedFile{};
            CHECK(std::filesystem::remove(path));
        }
    });
}
}

OPENT1_REGISTER_TEST("core", "mapped_archive_backing", mappedArchives);
