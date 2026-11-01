#pragma once
#include <opent1/core/types.hpp>
#include <filesystem>
#include <string>
#include <string_view>

namespace opent1 {
inline constexpr std::size_t kFileNameMaxLength = 1024;

// Resolve legacy Windows spelling on every host. Missing output components are
// retained, so this also works for newly exported files.
[[nodiscard]] std::filesystem::path resolveFilePath(const std::filesystem::path& root,
                                                    std::string_view name);

// Text is UTF-8. Component parsing recognizes both slash styles and drive colons
// on every host; filesystem conversion normalizes separators and dot components.
class FileName {
public:
    FileName() = default;

    explicit FileName(std::string_view text) {
        assign(text);
    }

    FileName& operator=(std::string_view text) {
        assign(text);
        return *this;
    }

    [[nodiscard]] bool operator==(const FileName& other) const noexcept {
        return *this == other.view();
    }

    [[nodiscard]] bool operator==(std::string_view text) const noexcept;

    [[nodiscard]] const char* c_str() const noexcept {
        return rep_.c_str();
    }

    [[nodiscard]] std::string_view view() const noexcept {
        return rep_;
    }

    [[nodiscard]] const std::string& str() const noexcept {
        return rep_;
    }

    [[nodiscard]] bool empty() const noexcept {
        return rep_.empty();
    }

    [[nodiscard]] std::string getExt() const;
    [[nodiscard]] std::string getBase() const;
    [[nodiscard]] std::string getTitle() const;
    [[nodiscard]] std::string getPath() const;
    void setExt();                           // Remove the dot and extension.
    void setExt(std::string_view extension); // Includes a dot even for empty text.
    void setBase(std::string_view base = {});
    void setTitle(std::string_view title = {});
    void setPath(std::string_view path = {});
    [[nodiscard]] std::string normalizedForwardSlashes() const;
    [[nodiscard]] std::filesystem::path toFilesystemPath() const;

private:
    void assign(std::string_view text);
    std::string rep_;
};
} // namespace opent1
