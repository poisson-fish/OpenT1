#pragma once

#include <opent1/core/stream_io.hpp>

#include <array>
#include <memory>

namespace opent1 {

inline constexpr int kLzhBuffSize = 4096;
inline constexpr int kLzhLookahead = 60;
inline constexpr int kLzhThreshold = 2;
inline constexpr int kLzhNil = kLzhBuffSize;
inline constexpr int kLzhNChar = (256 - kLzhThreshold + kLzhLookahead);
inline constexpr int kLzhTableSize = (kLzhNChar * 2 - 1);
inline constexpr int kLzhRoot = (kLzhTableSize - 1);
inline constexpr int kLzhMaxFreq = 0x8000;

// -----------------------------------------------------------------------------
// LZHuffman - Adaptive Huffman + Position Table Codec (Darkstar lzhuff.cpp)
// -----------------------------------------------------------------------------
class LZHuffman {
public:
    LZHuffman() noexcept;
    ~LZHuffman() = default;

    LZHuffman(const LZHuffman&) = delete;
    LZHuffman& operator=(const LZHuffman&) = delete;

    void attach(StreamIO& s) noexcept;
    void flush();
    void encodeChar(unsigned int c);
    void encodePosition(unsigned int c);
    [[nodiscard]] int decodeChar();
    [[nodiscard]] int decodePosition();

private:
    [[nodiscard]] int getBit();
    [[nodiscard]] int getByte();
    void putCode(int l, unsigned int c);
    void reconst() noexcept;
    void update(int c) noexcept;

    static const std::array<UInt8, 64> p_len;
    static const std::array<UInt8, 64> p_code;
    static const std::array<UInt8, 256> d_code;
    static const std::array<UInt8, 256> d_len;

    StreamIO* d_stream_{nullptr};
    std::array<UInt16, kLzhTableSize + 1> freq_{};
    std::array<Int16, kLzhTableSize + kLzhNChar> prnt_{};
    std::array<Int16, kLzhTableSize> son_{};

    UInt16 d_getbuf_{0};
    UInt8 d_getlen_{0};
    UInt16 d_putbuf_{0};
    UInt8 d_putlen_{0};
};

// -----------------------------------------------------------------------------
// LZHWStream - LZSS + adaptive Huffman. flush() finalizes and detaches.
// -----------------------------------------------------------------------------
class LZHWStream : public StreamIO {
public:
    LZHWStream();
    explicit LZHWStream(StreamIO& s);
    ~LZHWStream() override;

    LZHWStream(const LZHWStream&) = delete;
    LZHWStream& operator=(const LZHWStream&) = delete;

    void attach(StreamIO& s);
    void detach();
    void close() override;

    [[nodiscard]] Int32 getPosition() const override;
    Bool setPosition(Int32 pos) override;
    [[nodiscard]] Int32 getCapabilities(StreamCap cap) const override;
    Bool flush() override;

    using StreamIO::read;
    using StreamIO::write;

    Bool writeBytes(std::span<const std::byte> data) override;

private:
    void insertNode(int r) noexcept;
    void deleteNode(int p) noexcept;
    void initialize();

    StreamIO* d_stream_{nullptr};
    std::unique_ptr<LZHuffman> d_huff_;
    int iFileSize_{0};
    int ER_{0};
    int ES_{0};
    int ELEN_{0};
    int EFULL_{0};
    int ELM_{0};
    int ELF_{0};
    int match_position_{0};
    int match_length_{0};
    Int32 curPos_{0};

    std::array<UInt8, kLzhBuffSize + kLzhLookahead - 1> text_buf_{};
    std::array<Int16, kLzhBuffSize + 1> lson_{};
    std::array<Int16, kLzhBuffSize + 257> rson_{};
    std::array<Int16, kLzhBuffSize + 1> dad_{};
};

// -----------------------------------------------------------------------------
// LZHRStream - LZSS + adaptive Huffman. Caller supplies decoded length.
// Both filters borrow their parent, which must outlive attachment.
// -----------------------------------------------------------------------------
class LZHRStream : public StreamIO {
public:
    LZHRStream();
    explicit LZHRStream(StreamIO& s);
    ~LZHRStream() override;

    LZHRStream(const LZHRStream&) = delete;
    LZHRStream& operator=(const LZHRStream&) = delete;

    void attach(StreamIO& s);
    void detach();
    void close() override;

    [[nodiscard]] Int32 getPosition() const override;
    Bool setPosition(Int32 pos) override;
    [[nodiscard]] Int32 getCapabilities(StreamCap cap) const override;
    Bool flush() override;

    using StreamIO::read;
    using StreamIO::write;

    Bool readBytes(std::span<std::byte> data) override;

private:
    void initialize();

    StreamIO* d_stream_{nullptr};
    std::unique_ptr<LZHuffman> d_huff_;
    std::array<UInt8, kLzhBuffSize + kLzhLookahead - 1> text_buf_{};
    int DI_{0};
    int DJ_{0};
    int DK_{0};
    int DR_{0};
    Int32 curPos_{0};
    Int32 origin_{0};
};

} // namespace opent1
