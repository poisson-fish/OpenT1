#pragma once
#include <opent1/core/stream_io.hpp>

namespace opent1 {
// Non-owning fixed storage. The caller must keep the entire buffer alive.
class MemRWStream : public StreamIO {
public:
    MemRWStream() {
        close();
    }

    explicit MemRWStream(std::span<std::byte> buffer) {
        open(buffer);
    }

    template <std::size_t Extent>
    explicit MemRWStream(std::span<std::byte, Extent> buffer)
        : MemRWStream(std::span<std::byte>(buffer)) {}

    explicit MemRWStream(std::span<const std::byte> buffer) {
        open(buffer);
    }

    void open(std::span<std::byte> buffer);

    template <std::size_t Extent> void open(std::span<std::byte, Extent> buffer) {
        open(std::span<std::byte>(buffer));
    }

    void open(std::span<const std::byte> buffer);
    void close() override;

    void setLockable(bool value) noexcept {
        lockable_ = value;
    }

    [[nodiscard]] Int32 getPosition() const override {
        return position_;
    }

    Bool setPosition(Int32 position) override;
    [[nodiscard]] Int32 getCapabilities(StreamCap cap) const override;
    Bool writeBytes(std::span<const std::byte> data) override;
    Bool readBytes(std::span<std::byte> data) override;
    [[nodiscard]] std::span<std::byte> lockBytes() override;

private:
    std::span<const std::byte> readable_;
    std::span<std::byte> writable_;
    Int32 position_{0};
    bool readOnly_{false};
    bool open_{false};
    bool lockable_{false};
};
} // namespace opent1
