#pragma once

#include <opent1/core/base.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <ranges>
#include <span>
#include <utility>
#include <vector>

namespace opent1 {

inline constexpr UInt32 kBitByteCount = 8U;
inline constexpr UInt32 kBitInt32Count = sizeof(UInt32) * kBitByteCount;

[[nodiscard]] constexpr UInt32 bitMask(UInt32 shift) {
    if (shift >= 32U) {
        assertFatal(false, "Bit mask shift out of range");
    }
    return UInt32{1} << shift;
}

using BitMask32 = UInt32;
using BitMask16 = UInt16;

// -----------------------------------------------------------------------------
// BitSet32 & BitSet16 (Modernized from darkstar/Core/inc/bitset.h)
// -----------------------------------------------------------------------------
class BitSet32 {
public:
    constexpr BitSet32() noexcept = default;

    constexpr BitSet32(BitMask32 s) noexcept : bset_(s) {}

    [[nodiscard]] constexpr operator UInt32() const noexcept {
        return bset_;
    }

    constexpr BitMask32 operator=(BitMask32 m) noexcept {
        return bset_ = m;
    }

    constexpr BitMask32 operator|=(BitMask32 m) noexcept {
        return bset_ |= m;
    }

    constexpr BitMask32 operator&=(BitMask32 m) noexcept {
        return bset_ &= m;
    }

    constexpr BitMask32 operator^=(BitMask32 m) noexcept {
        return bset_ ^= m;
    }

    constexpr BitMask32 set() noexcept {
        return bset_ = ~0U;
    }

    constexpr BitMask32 set(BitMask32 s) noexcept {
        return bset_ |= s;
    }

    constexpr BitMask32 set(BitMask32 s, bool b) noexcept {
        return bset_ = (bset_ & ~s) | (b ? s : 0U);
    }

    constexpr BitMask32 clear() noexcept {
        return bset_ = 0U;
    }

    constexpr BitMask32 clear(BitMask32 s) noexcept {
        return bset_ &= ~s;
    }

    [[nodiscard]] constexpr bool test(BitMask32 s) const noexcept {
        return (bset_ & s) != 0U;
    }

    constexpr BitMask32 toggle(BitMask32 s) noexcept {
        return bset_ ^= s;
    }

    [[nodiscard]] constexpr BitMask32 mask() const noexcept {
        return bset_;
    }

    [[nodiscard]] constexpr Int32 popcount() const noexcept {
        return std::popcount(bset_);
    }

    constexpr bool operator==(const BitSet32&) const noexcept = default;

private:
    BitMask32 bset_{0U};
};

class BitSet16 {
public:
    constexpr BitSet16() noexcept = default;

    constexpr BitSet16(BitMask16 s) noexcept : bset_(s) {}

    [[nodiscard]] constexpr operator UInt32() const noexcept {
        return bset_;
    }

    constexpr BitMask16 operator=(BitMask16 m) noexcept {
        return bset_ = m;
    }

    constexpr BitMask16 set() noexcept {
        return bset_ = static_cast<BitMask16>(~0U);
    }

    constexpr BitMask16 set(BitMask16 s) noexcept {
        return bset_ |= s;
    }

    constexpr BitMask16 clear() noexcept {
        return bset_ = 0U;
    }

    constexpr BitMask16 clear(BitMask16 s) noexcept {
        return bset_ &= static_cast<BitMask16>(~s);
    }

    [[nodiscard]] constexpr UInt16 test(BitMask16 s) const noexcept {
        return static_cast<UInt16>(bset_ & s);
    }

    [[nodiscard]] constexpr BitMask16 mask() const noexcept {
        return bset_;
    }

    constexpr bool operator==(const BitSet16&) const noexcept = default;

private:
    BitMask16 bset_{0U};
};

// -----------------------------------------------------------------------------
// BitSetX & BitSetN (Packed 1-, 2-, 4-, 8-bit arrays for lightmap decompression)
// -----------------------------------------------------------------------------
template <std::size_t LogOfBitSize>
    requires(LogOfBitSize <= 2)
class BitSetX {
public:
    static constexpr int kNumBits = 1 << LogOfBitSize;
    static constexpr int kMask = (1 << kNumBits) - 1;
    static constexpr int kDownShift = 3 - static_cast<int>(LogOfBitSize);
    static constexpr int kShiftMask = (1 << kDownShift) - 1;

    constexpr BitSetX() noexcept = default;

    explicit constexpr BitSetX(std::span<UInt8> bytes) noexcept : vec_(bytes) {}

    constexpr void initBytes(std::span<UInt8> bytes) noexcept {
        vec_ = bytes;
    }

    [[nodiscard]] constexpr UInt16 operator[](std::size_t i) const {
        if ((i >> kDownShift) >= vec_.size()) {
            assertFatal(false, "Packed bit index out of range");
        }
        const auto shift = (i & kShiftMask) << LogOfBitSize;
        return static_cast<UInt16>((vec_[i >> kDownShift] >> shift) & kMask);
    }

    constexpr void set(std::size_t i, UInt32 value) {
        if ((i >> kDownShift) >= vec_.size()) {
            assertFatal(false, "Packed bit index out of range");
        }
        const auto shift = (i & kShiftMask) << LogOfBitSize;
        const UInt32 mask = static_cast<UInt32>(kMask) << shift;
        auto& byte = vec_[i >> kDownShift];
        byte = static_cast<UInt8>((byte & ~mask) | ((value << shift) & mask));
    }

private:
    std::span<UInt8> vec_{};
};

// Bytes are in legacy little-endian bit order, independent of host endian/alignment.
class BitSetN {
public:
    BitSetN(int bitCount, std::span<UInt8> bytes);
    void set(std::size_t index, UInt32 value);
    void
    decompress(std::span<UInt16> dst, std::span<const UInt16> lookup, std::size_t start = 0) const;

private:
    [[nodiscard]] UInt8 at(std::size_t index) const;
    std::span<UInt8> bytes_;
    unsigned bitCount_;
};

template <std::size_t NumFlags> class BitFlagArray {
public:
    constexpr void clear() noexcept {
        data_.fill(0U);
    }

    [[nodiscard]] constexpr UInt16 operator[](std::size_t i) const {
        if (i >= NumFlags) {
            assertFatal(false, "Flag index out of range");
        }
        return static_cast<UInt16>((data_[i / 8U] >> (i % 8U)) & 1U);
    }

    constexpr void set(std::size_t i, UInt32 value) {
        if (i >= NumFlags) {
            assertFatal(false, "Flag index out of range");
        }
        BitSetX<0>(data_).set(i, value);
    }

private:
    std::array<UInt8, NumFlags / 8U + (NumFlags % 8U != 0U)> data_{};
};

// -----------------------------------------------------------------------------
// BitVector (Modernized from darkstar/Core/inc/tBitVector.h)
// -----------------------------------------------------------------------------
class BitVector {
public:
    BitVector() = default;

    explicit BitVector(int bitSize) {
        setSize(bitSize);
    }

    void setSize(int bitSize) {
        assertFatal(bitSize >= 0, "Negative bit vector size");
        const auto count = static_cast<std::size_t>(bitSize);
        words_.resize(count / 32U + (count % 32U != 0U), 0U);
    }

    [[nodiscard]] std::size_t bitCapacity() const noexcept {
        return words_.size() * 32U;
    }

    [[nodiscard]] int size() const noexcept {
        return static_cast<int>(words_.size());
    }

    [[nodiscard]] std::span<UInt32> words() noexcept {
        return words_;
    }

    [[nodiscard]] std::span<const UInt32> words() const noexcept {
        return words_;
    }

    void zero() noexcept {
        std::ranges::fill(words_, 0U);
    }

    void zero(int bit) {
        checkBit(bit);
        words_[static_cast<std::size_t>(bit >> 5)] &= ~(0x80000000U >> (bit & 0x1F));
    }

    void zero(int a, int b);

    void set() noexcept {
        std::ranges::fill(words_, 0xFFFFFFFFU);
    }

    void set(int bit) {
        checkBit(bit);
        words_[static_cast<std::size_t>(bit >> 5)] |= (0x80000000U >> (bit & 0x1F));
    }

    void set(int a, int b);

    [[nodiscard]] bool test() const noexcept {
        return std::ranges::any_of(words_, [](UInt32 w) {
            return w != 0U;
        });
    }

    [[nodiscard]] bool test(int bit) const {
        checkBit(bit);
        return (words_[static_cast<std::size_t>(bit >> 5)] & (0x80000000U >> (bit & 0x1F))) != 0U;
    }

    void bitNot() noexcept {
        std::ranges::for_each(words_, [](UInt32& w) {
            w = ~w;
        });
    }

    void bitAnd(const BitVector& other) noexcept {
        const std::size_t n = std::min(words_.size(), other.words_.size());
        std::ranges::transform(std::span(words_).first(n),
                               std::span(other.words_).first(n),
                               words_.begin(),
                               [](UInt32 a, UInt32 b) {
                                   return a & b;
                               });
    }

    void bitOr(const BitVector& other) noexcept {
        const std::size_t n = std::min(words_.size(), other.words_.size());
        std::ranges::transform(std::span(words_).first(n),
                               std::span(other.words_).first(n),
                               words_.begin(),
                               [](UInt32 a, UInt32 b) {
                                   return a | b;
                               });
    }

    void bitXor(const BitVector& other) noexcept {
        const std::size_t n = std::min(words_.size(), other.words_.size());
        std::ranges::transform(std::span(words_).first(n),
                               std::span(other.words_).first(n),
                               words_.begin(),
                               [](UInt32 a, UInt32 b) {
                                   return a ^ b;
                               });
    }

private:
    void checkBit(int bit) const {
        assertFatal(bit >= 0 && static_cast<std::size_t>(bit) < bitCapacity(),
                    "Bit vector index out of range");
    }

    std::vector<UInt32> words_;
};

// -----------------------------------------------------------------------------
// BitSquare (Modernized RAII 2D power-of-two bit grid from Core/inc/BitGrid.h)
// -----------------------------------------------------------------------------
class BitSquare {
public:
    explicit BitSquare(int widthShift = 3);
    bool set(int x, int y);

    bool set() noexcept {
        std::ranges::fill(data_, UInt8{0xFF});
        return true;
    }

    void zero(int x, int y);

    void zero() noexcept {
        std::ranges::fill(data_, UInt8{0});
    }

    [[nodiscard]] bool test(int x, int y) const;
    bool setBox8(int x, int y, int w, int h);
    bool set(int x, int y, int w, int h);
    [[nodiscard]] bool testPwr2Sqr(int x, int y, int pwr) const;

private:
    [[nodiscard]] bool contains(int x, int y) const noexcept;
    [[nodiscard]] std::size_t byteOff(int x, int y) const noexcept;
    std::vector<UInt8> data_;
    int bitWidth_{};
};

} // namespace opent1
