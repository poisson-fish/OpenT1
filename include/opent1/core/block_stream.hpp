#pragma once
#include <opent1/core/stream_io.hpp>

namespace opent1 {
inline constexpr UInt32 kAlignDwordFlag = 0x80000000U;
inline constexpr Int32 ALIGN_DWORD = std::bit_cast<Int32>(kAlignDwordFlag);

// Non-owning filter; parent must outlive the block. Header is two LE 32-bit words.
// Size zero means an empty existing block, or an inferred size when creating.
class BlockStream : public StreamIO {
public:
    struct Header {
        Int32 id{0};
        UInt32 size{0};
    };

    BlockStream() {
        strm_status = STRM_CLOSED;
    }

    ~BlockStream() override;

    [[nodiscard]] Int32 getID() const noexcept {
        return header_.id;
    }

    [[nodiscard]] Int32 getDataSize() const noexcept {
        return static_cast<Int32>(header_.size & ~kAlignDwordFlag);
    }

    [[nodiscard]] Int32 getBlockSize() const;

    [[nodiscard]] Int32 getPosition() const override {
        return position_;
    }

    [[nodiscard]] Int32 getCapabilities(StreamCap cap) const override;
    Bool setPosition(Int32 position) override;
    Bool flush() override;
    Bool readBytes(std::span<std::byte> data) override;
    Bool writeBytes(std::span<const std::byte> data) override;
    [[nodiscard]] std::span<std::byte> lockBytes() override;
    void close() override;
    void undoOpen();

protected:
    Bool openExisting(StreamIO& parent, bool writable);
    Bool create(StreamIO& parent, Int32 id, Int32 size, bool dwordAlign);

private:
    Bool synchronize();
    Bool validExtent(UInt32 size) const;
    [[nodiscard]] UInt32 alignedSize() const noexcept;
    StreamIO* parent_{nullptr};
    Header header_;
    Int32 start_{0};
    Int32 position_{0};
    Int32 highWater_{0};
    bool writable_{false};
    bool created_{false};
    bool inferred_{false};
};

class BlockRWStream : public BlockStream {
public:
    Bool open(StreamIO& parent, Int32 id, Int32 size, Bool dwordAlign) {
        return create(parent, id, size, dwordAlign);
    }

    Bool open(StreamIO& parent) {
        return openExisting(parent, true);
    }

    Bool open(StreamIO& parent, Int32 id);
};

class BlockRStream : public BlockStream {
public:
    Bool open(StreamIO& parent) {
        return openExisting(parent, false);
    }

    Bool open(StreamIO& parent, Int32 id);
};
} // namespace opent1
