#include <opent1/core/mapped_file.hpp>
#if defined(_WIN32)
#include <windows.h>
#elif defined(__unix__) || defined(__APPLE__)
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#if defined(__linux__)
#include <linux/magic.h>
#include <sys/vfs.h>
#endif
#endif

namespace opent1 {
struct ReadOnlyMappedFile::Impl {
    const std::byte* data{};
    std::size_t size{};
#if defined(_WIN32)
    HANDLE file{INVALID_HANDLE_VALUE}, section{};

    ~Impl() {
        if (data)
            UnmapViewOfFile(data);
        if (section)
            CloseHandle(section);
        if (file != INVALID_HANDLE_VALUE)
            CloseHandle(file);
    }
#elif defined(__unix__) || defined(__APPLE__)
    int file{-1};
    struct stat initial{};

    ~Impl() {
        if (data)
            munmap(const_cast<std::byte*>(data), size);
        if (file >= 0)
            ::close(file);
    }
#endif
};

ReadOnlyMappedFile::ReadOnlyMappedFile() = default;
ReadOnlyMappedFile::~ReadOnlyMappedFile() = default;
ReadOnlyMappedFile::ReadOnlyMappedFile(ReadOnlyMappedFile&&) noexcept = default;
ReadOnlyMappedFile& ReadOnlyMappedFile::operator=(ReadOnlyMappedFile&&) noexcept = default;

bool ReadOnlyMappedFile::open(const std::filesystem::path& path, std::size_t maximumBytes) {
    impl_.reset();
    if (path.empty() || path.native().find(typename std::filesystem::path::value_type{}) !=
                            std::filesystem::path::string_type::npos)
        return false;
    auto next = std::make_unique<Impl>();
#if defined(_WIN32)
    // Deny in-place writes while allowing the author's atomic replacement policy.
    next->file = CreateFileW(path.c_str(),
                             GENERIC_READ,
                             FILE_SHARE_READ | FILE_SHARE_DELETE,
                             nullptr,
                             OPEN_EXISTING,
                             FILE_ATTRIBUTE_NORMAL,
                             nullptr);
    if (next->file == INVALID_HANDLE_VALUE)
        return false;
    DWORD volumeFlags{};
    if (!GetVolumeInformationByHandleW(
            next->file, nullptr, 0, nullptr, nullptr, &volumeFlags, nullptr, 0) ||
        !(volumeFlags & FILE_SUPPORTS_POSIX_UNLINK_RENAME))
        return false;
    LARGE_INTEGER length{};
    if (!GetFileSizeEx(next->file, &length) || length.QuadPart <= 0 ||
        UInt64(length.QuadPart) > maximumBytes)
        return false;
    next->size = std::size_t(length.QuadPart);
    next->section = CreateFileMappingW(next->file, nullptr, PAGE_READONLY, 0, 0, nullptr);
    if (!next->section)
        return false;
    next->data =
        static_cast<const std::byte*>(MapViewOfFile(next->section, FILE_MAP_READ, 0, 0, 0));
    if (!next->data)
        return false;
#elif defined(__unix__) || defined(__APPLE__)
    next->file = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (next->file < 0 || fstat(next->file, &next->initial) != 0 || next->initial.st_size <= 0 ||
        UInt64(next->initial.st_size) > maximumBytes)
        return false;
    next->size = std::size_t(next->initial.st_size);
#if defined(__linux__) && defined(V9FS_MAGIC)
    // WSL/9P metadata can follow a replaced pathname instead of the old file
    // object. Retain a buffered snapshot on that filesystem.
    struct statfs filesystem{};
    if (fstatfs(next->file, &filesystem) != 0 || filesystem.f_type == V9FS_MAGIC)
        return false;
#endif
    const auto mapped = mmap(nullptr, next->size, PROT_READ, MAP_PRIVATE, next->file, 0);
    if (mapped == MAP_FAILED)
        return false;
    next->data = static_cast<const std::byte*>(mapped);
#else
    (void)maximumBytes;
    return false;
#endif
    impl_ = std::move(next);
    return true;
}

std::span<const std::byte> ReadOnlyMappedFile::bytes() const noexcept {
    return impl_ ? std::span<const std::byte>(impl_->data, impl_->size)
                 : std::span<const std::byte>{};
}

bool ReadOnlyMappedFile::unchanged() const noexcept {
    if (!impl_)
        return false;
#if defined(_WIN32)
    return true; // the open handle denies in-place writes
#elif defined(__unix__) || defined(__APPLE__)
    struct stat current{};
    if (fstat(impl_->file, &current) != 0 || current.st_size != impl_->initial.st_size)
        return false;
#if defined(__APPLE__)
    return current.st_mtimespec.tv_sec == impl_->initial.st_mtimespec.tv_sec &&
           current.st_mtimespec.tv_nsec == impl_->initial.st_mtimespec.tv_nsec;
#else
    return current.st_mtim.tv_sec == impl_->initial.st_mtim.tv_sec &&
           current.st_mtim.tv_nsec == impl_->initial.st_mtim.tv_nsec;
#endif
#else
    return false;
#endif
}
}
