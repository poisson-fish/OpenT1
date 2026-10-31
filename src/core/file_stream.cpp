#include <opent1/core/file_stream.hpp>
#include <cerrno>
#include <utility>

namespace opent1 {
namespace {
std::FILE* openNative(const std::filesystem::path& path, const char* mode) {
#ifdef _WIN32
    const std::wstring wideMode(mode, mode + std::char_traits<char>::length(mode));
    return _wfopen(path.c_str(), wideMode.c_str());
#else
    return std::fopen(path.c_str(), mode);
#endif
}

constexpr auto maxPosition = std::numeric_limits<Int32>::max();
}

FileStream::FileStream(FileStream&& other) noexcept
    : StreamIO(std::move(other)), file_(std::move(other.file_)),
      position_(std::exchange(other.position_, 0)), access_(other.access_) {
    other.strm_status = STRM_CLOSED;
    other.lastBytes = 0;
}

FileStream& FileStream::operator=(FileStream&& other) noexcept {
    if (this != &other) {
        file_ = std::move(other.file_);
        position_ = std::exchange(other.position_, 0);
        access_ = other.access_;
        strm_status = std::exchange(other.strm_status, STRM_CLOSED);
        lastBytes = std::exchange(other.lastBytes, 0);
    }
    return *this;
}

Bool FileStream::openFile(const std::filesystem::path& path, Access access) {
    close();
    access_ = access;
    // Native APIs treat embedded NUL as an early terminator; reject it explicitly.
    if (path.empty() || path.native().find(typename std::filesystem::path::value_type{}) !=
                            std::filesystem::path::string_type::npos) {
        return fail(STRM_ILLEGAL_CALL);
    }
    const char* mode = access == Access::Read ? "rb" : access == Access::Write ? "wb" : "r+b";
    file_.reset(openNative(path, mode));
    if (!file_ && access == Access::ReadWrite && errno == ENOENT) {
        file_.reset(openNative(path, "w+b"));
    }
    if (!file_) {
        return fail(STRM_IO_ERROR);
    }
    if (std::fseek(file_.get(), 0, SEEK_END) != 0) {
        file_.reset();
        return fail(STRM_IO_ERROR);
    }
    const auto length = std::ftell(file_.get());
    if (length < 0 || static_cast<UInt64>(length) > static_cast<UInt64>(maxPosition) ||
        std::fseek(file_.get(), 0, SEEK_SET) != 0) {
        file_.reset();
        return fail(STRM_IO_ERROR);
    }
    clrStatus();
    return true;
}

void FileStream::close() {
    const bool ok = !file_ || std::fclose(file_.release()) == 0;
    position_ = 0;
    lastBytes = 0;
    strm_status = ok ? STRM_CLOSED : STRM_IO_ERROR;
}

UInt32 FileStream::getSize() const {
    if (!file_ || (strm_status != STRM_OK && strm_status != STRM_EOS)) {
        return 0;
    }
    if (std::fseek(file_.get(), 0, SEEK_END) != 0) {
        return 0;
    }
    const auto length = std::ftell(file_.get());
    const bool restored = std::fseek(file_.get(), position_, SEEK_SET) == 0;
    return restored && length >= 0 &&
                   static_cast<UInt64>(length) <= static_cast<UInt64>(maxPosition)
               ? static_cast<UInt32>(length)
               : 0;
}

Bool FileStream::setPosition(Int32 position) {
    if (!file_) {
        return fail(STRM_CLOSED);
    }
    if (strm_status == STRM_EOS) {
        clrStatus();
    }
    if (strm_status != STRM_OK) {
        return false;
    }
    if (position < 0) {
        return fail(STRM_ILLEGAL_CALL);
    }
    if (std::fseek(file_.get(), position, SEEK_SET) != 0) {
        return fail(STRM_IO_ERROR);
    }
    position_ = position;
    return true;
}

Int32 FileStream::getCapabilities(StreamCap cap) const {
    switch (cap) {
        case STRM_CAP_TYPE:
            return access_ == Access::Read    ? STRM_FILE_R
                   : access_ == Access::Write ? STRM_FILE_W
                                              : STRM_FILE_RW;
        case STRM_CAP_READ:
            return file_ && access_ != Access::Write;
        case STRM_CAP_WRITE:
            return file_ && access_ != Access::Read;
        case STRM_CAP_REPOSITION:
            return file_ != nullptr;
        default:
            return 0;
    }
}

Bool FileStream::flush() {
    if (!file_) {
        return fail(STRM_CLOSED);
    }
    if (strm_status != STRM_OK && strm_status != STRM_EOS) {
        return false;
    }
    return access_ == Access::Read || std::fflush(file_.get()) == 0 || fail(STRM_IO_ERROR);
}

Bool FileStream::readBytes(std::span<std::byte> data) {
    lastBytes = 0;
    if (!file_) {
        return fail(STRM_CLOSED);
    }
    if (strm_status != STRM_OK) {
        return false;
    }
    if (access_ == Access::Write) {
        return fail(STRM_ILLEGAL_CALL);
    }
    if (data.size() > static_cast<std::size_t>(maxPosition - position_)) {
        return fail(STRM_ILLEGAL_CALL);
    }
    if (data.empty()) {
        return true;
    }
    if (std::fseek(file_.get(), position_, SEEK_SET) != 0) {
        return fail(STRM_IO_ERROR);
    }
    lastBytes = static_cast<UInt32>(std::fread(data.data(), 1, data.size(), file_.get()));
    position_ += static_cast<Int32>(lastBytes);
    if (lastBytes != data.size()) {
        return fail(std::ferror(file_.get()) ? STRM_IO_ERROR : STRM_EOS);
    }
    return true;
}

Bool FileStream::writeBytes(std::span<const std::byte> data) {
    lastBytes = 0;
    if (!file_) {
        return fail(STRM_CLOSED);
    }
    if (strm_status != STRM_OK) {
        return false;
    }
    if (access_ == Access::Read) {
        return fail(STRM_ILLEGAL_CALL);
    }
    if (data.size() > static_cast<std::size_t>(maxPosition - position_)) {
        return fail(STRM_ILLEGAL_CALL);
    }
    if (data.empty()) {
        return true;
    }
    if (std::fseek(file_.get(), position_, SEEK_SET) != 0) {
        return fail(STRM_IO_ERROR);
    }
    lastBytes = static_cast<UInt32>(std::fwrite(data.data(), 1, data.size(), file_.get()));
    position_ += static_cast<Int32>(lastBytes);
    return lastBytes == data.size() || fail(STRM_IO_ERROR);
}
} // namespace opent1
