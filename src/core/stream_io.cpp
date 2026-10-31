#include <opent1/core/stream_io.hpp>
#include <opent1/core/string_table.hpp>

namespace opent1 {
namespace {
std::string_view cText(std::string_view text) {
    return text.substr(0, text.find('\0'));
}
}

Int32 StreamIO::getCapabilities(StreamCap cap) const {
    return cap == STRM_CAP_TYPE ? STRM_BASE : 0;
}

Bool StreamIO::setPosition(Int32) {
    return fail(STRM_ILLEGAL_CALL);
}

Bool StreamIO::writeBytes(std::span<const std::byte>) {
    lastBytes = 0;
    return fail(STRM_ILLEGAL_CALL);
}

Bool StreamIO::readBytes(std::span<std::byte>) {
    lastBytes = 0;
    return fail(STRM_ILLEGAL_CALL);
}

Bool StreamIO::write(std::string_view text) {
    text = cText(text);
    OPENT1_ASSERT_FATAL(text.size() < static_cast<std::size_t>(std::numeric_limits<Int32>::max()),
                        "Stream text exceeds supported length");
    return write(static_cast<UInt32>(text.size() + 1)) &&
           writeBytes(std::as_bytes(std::span(text))) && write(UInt8{0});
}

Bool StreamIO::read(std::string& text, UInt32 maxLength) {
    UInt32 length{};
    if (!read(length)) {
        return false;
    }
    if (length == 0) {
        text.clear();
        return true;
    }
    if (length > maxLength || length > static_cast<UInt32>(std::numeric_limits<Int32>::max())) {
        return fail(STRM_ILLEGAL_CALL);
    }
    std::string temporary(length, '\0');
    if (!readBytes(std::as_writable_bytes(std::span(temporary)))) {
        return false;
    }
    if (temporary.back() != '\0') {
        return fail(STRM_IO_ERROR);
    }
    temporary.resize(temporary.find('\0'));
    text = std::move(temporary);
    return true;
}

Bool StreamIO::writeString(std::string_view text, std::size_t maxLength) {
    text = cText(text).substr(0, std::min(maxLength, std::size_t{255}));
    return write(static_cast<UInt8>(text.size())) && writeBytes(std::as_bytes(std::span(text)));
}

Bool StreamIO::readString(std::string& text) {
    return readCharString(text);
}

StringTableEntry StreamIO::readSTString(bool caseSensitive) {
    std::string text;
    return readString(text) ? globalStringTable().insert(text, caseSensitive) : nullptr;
}

Bool StreamIO::writeCharString(std::string_view text) {
    text = cText(text);
    OPENT1_ASSERT_FATAL(text.size() <= 255, "Pascal text exceeds 255 bytes");
    return write(static_cast<UInt8>(text.size())) && writeBytes(std::as_bytes(std::span(text)));
}

Bool StreamIO::readCharString(std::string& text) {
    UInt8 length{};
    if (!read(length)) {
        return false;
    }
    std::string temporary(length, '\0');
    if (!readBytes(std::as_writable_bytes(std::span(temporary)))) {
        return false;
    }
    temporary.resize(std::min(temporary.size(), temporary.find('\0')));
    text = std::move(temporary);
    return true;
}
} // namespace opent1
