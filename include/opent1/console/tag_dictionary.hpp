#pragma once

#include <opent1/core/string_table.hpp>
#include <map>
#include <optional>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace opent1 {
class StreamIO;

// Instance-owned tags. Definitions use the StringTable's ASCII-insensitive
// identity and first spelling; values retain their case. Views remain valid
// until dictionary destruction, including across insertions. Text inputs stop
// at the first NUL and share StringTable::kMaxStringLength's allocation limit.
// Concurrent reads are permitted; mutation requires external synchronization.
class TagDictionary {
public:
    TagDictionary() = default;
    TagDictionary(const TagDictionary&) = delete;
    TagDictionary& operator=(const TagDictionary&) = delete;

    // Zero is reserved for a missing tag. Negative IDs, duplicate IDs and
    // duplicate definitions return false without replacing an existing entry.
    [[nodiscard]] bool addEntry(Int32 id, std::string_view define, std::string_view text);

    [[nodiscard]] std::size_t size() const noexcept {
        return byId_.size();
    }

    [[nodiscard]] Int32 defineToId(std::string_view define) const noexcept;
    // An engaged empty view is an existing empty value; nullopt means missing.
    [[nodiscard]] std::optional<std::string_view>
    defineToString(std::string_view define) const noexcept;
    [[nodiscard]] std::optional<std::string_view> idToString(Int32 id) const noexcept;
    [[nodiscard]] std::optional<std::string_view> idToDefine(Int32 id) const noexcept;

    // Append matches, then sort the entire vector, retaining existing elements
    // and duplicates. ID endpoints are exclusive. Wildcards are case-sensitive
    // byte matches: '*' matches zero or more bytes, '?' exactly one byte.
    void findIDs(std::vector<Int32>& out, Int32 minId, Int32 maxId) const;
    void findStrings(std::vector<Int32>& out, std::string_view pattern) const;
    void findDefines(std::vector<Int32>& out, std::string_view pattern) const;

    // Legacy CRLF header format, ascending IDs, definitions copied verbatim.
    // The caller chooses the guard's numeric suffix instead of reading a host
    // tick counter. Default output is deterministic. False means discard output.
    [[nodiscard]] bool writeHeader(StreamIO& stream, UInt32 guardId = 0) const;

private:
    struct Entry {
        std::string_view define, text;
    };

    [[nodiscard]] static bool match(std::string_view pattern, std::string_view text) noexcept;
    StringTable strings_;
    std::map<Int32, Entry> byId_;
    std::unordered_map<StringTableEntry, Int32> byDefine_;
};
} // namespace opent1
