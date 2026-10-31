#include <opent1/gui/native_object_hosts.hpp>
#include <opent1/sim/sim_gpu_canvas.hpp>
#include <opent1/core/file_stream.hpp>
#include <charconv>
#include <cmath>
#include <SDL3/SDL_video.h>
#include <SDL3/SDL_misc.h>
#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#endif
namespace opent1::gui {
namespace {
#ifdef _WIN32
std::wstring wide(std::string_view value) {
    if (value.empty()) {
        return {};
    }
    if (value.size() > 65536) {
        throw std::length_error("native text");
    }
    const auto size = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), Int32(value.size()), nullptr, 0);
    if (!size) {
        throw std::invalid_argument("native UTF-8");
    }
    std::wstring result(std::size_t(size), L'\0');
    MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), Int32(value.size()), result.data(), size);
    return result;
}

std::string utf8(std::wstring_view value) {
    if (value.empty()) {
        return {};
    }
    const auto size = WideCharToMultiByte(CP_UTF8,
                                          WC_ERR_INVALID_CHARS,
                                          value.data(),
                                          Int32(value.size()),
                                          nullptr,
                                          0,
                                          nullptr,
                                          nullptr);
    if (!size) {
        throw std::invalid_argument("native UTF-16");
    }
    std::string result(std::size_t(size), '\0');
    WideCharToMultiByte(CP_UTF8,
                        WC_ERR_INVALID_CHARS,
                        value.data(),
                        Int32(value.size()),
                        result.data(),
                        size,
                        nullptr,
                        nullptr);
    return result;
}

bool launch(std::string_view file, std::string_view parameters) {
    const auto path = wide(file), args = wide(parameters);
    return reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr,
                                                   L"open",
                                                   path.c_str(),
                                                   args.empty() ? nullptr : args.c_str(),
                                                   nullptr,
                                                   SW_SHOWNORMAL)) > 32;
}

struct NativeConsole {
    HANDLE input{INVALID_HANDLE_VALUE}, output{INVALID_HANDLE_VALUE};
    bool owned{}, enabled{};
    std::wstring pending;
    std::vector<std::wstring> history;
    std::size_t cursor{}, historyIndex{};

    ~NativeConsole() {
        stop();
    }

    void stop() {
        enabled = false;
        input = output = INVALID_HANDLE_VALUE;
        if (owned) {
            FreeConsole();
            owned = false;
        }
        pending.clear();
        history.clear();
        cursor = historyIndex = 0;
    }

    bool enable(bool value, std::string_view title) {
        if (!value) {
            stop();
            return true;
        }
        if (enabled) {
            return true;
        }
        if (!GetConsoleWindow()) {
            if (!AllocConsole()) {
                return false;
            }
            owned = true;
        }
        input = GetStdHandle(STD_INPUT_HANDLE);
        output = GetStdHandle(STD_OUTPUT_HANDLE);
        if (input == INVALID_HANDLE_VALUE || output == INVALID_HANDLE_VALUE) {
            stop();
            return false;
        }
        if (!title.empty() && !SetConsoleTitleW(wide(title).c_str())) {
            stop();
            return false;
        }
        enabled = true;
        return true;
    }

    void write(std::string_view text) {
        if (!enabled) {
            return;
        }
        const auto value = wide(text);
        DWORD written{};
        if (!WriteConsoleW(output, value.data(), DWORD(value.size()), &written, nullptr) ||
            written != value.size()) {
            throw std::runtime_error("console output");
        }
        static constexpr wchar_t newline[] = L"\r\n";
        WriteConsoleW(output, newline, 2, &written, nullptr);
    }

    std::vector<std::string> read() {
        std::vector<std::string> lines;
        if (!enabled) {
            return lines;
        }
        DWORD available{};
        if (!GetNumberOfConsoleInputEvents(input, &available)) {
            throw std::runtime_error("console input");
        }
        std::array<INPUT_RECORD, 256> records;
        DWORD count{};
        if (available &&
            !ReadConsoleInputW(
                input, records.data(), std::min<DWORD>(available, DWORD(records.size())), &count)) {
            throw std::runtime_error("console input");
        }
        for (DWORD i = 0; i < count && lines.size() < 64; ++i) {
            if (records[i].EventType != KEY_EVENT || !records[i].Event.KeyEvent.bKeyDown) {
                continue;
            }
            const auto& key = records[i].Event.KeyEvent;
            if (key.wVirtualKeyCode == VK_RETURN) {
                auto line = utf8(pending);
                if (line.size() > 65536) {
                    throw std::length_error("console line");
                }
                lines.push_back(std::move(line));
                if (!pending.empty()) {
                    history.push_back(pending);
                    if (history.size() > 20) {
                        history.erase(history.begin());
                    }
                }
                pending.clear();
                cursor = 0;
                historyIndex = history.size();
            } else if (key.wVirtualKeyCode == VK_UP) {
                if (historyIndex) {
                    pending = history[--historyIndex];
                    cursor = pending.size();
                }
            } else if (key.wVirtualKeyCode == VK_DOWN) {
                if (historyIndex < history.size()) {
                    ++historyIndex;
                    pending = historyIndex < history.size() ? history[historyIndex] : L"";
                    cursor = pending.size();
                }
            } else if (key.wVirtualKeyCode == VK_HOME) {
                cursor = 0;
            } else if (key.wVirtualKeyCode == VK_END) {
                cursor = pending.size();
            } else if (key.wVirtualKeyCode == VK_LEFT) {
                if (cursor) {
                    --cursor;
                    if (cursor && pending[cursor] >= 0xdc00 && pending[cursor] <= 0xdfff) {
                        --cursor;
                    }
                }
            } else if (key.wVirtualKeyCode == VK_RIGHT) {
                if (cursor < pending.size()) {
                    if (pending[cursor] >= 0xd800 && pending[cursor] <= 0xdbff &&
                        cursor + 1 < pending.size()) {
                        ++cursor;
                    }
                    ++cursor;
                }
            } else if (key.wVirtualKeyCode == VK_BACK) {
                if (cursor) {
                    const auto end = cursor;
                    --cursor;
                    if (cursor && pending[cursor] >= 0xdc00 && pending[cursor] <= 0xdfff) {
                        --cursor;
                    }
                    pending.erase(cursor, end - cursor);
                }
            } else if (key.wVirtualKeyCode == VK_DELETE) {
                if (cursor < pending.size()) {
                    const auto size = pending[cursor] >= 0xd800 && pending[cursor] <= 0xdbff &&
                                              cursor + 1 < pending.size()
                                          ? 2
                                          : 1;
                    pending.erase(cursor, size);
                }
            } else if (key.uChar.UnicodeChar >= 32) {
                const auto repeats = std::min<UInt32>(key.wRepeatCount, 256);
                if (pending.size() + repeats > 65536) {
                    throw std::length_error("console line");
                }
                pending.insert(cursor, repeats, key.uChar.UnicodeChar);
                cursor += repeats;
            }
        }
        return lines;
    }
};
#else
bool launch(std::string_view file, std::string_view parameters) {
    return parameters.empty() && (file.starts_with("http://") || file.starts_with("https://")) &&
           SDL_OpenURL(std::string(file).c_str());
}
#endif
}

world::ObjectHost nativeShellHost(platform::SdlApplication* application, ShellLauncher launcher) {
    if (!launcher) {
        launcher = launch;
    }
    world::ObjectHost host;
    host.command = [application, launcher = std::move(launcher)](
                       Console&,
                       sim::SimManager* manager,
                       std::string_view name,
                       std::span<const std::string> args) -> std::optional<std::string> {
        const bool goWindowed = name == "shellOpenAndGoWin" || name == "HTMLOpenAndGoWin";
        const std::size_t first = goWindowed ? 1 : 0;
        platform::SdlWindow* window = nullptr;
        bool fullscreen = false;
        if (goWindowed) {
            if (!manager || !application) {
                return "False";
            }
            auto* canvas = dynamic_cast<sim::SdlSimCanvas*>(manager->findObject(args[0]));
            if (!canvas || (window = application->findWindow(canvas->windowId())) == nullptr) {
                return "False";
            }
            fullscreen = window->state().fullscreen;
            window->setFullscreen(false);
        }
        bool success = false;
        try {
            success = launcher(args[first],
                               args.size() > first + 1 ? std::string_view(args[first + 1])
                                                       : std::string_view{});
        } catch (...) {
            if (window && fullscreen) {
                window->setFullscreen(true);
            }
            throw;
        }
        if (!success && window && fullscreen) {
            window->setFullscreen(true);
        }
        return success ? "True" : "False";
    };
    return host;
}

world::ObjectHost nativeConsoleHost() {
    world::ObjectHost host;
#ifdef _WIN32
    auto state = std::make_shared<NativeConsole>();
    host.consoleEnabled = [state](bool value, auto title) {
        return state->enable(value, title);
    };
    host.output = [state](auto value) {
        state->write(value);
    };
    host.input = [state] {
        return state->read();
    };
    host.stop = [state] {
        state->stop();
    };
#else
    host.consoleEnabled = [](bool value, auto) {
        return !value;
    };
#endif
    return host;
}

world::ObjectHost nativeGraphicsHost(platform::SdlApplication& application,
                                     ScreenshotWriter screenshot) {
    struct State {
        UInt32 sequence{};
    };

    auto state = std::make_shared<State>();
    if (!screenshot) {
        screenshot = [](std::string_view name, const gfx::Bitmap& bitmap) {
            FileWStream stream{std::filesystem::path(name)};
            return stream.getStatus() == STRM_OK &&
                   bitmap.write(stream, gfx::BitmapFormat::Microsoft) && stream.flush();
        };
    }
    world::ObjectHost host;
    host.command = [&application, state, screenshot = std::move(screenshot)](
                       Console& console,
                       sim::SimManager* manager,
                       std::string_view name,
                       std::span<const std::string> args) -> std::optional<std::string> {
        const auto yes = [](bool value) -> std::optional<std::string> {
            return value ? "True" : "False";
        };
        const auto number = [](std::string_view value, auto& result) {
            const auto r = std::from_chars(value.data(), value.data() + value.size(), result);
            return r.ec == std::errc{} && r.ptr == value.data() + value.size();
        };
        const auto drivers = gfx::gpuDrivers();
        if (name == "isGfxDriver") {
            if (args.empty()) {
                return yes(!drivers.empty());
            }
            if (!manager || args.size() < 2) {
                return "False";
            }
            auto* canvas = dynamic_cast<sim::SdlSimCanvas*>(manager->findObject(args[0]));
            auto* window = canvas ? application.findWindow(canvas->windowId()) : nullptr;
            auto* gpu = dynamic_cast<sim::SdlGpuCanvas*>(canvas);
            auto* surface = gpu ? gpu->surface() : nullptr;
            return yes(window && surface &&
                       (surface->driver() == args[1] || args[1] == "Software"));
        }
        if (name == "listDevices") {
            for (const auto& driver : drivers) {
                console.print(driver);
            }
            return "True";
        }
        if (name == "testDevice") {
            return yes(std::ranges::find(drivers, args[0]) != drivers.end());
        }
        if (name == "setScreenShotSeq") {
            UInt32 sequence{};
            if (!number(args[0], sequence)) {
                return "False";
            }
            state->sequence = sequence;
            return "True";
        }
        if (!manager) {
            return "False";
        }
        if (name == "flushTextureCache" && args.empty()) {
            auto* set = dynamic_cast<sim::SimSet*>(manager->findObject(sim::CanvasSetId));
            if (!set) {
                return "False";
            }
            for (auto* object : set->objects()) {
                if (auto* canvas = dynamic_cast<sim::SdlGpuCanvas*>(object);
                    canvas && canvas->surface()) {
                    canvas->surface()->clearTextureCache();
                }
            }
            return "True";
        }
        auto* canvas = dynamic_cast<sim::SdlSimCanvas*>(manager->findObject(args[0]));
        auto* window = canvas ? application.findWindow(canvas->windowId()) : nullptr;
        if (!window) {
            return "False";
        }
        auto* gpu = dynamic_cast<sim::SdlGpuCanvas*>(canvas);
        auto* surface = gpu ? gpu->surface() : nullptr;
        if (name == "isFullscreenMode") {
            return yes(window->state().fullscreen);
        }
        if (name == "isVirtualFS") {
            return yes(window->state().fullscreen &&
                       SDL_GetWindowFullscreenMode(window->nativeHandle()) == nullptr);
        }
        if (name == "setFullscreenMode") {
            if (args[1] != "True" && args[1] != "true" && args[1] != "1" && args[1] != "False" &&
                args[1] != "false" && args[1] != "0") {
                return "False";
            }
            window->setFullscreen(args[1] == "True" || args[1] == "true" || args[1] == "1");
            return "True";
        }
        if (name == "setWindowSize") {
            Int32 width{}, height{};
            if (!number(args[1], width) || !number(args[2], height) || width < 20 || height < 20 ||
                width > 16384 || height > 16384) {
                return "False";
            }
            window->setClientSize(width, height);
            return "True";
        }
        if (name == "lockWindowSize" || name == "unlockWindowSize") {
            window->setSizeLocked(name == "lockWindowSize");
            return "True";
        }
        if (name == "setGamma") {
            float gamma{};
            return yes(surface && number(args[1], gamma) && std::isfinite(gamma) &&
                       gpu->setGamma(gamma));
        }
        if (name == "flushTextureCache") {
            if (!surface) {
                return "False";
            }
            surface->clearTextureCache();
            return "True";
        }
        if (name == "swapSurfaces") {
            return yes(surface && surface->presentFrame() != gfx::PresentResult::Error);
        }
        if (name == "resetUpdateRegion") {
            canvas->renderFrame();
            return "True";
        }
        if (name == "screenShot") {
            if (!surface) {
                return "False";
            }
            const auto& source = surface->software().bitmap();
            std::vector<std::byte> rgba(std::size_t(source.width) * source.height * 4);
            if (!surface->readback(rgba)) {
                return "False";
            }
            gfx::Bitmap bitmap;
            bitmap.width = source.width;
            bitmap.height = source.height;
            bitmap.bitDepth = 24;
            bitmap.pixels.resize(std::size_t(bitmap.stride()) * bitmap.height);
            for (Int32 y = 0; y < bitmap.height; ++y) {
                for (Int32 x = 0; x < bitmap.width; ++x) {
                    const auto from = (std::size_t(y) * bitmap.width + x) * 4,
                               to = std::size_t(y) * bitmap.stride() + std::size_t(x) * 3;
                    bitmap.pixels[to] = rgba[from + 2];
                    bitmap.pixels[to + 1] = rgba[from + 1];
                    bitmap.pixels[to + 2] = rgba[from];
                }
            }
            const auto path =
                args.size() == 2 ? args[1] : std::format("temp/sshot{:04}.bmp", state->sequence);
            if (!screenshot(path, bitmap)) {
                return "False";
            }
            if (args.size() == 1) {
                ++state->sequence;
            }
            return "True";
        }
        if (name == "setWindowedDevice" || name == "setFullscreenDevice") {
            if (!surface || (surface->driver() != args[1] && args[1] != "Software")) {
                return "False";
            }
            window->setFullscreen(name == "setFullscreenDevice");
            return "True";
        }
        if (name == "messageCanvasDevice") {
            if (!surface || args.size() < 2) {
                return "False";
            }
            if (args[1] == "flushCache") {
                surface->clearTextureCache();
                return "True";
            }
            if (args[1] == "setGamma" && args.size() == 3) {
                float gamma{};
                return yes(number(args[2], gamma) && std::isfinite(gamma) && gpu->setGamma(gamma));
            }
            return "False";
        }
        if (name == "setFSResolution" || name == "nextRes" || name == "prevRes") {
            int count{};
            auto** modes = SDL_GetFullscreenDisplayModes(
                SDL_GetDisplayForWindow(window->nativeHandle()), &count);
            if (!modes || count < 1) {
                SDL_free(modes);
                return "False";
            }
            const auto deleter = [](SDL_DisplayMode** p) {
                SDL_free(p);
            };
            std::unique_ptr<SDL_DisplayMode*, decltype(deleter)> owner(modes, deleter);
            std::vector<const SDL_DisplayMode*> sorted(modes, modes + count);
            std::stable_sort(sorted.begin(), sorted.end(), [](auto* a, auto* b) {
                return a->w < b->w || (a->w == b->w && a->h < b->h);
            });
            sorted.erase(std::unique(sorted.begin(),
                                     sorted.end(),
                                     [](auto* a, auto* b) {
                                         return a->w == b->w && a->h == b->h;
                                     }),
                         sorted.end());
            const SDL_DisplayMode* selected = nullptr;
            if (name == "setFSResolution") {
                const auto separator = args[1].find_first_of("xX");
                Int32 width{}, height{};
                if (separator == args[1].npos ||
                    !number(std::string_view(args[1]).substr(0, separator), width) ||
                    !number(std::string_view(args[1]).substr(separator + 1), height)) {
                    return "False";
                }
                for (auto* mode : sorted) {
                    if (mode->w == width && mode->h == height) {
                        selected = mode;
                        break;
                    }
                }
            } else {
                const auto size = window->state();
                auto it = std::ranges::find_if(sorted, [&](auto* mode) {
                    return mode->w >= size.width && mode->h >= size.height;
                });
                auto index =
                    it == sorted.end() ? sorted.size() - 1 : std::size_t(it - sorted.begin());
                index = name == "nextRes" ? (index + 1) % sorted.size()
                                          : (index + sorted.size() - 1) % sorted.size();
                selected = sorted[index];
            }
            return yes(selected && SDL_SetWindowFullscreenMode(window->nativeHandle(), selected));
        }
        // Legacy hardware-driver messages have no generic SDL_GPU equivalent.
        // Unsupported requests report failure without changing a live device.
        return "False";
    };
    return host;
}
} // namespace opent1::gui
