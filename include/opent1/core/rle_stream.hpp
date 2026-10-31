#pragma once
#include <opent1/core/stream_io.hpp>

namespace opent1 {
// Non-owning filters. Parent must outlive attachment. RLE has no end marker:
// readers must be bounded by their parent and the caller's decoded length.
class RLERStream : public StreamIO {
public:
    RLERStream() {
        strm_status = STRM_FLTR_DETACHED;
    }

    explicit RLERStream(StreamIO& parent) {
        attach(parent);
    }

    void attach(StreamIO& parent);
    void detach();

    void close() override {
        detach();
    }

    [[nodiscard]] Int32 getPosition() const override {
        return position_;
    }

    Bool setPosition(Int32 position) override;
    [[nodiscard]] Int32 getCapabilities(StreamCap cap) const override;
    Bool readBytes(std::span<std::byte> data) override;

private:
    StreamIO* parent_{nullptr};
    Int32 origin_{0}, position_{0};
    std::size_t remaining_{0};
    std::byte repeated_{};
    bool run_{false};
};

class RLEWStream : public StreamIO {
public:
    RLEWStream() {
        strm_status = STRM_FLTR_DETACHED;
    }

    explicit RLEWStream(StreamIO& parent) {
        attach(parent);
    }

    ~RLEWStream() override;
    void attach(StreamIO& parent);
    void detach();

    void close() override {
        detach();
    }

    [[nodiscard]] Int32 getPosition() const override {
        return position_;
    }

    Bool setPosition(Int32 position) override;
    [[nodiscard]] Int32 getCapabilities(StreamCap cap) const override;
    Bool flush() override;
    Bool writeBytes(std::span<const std::byte> data) override;

private:
    Bool emitLiterals();
    Bool emitPending();
    StreamIO* parent_{nullptr};
    Int32 position_{0};
    std::array<std::byte, 127> literals_{};
    std::size_t length_{0}, runLength_{0};
    std::byte repeated_{};
};
} // namespace opent1
