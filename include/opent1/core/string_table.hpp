#pragma once

#include <opent1/core/types.hpp>

#include <cstddef>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

namespace opent1 {

// Monotonic storage aligned for any non-overaligned type. Allocations survive
// growth and moves; freeBlocks(), move assignment, or destruction invalidate them.
class DataChunker {
public:
    static constexpr std::size_t kDefaultChunkSize = 16376U;
    explicit DataChunker(std::size_t chunkSize = kDefaultChunkSize);
    ~DataChunker() = default;
    DataChunker(const DataChunker&) = delete;
    DataChunker& operator=(const DataChunker&) = delete;
    DataChunker(DataChunker&&) noexcept = default;
    DataChunker& operator=(DataChunker&&) noexcept = default;

    // Requires 0 < size <= chunkSize. Returned span has exactly size bytes.
    [[nodiscard]] std::span<std::byte> alloc(std::size_t size);
    void freeBlocks() noexcept;

private:
    struct DataBlock {
        std::unique_ptr<std::byte[]> data;
        std::size_t used = 0;
    };

    std::vector<DataBlock> blocks_;
    std::size_t chunkSize_;
};

// Legacy C-locale folding (ASCII A-Z only), including signed 8-bit hash terms.
// Bounded text inputs stop at their first NUL, just like legacy C strings.
[[nodiscard]] UInt32 hashString(std::string_view text) noexcept;

[[nodiscard]] inline UInt32 HashString(std::string_view text) noexcept {
    return hashString(text);
}

// Entries are immutable, NUL-terminated, and valid until table destruction.
// Case-insensitive queries return the first inserted spelling, even after resize.
// Concurrent readers are permitted; mutation requires external synchronization.
class StringTable {
public:
    static constexpr std::size_t kInitialBucketCount = 29U;
    static constexpr std::size_t kMaxStringLength = DataChunker::kDefaultChunkSize - 1U;
    StringTable();
    ~StringTable() = default;
    StringTable(const StringTable&) = delete;
    StringTable& operator=(const StringTable&) = delete;

    StringTableEntry insert(std::string_view text, bool caseSens = false);
    [[nodiscard]] StringTableEntry lookup(std::string_view text,
                                          bool caseSens = false) const noexcept;
    void resize(std::size_t newSize);

    [[nodiscard]] std::size_t numBuckets() const noexcept {
        return buckets_.size();
    }

    [[nodiscard]] std::size_t itemCount() const noexcept {
        return itemCount_;
    }

private:
    struct Node {
        std::string_view text;
        Node* next;
    };

    std::vector<Node*> buckets_;
    std::size_t itemCount_ = 0;
    DataChunker mempool_;
};

// Lazy construction can allocate and therefore can throw.
[[nodiscard]] StringTable& globalStringTable();
} // namespace opent1
