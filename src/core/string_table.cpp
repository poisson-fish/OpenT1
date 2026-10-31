#include <opent1/core/string_table.hpp>
#include <opent1/core/base.hpp>

#include <algorithm>
#include <bit>
#include <limits>
#include <numeric>

namespace opent1 {
namespace {
std::string_view textPrefix(std::string_view text) noexcept {
    return text.substr(0, text.find('\0'));
}

UInt8 fold(char c) noexcept {
    const auto byte = static_cast<UInt8>(c);
    return byte >= 'A' && byte <= 'Z' ? static_cast<UInt8>(byte + ('a' - 'A')) : byte;
}

bool equalText(std::string_view a, std::string_view b, bool caseSens) noexcept {
    return caseSens ? a == b : std::ranges::equal(a, b, [](char x, char y) {
        return fold(x) == fold(y);
    });
}
}

DataChunker::DataChunker(std::size_t chunkSize) : chunkSize_(chunkSize) {
    OPENT1_ASSERT_FATAL(chunkSize > 0 && chunkSize <= std::numeric_limits<std::size_t>::max() -
                                                          (sizeof(std::max_align_t) - 1U),
                        "Invalid arena chunk size");
}

std::span<std::byte> DataChunker::alloc(std::size_t size) {
    OPENT1_ASSERT_FATAL(size > 0 && size <= chunkSize_, "Invalid arena allocation size");
    constexpr auto alignment = alignof(std::max_align_t);
    const auto start =
        blocks_.empty() ? 0U : (blocks_.back().used + alignment - 1U) / alignment * alignment;
    if (blocks_.empty() || start > chunkSize_ || size > chunkSize_ - start) {
        // A byte array can provide storage for placement-created Nodes. Round
        // its allocation up to sizeof(max_align_t) so new[] guarantees alignment
        // for every type supported by this arena, even for tiny chunks.
        const auto bytes = (chunkSize_ + sizeof(std::max_align_t) - 1U) / sizeof(std::max_align_t) *
                           sizeof(std::max_align_t);
        blocks_.push_back({std::make_unique<std::byte[]>(bytes), 0});
    }
    auto& block = blocks_.back();
    const auto offset = (block.used + alignment - 1U) / alignment * alignment;
    auto result = std::span(block.data.get() + offset, size);
    block.used = offset + size;
    return result;
}

void DataChunker::freeBlocks() noexcept {
    blocks_.clear();
}

UInt32 hashString(std::string_view text) noexcept {
    text = textPrefix(text);
    return std::accumulate(text.begin(), text.end(), UInt32{0}, [](UInt32 hash, char c) {
        const auto value = static_cast<Int32>(std::bit_cast<Int8>(fold(c)));
        return (hash << 1U) ^ static_cast<UInt32>(value * value);
    });
}

StringTable::StringTable() : buckets_(kInitialBucketCount, nullptr) {}

StringTableEntry StringTable::insert(std::string_view text, bool caseSens) {
    text = textPrefix(text);
    OPENT1_ASSERT_FATAL(text.size() <= kMaxStringLength,
                        "Interned string exceeds arena chunk size");
    if (const auto existing = lookup(text, caseSens)) {
        return existing;
    }
    // Allocate the new bucket array before publishing an entry or changing links.
    if (itemCount_ / 2U >= buckets_.size()) {
        OPENT1_ASSERT_FATAL(buckets_.size() <= buckets_.max_size() / 4U,
                            "String table growth overflow");
        resize(4U * buckets_.size() - 1U);
    }
    auto storage = mempool_.alloc(text.size() + 1U);
    auto* value = reinterpret_cast<char*>(storage.data());
    std::ranges::copy(text, value);
    value[text.size()] = '\0';
    auto* node = std::construct_at(reinterpret_cast<Node*>(mempool_.alloc(sizeof(Node)).data()),
                                   Node{{value, text.size()}, nullptr});
    auto** tail = &buckets_[hashString(text) % buckets_.size()];
    while (*tail != nullptr) {
        tail = &(*tail)->next;
    }
    *tail = node;
    ++itemCount_;
    return value;
}

StringTableEntry StringTable::lookup(std::string_view text, bool caseSens) const noexcept {
    text = textPrefix(text);
    auto* node = buckets_[hashString(text) % buckets_.size()];
    while (node != nullptr) {
        if (equalText(node->text, text, caseSens)) {
            return node->text.data();
        }
        node = node->next;
    }
    return nullptr;
}

void StringTable::resize(std::size_t newSize) {
    OPENT1_ASSERT_FATAL(newSize > 0, "String table needs at least one bucket");
    std::vector<Node*> replacement(newSize, nullptr); // strong allocation guarantee
    Node* reversed = nullptr;
    std::ranges::for_each(buckets_, [&](Node* node) {
        while (node != nullptr) {
            auto* next = node->next;
            node->next = reversed;
            reversed = node;
            node = next;
        }
    });
    while (reversed != nullptr) {
        auto* node = reversed;
        reversed = node->next;
        auto& bucket = replacement[hashString(node->text) % newSize];
        node->next = bucket;
        bucket = node;
    }
    buckets_.swap(replacement);
}

StringTable& globalStringTable() {
    static StringTable instance;
    return instance;
}
} // namespace opent1
