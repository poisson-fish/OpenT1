#pragma once
#include <opent1/world/landscape_mapper.hpp>
#include <opent1/console/console.hpp>
#include <map>

namespace opent1::world {
struct TerrainEditorDisplay {
    UInt8 show{1}, frameColor{3}, fillColor{4}; // frame, fill, outline bits
};
struct TerrainEditorSettings {
    Flt32 height{200}, adjust{5}, scale{1}, smooth{.5f}, mouseAdjust{.25f};
    UInt32 brushDetail{}, pinDetail{1}, pinMax{10};
    UInt32 leftAction{20}, rightAction{6};
    UInt8 material{}, flags{}, paste{3}, terrainType{};
    bool snap{true}, feather{true};
};

struct TerrainSelectionInfo {
    Point2I point;
    Flt32 height{};
    TerrainGridMaterial material;
};
enum class TerrainGesture { Begin, Update, End, Cancel };
using TerrainOutputFile = std::pair<std::filesystem::path, std::vector<std::byte>>;
// Stage every output before replacing any destination; failure restores the
// prior files. Shared by the model's commands and native editor file hosts.
bool writeTerrainEditorFiles(std::span<const TerrainOutputFile>);

// GUI-independent Ted/LSEditor model. GUI focus/capture/drawing adapters are
// still tracked by GUI-1/2/3. One completed stroke is one history entry.
class TerrainEditor : public std::enable_shared_from_this<TerrainEditor> {
public:
    explicit TerrainEditor(std::size_t historyBytes = 256U * 1024U * 1024U,
                           UInt32 historySteps = 64)
        : history_(historyBytes, historySteps) {}

    bool attach(TerrainDocument);

    void publisher(std::function<bool(const TerrainDocument&)> callback) {
        history_.publish = std::move(callback);
    }

    void detach();
    bool open(std::span<const std::byte>);
    bool save(const std::filesystem::path&) const;
    bool settings(const TerrainEditorSettings&);

    [[nodiscard]] const TerrainEditorSettings& settings() const {
        return settings_;
    }

    bool select(std::span<const Point2I>, bool add = false);
    bool brush(Point2I);
    bool region(Point2I first, Point2I last, bool add = false);
    bool action(std::string_view);
    bool gesture(std::string_view, TerrainGesture, Point2I brush, Int32 mouseDeltaY = 0);
    // GUI input can roll back a model edit when a later control/host rejects the
    // same event. History snapshots stay shared; an in-progress stroke is copied.
    [[nodiscard]] std::function<void()> checkpoint();
    bool undo();
    bool redo();

    [[nodiscard]] bool canUndo() const {
        return !working_ && history_.canUndo();
    }

    [[nodiscard]] bool canRedo() const {
        return !working_ && history_.canRedo();
    }

    [[nodiscard]] std::vector<std::string> namedSelections() const {
        std::vector<std::string> names;
        for (const auto& entry : named_) {
            names.push_back(entry.first);
        }
        return names;
    }

    bool nameSelection(std::string_view);
    bool chooseSelection(std::string_view);
    bool removeSelection(std::string_view);
    [[nodiscard]] std::optional<std::vector<std::byte>> saveSelection() const;
    bool loadSelection(std::span<const std::byte>);
    [[nodiscard]] std::optional<std::vector<std::byte>> saveNamedSelections() const;
    bool loadNamedSelections(std::span<const std::byte>);

    [[nodiscard]] const std::vector<Point2I>& selection() const {
        return selection_;
    }

    [[nodiscard]] std::shared_ptr<const TerrainDocument> snapshot() const {
        return history_.snapshot();
    }

    [[nodiscard]] std::shared_ptr<const TerrainDocument> preview() const {
        return working_ ? working_ : history_.snapshot();
    }

    [[nodiscard]] UInt64 revision() const {
        return history_.revision();
    }

    [[nodiscard]] Flt32 averageHeight() const {
        return average_;
    }

    [[nodiscard]] bool floating() const {
        return floating_;
    }

    bool floatSelection();
    bool floatPaste();

    void cancelFloating() {
        floating_ = false;
    }

    [[nodiscard]] std::span<const TerrainSelectionInfo> clipboard() const {
        return clipboard_;
    }

    bool mirror(UInt32 sides);
    bool landscapeScript(std::string_view, const Landscape::Loader& loader = {});
    bool applyLandscape();
    bool applyTextures();
    bool materialResources(std::string_view,
                           std::span<const std::byte>,
                           std::optional<std::span<const std::byte>> textures = {});
    LandscapeMapper mapper;
    LandscapeTextureRecipe textureRecipe;
    Landscape landscape;
    std::function<bool(TerrainDocument&, bool)> relight;
    std::function<bool()> initialize;
    bool focused{};
    TerrainEditorDisplay selectionDisplay{}, highlightDisplay{1, 2, 8}, shadowDisplay{1, 6, 12};
    bool blockOutline{};
    std::function<std::optional<TerrainDocument>(std::string_view)> terrainLoader;
    std::string attachedFile; // host's terrain volume name, distinct from its grid entry
    std::optional<gfx::Palette> palette;
    [[nodiscard]] static std::span<const std::string_view> actions();
    std::optional<std::string> command(std::string_view, std::span<const std::string> = {});

private:
    bool mutate(TerrainDocument&, std::string_view, std::span<const Point2I>, Int32 delta = 0);
    TerrainHistory history_;
    TerrainEditorSettings settings_;
    std::vector<Point2I> selection_;
    std::vector<TerrainSelectionInfo> clipboard_;
    std::map<std::string, std::vector<Point2I>, std::less<>> named_;
    Point2I brush_;
    std::shared_ptr<TerrainDocument> working_;
    std::string gestureAction_;
    std::vector<Point2I> gestureSelection_;
    std::vector<Point2I> savedSelection_;
    Point2I grab_;
    Flt32 average_{};
    std::string landscapeCommands_;
    std::map<std::string, TsMaterialList, std::less<>> materialLists_;
    bool floating_{};
};

// Weak command captures remain safe if the editor is destroyed before Console.
// Call uninstall before reusing this command namespace for another plugin.
void installTerrainEditorCommands(Console&, const std::shared_ptr<TerrainEditor>&);
void uninstallTerrainEditorCommands(Console&);
} // namespace opent1::world
