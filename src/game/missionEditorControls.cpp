#include <opent1/game/mission_editor_controls.hpp>
#include <opent1/game/fear_mission_editor.hpp>
#include <opent1/game/string_table.hpp>
#include <opent1/gui/containers.hpp>
#include <opent1/sim/sim_inspector.hpp>
#include <opent1/core/filename.hpp>
#include <opent1/core/text.hpp>

#include <charconv>
#include <cmath>
#include <limits>
#include <set>

namespace opent1::game {
namespace {
using Error = Persistent::Error;

std::optional<Int32> integer(std::string_view text) {
    Int32 value{};
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    return result.ec == std::errc{} && result.ptr == text.data() + text.size()
               ? std::optional<Int32>(value)
               : std::nullopt;
}

std::optional<sim::WorldType> worldType(std::string_view id) {
    if (id == "1")
        return sim::WorldType::Server;
    if (id == "0")
        return sim::WorldType::Client;
    return {};
}

template <class Parent> class EditorControl : public Parent {
public:
    EditorControl() {
        defaults();
    }

    bool becomeFirstResponder() const override {
        if constexpr (std::is_same_v<Parent, gui::TextEdit>) {
            return Parent::becomeFirstResponder();
        } else {
            return false;
        }
    }

    Error read(StreamIO& s, Int32 v, Int32 u, Persistent::LoadContext& c) override {
        const auto result = Parent::read(s, v, u, c);
        if (result == Error::Ok)
            defaults();
        return result;
    }

    bool event(const gui::Event& event) override {
        if constexpr (std::is_same_v<Parent, gui::TextEdit>) {
            if (event.type == gui::EventType::KeyDown && event.key == gui::Key::Return) {
                return !this->canvas() || this->canvas()->makeFirstResponder(nullptr);
            }
        }
        return Parent::event(event);
    }

private:
    void defaults() {
        this->fontTag = this->highlightFontTag = IDFNT_EDITOR;
        if constexpr (std::is_base_of_v<gui::Text, Parent>) {
            this->disabledFontTag = IDFNT_EDITOR;
        }
        if constexpr (std::is_same_v<Parent, gui::Check>) {
            this->fields_.style.fill = 0;
            this->fields_.style.selectedFill = 255;
        }
        if constexpr (std::is_same_v<Parent, gui::TextEdit>) {
            this->highlightFontTag = IDFNT_EDITOR_HILITE;
            this->cursorColor = this->highlightColor = 255;
        }
    }
};

using METextList = EditorControl<gui::TextList>;
using MEButton = EditorControl<gui::Button>;
using MECheck = EditorControl<gui::Check>;
using MERadio = EditorControl<gui::Radio>;

class METextEdit : public EditorControl<gui::TextEdit> {
protected:
    void drawBackground(gfx::SoftwareSurface& surface, Point2I offset) const override {
        const gui::RectI bounds{
            offset.x, offset.y, offset.x + fields_.extent.x - 1, offset.y + fields_.extent.y - 1};
        surface.fill(bounds, 0);
        if (active)
            surface.rectangle(bounds, 255);
    }
};

class InspectAction : public MEButton {
public:
    std::function<void()> activate;

    void action() override {
        if (activate)
            activate();
    }
};

class MEPopup : public EditorControl<gui::ComboBox> {
public:
    sim::SimGame* game{};
    std::weak_ptr<const void> gameLife;

    MEPopup() {
        list().noDuplicates = false;
    }

    Int32 selectedId() const {
        const auto* entry = const_cast<MEPopup*>(this)->list().selection();
        return entry ? entry->tag : 0;
    }

    void setSelected(Int32 id) {
        auto& items = list();
        items.selected = {-1, -1};
        text.clear();
        for (std::size_t i = 0; i < items.entries.size(); ++i) {
            if (items.entries[i].tag == id) {
                items.selected = {0, Int32(i)};
                text = items.entries[i].text;
                break;
            }
        }
    }

    std::string getScriptValue() const override {
        return std::to_string(selectedId());
    }

    bool variableChanged(std::string_view value) override {
        const auto id = integer(value);
        if (!id)
            return false;
        setSelected(*id);
        return true;
    }

    bool draw(gfx::SoftwareSurface& s, Point2I p, gui::RectI clip) const override {
        if (active)
            s.rectangle({p.x, p.y, p.x + fields_.extent.x - 1, p.y + fields_.extent.y - 1}, 255);
        return gui::Text::draw(s, p + Point2I{3, 0}, clip);
    }

protected:
    void selectionCommitted() override {
        if (!game || gameLife.expired())
            return;
        if (!fields_.variable.empty())
            game->console().setVariable(fields_.variable, getScriptValue());
        if (getName().empty())
            return;
        const auto function = std::string(getName()) + "::onSelect";
        if (game->console().isFunction(function)) {
            const auto* entry = list().selection();
            const std::array<std::string, 2> args{entry ? entry->text : "",
                                                  entry ? std::to_string(entry->tag) : "-1"};
            (void)game->console().execute(function, args);
        }
    }
};

class MEPopupButton : public MEPopup {
public:
    bool current{};

    void makeCurrent() {
        if (parent()) {
            for (auto* object : parent()->objects()) {
                if (auto* button = dynamic_cast<MEPopupButton*>(object))
                    button->current = false;
            }
        }
        current = true;
    }

    void action() override {
        makeCurrent();
        if (!game || gameLife.expired() || getName().empty())
            return;
        const auto function = std::string(getName()) + "::onPressed";
        if (game->console().isFunction(function))
            (void)game->console().execute(function);
    }

    bool event(const gui::Event& e) override {
        if (active && canvas() && e.button == 3) {
            if (e.type == gui::EventType::Down) {
                depressed = true;
                return canvas()->lockMouse(this);
            }
            if (e.type == gui::EventType::Up && depressed) {
                depressed = false;
                canvas()->unlockMouse();
                makeCurrent();
                MEPopup::action();
                return true;
            }
        }
        return MEPopup::event(e);
    }

    bool draw(gfx::SoftwareSurface& s, Point2I p, gui::RectI clip) const override {
        if (current)
            s.rectangle({p.x + 1, p.y + 1, p.x + fields_.extent.x - 2, p.y + fields_.extent.y - 2},
                        255);
        return MEPopup::draw(s, p, clip);
    }

    std::function<void()> checkpoint() override {
        auto base = MEPopup::checkpoint();
        const bool wasCurrent = current;
        return [this, base = std::move(base), wasCurrent] {
            base();
            current = wasCurrent;
        };
    }
};

class MEInspectTagList : public gui::Control {
public:
    sim::SimGame* game{};
    std::weak_ptr<const void> gameLife;
    std::shared_ptr<const gfx::BitmapFont> font, highlight;
    sim::SimInspector inspector;
    std::vector<std::unique_ptr<gui::Control>> editors;
    std::vector<std::string> labels;
    Int32 rowHeight{18};

    bool inspect(sim::WorldType world, std::string_view name) {
        if (!game || gameLife.expired())
            return false;
        if (canvas() && canvas()->dispatching()) {
            const auto life = lifetime();
            canvas()->afterDispatch([this, life, world, name = std::string(name)] {
                if (!life.expired())
                    (void)inspect(world, name);
            });
            return true;
        }
        if (canvas())
            canvas()->makeFirstResponder(nullptr);
        editors.clear();
        labels.clear();
        inspector.close();
        if (name == "-1")
            return resize(fields_.position, {380, 1});
        std::optional<sim::SimInspection> document;
        game->withWorld(world, [&] {
            document = game->inspectObject(name);
        });
        if (!document || !inspector.open(std::move(*document), game->console().tags()))
            return false;
        auto* owner = game->world(world);
        auto* object = owner ? owner->manager().findObject(name) : nullptr;
        const bool locked = !object || object->isLocked();
        rowHeight = font ? font->info.height + 5 : 18;
        for (std::size_t i = 0; i < inspector.fields().size(); ++i) {
            const auto& field = inspector.fields()[i];
            labels.push_back(field.label);
            std::unique_ptr<gui::Control> control;
            if (field.control == sim::InspectorControl::Text) {
                auto edit = std::make_unique<METextEdit>();
                edit->font = edit->disabledFont = font;
                edit->highlightFont = highlight ? highlight : font;
                edit->maxBytes = 1024;
                edit->active = !locked;
                if (!edit->setText(field.text))
                    return false;
                control = std::move(edit);
            } else if (field.control == sim::InspectorControl::Checkbox) {
                auto check = std::make_unique<MECheck>();
                check->font = check->highlightFont = check->disabledFont = font;
                check->active = !locked;
                check->checked = field.text == "[x]";
                control = std::move(check);
            } else if (field.control == sim::InspectorControl::Choice) {
                auto popup = std::make_unique<MEPopup>();
                popup->font = popup->highlightFont = popup->disabledFont = font;
                popup->active = !locked;
                for (const auto& choice : field.choices) {
                    if (!popup->list().insert({choice.value, choice.label}))
                        return false;
                }
                // SimInspector's choice text is the selected label.
                for (const auto& choice : field.choices) {
                    if (choice.label == field.text)
                        popup->setSelected(choice.value);
                }
                control = std::move(popup);
            } else if (field.control == sim::InspectorControl::Action) {
                auto button = std::make_unique<InspectAction>();
                button->font = button->highlightFont = button->disabledFont = font;
                button->text = field.text;
                button->active = !locked;
                button->activate = [this, life = lifetime(), i] {
                    const auto run = [this, life, i] {
                        if (!life.expired() && !gameLife.expired())
                            (void)inspector.activate(*game, i);
                    };
                    if (canvas())
                        canvas()->afterDispatch(run);
                    else
                        run();
                };
                control = std::move(button);
            } else {
                auto text = std::make_unique<gui::Text>();
                text->font = font;
                text->text = field.text;
                control = std::move(text);
            }
            if (!control->resize({122, Int32(i) * rowHeight}, {256, rowHeight - 1}))
                return false;
            if (!addObject(*control))
                return false;
            editors.push_back(std::move(control));
        }
        return resize(fields_.position, {380, std::max(1, Int32(labels.size()) * rowHeight)});
    }

    bool apply() {
        if (!game || gameLife.expired() || !inspector.isOpen())
            return false;
        for (std::size_t i = 0; i < editors.size(); ++i) {
            if (auto* edit = dynamic_cast<METextEdit*>(editors[i].get())) {
                if (!inspector.setText(i, edit->text))
                    return false;
            } else if (auto* check = dynamic_cast<MECheck*>(editors[i].get())) {
                if ((inspector.fields()[i].text == "[x]") != check->checked && !inspector.toggle(i))
                    return false;
            } else if (auto* popup = dynamic_cast<MEPopup*>(editors[i].get())) {
                const auto& choices = inspector.fields()[i].choices;
                const auto choice =
                    std::ranges::find(choices, popup->selectedId(), &sim::InspectorChoice::value);
                if (choice == choices.end() ||
                    !inspector.select(i, std::size_t(choice - choices.begin())))
                    return false;
            }
        }
        return inspector.apply(*game);
    }

    bool draw(gfx::SoftwareSurface& s, Point2I p, gui::RectI clip) const override {
        const auto before = s.clip();
        if (font) {
            const auto labelClip =
                gfx::intersect(before, {p.x, p.y, p.x + 119, p.y + fields_.extent.y - 1});
            if (labelClip)
                s.setClip(*labelClip);
            for (std::size_t i = 0; i < labels.size(); ++i) {
                const auto x = std::max(0, 120 - Int32(font->textWidth(labels[i])));
                if (labelClip && !s.text(*font, {p.x + x, p.y + Int32(i) * rowHeight}, labels[i])) {
                    s.setClip(before);
                    return false;
                }
            }
        }
        s.setClip(before);
        return drawChildren(s, p, clip);
    }
};

struct WorldObject {
    sim::WorldType world{};
    UInt64 revision{};
    sim::SimAddress address;

    sim::SimObject* resolve(sim::SimGame& game) const {
        auto* owner = game.world(world);
        return owner && revision == game.worldRevision(world) ? address.resolve(owner->manager())
                                                              : nullptr;
    }
};

class MEObjectTree : public gui::Array {
public:
    struct Row {
        WorldObject object;
        Int32 depth{}, parent{-1};
        bool last{};
    };

    sim::SimGame* game{};
    std::weak_ptr<const void> gameLife;
    std::shared_ptr<const gfx::BitmapFont> font, highlight;
    std::shared_ptr<const gfx::BitmapArray> branches, selectedBranches;
    std::vector<WorldObject> roots;
    std::vector<Row> rows;
    Int32 selectionMode{};

    MEObjectTree() {
        cells = {1, 0};
    }

    Int32 iconWidth() const {
        return branches ? branches->bitmaps[11].width : 12;
    }

    bool becomeFirstResponder() const override {
        return false;
    }

    WorldObject reference(sim::SimObject& object) const {
        const auto world =
            game->world(sim::WorldType::Server) &&
                    &game->world(sim::WorldType::Server)->manager() == object.getManager()
                ? sim::WorldType::Server
                : sim::WorldType::Client;
        return {world, game->worldRevision(world), object.address()};
    }

    void rebuild() {
        rows.clear();
        if (!game || gameLife.expired()) {
            setSize({1, 0});
            return;
        }
        std::set<sim::SimObject*> seen;
        const auto visit =
            [&](auto&& self, sim::SimObject* object, Int32 depth, Int32 parent, bool last) -> void {
            if (!object || depth > 64 || rows.size() >= 65536 || !seen.insert(object).second)
                return;
            const auto index = Int32(rows.size());
            rows.push_back({reference(*object), depth, parent, last});
            if (auto* group = dynamic_cast<sim::SimSet*>(object); group && group->isExpanded()) {
                for (std::size_t i = 0; i < group->objects().size(); ++i) {
                    self(self,
                         group->objects()[i],
                         depth + 1,
                         index,
                         i + 1 == group->objects().size());
                }
            }
        };
        for (const auto& root : roots)
            visit(visit, root.resolve(*game), 0, -1, true);
        cellSize = {640, font ? font->info.height + 2 : 18};
        setSize({1, Int32(rows.size())});
    }

    void wake() override {
        rebuild();
    }

    void tick(double) override {
        rebuild();
    }

    void callback(std::string_view suffix, sim::SimObject* object = nullptr) {
        const auto function =
            std::string(getName().empty() ? "MissionObjectList" : getName()) + std::string(suffix);
        if (!game->console().isFunction(function))
            return;
        if (object) {
            const auto ref = reference(*object);
            const std::array<std::string, 2> args{ref.world == sim::WorldType::Server ? "1" : "0",
                                                  std::to_string(object->getId())};
            (void)game->console().execute(function, args);
        } else {
            (void)game->console().execute(function);
        }
    }

    void clearSelection() {
        std::set<sim::SimObject*> seen;
        const auto clear = [&](auto&& self, sim::SimObject* obj) -> void {
            if (!obj || !seen.insert(obj).second)
                return;
            obj->setSelected(false);
            if (auto* group = dynamic_cast<sim::SimSet*>(obj)) {
                for (auto* child : group->objects())
                    self(self, child);
            }
        };
        for (const auto& root : roots)
            clear(clear, root.resolve(*game));
        callback("::onSelectionCleared");
    }

    void selectObject(sim::SimObject& object, bool select) {
        const auto life = lifetime();
        std::vector<WorldObject> pending;
        std::set<sim::SimObject*> seen;
        const auto collect = [&](auto&& self, sim::SimObject* obj) -> void {
            if (!obj || !seen.insert(obj).second)
                return;
            pending.push_back(reference(*obj));
            if (select && selectionMode == 1) {
                if (auto* group = dynamic_cast<sim::SimSet*>(obj)) {
                    for (auto* child : group->objects())
                        self(self, child);
                }
            }
        };
        collect(collect, &object);
        for (const auto& ref : pending) {
            if (auto* obj = ref.resolve(*game); obj && obj->isSelected() != select) {
                obj->setSelected(select);
                callback(select ? "::onSelected" : "::onUnselected", obj);
                if (life.expired())
                    return;
            }
        }
    }

    bool event(const gui::Event& e) override {
        if (active && e.type == gui::EventType::Down && e.button == 1 && game &&
            !gameLife.expired()) {
            rebuild();
            const auto point = globalToLocal(e.point);
            const auto index = point.y >= 0 && cellSize.y > 0 ? point.y / cellSize.y : -1;
            if (index < 0 || std::size_t(index) >= rows.size())
                return true;
            const auto row = rows[index];
            const auto life = lifetime();
            const auto click = [this, life, row, point, control = e.control, shift = e.shift] {
                if (life.expired() || gameLife.expired())
                    return;
                auto* object = row.object.resolve(*game);
                if (!object)
                    return;
                const bool ctrl =
                    control || script::toBool(game->console().getVariable("ME::Mod1"));
                const bool range = shift || script::toBool(game->console().getVariable("ME::Mod2"));
                if (point.x >= row.depth * iconWidth() && point.x < (row.depth + 1) * iconWidth()) {
                    if (auto* group = dynamic_cast<sim::SimSet*>(object))
                        group->setExpanded(!group->isExpanded());
                } else if (point.x >= 2 + (row.depth + 1) * iconWidth()) {
                    if (range && object->getGroup()) {
                        const auto siblings = object->getGroup()->objects();
                        const auto hit = std::ranges::find(siblings, object);
                        const auto first =
                            std::ranges::find_if(siblings, [](const sim::SimObject* sibling) {
                                return sibling->isSelected();
                            });
                        if (first != siblings.end()) {
                            std::vector<WorldObject> pending;
                            for (auto at = std::min(hit, first); at <= std::max(hit, first); ++at)
                                pending.push_back(reference(**at));
                            for (const auto& ref : pending)
                                if (auto* sibling = ref.resolve(*game)) {
                                    selectObject(*sibling, true);
                                    if (life.expired())
                                        return;
                                }
                            rebuild();
                            return;
                        }
                    }
                    if (!ctrl)
                        clearSelection();
                    if (life.expired())
                        return;
                    object = row.object.resolve(*game);
                    if (object)
                        selectObject(*object, !ctrl || !object->isSelected());
                    if (life.expired())
                        return;
                }
                rebuild();
            };
            if (canvas())
                canvas()->afterDispatch(click);
            else
                click();
            return true;
        }
        return gui::Array::event(e);
    }

protected:
    bool drawCell(gfx::SoftwareSurface& s, Point2I p, Point2I cell, bool, bool) const override {
        if (!game || gameLife.expired() || cell.y < 0 || std::size_t(cell.y) >= rows.size())
            return true;
        const auto& row = rows[cell.y];
        auto* object = row.object.resolve(*game);
        if (!object)
            return true;
        const bool chosen = object->isSelected();
        if (chosen)
            s.fill({p.x, p.y, p.x + cellSize.x - 1, p.y + cellSize.y - 1}, 255);
        const auto f = chosen && highlight ? highlight : font;
        if (!f)
            return true;
        const auto art = chosen ? selectedBranches : branches;
        if (art) {
            const auto drawBranch = [&](Int32 index, Int32 depth) {
                return index < 0 || s.blit(art->bitmaps[index], {p.x + depth * iconWidth(), p.y});
            };
            auto* group = dynamic_cast<sim::SimSet*>(object);
            Int32 index = group && !group->empty() ? (group->isExpanded() ? 3 : 7) : -1;
            if (cell.y == 0 || row.depth == 0) {
                if (!row.last)
                    ++index;
            } else
                index += row.last ? 2 : 3;
            if (!drawBranch(index, row.depth))
                return false;
            for (auto parent = row.parent; parent >= 0; parent = rows[parent].parent) {
                if (!rows[parent].last && !drawBranch(11, rows[parent].depth))
                    return false;
            }
        }
        const auto* type = game->classes().find(typeid(*object));
        return s.text(*f,
                      {p.x + (row.depth + 1) * iconWidth(), p.y - 2},
                      std::format("{}: {} - {}",
                                  object->getId(),
                                  object->getName(),
                                  type ? type->name : "SimObject"));
    }
};
}

bool registerMissionEditorGuiClasses(Persistent::ClassRegistry& r) {
    return r.registerClass<METextList>("METextList", makeFourCC('M', 'e', 't', 'L')) &&
           r.registerClass<MEButton>("MEButton", makeFourCC('M', 'e', 'b', 'T')) &&
           r.registerClass<MEPopup>("MEPopup", makeFourCC('M', 'e', 'p', 'L')) &&
           r.registerClass<MEPopupButton>("MEPopupButton", makeFourCC('M', 'e', 'p', 'b')) &&
           r.registerClass<MECheck>("MECheck", makeFourCC('M', 'e', 'c', 'K')) &&
           r.registerClass<MERadio>("MERadio", makeFourCC('M', 'e', 'r', 'b')) &&
           r.registerClass<METextEdit>("METextEdit", makeFourCC('M', 'e', 'T', 'e')) &&
           r.registerClass<MEInspectTagList>("MEInspectTagList", makeFourCC('M', 'e', 'I', 'T')) &&
           r.registerClass<MEObjectTree>("MEObjectList", makeFourCC('M', 'e', 'O', 'L'));
}

bool bindMissionEditorGuiControl(gui::Control& control,
                                 sim::SimGame& game,
                                 ResourceManager& resources) {
    const auto font = [&](std::string_view name) -> std::shared_ptr<const gfx::BitmapFont> {
        const auto loaded = resources.load<gfx::BitmapFont>(name);
        return loaded ? std::make_shared<gfx::BitmapFont>(*loaded) : nullptr;
    };
    if (auto* popup = dynamic_cast<MEPopup*>(&control)) {
        popup->game = &game;
        popup->gameLife = game.lifetime();
    }
    if (auto* tree = dynamic_cast<MEObjectTree*>(&control)) {
        tree->game = &game;
        tree->gameLife = game.lifetime();
        if (tree->getName().empty())
            tree->assignName("MissionObjectList");
        tree->font = font("mefont.pft");
        tree->highlight = font("mefonthl.pft");
        const auto art = [&](std::string_view name) -> std::shared_ptr<const gfx::BitmapArray> {
            const auto loaded = resources.load<gfx::BitmapArray>(name);
            if (!loaded || loaded->bitmaps.size() < 12)
                return {};
            auto result = std::make_shared<gfx::BitmapArray>(*loaded);
            for (auto& bitmap : result->bitmaps)
                bitmap.attributes |= gfx::BitmapTransparent;
            return result;
        };
        tree->branches = art("METreeView.PBA");
        tree->selectedBranches = art("METreeViewSel.PBA");
        if (!tree->font || !tree->highlight || !tree->branches || !tree->selectedBranches)
            return false;
    }
    if (auto* inspector = dynamic_cast<MEInspectTagList*>(&control)) {
        inspector->game = &game;
        inspector->gameLife = game.lifetime();
        if (inspector->getName().empty())
            inspector->assignName("InspectTagList");
        inspector->font = font("mefont.pft");
        inspector->highlight = font("mefonthl.pft");
        if (!inspector->font || !inspector->highlight)
            return false;
    }
    return true;
}

void installMissionEditorGuiCommands(Console& console,
                                     const std::shared_ptr<gui::Runtime>& runtime,
                                     sim::SimGame& game) {
    const std::weak_ptr<gui::Runtime> weak = runtime;
    const auto gameLife = game.lifetime();
    // Runtime-owned GUI graphs are not simulation-manager objects. Route the
    // native postAction command through the live graph before world lookup.
    console.addCommand(
        0,
        "postAction",
        [weak, gameLife, &game](
            Console&, Int32, std::span<const std::string> argv) -> std::optional<std::string> {
            if (gameLife.expired() || argv.size() != 4)
                return "False";
            const auto ui = weak.lock();
            auto* control = ui && ui->running() ? ui->findControl(argv[1]) : nullptr;
            auto* object =
                control ? static_cast<sim::SimObject*>(control) : game.findObject(argv[1]);
            if (!object)
                return "False";
            auto action = integer(argv[2]);
            if (!action) {
                if (equalIgnoreCase(argv[2], "Attach"))
                    action = 103;
                else if (equalIgnoreCase(argv[2], "NextCamera"))
                    action = 101;
                else
                    return "False";
            }
            auto* client = game.world(sim::WorldType::Client);
            auto* owner = object->getManager();
            if (!owner && control && client)
                owner = &client->manager();
            if (!owner)
                return "False";
            float value{};
            const auto parsed =
                std::from_chars(argv[3].data(), argv[3].data() + argv[3].size(), value);
            if (parsed.ec != std::errc{} || parsed.ptr != argv[3].data() + argv[3].size()) {
                auto* reference = owner->findObject(argv[3]);
                if (!reference)
                    return "False";
                value = float(reference->getId());
            }
            if (!std::isfinite(value))
                return "False";
            if (auto* viewport = dynamic_cast<gui::TSControl*>(object); viewport && client) {
                if (*action == 103) {
                    if (value < 0 ||
                        double(value) > double(std::numeric_limits<sim::SimObjectId>::max()))
                        return "False";
                    auto* camera = owner->findObject(sim::SimObjectId(value));
                    return camera && viewport->attach(*client, camera) ? "True" : "False";
                }
                if (*action == 101)
                    return viewport->nextCamera(*client) ? "True" : "False";
            }
            sim::SimActionEvent event;
            event.action = *action;
            event.fValue = value;
            event.device = sim::ActionDigital;
            event.time = owner->getCurrentTime();
            return (control ? control->processEvent(event)
                            : owner->sendEvent(object->address(), event))
                       ? "True"
                       : "False";
        });
    runtime->loadFailed = [&game, gameLife](std::string_view path) {
        if (!gameLife.expired() && equalIgnoreCase(FileName(path).getTitle(), "editor.gui")) {
            game.console().setVariable("ME::Loaded", "False");
            game.console().print("Mission editor GUI could not be loaded.");
        }
    };
    const std::array names{"Popup::addLine",
                           "Popup::clear",
                           "Popup::setSelected",
                           "Popup::getSelected",
                           "PopupButton::makeCurrent",
                           "MissionObjectList::ClearDisplayGroups",
                           "MissionObjectList::AddDisplayGroup",
                           "MissionObjectList::SetSelMode",
                           "MissionObjectList::ExpandToObject",
                           "MissionObjectList::Inspect",
                           "MissionObjectList::Apply",
                           "MissionObjectList::ClearSelection",
                           "MissionObjectList::SelectObject",
                           "MissionObjectList::UnselectObject"};
    for (const std::string_view name : names) {
        console.addCommand(
            0,
            name,
            [weak, gameLife, &game, name](
                Console&, Int32, std::span<const std::string> argv) -> std::optional<std::string> {
                const auto ui = weak.lock();
                if (!ui || !ui->running() || gameLife.expired())
                    return "False";
                const auto yes = [](bool result) {
                    return result ? "True" : "False";
                };
                if (name.starts_with("Popup")) {
                    auto* popup = argv.size() > 1 ? dynamic_cast<MEPopup*>(ui->findControl(argv[1]))
                                                  : nullptr;
                    if (!popup)
                        return "False";
                    if (name == "Popup::clear" && argv.size() == 2) {
                        popup->list().clear();
                        popup->text.clear();
                        return "True";
                    }
                    if (name == "Popup::getSelected" && argv.size() == 2)
                        return std::to_string(popup->selectedId());
                    if (name == "Popup::addLine" && argv.size() == 4) {
                        const auto id = integer(argv[3]);
                        return yes(id && popup->list().insert({*id, argv[2]}));
                    }
                    if (name == "Popup::setSelected" && argv.size() == 3) {
                        const auto id = integer(argv[2]);
                        if (!id)
                            return "False";
                        popup->setSelected(*id);
                        return "True";
                    }
                    if (name == "PopupButton::makeCurrent" && argv.size() == 2) {
                        if (auto* button = dynamic_cast<MEPopupButton*>(popup)) {
                            button->makeCurrent();
                            return "True";
                        }
                    }
                    return "False";
                }
                auto* tree = dynamic_cast<MEObjectTree*>(ui->findControl("MissionObjectList"));
                if (!tree)
                    return "False";
                const auto treeLife = tree->lifetime();
                const auto command = name.substr(name.find("::") + 2);
                if (command == "ClearDisplayGroups" && argv.size() == 1) {
                    tree->roots.clear();
                    tree->rebuild();
                    return "True";
                }
                if (command == "SetSelMode" && argv.size() == 2) {
                    const auto mode = integer(argv[1]);
                    if (!mode || *mode < 0 || *mode > 1)
                        return "False";
                    tree->selectionMode = *mode;
                    return "True";
                }
                if (command == "ClearSelection" && argv.size() == 1) {
                    tree->clearSelection();
                    return "True";
                }
                auto* inspector =
                    dynamic_cast<MEInspectTagList*>(ui->findControl("InspectTagList"));
                if (command == "Apply" && argv.size() == 1)
                    return yes(inspector && inspector->apply());
                if (argv.size() != 3)
                    return "False";
                const auto type = worldType(argv[1]);
                auto* world = type ? game.world(*type) : nullptr;
                if (!world)
                    return "False";
                if (command == "Inspect")
                    return yes(inspector && inspector->inspect(*type, argv[2]));
                auto* object = world->manager().findObject(argv[2]);
                if (!object)
                    return "False";
                if (command == "AddDisplayGroup") {
                    if (!dynamic_cast<sim::SimGroup*>(object))
                        return "False";
                    tree->roots.push_back(tree->reference(*object));
                } else if (command == "ExpandToObject") {
                    std::set<sim::SimObject*> seen;
                    for (auto* group = object->getGroup(); group && seen.insert(group).second;
                         group = group->getGroup())
                        group->setExpanded(true);
                } else if (command == "SelectObject" || command == "UnselectObject") {
                    tree->selectObject(*object, command == "SelectObject");
                } else
                    return "False";
                if (!treeLife.expired())
                    tree->rebuild();
                return "True";
            });
    }
}
}
