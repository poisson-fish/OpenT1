#include <opent1/core/filename.hpp>
#include <opent1/core/base.hpp>
#include <algorithm>

namespace opent1 {
bool FileName::operator==(std::string_view text) const noexcept {
    const auto fold = [](unsigned char c) {
        return c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c;
    };
    return std::ranges::equal(rep_, text, {}, fold, fold);
}

void FileName::assign(std::string_view text) {
    OPENT1_ASSERT_FATAL(text.size() < kFileNameMaxLength, "Filename too long");
    OPENT1_ASSERT_FATAL(text.find('\0') == std::string_view::npos, "Filename contains NUL");
    // Temporary also makes assignment from our own component views safe.
    rep_ = std::string(text);
}

std::string FileName::getTitle() const {
    const auto separator = rep_.find_last_of("\\/:");
    return separator == std::string::npos ? rep_ : rep_.substr(separator + 1);
}

std::string FileName::getPath() const {
    const auto separator = rep_.find_last_of("\\/:");
    return separator == std::string::npos ? std::string{} : rep_.substr(0, separator + 1);
}

std::string FileName::getExt() const {
    const auto title = getTitle();
    const auto dot = title.rfind('.');
    return dot == std::string::npos ? std::string{} : title.substr(dot + 1);
}

std::string FileName::getBase() const {
    const auto title = getTitle();
    return title.substr(0, title.rfind('.'));
}

void FileName::setExt() {
    assign(getPath() + getBase());
}

void FileName::setExt(std::string_view extension) {
    assign(getPath() + getBase() + "." + std::string(extension));
}

void FileName::setBase(std::string_view base) {
    assign(getPath() + std::string(base) + "." + getExt());
}

void FileName::setTitle(std::string_view title) {
    assign(getPath() + std::string(title));
}

void FileName::setPath(std::string_view path) {
    std::string temporary(path);
    if (!temporary.empty() && temporary.back() != '/' && temporary.back() != '\\' &&
        temporary.back() != ':') {
        temporary += '\\';
    }
    assign(temporary + getTitle());
}

std::string FileName::normalizedForwardSlashes() const {
    auto text = rep_;
    std::ranges::replace(text, '\\', '/');
    return text;
}

std::filesystem::path FileName::toFilesystemPath() const {
    const auto text = normalizedForwardSlashes();
    std::u8string utf8;
    std::ranges::transform(text, std::back_inserter(utf8), [](char c) {
        return static_cast<char8_t>(c);
    });
    return std::filesystem::path(utf8).lexically_normal();
}
} // namespace opent1
