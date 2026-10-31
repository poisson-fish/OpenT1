#include <opent1/core/lzh_stream.hpp>

#include <algorithm>

namespace opent1 {

// =============================================================================
// LZHuffman Static Tables (Exact parity with lzhuff.cpp)
// =============================================================================

const std::array<UInt8, 64> LZHuffman::p_len = {
    0x03U, 0x04U, 0x04U, 0x04U, 0x05U, 0x05U, 0x05U, 0x05U, 0x05U, 0x05U, 0x05U, 0x05U, 0x06U,
    0x06U, 0x06U, 0x06U, 0x06U, 0x06U, 0x06U, 0x06U, 0x06U, 0x06U, 0x06U, 0x06U, 0x07U, 0x07U,
    0x07U, 0x07U, 0x07U, 0x07U, 0x07U, 0x07U, 0x07U, 0x07U, 0x07U, 0x07U, 0x07U, 0x07U, 0x07U,
    0x07U, 0x07U, 0x07U, 0x07U, 0x07U, 0x07U, 0x07U, 0x07U, 0x07U, 0x08U, 0x08U, 0x08U, 0x08U,
    0x08U, 0x08U, 0x08U, 0x08U, 0x08U, 0x08U, 0x08U, 0x08U, 0x08U, 0x08U, 0x08U, 0x08U};

const std::array<UInt8, 64> LZHuffman::p_code = {
    0x00U, 0x20U, 0x30U, 0x40U, 0x50U, 0x58U, 0x60U, 0x68U, 0x70U, 0x78U, 0x80U, 0x88U, 0x90U,
    0x94U, 0x98U, 0x9CU, 0xA0U, 0xA4U, 0xA8U, 0xACU, 0xB0U, 0xB4U, 0xB8U, 0xBCU, 0xC0U, 0xC2U,
    0xC4U, 0xC6U, 0xC8U, 0xCAU, 0xCCU, 0xCEU, 0xD0U, 0xD2U, 0xD4U, 0xD6U, 0xD8U, 0xDAU, 0xDCU,
    0xDEU, 0xE0U, 0xE2U, 0xE4U, 0xE6U, 0xE8U, 0xEAU, 0xECU, 0xEEU, 0xF0U, 0xF1U, 0xF2U, 0xF3U,
    0xF4U, 0xF5U, 0xF6U, 0xF7U, 0xF8U, 0xF9U, 0xFAU, 0xFBU, 0xFCU, 0xFDU, 0xFEU, 0xFFU};

const std::array<UInt8, 256> LZHuffman::d_code = {
    0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U,
    0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U,
    0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x01U, 0x01U, 0x01U, 0x01U, 0x01U, 0x01U, 0x01U,
    0x01U, 0x01U, 0x01U, 0x01U, 0x01U, 0x01U, 0x01U, 0x01U, 0x01U, 0x02U, 0x02U, 0x02U, 0x02U,
    0x02U, 0x02U, 0x02U, 0x02U, 0x02U, 0x02U, 0x02U, 0x02U, 0x02U, 0x02U, 0x02U, 0x02U, 0x03U,
    0x03U, 0x03U, 0x03U, 0x03U, 0x03U, 0x03U, 0x03U, 0x03U, 0x03U, 0x03U, 0x03U, 0x03U, 0x03U,
    0x03U, 0x03U, 0x04U, 0x04U, 0x04U, 0x04U, 0x04U, 0x04U, 0x04U, 0x04U, 0x05U, 0x05U, 0x05U,
    0x05U, 0x05U, 0x05U, 0x05U, 0x05U, 0x06U, 0x06U, 0x06U, 0x06U, 0x06U, 0x06U, 0x06U, 0x06U,
    0x07U, 0x07U, 0x07U, 0x07U, 0x07U, 0x07U, 0x07U, 0x07U, 0x08U, 0x08U, 0x08U, 0x08U, 0x08U,
    0x08U, 0x08U, 0x08U, 0x09U, 0x09U, 0x09U, 0x09U, 0x09U, 0x09U, 0x09U, 0x09U, 0x0AU, 0x0AU,
    0x0AU, 0x0AU, 0x0AU, 0x0AU, 0x0AU, 0x0AU, 0x0BU, 0x0BU, 0x0BU, 0x0BU, 0x0BU, 0x0BU, 0x0BU,
    0x0BU, 0x0CU, 0x0CU, 0x0CU, 0x0CU, 0x0DU, 0x0DU, 0x0DU, 0x0DU, 0x0EU, 0x0EU, 0x0EU, 0x0EU,
    0x0FU, 0x0FU, 0x0FU, 0x0FU, 0x10U, 0x10U, 0x10U, 0x10U, 0x11U, 0x11U, 0x11U, 0x11U, 0x12U,
    0x12U, 0x12U, 0x12U, 0x13U, 0x13U, 0x13U, 0x13U, 0x14U, 0x14U, 0x14U, 0x14U, 0x15U, 0x15U,
    0x15U, 0x15U, 0x16U, 0x16U, 0x16U, 0x16U, 0x17U, 0x17U, 0x17U, 0x17U, 0x18U, 0x18U, 0x19U,
    0x19U, 0x1AU, 0x1AU, 0x1BU, 0x1BU, 0x1CU, 0x1CU, 0x1DU, 0x1DU, 0x1EU, 0x1EU, 0x1FU, 0x1FU,
    0x20U, 0x20U, 0x21U, 0x21U, 0x22U, 0x22U, 0x23U, 0x23U, 0x24U, 0x24U, 0x25U, 0x25U, 0x26U,
    0x26U, 0x27U, 0x27U, 0x28U, 0x28U, 0x29U, 0x29U, 0x2AU, 0x2AU, 0x2BU, 0x2BU, 0x2CU, 0x2CU,
    0x2DU, 0x2DU, 0x2EU, 0x2EU, 0x2FU, 0x2FU, 0x30U, 0x31U, 0x32U, 0x33U, 0x34U, 0x35U, 0x36U,
    0x37U, 0x38U, 0x39U, 0x3AU, 0x3BU, 0x3CU, 0x3DU, 0x3EU, 0x3FU,
};

const std::array<UInt8, 256> LZHuffman::d_len = {
    0x03U, 0x03U, 0x03U, 0x03U, 0x03U, 0x03U, 0x03U, 0x03U, 0x03U, 0x03U, 0x03U, 0x03U, 0x03U,
    0x03U, 0x03U, 0x03U, 0x03U, 0x03U, 0x03U, 0x03U, 0x03U, 0x03U, 0x03U, 0x03U, 0x03U, 0x03U,
    0x03U, 0x03U, 0x03U, 0x03U, 0x03U, 0x03U, 0x04U, 0x04U, 0x04U, 0x04U, 0x04U, 0x04U, 0x04U,
    0x04U, 0x04U, 0x04U, 0x04U, 0x04U, 0x04U, 0x04U, 0x04U, 0x04U, 0x04U, 0x04U, 0x04U, 0x04U,
    0x04U, 0x04U, 0x04U, 0x04U, 0x04U, 0x04U, 0x04U, 0x04U, 0x04U, 0x04U, 0x04U, 0x04U, 0x04U,
    0x04U, 0x04U, 0x04U, 0x04U, 0x04U, 0x04U, 0x04U, 0x04U, 0x04U, 0x04U, 0x04U, 0x04U, 0x04U,
    0x04U, 0x04U, 0x05U, 0x05U, 0x05U, 0x05U, 0x05U, 0x05U, 0x05U, 0x05U, 0x05U, 0x05U, 0x05U,
    0x05U, 0x05U, 0x05U, 0x05U, 0x05U, 0x05U, 0x05U, 0x05U, 0x05U, 0x05U, 0x05U, 0x05U, 0x05U,
    0x05U, 0x05U, 0x05U, 0x05U, 0x05U, 0x05U, 0x05U, 0x05U, 0x05U, 0x05U, 0x05U, 0x05U, 0x05U,
    0x05U, 0x05U, 0x05U, 0x05U, 0x05U, 0x05U, 0x05U, 0x05U, 0x05U, 0x05U, 0x05U, 0x05U, 0x05U,
    0x05U, 0x05U, 0x05U, 0x05U, 0x05U, 0x05U, 0x05U, 0x05U, 0x05U, 0x05U, 0x05U, 0x05U, 0x05U,
    0x05U, 0x06U, 0x06U, 0x06U, 0x06U, 0x06U, 0x06U, 0x06U, 0x06U, 0x06U, 0x06U, 0x06U, 0x06U,
    0x06U, 0x06U, 0x06U, 0x06U, 0x06U, 0x06U, 0x06U, 0x06U, 0x06U, 0x06U, 0x06U, 0x06U, 0x06U,
    0x06U, 0x06U, 0x06U, 0x06U, 0x06U, 0x06U, 0x06U, 0x06U, 0x06U, 0x06U, 0x06U, 0x06U, 0x06U,
    0x06U, 0x06U, 0x06U, 0x06U, 0x06U, 0x06U, 0x06U, 0x06U, 0x06U, 0x06U, 0x07U, 0x07U, 0x07U,
    0x07U, 0x07U, 0x07U, 0x07U, 0x07U, 0x07U, 0x07U, 0x07U, 0x07U, 0x07U, 0x07U, 0x07U, 0x07U,
    0x07U, 0x07U, 0x07U, 0x07U, 0x07U, 0x07U, 0x07U, 0x07U, 0x07U, 0x07U, 0x07U, 0x07U, 0x07U,
    0x07U, 0x07U, 0x07U, 0x07U, 0x07U, 0x07U, 0x07U, 0x07U, 0x07U, 0x07U, 0x07U, 0x07U, 0x07U,
    0x07U, 0x07U, 0x07U, 0x07U, 0x07U, 0x07U, 0x08U, 0x08U, 0x08U, 0x08U, 0x08U, 0x08U, 0x08U,
    0x08U, 0x08U, 0x08U, 0x08U, 0x08U, 0x08U, 0x08U, 0x08U, 0x08U,
};

// =============================================================================
// LZHuffman
// =============================================================================

LZHuffman::LZHuffman() noexcept = default;

void LZHuffman::attach(StreamIO& s) noexcept {
    d_stream_ = &s;
    d_getbuf_ = 0U;
    d_getlen_ = 0U;
    d_putbuf_ = 0U;
    d_putlen_ = 0U;

    std::ranges::for_each(std::views::iota(0, kLzhNChar), [&](int i) {
        freq_[static_cast<std::size_t>(i)] = 1U;
        son_[static_cast<std::size_t>(i)] = static_cast<Int16>(i + kLzhTableSize);
        prnt_[static_cast<std::size_t>(i + kLzhTableSize)] = static_cast<Int16>(i);
    });

    std::ranges::for_each(std::views::iota(kLzhNChar, kLzhRoot + 1), [&](int j) {
        const int i = (j - kLzhNChar) * 2;
        freq_[static_cast<std::size_t>(j)] = static_cast<UInt16>(
            freq_[static_cast<std::size_t>(i)] + freq_[static_cast<std::size_t>(i + 1)]);
        son_[static_cast<std::size_t>(j)] = static_cast<Int16>(i);
        prnt_[static_cast<std::size_t>(i)] = static_cast<Int16>(j);
        prnt_[static_cast<std::size_t>(i + 1)] = static_cast<Int16>(j);
    });

    freq_[static_cast<std::size_t>(kLzhTableSize)] = 0xFFFFU;
    prnt_[static_cast<std::size_t>(kLzhRoot)] = 0;
}

void LZHuffman::encodeChar(unsigned int c) {
    OPENT1_ASSERT_FATAL(d_stream_ && c < static_cast<unsigned>(kLzhNChar), "Invalid LZH symbol");
    UInt32 i = 0U;
    int j = 0;
    int k = prnt_[static_cast<std::size_t>(c + kLzhTableSize)];

    do {
        i >>= 1U;
        if ((k & 1) != 0) {
            i += 0x8000U;
        }
        ++j;
        k = prnt_[static_cast<std::size_t>(k)];
    } while (k != kLzhRoot);

    putCode(j, i);
    update(static_cast<int>(c));
}

void LZHuffman::encodePosition(unsigned int c) {
    OPENT1_ASSERT_FATAL(d_stream_ && c < static_cast<unsigned>(kLzhBuffSize),
                        "Invalid LZH position");
    const unsigned int i = c >> 6U;
    putCode(static_cast<int>(p_len[i]), static_cast<UInt32>(p_code[i]) << 8U);
    putCode(6, static_cast<UInt32>(c & 0x3FU) << 10U);
}

int LZHuffman::decodeChar() {
    OPENT1_ASSERT_FATAL(d_stream_, "LZH decoder is detached");
    unsigned int c = static_cast<unsigned int>(son_[static_cast<std::size_t>(kLzhRoot)]);
    while (c < static_cast<unsigned int>(kLzhTableSize)) {
        c += static_cast<unsigned int>(getBit());
        if (d_stream_->getStatus() != STRM_OK) {
            return -1;
        }
        c = static_cast<unsigned int>(son_[static_cast<std::size_t>(c)]);
    }
    c -= static_cast<unsigned int>(kLzhTableSize);
    update(static_cast<int>(c));
    return static_cast<int>(c);
}

int LZHuffman::decodePosition() {
    OPENT1_ASSERT_FATAL(d_stream_, "LZH decoder is detached");
    unsigned int i = static_cast<unsigned int>(getByte());
    if (d_stream_->getStatus() != STRM_OK) {
        return -1;
    }
    const unsigned int c = static_cast<unsigned int>(d_code[i]) << 6U;
    auto extraBits = static_cast<unsigned int>(d_len[i]) - 2U;
    while (extraBits-- != 0U) {
        i = (i << 1U) + static_cast<unsigned int>(getBit());
        if (d_stream_->getStatus() != STRM_OK) {
            return -1;
        }
    }

    return static_cast<int>(c | (i & 0x3FU));
}

void LZHuffman::flush() {
    if (d_stream_ == nullptr) {
        return;
    }
    if (d_putlen_ != 0U) {
        d_stream_->write(static_cast<UInt8>(d_putbuf_ >> 8U));
    }
    d_stream_->write(UInt8{0});
}

int LZHuffman::getBit() {
    while (d_getlen_ <= 8U) {
        UInt8 cc = 0U;
        if (d_stream_ == nullptr || !d_stream_->read(cc)) {
            return 0;
        }
        d_getbuf_ |= static_cast<UInt16>(static_cast<UInt16>(cc) << (8U - d_getlen_));
        d_getlen_ = static_cast<UInt8>(d_getlen_ + 8U);
    }

    const Int16 i = static_cast<Int16>(d_getbuf_);
    d_getbuf_ <<= 1U;
    --d_getlen_;
    return (i < 0) ? 1 : 0;
}

int LZHuffman::getByte() {
    while (d_getlen_ <= 8U) {
        UInt8 cc = 0U;
        if (d_stream_ == nullptr || !d_stream_->read(cc)) {
            return 0;
        }
        d_getbuf_ |= static_cast<UInt16>(static_cast<UInt16>(cc) << (8U - d_getlen_));
        d_getlen_ = static_cast<UInt8>(d_getlen_ + 8U);
    }

    const UInt16 i = d_getbuf_;
    d_getbuf_ <<= 8U;
    d_getlen_ = static_cast<UInt8>(d_getlen_ - 8U);
    return static_cast<int>(i >> 8U);
}

void LZHuffman::putCode(int l, unsigned int c) {
    d_putbuf_ |= static_cast<UInt16>(c >> d_putlen_);

    d_putlen_ = static_cast<UInt8>(d_putlen_ + static_cast<UInt8>(l));
    if (d_putlen_ >= 8U) {
        if (d_stream_ != nullptr) {
            d_stream_->write(static_cast<UInt8>(d_putbuf_ >> 8U));
        }

        d_putlen_ = static_cast<UInt8>(d_putlen_ - 8U);
        if (d_putlen_ >= 8U) {
            if (d_stream_ != nullptr) {
                d_stream_->write(static_cast<UInt8>(d_putbuf_ & 0xFFU));
            }
            d_putlen_ = static_cast<UInt8>(d_putlen_ - 8U);
            d_putbuf_ = static_cast<UInt16>(c << (l - static_cast<int>(d_putlen_)));
        } else {
            d_putbuf_ <<= 8U;
        }
    }
}

void LZHuffman::reconst() noexcept {
    int j = 0;
    std::ranges::for_each(std::views::iota(0, kLzhTableSize), [&](int i) {
        if (son_[static_cast<std::size_t>(i)] >= kLzhTableSize) {
            freq_[static_cast<std::size_t>(j)] =
                static_cast<UInt16>((freq_[static_cast<std::size_t>(i)] + 1U) / 2U);
            son_[static_cast<std::size_t>(j)] = son_[static_cast<std::size_t>(i)];
            ++j;
        }
    });

    std::ranges::for_each(std::views::iota(kLzhNChar, kLzhTableSize), [&](int node) {
        const int i = (node - kLzhNChar) * 2;
        int k = i + 1;
        const UInt16 f = static_cast<UInt16>(freq_[static_cast<std::size_t>(i)] +
                                             freq_[static_cast<std::size_t>(k)]);
        freq_[static_cast<std::size_t>(node)] = f;
        const auto insertion =
            std::ranges::find_if(std::views::iota(0, node) | std::views::reverse, [&](int index) {
                return f >= freq_[static_cast<std::size_t>(index)];
            });
        k = *insertion + 1;
        std::ranges::copy_backward(std::span(freq_).subspan(static_cast<std::size_t>(k),
                                                            static_cast<std::size_t>(node - k)),
                                   freq_.begin() + node + 1);
        freq_[static_cast<std::size_t>(k)] = f;
        std::ranges::copy_backward(std::span(son_).subspan(static_cast<std::size_t>(k),
                                                           static_cast<std::size_t>(node - k)),
                                   son_.begin() + node + 1);
        son_[static_cast<std::size_t>(k)] = static_cast<Int16>(i);
    });

    std::ranges::for_each(std::views::iota(0, kLzhTableSize), [&](int i) {
        const int k = son_[static_cast<std::size_t>(i)];
        if (k >= kLzhTableSize) {
            prnt_[static_cast<std::size_t>(k)] = static_cast<Int16>(i);
        } else {
            prnt_[static_cast<std::size_t>(k)] = static_cast<Int16>(i);
            prnt_[static_cast<std::size_t>(k + 1)] = static_cast<Int16>(i);
        }
    });
}

void LZHuffman::update(int c) noexcept {
    if (freq_[static_cast<std::size_t>(kLzhRoot)] == kLzhMaxFreq) {
        reconst();
    }

    c = prnt_[static_cast<std::size_t>(c + kLzhTableSize)];
    do {
        const UInt16 k = ++freq_[static_cast<std::size_t>(c)];
        int l = c + 1;
        if (k > freq_[static_cast<std::size_t>(l)]) {
            while (k > freq_[static_cast<std::size_t>(++l)]) {
            }
            --l;
            freq_[static_cast<std::size_t>(c)] = freq_[static_cast<std::size_t>(l)];
            freq_[static_cast<std::size_t>(l)] = k;

            const Int16 i = son_[static_cast<std::size_t>(c)];
            prnt_[static_cast<std::size_t>(i)] = static_cast<Int16>(l);
            if (i < kLzhTableSize) {
                prnt_[static_cast<std::size_t>(i + 1)] = static_cast<Int16>(l);
            }

            const Int16 j = son_[static_cast<std::size_t>(l)];
            son_[static_cast<std::size_t>(l)] = i;
            prnt_[static_cast<std::size_t>(j)] = static_cast<Int16>(c);
            if (j < kLzhTableSize) {
                prnt_[static_cast<std::size_t>(j + 1)] = static_cast<Int16>(c);
            }
            son_[static_cast<std::size_t>(c)] = j;

            c = l;
        }
        c = prnt_[static_cast<std::size_t>(c)];
    } while (c != 0);
}

// =============================================================================
// LZHWStream
// =============================================================================

LZHWStream::LZHWStream() {
    initialize();
    strm_status = STRM_FLTR_DETACHED;
}

LZHWStream::LZHWStream(StreamIO& s) {
    initialize();
    attach(s);
}

LZHWStream::~LZHWStream() {
    detach();
}

void LZHWStream::initialize() {
    d_stream_ = nullptr;
    d_huff_ = std::make_unique<LZHuffman>();
    text_buf_.fill(0U);
    lson_.fill(0);
    rson_.fill(0);
    dad_.fill(0);
}

void LZHWStream::attach(StreamIO& s) {
    OPENT1_ASSERT_FATAL(&s != this, "LZH filter cannot attach to itself");
    detach();
    d_stream_ = &s;
    strm_status = s.getCapabilities(STRM_CAP_WRITE) ? s.getStatus() : STRM_ILLEGAL_CALL;

    d_huff_->attach(s);
    text_buf_.fill(0U);

    std::ranges::fill(std::span(rson_).subspan(kLzhBuffSize + 1, 256), static_cast<Int16>(kLzhNil));
    std::ranges::fill(std::span(dad_).first(kLzhBuffSize), static_cast<Int16>(kLzhNil));

    ES_ = 0;
    ER_ = kLzhBuffSize - kLzhLookahead;

    std::ranges::fill(std::span(text_buf_).first(kLzhBuffSize - kLzhLookahead), UInt8{' '});

    EFULL_ = 0;
    ELEN_ = 0;
    ELM_ = 0;
    ELF_ = 0;
    curPos_ = 0;
    iFileSize_ = 0;
}

void LZHWStream::detach() {
    if (d_stream_ == nullptr) {
        return;
    }
    const bool ok = strm_status == STRM_OK && flush();
    d_stream_ = nullptr;
    if (ok) {
        strm_status = STRM_FLTR_DETACHED;
    }
}

void LZHWStream::close() {
    detach();
}

Int32 LZHWStream::getCapabilities(StreamCap cap) const {
    switch (cap) {
        case STRM_CAP_TYPE:
            return STRM_LZH_W;
        case STRM_CAP_WRITE:
        case STRM_CAP_COMPRESS:
            return d_stream_ != nullptr;
        default:
            return 0;
    }
}

void LZHWStream::insertNode(int r) noexcept {
    int cmp = 1;
    const UInt8* key = &text_buf_[static_cast<std::size_t>(r)];
    int p = kLzhBuffSize + 1 + static_cast<int>(key[0]);
    rson_[static_cast<std::size_t>(r)] = static_cast<Int16>(kLzhNil);
    lson_[static_cast<std::size_t>(r)] = static_cast<Int16>(kLzhNil);
    match_length_ = 0;

    for (;;) {
        if (cmp >= 0) {
            if (rson_[static_cast<std::size_t>(p)] != kLzhNil) {
                p = rson_[static_cast<std::size_t>(p)];
            } else {
                rson_[static_cast<std::size_t>(p)] = static_cast<Int16>(r);
                dad_[static_cast<std::size_t>(r)] = static_cast<Int16>(p);
                return;
            }
        } else {
            if (lson_[static_cast<std::size_t>(p)] != kLzhNil) {
                p = lson_[static_cast<std::size_t>(p)];
            } else {
                lson_[static_cast<std::size_t>(p)] = static_cast<Int16>(r);
                dad_[static_cast<std::size_t>(r)] = static_cast<Int16>(p);
                return;
            }
        }

        const auto mismatch = std::ranges::mismatch(
            std::span(key, kLzhLookahead).subspan(1),
            std::span(text_buf_).subspan(static_cast<std::size_t>(p + 1), kLzhLookahead - 1));
        const auto i = static_cast<int>(std::to_address(mismatch.in1) - key);
        if (i < kLzhLookahead) {
            cmp = static_cast<int>(key[i]) -
                  static_cast<int>(text_buf_[static_cast<std::size_t>(p + i)]);
        }

        if (i > kLzhThreshold) {
            if (i > match_length_) {
                match_position_ = ((r - p) & (kLzhBuffSize - 1)) - 1;
                match_length_ = i;
                if (match_length_ >= kLzhLookahead) {
                    break;
                }
            }
            if (i == match_length_) {
                const int c = ((r - p) & (kLzhBuffSize - 1)) - 1;
                if (c < match_position_) {
                    match_position_ = c;
                }
            }
        }
    }

    dad_[static_cast<std::size_t>(r)] = dad_[static_cast<std::size_t>(p)];
    lson_[static_cast<std::size_t>(r)] = lson_[static_cast<std::size_t>(p)];
    rson_[static_cast<std::size_t>(r)] = rson_[static_cast<std::size_t>(p)];
    dad_[static_cast<std::size_t>(lson_[static_cast<std::size_t>(p)])] = static_cast<Int16>(r);
    dad_[static_cast<std::size_t>(rson_[static_cast<std::size_t>(p)])] = static_cast<Int16>(r);
    if (rson_[static_cast<std::size_t>(dad_[static_cast<std::size_t>(p)])] == p) {
        rson_[static_cast<std::size_t>(dad_[static_cast<std::size_t>(p)])] = static_cast<Int16>(r);
    } else {
        lson_[static_cast<std::size_t>(dad_[static_cast<std::size_t>(p)])] = static_cast<Int16>(r);
    }
    dad_[static_cast<std::size_t>(p)] = static_cast<Int16>(kLzhNil);
}

void LZHWStream::deleteNode(int p) noexcept {
    if (dad_[static_cast<std::size_t>(p)] == kLzhNil) {
        return;
    }

    int q = 0;
    if (rson_[static_cast<std::size_t>(p)] == kLzhNil) {
        q = lson_[static_cast<std::size_t>(p)];
    } else if (lson_[static_cast<std::size_t>(p)] == kLzhNil) {
        q = rson_[static_cast<std::size_t>(p)];
    } else {
        q = lson_[static_cast<std::size_t>(p)];
        if (rson_[static_cast<std::size_t>(q)] != kLzhNil) {
            do {
                q = rson_[static_cast<std::size_t>(q)];
            } while (rson_[static_cast<std::size_t>(q)] != kLzhNil);
            rson_[static_cast<std::size_t>(dad_[static_cast<std::size_t>(q)])] =
                lson_[static_cast<std::size_t>(q)];
            dad_[static_cast<std::size_t>(lson_[static_cast<std::size_t>(q)])] =
                dad_[static_cast<std::size_t>(q)];
            lson_[static_cast<std::size_t>(q)] = lson_[static_cast<std::size_t>(p)];
            dad_[static_cast<std::size_t>(lson_[static_cast<std::size_t>(p)])] =
                static_cast<Int16>(q);
        }
        rson_[static_cast<std::size_t>(q)] = rson_[static_cast<std::size_t>(p)];
        dad_[static_cast<std::size_t>(rson_[static_cast<std::size_t>(p)])] = static_cast<Int16>(q);
    }

    dad_[static_cast<std::size_t>(q)] = dad_[static_cast<std::size_t>(p)];
    if (rson_[static_cast<std::size_t>(dad_[static_cast<std::size_t>(p)])] == p) {
        rson_[static_cast<std::size_t>(dad_[static_cast<std::size_t>(p)])] = static_cast<Int16>(q);
    } else {
        lson_[static_cast<std::size_t>(dad_[static_cast<std::size_t>(p)])] = static_cast<Int16>(q);
    }
    dad_[static_cast<std::size_t>(p)] = static_cast<Int16>(kLzhNil);
}

Bool LZHWStream::writeBytes(std::span<const std::byte> data) {
    lastBytes = 0;
    if (!d_stream_) {
        return fail(STRM_FLTR_DETACHED);
    }
    if (strm_status != STRM_OK) {
        return false;
    }
    if (data.size() > static_cast<std::size_t>(std::numeric_limits<Int32>::max() - iFileSize_)) {
        return fail(STRM_ILLEGAL_CALL);
    }
    int length = static_cast<int>(data.size());
    if (strm_status == STRM_FLTR_DETACHED || d_stream_ == nullptr) {
        return false;
    }
    if (length <= 0) {
        lastBytes = 0;
        return true;
    }

    const auto* pdata = reinterpret_cast<const UInt8*>(data.data());
    const int totalLength = length;

    if (!EFULL_) {
        while (ELEN_ < kLzhLookahead) {
            text_buf_[static_cast<std::size_t>(ER_ + ELEN_++)] = *pdata++;
            ++curPos_;
            if (--length == 0) {
                lastBytes = static_cast<UInt32>(totalLength);
                iFileSize_ += totalLength;
                return true;
            }
        }

        std::ranges::for_each(std::views::iota(1, kLzhLookahead + 1), [&](int i) {
            insertNode(ER_ - i);
        });
        insertNode(ER_);
        EFULL_ = 1;
    }

    while (true) {
        if (ELF_ >= ELM_) {
            if (match_length_ > ELEN_) {
                match_length_ = ELEN_;
            }

            if (match_length_ <= kLzhThreshold) {
                match_length_ = 1;
                d_huff_->encodeChar(text_buf_[static_cast<std::size_t>(ER_)]);
            } else {
                d_huff_->encodeChar(static_cast<unsigned int>(255 - kLzhThreshold + match_length_));
                d_huff_->encodePosition(static_cast<unsigned int>(match_position_));
            }

            if (d_stream_->getStatus() != STRM_OK) {
                lastBytes = static_cast<UInt32>(totalLength - length);
                iFileSize_ += static_cast<Int32>(lastBytes);
                return fail(d_stream_->getStatus());
            }
            ELF_ = 0;
            ELM_ = match_length_;
        }

        while (ELF_++ < ELM_) {
            const UInt8 cc = *pdata++;

            deleteNode(ES_);
            text_buf_[static_cast<std::size_t>(ES_)] = cc;

            if (ES_ < kLzhLookahead - 1) {
                text_buf_[static_cast<std::size_t>(ES_ + kLzhBuffSize)] = cc;
            }

            ES_ = (ES_ + 1) & (kLzhBuffSize - 1);
            ER_ = (ER_ + 1) & (kLzhBuffSize - 1);

            insertNode(ER_);

            ++curPos_;
            if (--length == 0) {
                lastBytes = static_cast<UInt32>(totalLength);
                iFileSize_ += totalLength;
                return true;
            }
        }
    }
}

Bool LZHWStream::flush() {
    if (strm_status != STRM_OK) {
        return false;
    }
    if (strm_status == STRM_FLTR_DETACHED || d_stream_ == nullptr) {
        return false;
    }

    if (!EFULL_) {
        std::ranges::for_each(std::views::iota(1, kLzhLookahead + 1), [&](int i) {
            insertNode(ER_ - i);
        });
        insertNode(ER_);
    }

    while (ELEN_ != 0) {
        if (ELF_ >= ELM_) {
            if (match_length_ > ELEN_) {
                match_length_ = ELEN_;
            }

            if (match_length_ <= kLzhThreshold) {
                match_length_ = 1;
                d_huff_->encodeChar(text_buf_[static_cast<std::size_t>(ER_)]);
            } else {
                d_huff_->encodeChar(static_cast<unsigned int>(255 - kLzhThreshold + match_length_));
                d_huff_->encodePosition(static_cast<unsigned int>(match_position_));
            }

            if (d_stream_->getStatus() != STRM_OK) {
                return fail(d_stream_->getStatus());
            }
            ELM_ = match_length_;
            ELF_ = 0;
        }

        while (ELF_++ < ELM_) {
            deleteNode(ES_);

            ES_ = (ES_ + 1) & (kLzhBuffSize - 1);
            ER_ = (ER_ + 1) & (kLzhBuffSize - 1);

            if (--ELEN_ != 0) {
                insertNode(ER_);
            }
        }
    }

    d_huff_->flush();
    if (d_stream_->getStatus() != STRM_OK) {
        return fail(d_stream_->getStatus());
    }

    d_stream_ = nullptr;
    strm_status = STRM_FLTR_DETACHED;
    return true;
}

Int32 LZHWStream::getPosition() const {
    return iFileSize_;
}

Bool LZHWStream::setPosition(Int32 pos) {
    return d_stream_ && strm_status == STRM_OK && pos == iFileSize_ ? true
                                                                    : fail(STRM_ILLEGAL_CALL);
}

// =============================================================================
// LZHRStream
// =============================================================================

LZHRStream::LZHRStream() {
    initialize();
    strm_status = STRM_FLTR_DETACHED;
}

LZHRStream::LZHRStream(StreamIO& s) {
    initialize();
    attach(s);
}

LZHRStream::~LZHRStream() {
    detach();
}

void LZHRStream::initialize() {
    d_stream_ = nullptr;
    d_huff_ = std::make_unique<LZHuffman>();
    text_buf_.fill(0U);
}

void LZHRStream::attach(StreamIO& s) {
    OPENT1_ASSERT_FATAL(&s != this, "LZH filter cannot attach to itself");
    detach();
    d_stream_ = &s;
    origin_ = s.getPosition();
    d_huff_->attach(s);
    text_buf_.fill(0U);

    std::ranges::fill(std::span(text_buf_).first(kLzhBuffSize - kLzhLookahead), UInt8{' '});

    DR_ = kLzhBuffSize - kLzhLookahead;
    DJ_ = 0;
    DK_ = 0;
    curPos_ = 0;
    strm_status = s.getCapabilities(STRM_CAP_READ) ? s.getStatus() : STRM_ILLEGAL_CALL;
}

void LZHRStream::detach() {
    if (d_stream_ == nullptr) {
        return;
    }
    const bool ok = strm_status == STRM_OK && flush();
    d_stream_ = nullptr;
    if (ok) {
        strm_status = STRM_FLTR_DETACHED;
    }
}

void LZHRStream::close() {
    detach();
}

Int32 LZHRStream::getCapabilities(StreamCap cap) const {
    switch (cap) {
        case STRM_CAP_TYPE:
            return STRM_LZH_R;
        case STRM_CAP_REPOSITION:
            return d_stream_ && d_stream_->getCapabilities(cap);
        case STRM_CAP_READ:
        case STRM_CAP_COMPRESS:
            return d_stream_ != nullptr;
        default:
            return 0;
    }
}

Bool LZHRStream::setPosition(Int32 pos) {
    if (!d_stream_) {
        return fail(STRM_FLTR_DETACHED);
    }
    if (pos < 0) {
        return fail(STRM_ILLEGAL_CALL);
    }
    if (pos < curPos_ || strm_status == STRM_EOS) {
        if (!d_stream_->setPosition(origin_)) {
            return fail(d_stream_->getStatus());
        }
        d_huff_->attach(*d_stream_);
        text_buf_.fill(0);
        std::ranges::fill(std::span(text_buf_).first(kLzhBuffSize - kLzhLookahead), UInt8{' '});
        DR_ = kLzhBuffSize - kLzhLookahead;
        DJ_ = DK_ = 0;
        curPos_ = 0;
        clrStatus();
    }
    if (strm_status != STRM_OK) {
        return false;
    }
    std::array<std::byte, 512> discard{};
    while (curPos_ < pos) {
        if (!readBytes(std::span(discard).first(
                std::min(discard.size(), static_cast<std::size_t>(pos - curPos_))))) {
            return false;
        }
    }
    return true;
}

Bool LZHRStream::readBytes(std::span<std::byte> data) {
    lastBytes = 0;
    if (!d_stream_) {
        return fail(STRM_FLTR_DETACHED);
    }
    if (strm_status != STRM_OK) {
        return false;
    }
    if (data.size() > static_cast<std::size_t>(std::numeric_limits<Int32>::max() - curPos_)) {
        return fail(STRM_ILLEGAL_CALL);
    }
    for (auto& output : data) {
        int value;
        if (DK_ < DJ_) {
            value = text_buf_[static_cast<std::size_t>((DI_ + DK_++) & (kLzhBuffSize - 1))];
        } else {
            value = d_huff_->decodeChar();
            if (value < 0) {
                return fail(d_stream_->getStatus());
            }
            if (value >= 256) {
                const auto position = d_huff_->decodePosition();
                if (position < 0) {
                    return fail(d_stream_->getStatus());
                }
                DI_ = (DR_ - position - 1) & (kLzhBuffSize - 1);
                DJ_ = value - 255 + kLzhThreshold;
                DK_ = 1;
                value = text_buf_[static_cast<std::size_t>(DI_)];
            }
        }
        output = static_cast<std::byte>(value);
        text_buf_[static_cast<std::size_t>(DR_)] = static_cast<UInt8>(value);
        DR_ = (DR_ + 1) & (kLzhBuffSize - 1);
        ++curPos_;
        ++lastBytes;
    }
    return true;
}

Bool LZHRStream::flush() {
    return d_stream_ && strm_status == STRM_OK;
}

Int32 LZHRStream::getPosition() const {
    return curPos_;
}
} // namespace opent1
