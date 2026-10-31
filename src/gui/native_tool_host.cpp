#include <opent1/gui/native_tool_host.hpp>
#include <opent1/gui/sdl_presentation.hpp>
#include <opent1/sim/sim_sdl_canvas.hpp>
#include <charconv>
#include <map>

namespace opent1::gui {
namespace {
constexpr Int32 Width = 640, Height = 480;

bool number(std::string_view text, Int32& n) {
    const auto r = std::from_chars(text.data(), text.data() + text.size(), n);
    return r.ec == std::errc{} && r.ptr == text.data() + text.size();
}

bool textValid(std::string_view text, std::size_t maximum = 4096) {
    TextEdit e;
    e.maxBytes = maximum;
    return e.setText(std::string(text));
}

struct ToolButton : Check {
    std::string help;
    std::shared_ptr<const gfx::Bitmap> bitmap;
    bool toggle{};

    std::optional<std::string> helpText(double) const override {
        return help.empty() ? std::nullopt : std::optional(help);
    }

    void action() override {
        if (toggle) {
            Check::action();
        } else {
            Button::action();
        }
    }

    bool draw(gfx::SoftwareSurface& s, Point2I offset, RectI clip) const override {
        if (!(toggle ? Check::draw(s, offset, clip) : Button::draw(s, offset, clip))) {
            return false;
        }
        return !bitmap || s.blit(*bitmap, offset + Point2I{4, 4});
    }
};

struct Entry {
    std::string name, down, up, help;
    std::shared_ptr<const gfx::Bitmap> bitmap;
    bool gap{}, wrap{}, toggle{}, checked{};
};

struct Part {
    std::string variable, label;
};

struct Model {
    bool strip{};
    std::vector<Entry> entries;
    std::vector<Int32> edges;
    std::map<Int32, Part> parts;
};

struct View {
    Control root;
    std::vector<std::unique_ptr<ToolButton>> buttons;
    std::vector<std::unique_ptr<Text>> status;

    View(const Model& model, Console& c, const std::shared_ptr<const gfx::BitmapFont>& font) {
        auto fields = root.fields();
        fields.extent = {Width, Height};
        fields.style.opaque = 1;
        fields.style.fill = 249;
        if (!root.configure(fields)) {
            throw std::runtime_error("tool root");
        }
        Int32 x = 4, y = 4;
        for (std::size_t i = 0; i < model.entries.size(); ++i) {
            const auto& e = model.entries[i];
            auto b = std::make_unique<ToolButton>();
            b->text = e.bitmap ? std::string{} : e.name;
            b->bitmap = e.bitmap;
            b->help = e.help;
            b->toggle = e.toggle;
            b->checked = e.checked;
            b->font = b->highlightFont = b->disabledFont = font;
            b->active = !e.gap;
            b->message = UInt32(i + 1);
            const Int32 width = e.gap ? 12 : std::max(44, e.bitmap ? e.bitmap->width + 8 : 100),
                        height = std::max(28, e.bitmap ? e.bitmap->height + 8 : 28);
            if (x + width > Width) {
                x = 4;
                y += 40;
            }
            if (y + height > Height - 32 || !b->resize({x, y}, {width, height}) ||
                !root.addObject(*b)) {
                throw std::length_error("tool layout");
            }
            x += width + 4;
            if (model.strip || e.wrap) {
                x = 4;
                y += height + 4;
            }
            buttons.push_back(std::move(b));
        }
        Int32 left = 0;
        for (std::size_t i = 0; i < model.edges.size(); ++i) {
            auto t = std::make_unique<Text>();
            t->font = font;
            if (auto p = model.parts.find(Int32(i)); p != model.parts.end()) {
                t->text = p->second.label + ": " + c.getVariable(p->second.variable);
                if (!textValid(t->text)) {
                    throw std::length_error("tool status text");
                }
            }
            const Int32 right = model.edges[i] == -1 ? Width : model.edges[i];
            if (right <= left || right > Width ||
                !t->resize({left, Height - 28}, {right - left, 28}) || !root.addObject(*t)) {
                throw std::invalid_argument("tool status edges");
            }
            left = right;
            status.push_back(std::move(t));
        }
    }
};

struct Panel : std::enable_shared_from_this<Panel> {
    platform::SdlApplication& app;
    platform::WindowId id{}, parent{};
    Console* console{};
    Canvas canvas;
    Model model;
    std::shared_ptr<View> view;
    std::shared_ptr<SdlInput> input;
    std::unique_ptr<gfx::SdlGpuSurface> surface;
    std::shared_ptr<const gfx::BitmapFont> font;
    bool active{true}, busy{};
    SdlPresentation presentation;

    Panel(platform::SdlApplication& a,
          platform::WindowId p,
          Console& c,
          std::string_view title,
          std::shared_ptr<const gfx::BitmapFont> f,
          const gfx::GpuSurfaceConfig& gpu)
        : app(a), parent(p), console(&c), font(std::move(f)) {
        platform::WindowConfig config;
        config.title = title;
        config.hidden = true;
        config.width = Width;
        config.height = Height;
        config.resizable = false;
        auto& window = app.createWindow(config);
        id = window.id();
        try {
            surface = gfx::SdlGpuSurface::create(window, Width, Height, gpu);
            if (!surface) {
                throw std::runtime_error("tool GPU admission");
            }
            input = std::make_shared<SdlInput>(window, canvas);
        } catch (...) {
            app.destroyWindow(id);
            id = 0;
            throw;
        }
    }

    ~Panel() {
        dispose();
    }

    bool live() const {
        return active && console && app.findWindow(id) && app.findWindow(parent);
    }

    void dispose() {
        input.reset();
        canvas.captureMouse = {};
        canvas.content(nullptr);
        view.reset();
        surface.reset();
        if (id) {
            app.destroyWindow(std::exchange(id, 0));
        }
    }

    void close() {
        active = false;
        console = nullptr;
        if (busy) {
            return;
        }
        if (canvas.dispatching()) {
            canvas.afterDispatch([self = shared_from_this()] {
                self->dispose();
            });
        } else {
            dispose();
        }
    }

    bool replace(Model next) {
        if (!live() || busy || canvas.dispatching()) {
            return false;
        }
        busy = true;

        struct Reset {
            Panel& p;

            ~Reset() {
                p.busy = false;
                if (!p.active) {
                    p.dispose();
                }
            }
        } reset{*this};

        auto staged = std::make_shared<View>(next, *console, font);
        gfx::Palette palette;
        for (unsigned i = 0; i < 256; ++i) {
            palette.palettes[0].colors[i] = {UInt8(i), UInt8(i), UInt8(i), 0};
        }
        // Validate the complete new graph/raster before capture/lifecycle work.
        Canvas preparation;
        if (!preparation.content(&staged->root) || !preparation.render(Width, Height, &palette) ||
            !preparation.content(nullptr)) {
            return false;
        }
        if (!live() || !canvas.content(&staged->root)) {
            return false;
        }
        view = std::move(staged);
        if (!live()) {
            return false;
        }
        model = std::move(next);
        return true;
    }

    bool redraw(bool present) {
        if (!live() || busy) {
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
                                 {Width, Height},
                                 nativeUiScale(app.findWindow(parent), *console),
                                 present);
    }

    bool activate(UInt32 message) {
        if (!live() || busy || message == 0 || message > model.entries.size()) {
            return false;
        }
        const auto index = std::size_t(message - 1);
        const auto& entry = model.entries[index];
        auto current = view;
        const auto checked = current->buttons[index]->checked;
        // The original selects ConCommandUp when its toggle is down.
        const auto command = checked ? entry.up : entry.down;
        bool ok = true;
        for (std::size_t first = command.find_first_not_of("\\\n"); first != command.npos;) {
            const auto end = command.find_first_of("\\\n", first);
            if (!console
                     ->evaluate(command.substr(first, end == command.npos ? end : end - first),
                                "<tool button>")
                     .ok()) {
                ok = false;
                break;
            }
            if (!live() || end == command.npos) {
                break;
            }
            first = command.find_first_not_of("\\\n", end);
        }
        if (ok && live() && view == current) {
            model.entries[index].checked = checked;
            return true;
        }
        return false;
    }
};

struct Identity : sim::SimObject {
    std::weak_ptr<Panel> panel;

    explicit Identity(const std::shared_ptr<Panel>& p) : panel(p) {}

    void onRemove() override {
        if (auto p = panel.lock()) {
            p->close();
        }
    }

    void onDeleteNotify(sim::SimObject&) override {
        if (auto p = panel.lock()) {
            p->close();
        }
        deleteObject();
    }
};
}

struct NativeToolHost::State {
    platform::SdlApplication& app;
    sim::SimGame& game;
    std::weak_ptr<const void> lifetime;
    BitmapLoader loader;
    std::shared_ptr<const gfx::BitmapFont> font;
    gfx::GpuSurfaceConfig gpu;
    bool hidden;
    world::ObjectHost fallback;
    sim::SimManager* manager{};
    sim::SimAddress main;
    std::map<std::string, std::shared_ptr<Panel>, std::less<>> panels;
    std::vector<sim::SimAddress> identities;
    UInt64 epoch{};

    State(platform::SdlApplication& a,
          sim::SimGame& g,
          BitmapLoader l,
          std::shared_ptr<const gfx::BitmapFont> f,
          gfx::GpuSurfaceConfig s,
          bool h,
          world::ObjectHost b)
        : app(a), game(g), lifetime(g.lifetime()), loader(std::move(l)), font(std::move(f)),
          gpu(std::move(s)), hidden(h), fallback(std::move(b)) {}

    bool live() const {
        return !lifetime.expired() && manager && game.manager() == manager;
    }

    std::shared_ptr<Panel> find(std::string_view name) const {
        auto i = panels.find(name);
        return live() && i != panels.end() && i->second->live() ? i->second : nullptr;
    }

    platform::WindowId mainWindow() const {
        if (!live()) {
            return 0;
        }
        auto* c = dynamic_cast<sim::SdlSimCanvas*>(main.resolve(*manager));
        return c && app.findWindow(c->windowId()) ? c->windowId() : 0;
    }

    void stop() {
        ++epoch;
        auto previous = std::move(panels);
        auto owned = std::move(identities);
        auto* old = manager;
        main = {};
        manager = nullptr;
        for (auto& [name, p] : previous) {
            p->close();
        }
        if (!lifetime.expired() && game.manager() == old && old) {
            for (auto address : owned) {
                if (auto* o = address.resolve(*old)) {
                    old->deleteObject(*o);
                }
            }
        }
        if (fallback.stop) {
            fallback.stop();
        }
    }

    std::optional<std::string> command(Console& c,
                                       sim::SimManager* m,
                                       std::string_view name,
                                       std::span<const std::string> args) {
        const auto yes = [](bool value) -> std::optional<std::string> {
            return value ? "True" : "False";
        };
        if (!live() || m != manager) {
            return "False";
        }
        for (const auto& a : args) {
            if (!textValid(a, 65536)) {
                return "False";
            }
        }
        for (auto it = panels.begin(); it != panels.end();) {
            if (!it->second->live()) {
                if (auto* identity = dynamic_cast<Identity*>(manager->findObject(it->first))) {
                    manager->deleteObject(*identity);
                }
                it = panels.erase(it);
            } else {
                ++it;
            }
        }
        if (name == "setMainWindow") {
            auto* parent = dynamic_cast<sim::SdlSimCanvas*>(manager->findObject(args[0]));
            if (!parent || !app.findWindow(parent->windowId())) {
                return "False";
            }
            main = parent->address();
            return "True";
        }
        const bool modal = name == "editBox" || name == "edit2Box" || name == "browseBox" ||
                           name == "confirmBox" || name == "openFile" || name == "saveFileAs";
        if (modal && !mainWindow()) {
            return "False";
        }
        if (name == "newToolWindow" || name == "newToolStrip") {
            if (args[0].empty() || args[0].size() > 255 || manager->findObject(args[0]) ||
                panels.contains(args[0]) || panels.size() >= 32) {
                return "False";
            }
            auto* parent = dynamic_cast<sim::SdlSimCanvas*>(manager->findObject(args[1]));
            if (!parent || !app.findWindow(parent->windowId())) {
                return "False";
            }
            const auto parentAddress = parent->address();
            const auto stamp = epoch;
            auto p = std::make_shared<Panel>(app, parent->windowId(), c, args[0], font, gpu);
            Model model;
            model.strip = name == "newToolStrip";
            if (!p->replace(std::move(model)) || !p->redraw(false) || stamp != epoch || !live()) {
                return "False";
            }
            const std::weak_ptr<Panel> weak = p;
            p->canvas.onAction = [weak](Control&, UInt32 message, std::string_view) {
                auto p = weak.lock();
                return p && p->activate(message);
            };
            auto identity = std::make_unique<Identity>(p);
            identity->assignName(args[0]);
            identities.reserve(identities.size() + 1);
            identity->deleteNotify(*parent);
            auto [it, inserted] = panels.emplace(args[0], p);
            if (!inserted) {
                return "False";
            }
            try {
                if (!hidden) {
                    app.findWindow(p->id)->show();
                }
                if (stamp != epoch || !live() || !p->live() ||
                    (parent = dynamic_cast<sim::SdlSimCanvas*>(parentAddress.resolve(*manager))) ==
                        nullptr) {
                    panels.erase(args[0]);
                    return "False";
                }
                auto* adopted = manager->addObject(std::move(identity));
                if (!adopted) {
                    panels.erase(it);
                    return "False";
                }
                identities.push_back(adopted->address());
            } catch (...) {
                panels.erase(args[0]);
                throw;
            }
            return "True";
        }
        if (name == "listToolWindows" || name == "showToolWinAll" || name == "hideToolWinAll") {
            std::vector<std::pair<std::string, std::shared_ptr<Panel>>> snapshot(panels.begin(),
                                                                                 panels.end());
            for (const auto& [title, p] : snapshot) {
                if (!p->live()) {
                    continue;
                }
                auto* w = app.findWindow(p->id);
                if (name == "listToolWindows") {
                    c.print(title + ": " + (p->model.strip ? "Strip" : "Window") +
                            (w->state().visible ? " (Visible)" : " (Hidden)"));
                } else if (name == "showToolWinAll") {
                    w->show();
                } else {
                    w->hide();
                }
            }
            return yes(live());
        }
        if (args.empty()) {
            return "False";
        }
        auto p = find(args[0]);
        if (!p) {
            return fallback.command ? fallback.command(c, m, name, args)
                                    : std::optional<std::string>("False");
        }
        if (name == "showToolWin" || name == "hideToolWin") {
            auto* w = app.findWindow(p->id);
            if (name == "showToolWin") {
                w->show();
            } else {
                w->hide();
            }
            return "True";
        }
        if (name == "setToolWinPos") {
            Int32 x{}, y{};
            if (!number(args[1], x) || !number(args[2], y) || x < 0 || y < 0 || x > 16384 ||
                y > 16384) {
                return "False";
            }
            app.findWindow(p->id)->setPosition(x, y);
            return "True";
        }
        if (name == "listToolButtons") {
            const auto entries = p->model.entries;
            for (const auto& e : entries) {
                c.print(e.name + ": " + e.down);
            }
            return yes(p->live());
        }
        auto model = p->model;
        const auto entry = [&] {
            return std::ranges::find_if(model.entries, [&](const auto& e) {
                return e.name == args[1];
            });
        };
        if (name == "addToolButton" || name == "addToolGap") {
            if (args[1].empty() || args[1].size() > 255 || entry() != model.entries.end() ||
                model.entries.size() >= 1000 || (name == "addToolGap" && model.strip)) {
                return "False";
            }
            Entry next;
            next.name = args[1];
            next.gap = name == "addToolGap";
            if (!next.gap) {
                const auto stamp = epoch;
                next.bitmap = loader ? loader(args[2]) : nullptr;
                if (stamp != epoch || !p->live() || !next.bitmap || !next.bitmap->valid() ||
                    next.bitmap->bitDepth != 8 || next.bitmap->width > 128 ||
                    next.bitmap->height > 128) {
                    return "False";
                }
                Int32 wrap{}, toggle{};
                if ((args.size() > 3 && !number(args[3], wrap)) ||
                    (args.size() > 4 && !number(args[4], toggle))) {
                    return "False";
                }
                next.wrap = wrap != 0;
                next.toggle = toggle != 0;
            }
            model.entries.push_back(std::move(next));
        } else if (name == "delToolButton" || name == "setToolCommand" || name == "setButtonHelp" ||
                   name == "isButtonDown") {
            auto it = entry();
            if (it == model.entries.end()) {
                return "False";
            }
            if (name == "isButtonDown") {
                const auto index = std::size_t(it - model.entries.begin());
                const auto answer =
                    (it->checked || p->view->buttons[index]->depressed) ? "yes" : "no";
                if (args.size() == 3) {
                    c.setVariable(args[2], answer);
                } else {
                    c.print(answer);
                }
                return yes(p->live());
            }
            if (name == "delToolButton") {
                model.entries.erase(it);
            } else if (name == "setButtonHelp") {
                it->help = args[2];
            } else {
                it->down = args[2];
                it->up = args.size() == 4 ? args[3] : std::string{};
            }
        } else if (name == "addStatusBar") {
            if (model.strip || args.size() > 18) {
                return "False";
            }
            std::vector<Int32> edges;
            Int32 previous = 0;
            for (std::size_t i = 1; i < args.size(); ++i) {
                Int32 edge{};
                if (!number(args[i], edge) || (edge != -1 && (edge <= previous || edge > Width)) ||
                    (edge == -1 && i + 1 != args.size())) {
                    return "False";
                }
                previous = edge;
                edges.push_back(edge);
            }
            edges.back() = -1;
            model.edges = std::move(edges);
        } else if (name == "delStatusBar") {
            model.edges.clear();
        } else if (name == "setStatusField" || name == "getStatusField" ||
                   name == "clearStatusField") {
            Int32 index{};
            if (model.strip || !number(args[1], index) || index < 0 || index > 65535) {
                return "False";
            }
            if (name == "getStatusField") {
                if (auto it = model.parts.find(index); it != model.parts.end()) {
                    c.print("getStatusPart:  var: " + it->second.variable +
                            "; text:" + it->second.label);
                }
                return yes(p->live());
            }
            if (name == "clearStatusField") {
                model.parts.erase(index);
            } else {
                model.parts[index] = {args[2], args[3]};
            }
        } else {
            return fallback.command ? fallback.command(c, m, name, args)
                                    : std::optional<std::string>("False");
        }
        return yes(p->replace(std::move(model)));
    }
};

NativeToolHost::NativeToolHost(platform::SdlApplication& a,
                               sim::SimGame& g,
                               BitmapLoader l,
                               std::shared_ptr<const gfx::BitmapFont> f,
                               gfx::GpuSurfaceConfig gpu,
                               bool hidden,
                               world::ObjectHost fallback)
    : state_(std::make_shared<State>(
          a, g, std::move(l), std::move(f), std::move(gpu), hidden, std::move(fallback))) {}

NativeToolHost::~NativeToolHost() {
    state_->stop();
}

world::ObjectHost NativeToolHost::host() const {
    auto s = state_;
    world::ObjectHost h;
    h.command = [s](Console& c, auto* m, auto n, auto a) {
        return s->command(c, m, n, a);
    };
    h.managerChanged = [s](auto* m) {
        s->stop();
        s->manager = m;
        if (s->fallback.managerChanged) {
            s->fallback.managerChanged(m);
        }
    };
    h.stop = [s] {
        s->stop();
    };
    h.frame = [s] {
        if (!s->live()) {
            return;
        }
        auto snapshot = s->panels;
        for (auto& [name, p] : snapshot) {
            if (p->live() && !p->model.edges.empty()) {
                p->replace(p->model);
            }
        }
        if (s->fallback.frame) {
            s->fallback.frame();
        }
    };
    return h;
}

bool NativeToolHost::process(const platform::Event& e) {
    auto snapshot = state_->panels;
    for (auto& [name, p] : snapshot) {
        if (!p->live() || e.window != p->id) {
            continue;
        }
        if (const auto* w = std::get_if<platform::WindowEvent>(&e.data);
            w && w->type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) {
            p->close();
            return true;
        }
        auto input = p->input;
        return input && input->process(e);
    }
    return false;
}

bool NativeToolHost::redraw(std::string_view name, bool present) {
    auto p = state_->find(name);
    return p && p->redraw(present);
}

bool NativeToolHost::click(std::string_view name, std::string_view button) {
    auto p = state_->find(name);
    if (!p) {
        return false;
    }
    for (std::size_t i = 0; i < p->model.entries.size(); ++i) {
        if (p->model.entries[i].name == button) {
            auto retained = p->view;
            return p->canvas.performClick(*retained->buttons[i]);
        }
    }
    return false;
}

platform::WindowId NativeToolHost::mainWindow() const {
    return state_->mainWindow();
}

Canvas* NativeToolHost::canvas(std::string_view name) const {
    auto p = state_->find(name);
    return p ? &p->canvas : nullptr;
}

platform::SdlWindow* NativeToolHost::window(std::string_view name) const {
    auto p = state_->find(name);
    return p ? state_->app.findWindow(p->id) : nullptr;
}

gfx::SdlGpuSurface* NativeToolHost::surface(std::string_view name) const {
    auto p = state_->find(name);
    return p ? p->surface.get() : nullptr;
}

std::string NativeToolHost::status(std::string_view name, std::size_t index) const {
    auto p = state_->find(name);
    return p && index < p->view->status.size() ? p->view->status[index]->text : std::string{};
}
}
