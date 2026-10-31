#include <opent1/core/block_stream.hpp>

namespace opent1 {
namespace {
constexpr UInt32 headerSize = 8;
constexpr UInt32 maxPosition = static_cast<UInt32>(std::numeric_limits<Int32>::max());
}

BlockStream::~BlockStream() {
    close();
}

UInt32 BlockStream::alignedSize() const noexcept {
    const UInt32 mask = (header_.size & kAlignDwordFlag) != 0 ? 3U : 1U;
    return ((header_.size & ~kAlignDwordFlag) + mask) & ~mask;
}

Int32 BlockStream::getBlockSize() const {
    return checkedNarrow<Int32>(UInt64(alignedSize()) + headerSize);
}

Bool BlockStream::validExtent(UInt32 size) const {
    const UInt64 mask = (header_.size & kAlignDwordFlag) != 0 ? 3U : 1U;
    const UInt64 aligned = (UInt64(size) + mask) & ~mask;
    return start_ >= 0 && UInt64(start_) + headerSize + aligned <= maxPosition;
}

Int32 BlockStream::getCapabilities(StreamCap cap) const {
    if (!parent_) {
        return 0;
    }
    if (cap == STRM_CAP_WRITE) {
        return writable_ && parent_->getCapabilities(cap);
    }
    if (cap == STRM_CAP_LOCKABLE) {
        return writable_ && parent_->getCapabilities(cap);
    }
    return parent_->getCapabilities(cap);
}

Bool BlockStream::synchronize() {
    if (!parent_) {
        return fail(STRM_CLOSED);
    }
    const Int32 absolute = start_ + static_cast<Int32>(headerSize) + position_;
    if (parent_->getPosition() != absolute && !parent_->setPosition(absolute)) {
        return fail(STRM_IO_ERROR);
    }
    return true;
}

Bool BlockStream::setPosition(Int32 position) {
    if (!parent_) {
        return fail(STRM_CLOSED);
    }
    if (strm_status == STRM_EOS) {
        clrStatus();
    }
    if (strm_status != STRM_OK) {
        return false;
    }
    if (position < 0 || (!inferred_ && position > getDataSize()) ||
        (inferred_ && static_cast<UInt32>(position) >
                          maxPosition - static_cast<UInt32>(start_) - headerSize - 3U)) {
        return fail(STRM_ILLEGAL_CALL);
    }
    const auto previous = position_;
    position_ = position;
    if (!synchronize()) {
        position_ = previous;
        return false;
    }
    return true;
}

Bool BlockStream::flush() {
    if (!parent_) {
        return fail(STRM_CLOSED);
    }
    return parent_->flush() || fail(STRM_IO_ERROR);
}

Bool BlockStream::readBytes(std::span<std::byte> data) {
    lastBytes = 0;
    if (!parent_) {
        return fail(STRM_CLOSED);
    }
    if (strm_status != STRM_OK) {
        return false;
    }
    if (!synchronize()) {
        return false;
    }
    const auto remaining =
        static_cast<std::size_t>(std::max(0, (inferred_ ? highWater_ : getDataSize()) - position_));
    const auto count = std::min(data.size(), remaining);
    const bool ok = parent_->readBytes(data.first(count));
    lastBytes = parent_->getLastSize();
    position_ += static_cast<Int32>(lastBytes);
    if (!ok) {
        return fail(parent_->getStatus());
    }
    return count == data.size() || fail(STRM_EOS);
}

Bool BlockStream::writeBytes(std::span<const std::byte> data) {
    lastBytes = 0;
    if (!parent_) {
        return fail(STRM_CLOSED);
    }
    if (strm_status != STRM_OK) {
        return false;
    }
    if (!writable_) {
        return fail(STRM_ILLEGAL_CALL);
    }
    const auto limit = inferred_ ? maxPosition - static_cast<UInt32>(start_) - headerSize - 3U
                                 : static_cast<UInt32>(getDataSize());
    const auto remaining = static_cast<std::size_t>(limit - static_cast<UInt32>(position_));
    const auto count = std::min(data.size(), remaining);
    if (!synchronize()) {
        return false;
    }
    const bool ok = parent_->writeBytes(data.first(count));
    lastBytes = parent_->getLastSize();
    position_ += static_cast<Int32>(lastBytes);
    highWater_ = std::max(highWater_, position_);
    if (!ok) {
        return fail(parent_->getStatus());
    }
    return count == data.size() || fail(STRM_EOS);
}

std::span<std::byte> BlockStream::lockBytes() {
    if (!parent_ || !writable_ || strm_status != STRM_OK || !synchronize()) {
        return {};
    }
    // An inferred block has no declared extent; writes must track its size.
    if (inferred_) {
        return {};
    }
    auto bytes = parent_->lockBytes();
    if (created_ && !bytes.empty()) {
        highWater_ = getDataSize();
    }
    return bytes.first(std::min(bytes.size(), static_cast<std::size_t>(getDataSize() - position_)));
}

Bool BlockStream::create(StreamIO& parent, Int32 id, Int32 size, bool dwordAlign) {
    OPENT1_ASSERT_FATAL(&parent != this, "Block filter cannot attach to itself");
    close();
    if (size < 0 || parent.getStatus() != STRM_OK || !parent.getCapabilities(STRM_CAP_WRITE)) {
        return fail(STRM_ILLEGAL_CALL);
    }
    start_ = parent.getPosition();
    header_ = {id, static_cast<UInt32>(size) | (dwordAlign ? kAlignDwordFlag : 0U)};
    if (!validExtent(static_cast<UInt32>(size)) ||
        (size == 0 && (!parent.getCapabilities(STRM_CAP_REPOSITION) || !validExtent(3)))) {
        return fail(STRM_ILLEGAL_CALL);
    }
    if (!parent.write(header_.id) || !parent.write(header_.size)) {
        return fail(parent.getStatus());
    }
    parent_ = &parent;
    position_ = highWater_ = 0;
    writable_ = created_ = true;
    inferred_ = size == 0;
    lastBytes = 0;
    clrStatus();
    return true;
}

Bool BlockStream::openExisting(StreamIO& parent, bool writable) {
    OPENT1_ASSERT_FATAL(&parent != this, "Block filter cannot attach to itself");
    close();
    if (parent.getStatus() != STRM_OK || !parent.getCapabilities(STRM_CAP_READ) ||
        (writable && !parent.getCapabilities(STRM_CAP_WRITE))) {
        return fail(STRM_ILLEGAL_CALL);
    }
    start_ = parent.getPosition();
    if (!parent.read(header_.id) || !parent.read(header_.size)) {
        return fail(parent.getStatus());
    }
    if (!validExtent(static_cast<UInt32>(getDataSize()))) {
        return fail(STRM_ILLEGAL_CALL);
    }
    parent_ = &parent;
    position_ = 0;
    highWater_ = getDataSize();
    writable_ = writable;
    created_ = inferred_ = false;
    lastBytes = 0;
    clrStatus();
    return true;
}

void BlockStream::close() {
    if (!parent_) {
        return;
    }
    bool ok = true;
    if (inferred_) {
        header_.size |= static_cast<UInt32>(highWater_);
        ok = parent_->setPosition(start_) && parent_->write(header_.id) &&
             parent_->write(header_.size);
    }
    const Int32 end = start_ + static_cast<Int32>(headerSize) + static_cast<Int32>(alignedSize());
    if (ok && created_) {
        // Materialize unwritten data and alignment, including at physical EOF.
        position_ = highWater_;
        ok = synchronize();
        std::array<std::byte, 256> zeroes{};
        Int32 absolute = start_ + static_cast<Int32>(headerSize) + position_;
        while (ok && absolute < end) {
            const auto count = std::min(zeroes.size(), static_cast<std::size_t>(end - absolute));
            ok = parent_->writeBytes(std::span(zeroes).first(count));
            absolute += static_cast<Int32>(parent_->getLastSize());
        }
    } else if (ok && parent_->getPosition() != end) {
        if (parent_->getCapabilities(STRM_CAP_REPOSITION)) {
            ok = parent_->setPosition(end);
        } else {
            ok = synchronize();
            std::array<std::byte, 256> discard{};
            Int32 remaining = end - (start_ + static_cast<Int32>(headerSize) + position_);
            while (ok && remaining > 0) {
                const auto count = std::min(discard.size(), static_cast<std::size_t>(remaining));
                ok = parent_->readBytes(std::span(discard).first(count));
                remaining -= static_cast<Int32>(parent_->getLastSize());
            }
        }
    }
    parent_ = nullptr;
    position_ = 0;
    lastBytes = 0;
    strm_status = ok ? STRM_CLOSED : STRM_IO_ERROR;
}

void BlockStream::undoOpen() {
    if (!parent_) {
        return;
    }
    const bool ok = parent_->setPosition(start_);
    parent_ = nullptr;
    position_ = 0;
    lastBytes = 0;
    strm_status = ok ? STRM_CLOSED : STRM_IO_ERROR;
}

Bool BlockRWStream::open(StreamIO& parent, Int32 id) {
    if (!open(parent)) {
        return false;
    }
    if (getID() == id) {
        return true;
    }
    close();
    return false;
}

Bool BlockRStream::open(StreamIO& parent, Int32 id) {
    if (!open(parent)) {
        return false;
    }
    if (getID() == id) {
        return true;
    }
    close();
    return false;
}
} // namespace opent1
