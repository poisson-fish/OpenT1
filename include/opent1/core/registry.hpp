#pragma once
#include <opent1/core/stream_io.hpp>
#include <filesystem>
#include <map>
#include <optional>
#include <variant>
#include <vector>

namespace opent1 {
// Portable, instance-owned configuration tree; never accesses the host registry.
// ASCII-insensitive absolute open paths use either slash. close closes the whole
// selection; remove addresses a subtree relative to it. External synchronization
// is required. Explicit store/load persists an OpenT1 format, not a Windows hive.
class Registry {
public:
    using Value =
        std::variant<std::string, std::vector<std::string>, Int32, Flt32, std::vector<std::byte>>;
    bool open(std::string_view key, bool create = false);

    void close() noexcept {
        current_.reset();
    }

    bool remove(std::string_view subkey);
    [[nodiscard]] std::vector<std::string> enumerateKeys() const;
    [[nodiscard]] std::vector<std::string> enumerateTags() const;
    bool set(std::string_view tag, Value value);
    [[nodiscard]] std::optional<Value> get(std::string_view tag) const;

    template <class T> bool read(std::string_view tag, T& out) const {
        const auto value = get(tag);
        if (!value) {
            return false;
        }
        const auto* typed = std::get_if<T>(&*value);
        if (!typed) {
            return false;
        }
        out = *typed;
        return true;
    }

    bool store(StreamIO& stream) const;
    bool load(StreamIO& stream); // failure preserves tree and selection
    bool fileStore(const std::filesystem::path& path) const;
    bool fileLoad(const std::filesystem::path& path);

private:
    using Tags = std::map<std::string, Value, std::less<>>;
    std::map<std::string, Tags, std::less<>> keys_;
    std::optional<std::string> current_;
};
} // namespace opent1
