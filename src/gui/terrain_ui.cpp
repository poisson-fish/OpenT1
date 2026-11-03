#include <opent1/gui/terrain_tools.hpp>
#include <ranges>
#include <opent1/core/file_stream.hpp>
#include <charconv>
#include <cctype>
#include <set>

namespace opent1::gui {
namespace {
constexpr UInt32 uiAccept = 65536, uiCancel = 65537, uiChoose = 65538, uiUp = 65539;
constexpr UInt32 tedMenu = 65550, lsMenu = 65551, scriptTab = 65552, rulesTab = 65553,
                 addRule = 65554, editRule = 65555, deleteRule = 65556, typeList = 65557;
constexpr UInt32 lsLoadScript = 20000, lsSaveScript = 20001, lsLoadRules = 20002,
                 lsSaveRules = 20003, lsLoadBitmap = 20004, lsSaveBitmap = 20005,
                 lsTextures = 20006, lsPreview = 20007, lsApply = 20008, lsTexture = 20009,
                 lsAbout = 20010;

bool saving(TerrainFileAction a) {
    return a == TerrainFileAction::Save || a == TerrainFileAction::SaveSelection ||
           a == TerrainFileAction::SaveScript || a == TerrainFileAction::SaveRules ||
           a == TerrainFileAction::SaveBitmap;
}

bool browserPath(const std::filesystem::path& path) {
    const auto text = path.generic_string();
    return !text.empty() && text.size() <= 4096 && text.find('\0') == text.npos;
}

struct BrowserTarget {
    bool exists{};
    std::vector<std::byte> bytes;
    bool operator==(const BrowserTarget&) const = default;
};

std::optional<BrowserTarget> browserTarget(const std::filesystem::path& path,
                                           const TerrainTools::Reader& reader) {
    if (reader) {
        auto bytes = reader(path.generic_string());
        if (bytes && bytes->size() > 256U * 1024U * 1024U) {
            return {};
        }
        return BrowserTarget{bool(bytes), bytes ? std::move(*bytes) : std::vector<std::byte>{}};
    }
    std::error_code ec;
    const bool exists = std::filesystem::exists(path, ec);
    if (ec) {
        return {};
    }
    if (!exists) {
        return BrowserTarget{};
    }
    FileRStream input(path);
    if (input.getStatus() != STRM_OK || input.getSize() > 256U * 1024U * 1024U) {
        return {};
    }
    BrowserTarget result{true, std::vector<std::byte>(input.getSize())};
    if (!input.readBytes(result.bytes)) {
        return {};
    }
    return result;
}

std::string_view extension(TerrainFileAction a) {
    switch (a) {
        case TerrainFileAction::Open:
        case TerrainFileAction::Save:
            return ".ted";
        case TerrainFileAction::LoadSelection:
        case TerrainFileAction::SaveSelection:
            return ".map";
        case TerrainFileAction::LoadScript:
        case TerrainFileAction::SaveScript:
            return ".pls";
        case TerrainFileAction::LoadRules:
        case TerrainFileAction::SaveRules:
            return ".plr";
        case TerrainFileAction::LoadBitmap:
        case TerrainFileAction::SaveBitmap:
            return ".bmp";
        case TerrainFileAction::LoadPalette:
            return ".ppl";
        case TerrainFileAction::LoadMaterials:
        case TerrainFileAction::LoadTextures:
            return ".dml";
    }
    return {};
}

UInt8& color(TerrainDisplayOptions& d, TerrainColor c) {
    switch (c) {
        case TerrainColor::SelectionFill:
            return d.selectionColor;
        case TerrainColor::SelectionFrame:
            return d.selectionFrameColor;
        case TerrainColor::HighlightFill:
            return d.highlightColor;
        case TerrainColor::HighlightFrame:
            return d.highlightFrameColor;
        case TerrainColor::ShadowFill:
            return d.shadowColor;
        case TerrainColor::ShadowFrame:
            return d.shadowFrameColor;
    }
    throw std::invalid_argument("terrain color");
}

class Colors final : public Array {
public:
    Colors() {
        cells = {16, 16};
        cellSize = {16, 16};
    }

protected:
    bool drawCell(gfx::SoftwareSurface& surface,
                  Point2I p,
                  Point2I cell,
                  bool selectedCell,
                  bool) const override {
        const RectI rect{p.x, p.y, p.x + 15, p.y + 15};
        surface.fill(rect, UInt8(cell.y * 16 + cell.x));
        if (selectedCell) {
            surface.rectangle(rect, 255);
            surface.rectangle({p.x + 1, p.y + 1, p.x + 14, p.y + 14}, 0);
        }
        return true;
    }
};

class UiRoot final : public Control {
public:
    std::function<std::function<void()>()> save;
    std::function<void()> cancel, accept;

    bool event(const Event& e) override {
        if (e.type == EventType::KeyDown && e.key == Key::Escape) {
            if (canvas()) {
                canvas()->afterDispatch(cancel);
            }
            return true;
        }
        if (e.type == EventType::KeyDown && e.key == Key::Return) {
            if (canvas()) {
                canvas()->afterDispatch(accept);
            }
            return true;
        }
        return Control::event(e);
    }

    std::function<void()> checkpoint() override {
        auto base = Control::checkpoint();
        auto extra = save ? save() : std::function<void()>{};
        return [base = std::move(base), extra = std::move(extra)] {
            base();
            if (extra) {
                extra();
            }
        };
    }
};

std::optional<std::vector<TerrainBrowserEntry>> directory(const std::filesystem::path& path) {
    std::error_code ec;
    std::filesystem::directory_iterator it(path, ec), end;
    if (ec) {
        return {};
    }
    std::vector<TerrainBrowserEntry> result;
    for (; it != end; it.increment(ec)) {
        if (ec || result.size() >= 4096) {
            return {};
        }
        const auto& entry = *it;
        const auto name = entry.path().filename().generic_string();
        if (name.size() > 255) {
            return {};
        }
        const bool dir = entry.is_directory(ec);
        if (ec) {
            return {};
        }
        if (dir || entry.is_regular_file(ec)) {
            result.push_back({name, dir});
        }
        if (ec) {
            return {};
        }
    }
    if (ec) {
        return {};
    }
    return result;
}
}

struct TerrainUiPanel {
    enum class Kind { Menu, Name, Choose, Remove, Browser, Color, Confirm, Info, Types };
    UiRoot root;
    Scroll scroll;
    TextList list;
    TextEdit text, second;
    Text title;
    Colors colors;
    std::vector<std::unique_ptr<Button>> buttons;
    Kind kind{};
    Canvas* canvas{};
    std::vector<TerrainMenuItem> menu;
    std::vector<TerrainBrowserEntry> files;
    std::vector<std::string> names;
    std::filesystem::path directory;
    TerrainFileAction fileAction{};
    TerrainColor color{};
    UInt32 command{};
    UInt64 revision{};
    std::size_t rule{};
    bool landscape{};
    std::vector<Point2I> selection;
    std::optional<std::string> ruleSignature;
    bool overwrite{};
    std::function<bool()> overwriteSource;
    std::optional<BrowserTarget> overwriteTarget;
};

struct TerrainTools::Ui {
    using Panel = TerrainUiPanel;
    using Kind = Panel::Kind;
    std::vector<std::shared_ptr<Panel>> panels;
    std::vector<std::unique_ptr<Button>> buttons;
    Control rulesRoot;
    Scroll rulesScroll;
    TextList rules;
    LandscapeTab tab{LandscapeTab::Script};
    std::filesystem::path currentFile;
    bool landscapeVisible{true};
};

void TerrainTools::initializeUi() {
    ui_ = std::make_shared<Ui>();
    const auto button = [&](Control& root, std::string text, UInt32 id, Point2I p, Point2I size) {
        auto b = std::make_unique<Button>();
        b->text = std::move(text);
        b->message = id;
        b->tag(id);
        if (!b->resize(p, size) || !root.addObject(*b)) {
            throw std::runtime_error("terrain UI toolbar");
        }
        ui_->buttons.push_back(std::move(b));
    };
    button(tedRoot_, "Menu", tedMenu, {264, 0}, {56, 24});
    constexpr std::array modes{"Select", "Raise", "Height", "Pin", "Rotate", "Smooth"};
    for (auto i : std::views::iota(UInt32{0}, UInt32{6})) {
        button(tedRoot_, modes[i], 65700 + i, {320 + Int32(i) * 48, 0}, {48, 24});
    }
    button(landscapeRoot_, "Menu", lsMenu, {264, 0}, {56, 24});
    button(landscapeRoot_, "Script", scriptTab, {320, 0}, {64, 24});
    button(landscapeRoot_, "Rules", rulesTab, {384, 0}, {64, 24});
    button(ui_->rulesRoot, "Add", addRule, {0, 0}, {72, 24});
    button(ui_->rulesRoot, "Edit", editRule, {72, 0}, {72, 24});
    button(ui_->rulesRoot, "Remove", deleteRule, {144, 0}, {80, 24});
    ui_->rulesScroll.horizontalPolicy = Scroll::Dynamic;
    ui_->rulesScroll.verticalPolicy = Scroll::Dynamic;
    ui_->rules.cellSize = {300, 24};
    ui_->rules.message = uiChoose;
    ui_->rules.noDuplicates = false;
    if (!landscapeRoot_.addObject(ui_->rulesRoot) || !ui_->rulesRoot.addObject(ui_->rulesScroll) ||
        !ui_->rulesScroll.addObject(ui_->rules)) {
        throw std::runtime_error("landscape rules UI");
    }
    ui_->rulesRoot.visible(false);
    // The shared dialog's type list uses the same mapper descriptions as Rules.
    if (!editorModal_.addObject(editorScroll_) || !editorScroll_.addObject(editorDialog_)) {
        throw std::runtime_error("terrain dialog scroll");
    }
    button(editorModal_, "Types", typeList, {208, 0}, {72, 24});
    // The original display dialog exposes six independent palette indices.
    constexpr std::array names{"Selection fill",
                               "Selection frame",
                               "Highlight fill",
                               "Highlight frame",
                               "Shadow fill",
                               "Shadow frame"};
    for (auto i : std::views::iota(UInt32{0}) | std::views::take_while([&](UInt32 i) {
                      return i < names.size();
                  })) {
        button(displayRoot_,
               names[i],
               65600 + i,
               {16 + Int32(i % 2) * 154, 148 + Int32(i / 2) * 28},
               {150, 24});
    }
}

bool TerrainTools::resizeUi(bool ls, Point2I size) {
    if (!ui_) {
        return true;
    }
    const auto width = std::min(88, (size.x - 56) / 3);
    for (auto i : std::views::iota(std::size_t{0}, std::size_t{3})) {
        if (!buttons_[i + (ls ? 3 : 0)]->resize({Int32(i) * width, 0}, {width, 24})) {
            return false;
        }
    }
    auto& root = ls ? landscapeRoot_ : tedRoot_;
    if (auto* menu = root.findTag(ls ? lsMenu : tedMenu);
        menu && !menu->resize({width * 3, 0}, {56, 24})) {
        return false;
    }
    if (!ls) {
        for (auto i : std::views::iota(UInt32{0}, UInt32{6})) {
            if (auto* mode = root.findTag(65700 + i)) {
                mode->visible(size.x >= 608);
            }
        }
        return true;
    }
    const auto middle = size.x / 2;
    return ui_->rulesRoot.resize({middle, 24}, {size.x - middle, size.y - 24}) &&
           ui_->rulesScroll.resize({0, 26}, {size.x - middle, std::max(4, size.y - 50)}) &&
           ui_->rulesScroll.computeSizes();
}

void TerrainTools::fontUi() {
    if (!ui_) {
        return;
    }
    for (auto& b : ui_->buttons) {
        b->font = b->highlightFont = b->disabledFont = font_;
    }
    ui_->rules.font = ui_->rules.highlightFont = font_;
    for (auto& p : ui_->panels) {
        p->list.font = p->list.highlightFont = font_;
        p->text.font = p->text.highlightFont = p->second.font = p->second.highlightFont =
            p->title.font = font_;
        for (auto& b : p->buttons) {
            b->font = b->highlightFont = b->disabledFont = font_;
        }
    }
}

std::vector<std::string> TerrainTools::terrainTypes() const {
    std::vector<std::string> result;
    const auto& table = editor_->mapper.textures();
    if (table.types) {
        return table.descriptions;
    }
    for (const auto& item : editor_->textureRecipe.types) {
        result.push_back(item.second);
    }
    return result;
}

std::vector<std::string> TerrainTools::ruleItems() const {
    const auto types = terrainTypes();
    std::vector<std::string> result;
    const auto& rules = editor_->mapper.rules();
    for (auto i : std::views::iota(std::size_t{0}) | std::views::take_while([&](std::size_t i) {
                      return i < rules.size();
                  })) {
        const auto g = rules[i].group;
        result.push_back(std::format("Rule: {} - {}",
                                     i,
                                     g >= 0 && std::size_t(g) < types.size()
                                         ? types[std::size_t(g)]
                                         : std::format("[undefined type: {}]", g)));
    }
    return result;
}

bool TerrainTools::landscapeTab(LandscapeTab tab) {
    if (!ui_ || (tab != LandscapeTab::Script && tab != LandscapeTab::Rules) ||
        landscape.dialogCount() || landscape.dispatching() || landscape.capture()) {
        return false;
    }
    try {
        const auto names = ruleItems();
        TextList staged;
        staged.noDuplicates = false;
        staged.cellSize = ui_->rules.cellSize;
        for (auto i : std::views::iota(std::size_t{0}) | std::views::take_while([&](std::size_t i) {
                          return i < names.size();
                      })) {
            if (!staged.insert({Int32(i), names[i]})) {
                return false;
            }
        }
        // Refreshing the original Rules page resets its selection.
        auto restore = ui_->rules.checkpoint(), restoreScroll = ui_->rulesScroll.checkpoint();
        ui_->rules.entries = std::move(staged.entries);
        ui_->rules.cells = staged.cells;
        ui_->rules.selected = {-1, -1};
        if (!ui_->rules.resize({}, staged.fields().extent) || !ui_->rulesScroll.computeSizes()) {
            restore();
            restoreScroll();
            return false;
        }
        ui_->tab = tab;
        script.visible(tab == LandscapeTab::Script);
        ui_->rulesRoot.visible(tab == LandscapeTab::Rules);
        landscape.rebuildTabs();
        return true;
    } catch (...) {
        return false;
    }
}

LandscapeTab TerrainTools::landscapeTab() const {
    return ui_->tab;
}

bool TerrainTools::selectRule(std::size_t index) {
    return ui_->tab == LandscapeTab::Rules && index < editor_->mapper.rules().size() &&
           ui_->rules.select({0, Int32(index)});
}

bool TerrainTools::ruleType(std::size_t index) {
    return dialog_ == TerrainDialog::Rule && index < terrainTypes().size() &&
           dialogValue(0, std::to_string(index));
}

std::optional<std::size_t> TerrainTools::ruleTypeSelection() const {
    if (dialog_ != TerrainDialog::Rule) {
        return {};
    }
    const auto value = dialogValue(0);
    if (!value) {
        return {};
    }
    std::size_t index{};
    const auto parsed = std::from_chars(value->data(), value->data() + value->size(), index);
    return parsed.ec == std::errc{} && parsed.ptr == value->data() + value->size() &&
                   index < terrainTypes().size()
               ? std::optional(index)
               : std::nullopt;
}

std::vector<TerrainMenuItem> TerrainTools::menuItems(bool ls) const {
    std::vector<TerrainMenuItem> result;
    const bool terrain = bool(editor_->snapshot()), selection = !editor_->selection().empty(),
               clipboard = !editor_->clipboard().empty();
    const auto add = [&](UInt32 id,
                         std::string group,
                         std::string label,
                         bool enabled = true,
                         bool checked = false) {
        result.push_back({id, std::move(group), std::move(label), enabled, checked});
    };
    if (ls) {
        add(lsLoadScript, "Script", "Load script");
        add(lsSaveScript, "Script", "Save script");
        add(lsPreview, "Script", "Generate preview");
        add(scriptTab, "Tabs", "Script", true, ui_->tab == LandscapeTab::Script);
        add(rulesTab, "Tabs", "Rules", true, ui_->tab == LandscapeTab::Rules);
        add(lsLoadRules, "Rules", "Load rules");
        add(lsSaveRules, "Rules", "Save rules", !editor_->mapper.rules().empty());
        add(addRule, "Rules", "Add rule");
        add(editRule, "Rules", "Edit rule", ui_->rules.selection() != nullptr);
        add(deleteRule, "Rules", "Remove rule", ui_->rules.selection() != nullptr);
        add(lsLoadBitmap, "File", "Load bitmap");
        add(lsSaveBitmap, "File", "Save bitmap", bool(preview.snapshot()));
        add(lsTextures, "File", "Load textures", terrain);
        add(lsApply, "Apply", "Apply landscape", terrain && bool(preview.snapshot()));
        add(lsTexture,
            "Apply",
            "Apply textures",
            terrain && !editor_->mapper.rules().empty() && editor_->mapper.textures().types);
        add(lsAbout, "Help", "About");
        return result;
    }
    add(1000, "File", "New");
    add(1001, "File", "Open");
    add(1003, "File", "Close", terrain);
    add(1004, "File", "Save", terrain);
    add(1013, "File", "Save as", terrain);
    add(1014, "File", "Load palette", terrain);
    add(1015, "File", "Load material list", terrain);
    add(1005, "File", "Exit");
    add(1006, "Edit", "Undo", terrain && editor_->canUndo());
    add(1007, "Edit", "Redo", terrain && editor_->canRedo());
    add(1008, "Edit", "Copy", terrain && selection);
    add(1009, "Edit", "Paste all", terrain && clipboard);
    add(1012, "Edit", "Paste materials", terrain && clipboard);
    add(1011, "Edit", "Paste heights", terrain && clipboard);
    add(1018, "Edit", "Clear selection", terrain && selection);
    add(1019, "Edit", "Float selection", terrain && selection);
    add(1025, "Edit", "Name selection", terrain && selection);
    const auto named = editor_->namedSelections();
    add(1017, "Edit", "Get selection", terrain && !named.empty());
    add(1026, "Edit", "Delete selection", terrain && !named.empty());
    add(1053, "Edit", "Save selection", terrain && selection);
    add(1054, "Edit", "Load selection", terrain);
    add(1027, "Settings", "Settings", terrain);
    add(1022, "Settings", "Display", terrain);
    constexpr std::array values{std::pair{1038U, "Adjust height"},
                                std::pair{1039U, "Set height"},
                                std::pair{1040U, "Material index"},
                                std::pair{1041U, "Flag mask"},
                                std::pair{1042U, "Paste mask"},
                                std::pair{1050U, "Scale value"},
                                std::pair{1048U, "Terrain type"},
                                std::pair{1051U, "Pin detail level"},
                                std::pair{1059U, "Pin detail max"},
                                std::pair{1052U, "Smooth value"}};
    for (auto [id, name] : values) {
        add(id, "Values", name, terrain);
    }
    add(1036, "Brush", "Snap to detail", terrain, editor_->settings().snap);
    add(1037, "Brush", "Auto feather", terrain, editor_->settings().feather);
    if (terrain) {
        for (auto i : std::views::iota(UInt32{0}) | std::views::take_while([&](UInt32 i) {
                          return i < UInt32(editor_->snapshot()->grid.detailCount);
                      })) {
            add(4096 + i,
                "Detail",
                std::format("Level {}", i),
                true,
                editor_->settings().brushDetail == i);
        }
    }
    const auto actions = world::TerrainEditor::actions();
    for (auto i : std::views::iota(UInt32{0}) | std::views::take_while([&](UInt32 i) {
                      return i < actions.size();
                  })) {
        if (i == 1 || i == 5 || i == 9 || i == 11 || i == 12) {
            continue;
        }
        add(2048 + i, "Selection", std::string(actions[i]), terrain && selection);
        add(8192 + i,
            "Left button",
            std::string(actions[i]),
            terrain,
            editor_->settings().leftAction == i);
        add(16384 + i,
            "Right button",
            std::string(actions[i]),
            terrain,
            editor_->settings().rightAction == i);
    }
    constexpr std::array modes{"Select", "Adjust height", "Set height", "Pin", "Rotate", "Smooth"};
    constexpr std::array<UInt32, 6> left{20, 14, 24, 13, 17, 27}, right{6, 10, 28, 4, 18, 28};
    for (auto i : std::views::iota(UInt32{0}, UInt32{6})) {
        add(700 + i,
            "Toolbar",
            modes[i],
            terrain,
            editor_->settings().leftAction == left[i] &&
                editor_->settings().rightAction == right[i]);
    }
    add(706, "Toolbar", "Set terrain type", terrain && selection);
    add(1043, "Misc", "Landscaper", terrain);
    add(1044, "Misc", "List commands", terrain);
    add(1049, "Misc", "Clear pin maps", terrain);
    add(1045, "Misc", "Relight terrain", terrain);
    add(1046, "Misc", "About");
    for (auto i : std::views::iota(UInt32{0}, UInt32{6})) {
        add(65600 + i,
            "Colors",
            std::array{"Selection fill",
                       "Selection frame",
                       "Highlight fill",
                       "Highlight frame",
                       "Shadow fill",
                       "Shadow frame"}[i],
            terrain);
    }
    return result;
}

// All controls are prepared off-canvas. A failed wake/capture release publishes
// no panel and leaves the previous graph and owned model intact.
bool TerrainTools::menu(std::string_view group, bool ls) {
    auto panel = std::make_shared<Ui::Panel>();
    panel->kind = Ui::Kind::Menu;
    panel->landscape = ls;
    panel->canvas = ls ? &landscape : &ted;
    for (auto item : menuItems(ls)) {
        if (group.empty() || item.group == group) {
            panel->names.push_back((item.checked ? "[x] " : "") +
                                   (item.enabled ? std::string{} : "[disabled] ") + item.group +
                                   ": " + item.label);
            panel->menu.push_back(std::move(item));
        }
    }
    if (panel->names.empty()) {
        return false;
    }
    return showUi(panel);
}

bool TerrainTools::showUi(const std::shared_ptr<Ui::Panel>& p) {
    if (!p->canvas || p->canvas->dispatching() || ui_->panels.size() >= 16) {
        return false;
    }
    const auto size = (p->canvas == &ted ? tedRoot_ : landscapeRoot_).fields().extent;
    const auto width = std::min(560, size.x - 16), height = std::min(360, size.y - 16);
    if (width < 248 || height < 100) {
        return false;
    }
    const auto style = [&](Control& c) {
        auto f = c.fields();
        f.style.opaque = 1;
        f.style.fill = 249;
        f.style.border = 1;
        f.style.borderColor = 255;
        return c.configure(f);
    };
    p->root.save = [this] {
        return editor_->checkpoint();
    };
    p->root.cancel = [this] {
        cancelUi();
    };
    p->root.accept = [this] {
        acceptUi();
    };
    p->text.maxBytes = p->second.maxBytes = 4096;
    p->title.text = p->kind == Ui::Kind::Browser   ? p->directory.generic_string()
                    : p->kind == Ui::Kind::Confirm ? "Confirm action"
                    : p->kind == Ui::Kind::Color   ? "Choose palette index"
                                                   : "Terrain editor";
    p->scroll.horizontalPolicy = Scroll::Dynamic;
    p->scroll.verticalPolicy = Scroll::Dynamic;
    p->list.cellSize = {std::max(240, width - 30), 24};
    p->list.message = uiChoose;
    p->list.noDuplicates = false;
    for (auto i : std::views::iota(std::size_t{0}) | std::views::take_while([&](std::size_t i) {
                      return i < p->names.size();
                  })) {
        if (!p->list.insert({Int32(i), p->names[i]})) {
            return false;
        }
    }
    if (!style(p->root) || !p->root.resize({8, 8}, {width, height}) ||
        !p->title.resize({8, 4}, {width - 16, 20}) || !p->root.addObject(p->title)) {
        return false;
    }
    const auto editRows =
        p->kind == Ui::Kind::Browser && p->fileAction == TerrainFileAction::LoadTextures ? 2 : 1;
    const auto listHeight = std::max(4, height - 86 - (editRows - 1) * 26);
    if (p->kind == Ui::Kind::Color) {
        if (!p->colors.resize({}, {256, 256}) ||
            !p->scroll.resize({8, 28}, {width - 16, listHeight}) || !p->root.addObject(p->scroll) ||
            !p->scroll.addObject(p->colors) || !p->scroll.computeSizes()) {
            return false;
        }
        p->colors.message = uiChoose;
    } else if (!p->scroll.resize({8, 28}, {width - 16, listHeight}) ||
               !p->root.addObject(p->scroll) || !p->scroll.addObject(p->list) ||
               !p->scroll.computeSizes()) {
        return false;
    }
    const auto y = height - 54 - (editRows - 1) * 26;
    if (!p->text.resize({8, y}, {width - 16, 24}) || !p->root.addObject(p->text)) {
        return false;
    }
    p->text.visible(p->kind == Ui::Kind::Name || p->kind == Ui::Kind::Browser);
    if (editRows == 2 &&
        (!p->second.resize({8, y + 26}, {width - 16, 24}) || !p->root.addObject(p->second))) {
        return false;
    }
    const auto button = [&](const char* label, UInt32 message, Int32 x) {
        auto b = std::make_unique<Button>();
        b->text = label;
        b->message = message;
        if (!b->resize({x, height - 28}, {76, 24}) || !p->root.addObject(*b)) {
            return false;
        }
        p->buttons.push_back(std::move(b));
        return true;
    };
    if (!button(p->kind == Ui::Kind::Confirm ? "Yes" : "OK", uiAccept, 8) ||
        !button(p->kind == Ui::Kind::Confirm ? "No" : "Cancel", uiCancel, 88)) {
        return false;
    }
    if (p->kind == Ui::Kind::Browser && !button("Up", uiUp, 168)) {
        return false;
    }
    ui_->panels.reserve(ui_->panels.size() + 1);
    ui_->panels.push_back(p);
    fontUi();
    if (!p->canvas->pushDialog(p->root)) {
        ui_->panels.pop_back();
        return false;
    }
    return true;
}

bool TerrainTools::selectionDialog(TerrainSelectionDialog kind) {
    if (!editor_->snapshot() ||
        (kind == TerrainSelectionDialog::Name && editor_->selection().empty())) {
        return false;
    }
    auto p = std::make_shared<Ui::Panel>();
    p->canvas = &ted;
    p->kind = kind == TerrainSelectionDialog::Name     ? Ui::Kind::Name
              : kind == TerrainSelectionDialog::Choose ? Ui::Kind::Choose
                                                       : Ui::Kind::Remove;
    p->names = editor_->namedSelections();
    p->revision = editor_->revision();
    p->selection = editor_->selection();
    if (kind != TerrainSelectionDialog::Name && p->names.empty()) {
        return false;
    }
    return showUi(p);
}

bool TerrainTools::colorDialog(TerrainColor which) {
    if (UInt32(which) > UInt32(TerrainColor::ShadowFrame)) {
        return false;
    }
    auto p = std::make_shared<Ui::Panel>();
    p->kind = Ui::Kind::Color;
    p->canvas = &ted;
    p->color = which;
    const auto value = color(displayDraft_ ? *displayDraft_ : view.display, which);
    p->colors.selected = {value % 16, value / 16};
    return showUi(p);
}

bool TerrainTools::browser(TerrainFileAction action, std::filesystem::path path) {
    if (UInt32(action) > UInt32(TerrainFileAction::LoadTextures)) {
        return false;
    }
    if (path.empty()) {
        std::error_code ec;
        path = std::filesystem::current_path(ec);
        if (ec) {
            return false;
        }
    }
    auto p = std::make_shared<Ui::Panel>();
    p->kind = Ui::Kind::Browser;
    p->canvas = &ted;
    p->fileAction = action;
    try {
        if (!browserPath(path)) {
            return false;
        }
        auto entries = readDirectory ? readDirectory(path) : directory(path);
        if (!entries) {
            return false;
        }
        if (!prepareBrowser(*p, path, std::move(*entries))) {
            return false;
        }
        return showUi(p);
    } catch (...) {
        return false;
    }
}

bool TerrainTools::prepareBrowser(Ui::Panel& p,
                                  const std::filesystem::path& path,
                                  std::vector<TerrainBrowserEntry> entries) {
    if (entries.size() > 4096 || !browserPath(path)) {
        return false;
    }
    std::set<std::string> used;
    for (const auto& e : entries) {
        if (e.name.empty() || e.name.size() > 255 || e.name == "." || e.name == ".." ||
            e.name.find_first_of("/\\\0", 0, 3) != e.name.npos || !used.insert(e.name).second) {
            return false;
        }
    }
    std::erase_if(entries, [&](const auto& e) {
        if (e.directory) {
            return false;
        }
        auto suffix = std::filesystem::path(e.name).extension().string();
        std::ranges::transform(suffix, suffix.begin(), [](unsigned char c) {
            return char(std::tolower(c));
        });
        return suffix != extension(p.fileAction);
    });
    std::ranges::sort(entries, [](const auto& a, const auto& b) {
        return a.directory != b.directory ? a.directory > b.directory : a.name < b.name;
    });
    std::vector<std::string> names;
    for (const auto& e : entries) {
        names.push_back((e.directory ? "[dir] " : "") + e.name);
    }
    p.directory = path;
    p.files = std::move(entries);
    p.names = std::move(names);
    return true;
}

bool TerrainTools::browse(const std::filesystem::path& path) {
    if (ui_->panels.empty()) {
        return false;
    }
    const auto p = ui_->panels.back();
    if (p->kind != Ui::Kind::Browser || p->overwrite) {
        return false;
    }
    try {
        if (!browserPath(path)) {
            return false;
        }
        auto entries = readDirectory ? readDirectory(path) : directory(path);
        if (!entries) {
            return false;
        }
        Ui::Panel staged;
        staged.fileAction = p->fileAction;
        if (!prepareBrowser(staged, path, std::move(*entries))) {
            return false;
        }
        TextList list;
        list.noDuplicates = false;
        list.cellSize = p->list.cellSize;
        for (auto i : std::views::iota(std::size_t{0}) | std::views::take_while([&](std::size_t i) {
                          return i < staged.names.size();
                      })) {
            if (!list.insert({Int32(i), staged.names[i]})) {
                return false;
            }
        }
        auto restore = p->list.checkpoint();
        auto scroll = p->scroll.checkpoint();
        p->list.entries = std::move(list.entries);
        p->list.cells = list.cells;
        p->list.selected = {-1, -1};
        if (!p->list.resize({}, list.fields().extent) || !p->scroll.computeSizes()) {
            restore();
            scroll();
            return false;
        }
        p->directory = std::move(staged.directory);
        p->names = std::move(staged.names);
        p->files = std::move(staged.files);
        p->title.text = p->directory.generic_string();
        p->text.setText("");
        return true;
    } catch (...) {
        return false;
    }
}

std::vector<std::string> TerrainTools::uiItems() const {
    return ui_->panels.empty() ? std::vector<std::string>{} : ui_->panels.back()->names;
}

bool TerrainTools::uiText(std::string text) {
    if (ui_->panels.empty()) {
        return false;
    }
    auto& p = *ui_->panels.back();
    return !p.overwrite && (p.kind == Ui::Kind::Name || p.kind == Ui::Kind::Browser) &&
           p.text.setText(std::move(text));
}

bool TerrainTools::uiSelect(std::size_t index) {
    if (ui_->panels.empty()) {
        return false;
    }
    const auto p = ui_->panels.back();
    if (p->overwrite) {
        return false;
    }
    if (p->kind == Ui::Kind::Color) {
        return index < 256 && p->colors.select({Int32(index % 16), Int32(index / 16)});
    }
    if (index >= p->names.size()) {
        return false;
    }
    p->list.selected = {0, Int32(index)};
    if (p->kind == Ui::Kind::Browser) {
        return p->text.setText(p->files[index].directory ? std::string{} : p->files[index].name);
    }
    if (p->kind == Ui::Kind::Name) {
        return p->text.setText(p->names[index]);
    }
    return true;
}

bool TerrainTools::cancelUi() {
    if (!ui_ || ui_->panels.empty()) {
        return false;
    }
    auto p = ui_->panels.back();
    if (p->canvas->top() != &p->root) {
        return false;
    }
    if (p->overwrite) {
        auto title = p->directory.generic_string();
        p->overwrite = false;
        p->overwriteSource = {};
        p->overwriteTarget.reset();
        p->title.text.swap(title);
        p->buttons[0]->text = "OK";
        p->buttons[1]->text = "Cancel";
        return true;
    }
    if (!p->canvas->popDialog()) {
        return false;
    }
    ui_->panels.pop_back();
    return true;
}

bool TerrainTools::uiOpen() const {
    return ui_ && !ui_->panels.empty();
}

bool TerrainTools::acceptUi() {
    if (ui_->panels.empty()) {
        return false;
    }
    const auto p = ui_->panels.back();
    if (p->canvas->top() != &p->root) {
        return false;
    }
    const auto index = p->list.selected.y;
    if (p->kind == Ui::Kind::Menu) {
        if (index < 0 || std::size_t(index) >= p->menu.size() ||
            !p->menu[std::size_t(index)].enabled) {
            return false;
        }
        const auto id = p->menu[std::size_t(index)].id;
        if (!cancelUi()) {
            return false;
        }
        if (invoke(id)) {
            return true;
        }
        return showUiAgain(p);
    }
    if (p->kind == Ui::Kind::Browser && index >= 0 && std::size_t(index) < p->files.size() &&
        p->files[std::size_t(index)].directory && p->text.text.empty()) {
        return browse(p->directory / p->files[std::size_t(index)].name);
    }
    auto restore = view.checkpoint();
    auto scriptRestore = script.checkpoint();
    const auto oldPath = scriptPath_;
    const auto oldFormat = scriptFormat_;
    const auto oldFile = ui_->currentFile;
    const auto oldDraft = displayDraft_;
    std::filesystem::path stagedPath;
    if (p->kind == Ui::Kind::Browser) {
        try {
            stagedPath = (p->directory / std::filesystem::path(p->text.text)).lexically_normal();
            if (p->overwrite) {
                auto target = browserTarget(stagedPath, readFile);
                restore();
                scriptRestore();
                scriptPath_ = oldPath;
                scriptFormat_ = oldFormat;
                ui_->currentFile = oldFile;
                displayDraft_ = oldDraft;
                if (!target || target != p->overwriteTarget || !p->overwriteSource ||
                    !p->overwriteSource()) {
                    return false;
                }
            } else if (!p->text.text.empty() && saving(p->fileAction)) {
                auto target = browserTarget(stagedPath, readFile);
                restore();
                scriptRestore();
                scriptPath_ = oldPath;
                scriptFormat_ = oldFormat;
                ui_->currentFile = oldFile;
                displayDraft_ = oldDraft;
                if (!target) {
                    return false;
                }
                if (target->exists) {
                    auto title = "Overwrite " + p->text.text + "?";
                    const auto revision = editor_->revision();
                    const auto selected = editor_->selection();
                    const auto source = script.text;
                    const auto format = scriptFormat_;
                    const auto rules = world::writeLandscapeRules(editor_->mapper.rules());
                    const auto image = p->fileAction == TerrainFileAction::SaveBitmap
                                           ? preview.snapshot()
                                           : nullptr;
                    const auto action = p->fileAction;
                    const auto pathText = p->text.text;
                    p->overwriteSource = [this,
                                          revision,
                                          selected,
                                          source,
                                          format,
                                          rules,
                                          image,
                                          action,
                                          pathText,
                                          panel = std::weak_ptr<Ui::Panel>(p)] {
                        const auto currentPanel = panel.lock();
                        if (!currentPanel || currentPanel->text.text != pathText) {
                            return false;
                        }
                        if (editor_->revision() != revision || editor_->selection() != selected) {
                            return false;
                        }
                        if (action == TerrainFileAction::SaveScript &&
                            (script.text != source || scriptFormat_ != format)) {
                            return false;
                        }
                        if (action == TerrainFileAction::SaveRules &&
                            world::writeLandscapeRules(editor_->mapper.rules()) != rules) {
                            return false;
                        }
                        if (action == TerrainFileAction::SaveBitmap) {
                            const auto current = preview.snapshot();
                            return image && current && image->width == current->width &&
                                   image->height == current->height &&
                                   image->pixels == current->pixels;
                        }
                        return true;
                    };
                    p->overwriteTarget = std::move(target);
                    p->overwrite = true;
                    p->title.text.swap(title);
                    p->buttons[0]->text = "Yes";
                    p->buttons[1]->text = "No";
                    return true;
                }
            }
        } catch (...) {
            restore();
            scriptRestore();
            scriptPath_ = oldPath;
            scriptFormat_ = oldFormat;
            ui_->currentFile = oldFile;
            displayDraft_ = oldDraft;
            return false;
        }
    }
    const auto commit = [&] {
        switch (p->kind) {
            case Ui::Kind::Name:
                return p->revision == editor_->revision() && p->selection == editor_->selection() &&
                       editor_->nameSelection(p->text.text);
            case Ui::Kind::Choose:
            case Ui::Kind::Remove:
                if (p->revision != editor_->revision() || p->names != editor_->namedSelections() ||
                    index < 0 || std::size_t(index) >= p->names.size()) {
                    return false;
                }
                return p->kind == Ui::Kind::Choose
                           ? editor_->chooseSelection(p->names[std::size_t(index)])
                           : editor_->removeSelection(p->names[std::size_t(index)]);
            case Ui::Kind::Color:
                if (p->colors.selected.x < 0 || p->colors.selected.y < 0) {
                    return false;
                }
                color(displayDraft_ ? *displayDraft_ : view.display, p->color) =
                    UInt8(p->colors.selected.y * 16 + p->colors.selected.x);
                return true;
            case Ui::Kind::Browser: {
                if (p->text.text.empty()) {
                    return false;
                }
                if (p->overwrite) {
                    auto currentView = view.checkpoint();
                    auto currentScript = script.checkpoint();
                    const auto currentPath = scriptPath_;
                    const auto currentFormat = scriptFormat_;
                    auto target = browserTarget(stagedPath, readFile);
                    currentView();
                    currentScript();
                    scriptPath_ = currentPath;
                    scriptFormat_ = currentFormat;
                    if (!target || target != p->overwriteTarget || !p->overwriteSource ||
                        !p->overwriteSource()) {
                        return false;
                    }
                }
                if (!file(p->fileAction,
                          stagedPath.generic_string(),
                          p->second.text.empty() ? std::string{}
                                                 : (p->directory / p->second.text)
                                                       .lexically_normal()
                                                       .generic_string())) {
                    return false;
                }
                if (p->fileAction == TerrainFileAction::Save ||
                    p->fileAction == TerrainFileAction::Open) {
                    ui_->currentFile.swap(stagedPath);
                }
                return true;
            }
            case Ui::Kind::Confirm:
                if (p->revision != editor_->revision() ||
                    (p->command == deleteRule &&
                     p->ruleSignature != world::writeLandscapeRules(editor_->mapper.rules()))) {
                    return false;
                }
                return confirmed(p->command, p->rule);
            case Ui::Kind::Types:
                return index >= 0 && ruleType(std::size_t(index));
            case Ui::Kind::Info:
                return true;
            default:
                return false;
        }
    };
    if (!p->canvas->commitDialog(commit)) {
        restore();
        scriptRestore();
        scriptPath_ = oldPath;
        scriptFormat_ = oldFormat;
        ui_->currentFile = oldFile;
        displayDraft_ = oldDraft;
        return false;
    }
    ui_->panels.pop_back();
    return true;
}

bool TerrainTools::showUiAgain(const std::shared_ptr<Ui::Panel>& p) {
    ui_->panels.push_back(p);
    if (!p->canvas->pushDialog(p->root)) {
        ui_->panels.pop_back();
    }
    return false;
}

bool TerrainTools::confirmed(UInt32 id, std::size_t rule) {
    if (id == 1003 || id == 1005) {
        editor_->detach();
        ui_->currentFile.clear();
        return true;
    }
    if (id == 1049) {
        return editor_->command("clearPinMaps") == "True";
    }
    if (id == 1045) {
        return editor_->command("relight", std::array<std::string, 1>{"false"}) == "True";
    }
    if (id == lsApply) {
        return editor_->applyLandscape();
    }
    if (id == lsTexture) {
        return editor_->applyTextures();
    }
    if (id == deleteRule) {
        return removeRule(rule) && landscapeTab(ui_->tab);
    }
    return false;
}

bool TerrainTools::invoke(UInt32 id) {
    if (id == scriptTab || id == rulesTab) {
        return landscapeTab(id == scriptTab ? LandscapeTab::Script : LandscapeTab::Rules);
    }
    if (id == addRule) {
        return dialog(TerrainDialog::Rule, editor_->mapper.rules().size());
    }
    if (id == editRule) {
        const auto* selected = ui_->rules.selection();
        return selected && dialog(TerrainDialog::Rule, std::size_t(selected->tag));
    }
    if (id >= 65600 && id < 65606) {
        return colorDialog(TerrainColor(id - 65600));
    }
    if (id == 1003 || id == 1005 || id == 1049 || id == 1045 || id == lsApply || id == lsTexture ||
        id == deleteRule) {
        auto p = std::make_shared<Ui::Panel>();
        p->kind = Ui::Kind::Confirm;
        p->canvas = &ted;
        p->command = id;
        p->revision = editor_->revision();
        if (id == deleteRule) {
            const auto* selected = ui_->rules.selection();
            if (!selected) {
                return false;
            }
            p->rule = std::size_t(selected->tag);
            if (p->rule >= editor_->mapper.rules().size()) {
                return false;
            }
            p->ruleSignature = world::writeLandscapeRules(editor_->mapper.rules());
            p->names = {"Remove " + ruleItems()[p->rule] + "?"};
        } else {
            p->names = {id == 1003 || id == 1005 ? "Close terrain and discard unsaved changes?"
                        : id == 1049             ? "Clear all pin maps?"
                        : id == 1045             ? "Relight terrain?"
                        : id == lsApply          ? "Apply generated heights?"
                                                 : "Apply texture rules?"};
        }
        return showUi(p);
    }
    constexpr std::array<std::pair<UInt32, TerrainFileAction>, 13> files{
        {{1001, TerrainFileAction::Open},
         {1013, TerrainFileAction::Save},
         {1054, TerrainFileAction::LoadSelection},
         {1053, TerrainFileAction::SaveSelection},
         {lsLoadScript, TerrainFileAction::LoadScript},
         {lsSaveScript, TerrainFileAction::SaveScript},
         {lsLoadRules, TerrainFileAction::LoadRules},
         {lsSaveRules, TerrainFileAction::SaveRules},
         {lsLoadBitmap, TerrainFileAction::LoadBitmap},
         {lsSaveBitmap, TerrainFileAction::SaveBitmap},
         {1014, TerrainFileAction::LoadPalette},
         {1015, TerrainFileAction::LoadMaterials},
         {lsTextures, TerrainFileAction::LoadTextures}}};
    for (auto [key, action] : files) {
        if (id == key) {
            return browser(action);
        }
    }
    if (id == 1004) {
        return ui_->currentFile.empty()
                   ? browser(TerrainFileAction::Save)
                   : file(TerrainFileAction::Save, ui_->currentFile.generic_string());
    }
    if (id == 1000) {
        return dialog(TerrainDialog::NewTerrain);
    }
    if (id == 1022) {
        return displayDialog();
    }
    if (id == 1027) {
        return dialog(TerrainDialog::Settings);
    }
    if ((id >= 1038 && id <= 1042) || id == 1048 || id == 1050 || id == 1051 || id == 1059 ||
        id == 1052 || id == 706) {
        return valueDialog(id);
    }
    if (id == 1025 || id == 1017 || id == 1026) {
        return selectionDialog(id == 1025   ? TerrainSelectionDialog::Name
                               : id == 1017 ? TerrainSelectionDialog::Choose
                                            : TerrainSelectionDialog::Remove);
    }
    if (id == 1006) {
        return editor_->undo();
    }
    if (id == 1007) {
        return editor_->redo();
    }
    if (id == 1008) {
        return editor_->action("copy");
    }
    if (id == 1018) {
        return editor_->select({});
    }
    if (id == 1019) {
        return editor_->floatSelection();
    }
    if (id == 1009 || id == 1011 || id == 1012) {
        auto restore = editor_->checkpoint();
        auto settings = editor_->settings();
        settings.paste = id == 1009 ? 3 : id == 1011 ? 2 : 1;
        if (editor_->settings(settings) && editor_->floatPaste()) {
            return true;
        }
        restore();
        return false;
    }
    if (id == 1036 || id == 1037) {
        auto settings = editor_->settings();
        (id == 1036 ? settings.snap : settings.feather) =
            !(id == 1036 ? settings.snap : settings.feather);
        return editor_->settings(settings);
    }
    if (id >= 700 && id <= 705) {
        constexpr std::array<UInt32, 6> left{20, 14, 24, 13, 17, 27}, right{6, 10, 28, 4, 18, 28};
        auto settings = editor_->settings();
        settings.leftAction = left[id - 700];
        settings.rightAction = right[id - 700];
        return editor_->settings(settings);
    }
    const auto actions = world::TerrainEditor::actions();
    if (id >= 4096 && id < 8192) {
        if (!editor_->snapshot() || id - 4096 >= UInt32(editor_->snapshot()->grid.detailCount)) {
            return false;
        }
        auto settings = editor_->settings();
        settings.brushDetail = id - 4096;
        return editor_->settings(settings);
    }
    if (id >= 2048 && id < 4096) {
        return id - 2048 < actions.size() &&
               editor_->command("processAction",
                                std::array<std::string, 1>{std::string(actions[id - 2048])}) ==
                   "True";
    }
    if ((id >= 8192 && id < 16384) || (id >= 16384 && id < 20000)) {
        const bool right = id >= 16384;
        const auto index = id - (right ? 16384 : 8192);
        if (index >= actions.size()) {
            return false;
        }
        auto settings = editor_->settings();
        (right ? settings.rightAction : settings.leftAction) = index;
        return editor_->settings(settings);
    }
    if (id == lsPreview) {
        return runScript(script.text);
    }
    if (id == 1043) {
        ui_->landscapeVisible = true;
        return landscapeTab(LandscapeTab::Script);
    }
    if (id == 1046 || id == lsAbout || id == 1044) {
        auto p = std::make_shared<Ui::Panel>();
        p->kind = Ui::Kind::Info;
        p->canvas = &ted;
        p->names = {id == 1044 ? editor_->command("listCommands").value_or("")
                               : "Ted / Landscape editor - shared terrain model"};
        return showUi(p);
    }
    return false;
}

bool TerrainTools::uiMessage(UInt32 id, Canvas& host) {
    if (id >= uiAccept && id <= uiUp && !ui_->panels.empty() &&
        ui_->panels.back()->canvas != &host) {
        return false;
    }
    if (id >= 65700 && id < 65706) {
        return invoke(700 + id - 65700);
    }
    if (id == uiChoose) {
        if (ui_->panels.empty()) {
            return true;
        }
        auto p = ui_->panels.back();
        const auto selected = p->list.selected.y;
        return p->kind == Ui::Kind::Color || selected < 0 || uiSelect(std::size_t(selected));
    }
    host.afterDispatch([this, id] {
        if (id == uiAccept) {
            acceptUi();
        } else if (id == uiCancel) {
            cancelUi();
        } else if (id == uiUp && !ui_->panels.empty()) {
            browse(ui_->panels.back()->directory.parent_path());
        } else if (id == tedMenu || id == lsMenu) {
            menu({}, id == lsMenu);
        } else if (id == typeList && dialog_ == TerrainDialog::Rule) {
            auto p = std::make_shared<Ui::Panel>();
            p->kind = Ui::Kind::Types;
            p->canvas = &ted;
            p->names = terrainTypes();
            const auto selected = ruleTypeSelection();
            if (showUi(p) && selected) {
                uiSelect(*selected);
            }
        } else {
            invoke(id);
        }
    });
    return true;
}

bool TerrainTools::shortcut(const Event& e) {
    if (e.type != EventType::KeyDown || !e.control || ted.dialogCount() || !editor_->snapshot()) {
        return false;
    }
    const auto id = e.key == Key::Z               ? (e.shift ? 1007U : 1006U)
                    : !e.shift && e.key == Key::C ? 1008U
                    : !e.shift && e.key == Key::V ? 1009U
                                                  : 0U;
    if (!id) {
        return false;
    }
    const auto items = menuItems();
    const auto item = std::ranges::find(items, id, &TerrainMenuItem::id);
    if (item == items.end() || !item->enabled) {
        return false;
    }
    auto restore = editor_->checkpoint();
    try {
        if (invoke(id)) {
            return true;
        }
    } catch (...) {
    }
    restore();
    return false;
}
} // namespace opent1::gui
