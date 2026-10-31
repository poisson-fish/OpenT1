#pragma once

#include <opent1/core/base.hpp>
#include <algorithm>
#include <array>
#include <bit>
#include <ranges>
#include <span>
#include <string>
#include <string_view>

namespace opent1 {
enum StreamType : Int32 {
    STRM_BASE,
    STRM_FILE_RW,
    STRM_FILE_R,
    STRM_FILE_W,
    STRM_MEM_RW,
    STRM_MEM_R,
    STRM_MEM_W,
    STRM_RLE_R,
    STRM_RLE_W,
    STRM_LZH_R,
    STRM_LZH_W,
    STRM_BIT_R,
    STRM_BIT_W,
    STRM_BUFF_R,
    STRM_BUFF_W
};

enum StreamCompressType : Int32 {
    STRM_COMPRESS_NONE,
    STRM_COMPRESS_RLE,
    STRM_COMPRESS_LZ,
    STRM_COMPRESS_LZH
};

enum StreamCap : Int32 {
    STRM_CAP_TYPE,
    STRM_CAP_WRITE,
    STRM_CAP_READ,
    STRM_CAP_REPOSITION,
    STRM_CAP_COMPRESS,
    STRM_CAP_LOCKABLE
};

enum StreamStatus : Int32 {
    STRM_OK,
    STRM_IO_ERROR,
    STRM_EOS,
    STRM_UNKNOWN_ERROR,
    STRM_ILLEGAL_CALL,
    STRM_CLOSED,
    STRM_FLTR_DETACHED
};

template <class T>
concept StreamInteger = std::integral<T> && !std::same_as<T, bool> &&
                        (sizeof(T) == 1 || sizeof(T) == 2 || sizeof(T) == 4 || sizeof(T) == 8);

// Byte operations are bounded; primitive encoding is explicitly little-endian.
// Positions retain the legacy nonnegative Int32 range. Streams are not synchronized.
class StreamIO {
public:
    virtual ~StreamIO() = default;

    [[nodiscard]] StreamStatus operator()() const noexcept {
        return getStatus();
    }

    [[nodiscard]] virtual StreamStatus getStatus() const noexcept {
        return strm_status;
    }

    void clrStatus() noexcept {
        strm_status = STRM_OK;
    }

    [[nodiscard]] UInt32 getLastSize() const noexcept {
        return lastBytes;
    }

    [[nodiscard]] virtual Int32 getPosition() const {
        return 0;
    }

    virtual Bool setPosition(Int32);
    [[nodiscard]] virtual Int32 getCapabilities(StreamCap cap) const;

    virtual Bool flush() {
        return strm_status == STRM_OK;
    }

    virtual void close() {
        strm_status = STRM_CLOSED;
        lastBytes = 0;
    }

    virtual Bool writeBytes(std::span<const std::byte>);
    virtual Bool readBytes(std::span<std::byte>);

    [[nodiscard]] virtual std::span<std::byte> lockBytes() {
        return {};
    }

    template <StreamInteger T> Bool write(T value) {
        using U = std::make_unsigned_t<T>;
        const U bits = static_cast<U>(value);
        std::array<std::byte, sizeof(T)> bytes{};
        std::ranges::transform(
            std::views::iota(std::size_t{0}, sizeof(T)), bytes.begin(), [bits](std::size_t i) {
                return static_cast<std::byte>((bits >> (i * 8U)) & U{255});
            });
        return writeBytes(bytes);
    }

    template <StreamInteger T> Bool read(T& value) {
        std::array<std::byte, sizeof(T)> bytes{};
        if (!readBytes(bytes)) {
            return false;
        }
        using U = std::make_unsigned_t<T>;
        U bits = 0;
        std::ranges::for_each(std::views::iota(std::size_t{0}, sizeof(T)), [&](std::size_t i) {
            bits = static_cast<U>(bits |
                                  (static_cast<U>(std::to_integer<UInt8>(bytes[i])) << (i * 8U)));
        });
        value = std::bit_cast<T>(bits);
        return true;
    }

    template <std::same_as<bool> T> Bool write(T value) {
        return write(UInt8(value ? 1 : 0));
    }

    Bool read(bool& value) {
        UInt8 byte{};
        if (!read(byte)) {
            return false;
        }
        value = byte != 0;
        return true;
    }

    Bool write(Flt32 value) {
        return write(std::bit_cast<UInt32>(value));
    }

    Bool write(Flt64 value) {
        return write(std::bit_cast<UInt64>(value));
    }

    Bool read(Flt32& value) {
        UInt32 bits{};
        if (!read(bits)) {
            return false;
        }
        value = std::bit_cast<Flt32>(bits);
        return true;
    }

    Bool read(Flt64& value) {
        UInt64 bits{};
        if (!read(bits)) {
            return false;
        }
        value = std::bit_cast<Flt64>(bits);
        return true;
    }

    // Legacy String: UInt32 length including NUL. std::string's empty value is
    // encoded as a non-null empty String (length 1); length 0 also reads as empty.
    Bool write(std::string_view text);
    Bool read(std::string& text, UInt32 maxLength = 16U * 1024U * 1024U);
    // Legacy Pascal text; overridden by BitStream's Huffman text codec.
    virtual Bool writeString(std::string_view text, std::size_t maxLength = 255);
    virtual Bool readString(std::string& text);
    [[nodiscard]] StringTableEntry readSTString(bool caseSensitive = false);
    Bool writeCharString(std::string_view text);
    Bool readCharString(std::string& text);

    StreamIO() = default;
    StreamIO(const StreamIO&) = delete;
    StreamIO& operator=(const StreamIO&) = delete;
    StreamIO(StreamIO&&) = default;
    StreamIO& operator=(StreamIO&&) = default;

protected:
    UInt32 lastBytes{0};
    StreamStatus strm_status{STRM_OK};

    Bool fail(StreamStatus status) noexcept {
        strm_status = status;
        return false;
    }
};
} // namespace opent1
