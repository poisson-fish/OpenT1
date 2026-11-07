#include "test_support.hpp"
#include "test_registry.hpp"
#include <opent1/core/vol_stream.hpp>
#include <array>
#include <ranges>

namespace {
using namespace opent1;

void put(std::vector<std::byte>& bytes, UInt32 value) {
    for (unsigned shift = 0; shift < 32; shift += 8) {
        bytes.push_back(static_cast<std::byte>((value >> shift) & 255U));
    }
}

void patch(std::vector<std::byte>& bytes, std::size_t at, UInt32 value) {
    for (unsigned i = 0; i < 4; ++i) {
        bytes[at + i] = static_cast<std::byte>((value >> (i * 8)) & 255U);
    }
}

// Construct native-layout input independently of the VOL writer. Padding after
// the directory-addressed VBLK is optional; directory blocks remain word-aligned.
auto volume(std::string_view payload, UInt32 alignment, unsigned padding = 0) {
    std::vector<std::byte> bytes;
    put(bytes, kVolFileId);
    put(bytes, 16 + static_cast<UInt32>(payload.size()) + padding);
    put(bytes, kVolFileBlockTag);
    put(bytes, static_cast<UInt32>(payload.size()) | alignment);
    const auto data = std::as_bytes(std::span(payload));
    bytes.insert(bytes.end(), data.begin(), data.end());
    bytes.insert(bytes.end(), padding, std::byte{0});
    put(bytes, kVolStringTag);
    constexpr char name[] = "asset.dat";
    put(bytes, sizeof(name));
    const auto nameBytes = std::as_bytes(std::span(name));
    bytes.insert(bytes.end(), nameBytes.begin(), nameBytes.end());
    put(bytes, kVolIdsTag);
    put(bytes, 17);
    put(bytes, 7);
    put(bytes, 0);
    put(bytes, 8);
    put(bytes, static_cast<UInt32>(payload.size()));
    bytes.push_back(static_cast<std::byte>(STRM_COMPRESS_NONE));
    bytes.push_back(std::byte{0});
    return bytes;
}

constexpr UInt32 dwordAlignment = 0x80000000U;
// For the issue's two-byte payload: strings at 18, dictionary record at 44.
constexpr std::size_t recordAt = 44;

int volumePadding() {
    return test::run([] {
        for (const auto alignment : std::array{UInt32{0}, dwordAlignment}) {
            for (unsigned size = 0; size <= 4; ++size) {
                const std::string payload = std::string("OKAY").substr(0, size);
                const unsigned mask = alignment ? 3U : 1U;
                const unsigned padding = ((size + mask) & ~mask) - size;
                for (unsigned stored = 0; stored <= padding; ++stored) {
                    const auto bytes = volume(payload, alignment, stored);
                    VolumeRStream reader;
                    CHECK(reader.openVolume(bytes) && reader.getVolumeItems().size() == 1);
                    CHECK(reader.open("ASSET.DAT") && reader.getSize() == size);
                    CHECK(std::ranges::equal(reader.data(), std::as_bytes(std::span(payload))));
                    CHECK(reader.open(UInt32{7}));
                    std::vector<std::byte> extracted(size);
                    CHECK(reader.readBytes(extracted));
                    CHECK(std::ranges::equal(extracted, std::as_bytes(std::span(payload))));
                    std::byte pastEnd{};
                    CHECK(!reader.readBytes(std::span(&pastEnd, 1)) && reader() == STRM_EOS);

                    VolumeRWStream writer;
                    CHECK(writer.openVolume(bytes));
                    const auto encoded = writer.encode();
                    CHECK(encoded && reader.openVolume(*encoded) && reader.open("asset.dat"));
                    CHECK(std::ranges::equal(reader.data(), std::as_bytes(std::span(payload))));
                }
            }
        }

        // A two-byte RLE run decodes to XX. Compression uses the same payload
        // bounds even when its block's DWORD padding is absent.
        auto compressed = volume("\x82X", dwordAlignment);
        compressed[recordAt + 16] = static_cast<std::byte>(STRM_COMPRESS_RLE);
        VolumeRStream reader;
        CHECK(reader.openVolume(compressed) && reader.open("asset.dat"));
        constexpr std::array expected{std::byte{'X'}, std::byte{'X'}};
        CHECK(std::ranges::equal(reader.data(), expected));
        compressed[16] = std::byte{2}; // A literal packet missing its second byte.
        CHECK(reader.openVolume(compressed) && !reader.open("asset.dat"));
    });
}

int volumeBounds() {
    return test::run([] {
        const auto original = volume("OK", 0);
        const auto rejects = [](const auto& bytes) {
            VolumeRStream reader;
            CHECK(!reader.openVolume(bytes) && reader() == STRM_IO_ERROR);
            CHECK(reader.getVolumeItems().empty() && !reader.error().empty());
            CHECK(!reader.open("asset.dat"));
        };
        // Actual payload overlaps the string-table header by one byte, with
        // matching declared size so the uncompressed size check cannot hide it.
        auto invalid = original;
        patch(invalid, 12, dwordAlignment | 3U);
        patch(invalid, recordAt + 12, 3);
        rejects(invalid);

        // A complete VBLK header starting in the directory is still invalid,
        // even with no data bytes. The unused string bytes hold the fake block.
        invalid = original;
        patch(invalid, 27, kVolFileBlockTag);
        patch(invalid, 31, 0);
        patch(invalid, recordAt + 4, ~UInt32{0});
        patch(invalid, recordAt + 8, 27);
        patch(invalid, recordAt + 12, 0);
        rejects(invalid);

        for (const auto offset : std::array{UInt32{0}, UInt32{60}, ~UInt32{0}}) {
            invalid = original;
            patch(invalid, recordAt + 8, offset);
            rejects(invalid); // PVOL header, truncated VBLK header, out of file.
        }
        invalid = original;
        patch(invalid, 12, 0xFFFFFFFFU); // Payload extent exceeds the file.
        rejects(invalid);
        invalid = original;
        patch(invalid, recordAt + 12, 3); // Uncompressed size mismatch.
        rejects(invalid);
        invalid = original;
        invalid[recordAt + 16] = std::byte{255};
        rejects(invalid);

        // Sequential directory traversal still requires the string block's
        // advertised alignment, while the final dictionary may omit its pad.
        invalid = original;
        patch(invalid, 22, dwordAlignment | 10U);
        rejects(invalid);
        auto unpaddedDictionary = original;
        unpaddedDictionary.pop_back();
        VolumeRStream reader;
        CHECK(reader.openVolume(unpaddedDictionary) && reader.open("asset.dat"));
        unpaddedDictionary.pop_back(); // Missing dictionary data is not padding.
        rejects(unpaddedDictionary);
    });
}
}

OPENT1_REGISTER_TEST("core", "volume_entry_padding", volumePadding);
OPENT1_REGISTER_TEST("core", "volume_entry_bounds", volumeBounds);
