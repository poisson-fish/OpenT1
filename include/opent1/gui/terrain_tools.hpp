#pragma once
#include <opent1/gui/controls.hpp>
#include <opent1/gui/containers.hpp>
#include <opent1/world/terrain_editor.hpp>
#include <opent1/world/world_surfaces.hpp>
#include <opent1/sim/sim_environment_camera.hpp>

namespace opent1::gui {
struct TerrainDisplayOptions {
    UInt8 selection{7}, highlight{7}, shadow{7};
    UInt8 selectionColor{255}, highlightColor{254}, shadowColor{249};
    UInt8 selectionFrameColor{255}, highlightFrameColor{254}, shadowFrameColor{249};
    UInt8 blockFrameColor{};
    bool materials{}, grid{true}, blockOutline{};
    bool operator==(const TerrainDisplayOptions&) const = default;
};

class TerrainView : public Active {
public:
    explicit TerrainView(std::shared_ptr<world::TerrainEditor>);

    ~TerrainView() override {
        try {
            close();
        } catch (...) {
        }
    }

    void onRemove() override {
        close();
        Active::onRemove();
    }

    TerrainDisplayOptions display;
    // A perspective/orthographic viewport host can replace this with a terrain
    // LOS picker. The default is the editor's complete top-down grid extent.
    std::function<std::optional<Point2I>(Point2I)> pick;
    // Camera coordinates are terrain-local. Display and LOS picking share the
    // same projection; all generated geometry/resources are owned snapshots.
    bool projection(std::optional<sim::EnvironmentCamera>, world::TsMaterialResources = {});
    [[nodiscard]] std::shared_ptr<const world::TsStaticShapeFrame> projectedFrame() const;

    void materialResources(world::TsMaterialResources resources) {
        materials_ = std::move(resources);
    }

    bool highlight(std::span<const Point2I>);

    [[nodiscard]] std::span<const Point2I> highlighted() const {
        return highlight_;
    }

    [[nodiscard]] std::span<const Point2I> shadow() const {
        return shadow_;
    }

    [[nodiscard]] std::optional<Point2I> cell(Point2I) const;
    [[nodiscard]] std::optional<std::pair<std::string, std::string>> brushStatus(Point2I) const;
    bool event(const Event&) override;
    void close() override;
    bool draw(gfx::SoftwareSurface&, Point2I, RectI) const override;
    [[nodiscard]] std::shared_ptr<const world::WorldSurfaceMesh>
        surface(world::TsMaterialResources = {}) const;
    std::function<void()> checkpoint() override;

private:
    std::shared_ptr<world::TerrainEditor> editor_;
    std::string action_;
    bool dragging_{};
    Int32 lastY_{};
    UInt8 button_{};
    std::vector<Point2I> highlight_, shadow_;
    void updateShadow(Point2I);
    std::optional<sim::EnvironmentCamera> camera_;
    world::TsMaterialResources materials_;
    bool projectedDraw(gfx::SoftwareSurface&, Point2I, RectI) const;
};

class LandscapeView : public Control {
public:
    explicit LandscapeView(std::shared_ptr<world::TerrainEditor> editor)
        : editor_(std::move(editor)) {}

    bool draw(gfx::SoftwareSurface&, Point2I, RectI) const override;
    [[nodiscard]] std::shared_ptr<const gfx::Bitmap> snapshot() const;

private:
    std::shared_ptr<world::TerrainEditor> editor_;
};
enum class TerrainFileAction {
    Open,
    Save,
    LoadSelection,
    SaveSelection,
    LoadScript,
    SaveScript,
    LoadRules,
    SaveRules,
    LoadBitmap,
    SaveBitmap,
    LoadPalette,
    LoadMaterials,
    LoadTextures
};
enum class TerrainDialog { Settings, NewTerrain, Rule, File, Value };
enum class LandscapeTab { Script, Rules };
enum class TerrainSelectionDialog { Name, Choose, Remove };
enum class TerrainColor {
    SelectionFill,
    SelectionFrame,
    HighlightFill,
    HighlightFrame,
    ShadowFill,
    ShadowFrame
};

struct TerrainMenuItem {
    UInt32 id{};
    std::string group, label;
    bool enabled{true}, checked{};
};

struct TerrainBrowserEntry {
    std::string name;
    bool directory{};
};
struct TerrainUiPanel;

// Both windows share the exact B09 editor/history/landscape model. Dialog edits
// are staged until accept; closing a window cancels its current brush stroke.
class TerrainTools {
public:
    explicit TerrainTools(std::shared_ptr<world::TerrainEditor>);
    ~TerrainTools();
    Canvas ted, landscape;
    TerrainView view;
    LandscapeView preview;
    TextEdit script;
    bool resize(bool landscapeWindow, Point2I);
    bool displayDialog();
    bool acceptDisplay(const TerrainDisplayOptions&);
    bool cancelDialog();
    bool dialog(TerrainDialog, std::size_t rule = 0);
    bool fileDialog(TerrainFileAction);
    bool acceptDialog();
    bool cancelEditorDialog();

    [[nodiscard]] std::size_t dialogFieldCount() const {
        return edits_.size();
    }

    bool dialogValue(std::size_t, std::string);
    [[nodiscard]] std::optional<std::string> dialogValue(std::size_t) const;
    bool removeRule(std::size_t);
    // Stable original Ted command IDs; menus and toolbar use the same routes.
    [[nodiscard]] std::vector<TerrainMenuItem> menuItems(bool landscapeWindow = false) const;
    bool menu(std::string_view group = {}, bool landscapeWindow = false);
    bool invoke(UInt32 id);
    // Original Ted accelerators. Modal controls and the landscape script keep
    // their own editing keys; callers route only the Ted window here.
    bool shortcut(const Event&);
    bool selectionDialog(TerrainSelectionDialog);
    bool colorDialog(TerrainColor);
    bool browser(TerrainFileAction, std::filesystem::path directory = {});
    using DirectoryReader = std::function<std::optional<std::vector<TerrainBrowserEntry>>(
        const std::filesystem::path&)>;
    DirectoryReader readDirectory;
    bool browse(const std::filesystem::path&);
    [[nodiscard]] std::vector<std::string> uiItems() const;
    bool uiSelect(std::size_t);
    bool uiText(std::string);
    bool acceptUi();
    bool cancelUi();
    bool landscapeTab(LandscapeTab);
    [[nodiscard]] LandscapeTab landscapeTab() const;
    bool selectRule(std::size_t);
    [[nodiscard]] std::vector<std::string> ruleItems() const;
    [[nodiscard]] std::vector<std::string> terrainTypes() const;
    bool ruleType(std::size_t);
    [[nodiscard]] std::optional<std::size_t> ruleTypeSelection() const;
    using Reader = std::function<std::optional<std::vector<std::byte>>(std::string_view)>;
    using Writer = std::function<bool(std::span<const world::TerrainOutputFile>)>;
    // Defaults use bounded disk reads and the model's atomic multi-file writer.
    Reader readFile;
    Writer writeFiles;
    bool file(TerrainFileAction, std::string_view, std::string_view second = {});

    [[nodiscard]] world::LandscapeScriptFormat scriptFormat() const {
        return scriptFormat_;
    }

    void scriptFormat(world::LandscapeScriptFormat format) {
        scriptFormat_ = format;
    }

    bool runScript(std::string_view, const world::Landscape::Loader& = {});
    std::optional<std::string> command(std::string_view, std::span<const std::string> = {});

    [[nodiscard]] std::string_view status(std::size_t pane) const {
        return pane < status_.size() ? std::string_view(status_[pane]) : std::string_view{};
    }

    void font(std::shared_ptr<const gfx::BitmapFont>);

    [[nodiscard]] const std::shared_ptr<world::TerrainEditor>& editor() const {
        return editor_;
    }

private:
    std::shared_ptr<world::TerrainEditor> editor_;
    std::array<std::string, 3> status_{};
    Control tedRoot_, landscapeRoot_, displayRoot_, editorDialog_, editorModal_;
    Scroll editorScroll_;
    std::vector<std::unique_ptr<Button>> buttons_;
    std::vector<std::unique_ptr<Check>> displayChecks_;
    std::shared_ptr<const gfx::BitmapFont> font_;
    std::vector<std::unique_ptr<TextEdit>> edits_;
    std::vector<std::unique_ptr<Text>> labels_;
    std::vector<std::unique_ptr<Button>> dialogButtons_;
    std::optional<TerrainDialog> dialog_;
    std::size_t rule_{};
    TerrainFileAction fileAction_{};
    std::filesystem::path scriptPath_;
    world::LandscapeScriptFormat scriptFormat_{world::LandscapeScriptFormat::RawText};
    std::optional<TerrainDisplayOptions> displayDraft_;
    bool addDialogField(std::string, std::string);
    bool showEditorDialog();
    bool valueDialog(UInt32);
    UInt32 valueCommand_{};
    UInt64 valueRevision_{};
    std::vector<Point2I> valueSelection_;
    struct Ui;
    std::shared_ptr<Ui> ui_;
    void initializeUi();
    void fontUi();
    bool uiMessage(UInt32, Canvas&);
    bool resizeUi(bool, Point2I);
    bool uiOpen() const;
    bool showUi(const std::shared_ptr<TerrainUiPanel>&);
    bool showUiAgain(const std::shared_ptr<TerrainUiPanel>&);
    bool
    prepareBrowser(TerrainUiPanel&, const std::filesystem::path&, std::vector<TerrainBrowserEntry>);
    bool confirmed(UInt32, std::size_t);
};
} // namespace opent1::gui
