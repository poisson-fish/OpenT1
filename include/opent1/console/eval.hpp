#pragma once
#include <opent1/console/lexer_parser.hpp>
#include <span>

namespace opent1 {
class Console;
}

namespace opent1::script {
struct ExecutionLimits {
    Limits parser{.legacyDecimalIntegers = true};
    // Legacy execution is unmetered. Hosts may opt into finite limits; nested
    // eval/exec and callbacks share them. Native stack exhaustion is guarded
    // independently of a script's instruction or recursion count.
    static constexpr std::size_t Unlimited = std::numeric_limits<std::size_t>::max();
    std::size_t maxSteps = Unlimited;
    std::size_t maxDepth = Unlimited;
    std::size_t maxValueBytes = Unlimited;
    std::size_t maxArguments = 511; // excludes command name; field lists capped at 127
};

struct ExecutionResult {
    std::string value;
    std::vector<Diagnostic> diagnostics;

    [[nodiscard]] bool ok() const noexcept {
        return diagnostics.empty();
    }
};

// Locale-independent legacy conversions. Expression truth accepts exactly
// "True" (ASCII insensitive) or a numeric prefix. Console bool accepts T*/F*.
[[nodiscard]] double toNumber(std::string_view text) noexcept;
[[nodiscard]] std::int32_t toInteger(std::string_view text) noexcept;
[[nodiscard]] bool toBool(std::string_view text) noexcept;
[[nodiscard]] std::string formatNumber(double value);

// Evaluates owned parser output; registered functions retain shared ownership
// of their Program. Console::evaluate is the normal parse-and-execute entry.
[[nodiscard]] ExecutionResult evaluate(Console& console, Program program);
} // namespace opent1::script
