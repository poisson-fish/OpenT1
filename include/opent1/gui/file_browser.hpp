#pragma once
#include <opent1/gui/containers.hpp>
#include <opent1/gui/modal.hpp>
#include <opent1/world/object_plugins.hpp>
#include <filesystem>

namespace opent1::gui {
struct FileFilter {
    std::string label, pattern;
};

struct FileBrowserEntry {
    std::string name;
    bool directory{};
};

struct FileBrowserRequest {
    std::filesystem::path directory;
    std::vector<FileFilter> filters;
    bool save{};
    std::string defaultExtension{"txt"};
    std::size_t maxPathBytes{510}; // native nMaxFile=511 includes its terminator
};

// A chooser selects a path; it never reads resources or publishes file output.
// TerrainTools retains its separate writer/overwrite-freshness transaction.
class FileBrowser {
public:
    enum class Kind { Missing, File, Directory, Refused };
    using DirectoryReader =
        std::function<std::optional<std::vector<FileBrowserEntry>>(const std::filesystem::path&)>;
    using Probe = std::function<Kind(const std::filesystem::path&, bool saving)>;

    struct Result {
        bool accepted{};
        std::filesystem::path path, directory;
    };

    FileBrowser();
    ~FileBrowser();
    FileBrowser(const FileBrowser&) = delete;
    FileBrowser& operator=(const FileBrowser&) = delete;
    DirectoryReader readDirectory;
    Probe probe;
    bool open(FileBrowserRequest);
    bool browse(const std::filesystem::path&);
    bool filter(std::size_t);
    bool text(std::string);
    bool select(std::size_t);
    bool accept();
    bool cancel();
    [[nodiscard]] bool active() const;
    [[nodiscard]] std::vector<std::string> items() const;
    [[nodiscard]] std::optional<Result> result() const;
    [[nodiscard]] Canvas& canvas();
    [[nodiscard]] ModalView view();
    void font(std::shared_ptr<const gfx::BitmapFont>);
    // The application supplies its synchronous modal event pump. The driver
    // must return after accept/cancel; unavailable pumps fail explicitly.
    // This host handles openFile/saveFileAs only; other tool endpoints fail.
    [[nodiscard]] world::ObjectHost host(std::function<void(FileBrowser&)> modal);

private:
    struct State;
    std::shared_ptr<State> state_;
};
} // namespace opent1::gui
