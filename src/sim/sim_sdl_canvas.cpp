#include <opent1/sim/sim_sdl_canvas.hpp>
#include <algorithm>
#include <charconv>
#include <utility>

namespace opent1::sim {
namespace {
bool integer(std::string_view text, Int32& value) {
    const auto r = std::from_chars(text.data(), text.data() + text.size(), value);
    return r.ec == std::errc{} && r.ptr == text.data() + text.size();
}

bool point(std::string_view text, Int32& x, Int32& y) {
    const auto split = text.find(' ');
    if (split == text.npos) {
        return false;
    }
    auto tail = text.substr(split);
    const auto start = tail.find_first_not_of(' ');
    return start != tail.npos && integer(text.substr(0, split), x) &&
           integer(tail.substr(start), y);
}
}

SdlSimCanvas::SdlSimCanvas(platform::SdlApplication& application,
                           Draw draw,
                           bool hidden,
                           PaletteChange paletteChange)
    : application_(application), draw_(std::move(draw)), paletteChange_(std::move(paletteChange)),
      hidden_(hidden) {}

SdlSimCanvas::~SdlSimCanvas() {
    try {
        onRemove();
    } catch (...) {
    }
}

bool SdlSimCanvas::processArguments(std::span<const std::string> args) {
    if (window_ || args.empty() || args.size() > 6 || args.size() == 5 || args[0].size() > 4096 ||
        args[0].find('\0') != args[0].npos) {
        return false;
    }
    platform::WindowConfig config;
    config.title = args[0];
    config.hidden = hidden_;
    if ((args.size() > 1 && !integer(args[1], config.width)) ||
        (args.size() > 2 && !integer(args[2], config.height))) {
        return false;
    }
    if (args.size() > 3 && !args[3].empty()) {
        const auto c = args[3].front();
        config.quitOnDestroy = c == 't' || c == 'T' || c == '1';
    }
    if (args.size() == 6 && (!point(args[4], config.minWidth, config.minHeight) ||
                             !point(args[5], config.maxWidth, config.maxHeight))) {
        return false;
    }
    // Validate before creating a native window; malformed script arguments have
    // no window/quit side effects. Native failures use normal engine diagnostics.
    if (config.width <= 0 || config.height <= 0 || config.minWidth <= 0 || config.minHeight <= 0 ||
        config.maxWidth < 0 || config.maxHeight < 0 ||
        (config.maxWidth && config.maxWidth < config.minWidth) ||
        (config.maxHeight && config.maxHeight < config.minHeight)) {
        return false;
    }
    window_ = application_.createWindow(config).id();
    paletteChanged();
    return !isRemoved();
}

bool SdlSimCanvas::setTitle(std::string_view title) {
    if (title.size() > 4096 || title.find('\0') != title.npos) {
        return false;
    }
    auto* window = application_.findWindow(window_);
    if (!window) {
        return false;
    }
    window->setTitle(title);
    return true;
}

void SdlSimCanvas::renderFrame() {
    auto* window = application_.findWindow(window_);
    if (!window) {
        if (getManager()) {
            deleteObject();
        }
        return;
    }
    const auto draw = draw_;
    if (draw) {
        draw(*window);
    } // callback may remove this canvas
}

void SdlSimCanvas::paletteChanged() {
    auto* window = application_.findWindow(window_);
    const auto callback = paletteChange_;
    if (window && callback) {
        callback(*window, getPalette(), paletteTransitionColor());
    }
}

void SdlSimCanvas::onRemove() {
    SimCanvas::onRemove();
    const auto window = std::exchange(window_, 0);
    if (window && application_.findWindow(window)) {
        application_.destroyWindow(window);
    }
}

bool installSdlCanvasFactory(SimGame& game,
                             platform::SdlApplication& application,
                             SdlSimCanvas::Draw draw,
                             bool hidden,
                             SdlSimCanvas::PaletteChange paletteChange) {
    return game.registerFactory(
        "SimCanvas",
        [&application, draw = std::move(draw), hidden, paletteChange = std::move(paletteChange)] {
            return std::make_unique<SdlSimCanvas>(application, draw, hidden, paletteChange);
        });
}
}
