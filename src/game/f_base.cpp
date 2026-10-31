#include <opent1/game/f_base.hpp>
#include <filesystem>
#include <fstream>

namespace opent1::game {

namespace {
PhoenixAssert g_defaultAssert{};
PhoenixAssert* g_activeAssert = &g_defaultAssert;
thread_local std::string g_avarBuffer{};
} // namespace

PhoenixAssert::PhoenixAssert() {
    std::error_code ec{};
    const auto cwd = std::filesystem::current_path(ec).string();
    if (!ec && !cwd.empty() && cwd.size() <= (kMaxLength - 14)) {
        logFile_ = cwd + "\\tribesgame.log";
    }
}

void PhoenixAssert::install(PhoenixAssert* instance) noexcept {
    g_activeAssert = instance ? instance : &g_defaultAssert;
}

PhoenixAssert& PhoenixAssert::active() noexcept {
    return *g_activeAssert;
}

void PhoenixAssert::setLogFile(std::string_view path) {
    logFile_.assign(path);
}

void PhoenixAssert::reset() noexcept {
    flags_ = ASSERT_NO_DIALOG;
    firstAssert_ = true;
    exceptionsEnabled_ = false;
    countMessage_ = 0;
    countWarning_ = 0;
    countISV_ = 0;
    logBuffer_.clear();
    lastLine1_.clear();
    lastLine2_.clear();
}

void PhoenixAssert::log(std::string_view line1, std::string_view line2) {
    if (flags_ & ASSERT_NO_LOG) {
        return;
    }
    const bool writeSep = firstAssert_;
    if (firstAssert_) {
        logBuffer_.append(kSeparator);
        firstAssert_ = false;
    }
    if (!line1.empty()) {
        logBuffer_.append(line1);
    }
    logBuffer_.append(line2);
    logBuffer_.append("\r\n");

    if (!logFile_.empty()) {
        std::ofstream out(logFile_, std::ios::binary | std::ios::app);
        if (out) {
            if (writeSep) {
                out.write(kSeparator.data(), static_cast<std::streamsize>(kSeparator.size()));
            }
            if (!line1.empty()) {
                out.write(line1.data(), static_cast<std::streamsize>(line1.size()));
            }
            out.write(line2.data(), static_cast<std::streamsize>(line2.size()));
            out.write("\r\n", 2);
        }
    }
}

void PhoenixAssert::process(PhoenixAssertType type,
                            std::string_view fileName,
                            UInt32 lineNumber,
                            std::string_view message) {
    switch (type) {
        case ASSERT_MSG:
            if (flags_ & (ASSERT_NO_MESSAGE | ASSERT_NO_LOG)) {
                return;
            }
            ++countMessage_;
            lastLine1_.clear();
            lastLine2_.assign(message);
            log({}, message);
            break;

        case ASSERT_WRN: {
            if (flags_ & (ASSERT_NO_WARNING | ASSERT_NO_LOG)) {
                return;
            }
            ++countWarning_;
            const std::string fileLine =
                std::format("WARNING: {} @ {}\r\n  ", fileName, lineNumber);
            lastLine1_ = fileLine;
            lastLine2_.assign(message);
            log(fileLine, message);
            break;
        }

        case ASSERT_EXC:
        case ASSERT_ISV:
        default: {
            ++countISV_;
            const std::string fileLine = std::format("FATAL:   {} @ {}\r\n", fileName, lineNumber);
            lastLine1_ = fileLine;
            lastLine2_.assign(message);
            if (callback_) {
                callback_(fileLine, message);
            }
            log(fileLine, message);
            if (exceptionsEnabled_) {
                throw PhoenixAssertException{
                    std::string(fileName), lineNumber, std::string(message)};
            }
            break;
        }
    }
}

std::string_view cacheAvarOutput(std::string formatted) {
    if (formatted.size() > 254) {
        formatted.resize(254);
    }
    g_avarBuffer = std::move(formatted);
    return g_avarBuffer;
}

} // namespace opent1::game
