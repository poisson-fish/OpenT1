#include "runtime_internal.hpp"
#include <opent1/core/filename.hpp>
#include <opent1/core/res_manager.hpp>
#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <ctime>
#include <iomanip>

namespace opent1::script {
std::string_view cText(std::string_view text) noexcept {
    return text.substr(0, text.find('\0'));
}

std::string canonical(std::string_view text) {
    std::string out(cText(text));
    std::ranges::transform(out, out.begin(), [](char c) {
        return c >= 'A' && c <= 'Z' ? static_cast<char>(c + ('a' - 'A')) : c;
    });
    return out;
}

namespace {
std::string_view numberStart(std::string_view text) {
    text = cText(text);
    const auto first = text.find_first_not_of(" \t\n\r\f\v");
    text.remove_prefix(first == text.npos ? text.size() : first);
    if (text.starts_with('+')) {
        text.remove_prefix(1);
        if (text.starts_with('-') || text.starts_with('+')) {
            return {};
        }
    }
    return text;
}

bool isTrue(std::string_view text) noexcept {
    text = cText(text);
    return text.size() == 4 && (text[0] == 't' || text[0] == 'T') &&
           (text[1] == 'r' || text[1] == 'R') && (text[2] == 'u' || text[2] == 'U') &&
           (text[3] == 'e' || text[3] == 'E');
}
} // namespace

double parseNumber(std::string_view text) noexcept {
    text = numberStart(text);
    double result = 0;
    if (text.empty()) {
        return 0;
    }
    const bool negative = text.starts_with('-');
    const auto unsignedText = negative ? text.substr(1) : text;
    const bool hex = unsignedText.starts_with("0x") || unsignedText.starts_with("0X");
    const auto input = hex ? unsignedText.substr(2) : text;
    if (input.empty()) {
        return 0;
    }
    const auto parsed = std::from_chars(input.data(),
                                        input.data() + input.size(),
                                        result,
                                        hex ? std::chars_format::hex : std::chars_format::general);
    if (hex && negative) {
        result = -result;
    }
    if (parsed.ec == std::errc::result_out_of_range) {
        // from_chars leaves the destination unchanged on some standard
        // libraries. Distinguish decimal overflow from underflow explicitly.
        text = hex ? input : unsignedText;
        const auto end = text.find_first_of(hex ? "pP" : "eE");
        const auto mantissa = text.substr(0, end);
        const auto dot = mantissa.find('.');
        const auto first = mantissa.find_first_not_of("0.");
        std::int64_t exponent = 0;
        if (end != text.npos) {
            auto digits = text.substr(end + 1);
            const bool minus = digits.starts_with('-');
            if (minus || digits.starts_with('+')) {
                digits.remove_prefix(1);
            }
            for (const auto digit : digits) {
                if (digit < '0' || digit > '9') {
                    break;
                }
                exponent = std::min<std::int64_t>(1'000'000'000, exponent * 10 + (digit - '0'));
            }
            if (minus) {
                exponent = -exponent;
            }
        }
        const auto decimal = dot == mantissa.npos ? mantissa.size() : dot;
        const auto order = first == mantissa.npos
                               ? 0
                               : exponent + (hex ? 4 : 1) * (static_cast<std::int64_t>(decimal) -
                                                             static_cast<std::int64_t>(first) -
                                                             (first < decimal ? 1 : 0));
        result = first != mantissa.npos && order > 0 ? std::numeric_limits<double>::infinity() : 0;
        if (negative) {
            result = -result;
        }
    }
    return result;
}

std::int32_t parseInteger(std::string_view text) noexcept {
    text = numberStart(text);
    std::int32_t result = 0;
    if (!text.empty()) {
        const auto parsed = std::from_chars(text.data(), text.data() + text.size(), result);
        if (parsed.ec == std::errc::result_out_of_range) {
            result = text.starts_with('-') ? std::numeric_limits<std::int32_t>::min()
                                           : std::numeric_limits<std::int32_t>::max();
        }
    }
    return result;
}

double toNumber(std::string_view text) noexcept {
    return isTrue(text) ? 1 : parseNumber(text);
}

std::int32_t toInteger(std::string_view text) noexcept {
    return isTrue(text) ? 1 : parseInteger(text);
}

bool toBool(std::string_view text) noexcept {
    text = cText(text);
    if (text.starts_with('t') || text.starts_with('T')) {
        return true;
    }
    if (text.starts_with('f') || text.starts_with('F')) {
        return false;
    }
    return toNumber(text) != 0;
}

std::string formatNumber(double value) {
    std::array<char, 64> bytes{};
    const auto result = std::to_chars(
        bytes.data(), bytes.data() + bytes.size(), value, std::chars_format::general, 6);
    if (result.ec != std::errc{}) {
        throw std::runtime_error("Cannot format script number");
    }
    return {bytes.data(), result.ptr};
}
} // namespace opent1::script

namespace opent1 {
namespace {
std::string globalName(std::string_view name) {
    name = script::cText(name);
    return script::canonical(name.starts_with('$') ? std::string(name) : "$" + std::string(name));
}

// Original FindMatch: literals fold ASCII, but '*' searches for the next
// literal byte with strchr. Preserve even its empty-suffix edge cases.
bool match(std::string_view pattern, std::string_view text, const std::function<void()>& step) {
    struct Retry {
        std::size_t pattern, next;
    };

    std::vector<Retry> retries;
    std::size_t p = 0, t = 0;
    const auto fold = [](char c) {
        return c >= 'A' && c <= 'Z' ? static_cast<char>(c + 32) : c;
    };
    const auto find = [&](std::size_t at, std::size_t start) {
        if (start > text.size()) {
            return text.npos;
        }
        return at == pattern.size() ? text.size() : text.find(pattern[at], start);
    };
    for (;;) {
        step();
        if (p == pattern.size() && t == text.size()) {
            return true;
        }
        if (p < pattern.size() && t < text.size()) {
            if (pattern[p] == '*') {
                const auto at = find(++p, t);
                if (at != text.npos) {
                    retries.push_back({p, at + 1});
                    t = at;
                    continue;
                }
            } else if (pattern[p] == '?' || fold(pattern[p]) == fold(text[t])) {
                ++p;
                ++t;
                continue;
            }
        }
        bool retry = false;
        while (!retries.empty()) {
            step();
            auto& next = retries.back();
            const auto at = find(next.pattern, next.next);
            if (at == text.npos) {
                retries.pop_back();
                continue;
            }
            p = next.pattern;
            t = at;
            next.next = at + 1;
            retry = true;
            break;
        }
        if (!retry) {
            return false;
        }
    }
}

std::string escaped(std::string_view text) {
    std::string out;
    constexpr std::string_view hex = "0123456789ABCDEF";
    for (const unsigned char c : text) {
        switch (c) {
            case '"':
                out += "\\\"";
                break;
            case '\\':
                out += "\\\\";
                break;
            case '\r':
                out += "\\r";
                break;
            case '\n':
                out += "\\n";
                break;
            case '\t':
                out += "\\t";
                break;
            default:
                if (c < 32 || c > 127) {
                    out += "\\x";
                    out += hex[c >> 4];
                    out += hex[c & 15];
                } else {
                    out += static_cast<char>(c);
                }
        }
    }
    return out;
}
} // namespace

void ConsoleDataManager::declareDataBlock(std::string_view, std::string_view) {}

void ConsoleDataManager::endDataBlock() {}

void ConsoleDataManager::abortDataBlock() {}

void ConsoleDataManager::beginObject(std::string_view, std::optional<std::string_view>) {}

void ConsoleDataManager::endObject() {}

void ConsoleDataManager::abortObject() {
    endObject();
}

std::string ConsoleDataManager::getDataField(std::string_view,
                                             std::string_view,
                                             std::optional<std::string_view>) {
    return {};
}

void ConsoleDataManager::setDataField(std::optional<std::string_view>,
                                      std::string_view,
                                      std::optional<std::string_view>,
                                      std::span<const std::string>) {}

void Console::Impl::fail(script::DiagnosticCode code, std::string message) const {
    throw script::RuntimeFault{{code, sourceName, range, std::move(message)}};
}

void Console::Impl::check(std::string_view text) const {
    if (text.size() > limits.maxValueBytes) {
        if (!invocations) {
            throw std::length_error("script value exceeds byte limit");
        }
        fail(script::DiagnosticCode::ExecutionLimit, "script value exceeds byte limit");
    }
}

void Console::Impl::step() {
    if (steps >= limits.maxSteps) {
        fail(script::DiagnosticCode::ExecutionLimit, "script execution step limit reached");
    }
    ++steps;
}

std::string Console::Impl::intern(std::string_view name) {
    name = script::cText(name);
    check(name);
    const auto key = script::canonical(name);
    return names.try_emplace(key, script::cText(name)).first->second;
}

std::string Console::Impl::read(std::string_view name) const {
    const auto key = script::canonical(name);
    if (!name.starts_with('$') && !frames.empty()) {
        const auto found = frames.back().variables.find(key);
        return found == frames.back().variables.end() ? "" : found->second;
    }
    const auto found = globals.find(key);
    if (found == globals.end()) {
        return {};
    }
    // Copy callbacks before invoking: a binding may remove or replace itself.
    const auto binding = found->second;
    auto value = binding.get ? binding.get() : binding.value;
    check(value);
    return std::string(script::cText(value));
}

void Console::Impl::write(std::string_view name, std::string_view value) {
    const auto key = script::canonical(name);
    value = script::cText(value);
    check(name);
    check(value);
    if (!name.starts_with('$') && !frames.empty()) {
        if (value.empty()) {
            frames.back().variables.erase(key);
        } else {
            frames.back().variables[key] = value;
        }
        return;
    }
    intern(name);
    auto& entry = globals[key];
    if (value.empty() && !entry.set && !entry.get) {
        globals.erase(key);
        return;
    }
    entry.value = value;
    const auto setter = entry.set;
    const std::string owned(value);
    if (setter) {
        setter(owned);
    }
}

Console::Console(ResourceManager* resources, script::ExecutionLimits limits)
    : impl_(std::make_unique<Impl>(resources, limits)) {
    impl_->builtins(*this);
}

Console::~Console() {
    impl_->commandLifetime.reset();
}

script::ExecutionResult Console::evaluate(std::string_view source, std::string_view name) {
    return script::Evaluator::run(*this, [&] {
        auto parsed = script::parse(source, script::cText(name), impl_->limits.parser);
        if (!parsed.ok()) {
            std::ranges::move(parsed.diagnostics, std::back_inserter(impl_->diagnostics));
            return;
        }
        script::Evaluator::program(
            *this, std::make_shared<const script::Program>(std::move(*parsed.program)));
    });
}

script::ExecutionResult Console::execute(std::string_view name,
                                         std::span<const std::string> arguments) {
    return script::Evaluator::run(*this, [&] {
        impl_->check(script::cText(name));
        if (arguments.size() > impl_->limits.maxArguments) {
            impl_->fail(script::DiagnosticCode::ExecutionLimit, "command argument limit reached");
        }
        std::vector<std::string> owned;
        owned.reserve(arguments.size());
        std::ranges::for_each(arguments, [&](const auto& arg) {
            impl_->check(script::cText(arg));
            owned.emplace_back(script::cText(arg));
        });
        script::Evaluator::call(*this, name, std::move(owned), false);
    });
}

script::ExecutionResult Console::executeFile(std::string_view name) {
    return script::Evaluator::run(*this, [&] {
        std::string file(script::cText(name));
        impl_->check(file);
        if (std::ranges::none_of(std::array{".cs", ".mis", ".dsc", ".mac"}, [&](auto extension) {
                return file.find(extension) != file.npos;
            })) {
            file += ".cs";
        }
        std::optional<std::string> source;
        const auto loader = impl_->loader;
        if (loader) {
            source = loader(file);
        } else if (impl_->resources) {
            std::vector<std::byte> bytes;
            const auto max = static_cast<UInt32>(std::min<std::size_t>(
                impl_->limits.parser.maxSourceBytes, std::numeric_limits<UInt32>::max()));
            if (impl_->resources->getSize(file).value_or(0) > max) {
                impl_->fail(script::DiagnosticCode::SourceLimit,
                            "loaded script exceeds byte limit");
            }
            if (impl_->resources->read(file, bytes, max)) {
                source.emplace();
                if (!bytes.empty()) {
                    source->assign(reinterpret_cast<const char*>(bytes.data()), bytes.size());
                }
            }
        }
        if (!source || source->empty()) {
            print("exec: invalid script file " + file + ".");
            impl_->result = "False";
            return;
        }
        if (source->size() > impl_->limits.parser.maxSourceBytes) {
            impl_->fail(script::DiagnosticCode::SourceLimit, "loaded script exceeds byte limit");
        }
        print("Executing " + file + ".");
        const auto result = evaluate(*source, file);
        impl_->result = result.ok() ? "True" : "False";
    });
}

void Console::addCommand(Int32 id, std::string_view name, Command command) {
    if (!command || script::cText(name).empty()) {
        throw std::invalid_argument("Command requires a name and callback");
    }
    const auto spelling = impl_->intern(name);
    impl_->commands[script::canonical(name)] = {
        spelling, id, std::move(command), {}, impl_->nextRegistration++};
}

std::shared_ptr<void> Console::scopedCommand(Int32 id, std::string_view name, Command command) {
    if (isFunction(name)) {
        throw std::runtime_error("Command namespace in use");
    }
    addCommand(id, name, std::move(command));
    const auto key = script::canonical(name);
    const auto serial = impl_->commands.at(key).registration;
    const std::weak_ptr<const int> lifetime = impl_->commandLifetime;
    return std::shared_ptr<void>(new int(0), [this, key, serial, lifetime](void* pointer) {
        delete static_cast<int*>(pointer);
        if (lifetime.expired()) {
            return;
        }
        const auto found = impl_->commands.find(key);
        if (found != impl_->commands.end() && found->second.registration == serial) {
            impl_->commands.erase(found);
        }
    });
}

bool Console::removeCommand(std::string_view name) {
    return impl_->commands.erase(script::canonical(name)) != 0;
}

bool Console::isFunction(std::string_view name) const {
    return impl_->commands.contains(script::canonical(name));
}

void Console::setVariable(std::string_view name, std::string_view value) {
    name = script::cText(name);
    impl_->write(name.starts_with('$') ? std::string(name) : "$" + std::string(name), value);
}

void Console::setFileRoot(std::filesystem::path root) {
    const auto mode = impl_->logMode;
    impl_->setLogMode(0);
    impl_->fileRoot = std::move(root);
    impl_->setLogMode(mode);
}

std::filesystem::path Console::filePath(std::string_view name) const {
    return resolveFilePath(impl_->fileRoot, name);
}

std::string Console::getVariable(std::string_view name) const {
    return impl_->read(globalName(name));
}

std::string Console::internString(std::string_view text) {
    return impl_->intern(text);
}

bool Console::hasVariable(std::string_view name) const {
    return impl_->globals.contains(globalName(name));
}

bool Console::removeVariable(std::string_view name) {
    return impl_->globals.erase(globalName(name)) != 0;
}

void Console::bindVariable(std::string_view name, VariableGetter get, VariableSetter set) {
    const auto key = globalName(name);
    impl_->intern(script::cText(name).starts_with('$') ? std::string(name)
                                                       : "$" + std::string(name));
    const auto found = impl_->globals.find(key);
    const auto previous =
        found == impl_->globals.end() ? std::optional<std::string>{} : found->second.value;
    impl_->globals[key] = {previous.value_or(""), std::move(get), set};
    if (previous && set) {
        set(*previous);
    }
}

void Console::bindVariable(std::string_view name, bool& value) {
    bindVariable(
        name,
        [&value] {
            return value ? "True" : "False";
        },
        [&value](auto text) {
            value = script::toBool(text);
        });
}

void Console::bindVariable(std::string_view name, Int32& value) {
    bindVariable(
        name,
        [&value] {
            return std::to_string(value);
        },
        [&value](auto text) {
            value = script::parseInteger(text);
        });
}

void Console::bindVariable(std::string_view name, float& value) {
    bindVariable(
        name,
        [&value] {
            return script::formatNumber(value);
        },
        [&value](auto text) {
            value = static_cast<float>(script::parseNumber(text));
        });
}

void Console::bindVariable(std::string_view name, double& value) {
    bindVariable(
        name,
        [&value] {
            std::array<char, 384> buffer{};
            const auto out = std::to_chars(
                buffer.data(), buffer.data() + buffer.size(), value, std::chars_format::fixed, 6);
            return std::string(buffer.data(), out.ptr);
        },
        [&value](auto text) {
            value = script::parseNumber(text);
        });
}

void Console::setDataManager(std::shared_ptr<ConsoleDataManager> manager) {
    impl_->data = manager ? std::move(manager) : std::make_shared<ConsoleDataManager>();
}

void Console::setScriptLoader(ScriptLoader loader) {
    impl_->loader = std::move(loader);
}

void Console::setOutput(Output output) {
    impl_->output = std::move(output);
}

std::shared_ptr<Console::Output> Console::subscribeOutput(Output output) {
    if (!output) {
        return {};
    }
    std::erase_if(impl_->outputs, [](const auto& sink) {
        return sink.expired();
    });
    if (impl_->outputs.size() >= 1024) {
        throw std::length_error("console output subscriber limit");
    }
    auto sink = std::make_shared<Output>(std::move(output));
    impl_->outputs.push_back(sink);
    return sink;
}

void Console::print(std::string_view line, Int32 level) {
    if (!impl_->printLevel || impl_->printLevel < level) {
        return;
    }
    impl_->check(line);
    const std::string owned(script::cText(line));
    impl_->logToFile(owned);
    if (impl_->logEnabled) {
        impl_->log.push_back(owned);
    }
    const auto output = impl_->output;
    const auto outputs = impl_->outputs;
    if (output) {
        output(owned);
    }
    std::ranges::for_each(outputs, [&](const auto& weak) {
        if (const auto sink = weak.lock()) {
            (*sink)(owned);
        }
    });
}

const std::vector<std::string>& Console::log() const noexcept {
    return impl_->log;
}

void Console::clearLog() {
    impl_->log.clear();
}

TagDictionary& Console::tags() noexcept {
    return impl_->tags;
}

const script::ExecutionLimits& Console::limits() const noexcept {
    return impl_->limits;
}

void Console::Impl::setLogMode(Int32 mode) {
    if (mode == logMode) {
        return;
    }
    if (mode != 0 && logMode == 0) {
        newLogFile = true;
    }
    if (logFile.is_open()) {
        logFile.close();
    }
    logFile.clear();
    logMode = mode;
    if (mode == 2) {
        logFile.open(fileRoot / "console.log", std::ios::binary | std::ios::app);
    }
}

void Console::Impl::logToFile(std::string_view line) {
    if (logMode == 0) {
        return;
    }
    if (logMode == 1) {
        logFile.clear();
        logFile.open(fileRoot / "console.log", std::ios::binary | std::ios::app);
    }
    if (logFile) {
        if (newLogFile) {
            const auto now = std::time(nullptr);
            std::tm local{};
#if defined(_WIN32)
            localtime_s(&local, &now);
#else
            localtime_r(&now, &local);
#endif
            logFile << "-------------------------- " << (local.tm_mon + 1) << '/' << local.tm_mday
                    << '/' << (local.tm_year + 1900) << " -- " << std::put_time(&local, "%H:%M:%S")
                    << " -----\r\n";
            newLogFile = false;
        }
        logFile.write(line.data(), static_cast<std::streamsize>(line.size()));
        logFile << "\r\n";
        logFile.flush();
    }
    if (logMode == 1 && logFile.is_open()) {
        logFile.close();
    }
}

void Console::Impl::builtins(Console& c) {
    c.bindVariable("Console::logBufferEnabled", logEnabled);
    c.bindVariable("Console::printLevel", printLevel);
    c.bindVariable("Console::updateMetrics", metrics);
    c.bindVariable(
        "Console::logMode",
        [this] {
            return std::to_string(logMode);
        },
        [this](std::string_view value) {
            setLogMode(script::parseInteger(value));
        });
    c.addCommand(0, "cls", [](Console& console, auto, auto) {
        console.clearLog();
        return "True";
    });
    c.addCommand(0, "sqrt", [](Console&, auto, auto a) {
        return a.size() < 2 ? std::string("0")
                            : script::formatNumber(std::sqrt(script::parseNumber(a[1])));
    });
    c.addCommand(0, "floor", [](Console&, auto, auto a) {
        const auto n = a.size() < 2 ? 0 : script::parseNumber(a[1]);
        return std::to_string(std::isfinite(n) && n >= -2147483648.0 && n < 2147483648.0
                                  ? static_cast<Int32>(n)
                                  : std::numeric_limits<Int32>::min());
    });
    const auto concat = [](Console& console, std::span<const std::string> args) {
        std::string out;
        std::ranges::for_each(args, [&](const auto& s) {
            if (s.size() > console.limits().maxValueBytes - out.size()) {
                console.impl_->fail(script::DiagnosticCode::ExecutionLimit,
                                    "concatenation exceeds byte limit");
            }
            out += s;
        });
        return out;
    };
    c.addCommand(0, "strcat", [concat](Console& console, auto, auto a) {
        return concat(console, a.subspan(1));
    });
    c.addCommand(0, "echo", [concat](Console& console, auto, auto a) {
        console.print(concat(console, a.subspan(1)));
        return "True";
    });
    c.addCommand(0, "dbecho", [concat](Console& console, auto, auto a) {
        if (a.size() > 1) {
            console.print(concat(console, a.subspan(2)), script::parseInteger(a[1]));
        }
        return "True";
    });
    c.addCommand(0, "eval", [](Console& console, auto, auto a) {
        if (a.size() != 2) {
            console.print("eval(consoleString);");
            return std::string("False");
        }
        return console.evaluate(a[1], "<eval>").value;
    });
    c.addCommand(0, "exec", [](Console& console, auto, auto a) {
        if (a.size() != 2) {
            console.print("exec(filename);");
            return std::string("False");
        }
        return console.executeFile(a[1]).value;
    });
    c.addCommand(0, "trace", [](Console& console, auto, auto) {
        std::string out = "TRACE: ";
        std::ranges::for_each(console.impl_->frames, [&](const auto& frame) {
            if (out.size() > 7) {
                out += "->";
            }
            out += frame.name;
        });
        console.print(out);
        return "True";
    });
    for (const bool functions : {false, true}) {
        const std::string suffix = functions ? "Functions" : "Variables";
        c.addCommand(0, "delete" + suffix, [functions](Console& console, auto, auto a) {
            if (a.size() < 2) {
                return "False";
            }
            auto& state = *console.impl_;
            const auto pattern = functions ? a[1] : (a[1].starts_with('$') ? a[1] : "$" + a[1]);
            if (functions) {
                std::erase_if(state.commands, [&](const auto& entry) {
                    return match(pattern, entry.second.name, [&] {
                        state.step();
                    });
                });
            } else {
                std::erase_if(state.globals, [&](const auto& entry) {
                    return match(pattern, state.names.at(entry.first), [&] {
                        state.step();
                    });
                });
            }
            return "True";
        });
        c.addCommand(
            0,
            functions ? "exportFunctions" : "export",
            [functions](Console& console, auto, auto a) {
                if (a.size() < 2) {
                    return "False";
                }
                auto& state = *console.impl_;
                const auto pattern = functions ? a[1] : (a[1].starts_with('$') ? a[1] : "$" + a[1]);
                std::map<std::string, std::string> lines;
                if (functions) {
                    std::ranges::for_each(state.commands, [&](const auto& entry) {
                        if (match(pattern, entry.second.name, [&] {
                                state.step();
                            })) {
                            lines[entry.first] = entry.second.name + "();";
                        }
                    });
                } else {
                    std::vector<std::pair<std::string, std::string>> snapshot;
                    std::ranges::for_each(state.globals, [&](const auto& entry) {
                        snapshot.emplace_back(entry.first, state.names.at(entry.first));
                    });
                    // A getter may mutate the registry or load another script.
                    std::ranges::for_each(snapshot, [&](const auto& entry) {
                        if (match(pattern, entry.second, [&] {
                                state.step();
                            })) {
                            lines[entry.first] =
                                entry.second + " = \"" + escaped(state.read(entry.first)) + "\";";
                        }
                    });
                }
                if (a.size() >= 3) {
                    const auto path = console.filePath(a[2]);
                    if (path.has_parent_path()) {
                        std::error_code ec;
                        std::filesystem::create_directories(path.parent_path(), ec);
                        if (ec) {
                            return "False";
                        }
                    }
                    std::ofstream file(path,
                                       std::ios::binary | (a.size() == 4 && script::toBool(a[3])
                                                               ? std::ios::app
                                                               : std::ios::trunc));
                    if (!file) {
                        return "False";
                    }
                    std::ranges::for_each(lines, [&](const auto& line) {
                        file << line.second << "\r\n";
                    });
                    if (!file) {
                        return "False";
                    }
                } else {
                    std::ranges::for_each(lines, [&](const auto& line) {
                        console.print(line.second);
                    });
                }
                return "True";
            });
    }
}
} // namespace opent1
