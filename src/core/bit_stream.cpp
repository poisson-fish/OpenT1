#include <opent1/core/bit_stream.hpp>
#include <cmath>
#include <numeric>

namespace opent1 {
namespace {
constexpr std::array<UInt32, 256> kCharFreqs = {
    0,    0,    0,    0,    0,    0,    0,   0,   0,   329,  21,  0,   0,    0,   0,    0,
    0,    0,    0,    0,    0,    0,    0,   0,   0,   0,    0,   0,   0,    0,   0,    0,
    2809, 68,   0,    27,   0,    58,   3,   62,  4,   7,    0,   0,   15,   65,  554,  3,
    394,  404,  189,  117,  30,   51,   27,  15,  34,  32,   80,  1,   142,  3,   142,  39,
    0,    144,  125,  44,   122,  275,  70,  135, 61,  127,  8,   12,  113,  246, 122,  36,
    185,  1,    149,  309,  335,  12,   11,  14,  54,  151,  0,   0,   2,    0,   0,    211,
    0,    2090, 344,  736,  993,  2872, 701, 605, 646, 1552, 328, 305, 1240, 735, 1533, 1713,
    562,  3,    1775, 1149, 1469, 979,  407, 553, 59,  279,  31,  0,   0,    0,   68,   0,
    0,    0,    0,    0,    0,    0,    0,   0,   0,   0,    0,   0,   0,    0,   0,    0,
    0,    0,    0,    0,    0,    0,    0,   0,   0,   0,    0,   0,   0,    0,   0,    0,
    0,    0,    0,    0,    0,    0,    0,   0,   0,   0,    0,   0,   0,    0,   0,    0,
    0,    0,    0,    0,    0,    0,    0,   0,   0,   0,    0,   0,   0,    0,   0,    0,
    0,    0,    0,    0,    0,    0,    0,   0,   0,   0,    0,   0,   0,    0,   0,    0,
    0,    0,    0,    0,    0,    0,    0,   0,   0,   0,    0,   0,   0,    0,   0,    0,
    0,    0,    0,    0,    0,    0,    0,   0,   0,   0,    0,   0,   0,    0,   0,    0,
    0,    0,    0,    0,    0,    0,    0,   0,   0,   0,    0,   0,   0,    0,   0,    0};

class HuffmanTable {
public:
    struct Node {
        UInt32 population{};
        Int32 zero{}, one{};
    };

    struct Leaf {
        UInt32 population{}, code{};
        Int32 bits{};
    };

    std::array<Node, 256> nodes{};
    std::array<Leaf, 256> leaves{};

    HuffmanTable() {
        std::array<Int32, 256> wraps{};
        std::ranges::for_each(std::views::iota(0, 256), [&](Int32 i) {
            const auto c = static_cast<UInt32>(i);
            const bool alnum =
                (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
            leaves[static_cast<std::size_t>(i)].population =
                kCharFreqs[static_cast<std::size_t>(i)] + 1U + UInt32(alnum);
            wraps[static_cast<std::size_t>(i)] = -(i + 1);
        });
        const auto population = [&](Int32 index) {
            return index < 0 ? leaves[static_cast<std::size_t>(-index - 1)].population
                             : nodes[static_cast<std::size_t>(index)].population;
        };
        // Stable minimum selection and swap removal are part of the wire format.
        std::ranges::for_each(std::views::iota(1, 256), [&](Int32 next) {
            const Int32 count = 257 - next;
            UInt32 min1 = 0xFFFFFFFEU, min2 = 0xFFFFFFFFU;
            Int32 index1 = -1, index2 = -1;
            std::ranges::for_each(std::views::iota(0, count), [&](Int32 i) {
                const auto pop = population(wraps[static_cast<std::size_t>(i)]);
                if (pop < min1) {
                    min2 = min1;
                    index2 = index1;
                    min1 = pop;
                    index1 = i;
                } else if (pop < min2) {
                    min2 = pop;
                    index2 = i;
                }
            });
            nodes[static_cast<std::size_t>(next)] = {min1 + min2,
                                                     wraps[static_cast<std::size_t>(index1)],
                                                     wraps[static_cast<std::size_t>(index2)]};
            wraps[static_cast<std::size_t>(std::min(index1, index2))] = next;
            if (index2 != count - 1) {
                wraps[static_cast<std::size_t>(std::max(index1, index2))] =
                    wraps[static_cast<std::size_t>(count - 1)];
            }
        });
        nodes[0] = nodes[static_cast<std::size_t>(wraps[0])];
        generate(0, 0, 0);
    }

private:
    void generate(Int32 index, UInt32 code, Int32 depth) {
        if (index < 0) {
            auto& leaf = leaves[static_cast<std::size_t>(-index - 1)];
            leaf.code = code;
            leaf.bits = depth;
            return;
        }
        OPENT1_ASSERT_FATAL(depth < 32, "Huffman code exceeds 32 bits");
        const auto& node = nodes[static_cast<std::size_t>(index)];
        generate(node.zero, code, depth + 1);
        generate(node.one, code | (UInt32{1} << depth), depth + 1);
    }
};

const HuffmanTable& huffman() {
    static const HuffmanTable table;
    return table;
}

void checkWidth(Int32 bits, Int32 minimum, Int32 maximum) {
    OPENT1_ASSERT_FATAL(bits >= minimum && bits <= maximum, "Invalid protocol bit width");
}

void checkFloat(Flt32 value, Flt32 minimum, Flt32 maximum) {
    OPENT1_ASSERT_FATAL(std::isfinite(value) && value >= minimum && value <= maximum,
                        "Invalid quantized float");
}

void checkNormal(const NormalVector3F& v) {
    checkFloat(v.x, -1, 1);
    checkFloat(v.y, -1, 1);
    checkFloat(v.z, -1, 1);
}

UInt32 quantizer(Int32 bits) {
    checkWidth(bits, 1, 30);
    return (UInt32{1} << bits) - 1;
}
}

void BitStream::setBuffer(std::span<const std::byte> buffer) {
    OPENT1_ASSERT_FATAL(buffer.size() <=
                            static_cast<std::size_t>(std::numeric_limits<Int32>::max()) / 8,
                        "Bit buffer exceeds supported size");
    readable_ = buffer;
    writable_ = {};
    position_ = 0;
    readLimit_ = buffer.size() * 8;
    writeLimit_ = 0;
    open_ = readOnly_ = true;
    lastBytes = 0;
    clrStatus();
}

void BitStream::setBuffer(std::span<std::byte> buffer) {
    setBuffer(std::span<const std::byte>(buffer));
    writable_ = buffer;
    writeLimit_ = buffer.size() * 8;
    readOnly_ = false;
}

void BitStream::setLimits(std::size_t readBits, std::size_t writeBits) {
    OPENT1_ASSERT_FATAL(readBits <= readable_.size() * 8 && writeBits <= writable_.size() * 8,
                        "Bit limit exceeds backing span");
    OPENT1_ASSERT_FATAL(position_ <= std::max(readBits, writeBits), "Bit limit precedes cursor");
    readLimit_ = readBits;
    writeLimit_ = writeBits;
}

Bool BitStream::setCurPos(std::size_t position) {
    if (!open_) {
        return fail(STRM_CLOSED);
    }
    if (strm_status == STRM_EOS) {
        clrStatus();
    }
    if (strm_status != STRM_OK) {
        return false;
    }
    if (position > std::max(readLimit_, writeLimit_)) {
        return fail(STRM_ILLEGAL_CALL);
    }
    position_ = position;
    return true;
}

Bool BitStream::setPosition(Int32 position) {
    return position >= 0 ? setCurPos(static_cast<std::size_t>(position) * 8)
                         : fail(STRM_ILLEGAL_CALL);
}

Int32 BitStream::getCapabilities(StreamCap cap) const {
    switch (cap) {
        case STRM_CAP_TYPE:
            return readOnly_ ? STRM_BIT_R : STRM_BIT_W;
        case STRM_CAP_READ:
        case STRM_CAP_REPOSITION:
            return open_;
        case STRM_CAP_WRITE:
            return open_ && !readOnly_;
        default:
            return 0;
    }
}

void BitStream::clear() {
    if (!open_) {
        fail(STRM_CLOSED);
        return;
    }
    if (readOnly_) {
        fail(STRM_ILLEGAL_CALL);
        return;
    }
    std::ranges::fill(writable_, std::byte{0});
    position_ = 0;
    lastBytes = 0;
    clrStatus();
}

void BitStream::close() {
    readable_ = {};
    writable_ = {};
    position_ = readLimit_ = writeLimit_ = 0;
    lastBytes = 0;
    open_ = false;
    strm_status = STRM_CLOSED;
}

Bool BitStream::ready(std::size_t bits, bool writing) {
    lastBytes = 0;
    if (!open_) {
        return fail(STRM_CLOSED);
    }
    if (strm_status != STRM_OK) {
        return false;
    }
    if (writing && readOnly_) {
        return fail(STRM_ILLEGAL_CALL);
    }
    const auto limit = writing ? writeLimit_ : readLimit_;
    if (position_ > limit || bits > limit - position_) {
        return fail(writing ? STRM_IO_ERROR : STRM_EOS);
    }
    return true;
}

Bool BitStream::writeBits(std::size_t bits, std::span<const std::byte> data) {
    if (bits / 8 + (bits % 8 != 0) > data.size()) {
        lastBytes = 0;
        return fail(STRM_ILLEGAL_CALL);
    }
    if (!ready(bits, true)) {
        return false;
    }
    if (bits == 0) {
        return true;
    }
    const auto first = position_ / 8, end = (position_ + bits - 1) / 8;
    const auto shift = position_ % 8;
    std::ranges::for_each(std::views::iota(first, end + 1), [&](std::size_t dest) {
        const auto index = dest - first;
        UInt32 value = index < data.size() ? std::to_integer<UInt8>(data[index]) << shift : 0U;
        if (index != 0 && shift != 0) {
            value |= std::to_integer<UInt8>(data[index - 1]) >> (8 - shift);
        }
        if (dest == first) {
            value |= std::to_integer<UInt8>(writable_[dest]) & ((1U << shift) - 1);
        }
        writable_[dest] = static_cast<std::byte>(value & 255U);
    });
    writable_[end] &= static_cast<std::byte>((1U << ((position_ + bits - 1) % 8 + 1)) - 1);
    position_ += bits;
    lastBytes = static_cast<UInt32>((bits + 7) / 8);
    return true;
}

Bool BitStream::readBits(std::size_t bits, std::span<std::byte> data) {
    const auto count = bits / 8 + (bits % 8 != 0);
    if (count > data.size()) {
        lastBytes = 0;
        return fail(STRM_ILLEGAL_CALL);
    }
    if (!ready(bits, false)) {
        return false;
    }
    const auto first = position_ / 8, shift = position_ % 8;
    std::ranges::for_each(std::views::iota(std::size_t{0}, count), [&](std::size_t i) {
        UInt32 value = std::to_integer<UInt8>(readable_[first + i]) >> shift;
        if (shift != 0 && first + i + 1 < readable_.size()) {
            value |= std::to_integer<UInt8>(readable_[first + i + 1]) << (8 - shift);
        }
        data[i] = static_cast<std::byte>(value & 255U);
    });
    if (bits % 8 != 0) {
        data[count - 1] &= static_cast<std::byte>((1U << (bits % 8)) - 1);
    }
    position_ += bits;
    lastBytes = static_cast<UInt32>(count);
    return true;
}

Bool BitStream::writeBytes(std::span<const std::byte> data) {
    if (!ready(0, true)) {
        return false;
    }
    if (data.size() > writable_.size()) {
        lastBytes = 0;
        return fail(STRM_IO_ERROR);
    }
    return writeBits(data.size() * 8, data);
}

Bool BitStream::readBytes(std::span<std::byte> data) {
    if (!ready(0, false)) {
        return false;
    }
    if (data.size() > readable_.size()) {
        lastBytes = 0;
        return fail(STRM_EOS);
    }
    return readBits(data.size() * 8, data);
}

Bool BitStream::setBit(std::size_t bit, bool value) {
    if (!ready(0, true)) {
        return false;
    }
    if (bit >= writeLimit_) {
        return fail(STRM_ILLEGAL_CALL);
    }
    const auto mask = static_cast<std::byte>(1U << (bit % 8));
    if (value) {
        writable_[bit / 8] |= mask;
    } else {
        writable_[bit / 8] &= ~mask;
    }
    return true;
}

bool BitStream::testBit(std::size_t bit) const {
    OPENT1_ASSERT_FATAL(open_ && bit < readLimit_, "Bit index exceeds readable limit");
    return (std::to_integer<UInt8>(readable_[bit / 8]) & (1U << (bit % 8))) != 0;
}

Bool BitStream::writeFlag(bool value) {
    if (!ready(1, true)) {
        return false;
    }
    (void)setBit(position_, value);
    ++position_;
    lastBytes = 1;
    return value;
}

bool BitStream::readFlag() {
    if (!ready(1, false)) {
        return false;
    }
    const bool value = testBit(position_);
    ++position_;
    lastBytes = 1;
    return value;
}

void BitStream::writeInt(Int32 value, Int32 bits) {
    checkWidth(bits, 0, 32);
    const auto u = static_cast<UInt32>(value);
    const std::array bytes{static_cast<std::byte>(u & 255U),
                           static_cast<std::byte>((u >> 8) & 255U),
                           static_cast<std::byte>((u >> 16) & 255U),
                           static_cast<std::byte>(u >> 24)};
    (void)writeBits(static_cast<std::size_t>(bits), bytes);
}

Int32 BitStream::readInt(Int32 bits) {
    checkWidth(bits, 0, 32);
    std::array<std::byte, 4> bytes{};
    if (!readBits(static_cast<std::size_t>(bits), bytes)) {
        return 0;
    }
    UInt32 value{};
    std::ranges::for_each(std::views::iota(0U, 4U), [&](UInt32 i) {
        value |= UInt32(std::to_integer<UInt8>(bytes[i])) << (i * 8);
    });
    return std::bit_cast<Int32>(value);
}

void BitStream::writeSignedInt(Int32 value, Int32 bits) {
    checkWidth(bits, 1, 32);
    OPENT1_ASSERT_FATAL(value != std::numeric_limits<Int32>::min(),
                        "Signed magnitude cannot encode INT32_MIN");
    if (!ready(static_cast<std::size_t>(bits), true)) {
        return;
    }
    (void)writeFlag(value < 0);
    writeInt(value < 0 ? -value : value, bits - 1);
}

Int32 BitStream::readSignedInt(Int32 bits) {
    checkWidth(bits, 1, 32);
    if (!ready(static_cast<std::size_t>(bits), false)) {
        return 0;
    }
    const bool negative = readFlag();
    const auto value = readInt(bits - 1);
    return negative ? -value : value;
}

void BitStream::writeFloat(Flt32 value, Int32 bits) {
    const auto scale = quantizer(bits);
    checkFloat(value, 0, 1);
    writeInt(static_cast<Int32>(value * static_cast<Flt32>(scale)), bits);
}

Flt32 BitStream::readFloat(Int32 bits) {
    const auto scale = quantizer(bits);
    return static_cast<Flt32>(readInt(bits)) / static_cast<Flt32>(scale);
}

void BitStream::writeSignedFloat(Flt32 value, Int32 bits) {
    const auto scale = quantizer(bits);
    checkFloat(value, -1, 1);
    writeInt(static_cast<Int32>(static_cast<double>(value + 1.0f) * 0.5 * scale), bits);
}

Flt32 BitStream::readSignedFloat(Int32 bits) {
    const auto scale = quantizer(bits);
    return static_cast<Flt32>(readInt(bits) * 2) / static_cast<Flt32>(scale) - 1.0f;
}

void BitStream::writeNormalVector(const NormalVector3F& v, Int32 bits) {
    (void)quantizer(bits);
    checkNormal(v);
    writeSignedFloat(v.x, bits);
    writeSignedFloat(v.y, bits);
    (void)writeFlag(v.z < 0);
}

void BitStream::readNormalVector(NormalVector3F& v, Int32 bits) {
    NormalVector3F result;
    result.x = readSignedFloat(bits);
    result.y = readSignedFloat(bits);
    const auto square = 1.0f - result.x * result.x - result.y * result.y;
    result.z = square < 0 ? 0.0f : std::sqrt(square);
    const bool negative = readFlag();
    if (negative && square >= 0) {
        result.z = -result.z;
    }
    if (isValid()) {
        v = result;
    }
}

void BitStream::writeNormalVector(const NormalVector3F& v, Int32 angleBits, Int32 zBits) {
    (void)quantizer(angleBits);
    (void)quantizer(zBits);
    checkNormal(v);
    writeSignedFloat(v.z, zBits);
    if (!isEqual(std::abs(v.z), 1.0f)) {
        const auto angle = std::atan2(v.y, v.x);
        writeSignedFloat(static_cast<Flt32>(static_cast<double>(angle) / kRealD2Pi), angleBits);
    }
}

void BitStream::readNormalVector(NormalVector3F& v, Int32 angleBits, Int32 zBits) {
    (void)quantizer(angleBits);
    (void)quantizer(zBits);
    NormalVector3F result;
    result.z = readSignedFloat(zBits);
    if (!isEqual(std::abs(result.z), 1.0f)) {
        const auto angle = static_cast<Flt32>(kRealD2Pi * readSignedFloat(angleBits));
        const auto mult = std::sqrt(std::max(0.0f, 1.0f - result.z * result.z));
        result.x = mult * std::cos(angle);
        result.y = mult * std::sin(angle);
    }
    if (isValid()) {
        v = result;
    }
}

Bool BitStream::writeString(std::string_view text, std::size_t maxLength) {
    text = text.substr(0, text.find('\0')).substr(0, std::min(maxLength, std::size_t{255}));
    const auto& table = huffman();
    const auto bits =
        std::accumulate(text.begin(), text.end(), std::size_t{0}, [&](std::size_t sum, char c) {
            return sum + static_cast<std::size_t>(table.leaves[static_cast<UInt8>(c)].bits);
        });
    const bool compressed = bits < text.size() * 8;
    if (!ready(9 + (compressed ? bits : text.size() * 8), true)) {
        return false;
    }
    (void)writeFlag(compressed);
    writeInt(static_cast<Int32>(text.size()), 8);
    if (!compressed) {
        return writeBytes(std::as_bytes(std::span(text)));
    }
    std::ranges::for_each(text, [&](char c) {
        const auto& leaf = table.leaves[static_cast<UInt8>(c)];
        writeInt(std::bit_cast<Int32>(leaf.code), leaf.bits);
    });
    return isValid();
}

Bool BitStream::readString(std::string& text) {
    if (!ready(9, false)) {
        return false;
    }
    const bool compressed = readFlag();
    const auto length = static_cast<std::size_t>(readInt(8));
    std::string temporary(length, '\0');
    if (!compressed) {
        if (!readBytes(std::as_writable_bytes(std::span(temporary)))) {
            return false;
        }
    } else {
        const auto& table = huffman();
        std::ranges::for_each(temporary, [&](char& c) {
            if (!isValid()) {
                return;
            }
            Int32 index = 0;
            while (index >= 0 && isValid()) {
                const auto& node = table.nodes[static_cast<std::size_t>(index)];
                index = readFlag() ? node.one : node.zero;
            }
            if (isValid()) {
                c = static_cast<char>(-index - 1);
            }
        });
    }
    if (!isValid()) {
        return false;
    }
    temporary.resize(std::min(temporary.size(), temporary.find('\0')));
    text = std::move(temporary);
    return true;
}
} // namespace opent1
