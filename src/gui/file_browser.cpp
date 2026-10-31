#include <opent1/gui/file_browser.hpp>
#include <cstdio>
#include <set>

namespace opent1::gui {
namespace {
std::string pathText(const std::filesystem::path& path) {
    const auto s = path.generic_u8string();
    return {reinterpret_cast<const char*>(s.data()), s.size()};
}

std::filesystem::path pathFrom(std::string_view s) {
    return std::filesystem::path(
        std::u8string_view(reinterpret_cast<const char8_t*>(s.data()), s.size()));
}

bool validText(std::string_view s, std::size_t limit) {
    TextEdit validator;
    validator.maxBytes = limit;
    return validator.setText(std::string(s));
}

char lower(char c) {
    return c >= 'A' && c <= 'Z' ? char(c - 'A' + 'a') : c;
}

std::size_t nextCharacter(std::string_view text, std::size_t at) {
    if (at < text.size()) {
        ++at;
        while (at < text.size() && (static_cast<unsigned char>(text[at]) & 0xc0) == 0x80) {
            ++at;
        }
    }
    return at;
}

bool wildcard(std::string_view name, std::string_view pattern) {
    if (pattern == "*.*") {
        return true;
    }
    std::size_t n{}, p{}, star = pattern.npos, retry{};
    while (n < name.size()) {
        if (p < pattern.size() && pattern[p] == '?') {
            n = nextCharacter(name, n);
            ++p;
        } else if (p < pattern.size() && lower(pattern[p]) == lower(name[n])) {
            ++n;
            ++p;
        } else if (p < pattern.size() && pattern[p] == '*') {
            star = p++;
            retry = n;
        } else if (star != pattern.npos) {
            p = star + 1;
            retry = nextCharacter(name, retry);
            n = retry;
        } else {
            return false;
        }
    }
    while (p < pattern.size() && pattern[p] == '*') {
        ++p;
    }
    return p == pattern.size();
}

bool matches(std::string_view name, std::string_view patterns) {
    while (!patterns.empty()) {
        const auto end = patterns.find(';');
        if (wildcard(name, patterns.substr(0, end))) {
            return true;
        }
        if (end == patterns.npos) {
            break;
        }
        patterns.remove_prefix(end + 1);
    }
    return false;
}

bool validPatterns(std::string_view patterns) {
    if (patterns.empty() || !validText(patterns, 4096) ||
        patterns.find_first_of("/\\:") != patterns.npos) {
        return false;
    }
    return patterns.front() != ';' && patterns.back() != ';' &&
           patterns.find(";;") == patterns.npos;
}

std::optional<std::vector<FileBrowserEntry>> diskDirectory(const std::filesystem::path& path) {
    std::error_code ec;
    std::filesystem::directory_iterator it(path, ec), end;
    if (ec) {
        return {};
    }
    std::vector<FileBrowserEntry> entries;
    for (; it != end; it.increment(ec)) {
        if (ec || entries.size() >= 4096) {
            return {};
        }
        const bool dir = it->is_directory(ec);
        if (ec) {
            return {};
        }
        if (dir || it->is_regular_file(ec)) {
            entries.push_back({pathText(it->path().filename()), dir});
        }
        if (ec) {
            return {};
        }
    }
    if (ec) {
        return {};
    }
    return entries;
}

FileBrowser::Kind diskProbe(const std::filesystem::path& path, bool saving) {
    std::error_code ec;
    const auto status = std::filesystem::status(path, ec);
    if (ec == std::errc::no_such_file_or_directory) {
        return FileBrowser::Kind::Missing;
    }
    if (ec) {
        return FileBrowser::Kind::Refused;
    }
    if (!std::filesystem::exists(status)) {
        return FileBrowser::Kind::Missing;
    }
    if (std::filesystem::is_directory(status)) {
        return FileBrowser::Kind::Directory;
    }
    if (!std::filesystem::is_regular_file(status)) {
        return FileBrowser::Kind::Refused;
    }
    // No create-on-missing fallback: a race with deletion must never create a
    // chooser target, and probing a save target must never truncate its bytes.
#ifdef _WIN32
    auto* file = _wfopen(path.c_str(), saving ? L"r+b" : L"rb");
#else
    auto* file = std::fopen(path.c_str(), saving ? "r+b" : "rb");
#endif
    if (!file) {
        return FileBrowser::Kind::Refused;
    }
    std::fclose(file);
    return FileBrowser::Kind::File;
}

class BrowserRoot final : public Control {
public:
    std::function<void(bool)> finish;

    bool event(const Event& event) override {
        if (event.type == EventType::KeyDown &&
            (event.key == Key::Escape || event.key == Key::Return)) {
            const bool accept = event.key == Key::Return;
            if (canvas()) {
                canvas()->afterDispatch([fn = finish, accept] {
                    if (fn) {
                        fn(accept);
                    }
                });
            }
            return true;
        }
        return Control::event(event);
    }
};
}

struct FileBrowser::State {
    struct Panel {
        BrowserRoot root;
        Scroll scroll;
        TextList list;
        TextEdit filename;
        Text title;
        ComboBox filters;
        std::array<Button, 3> buttons;
        FileBrowserRequest request;
        std::size_t selectedFilter{};
        std::string patterns;
        std::vector<FileBrowserEntry> entries;
        std::vector<std::string> names;
    };

    Control root;
    Canvas canvas;
    std::shared_ptr<Panel> panel;
    std::optional<Result> result;
    std::shared_ptr<const gfx::BitmapFont> font;
    bool busy{}, stopping{};
    UInt64 epoch{};

    State() {
        if (!root.resize({}, {640, 480}) || !canvas.content(&root)) {
            throw std::runtime_error("file browser canvas");
        }
    }

    ~State() {
        canvas.captureMouse = {};
        canvas.content(nullptr);
        panel.reset();
    }

    void retire() {
        if (!stopping || busy || canvas.dispatching()) {
            return;
        }
        canvas.captureMouse = {};
        if (!canvas.content(nullptr)) {
            return;
        }
        panel.reset();
        result.reset();
        canvas.content(&root);
        stopping = false;
    }

    struct Operation {
        State& state;

        explicit Operation(State& s) : state(s) {
            state.busy = true;
        }

        ~Operation() {
            state.busy = false;
            state.retire();
        }
    };
};

FileBrowser::FileBrowser() : state_(std::make_shared<State>()) {}

FileBrowser::~FileBrowser() {
    state_->canvas.onAction = {};
    state_->canvas.keyCommand = {};
    state_->canvas.captureMouse = {};
    state_->canvas.content(nullptr);
    state_->panel.reset();
}

Canvas& FileBrowser::canvas() {
    return state_->canvas;
}

ModalView FileBrowser::view() {
    return {[this]() -> Canvas& {
                return canvas();
            },
            [this] {
                return active();
            },
            [] {
                return true;
            },
            [this] {
                return cancel();
            }};
}

bool FileBrowser::active() const {
    return bool(state_->panel);
}

std::vector<std::string> FileBrowser::items() const {
    return active() ? state_->panel->names : std::vector<std::string>{};
}

std::optional<FileBrowser::Result> FileBrowser::result() const {
    return state_->result;
}

void FileBrowser::font(std::shared_ptr<const gfx::BitmapFont> font) {
    state_->font = std::move(font);
    if (auto p = state_->panel) {
        p->title.font = p->filename.font = p->filename.highlightFont = p->list.font =
            p->list.highlightFont = p->filters.font = p->filters.highlightFont = state_->font;
        p->filters.list().font = p->filters.list().highlightFont = state_->font;
        for (auto& b : p->buttons) {
            b.font = b.highlightFont = b.disabledFont = state_->font;
        }
    }
}

bool FileBrowser::open(FileBrowserRequest request) {
    auto s = state_;
    if (s->busy || active() || request.filters.empty() || request.filters.size() > 31 ||
        !request.maxPathBytes || request.maxPathBytes > 4096 ||
        !validText(request.defaultExtension, 255) ||
        request.defaultExtension.find_first_of("./\\:*?;") != request.defaultExtension.npos) {
        return false;
    }
    const auto epoch = s->epoch;
    for (const auto& filter : request.filters) {
        if (filter.label.empty() || !validText(filter.label, 255) ||
            !validPatterns(filter.pattern)) {
            return false;
        }
    }
    try {
        if (request.directory.empty()) {
            request.directory = std::filesystem::current_path();
        }
        if (!readDirectory) {
            request.directory = std::filesystem::absolute(request.directory);
        }
        request.directory = request.directory.lexically_normal();
        auto p = std::make_shared<State::Panel>();
        p->request = std::move(request);
        p->patterns = p->request.filters.front().pattern;
        p->filename.maxBytes = 4096;
        p->list.noDuplicates = false;
        p->list.cellSize = {530, 24};
        p->list.message = 4;
        p->scroll.horizontalPolicy = Scroll::Dynamic;
        p->scroll.verticalPolicy = Scroll::Dynamic;
        auto fields = p->root.fields();
        fields.style.opaque = 1;
        fields.style.fill = 249;
        fields.style.border = 1;
        fields.style.borderColor = 255;
        if (!p->root.configure(fields) || !p->root.resize({16, 16}, {608, 448}) ||
            !p->title.resize({8, 4}, {592, 24}) || !p->root.addObject(p->title) ||
            !p->scroll.resize({8, 34}, {592, 280}) || !p->root.addObject(p->scroll) ||
            !p->scroll.addObject(p->list) || !p->filename.resize({8, 320}, {592, 24}) ||
            !p->root.addObject(p->filename) || !p->filters.resize({8, 350}, {592, 24}) ||
            !p->root.addObject(p->filters)) {
            return false;
        }
        for (std::size_t i = 0; i < p->request.filters.size(); ++i) {
            if (!p->filters.list().insert({Int32(i), p->request.filters[i].label})) {
                return false;
            }
        }
        p->filters.text = p->request.filters.front().label;
        p->filters.message = 5;
        for (std::size_t i = 0; i < p->buttons.size(); ++i) {
            auto& b = p->buttons[i];
            b.text = std::array{"OK", "Cancel", "Up"}[i];
            b.message = UInt32(i + 1);
            if (!b.resize({8 + Int32(i) * 100, 408}, {92, 24}) || !p->root.addObject(b)) {
                return false;
            }
        }
        s->panel = p;
        const auto oldResult = s->result;
        if (!browse(p->request.directory) || epoch != s->epoch || s->panel != p) {
            s->panel.reset();
            return false;
        }
        font(s->font);
        p->root.finish = [this](bool ok) {
            if (ok) {
                accept();
            } else {
                cancel();
            }
        };
        s->canvas.onAction = [this](Control&, UInt32 message, std::string_view) {
            auto p = state_->panel;
            if (!p) {
                return false;
            }
            if (message == 4) {
                return p->list.selected.y >= 0 && select(std::size_t(p->list.selected.y));
            }
            state_->canvas.afterDispatch([this, message] {
                if (message == 1) {
                    accept();
                } else if (message == 2) {
                    cancel();
                } else if (message == 3 && active()) {
                    browse(state_->panel->request.directory.parent_path());
                } else if (message == 5 && active()) {
                    const auto* choice = state_->panel->filters.list().selection();
                    if (choice) {
                        filter(std::size_t(choice->tag));
                    }
                }
            });
            return true;
        };
        s->canvas.keyCommand = [this](const Event& event) {
            auto p = state_->panel;
            if (!p || state_->canvas.top() != &p->root || event.type != EventType::KeyDown ||
                (event.key != Key::Return && event.key != Key::Escape)) {
                return false;
            }
            auto* button = dynamic_cast<Button*>(state_->canvas.focus());
            state_->canvas.afterDispatch([this, p, button, accepting = event.key == Key::Return] {
                if (state_->panel != p) {
                    return;
                }
                if (!accepting) {
                    cancel();
                } else if (button) {
                    state_->canvas.performClick(*button);
                } else {
                    accept();
                }
            });
            return true;
        };
        State::Operation operation(*s);
        if (!s->canvas.pushDialog(p->root) || !s->canvas.makeFirstResponder(&p->filename)) {
            s->panel.reset();
            s->result = oldResult;
            return false;
        }
        if (epoch != s->epoch) {
            s->stopping = true;
            return false;
        }
        s->result.reset();
        return true;
    } catch (...) {
        s->panel.reset();
        return false;
    }
}

bool FileBrowser::browse(const std::filesystem::path& directory) {
    auto s = state_;
    auto p = s->panel;
    if (!p || s->busy) {
        return false;
    }
    State::Operation operation(*s);
    const auto epoch = s->epoch;
    try {
        auto path =
            (readDirectory ? directory : std::filesystem::absolute(directory)).lexically_normal();
        const auto spelling = pathText(path);
        if (spelling.empty() || !validText(spelling, 4096)) {
            return false;
        }
        const auto callback = readDirectory;
        auto entries = callback ? callback(path) : diskDirectory(path);
        if (!entries || entries->size() > 4096 || epoch != s->epoch || s->panel != p) {
            return false;
        }
        std::set<std::string> names;
        for (const auto& e : *entries) {
            if (e.name.empty() || e.name == "." || e.name == ".." || !validText(e.name, 255) ||
                e.name.find_first_of("/\\:") != e.name.npos || !names.insert(e.name).second) {
                return false;
            }
        }
        std::erase_if(*entries, [&](const auto& e) {
            return !e.directory && !matches(e.name, p->patterns);
        });
        std::ranges::sort(*entries, [](const auto& a, const auto& b) {
            return a.directory != b.directory ? a.directory > b.directory : a.name < b.name;
        });
        TextList list;
        list.noDuplicates = false;
        list.cellSize = p->list.cellSize;
        std::vector<std::string> labels;
        for (std::size_t i = 0; i < entries->size(); ++i) {
            labels.push_back(((*entries)[i].directory ? "[dir] " : "") + (*entries)[i].name);
            if (!list.insert({Int32(i), labels.back()})) {
                return false;
            }
        }
        auto restore = p->list.checkpoint(), scroll = p->scroll.checkpoint();
        p->list.entries = std::move(list.entries);
        p->list.cells = list.cells;
        p->list.selected = {-1, -1};
        if (!p->list.resize({}, list.fields().extent) || !p->scroll.computeSizes()) {
            restore();
            scroll();
            return false;
        }
        p->request.directory.swap(path);
        p->entries.swap(*entries);
        p->names.swap(labels);
        p->title.text = spelling;
        p->filename.setText("");
        return true;
    } catch (...) {
        return false;
    }
}

bool FileBrowser::filter(std::size_t index) {
    auto p = state_->panel;
    if (!p || state_->busy || index >= p->request.filters.size()) {
        return false;
    }
    auto old = p->patterns;
    p->patterns = p->request.filters[index].pattern;
    if (!browse(p->request.directory)) {
        p->patterns.swap(old);
        return false;
    }
    p->selectedFilter = index;
    p->filters.text = p->request.filters[index].label;
    return true;
}

bool FileBrowser::text(std::string value) {
    return active() && !state_->busy && state_->panel->filename.setText(std::move(value));
}

bool FileBrowser::select(std::size_t index) {
    auto p = state_->panel;
    if (!p || state_->busy || index >= p->entries.size() ||
        !p->filename.setText(p->entries[index].name)) {
        return false;
    }
    p->list.selected = {0, Int32(index)};
    return true;
}

bool FileBrowser::accept() {
    auto s = state_;
    auto p = s->panel;
    if (!p || s->busy || s->canvas.top() != &p->root || p->filename.text.empty()) {
        return false;
    }
    try {
        auto input = p->filename.text;
        if (input.find_first_of("*?") != input.npos) {
            if (!validPatterns(input)) {
                return false;
            }
            auto old = p->patterns;
            p->patterns = input;
            if (browse(p->request.directory)) {
                return true;
            }
            p->patterns.swap(old);
            return false;
        }
        const auto source = pathFrom(input);
        auto path = (p->request.directory / source).lexically_normal();
        auto callback = probe;
        const auto inspect = [&](const auto& target) {
            return callback ? callback(target, p->request.save)
                            : diskProbe(target, p->request.save);
        };
        State::Operation operation(*s);
        auto kind = inspect(path);
        if (kind == Kind::Directory) {
            s->busy = false;
            return browse(path);
        }
        if (kind == Kind::Missing && !source.has_extension() &&
            !p->request.defaultExtension.empty()) {
            auto suffix = p->request.defaultExtension;
            auto pattern = std::string_view(p->request.filters[p->selectedFilter].pattern);
            pattern = pattern.substr(0, pattern.find(';'));
            if (pattern.starts_with("*.") && pattern.find_first_of("*?;", 2) == pattern.npos) {
                suffix = pattern.substr(2);
            }
            path += pathFrom("." + suffix);
            kind = inspect(path);
        }
        const auto spelling = pathText(path);
        if (!validText(spelling, p->request.maxPathBytes) || spelling.empty() ||
            kind == Kind::Refused || kind == Kind::Directory ||
            (!p->request.save && kind != Kind::File) ||
            inspect(path.parent_path()) != Kind::Directory || s->panel != p) {
            return false;
        }
        Result result{true, path, p->request.directory};
        const auto epoch = s->epoch;
        const auto name = p->filename.text;
        // Recheck after close/capture/focus preparation. A chooser cannot promise
        // future file I/O, but it must not publish a path refused during close.
        if (!s->canvas.commitDialog([&] {
                const auto current = inspect(path);
                return epoch == s->epoch && name == p->filename.text && current == kind &&
                       inspect(path.parent_path()) == Kind::Directory;
            })) {
            return false;
        }
        s->result = std::move(result);
        s->panel.reset();
        return true;
    } catch (...) {
        return false;
    }
}

bool FileBrowser::cancel() {
    auto s = state_;
    auto p = s->panel;
    if (!p || s->busy) {
        return false;
    }
    if (p->filters.isOpen() && !p->filters.dismiss(false)) {
        return false;
    }
    if (s->canvas.top() != &p->root) {
        return false;
    }
    State::Operation operation(*s);
    Result result{false, {}, p->request.directory};
    if (!s->canvas.popDialog()) {
        return false;
    }
    s->result = std::move(result);
    s->panel.reset();
    return true;
}

world::ObjectHost FileBrowser::host(std::function<void(FileBrowser&)> modal) {
    const std::weak_ptr<State> weak = state_;
    world::ObjectHost host;
    host.command = [this, weak, modal = std::move(modal)](
                       Console& console,
                       sim::SimManager*,
                       std::string_view name,
                       std::span<const std::string> args) -> std::optional<std::string> {
        auto s = weak.lock();
        if (!s || !modal || (name != "openFile" && name != "saveFileAs") || args.size() < 3 ||
            args.size() % 2 == 0) {
            return "False";
        }
        FileBrowserRequest request;
        request.save = name == "saveFileAs";
        request.directory = pathFrom(console.getVariable("Tool::openSaveDirectory"));
        for (std::size_t i = 1; i < args.size(); i += 2) {
            request.filters.push_back({args[i], args[i + 1]});
        }
        const auto epoch = s->epoch;
        if (!open(std::move(request))) {
            return "False";
        }
        try {
            modal(*this);
        } catch (...) {
            cancel();
            return "False";
        }
        if (epoch != s->epoch) {
            if (active()) {
                cancel();
            }
            return "False";
        }
        if (active()) {
            cancel();
            return "False";
        }
        const auto result = s->result;
        if (!result) {
            return "False";
        }
        console.setVariable("Tool::openSaveDirectory", pathText(result->directory));
        if (result->accepted) {
            console.setVariable(args.front(), pathText(result->path));
        }
        console.setVariable("dlgResult", result->accepted ? "[ok]" : "[cancel]");
        return "True";
    };
    host.managerChanged = [weak](sim::SimManager*) {
        if (auto s = weak.lock()) {
            ++s->epoch;
        }
    };
    host.stop = [weak] {
        if (auto s = weak.lock()) {
            ++s->epoch;
            s->stopping = true;
            if (s->canvas.dispatching()) {
                s->canvas.afterDispatch([weak] {
                    if (auto state = weak.lock()) {
                        state->retire();
                    }
                });
            } else {
                s->retire();
            }
        }
    };
    return host;
}
} // namespace opent1::gui
