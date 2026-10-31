#include "test_support.hpp"
#include <opent1/core/mem_stream.hpp>
#include <opent1/core/rle_stream.hpp>
#include <opent1/core/lzh_stream.hpp>
#include <vector>

namespace {
using namespace opent1;

template <class Writer, class Reader> void codec() {
    std::array<std::byte, 2048> storage{};
    MemRWStream parent{std::span(storage)};
    CHECK(parent.write(UInt32{0xDEADBEEF}));
    Writer writer(parent);
    std::array<std::byte, 512> original{};
    std::ranges::for_each(std::views::iota(std::size_t{0}, original.size()), [&](std::size_t i) {
        original[i] = static_cast<std::byte>((i / 5) & 255U);
    });
    CHECK(writer.writeBytes(original));
    CHECK(writer.getPosition() == 512 && writer.getLastSize() == 512);
    CHECK(writer.flush());
    const auto length = static_cast<std::size_t>(parent.getPosition());
    writer.detach();
    MemRWStream source{std::span<const std::byte>(storage).first(length)};
    CHECK(source.setPosition(4));
    Reader reader(source);
    std::array<std::byte, 512> decoded{};
    CHECK(reader.readBytes(std::span(decoded).first(300)) && reader.getPosition() == 300);
    CHECK(reader.setPosition(7) && reader.getPosition() == 7);
    CHECK(reader.readBytes(std::span(decoded).subspan(7, 200)));
    CHECK(std::ranges::equal(std::span(decoded).subspan(7, 200),
                             std::span(original).subspan(7, 200)));
    CHECK(reader.setPosition(0) && reader.readBytes(decoded) && decoded == original);
    reader.detach();
    CHECK(!reader.readBytes(decoded) && reader() == STRM_FLTR_DETACHED);
    CHECK(!writer.writeBytes(original) && writer() == STRM_FLTR_DETACHED);
    writer.attach(parent);
    CHECK(writer.writeBytes(std::span(original).first(1)));
    CHECK(writer.flush());
    writer.detach();

    // Every strict prefix fails to produce the complete uncompressed payload.
    std::ranges::for_each(std::views::iota(std::size_t{4}, length - 1), [&](std::size_t end) {
        MemRWStream truncated{std::span<const std::byte>(storage).first(end)};
        CHECK(truncated.setPosition(4));
        Reader shortReader(truncated);
        std::array<std::byte, 512> output{};
        CHECK(!shortReader.readBytes(output));
        CHECK(shortReader() == STRM_EOS || shortReader() == STRM_IO_ERROR);
        CHECK(shortReader.getLastSize() < original.size());
    });
    // A full compressed stream has no decoded-length field. Caller owns the
    // expected length; this test only checks physical input exhaustion.
    MemRWStream input{std::span<const std::byte>(storage).subspan(4, length - 4)};
    Reader atEnd(input);
    std::array<std::byte, 2048> tooMuch{};
    CHECK(!atEnd.readBytes(tooMuch) && atEnd.getLastSize() >= original.size());

    std::array<std::byte, 1> tiny{};
    MemRWStream sink{std::span(tiny)};
    Writer failure(sink);
    const bool accepted = failure.writeBytes(original);
    CHECK(!accepted || !failure.flush());
    CHECK(failure() == STRM_IO_ERROR);
    failure.detach();
    CHECK(failure() == STRM_IO_ERROR);
    Writer detached;
    CHECK(!detached.writeBytes({}) && detached() == STRM_FLTR_DETACHED);
    test::rejects([&] {
        detached.attach(detached);
    });
}

void tests() {
    codec<RLEWStream, RLERStream>();
    codec<LZHWStream, LZHRStream>();
    std::array<std::byte, 32> storage{};
    MemRWStream parent{std::span(storage)};
    RLEWStream writer(parent);
    CHECK(writer.flush() && parent.getPosition() == 0);
    CHECK(writer.write(UInt8{7}) && writer.flush());
    const auto position = parent.getPosition();
    CHECK(writer.flush() && parent.getPosition() == position);
    writer.detach();
    std::array<std::byte, 1> corrupt{std::byte{0}};
    MemRWStream bad{std::span<const std::byte>(corrupt)};
    RLERStream reader(bad);
    UInt8 value{};
    CHECK(!reader.read(value) && reader() == STRM_IO_ERROR);
    LZHuffman unattached;
    test::rejects([&] {
        (void)unattached.decodeChar();
    });
    unattached.attach(parent);
    test::rejects([&] {
        unattached.encodeChar(9999);
    });
    test::rejects([&] {
        unattached.encodePosition(4096);
    });
}
}

int opent1_test_core_compression() {
    return opent1::test::run(tests);
}

#include "test_registry.hpp"
OPENT1_REGISTER_TEST("core", "core_compression", opent1_test_core_compression);
