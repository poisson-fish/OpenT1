#include <opent1/gui/native_tree_host.hpp>
#include <ranges>
#include <opent1/gui/containers.hpp>
#include <opent1/gui/sdl_gui.hpp>
#include <opent1/gui/sdl_presentation.hpp>
#include <opent1/sim/sim_sdl_canvas.hpp>
#include <opent1/sim/sim_sdl_inspector.hpp>
#include <charconv>
#include <map>

namespace opent1::gui {
namespace {
bool number(std::string_view text, Int32& n) {
    const auto r = std::from_chars(text.data(), text.data() + text.size(), n);
    return r.ec == std::errc{} && r.ptr == text.data() + text.size();
}

bool configure(Control& control, Point2I p, Point2I e) {
    return control.resize(p, e);
}

struct TreeInspector {
    platform::SdlApplication& app;
    platform::WindowId id{};
    sim::SimInspector model;
    std::unique_ptr<gfx::SdlGpuSurface> surface;
    std::unique_ptr<sim::SdlInspectorInput> input;
    SdlPresentation presentation;

    TreeInspector(platform::SdlApplication& a,
                  sim::SimInspection document,
                  const TagDictionary& tags,
                  const gfx::GpuSurfaceConfig& gpu,
                  bool hidden)
        : app(a) {
        if (!model.resize(320, 400) || !model.open(std::move(document), tags)) {
            throw std::runtime_error("tree inspection");
        }
        platform::WindowConfig config;
        config.title = "Object Info";
        config.width = 320;
        config.height = 400;
        config.hidden = hidden;
        auto& window = app.createWindow(config);
        id = window.id();
        try {
            surface = gfx::SdlGpuSurface::create(window, 320, 400, gpu);
            if (!surface) {
                throw std::runtime_error("inspector GPU surface");
            }
            input = std::make_unique<sim::SdlInspectorInput>(window, model);
            input->syncTextInput();
        } catch (...) {
            app.destroyWindow(id);
            id = 0;
            throw;
        }
    }

    ~TreeInspector() {
        input.reset();
        surface.reset();
        if (id) {
            app.destroyWindow(id);
        }
    }
};

struct TreePanel : std::enable_shared_from_this<TreePanel> {
    platform::SdlApplication& app;
    platform::WindowId parent{}, id{};
    std::weak_ptr<sim::SimWorld> world;
    Control root;
    Scroll scroll;
    MissionTree tree;
    Canvas canvas;
    std::unique_ptr<gfx::SdlGpuSurface> surface;
    std::shared_ptr<SdlInput> input;
    std::weak_ptr<const void> viewLifetime;
    SdlPresentation presentation;
    std::vector<sim::SimAddress> sets;
    std::vector<std::pair<std::string, std::string>> scripts;
    std::vector<std::unique_ptr<Button>> buttons;
    std::shared_ptr<const gfx::BitmapFont> font;
    Console* console{};
    bool active{true};
    sim::SimGame* game{};
    std::weak_ptr<const void> gameLifetime;
    gfx::GpuSurfaceConfig gpu;
    bool hidden{};
    std::shared_ptr<std::vector<std::byte>> clipboard;
    std::shared_ptr<TreeInspector> inspection;
    Button editMenu;
    Control menu, renameDialog;
    TextEdit renameEdit;
    Button renameOK, renameCancel;
    std::vector<std::unique_ptr<Button>> menuButtons;
    std::optional<MissionTree::Row> renameRow;

    TreePanel(platform::SdlApplication& a,
              MissionView& view,
              platform::WindowId p,
              std::string_view title,
              std::shared_ptr<const gfx::BitmapFont> f,
              const gfx::GpuSurfaceConfig& configGpu,
              bool configHidden)
        : app(a), parent(p), world(view.world), tree(view), font(std::move(f)), gpu(configGpu),
          hidden(configHidden) {
        viewLifetime = view.lifetime();
        platform::WindowConfig config;
        config.title = title;
        config.width = 320;
        config.height = 400;
        config.hidden = hidden;
        config.resizable = false;
        auto& window = app.createWindow(config);
        id = window.id();
        try {
            surface = gfx::SdlGpuSurface::create(window, 320, 400, gpu);
            if (!surface) {
                throw std::runtime_error("tree GPU surface");
            }
            tree.font = font;
            tree.cellSize = {280, 18};
            scroll.horizontalPolicy = Scroll::AlwaysOff;
            scroll.verticalPolicy = Scroll::Dynamic;
            if (!configure(root, {}, {320, 400}) || !configure(scroll, {0, 28}, {320, 372}) ||
                !root.addObject(scroll) || !scroll.addObject(tree) || !canvas.content(&root)) {
                throw std::runtime_error("tree controls");
            }
            input = std::make_shared<SdlInput>(window, canvas);
            editMenu.text = "Edit";
            editMenu.font = editMenu.highlightFont = font;
            if (!configure(editMenu, {}, {104, 24}) || !root.addObject(editMenu)) {
                throw std::runtime_error("tree menu");
            }
            tree.command = [this](std::string_view command) {
                const std::string owned(command);
                canvas.afterDispatch([self = shared_from_this(), owned] {
                    self->action(owned, {});
                });
                return true;
            };
            tree.dropped = [this](const auto& from, const auto& to, bool before, bool copy) {
                canvas.afterDispatch([self = shared_from_this(), from, to, before, copy] {
                    if (self->live()) {
                        self->tree.drop(from, to, before, copy);
                    }
                });
                return true;
            };
            canvas.onAction = [this](Control& control, UInt32, std::string_view) {
                if (&control == &editMenu) {
                    canvas.afterDispatch([self = shared_from_this()] {
                        self->action("menu", {});
                    });
                    return true;
                }
                if (&control == &renameOK || &control == &renameCancel) {
                    const bool accept = &control == &renameOK;
                    canvas.afterDispatch([self = shared_from_this(), accept] {
                        if (!self->live()) {
                            return;
                        }
                        if (accept &&
                            (!self->renameRow ||
                             !self->tree.rename(*self->renameRow, self->renameEdit.text))) {
                            return;
                        }
                        if (self->canvas.popDialog()) {
                            self->renameRow.reset();
                        }
                    });
                    return true;
                }
                for (const auto& menuButton : menuButtons) {
                    if (menuButton.get() == &control) {
                        const auto action = menuButton->text;
                        canvas.afterDispatch([self = shared_from_this(), action] {
                            if (self->canvas.popDialog()) {
                                self->action(action, {});
                            }
                        });
                        return true;
                    }
                }
                for (auto i : std::views::iota(std::size_t{0}, buttons.size())) {
                    if (buttons[i].get() == &control) {
                        return run(i);
                    }
                }
                return true;
            };
        } catch (...) {
            app.destroyWindow(id);
            id = 0;
            throw;
        }
    }

    ~TreePanel() {
        close();
    }

    void dispose() {
        inspection.reset();
        input.reset();
        canvas.content(nullptr);
        surface.reset();
        if (id) {
            app.destroyWindow(id);
            id = 0;
        }
    }

    void close() {
        active = false;
        console = nullptr;
        if (canvas.dispatching()) {
            canvas.afterDispatch([self = shared_from_this()] {
                self->dispose();
            });
        } else {
            dispose();
        }
    }

    bool live() const {
        return active && id && app.findWindow(id) && app.findWindow(parent) && !world.expired() &&
               !viewLifetime.expired();
    }

    bool run(std::size_t index) {
        if (!live() || !console || index >= scripts.size() || scripts[index].second.empty()) {
            return false;
        }
        const auto text = scripts[index].second;
        return console->evaluate(text, "<tree script>").ok();
    }

    bool addScript(std::string label, std::string script) {
        if (scripts.size() >= 32 || label.empty() || label.size() > 255 || script.size() > 65536) {
            return false;
        }
        auto button = std::make_unique<Button>();
        button->text = label;
        button->font = button->highlightFont = font;
        button->active = !script.empty();
        // Script entries are visible controls, including disabled separators.
        const auto index = scripts.size();
        const auto slot = index + 1;
        const auto rows = Int32(slot / 3 + 1), y = Int32(slot / 3) * 26;
        if (!configure(*button, {Int32(slot % 3) * 104, y}, {104, 24})) {
            return false;
        }
        scripts.reserve(index + 1);
        buttons.reserve(index + 1);
        auto restore = scroll.checkpoint();
        if (!configure(scroll, {0, rows * 26}, {320, std::max(20, 400 - rows * 26)}) ||
            !root.addObject(*button)) {
            restore();
            return false;
        }
        scripts.emplace_back(std::move(label), std::move(script));
        buttons.push_back(std::move(button));
        return true;
    }

    bool action(std::string_view command, std::string_view value) {
        if (!live()) {
            return false;
        }
        if (command == "close") {
            close();
            return true;
        }
        if (command == "menu") {
            if (canvas.dialogCount()) {
                return false;
            }
            menuButtons.clear();
            if (!configure(menu, {8, 32}, {300, 180})) {
                return false;
            }
            constexpr std::array commands{"cut",
                                          "copy",
                                          "paste",
                                          "delete",
                                          "remove",
                                          "duplicate",
                                          "inspect",
                                          "rename",
                                          "lock",
                                          "unlock",
                                          "cancel"};
            for (auto i :
                 std::views::iota(std::size_t{0}) | std::views::take_while([&](std::size_t i) {
                     return i < commands.size();
                 })) {
                auto b = std::make_unique<Button>();
                b->text = commands[i];
                b->font = b->highlightFont = font;
                if (!configure(*b, {Int32(i % 2) * 148, Int32(i / 2) * 28}, {144, 26}) ||
                    !menu.addObject(*b)) {
                    return false;
                }
                menuButtons.push_back(std::move(b));
            }
            return canvas.pushDialog(menu);
        }
        if (command == "cancel") {
            return true;
        }
        if (canvas.dialogCount()) {
            return false;
        }
        const auto row = tree.current();
        if (!row) {
            return false;
        }
        auto model = world.lock();
        auto* object = row->address.resolve(model->manager());
        if (!object || object == &model->manager()) {
            return false;
        }
        if (command == "rename") {
            if (!value.empty()) {
                return tree.rename(*row, value);
            }
            renameRow = row;
            renameEdit.text = std::string(object->getName());
            renameEdit.maxBytes = 255;
            renameEdit.cursor = renameEdit.text.size();
            renameEdit.anchor = 0;
            renameEdit.font = font;
            renameOK.text = "OK";
            renameCancel.text = "Cancel";
            renameOK.font = renameOK.highlightFont = renameCancel.font =
                renameCancel.highlightFont = font;
            return configure(renameDialog, {8, 32}, {300, 60}) &&
                   configure(renameEdit, {4, 2}, {292, 26}) &&
                   configure(renameOK, {4, 30}, {140, 26}) &&
                   configure(renameCancel, {152, 30}, {140, 26}) &&
                   (renameEdit.parent() || renameDialog.addObject(renameEdit)) &&
                   (renameOK.parent() || renameDialog.addObject(renameOK)) &&
                   (renameCancel.parent() || renameDialog.addObject(renameCancel)) &&
                   canvas.pushDialog(renameDialog) && canvas.makeFirstResponder(&renameEdit);
        }
        if (command == "lock" || command == "unlock") {
            return tree.lock(*row, command == "lock");
        }
        if (command == "remove") {
            return tree.remove(*row);
        }
        if (command == "delete") {
            return tree.erase(*row);
        }
        if (!game || gameLifetime.expired() || game->manager() != &model->manager()) {
            return false;
        }
        if (command == "inspect") {
            auto document = game->inspectObject(std::to_string(object->getId()));
            if (!document) {
                return false;
            }
            auto next = std::make_shared<TreeInspector>(
                app, std::move(*document), model->manager().tags(), gpu, hidden);
            inspection = std::move(next);
            return true;
        }
        if (command == "copy" || command == "cut" || command == "duplicate") {
            auto bytes = game->encodeObject(*object);
            if (!bytes || !live()) {
                return false;
            }
            object = row->address.resolve(model->manager());
            if (!object) {
                return false;
            }
            if (command == "cut" && !tree.erase(*row)) {
                return false;
            }
            *clipboard = std::move(*bytes);
            if (command != "duplicate") {
                return true;
            }
        }
        if (command == "paste" || command == "duplicate") {
            auto* target = dynamic_cast<sim::SimSet*>(object);
            if (!target) {
                target = dynamic_cast<sim::SimSet*>(row->parent.resolve(model->manager()));
            }
            if (!target || clipboard->empty()) {
                return false;
            }
            auto restore = tree.checkpoint();
            const auto targetAddress = target->address();
            const bool expanded = target->isExpanded();
            sim::SimObject* adopted{};
            const auto rollback = [&] {
                restore();
                if (auto* current = targetAddress.resolve(model->manager())) {
                    current->setExpanded(expanded);
                }
            };
            try {
                adopted = game->loadObject(
                    "",
                    std::span<const std::byte>(*clipboard),
                    target,
                    [&](sim::SimObject& candidate) {
                        if (!live()) {
                            return false;
                        }
                        auto* current = targetAddress.resolve(model->manager());
                        if (!current) {
                            return false;
                        }
                        current->setExpanded(true);
                        if (!tree.refresh()) {
                            return false;
                        }
                        for (auto i : std::views::iota(std::size_t{0}, tree.rows().size())) {
                            if (tree.rows()[i].address.generation == candidate.generation()) {
                                return tree.select({0, Int32(i)});
                            }
                        }
                        return false;
                    });
            } catch (...) {
                rollback();
                throw;
            }
            if (!adopted) {
                rollback();
                return false;
            }
            return true;
        }
        return false;
    }

    bool processInspector(const platform::Event& event) {
        auto retained = inspection;
        if (!retained || event.window != retained->id) {
            return false;
        }
        if (const auto* w = std::get_if<platform::WindowEvent>(&event.data);
            w && w->type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) {
            inspection.reset();
            return true;
        }
        return game && !gameLifetime.expired() && retained->input->process(event, *game);
    }

    bool redraw(bool present) {
        if (!live() || !tree.refresh() || !scroll.computeSizes()) {
            return false;
        }
        gfx::Palette palette;
        for (auto i : std::views::iota(std::size_t{0}, std::size_t{256})) {
            palette.palettes[0].colors[i] = {UInt8(i), UInt8(i), UInt8(i), 0};
        }
        const auto scale = nativeUiScale(app.findWindow(parent), *console);
        if (inspection && app.findWindow(inspection->id) && font) {
            const auto view = inspection->presentation.layout(
                *app.findWindow(inspection->id), inspection->model.size(), scale);
            auto frame = gfx::SoftwareSurface::create(inspection->model.size().x,
                                                      inspection->model.size().y);
            if (!frame || !frame->setPalette(palette) || !inspection->model.draw(*frame, *font)) {
                return false;
            }
            auto bitmap = std::move(*frame).releaseBitmap();
            bitmap.palette = palette;
            if (!SdlPresentation::upload(*inspection->surface, bitmap, view, present)) {
                return false;
            }
        }
        return presentation.draw(
            canvas, *app.findWindow(id), *surface, *input, palette, {320, 400}, scale, present);
    }
};

struct TreeIdentity : sim::SimObject {
    std::shared_ptr<TreePanel> panel;

    explicit TreeIdentity(std::shared_ptr<TreePanel> value) : panel(std::move(value)) {}

    void onRemove() override {
        panel->close();
    }
};
}

struct NativeTreeHost::State {
    platform::SdlApplication* app{};
    MissionView* view{};
    std::weak_ptr<const void> lifetime;
    BitmapLoader loader;
    Target target;
    std::shared_ptr<const gfx::BitmapFont> font;
    gfx::GpuSurfaceConfig gpu;
    bool hidden{};
    std::shared_ptr<const Persistent::ClassRegistry> registry;
    sim::SimGame* game{};
    std::weak_ptr<const void> gameLifetime;
    std::shared_ptr<std::vector<std::byte>> clipboard{std::make_shared<std::vector<std::byte>>()};
    std::weak_ptr<sim::SimWorld> world;
    std::map<std::string, std::shared_ptr<TreePanel>, std::less<>> panels;
    std::vector<sim::SimAddress> identities;

    void frame() {
        auto model = world.lock();
        std::vector<sim::SimAddress> live;
        live.reserve(identities.size());
        for (const auto address : identities) {
            auto* object =
                model ? dynamic_cast<TreeIdentity*>(address.resolve(model->manager())) : nullptr;
            if (!object) {
                continue;
            }
            if (!object->panel->live()) {
                model->manager().deleteObject(*object);
            } else {
                live.push_back(address);
            }
        }
        identities = std::move(live);
        std::erase_if(panels, [](const auto& item) {
            if (item.second->live()) {
                return false;
            }
            item.second->close();
            return true;
        });
    }

    void stop() {
        auto model = world.lock();
        auto old = std::move(panels);
        auto ids = std::move(identities);
        for (auto& [name, panel] : old) {
            panel->close();
        }
        if (model) {
            for (const auto address : ids) {
                if (auto* object = address.resolve(model->manager())) {
                    model->manager().deleteObject(*object);
                }
            }
        }
        world.reset();
    }

    std::shared_ptr<TreePanel> find(std::string_view name) const {
        const auto it = panels.find(name);
        return it != panels.end() && it->second->live() ? it->second : nullptr;
    }

    std::optional<std::string> command(Console& console,
                                       sim::SimManager* manager,
                                       std::string_view name,
                                       std::span<const std::string> args) {
        auto model = world.lock();
        if (!model || manager != &model->manager() || lifetime.expired()) {
            return "False";
        }
        if (name == "simTreeCreate") {
            if (manager->findObject(args[0])) {
                return "False";
            }
            panels.erase(args[0]);
            auto* parent = dynamic_cast<sim::SdlSimCanvas*>(manager->findObject(args[1]));
            if (!parent || !app->findWindow(parent->windowId())) {
                return "False";
            }
            auto* selected = args.size() == 3 ? (target ? target(args[2]) : nullptr) : view;
            if (!selected) {
                return "False";
            }
            auto selectedWorld = selected->world.lock();
            if (!selectedWorld) {
                return "False";
            }
            std::vector<std::shared_ptr<const gfx::Bitmap>> images;
            for (const auto file : {"Node.bmp",
                                    "NodeOpen.bmp",
                                    "FolderClosed.bmp",
                                    "FolderOpen.bmp",
                                    "SetClosed.bmp",
                                    "SetOpen.bmp",
                                    "DropPt.bmp",
                                    "DropPtOpen.bmp"}) {
                auto image = loader ? loader(file) : nullptr;
                if (!image || !image->valid()) {
                    return "False";
                }
                images.push_back(std::move(image));
            }
            auto panel = std::make_shared<TreePanel>(
                *app, *selected, parent->windowId(), args[0], font, gpu, hidden);
            panel->console = &console;
            panel->game = game;
            panel->gameLifetime = gameLifetime;
            panel->clipboard = clipboard;
            panel->tree.bitmaps = std::move(images);
            panel->tree.classes = {{"Locked", 6, 7}, {"SimGroup", 2, 3}, {"SimSet", 4, 5}};
            panel->tree.className = [classes = registry](const sim::SimObject& object) {
                const auto* entry = classes ? classes->find(typeid(object)) : nullptr;
                return entry                                         ? entry->name
                       : dynamic_cast<const sim::SimGroup*>(&object) ? std::string("SimGroup")
                       : dynamic_cast<const sim::SimSet*>(&object)   ? std::string("SimSet")
                                                                     : std::string("SimObject");
            };
            auto identity = std::make_unique<TreeIdentity>(panel);
            identity->assignName(args[0]);
            identities.reserve(identities.size() + 1);
            auto [it, inserted] = panels.emplace(args[0], panel);
            if (!inserted) {
                return "False";
            }
            try {
                auto* adopted = manager->addObject(std::move(identity));
                if (!adopted) {
                    panels.erase(it);
                    return "False";
                }
                identities.push_back(adopted->address());
            } catch (...) {
                panels.erase(it);
                throw;
            }
            return "True";
        }
        auto panel = find(args[0]);
        if (!panel) {
            return "False";
        }
        auto targetWorld = panel->world.lock();
        if (!targetWorld) {
            return "False";
        }
        if (name == "simTreeAddSet") {
            auto addresses = panel->sets;
            std::vector<sim::SimSet*> sets;
            for (const auto address : addresses) {
                auto* set =
                    address.objectId == targetWorld->manager().getId()
                        ? &targetWorld->manager()
                        : dynamic_cast<sim::SimSet*>(address.resolve(targetWorld->manager()));
                if (!set) {
                    return "False";
                }
                sets.push_back(set);
            }
            for (const auto& arg : args.subspan(1)) {
                auto* set =
                    arg == "manager"
                        ? &targetWorld->manager()
                        : dynamic_cast<sim::SimSet*>(targetWorld->manager().findObject(arg));
                if (!set || std::ranges::find(sets, set) != sets.end()) {
                    return "False";
                }
                sets.push_back(set);
                addresses.push_back(set->address());
            }
            if (!panel->tree.roots(sets)) {
                return "False";
            }
            panel->sets = std::move(addresses);
            return "True";
        }
        if (name == "simTreeRegBitmaps") {
            if (panel->tree.bitmaps.size() + args.size() - 1 > 256) {
                return "False";
            }
            auto staged = panel->tree.bitmaps;
            for (const auto& file : args.subspan(1)) {
                auto image = loader ? loader(file) : nullptr;
                if (!image || !image->valid()) {
                    return "False";
                }
                staged.push_back(std::move(image));
            }
            panel->tree.bitmaps = std::move(staged);
            return "True";
        }
        if (name == "simTreeRegClass") {
            Int32 normal{}, chosen{};
            if (args[1].empty() || args[1].size() > 255 || !number(args[2], normal) ||
                !number(args[3], chosen) || normal < 0 || chosen < 0 ||
                std::size_t(std::max(normal, chosen)) >= panel->tree.bitmaps.size() ||
                panel->tree.classes.size() >= 256) {
                return "False";
            }
            panel->tree.classes.insert(panel->tree.classes.begin(), {args[1], normal, chosen});
            return "True";
        }
        if (name == "simTreeRegScript") {
            if (args.size() == 2 && args[1] != "separator") {
                return "False";
            }
            return panel->addScript(args[1], args.size() == 3 ? args[2] : std::string{}) ? "True"
                                                                                         : "False";
        }
        return "False";
    }
};

NativeTreeHost::NativeTreeHost(platform::SdlApplication& app,
                               MissionView& view,
                               BitmapLoader loader,
                               std::shared_ptr<const gfx::BitmapFont> font,
                               gfx::GpuSurfaceConfig gpu,
                               bool hidden,
                               Target target,
                               std::shared_ptr<const Persistent::ClassRegistry> registry,
                               sim::SimGame* game)
    : state_(std::make_shared<State>()) {
    state_->app = &app;
    state_->view = &view;
    state_->lifetime = view.lifetime();
    state_->loader = std::move(loader);
    state_->font = std::move(font);
    state_->gpu = std::move(gpu);
    state_->hidden = hidden;
    state_->target = std::move(target);
    state_->registry = std::move(registry);
    state_->game = game;
    if (game) {
        state_->gameLifetime = game->lifetime();
    }
}

world::ObjectHost NativeTreeHost::host() const {
    auto state = state_;
    world::ObjectHost host;
    host.managerChanged = [state](auto* manager) {
        state->stop();
        if (!state->lifetime.expired()) {
            auto model = state->view->world.lock();
            if (model && manager == &model->manager()) {
                state->world = model;
            }
        }
    };
    host.command = [state](Console& c, auto* m, auto n, auto a) {
        return state->command(c, m, n, a);
    };
    host.frame = [state] {
        state->frame();
    };
    host.stop = [state] {
        state->stop();
    };
    return host;
}

bool NativeTreeHost::process(const platform::Event& event) {
    auto state = state_;
    for (const auto& [name, panel] : state->panels) {
        auto retained = panel;
        if (retained->live() && retained->processInspector(event)) {
            return true;
        }
        if (retained->live() && event.window == retained->id) {
            if (const auto* w = std::get_if<platform::WindowEvent>(&event.data);
                w && w->type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) {
                retained->close();
                return true;
            }
            auto input = retained->input;
            return input && input->process(event);
        }
    }
    return false;
}

bool NativeTreeHost::redraw(std::string_view name, bool present) {
    auto panel = state_->find(name);
    return panel && panel->redraw(present);
}

bool NativeTreeHost::runScript(std::string_view name, std::size_t index) {
    auto panel = state_->find(name);
    return panel && panel->run(index);
}

bool NativeTreeHost::action(std::string_view name,
                            std::string_view command,
                            std::string_view value) {
    auto panel = state_->find(name);
    return panel && panel->action(command, value);
}

bool NativeTreeHost::drop(
    std::string_view name, std::size_t source, std::size_t target, bool before, bool copy) {
    auto panel = state_->find(name);
    return panel && source < panel->tree.rows().size() && target < panel->tree.rows().size() &&
           panel->tree.drop(panel->tree.rows()[source], panel->tree.rows()[target], before, copy);
}

sim::SimInspector* NativeTreeHost::inspector(std::string_view name) const {
    auto panel = state_->find(name);
    return panel && panel->inspection ? &panel->inspection->model : nullptr;
}

platform::SdlWindow* NativeTreeHost::inspectorWindow(std::string_view name) const {
    auto panel = state_->find(name);
    return panel && panel->inspection ? state_->app->findWindow(panel->inspection->id) : nullptr;
}

MissionTree* NativeTreeHost::tree(std::string_view name) const {
    auto panel = state_->find(name);
    return panel ? &panel->tree : nullptr;
}

Canvas* NativeTreeHost::canvas(std::string_view name) const {
    auto panel = state_->find(name);
    return panel ? &panel->canvas : nullptr;
}

platform::SdlWindow* NativeTreeHost::window(std::string_view name) const {
    auto panel = state_->find(name);
    return panel ? state_->app->findWindow(panel->id) : nullptr;
}
}
