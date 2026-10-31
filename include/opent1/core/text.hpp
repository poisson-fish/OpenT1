#pragma once

#include <algorithm>
#include <cctype>
#include <string_view>

namespace opent1 {

// Preserve case folding in the current C locale and compare the entire views,
// including embedded NULs. Cast bytes before passing them to std::tolower.
[[nodiscard]] inline bool equalIgnoreCase(std::string_view a, std::string_view b) noexcept {
    const auto fold = [](unsigned char c) {
        return std::tolower(c);
    };
    return std::ranges::equal(a, b, {}, fold, fold);
}

} // namespace opent1
