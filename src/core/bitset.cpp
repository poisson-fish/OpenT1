#include <opent1/core/bitset.hpp>

namespace opent1 {

BitSetN::BitSetN(int bitCount, std::span<UInt8> bytes)
    : bytes_(bytes), bitCount_(static_cast<unsigned>(bitCount)) {
    assertFatal(bitCount == 1 || bitCount == 2 || bitCount == 4 || bitCount == 8,
                "Packed bit width must be 1, 2, 4 or 8");
}

UInt8 BitSetN::at(std::size_t index) const {
    const auto perByte = 8U / bitCount_;
    assertFatal(index / perByte < bytes_.size(), "Packed bit index out of range");
    const auto shift = (index % perByte) * bitCount_;
    return static_cast<UInt8>((bytes_[index / perByte] >> shift) & ((1U << bitCount_) - 1U));
}

void BitSetN::set(std::size_t index, UInt32 value) {
    (void)at(index);
    const auto perByte = 8U / bitCount_;
    const auto shift = (index % perByte) * bitCount_;
    const UInt32 mask = ((1U << bitCount_) - 1U) << shift;
    auto& byte = bytes_[index / perByte];
    byte = static_cast<UInt8>((byte & ~mask) | ((value << shift) & mask));
}

void BitSetN::decompress(std::span<UInt16> dst,
                         std::span<const UInt16> lookup,
                         std::size_t start) const {
    const auto perByte = 8U / bitCount_;
    // Subtract before adding so even SIZE_MAX inputs cannot wrap.
    const auto byteStart = start / perByte;
    assertFatal(byteStart <= bytes_.size(), "Packed range out of bounds");
    const auto remainingBytes = bytes_.size() - byteStart;
    const auto skipped = start % perByte;
    assertFatal(remainingBytes != 0U || (skipped == 0U && dst.empty()),
                "Packed range out of bounds");
    const auto neededBytes =
        dst.size() / perByte + ((dst.size() % perByte + skipped + perByte - 1U) / perByte);
    assertFatal(neededBytes <= remainingBytes, "Packed range out of bounds");
    const auto indices = std::views::iota(std::size_t{0}, dst.size());
    // Validate first: malformed indices/table leave output unchanged.
    assertFatal(std::ranges::all_of(indices,
                                    [&](auto i) {
                                        return at(start + i) < lookup.size();
                                    }),
                "Packed lookup index out of bounds");
    std::ranges::transform(indices, dst.begin(), [&](auto i) {
        return lookup[at(start + i)];
    });
}

void BitVector::zero(int a, int b) {
    if (a > b) {
        std::swap(a, b);
    }
    checkBit(a);
    checkBit(b);
    const auto first = static_cast<std::size_t>(a / 32);
    const auto last = static_cast<std::size_t>(b / 32);
    const UInt32 head = 0xFFFFFFFFU >> (a & 31);
    const UInt32 tail = 0xFFFFFFFFU << (31 - (b & 31));
    if (first == last) {
        words_[first] &= ~(head & tail);
        return;
    }
    words_[first] &= ~head;
    words_[last] &= ~tail;
    std::ranges::fill(std::span(words_).subspan(first + 1U, last - first - 1U), 0U);
}

void BitVector::set(int a, int b) {
    if (a > b) {
        std::swap(a, b);
    }
    checkBit(a);
    checkBit(b);
    const auto first = static_cast<std::size_t>(a / 32);
    const auto last = static_cast<std::size_t>(b / 32);
    const UInt32 head = 0xFFFFFFFFU >> (a & 31);
    const UInt32 tail = 0xFFFFFFFFU << (31 - (b & 31));
    if (first == last) {
        words_[first] |= head & tail;
        return;
    }
    words_[first] |= head;
    words_[last] |= tail;
    std::ranges::fill(std::span(words_).subspan(first + 1U, last - first - 1U), 0xFFFFFFFFU);
}

BitSquare::BitSquare(int widthShift) {
    widthShift = std::max(widthShift, 3);
    // The legacy bit count was a signed 32-bit value; avoid overflow on wasm32 too.
    assertFatal(widthShift <= 15, "Bit square width out of range");
    bitWidth_ = 1 << widthShift;
    const auto width = static_cast<std::size_t>(bitWidth_);
    data_.resize(width * (width / 8U), 0U);
}

bool BitSquare::contains(int x, int y) const noexcept {
    return x >= 0 && y >= 0 && x < bitWidth_ && y < bitWidth_;
}

std::size_t BitSquare::byteOff(int x, int y) const noexcept {
    return static_cast<std::size_t>(y) * static_cast<std::size_t>(bitWidth_ / 8) +
           static_cast<std::size_t>(x / 8);
}

bool BitSquare::set(int x, int y) {
    if (!contains(x, y)) {
        return false;
    }
    data_[byteOff(x, y)] |= static_cast<UInt8>(1U << (x & 7));
    return true;
}

void BitSquare::zero(int x, int y) {
    assertFatal(contains(x, y), "Bit square index out of range");
    data_[byteOff(x, y)] &= static_cast<UInt8>(~(1U << (x & 7)));
}

bool BitSquare::test(int x, int y) const {
    return contains(x, y) && (data_[byteOff(x, y)] & (1U << (x & 7))) != 0U;
}

bool BitSquare::setBox8(int x, int y, int w, int h) {
    return set(x, y, std::min(w, 8), h);
}

bool BitSquare::set(int x, int y, int w, int h) {
    if (!contains(x, y) || w <= 0 || h <= 0) {
        return false;
    }
    w = std::min(w, bitWidth_ - x);
    h = std::min(h, bitWidth_ - y);
    const auto firstByte = x / 8;
    const auto lastByte = (x + w - 1) / 8;
    const auto head = static_cast<UInt8>(0xFFU << (x & 7));
    const auto tail = static_cast<UInt8>(0xFFU >> (7 - ((x + w - 1) & 7)));
    std::ranges::for_each(std::views::iota(y, y + h), [&](int row) {
        if (firstByte == lastByte) {
            data_[byteOff(x, row)] |= head & tail;
        } else {
            data_[byteOff(x, row)] |= head;
            data_[byteOff(x + w - 1, row)] |= tail;
            std::ranges::fill(
                std::span(data_).subspan(byteOff(x, row) + 1U,
                                         static_cast<std::size_t>(lastByte - firstByte - 1)),
                UInt8{0xFF});
        }
    });
    return true;
}

bool BitSquare::testPwr2Sqr(int x, int y, int pwr) const {
    if (!contains(x, y) || pwr < 0 || pwr > 15) {
        return false;
    }
    const auto width = 1 << pwr;
    if (width > bitWidth_ - x || width > bitWidth_ - y || x % width != 0 || y % width != 0) {
        return false;
    }
    const auto byteCount = static_cast<std::size_t>(std::max(width / 8, 1));
    const auto mask = width < 8 ? static_cast<UInt8>(((1U << width) - 1U) << (x & 7)) : UInt8{0xFF};
    return std::ranges::any_of(std::views::iota(y, y + width), [&](int row) {
        return std::ranges::any_of(std::span(data_).subspan(byteOff(x, row), byteCount),
                                   [mask](UInt8 byte) {
                                       return (byte & mask) != 0U;
                                   });
    });
}

} // namespace opent1
