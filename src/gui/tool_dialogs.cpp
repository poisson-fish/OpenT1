#include <opent1/gui/tool_dialogs.hpp>
#include <ranges>

namespace opent1::gui {
namespace {
std::string spelling(const std::filesystem::path& p) {
    const auto s = p.generic_u8string();
    return {reinterpret_cast<const char*>(s.data()), s.size()};
}

std::filesystem::path path(std::string_view s) {
    return std::filesystem::path(
        std::u8string_view(reinterpret_cast<const char8_t*>(s.data()), s.size()));
}

bool textValid(std::string_view s, std::size_t limit) {
    TextEdit edit;
    edit.maxBytes = limit;
    return edit.setText(std::string(s));
}
}

struct ToolDialogs::State {
    struct Panel {
        Control root;
        Text title, question;
        std::array<Text, 2> labels;
        std::array<TextEdit, 2> edits;
        std::array<Button, 4> buttons;
        std::string kind;
        std::vector<std::string> args;
        FileBrowserRequest request;
        std::array<std::string, 2> values;
        std::string result;
        bool choosing{}, accepted{};
    };

    Control root;
    Canvas canvas;
    FileBrowser browser;
    std::shared_ptr<Panel> panel;
    std::shared_ptr<const gfx::BitmapFont> font;
    UInt64 epoch{};
    bool busy{}, stopping{};

    State() {
        if (!root.resize({}, {640, 480}) || !canvas.content(&root)) {
            throw std::runtime_error("tool dialog canvas");
        }
    }

    ~State() {
        canvas.captureMouse = {};
        canvas.content(nullptr);
        panel.reset();
    }

    void retire() {
        if (!stopping || busy || canvas.dispatching() || browser.canvas().dispatching()) {
            return;
        }
        auto host = browser.host({});
        host.stop();
        canvas.captureMouse = {};
        canvas.content(nullptr);
        panel.reset();
        canvas.content(&root);
        stopping = false;
    }

    struct Operation {
        State& s;

        explicit Operation(State& v) : s(v) {
            s.busy = true;
        }

        ~Operation() {
            s.busy = false;
            s.retire();
        }
    };
};

ToolDialogs::ToolDialogs() : state_(std::make_shared<State>()) {}

ToolDialogs::~ToolDialogs() {
    state_->canvas.onAction = {};
    state_->canvas.keyCommand = {};
    state_->canvas.captureMouse = {};
    state_->canvas.content(nullptr);
    state_->panel.reset();
}

bool ToolDialogs::active() const {
    return state_->panel && state_->panel->result.empty();
}

Canvas& ToolDialogs::canvas() {
    return state_->browser.active() ? state_->browser.canvas() : state_->canvas;
}

FileBrowser& ToolDialogs::browser() {
    return state_->browser;
}

ModalView ToolDialogs::view() {
    return {[this]() -> Canvas& {
                return canvas();
            },
            [this] {
                return active();
            },
            [this] {
                return update();
            },
            [this] {
                return cancel();
            }};
}

void ToolDialogs::font(std::shared_ptr<const gfx::BitmapFont> font) {
    state_->font = std::move(font);
    state_->browser.font(state_->font);
    if (auto p = state_->panel) {
        p->title.font = p->question.font = state_->font;
        for (auto& l : p->labels) {
            l.font = state_->font;
        }
        for (auto& e : p->edits) {
            e.font = e.highlightFont = e.disabledFont = state_->font;
        }
        for (auto& b : p->buttons) {
            b.font = b.highlightFont = b.disabledFont = state_->font;
        }
    }
}

bool ToolDialogs::open(std::string_view kind, std::span<const std::string> args, Console& console) {
    auto s = state_;
    if (s->busy || active() || s->browser.active()) {
        return false;
    }
    const bool confirm = kind == "confirmBox", two = kind == "edit2Box",
               browse = kind == "browseBox";
    if ((!confirm && !two && !browse && kind != "editBox") ||
        args.size() < (confirm  ? 2U
                       : two    ? 6U
                       : browse ? 5U
                                : 3U) ||
        ((!browse) && args.size() != (confirm ? 2U
                                      : two   ? 6U
                                              : 3U)) ||
        (browse && args.size() % 2 == 0)) {
        return false;
    }
    for (const auto& arg : args) {
        if (!textValid(arg, 4096)) {
            return false;
        }
    }
    State::Operation operation(*s);
    const auto epoch = s->epoch;
    try {
        auto p = std::make_shared<State::Panel>();
        p->kind = kind;
        p->args.assign(args.begin(), args.end());
        auto fields = p->root.fields();
        fields.style.opaque = 1;
        fields.style.fill = 249;
        fields.style.border = 1;
        fields.style.borderColor = 255;
        if (!p->root.configure(fields) || !p->root.resize({16, 80}, {608, 300})) {
            return false;
        }
        p->title.text = args[0];
        p->question.text = args[1];
        if (!p->title.resize({8, 8}, {592, 28}) || !p->question.resize({8, 40}, {592, 52}) ||
            !p->root.addObject(p->title) || !p->root.addObject(p->question)) {
            return false;
        }
        if (!confirm) {
            for (auto i : std::views::iota(unsigned{0}) | std::views::take_while([&](unsigned i) {
                              return i < (two ? 2U : 1U);
                          })) {
                auto& edit = p->edits[i];
                edit.maxBytes = 254;
                edit.active = true;
                const auto variable = args[two ? 4 + i : 2];
                if (!edit.setText(console.getVariable(variable)) ||
                    !edit.resize({8, 124 + Int32(i) * 44}, {592, 28}) || !p->root.addObject(edit)) {
                    return false;
                }
                if (two) {
                    p->labels[i].text = args[2 + i];
                    if (!p->labels[i].resize({8, 100 + Int32(i) * 44}, {592, 24}) ||
                        !p->root.addObject(p->labels[i])) {
                        return false;
                    }
                }
            }
        }
        if (browse) {
            p->request.directory = path(console.getVariable("Tool::openSaveDirectory"));
            for (std::size_t i = 3; i < args.size(); i += 2) {
                p->request.filters.push_back({args[i], args[i + 1]});
            }
        }
        const std::array<std::string_view, 4> labels =
            confirm ? std::array<std::string_view, 4>{"Yes", "No", "Cancel", ""}
                    : std::array<std::string_view, 4>{"OK", "Cancel", "Browse...", ""};
        const unsigned count = confirm || browse ? 3 : 2;
        for (auto i : std::views::iota(unsigned{0}) | std::views::take_while([&](unsigned i) {
                          return i < count;
                      })) {
            auto& b = p->buttons[i];
            b.text = labels[i];
            b.message = i + 1;
            b.active = true;
            if (!b.resize({8 + Int32(i) * 124, 252}, {116, 28}) || !p->root.addObject(b)) {
                return false;
            }
        }
        s->panel = p;
        font(s->font);
        s->canvas.onAction = [this](Control&, UInt32 message, std::string_view) {
            auto s = state_;
            auto p = s->panel;
            if (!p) {
                return false;
            }
            s->canvas.afterDispatch([this, message] {
                auto p = state_->panel;
                if (!p) {
                    return;
                }
                if (p->kind == "confirmBox") {
                    if (message == 3) {
                        cancel();
                    } else {
                        decision(message == 1);
                    }
                } else if (message == 1) {
                    accept();
                } else if (message == 2) {
                    cancel();
                } else if (message == 3) {
                    this->browse();
                }
            });
            return true;
        };
        s->canvas.keyCommand = [this](const Event& e) {
            if (e.type != EventType::KeyDown || (e.key != Key::Return && e.key != Key::Escape)) {
                return false;
            }
            auto p = state_->panel;
            auto* button = dynamic_cast<Button*>(state_->canvas.focus());
            state_->canvas.afterDispatch([this, p, button, key = e.key] {
                if (state_->panel != p) {
                    return;
                }
                if (key == Key::Escape) {
                    cancel();
                } else if (button) {
                    state_->canvas.performClick(*button);
                } else {
                    accept();
                }
            });
            return true;
        };
        if (!s->canvas.pushDialog(p->root) || epoch != s->epoch) {
            s->stopping = true;
            return false;
        }
        return true;
    } catch (...) {
        s->stopping = true;
        return false;
    }
}

bool ToolDialogs::value(std::size_t i, std::string value) {
    auto p = state_->panel;
    return active() && !state_->busy && !p->choosing && i < (p->kind == "edit2Box" ? 2U : 1U) &&
           p->kind != "confirmBox" && p->edits[i].setText(std::move(value));
}

std::string ToolDialogs::value(std::size_t i) const {
    auto p = state_->panel;
    return p && i < 2 ? p->edits[i].text : std::string{};
}

bool ToolDialogs::browse() {
    auto s = state_;
    auto p = s->panel;
    if (!active() || s->busy || p->kind != "browseBox" || p->choosing) {
        return false;
    }
    State::Operation operation(*s);
    const auto epoch = s->epoch;
    if (!s->browser.open(p->request) || epoch != s->epoch) {
        return false;
    }
    p->choosing = true;
    return true;
}

bool ToolDialogs::update() {
    auto s = state_;
    auto p = s->panel;
    if (!p || s->busy) {
        return !s->busy;
    }
    if (!p->choosing || s->browser.active()) {
        return true;
    }
    try {
        const auto result = s->browser.result();
        if (!result) {
            return false;
        }
        auto directory = result->directory;
        if (result->accepted && !p->edits[0].setText(spelling(result->path))) {
            return false;
        }
        p->request.directory.swap(directory);
        p->choosing = false;
        return true;
    } catch (...) {
        return false;
    }
}

bool ToolDialogs::finish(std::string result) {
    auto s = state_;
    auto p = s->panel;
    if (!active() || s->busy || p->choosing) {
        return false;
    }
    State::Operation operation(*s);
    const auto epoch = s->epoch;
    try {
        std::array<std::string, 2> values{p->edits[0].text, p->edits[1].text};
        const bool accepted = result == "[ok]" && values[0] != "[cancel]";
        if (result == "[ok]" && !accepted) {
            result = "[cancel]";
        }
        if (!s->canvas.commitDialog([&] {
                return epoch == s->epoch && s->panel == p && p->edits[0].text == values[0] &&
                       p->edits[1].text == values[1];
            })) {
            return false;
        }
        p->values.swap(values);
        p->accepted = accepted;
        p->result.swap(result);
        return true;
    } catch (...) {
        return false;
    }
}

bool ToolDialogs::accept() {
    auto p = state_->panel;
    return p && p->kind != "confirmBox" && finish("[ok]");
}

bool ToolDialogs::decision(bool yes) {
    auto p = state_->panel;
    return p && p->kind == "confirmBox" && finish(yes ? "[yes]" : "[no]");
}

bool ToolDialogs::cancel() {
    auto s = state_;
    if (s->browser.active()) {
        return s->browser.cancel() && update();
    }
    return finish("[cancel]");
}

world::ObjectHost ToolDialogs::host(std::function<bool(ModalView)> pump) {
    const std::weak_ptr<State> weak = state_;
    auto files = state_->browser.host([pump](FileBrowser& b) {
        if (!pump || !pump(b.view())) {
            throw std::runtime_error("tool file modal pump");
        }
    });
    world::ObjectHost host;
    host.command =
        [this, weak, pump, files](Console& console,
                                  sim::SimManager* manager,
                                  std::string_view name,
                                  std::span<const std::string> args) -> std::optional<std::string> {
        auto s = weak.lock();
        if (!s || !pump) {
            return "False";
        }
        if (name == "openFile" || name == "saveFileAs") {
            return files.command(console, manager, name, args);
        }
        const auto epoch = s->epoch;
        if (!open(name, args, console)) {
            return "False";
        }
        auto p = s->panel;
        bool completed{};
        try {
            completed = pump(view());
        } catch (...) {
        }
        if (!completed || epoch != s->epoch || active() || !p || p->result.empty()) {
            s->stopping = true;
            s->retire();
            return "False";
        }
        // External console variable-binding callbacks retain their existing
        // qualification; their arbitrary side effects cannot be rolled back.
        if (p->accepted) {
            console.setVariable(p->args[p->kind == "edit2Box" ? 4 : 2], p->values[0]);
            if (p->kind == "edit2Box") {
                console.setVariable(p->args[5], p->values[1]);
            }
        }
        console.setVariable("dlgResult", p->result);
        s->panel.reset();
        return "True";
    };
    host.managerChanged = [weak, files](sim::SimManager* manager) {
        if (auto s = weak.lock()) {
            ++s->epoch;
        }
        files.managerChanged(manager);
    };
    host.stop = [weak, files] {
        files.stop();
        if (auto s = weak.lock()) {
            ++s->epoch;
            s->stopping = true;
            auto retire = [weak] {
                if (auto state = weak.lock()) {
                    state->retire();
                }
            };
            if (s->canvas.dispatching()) {
                s->canvas.afterDispatch(retire);
            } else if (s->browser.canvas().dispatching()) {
                s->browser.canvas().afterDispatch(retire);
            } else {
                s->retire();
            }
        }
    };
    return host;
}
}
