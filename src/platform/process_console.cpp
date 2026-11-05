#include <opent1/platform/process_console.hpp>
#include <array>
#include <cstdio>
#include <iostream>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <io.h>
#else
#include <cerrno>
#include <poll.h>
#include <unistd.h>
#endif

namespace opent1::platform {
namespace {
constexpr std::size_t MaxLine = 65536;
constexpr std::size_t MaxCommands = 64;

#if defined(_WIN32)
void eraseCharacter(std::string& text) {
    if (text.empty()) {
        return;
    }
    auto position = text.size() - 1;
    while (position && (static_cast<unsigned char>(text[position]) & 0xc0) == 0x80) {
        --position;
    }
    text.resize(position);
}

bool validHandle(HANDLE handle) {
    if (!handle || handle == INVALID_HANDLE_VALUE) {
        return false;
    }
    SetLastError(NO_ERROR);
    return GetFileType(handle) != FILE_TYPE_UNKNOWN || GetLastError() == NO_ERROR;
}

void repairStream(FILE* stream, const char* name, const char* mode) {
    const auto descriptor = _fileno(stream);
    if (descriptor >= 0 && validHandle(reinterpret_cast<HANDLE>(_get_osfhandle(descriptor)))) {
        return;
    }
    FILE* reopened{};
    (void)freopen_s(&reopened, name, mode, stream);
}
#endif
}

struct ProcessConsole::Impl {
    std::string pending, line, prompt;
    bool closed{}, discard{}, interactive{}, promptShown{};
#if defined(_WIN32)
    HANDLE input{}, output{};
    bool consoleInput{}, consoleOutput{}, allocated{};
    KEY_EVENT_RECORD key{};
    WORD repeats{};
    wchar_t highSurrogate{};
#endif

    Impl() {
#if defined(_WIN32)
        if (!GetConsoleWindow() && !validHandle(GetStdHandle(STD_OUTPUT_HANDLE)) &&
            !validHandle(GetStdHandle(STD_ERROR_HANDLE))) {
            if (!AttachConsole(ATTACH_PARENT_PROCESS)) {
                allocated = AllocConsole() != FALSE;
            }
        }
        if (GetConsoleWindow()) {
            repairStream(stdin, "CONIN$", "r");
            repairStream(stdout, "CONOUT$", "w");
            repairStream(stderr, "CONOUT$", "w");
            std::cin.clear();
            std::cout.clear();
            std::cerr.clear();
        }
        input = GetStdHandle(STD_INPUT_HANDLE);
        output = GetStdHandle(STD_OUTPUT_HANDLE);
        DWORD mode{};
        consoleInput = GetConsoleMode(input, &mode) != FALSE;
        consoleOutput = GetConsoleMode(output, &mode) != FALSE;
        interactive = consoleInput && consoleOutput;
        closed = !validHandle(input);
#else
        interactive = isatty(STDIN_FILENO) && isatty(STDOUT_FILENO);
#endif
    }

    ~Impl() {
        if (promptShown) {
            write("\n");
        }
        std::fflush(stdout);
#if defined(_WIN32)
        if (allocated) {
            FreeConsole();
        }
#endif
    }

    void write(std::string_view text) {
#if defined(_WIN32)
        if (consoleOutput && !text.empty()) {
            const auto count = static_cast<int>(text.size());
            const auto length = MultiByteToWideChar(CP_UTF8, 0, text.data(), count, nullptr, 0);
            std::wstring wide(static_cast<std::size_t>(length), L'\0');
            (void)MultiByteToWideChar(CP_UTF8, 0, text.data(), count, wide.data(), length);
            DWORD written{};
            (void)WriteConsoleW(output, wide.data(), static_cast<DWORD>(wide.size()), &written, nullptr);
            return;
        }
#endif
        (void)std::fwrite(text.data(), 1, text.size(), stdout);
        std::fflush(stdout);
    }

    void clearPrompt() {
        if (promptShown) {
            write("\r" + std::string(prompt.size() + line.size(), ' ') + "\r");
        }
    }

    void displayPrompt() {
        if (interactive) {
            write(prompt + line);
            promptShown = true;
        }
    }

    void readInput() {
        std::array<char, 4096> bytes{};
#if defined(_WIN32)
        if (consoleInput) {
            // ReadConsoleInput bypasses the blocking cooked-line read. Keep
            // repeat counts across polls and let the main thread edit the line.
            for (std::size_t events = 0; events < bytes.size(); ++events) {
                if (!repeats) {
                    DWORD count{};
                    if (!GetNumberOfConsoleInputEvents(input, &count) || !count) {
                        break;
                    }
                    INPUT_RECORD record{};
                    if (!ReadConsoleInputW(input, &record, 1, &count) || !count) {
                        closed = true;
                        break;
                    }
                    if (record.EventType != KEY_EVENT || !record.Event.KeyEvent.bKeyDown) {
                        continue;
                    }
                    key = record.Event.KeyEvent;
                    repeats = key.wRepeatCount;
                    if (!repeats) {
                        continue;
                    }
                }
                --repeats;
                const auto character = key.uChar.UnicodeChar;
                if (character == L'\r') {
                    pending += '\n';
                    highSurrogate = 0;
                } else if (character == L'\b') {
                    pending += '\b';
                    highSurrogate = 0;
                } else if (character >= 0xd800 && character <= 0xdbff) {
                    highSurrogate = character;
                } else if (character >= L' ' || character == L'\t') {
                    const std::array wide{highSurrogate, character};
                    const bool pair = highSurrogate && character >= 0xdc00 && character <= 0xdfff;
                    std::array<char, 4> encoded{};
                    const int count = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
                                                         pair ? wide.data() : &character,
                                                         pair ? 2 : 1, encoded.data(),
                                                         static_cast<int>(encoded.size()), nullptr, nullptr);
                    highSurrogate = 0;
                    if (count > 0) {
                        pending.append(encoded.data(), static_cast<std::size_t>(count));
                    }
                }
            }
            return;
        }
        DWORD count{};
        if (GetFileType(input) == FILE_TYPE_PIPE) {
            DWORD available{};
            if (!PeekNamedPipe(input, nullptr, 0, nullptr, &available, nullptr)) {
                closed = true;
                return;
            }
            if (!available) {
                return;
            }
        }
        if (!ReadFile(input, bytes.data(), static_cast<DWORD>(bytes.size()), &count, nullptr) || !count) {
            closed = true;
            return;
        }
        pending.append(bytes.data(), count);
#else
        pollfd descriptor{STDIN_FILENO, POLLIN, 0};
        const auto ready = ::poll(&descriptor, 1, 0);
        if (ready <= 0) {
            return;
        }
        if (!(descriptor.revents & (POLLIN | POLLHUP))) {
            closed = true;
            return;
        }
        const auto count = ::read(STDIN_FILENO, bytes.data(), bytes.size());
        if (count > 0) {
            pending.append(bytes.data(), static_cast<std::size_t>(count));
        } else if (count == 0 || (errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK)) {
            closed = true;
        }
#endif
    }
};

ProcessConsole::ProcessConsole() : impl_(std::make_unique<Impl>()) {}
ProcessConsole::~ProcessConsole() = default;

void ProcessConsole::writeLine(std::string_view text) {
    impl_->clearPrompt();
    impl_->write(text);
    impl_->write("\n");
    if (impl_->promptShown) {
        impl_->displayPrompt();
    }
}

void ProcessConsole::showPrompt(std::string_view prompt) {
    impl_->clearPrompt();
    impl_->prompt = prompt.empty() ? "> " : std::string(prompt);
    impl_->displayPrompt();
}

std::vector<std::string> ProcessConsole::poll() {
    std::vector<std::string> commands;
    if (impl_->pending.empty() && !impl_->closed) {
        impl_->readInput();
    }
    std::size_t consumed{};
    const auto finishLine = [&] {
        if (!impl_->discard && impl_->line.find_first_not_of(" \t\r") != std::string::npos) {
            commands.push_back(std::move(impl_->line));
        }
        impl_->line.clear();
        impl_->discard = false;
    };
    for (const char character : impl_->pending) {
        ++consumed;
        if (character == '\n') {
#if defined(_WIN32)
            if (impl_->interactive) {
                impl_->write("\r\n");
            }
#endif
            impl_->promptShown = false;
            finishLine();
            if (commands.size() == MaxCommands) {
                break;
            }
        } else if (character == '\r') {
            continue;
#if defined(_WIN32)
        } else if (impl_->consoleInput && character == '\b') {
            if (!impl_->line.empty()) {
                eraseCharacter(impl_->line);
                if (impl_->interactive) {
                    impl_->write("\b \b");
                }
            }
#endif
        } else if (!impl_->discard) {
            if (character == '\0' || impl_->line.size() == MaxLine) {
                impl_->discard = true;
                writeLine("stdin: discarded command containing NUL or exceeding 65536 bytes");
                impl_->line.clear();
            } else {
                impl_->line += character;
#if defined(_WIN32)
                if (impl_->interactive) {
                    // Echo a complete UTF-8 sequence through the console writer.
                    if ((static_cast<unsigned char>(character) & 0x80) == 0) {
                        impl_->write(std::string_view(&character, 1));
                    } else if (consumed == impl_->pending.size() ||
                               (static_cast<unsigned char>(impl_->pending[consumed]) & 0xc0) != 0x80) {
                        auto start = impl_->line.size() - 1;
                        while (start && (static_cast<unsigned char>(impl_->line[start]) & 0xc0) == 0x80) {
                            --start;
                        }
                        impl_->write(std::string_view(impl_->line).substr(start));
                    }
                }
#endif
            }
        }
    }
    impl_->pending.erase(0, consumed);
    if (impl_->closed && impl_->pending.empty() && commands.size() < MaxCommands) {
        finishLine();
    }
    if (impl_->interactive && !impl_->promptShown && commands.empty()) {
        impl_->displayPrompt();
    }
    return commands;
}
}
