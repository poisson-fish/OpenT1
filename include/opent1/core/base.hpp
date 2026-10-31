#pragma once

#include <opent1/core/types.hpp>

#include <concepts>
#include <exception>
#include <filesystem>
#include <format>
#include <functional>
#include <limits>
#include <source_location>
#include <string>
#include <string_view>
#include <utility>

namespace opent1 {

// -----------------------------------------------------------------------------
// Assert Flags & Types (Parity with Darkstar Core/inc/base.h)
// -----------------------------------------------------------------------------
using AssertFlag = UInt32;

inline constexpr AssertFlag kAssertNoLog = 1U << 0U;
inline constexpr AssertFlag kAssertNoDialog = 1U << 1U;
inline constexpr AssertFlag kAssertNoWarning = 1U << 2U;
inline constexpr AssertFlag kAssertNoMessage = 1U << 3U;

enum class AssertType : UInt32 {
    Message = 1,
    Warning = 2,
    Isv = 3,
    Exception = 4,
};

using AssertCallback = void (*)(std::string_view line1, std::string_view line2);

// -----------------------------------------------------------------------------
// AssertException (C++20 std::exception-compatible)
// -----------------------------------------------------------------------------
class AssertException final : public std::exception {
public:
    AssertException(std::string_view file,
                    UInt32 line,
                    std::string_view message,
                    std::string_view function = {});

    [[nodiscard]] const char* what() const noexcept override {
        return formatted_.c_str();
    }

    [[nodiscard]] std::string_view file() const noexcept {
        return file_;
    }

    [[nodiscard]] UInt32 line() const noexcept {
        return line_;
    }

    [[nodiscard]] std::string_view message() const noexcept {
        return message_;
    }

    [[nodiscard]] std::string_view function() const noexcept {
        return function_;
    }

private:
    std::string file_;
    UInt32 line_{0};
    std::string message_;
    std::string function_;
    std::string formatted_;
};

// -----------------------------------------------------------------------------
// PhoenixAssert — Modernized Assertion Processor & Logger
// -----------------------------------------------------------------------------
class PhoenixAssert {
public:
    PhoenixAssert();
    ~PhoenixAssert();

    PhoenixAssert(const PhoenixAssert&) = delete;
    PhoenixAssert& operator=(const PhoenixAssert&) = delete;

    void process(AssertType type,
                 std::string_view message,
                 std::source_location location = std::source_location::current());

    void process(AssertType type, std::string_view file, UInt32 line, std::string_view message);

    void installCallback(AssertCallback callback) noexcept {
        callback_ = callback;
    }

    void setFlags(AssertFlag flags) noexcept {
        flags_ = flags;
    }

    [[nodiscard]] AssertFlag flags() const noexcept {
        return flags_;
    }

    void enableExceptions() noexcept {
        exceptionsEnabled_ = true;
    }

    void disableExceptions() noexcept {
        exceptionsEnabled_ = false;
    }

    [[nodiscard]] bool exceptionsEnabled() const noexcept {
        return exceptionsEnabled_;
    }

    void setLogFilePath(std::filesystem::path path);

    [[nodiscard]] const std::filesystem::path& logFilePath() const noexcept {
        return logFilePath_;
    }

    [[nodiscard]] UInt32 countMessage() const noexcept {
        return countMessage_;
    }

    [[nodiscard]] UInt32 countWarning() const noexcept {
        return countWarning_;
    }

    [[nodiscard]] UInt32 countISV() const noexcept {
        return countISV_;
    }

    void resetCounts() noexcept {
        countMessage_ = 0;
        countWarning_ = 0;
        countISV_ = 0;
        firstAssert_ = true;
    }

    bool inDebugger{false};

private:
    void processAt(AssertType type,
                   std::string_view file,
                   UInt32 line,
                   std::string_view message,
                   std::string_view function);
    void log(std::string_view line1, std::string_view line2);

    AssertCallback callback_{nullptr};
    AssertFlag flags_{kAssertNoDialog | kAssertNoLog};
    bool firstAssert_{true};
    std::filesystem::path logFilePath_;
    UInt32 countMessage_{0};
    UInt32 countWarning_{0};
    UInt32 countISV_{0};
    bool exceptionsEnabled_{true};
};

using Phoenix_Assert = PhoenixAssert;

[[nodiscard]] PhoenixAssert& globalAssert() noexcept;

// -----------------------------------------------------------------------------
// Formatting Helpers
// -----------------------------------------------------------------------------
template <typename... Args>
[[nodiscard]] std::string formatMessage(std::format_string<Args...> fmt, Args&&... args) {
    return std::format(fmt, std::forward<Args>(args)...);
}

// -----------------------------------------------------------------------------
// C++20 Assertion Functions with std::source_location
// -----------------------------------------------------------------------------
inline void assertMessage(bool condition,
                          std::string_view message,
                          std::source_location location = std::source_location::current()) {
    if (!condition) {
        globalAssert().process(AssertType::Message, message, location);
    }
}

inline void assertWarn(bool condition,
                       std::string_view message,
                       std::source_location location = std::source_location::current()) {
    if (!condition) {
        globalAssert().process(AssertType::Warning, message, location);
    }
}

inline void assertFatal(bool condition,
                        std::string_view message,
                        std::source_location location = std::source_location::current()) {
    if (!condition) {
        globalAssert().process(AssertType::Exception, message, location);
    }
}

inline void assertISV(bool condition,
                      std::string_view message,
                      std::source_location location = std::source_location::current()) {
    if (!condition) {
        globalAssert().process(AssertType::Isv, message, location);
    }
}

// -----------------------------------------------------------------------------
// 64-Bit Safe Checked Narrowing Conversion
// -----------------------------------------------------------------------------
template <typename Target, typename Source>
    requires std::integral<Target> && std::integral<Source>
[[nodiscard]] constexpr Target narrowCast(Source value) noexcept {
    return static_cast<Target>(value);
}

template <typename Target, typename Source>
    requires std::integral<Target> && std::integral<Source>
[[nodiscard]] constexpr Target
checkedNarrow(Source value, std::source_location location = std::source_location::current()) {
    const bool fits = [&] {
        if constexpr (std::same_as<Target, bool>) {
            return value == 0 || value == 1;
        } else if constexpr (std::same_as<Source, bool>) {
            return true;
        } else {
            return std::in_range<Target>(value);
        }
    }();
    if (!fits) {
        assertFatal(false, "Integral narrowing conversion overflow", location);
    }
    return static_cast<Target>(value);
}

} // namespace opent1

#define OPENT1_ASSERT_MSG(cond, msg)                                                               \
    do {                                                                                           \
        if (!static_cast<bool>(cond)) {                                                            \
            ::opent1::assertMessage(false, (msg));                                                 \
        }                                                                                          \
    } while (false)
#define OPENT1_ASSERT_WARN(cond, msg)                                                              \
    do {                                                                                           \
        if (!static_cast<bool>(cond)) {                                                            \
            ::opent1::assertWarn(false, (msg));                                                    \
        }                                                                                          \
    } while (false)
#define OPENT1_ASSERT_FATAL(cond, msg)                                                             \
    do {                                                                                           \
        if (!static_cast<bool>(cond)) {                                                            \
            ::opent1::assertFatal(false, (msg));                                                   \
        }                                                                                          \
    } while (false)
#define OPENT1_ASSERT_ISV(cond, msg)                                                               \
    do {                                                                                           \
        if (!static_cast<bool>(cond)) {                                                            \
            ::opent1::assertISV(false, (msg));                                                     \
        }                                                                                          \
    } while (false)
