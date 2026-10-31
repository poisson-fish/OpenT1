#pragma once
#include <opent1/core/stream_io.hpp>

namespace opent1 {
struct NormalVector3F {
    Flt32 x{}, y{}, z{};
};

// Non-owning LSB-first protocol stream. Bit operations are atomic on overflow;
// source/destination spans must not alias the borrowed backing storage.
class BitStream : public StreamIO {
public:
    explicit BitStream(std::span<std::byte> buffer) {
        setBuffer(buffer);
    }

    template <std::size_t Extent>
    explicit BitStream(std::span<std::byte, Extent> buffer)
        : BitStream(std::span<std::byte>(buffer)) {}

    explicit BitStream(std::span<const std::byte> buffer) {
        setBuffer(buffer);
    }

    void setBuffer(std::span<std::byte> buffer);

    template <std::size_t Extent> void setBuffer(std::span<std::byte, Extent> buffer) {
        setBuffer(std::span<std::byte>(buffer));
    }

    void setBuffer(std::span<const std::byte> buffer);
    // Explicit bit limits support packets whose final byte is only partially used.
    void setLimits(std::size_t readBits, std::size_t writeBits);

    [[nodiscard]] Int32 getCurPos() const noexcept {
        return static_cast<Int32>(position_);
    }

    Bool setCurPos(std::size_t position);

    [[nodiscard]] Int32 getPosition() const override {
        return static_cast<Int32>((position_ + 7) / 8);
    }

    Bool setPosition(Int32 position) override;
    [[nodiscard]] Int32 getCapabilities(StreamCap cap) const override;

    [[nodiscard]] bool isFull() const noexcept {
        return position_ >= writeLimit_;
    }

    [[nodiscard]] bool isValid() const noexcept {
        return strm_status == STRM_OK;
    }

    [[nodiscard]] std::size_t remainingReadBits() const noexcept {
        return position_ <= readLimit_ ? readLimit_ - position_ : 0;
    }

    [[nodiscard]] std::size_t remainingWriteBits() const noexcept {
        return position_ <= writeLimit_ ? writeLimit_ - position_ : 0;
    }

    // Resets cursor/status as well as clearing writable storage.
    void clear();
    void close() override;
    Bool writeBits(std::size_t bitCount, std::span<const std::byte> data);
    Bool readBits(std::size_t bitCount, std::span<std::byte> data);
    Bool writeBytes(std::span<const std::byte> data) override;
    Bool readBytes(std::span<std::byte> data) override;
    Bool writeFlag(bool value); // Returns the supplied value, as in legacy.
    [[nodiscard]] bool readFlag();
    Bool setBit(std::size_t bit, bool value);
    [[nodiscard]] bool testBit(std::size_t bit) const;
    void writeInt(Int32 value, Int32 bitCount);
    [[nodiscard]] Int32 readInt(Int32 bitCount);
    void writeSignedInt(Int32 value, Int32 bitCount);
    [[nodiscard]] Int32 readSignedInt(Int32 bitCount);
    // Legacy quantization; valid widths 1..30, finite input in [0,1]/[-1,1].
    void writeFloat(Flt32 value, Int32 bitCount);
    [[nodiscard]] Flt32 readFloat(Int32 bitCount);
    void writeSignedFloat(Flt32 value, Int32 bitCount);
    [[nodiscard]] Flt32 readSignedFloat(Int32 bitCount);
    void writeNormalVector(const NormalVector3F& vector, Int32 bitCount);
    void readNormalVector(NormalVector3F& vector, Int32 bitCount);
    void writeNormalVector(const NormalVector3F& vector, Int32 angleBits, Int32 zBits);
    void readNormalVector(NormalVector3F& vector, Int32 angleBits, Int32 zBits);
    Bool writeString(std::string_view text, std::size_t maxLength = 255) override;
    Bool readString(std::string& text) override;

private:
    Bool ready(std::size_t bitCount, bool writing);
    std::span<const std::byte> readable_;
    std::span<std::byte> writable_;
    std::size_t position_{0};
    std::size_t readLimit_{0};
    std::size_t writeLimit_{0};
    bool open_{false};
    bool readOnly_{false};
};
} // namespace opent1
