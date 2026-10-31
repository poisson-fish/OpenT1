#pragma once
#include <opent1/core/types.hpp>
#include <filesystem>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace opent1::game {

class BanList {
public:
    static constexpr std::size_t kMaxAddrLen = 256;
    static constexpr Int32 kPermanentBan = 0;

    struct BanInfo {
        std::string transportAddress;
        Int32 bannedUntil{};
    };

    using ClockFn = std::function<Int32()>;

    explicit BanList(ClockFn clock = {});

    void setClock(ClockFn clock);
    [[nodiscard]] Int32 currentTime() const;

    void addBan(std::string_view transportAddress, Int32 banTime);
    void addBanRelative(std::string_view transportAddress, Int32 numSeconds);
    void removeBan(std::string_view transportAddress);
    [[nodiscard]] bool isBanned(std::string_view transportAddress);
    [[nodiscard]] bool isTAEq(std::string_view bannedTA, std::string_view ta) const noexcept;

    [[nodiscard]] std::string exportToText(std::string_view fileName = "config/banlist.cs");
    bool exportToFile(const std::filesystem::path& path, std::string_view scriptDisplayName = {});
    bool exportToFile(std::string_view fileName);

    void pruneExpired();

    void clear() noexcept {
        list_.clear();
    }

    [[nodiscard]] std::span<const BanInfo> entries() const noexcept {
        return list_;
    }

private:
    std::vector<BanInfo> list_;
    ClockFn clock_;
};

} // namespace opent1::game
