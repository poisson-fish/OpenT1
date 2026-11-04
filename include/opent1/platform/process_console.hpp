#pragma once

#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace opent1::platform {
// Standalone-process stdio. Poll on the simulation thread; no blocking reader
// or worker thread survives shutdown. Existing stream redirection is preserved.
class ProcessConsole {
public:
    ProcessConsole();
    ~ProcessConsole();
    ProcessConsole(const ProcessConsole&) = delete;
    ProcessConsole& operator=(const ProcessConsole&) = delete;

    void writeLine(std::string_view text);
    void showPrompt(std::string_view prompt);
    [[nodiscard]] std::vector<std::string> poll();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
