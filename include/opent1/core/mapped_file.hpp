#pragma once
#include <opent1/core/types.hpp>
#include <filesystem>
#include <memory>
#include <span>

namespace opent1 {
// Read-only file backing; independent handles/mappings survive atomic replacement.
// Mounted files must not be edited in place. Unsupported mappings can use buffered I/O.
class ReadOnlyMappedFile {
public:
    ReadOnlyMappedFile();
    ~ReadOnlyMappedFile();
    ReadOnlyMappedFile(ReadOnlyMappedFile&&) noexcept;
    ReadOnlyMappedFile& operator=(ReadOnlyMappedFile&&) noexcept;
    ReadOnlyMappedFile(const ReadOnlyMappedFile&) = delete;
    ReadOnlyMappedFile& operator=(const ReadOnlyMappedFile&) = delete;
    bool open(const std::filesystem::path&, std::size_t maximumBytes);
    [[nodiscard]] std::span<const std::byte> bytes() const noexcept;
    [[nodiscard]] bool unchanged() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
