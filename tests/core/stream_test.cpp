#include "test_support.hpp"
#include <opent1/core/mem_stream.hpp>
#include <opent1/core/block_stream.hpp>
#include <array>
#include <limits>

namespace {
using namespace opent1;
using namespace std::literals;

void tests() {
    std::array<std::byte, 512> bytes{};
    MemRWStream stream{std::span(bytes)};
    CHECK(stream.write(UInt8{0xFE}) && stream.write(Int16{-2}) && stream.write(UInt32{0x12345678}));
    CHECK(stream.write(-0.0f) && stream.write(1.0));
    constexpr std::array<UInt8, 19> expected{
        0xFE, 0xFE, 0xFF, 0x78, 0x56, 0x34, 0x12, 0, 0, 0, 0x80, 0, 0, 0, 0, 0, 0, 0xF0, 0x3F};
    CHECK(
        std::ranges::equal(std::span(bytes).first(expected.size()), expected, {}, [](std::byte b) {
            return std::to_integer<UInt8>(b);
        }));
    CHECK(stream.setPosition(0));
    UInt8 u8{};
    Int16 i16{};
    UInt32 u32{};
    Flt32 f32{};
    Flt64 f64{};
    CHECK(stream.read(u8) && u8 == 0xFE);
    CHECK(stream.read(i16) && i16 == -2);
    CHECK(stream.read(u32) && u32 == 0x12345678);
    CHECK(stream.read(f32) && std::bit_cast<UInt32>(f32) == 0x80000000U);
    CHECK(stream.read(f64) && f64 == 1);
    CHECK(stream.setPosition(0) && stream.write(""sv));
    CHECK(bytes[0] == std::byte{1} && bytes[4] == std::byte{0});
    CHECK(stream.write("hello"sv) && stream.writeString("abc") && stream.writeCharString(""));
    const auto end = stream.getPosition();
    CHECK(stream.setPosition(0));
    std::string text = "unchanged";
    CHECK(stream.read(text) && text.empty());
    CHECK(stream.read(text) && text == "hello");
    CHECK(stream.readString(text) && text == "abc");
    CHECK(stream.readCharString(text) && text.empty());
    CHECK(stream.getPosition() == end);
    CHECK(stream.setPosition(0));
    CHECK(stream.writeString(std::string(300, 'a')));
    CHECK(stream.getPosition() == 256);
    CHECK(stream.setPosition(0) && stream.readString(text) && text.size() == 255);
    test::rejects([&] {
        (void)stream.writeCharString(std::string(256, 'a'));
    });
    CHECK(stream.setPosition(0) && stream.write(UInt32{0xFFFFFFFF}));
    CHECK(stream.setPosition(0));
    text = "unchanged";
    CHECK(!stream.read(text) && text == "unchanged");
    CHECK(stream.getStatus() == STRM_ILLEGAL_CALL);
    stream.clrStatus();
    CHECK(stream.setPosition(0) && stream.write(UInt32{2}) && stream.write(UInt16{0x4142}));
    CHECK(stream.setPosition(0) && !stream.read(text) && text == "unchanged");
    CHECK(stream.getStatus() == STRM_IO_ERROR);

    std::array<std::byte, 3> small{std::byte{1}, std::byte{2}, std::byte{3}};
    MemRWStream limited{std::span(small)};
    UInt32 sentinel = 0x12345678;
    CHECK(!limited.read(sentinel) && sentinel == 0x12345678);
    CHECK(limited.getLastSize() == 3 && limited.getPosition() == 3 && limited() == STRM_EOS);
    CHECK(limited.setPosition(0) && limited() == STRM_OK);
    CHECK(!limited.write(UInt32{0xAABBCCDD}));
    CHECK(limited.getLastSize() == 3 && limited() == STRM_IO_ERROR);
    CHECK(!limited.setPosition(0));
    limited.clrStatus();
    CHECK(limited.setPosition(0));
    CHECK(limited.lockBytes().size() == 3);
    limited.setLockable(false);
    CHECK(limited.lockBytes().empty());
    MemRWStream readOnly{std::span<const std::byte>(small)};
    CHECK(readOnly.getCapabilities(STRM_CAP_WRITE) == 0 && readOnly.lockBytes().empty());
    CHECK(!readOnly.write(UInt8{1}) && readOnly() == STRM_ILLEGAL_CALL);
    MemRWStream empty{std::span<std::byte>{}};
    CHECK(empty.readBytes({}) && empty.writeBytes({}) && empty.setPosition(0));
    CHECK(!empty.read(u8) && empty() == STRM_EOS);
    empty.close();
    empty.clrStatus();
    CHECK(!empty.readBytes({}) && empty() == STRM_CLOSED);
    StreamIO base;
    CHECK(!base.write(UInt8{1}) && base() == STRM_ILLEGAL_CALL);

    stream.clrStatus();
    CHECK(stream.setPosition(0));
    std::ranges::fill(bytes, std::byte{0xCC});
    BlockRWStream block;
    CHECK(block.open(stream, 0x12345678, 0, true));
    CHECK(block.write(UInt32{0xAABBCCDD}) && block.write(UInt8{0xEF}));
    CHECK(block.setPosition(0) && block.write(UInt8{0x11}));
    block.close();
    CHECK(block() == STRM_CLOSED && stream.getPosition() == 16);
    CHECK(bytes[4] == std::byte{5} && bytes[7] == std::byte{0x80});
    CHECK(bytes[8] == std::byte{0x11} && bytes[12] == std::byte{0xEF});
    CHECK(bytes[13] == std::byte{0} && bytes[15] == std::byte{0});
    CHECK(stream.setPosition(0));
    BlockRStream readBlock;
    CHECK(readBlock.open(stream, 0x12345678) && readBlock.getDataSize() == 5 &&
          readBlock.getBlockSize() == 16);
    std::array<std::byte, 6> output{};
    CHECK(!readBlock.readBytes(output) && readBlock() == STRM_EOS && readBlock.getLastSize() == 5);
    CHECK(readBlock.setPosition(0) && readBlock.read(u32) && u32 == 0xAABBCC11);
    CHECK(!readBlock.write(UInt8{0}) && readBlock() == STRM_ILLEGAL_CALL);
    readBlock.close();
    CHECK(stream.getPosition() == 16);

    CHECK(stream.setPosition(0));
    CHECK(block.open(stream, 7, 3, false) && block.write(UInt8{9}));
    block.close();
    CHECK(stream.getPosition() == 12 && bytes[9] == std::byte{0} && bytes[11] == std::byte{0});
    CHECK(stream.setPosition(0));
    CHECK(readBlock.open(stream));
    readBlock.undoOpen();
    CHECK(stream.getPosition() == 0);
    CHECK(!readBlock.open(stream, 99) && stream.getPosition() == 12);
    CHECK(stream.setPosition(0) && stream.write(Int32{1}) && stream.write(UInt32{0}));
    CHECK(stream.setPosition(0) && readBlock.open(stream));
    CHECK(!readBlock.read(u8) && readBlock() == STRM_EOS && readBlock.getLastSize() == 0);
    readBlock.close();
    CHECK(stream.getPosition() == 8);
    CHECK(stream.setPosition(0) && stream.write(Int32{1}) && stream.write(UInt32{0xFFFFFFFF}));
    CHECK(stream.setPosition(0) && !readBlock.open(stream) && readBlock() == STRM_ILLEGAL_CALL);
    CHECK(!block.open(stream, 1, std::numeric_limits<Int32>::max(), true));
}
}

int opent1_test_core_stream() {
    return opent1::test::run(tests);
}

#include "test_registry.hpp"
OPENT1_REGISTER_TEST("core", "core_stream", opent1_test_core_stream);
