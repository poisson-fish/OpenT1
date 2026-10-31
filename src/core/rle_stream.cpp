#include <opent1/core/rle_stream.hpp>

namespace opent1 {
void RLERStream::attach(StreamIO& parent) {
    OPENT1_ASSERT_FATAL(&parent != this, "RLE filter cannot attach to itself");
    parent_ = &parent;
    origin_ = parent.getPosition();
    position_ = 0;
    remaining_ = 0;
    lastBytes = 0;
    strm_status = parent.getCapabilities(STRM_CAP_READ) ? parent.getStatus() : STRM_ILLEGAL_CALL;
}

void RLERStream::detach() {
    parent_ = nullptr;
    position_ = 0;
    remaining_ = 0;
    lastBytes = 0;
    strm_status = STRM_FLTR_DETACHED;
}

Int32 RLERStream::getCapabilities(StreamCap cap) const {
    switch (cap) {
        case STRM_CAP_TYPE:
            return STRM_RLE_R;
        case STRM_CAP_READ:
        case STRM_CAP_COMPRESS:
            return parent_ != nullptr;
        case STRM_CAP_REPOSITION:
            return parent_ && parent_->getCapabilities(cap);
        default:
            return 0;
    }
}

Bool RLERStream::readBytes(std::span<std::byte> data) {
    lastBytes = 0;
    if (!parent_) {
        return fail(STRM_FLTR_DETACHED);
    }
    if (strm_status != STRM_OK) {
        return false;
    }
    if (data.size() > static_cast<std::size_t>(std::numeric_limits<Int32>::max() - position_)) {
        return fail(STRM_ILLEGAL_CALL);
    }
    while (!data.empty()) {
        if (remaining_ == 0) {
            UInt8 marker{};
            if (!parent_->read(marker)) {
                return fail(parent_->getStatus());
            }
            remaining_ = marker & 127U;
            run_ = (marker & 128U) != 0;
            // Zero-length packets never occur in the encoder; rejecting them
            // prevents malformed input from spinning without producing output.
            if (remaining_ == 0) {
                return fail(STRM_IO_ERROR);
            }
            if (run_) {
                UInt8 value{};
                if (!parent_->read(value)) {
                    return fail(parent_->getStatus());
                }
                repeated_ = static_cast<std::byte>(value);
            }
        }
        const auto count = std::min(remaining_, data.size());
        auto output = data.first(count);
        UInt32 transferred = static_cast<UInt32>(count);
        bool ok = true;
        if (run_) {
            std::ranges::fill(output, repeated_);
        } else {
            ok = parent_->readBytes(output);
            transferred = parent_->getLastSize();
        }
        lastBytes += transferred;
        position_ += static_cast<Int32>(transferred);
        remaining_ -= transferred;
        if (!ok) {
            return fail(parent_->getStatus());
        }
        data = data.subspan(count);
    }
    return true;
}

Bool RLERStream::setPosition(Int32 position) {
    if (!parent_) {
        return fail(STRM_FLTR_DETACHED);
    }
    if (position < 0) {
        return fail(STRM_ILLEGAL_CALL);
    }
    if (position < position_ || strm_status == STRM_EOS) {
        if (!parent_->setPosition(origin_)) {
            return fail(parent_->getStatus());
        }
        remaining_ = 0;
        position_ = 0;
        clrStatus();
    }
    if (strm_status != STRM_OK) {
        return false;
    }
    std::array<std::byte, 512> discard{};
    while (position_ < position) {
        if (!readBytes(std::span(discard).first(
                std::min(discard.size(), static_cast<std::size_t>(position - position_))))) {
            return false;
        }
    }
    return true;
}

RLEWStream::~RLEWStream() {
    detach();
}

void RLEWStream::attach(StreamIO& parent) {
    OPENT1_ASSERT_FATAL(&parent != this, "RLE filter cannot attach to itself");
    detach();
    parent_ = &parent;
    position_ = 0;
    lastBytes = 0;
    length_ = runLength_ = 0;
    strm_status = parent.getCapabilities(STRM_CAP_WRITE) ? parent.getStatus() : STRM_ILLEGAL_CALL;
}

void RLEWStream::detach() {
    if (!parent_) {
        return;
    }
    const bool ok = strm_status == STRM_OK && flush();
    parent_ = nullptr;
    lastBytes = 0;
    if (ok) {
        strm_status = STRM_FLTR_DETACHED;
    }
}

Int32 RLEWStream::getCapabilities(StreamCap cap) const {
    switch (cap) {
        case STRM_CAP_TYPE:
            return STRM_RLE_W;
        case STRM_CAP_WRITE:
        case STRM_CAP_COMPRESS:
            return parent_ != nullptr;
        default:
            return 0;
    }
}

Bool RLEWStream::setPosition(Int32 position) {
    return parent_ && strm_status == STRM_OK && position == position_ ? true
                                                                      : fail(STRM_ILLEGAL_CALL);
}

Bool RLEWStream::emitLiterals() {
    if (length_ == 0) {
        return true;
    }
    if (!parent_->write(static_cast<UInt8>(length_)) ||
        !parent_->writeBytes(std::span(literals_).first(length_))) {
        return fail(parent_->getStatus());
    }
    length_ = 0;
    return true;
}

Bool RLEWStream::emitPending() {
    if (runLength_ == 0) {
        return true;
    }
    if (runLength_ < 3) {
        // Preserve the original encoder's 126-byte early flush rule.
        if (length_ >= 126 && !emitLiterals()) {
            return false;
        }
        std::ranges::fill(std::span(literals_).subspan(length_, runLength_), repeated_);
        length_ += runLength_;
    } else {
        if (!emitLiterals() || !parent_->write(static_cast<UInt8>(128U | runLength_)) ||
            !parent_->write(std::to_integer<UInt8>(repeated_))) {
            return fail(parent_->getStatus());
        }
    }
    runLength_ = 0;
    return true;
}

Bool RLEWStream::flush() {
    if (!parent_) {
        return fail(STRM_FLTR_DETACHED);
    }
    if (strm_status != STRM_OK) {
        return false;
    }
    return emitPending() && emitLiterals() && (parent_->flush() || fail(parent_->getStatus()));
}

Bool RLEWStream::writeBytes(std::span<const std::byte> data) {
    lastBytes = 0;
    if (!parent_) {
        return fail(STRM_FLTR_DETACHED);
    }
    if (strm_status != STRM_OK) {
        return false;
    }
    if (data.size() > static_cast<std::size_t>(std::numeric_limits<Int32>::max() - position_)) {
        return fail(STRM_ILLEGAL_CALL);
    }
    // Run state depends on the preceding byte and persists across calls.
    for (const auto byte : data) {
        if (runLength_ != 0 && (byte != repeated_ || runLength_ == 127) && !emitPending()) {
            return false;
        }
        repeated_ = byte;
        ++runLength_;
        ++position_;
        ++lastBytes;
    }
    return true;
}
} // namespace opent1
