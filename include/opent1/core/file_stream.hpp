#pragma once
#include <opent1/core/stream_io.hpp>
#include <cstdio>
#include <filesystem>
#include <memory>

namespace opent1 {
// Owns a binary C stdio stream. Native filesystem paths preserve Unicode on
// Windows. Offsets and file lengths above INT32_MAX are rejected, never wrapped.
class FileStream : public StreamIO {
public:
    ~FileStream() override = default;
    FileStream(const FileStream&) = delete;
    FileStream& operator=(const FileStream&) = delete;
    FileStream(FileStream&& other) noexcept;
    FileStream& operator=(FileStream&& other) noexcept;
    void close() override;
    [[nodiscard]] UInt32 getSize() const;

    [[nodiscard]] Int32 getPosition() const override {
        return position_;
    }

    Bool setPosition(Int32 position) override;
    [[nodiscard]] Int32 getCapabilities(StreamCap cap) const override;
    Bool flush() override;
    Bool readBytes(std::span<std::byte> data) override;
    Bool writeBytes(std::span<const std::byte> data) override;

protected:
    enum class Access { ReadWrite, Read, Write };

    explicit FileStream(Access access) : access_(access) {
        strm_status = STRM_CLOSED;
    }

    Bool openFile(const std::filesystem::path& path, Access access);

private:
    struct FileCloser {
        void operator()(std::FILE* file) const noexcept {
            std::fclose(file);
        }
    };

    std::unique_ptr<std::FILE, FileCloser> file_;
    Int32 position_{0};
    Access access_;
};

class FileRWStream : public FileStream {
public:
    FileRWStream() : FileStream(Access::ReadWrite) {}

    explicit FileRWStream(const std::filesystem::path& path) : FileRWStream() {
        open(path);
    }

    Bool open(const std::filesystem::path& path, bool readOnly = false) {
        return openFile(path, readOnly ? Access::Read : Access::ReadWrite);
    }
};

class FileRStream : public FileStream {
public:
    FileRStream() : FileStream(Access::Read) {}

    explicit FileRStream(const std::filesystem::path& path) : FileRStream() {
        open(path);
    }

    Bool open(const std::filesystem::path& path) {
        return openFile(path, Access::Read);
    }
};

class FileWStream : public FileStream {
public:
    FileWStream() : FileStream(Access::Write) {}

    explicit FileWStream(const std::filesystem::path& path) : FileWStream() {
        open(path);
    }

    Bool open(const std::filesystem::path& path) {
        return openFile(path, Access::Write);
    }
};
} // namespace opent1
