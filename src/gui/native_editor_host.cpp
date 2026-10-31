#include <opent1/gui/native_editor_host.hpp>
#include <opent1/gui/containers.hpp>
#include <opent1/gui/sdl_presentation.hpp>
#include <charconv>
#include <map>

namespace opent1::gui {
namespace {
struct Panel {
    platform::SdlApplication& app;
    platform::WindowId id{};
    Control root, body;
    Scroll scroll;
    Canvas canvas;
    std::unique_ptr<gfx::SdlGpuSurface> surface;
    std::shared_ptr<SdlInput> input;
    SdlPresentation presentation;
    std::vector<std::unique_ptr<Control>> controls;
    std::map<std::string, TextEdit*, std::less<>> fields;
    std::shared_ptr<const gfx::BitmapFont> font;
    Control* target{};
    std::weak_ptr<const void> targetLifetime;

    Panel(platform::SdlApplication& a,
          std::string_view title,
          Int32 height,
          std::shared_ptr<const gfx::BitmapFont> f,
          const gfx::GpuSurfaceConfig& gpu,
          bool hidden)
        : app(a), font(std::move(f)) {
        platform::WindowConfig config;
        config.title = title;
        config.width = 420;
        config.height = height;
        config.hidden = hidden;
        config.resizable = false;
        auto& window = app.createWindow(config);
        id = window.id();
        try {
            surface = gfx::SdlGpuSurface::create(window, 420, height, gpu);
            if (!surface) {
                throw std::runtime_error("GUI editor GPU surface");
            }
            auto configFields = root.fields();
            configFields.extent = {420, height};
            configFields.style.opaque = 1;
            configFields.style.fill = 249;
            if (!root.configure(configFields) || !canvas.content(&root)) {
                throw std::runtime_error("GUI editor panel");
            }
            input = std::make_shared<SdlInput>(window, canvas);
        } catch (...) {
            app.destroyWindow(id);
            id = 0;
            throw;
        }
    }

    ~Panel() {
        input.reset();
        canvas.content(nullptr);
        controls.clear();
        surface.reset();
        if (id) {
            app.destroyWindow(id);
        }
    }

    void button(std::string text, UInt32 message, Point2I position, Point2I extent = {100, 26}) {
        auto b = std::make_unique<Button>();
        b->font = b->highlightFont = b->disabledFont = font;
        b->text = std::move(text);
        b->message = message;
        b->active = true;
        if (!b->resize(position, extent) || !root.addObject(*b)) {
            throw std::runtime_error("GUI editor button");
        }
        controls.push_back(std::move(b));
    }

    void field(std::string label, std::string value, Int32 row) {
        auto text = std::make_unique<Text>();
        text->font = font;
        text->text = label;
        auto edit = std::make_unique<TextEdit>();
        edit->font = edit->highlightFont = edit->disabledFont = font;
        edit->text = std::move(value);
        edit->active = true;
        edit->maxBytes = 80;
        edit->cursor = edit->anchor = edit->text.size();
        if (!text->resize({8, 8 + row * 28}, {170, 22}) ||
            !edit->resize({180, 8 + row * 28}, {208, 22}) || !body.addObject(*text) ||
            !body.addObject(*edit)) {
            throw std::runtime_error("GUI editor field");
        }
        fields.emplace(std::move(label), edit.get());
        controls.push_back(std::move(text));
        controls.push_back(std::move(edit));
    }

    bool redraw(bool present, float scale) {
        if (!app.findWindow(id)) {
            return false;
        }
        gfx::Palette palette;
        for (unsigned i = 0; i < 256; ++i) {
            palette.palettes[0].colors[i] = {UInt8(i), UInt8(i), UInt8(i), 0};
        }
        return presentation.draw(canvas,
                                 *app.findWindow(id),
                                 *surface,
                                 *input,
                                 palette,
                                 root.fields().extent,
                                 scale,
                                 present);
    }
};

constexpr std::array<std::string_view, 17> buttons = {"Left",
                                                      "Right",
                                                      "Center",
                                                      "Top",
                                                      "Bottom",
                                                      "Vertical space",
                                                      "Horizontal space",
                                                      "Front",
                                                      "Back",
                                                      "Edit mode",
                                                      "Close toolbar",
                                                      "Undo",
                                                      "Redo",
                                                      "Copy",
                                                      "Paste",
                                                      "Delete",
                                                      "Inspect"};

template <class T> bool parse(std::string_view s, T& value) {
    const auto r = std::from_chars(s.data(), s.data() + s.size(), value);
    return r.ec == std::errc{} && r.ptr == s.data() + s.size();
}
}

struct NativeEditorHost::State : std::enable_shared_from_this<State> {
    platform::SdlApplication& app;
    platform::WindowId parent;
    std::shared_ptr<Runtime> runtime;
    std::shared_ptr<const gfx::BitmapFont> font;
    gfx::GpuSurfaceConfig gpu;
    bool hidden;
    std::shared_ptr<SdlInput> input;
    std::shared_ptr<Panel> toolbar, inspector;

    State(platform::SdlApplication& a,
          platform::SdlWindow& w,
          std::shared_ptr<Runtime> r,
          std::shared_ptr<const gfx::BitmapFont> f,
          gfx::GpuSurfaceConfig g,
          bool h)
        : app(a), parent(w.id()), runtime(std::move(r)), font(std::move(f)), gpu(std::move(g)),
          hidden(h) {
        if (!runtime || !font || runtime->toolbar || runtime->inspect || runtime->inputEnabled) {
            throw std::invalid_argument("GUI editor host requires an unbound runtime and font");
        }
        input = std::make_shared<SdlInput>(w, runtime->canvas);
    }

    bool live() const {
        return runtime && runtime->running() && app.findWindow(parent);
    }

    float scale() const {
        return nativeUiScale(app.findWindow(parent), runtime->console());
    }

    bool contains(Control* target) const {
        if (!live() || !target) {
            return false;
        }
        for (auto* c = target; c; c = c->parent()) {
            if (c == runtime->canvas.top()) {
                return true;
            }
        }
        return false;
    }

    void detach() {
        if (runtime) {
            runtime->toolbar = {};
            runtime->inspect = {};
            runtime->inputEnabled = {};
        }
        input.reset();
        runtime.reset();
        for (auto& p : {toolbar, inspector}) {
            if (p) {
                p->canvas.cancel();
                p->canvas.onAction = {};
            }
        }
    }

    bool openToolbar() {
        if (!live()) {
            return false;
        }
        if (toolbar && app.findWindow(toolbar->id)) {
            return true;
        }
        try {
            auto next = std::make_shared<Panel>(app, "GUI Editor", 154, font, gpu, hidden);
            for (std::size_t i = 0; i < buttons.size(); ++i) {
                next->button(std::string(buttons[i]),
                             UInt32(i + 1),
                             {Int32(i % 4) * 104 + 2, Int32(i / 4) * 29 + 2});
            }
            const auto weak = weak_from_this();
            next->canvas.onAction = [weak](Control&, UInt32 m, std::string_view) {
                auto self = weak.lock();
                return self && m > 0 && m <= buttons.size() && self->action(buttons[m - 1]);
            };
            if (!next->redraw(false, scale())) {
                return false;
            }
            toolbar = std::move(next);
            return true;
        } catch (...) {
            return false;
        }
    }

    bool inspect(Control* target) {
        if (!live()) {
            return false;
        }
        // Selection notifications only refresh an already opened inspector.
        if (runtime->canvas.dispatching() && !inspector) {
            return true;
        }
        if (!target) {
            inspector.reset();
            return true;
        }
        if (!contains(target)) {
            return false;
        }
        try {
            auto next = std::make_shared<Panel>(app, "GUI Inspector", 400, font, gpu, hidden);
            next->target = target;
            next->targetLifetime = target->lifetime();
            const auto& f = target->fields();
            Int32 row = 0;
            const auto field = [&](std::string label, auto value) {
                next->field(std::move(label), std::to_string(value), row++);
            };
            field("X", f.position.x);
            field("Y", f.position.y);
            field("Width", f.extent.x);
            field("Height", f.extent.y);
            field("Tag", f.tag);
            field("Horizontal sizing", f.horizontal);
            field("Vertical sizing", f.vertical);
            field("Flags", f.flags);
            next->field("Variable", f.variable, row++);
            next->field("Command", f.command, row++);
            next->field("Alternate command", f.alternateCommand, row++);
            field("Opaque", f.style.opaque);
            field("Fill", f.style.fill);
            field("Selected fill", f.style.selectedFill);
            field("Ghost fill", f.style.ghostFill);
            field("Border", f.style.border);
            field("Border color", f.style.borderColor);
            field("Selected border", f.style.selectedBorder);
            field("Ghost border", f.style.ghostBorder);
            field("Help tag", f.helpTag);
            next->scroll.horizontalPolicy = Scroll::AlwaysOff;
            next->scroll.verticalPolicy = Scroll::AlwaysOn;
            if (!next->body.resize({}, {400, 16 + row * 28}) ||
                !next->root.addObject(next->scroll) || !next->scroll.resize({0, 0}, {420, 356}) ||
                !next->scroll.addObject(next->body)) {
                return false;
            }
            next->button("Apply", 1, {202, 365});
            next->button("Cancel", 2, {308, 365});
            const auto weak = weak_from_this();
            next->canvas.onAction = [weak](Control&, UInt32 m, std::string_view) {
                auto self = weak.lock();
                if (!self) {
                    return false;
                }
                if (m == 1) {
                    return self->apply();
                }
                if (m == 2) {
                    auto panel = self->inspector;
                    if (!panel) {
                        return false;
                    }
                    panel->canvas.afterDispatch([weak, panel] {
                        if (auto s = weak.lock(); s && s->inspector == panel) {
                            s->inspector.reset();
                        }
                    });
                    return true;
                }
                return false;
            };
            next->canvas.rebuildTabs();
            if (!next->redraw(false, scale())) {
                return false;
            }
            inspector = std::move(next);
            return true;
        } catch (...) {
            return false;
        }
    }

    bool apply() {
        auto panel = inspector;
        if (!panel || panel->targetLifetime.expired() || !contains(panel->target) ||
            !runtime->editor()) {
            return false;
        }
        auto f = panel->target->fields();
        const auto read = [&](std::string_view label, auto& value) {
            return parse(panel->fields.at(std::string(label))->text, value);
        };
        // All fields parse and validate together; no target mutation on refusal.
        if (!read("X", f.position.x) || !read("Y", f.position.y) || !read("Width", f.extent.x) ||
            !read("Height", f.extent.y) || !read("Tag", f.tag) ||
            !read("Horizontal sizing", f.horizontal) || !read("Vertical sizing", f.vertical) ||
            !read("Flags", f.flags) || !read("Opaque", f.style.opaque) ||
            !read("Fill", f.style.fill) || !read("Selected fill", f.style.selectedFill) ||
            !read("Ghost fill", f.style.ghostFill) || !read("Border", f.style.border) ||
            !read("Border color", f.style.borderColor) ||
            !read("Selected border", f.style.selectedBorder) ||
            !read("Ghost border", f.style.ghostBorder) || !read("Help tag", f.helpTag)) {
            return false;
        }
        f.variable = panel->fields.at("Variable")->text;
        f.command = panel->fields.at("Command")->text;
        f.alternateCommand = panel->fields.at("Alternate command")->text;
        return runtime->editor()->properties(*panel->target, std::move(f));
    }

    bool action(std::string_view name) {
        if (!live()) {
            return false;
        }
        const auto run = [&](std::string_view command, std::span<const std::string> args = {}) {
            return runtime->command(command, args) == "True";
        };
        constexpr std::array names = {
            "Left", "Right", "Center", "Top", "Bottom", "Vertical space", "Horizontal space"};
        constexpr std::array modes = {"0", "2", "1", "3", "4", "5", "6"};
        for (std::size_t i = 0; i < names.size(); ++i) {
            if (name == names[i]) {
                return run("GuiJustify", std::array<std::string, 1>{modes[i]});
            }
        }
        if (name == "Close toolbar") {
            auto panel = toolbar;
            if (!panel) {
                return false;
            }
            if (panel->canvas.dispatching()) {
                const auto weak = weak_from_this();
                panel->canvas.afterDispatch([weak, panel] {
                    if (auto self = weak.lock(); self && self->toolbar == panel) {
                        self->toolbar.reset();
                    }
                });
            } else {
                toolbar.reset();
            }
            return true;
        }
        if (name == "Edit mode") {
            return run("GuiEditMode");
        }
        if (name == "Front") {
            return run("GuiBringToFront");
        }
        if (name == "Back") {
            return run("GuiSendToBack");
        }
        if (name == "Undo") {
            return run("GuiUndo");
        }
        if (name == "Redo") {
            return run("GuiRedo");
        }
        if (name == "Inspect") {
            return run("GuiInspect");
        }
        if (name == "Delete") {
            return run("GuiDeleteSelection");
        }
        auto* editor = runtime->editor();
        if (!editor) {
            return false;
        }
        if (name == "Copy" || name == "Cut") {
            return editor->copySelection && editor->copySelection(name == "Cut");
        }
        if (name == "Paste") {
            return editor->pasteSelection && editor->pasteSelection();
        }
        return false;
    }
};

NativeEditorHost::NativeEditorHost(platform::SdlApplication& app,
                                   platform::SdlWindow& window,
                                   std::shared_ptr<Runtime> runtime,
                                   std::shared_ptr<const gfx::BitmapFont> font,
                                   gfx::GpuSurfaceConfig gpu,
                                   bool hidden)
    : state_(std::make_shared<State>(
          app, window, std::move(runtime), std::move(font), std::move(gpu), hidden)) {
    const std::weak_ptr<State> weak = state_;
    state_->runtime->toolbar = [weak](EditControl&) {
        auto self = weak.lock();
        return self && self->openToolbar();
    };
    state_->runtime->inspect = [weak](Control* c) {
        auto self = weak.lock();
        return self && self->inspect(c);
    };
    state_->runtime->inputEnabled = [weak](bool mouse, bool enabled) {
        auto self = weak.lock();
        return self && self->live() && self->input && self->input->enabled(mouse, enabled);
    };
}

NativeEditorHost::~NativeEditorHost() {
    detach();
}

void NativeEditorHost::detach() {
    state_->detach();
}

bool NativeEditorHost::process(const platform::Event& event) {
    auto state = state_;
    if (!state->live()) {
        return false;
    }
    if (event.window == state->parent) {
        auto input = state->input;
        const auto w = state->app.findWindow(state->parent)->state();
        if (input) {
            input->presentation(
                Presentation::fit({w.pixelWidth, w.pixelHeight},
                                  Presentation::requestedScale(
                                      state->runtime->console().getVariable("pref::UI::scale"))));
        }
        return input && input->process(event);
    }
    for (bool inspector : {false, true}) {
        auto panel = inspector ? state->inspector : state->toolbar;
        if (!panel || event.window != panel->id) {
            continue;
        }
        if (const auto* w = std::get_if<platform::WindowEvent>(&event.data);
            w && w->type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) {
            (inspector ? state->inspector : state->toolbar).reset();
            return true;
        }
        auto input = panel->input;
        return input && input->process(event);
    }
    return false;
}

bool NativeEditorHost::redraw(bool inspector, bool present) {
    auto state = state_;
    auto panel = inspector ? state->inspector : state->toolbar;
    return state->live() && panel && panel->redraw(present, state->scale());
}

bool NativeEditorHost::action(std::string_view name) {
    return state_->action(name);
}

bool NativeEditorHost::apply() {
    return state_->apply();
}

bool NativeEditorHost::setField(std::string_view label, std::string_view value) {
    auto p = state_->inspector;
    if (!state_->live() || !p || value.size() > 80 || value.find('\0') != value.npos) {
        return false;
    }
    const auto found = p->fields.find(label);
    if (found == p->fields.end()) {
        return false;
    }
    return found->second->variableChanged(value);
}

Canvas* NativeEditorHost::canvas(bool inspector) const {
    auto p = inspector ? state_->inspector : state_->toolbar;
    return p ? &p->canvas : nullptr;
}

platform::SdlWindow* NativeEditorHost::window(bool inspector) const {
    auto p = inspector ? state_->inspector : state_->toolbar;
    return p ? state_->app.findWindow(p->id) : nullptr;
}

gfx::SdlGpuSurface* NativeEditorHost::surface(bool inspector) const {
    auto p = inspector ? state_->inspector : state_->toolbar;
    return p ? p->surface.get() : nullptr;
}

SdlInput* NativeEditorHost::input() const {
    return state_->input.get();
}
}
