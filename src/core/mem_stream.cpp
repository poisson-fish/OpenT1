#include <opent1/core/mem_stream.hpp>
#include <cstring>

namespace opent1 {
void MemRWStream::open(std::span<const std::byte> buffer) {
    (void)checkedNarrow<Int32>(buffer.size());
    readable_ = buffer;
    writable_ = {};
    position_ = 0;
    lastBytes = 0;
    readOnly_ = true;
    open_ = lockable_ = true;
    strm_status = STRM_OK;
}

void MemRWStream::open(std::span<std::byte> buffer) {
    open(std::span<const std::byte>(buffer));
    writable_ = buffer;
    readOnly_ = false;
}

void MemRWStream::close() {
    readable_ = {};
    writable_ = {};
    position_ = 0;
    lastBytes = 0;
    open_ = lockable_ = false;
    strm_status = STRM_CLOSED;
}

Int32 MemRWStream::getCapabilities(StreamCap cap) const {
    switch (cap) {
        case STRM_CAP_TYPE:
            return readOnly_ ? STRM_MEM_R : STRM_MEM_RW;
        case STRM_CAP_READ:
        case STRM_CAP_REPOSITION:
            return open_;
        case STRM_CAP_WRITE:
            return open_ && !readOnly_;
        case STRM_CAP_LOCKABLE:
            return open_ && lockable_ && !readOnly_;
        default:
            return 0;
    }
}

Bool MemRWStream::setPosition(Int32 position) {
    if (!open_) {
        return fail(STRM_CLOSED);
    }
    if (strm_status == STRM_EOS) {
        clrStatus();
    }
    if (strm_status != STRM_OK) {
        return false;
    }
    if (position < 0 || static_cast<std::size_t>(position) > readable_.size()) {
        return fail(static_cast<std::size_t>(position_) == readable_.size() ? STRM_EOS
                                                                            : STRM_IO_ERROR);
    }
    position_ = position;
    return true;
}

Bool MemRWStream::readBytes(std::span<std::byte> data) {
    lastBytes = 0;
    if (!open_) {
        return fail(STRM_CLOSED);
    }
    if (strm_status != STRM_OK) {
        return false;
    }
    const auto available = readable_.subspan(static_cast<std::size_t>(position_));
    const auto count = std::min(data.size(), available.size());
    if (count != 0) {
        std::memmove(data.data(), available.data(), count);
    }
    lastBytes = static_cast<UInt32>(count);
    position_ += static_cast<Int32>(count);
    return count == data.size() || fail(STRM_EOS);
}

Bool MemRWStream::writeBytes(std::span<const std::byte> data) {
    lastBytes = 0;
    if (!open_) {
        return fail(STRM_CLOSED);
    }
    if (strm_status != STRM_OK) {
        return false;
    }
    if (readOnly_) {
        return fail(STRM_ILLEGAL_CALL);
    }
    const auto available = writable_.subspan(static_cast<std::size_t>(position_));
    const auto count = std::min(data.size(), available.size());
    if (count != 0) {
        std::memmove(available.data(), data.data(), count);
    }
    lastBytes = static_cast<UInt32>(count);
    position_ += static_cast<Int32>(count);
    return count == data.size() || fail(STRM_IO_ERROR);
}

std::span<std::byte> MemRWStream::lockBytes() {
    return open_ && lockable_ && !readOnly_ && strm_status == STRM_OK
               ? writable_.subspan(static_cast<std::size_t>(position_))
               : std::span<std::byte>{};
}
} // namespace opent1
