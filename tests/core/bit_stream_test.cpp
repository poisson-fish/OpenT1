#include "test_support.hpp"
#include <opent1/core/bit_stream.hpp>

namespace {
using namespace opent1;

void tests() {
    std::array<std::byte, 1024> buffer{};
    BitStream stream{std::span(buffer)};
    std::ranges::for_each(std::views::iota(0, 8), [&](int offset) {
        std::ranges::for_each(std::views::iota(0, 33), [&](int width) {
            stream.clear();
            CHECK(stream.setCurPos(static_cast<std::size_t>(offset)));
            stream.writeInt(std::bit_cast<Int32>(0xDEADBEEFU), width);
            CHECK(stream.setCurPos(static_cast<std::size_t>(offset)));
            const auto mask = width == 32 ? 0xFFFFFFFFU : (UInt32{1} << width) - 1U;
            CHECK(static_cast<UInt32>(stream.readInt(width)) == (0xDEADBEEFU & mask));
        });
    });
    stream.clear();
    CHECK(stream.writeFlag(false) == false && stream.isValid());
    CHECK(stream.writeFlag(true));
    stream.writeSignedInt(-123, 9);
    CHECK(stream.setCurPos(0) && !stream.readFlag() && stream.readFlag() &&
          stream.readSignedInt(9) == -123);
    std::string text;
    stream.clear();
    CHECK(stream.writeString("the quick brown fox jumps over the lazy dog"));
    CHECK((buffer[0] & std::byte{1}) != std::byte{0}); // Compressed path.
    CHECK(stream.writeString(std::string(255, static_cast<char>(0xFF))));
    const auto end = stream.getCurPos();
    CHECK(stream.setCurPos(0) && stream.readString(text) &&
          text == "the quick brown fox jumps over the lazy dog");
    CHECK(stream.readString(text) && text == std::string(255, static_cast<char>(0xFF)));
    CHECK(stream.getCurPos() == end);
    stream.clear();
    CHECK(stream.writeString(std::string(300, 'e')) && stream.setCurPos(0));
    CHECK(stream.readString(text) && text.size() == 255);
    stream.clear();
    CHECK(stream.writeString(std::string_view("a\0b", 3)) && stream.setCurPos(0));
    CHECK(stream.readString(text) && text == "a");

    std::array<std::byte, 1> one{std::byte{0xA5}};
    BitStream bounded{std::span(one)};
    CHECK(bounded.setCurPos(8));
    CHECK(!bounded.readFlag() && bounded() == STRM_EOS && bounded.getCurPos() == 8);
    CHECK(bounded.setCurPos(0));
    CHECK(!bounded.writeString("too long") && bounded() == STRM_IO_ERROR);
    CHECK(one[0] == std::byte{0xA5} && bounded.getCurPos() == 0);
    bounded.clear();
    bounded.setLimits(3, 3);
    CHECK(bounded.writeFlag(true) && bounded.writeFlag(true) && bounded.writeFlag(true) &&
          bounded.isFull());
    CHECK(!bounded.writeFlag(false) && bounded() == STRM_IO_ERROR && one[0] == std::byte{7});
    bounded.clrStatus();
    CHECK(bounded.setCurPos(0));
    CHECK(bounded.readFlag() && bounded.readFlag() && bounded.readFlag());
    CHECK(!bounded.readFlag() && bounded() == STRM_EOS);
    CHECK(bounded.setCurPos(0));
    std::array<std::byte, 1> result{std::byte{0xFF}};
    CHECK(bounded.readBits(3, result) && result[0] == std::byte{7});
    bounded.clear();
    CHECK(!bounded.writeBits(9, result) && bounded() == STRM_ILLEGAL_CALL);
    CHECK(bounded.getCurPos() == 0);
    test::rejects([&] {
        stream.writeInt(0, -1);
    });
    test::rejects([&] {
        (void)stream.readInt(33);
    });
    test::rejects([&] {
        stream.writeSignedInt(std::numeric_limits<Int32>::min(), 32);
    });
    test::rejects([&] {
        stream.writeFloat(0.5f, 0);
    });
    test::rejects([&] {
        stream.writeSignedFloat(1.1f, 8);
    });
    test::rejects([&] {
        stream.writeFloat(std::numeric_limits<float>::quiet_NaN(), 8);
    });
    test::rejects([&] {
        bounded.setLimits(9, 1);
    });
    test::rejects([&] {
        (void)bounded.testBit(3);
    });
    BitStream readOnly{std::span<const std::byte>(one)};
    CHECK(!readOnly.readFlag() && readOnly.isValid());
    CHECK(!readOnly.writeFlag(true) && readOnly() == STRM_ILLEGAL_CALL);
    readOnly.close();
    readOnly.clrStatus();
    CHECK(!readOnly.readFlag() && readOnly() == STRM_CLOSED);

    // Every truncated bit of an encoded string must fail without publishing a partial string.
    stream.clear();
    CHECK(stream.writeString("testing Huffman packet truncation"));
    const auto fullBits = static_cast<std::size_t>(stream.getCurPos());
    std::ranges::for_each(std::views::iota(std::size_t{0}, fullBits), [&](std::size_t limit) {
        BitStream packet{std::span<const std::byte>(buffer)};
        packet.setLimits(limit, 0);
        text = "unchanged";
        CHECK(!packet.readString(text) && text == "unchanged" && packet() == STRM_EOS);
    });
    stream.clear();
    stream.writeNormalVector(NormalVector3F{0, 0, -1}, 8, 8);
    CHECK(stream.getCurPos() == 8 && stream.setCurPos(0));
    NormalVector3F vector;
    stream.readNormalVector(vector, 8, 8);
    CHECK(vector.x == 0 && vector.y == 0 && vector.z == -1);
}
}

int opent1_test_core_bit_stream() {
    return opent1::test::run(tests);
}

#include "test_registry.hpp"
OPENT1_REGISTER_TEST("core", "core_bit_stream", opent1_test_core_bit_stream);
