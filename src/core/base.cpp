#include <opent1/core/base.hpp>

#include <array>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <fstream>

namespace opent1 {

namespace {
constexpr std::string_view kSeparator =
    "========================================================================\r\n";
} // namespace

AssertException::AssertException(std::string_view file,
                                 UInt32 line,
                                 std::string_view message,
                                 std::string_view function)
    : file_(file), line_(line), message_(message), function_(function),
      formatted_(
          std::format("Assertion failed at {}:{} ({}): {}", file_, line_, function_, message_)) {}

PhoenixAssert::PhoenixAssert() : logFilePath_("assert.log") {}

PhoenixAssert::~PhoenixAssert() = default;

void PhoenixAssert::setLogFilePath(std::filesystem::path path) {
    logFilePath_ = std::move(path);
}

void PhoenixAssert::log(std::string_view line1, std::string_view line2) {
    if ((flags_ & kAssertNoLog) != 0U || logFilePath_.empty()) {
        return;
    }

    std::ofstream out(logFilePath_, std::ios::binary | std::ios::app);
    if (!out.is_open()) {
        return;
    }

    if (firstAssert_) {
        out.write(kSeparator.data(), static_cast<std::streamsize>(kSeparator.size()));
        firstAssert_ = false;
    }
    if (!line1.empty()) {
        out.write(line1.data(), static_cast<std::streamsize>(line1.size()));
    }
    out.write(line2.data(), static_cast<std::streamsize>(line2.size()));
    out.write("\r\n", 2);
}

void PhoenixAssert::process(AssertType type,
                            std::string_view message,
                            std::source_location location) {
    processAt(type,
              location.file_name(),
              static_cast<UInt32>(location.line()),
              message,
              location.function_name());
}

void PhoenixAssert::process(AssertType type,
                            std::string_view file,
                            UInt32 line,
                            std::string_view message) {
    processAt(type, file, line, message, {});
}

void PhoenixAssert::processAt(AssertType type,
                              std::string_view safeFile,
                              UInt32 line,
                              std::string_view safeMsg,
                              std::string_view function) {

    switch (type) {
        case AssertType::Message: {
            if ((flags_ & (kAssertNoMessage | kAssertNoLog)) != 0U) {
                return;
            }
            ++countMessage_;
            log({}, safeMsg);
            break;
        }
        case AssertType::Warning: {
            if ((flags_ & (kAssertNoWarning | kAssertNoLog)) != 0U) {
                return;
            }
            ++countWarning_;
            const std::string header = std::format("WARNING: {} @ {}\r\n  ", safeFile, line);
            log(header, safeMsg);
            break;
        }
        case AssertType::Isv:
        case AssertType::Exception:
        default: {
            ++countISV_;
            const std::string header = std::format("FATAL:   {} @ {}\r\n", safeFile, line);
            if (callback_ != nullptr) {
                callback_(header, safeMsg);
            }
            log(header, safeMsg);

            if (exceptionsEnabled_) {
                throw AssertException(safeFile, line, safeMsg, function);
            }
            std::abort();
        }
    }
}

PhoenixAssert& globalAssert() noexcept {
    static PhoenixAssert instance;
    return instance;
}

const char* avar(const char* fmt, ...) {
    thread_local std::array<char, 1024> buffer{};
    if (fmt == nullptr) {
        buffer[0] = '\0';
        return buffer.data();
    }

    va_list args;
    va_start(args, fmt);
    std::vsnprintf(buffer.data(), buffer.size(), fmt, args);
    va_end(args);
    buffer.back() = '\0';
    return buffer.data();
}

} // namespace opent1
