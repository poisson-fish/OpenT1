#include <opent1/game/ban_list.hpp>
#include <ranges>
#include <opent1/core/text.hpp>
#include <algorithm>
#include <cctype>
#include <ctime>
#include <fstream>

namespace opent1::game {
namespace {

[[nodiscard]] char toLowerAscii(char c) noexcept {
    return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
}

[[nodiscard]] bool startsWithIgnoreCaseAscii(std::string_view s, std::string_view prefix) noexcept {
    return s.size() >= prefix.size() && equalIgnoreCase(s.substr(0, prefix.size()), prefix);
}

} // namespace

BanList::BanList(ClockFn clock) : clock_(std::move(clock)) {}

void BanList::setClock(ClockFn clock) {
    clock_ = std::move(clock);
}

Int32 BanList::currentTime() const {
    if (clock_) {
        return clock_();
    }
    return static_cast<Int32>(std::time(nullptr));
}

void BanList::addBan(std::string_view transportAddress, Int32 banTime) {
    const Int32 curTime = currentTime();
    if (banTime != 0 && banTime < curTime) {
        return;
    }

    const auto boundedTa = transportAddress.substr(0, kMaxAddrLen - 1);
    const auto found = std::ranges::find_if(list_, [&](const BanInfo& entry) {
        return equalIgnoreCase(entry.transportAddress, boundedTa);
    });
    if (found != list_.end()) {
        found->bannedUntil = banTime;
        return;
    }

    BanInfo info;
    info.transportAddress = std::string(boundedTa);
    if (startsWithIgnoreCaseAscii(info.transportAddress, "ip:")) {
        const auto colonPos = info.transportAddress.find(':', 3);
        if (colonPos != std::string::npos) {
            info.transportAddress.resize(colonPos + 1);
            info.transportAddress.push_back('*');
        }
    }
    info.bannedUntil = banTime;
    list_.push_back(std::move(info));
}

void BanList::addBanRelative(std::string_view transportAddress, Int32 numSeconds) {
    const Int32 curTime = currentTime();
    const Int32 banTime = (numSeconds != -1) ? (curTime + numSeconds) : 0;
    addBan(transportAddress, banTime);
}

void BanList::removeBan(std::string_view transportAddress) {
    const auto boundedTa = transportAddress.substr(0, kMaxAddrLen - 1);
    const auto found = std::ranges::find_if(list_, [&](const BanInfo& entry) {
        return equalIgnoreCase(entry.transportAddress, boundedTa);
    });
    if (found != list_.end()) {
        list_.erase(found);
    }
}

bool BanList::isTAEq(std::string_view bannedTA, std::string_view ta) const noexcept {
    for (const auto i : std::views::iota(std::size_t{0})) {
        const char a = (i < bannedTA.size()) ? bannedTA[i] : '\0';
        const char b = (i < ta.size()) ? ta[i] : '\0';
        if (a == '*' || (a == '\0' && b == ':')) {
            return true;
        }
        if (toLowerAscii(a) != toLowerAscii(b)) {
            return false;
        }
        if (a == '\0') {
            return true;
        }
    }
    return false;
}

bool BanList::isBanned(std::string_view transportAddress) {
    const Int32 curTime = currentTime();
    for (auto it = list_.begin(); it != list_.end();) {
        if (it->bannedUntil != 0 && it->bannedUntil < curTime) {
            it = list_.erase(it);
            continue;
        }
        if (isTAEq(it->transportAddress, transportAddress)) {
            return true;
        }
        ++it;
    }
    return false;
}

void BanList::pruneExpired() {
    const Int32 curTime = currentTime();
    std::erase_if(list_, [curTime](const BanInfo& info) {
        return info.bannedUntil != 0 && info.bannedUntil < curTime;
    });
}

std::string BanList::exportToText(std::string_view /*fileName*/) {
    std::string out;
    for (const auto& entry : list_) {
        out += "BanList::addAbsolute(\"";
        out += entry.transportAddress;
        out += "\", ";
        out += std::to_string(entry.bannedUntil);
        out += ");\r\n";
    }
    return out;
}

bool BanList::exportToFile(const std::filesystem::path& path, std::string_view scriptDisplayName) {
    std::error_code ec;
    if (path.has_parent_path()) {
        std::filesystem::create_directories(path.parent_path(), ec);
    }
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file.is_open()) {
        return false;
    }
    const auto text =
        exportToText(scriptDisplayName.empty() ? path.generic_string() : scriptDisplayName);
    file.write(text.data(), static_cast<std::streamsize>(text.size()));
    return file.good();
}

bool BanList::exportToFile(std::string_view fileName) {
    if (fileName.empty()) {
        return false;
    }
    return exportToFile(std::filesystem::path(fileName), fileName);
}

} // namespace opent1::game
