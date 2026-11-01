#include <opent1/core/filename.hpp>
#include <opent1/game/file_plugin.hpp>
#include <algorithm>
#include <cctype>
#include <charconv>

namespace opent1::game {
namespace {

[[nodiscard]] Int32 parseIntAscii(std::string_view sv) noexcept {
    while (!sv.empty() && std::isspace(static_cast<unsigned char>(sv.front())) != 0) {
        sv.remove_prefix(1);
    }
    Int32 out = 0;
    (void)std::from_chars(sv.data(), sv.data() + sv.size(), out);
    return out;
}

[[nodiscard]] char toUpperAscii(char c) noexcept {
    return static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
}

[[nodiscard]] char toLowerAscii(char c) noexcept {
    return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
}

[[nodiscard]] int asciiStricmp(std::string_view a, std::string_view b) noexcept {
    const std::size_t n = std::min(a.size(), b.size());
    for (std::size_t i = 0; i < n; ++i) {
        const int ca = static_cast<unsigned char>(toLowerAscii(a[i]));
        const int cb = static_cast<unsigned char>(toLowerAscii(b[i]));
        if (ca != cb) {
            return ca - cb;
        }
    }
    if (a.size() == b.size()) {
        return 0;
    }
    return a.size() < b.size() ? -static_cast<int>(static_cast<unsigned char>(toLowerAscii(b[n])))
                               : static_cast<int>(static_cast<unsigned char>(toLowerAscii(a[n])));
}

[[nodiscard]] bool isPathSeparator(char c) noexcept {
    return c == '\\' || c == ':' || c == '/';
}

// Exact match for legacy Darkstar FileName::getExt / getBase / getTitle / getPath
// in darkstar/Core/code/fileName.cpp.
[[nodiscard]] std::string legacyGetExt(std::string_view str) {
    auto pos = str.rfind('.');
    if (pos == std::string_view::npos) {
        return {};
    }
    return std::string(str.substr(pos + 1));
}

[[nodiscard]] std::string legacyGetTitle(std::string_view str) {
    std::size_t i = str.size();
    while (i > 0 && !isPathSeparator(str[i - 1])) {
        --i;
    }
    return std::string(str.substr(i));
}

[[nodiscard]] std::string legacyGetBase(std::string_view str) {
    std::string title = legacyGetTitle(str);
    const auto dotPos = title.rfind('.');
    if (dotPos == std::string::npos) {
        return title;
    }
    title.resize(dotPos);
    return title;
}

[[nodiscard]] std::string legacyGetPath(std::string_view str) {
    std::size_t i = str.size();
    while (i > 0 && !isPathSeparator(str[i - 1])) {
        --i;
    }
    if (i == 0) {
        return {};
    }
    return std::string(str.substr(0, i));
}

[[nodiscard]] bool isValidRelativeWriteFileName(std::string_view name) noexcept {
    if (name.empty()) {
        return false;
    }
    if (name.front() == '/' || name.front() == '\\' || name.find(':') != std::string_view::npos) {
        return false;
    }
    if (name.find("..") != std::string_view::npos) {
        return false;
    }
    const auto ext = legacyGetExt(name);
    if (asciiStricmp(ext, "exe") == 0 || asciiStricmp(ext, "dll") == 0 ||
        asciiStricmp(ext, "bat") == 0 || asciiStricmp(ext, "cmd") == 0 ||
        asciiStricmp(ext, "com") == 0) {
        return false;
    }
    return true;
}

} // namespace

FilePlugin::FilePlugin(std::shared_ptr<ResourceManager> resources,
                       std::shared_ptr<BanList> banList,
                       std::filesystem::path writeRoot)
    : resources_(resources ? std::move(resources) : std::make_shared<ResourceManager>()),
      banList_(banList ? std::move(banList) : std::make_shared<BanList>()),
      writeRoot_(std::move(writeRoot)) {}

void FilePlugin::init(Console& console, sim::SimManager* manager) {
    console_ = &console;
    manager_ = manager;

    console.addCommand(0,
                       "File::findFirst",
                       [this](Console&, Int32, std::span<const std::string> argv) -> std::string {
                           if (argv.size() != 2) {
                               return {};
                           }
                           const auto match = resources_->findMatch(argv[1]);
                           finding_ = match.has_value();
                           findCursor_ = match ? match->cursor : 0;
                           return match ? match->name : std::string{};
                       });

    console.addCommand(0,
                       "File::findNext",
                       [this](Console&, Int32, std::span<const std::string> argv) -> std::string {
                           if (argv.size() != 2) {
                               return {};
                           }
                           if (!finding_) {
                               return {};
                           }
                           const auto match = resources_->findMatch(argv[1], findCursor_);
                           finding_ = match.has_value();
                           findCursor_ = match ? match->cursor : 0;
                           return match ? match->name : std::string{};
                       });

    console.addCommand(
        0,
        "File::copy",
        [this](Console& con, Int32, std::span<const std::string> argv) -> std::string {
            if (argv.size() != 3) {
                con.print("Invalid use of File::copy(sourceFN, destFN).");
                return "FALSE";
            }
            if (!isValidRelativeWriteFileName(argv[2])) {
                return "False";
            }
            const auto resolvePath = [this](std::string_view rel) {
                if (!writeRoot_.empty()) {
                    return writeRoot_ / std::filesystem::path(rel);
                }
                return std::filesystem::path(rel);
            };
            std::error_code ec;
            const auto srcPath = resolvePath(argv[1]);
            const auto dstPath = resolvePath(argv[2]);
            if (dstPath.has_parent_path()) {
                std::filesystem::create_directories(dstPath.parent_path(), ec);
            }
            if (std::filesystem::copy_file(
                    srcPath, dstPath, std::filesystem::copy_options::overwrite_existing, ec) &&
                !ec) {
                return "TRUE";
            }
            return "FALSE";
        });

    console.addCommand(
        0,
        "File::delete",
        [this](Console& con, Int32, std::span<const std::string> argv) -> std::string {
            if (argv.size() != 2) {
                con.print("Invalid use of File::delete(fileName).");
                return "FALSE";
            }
            if (!isValidRelativeWriteFileName(argv[1])) {
                return "False";
            }
            const auto targetPath = resolveFilePath(writeRoot_, argv[1]);
            std::error_code ec;
            if (std::filesystem::remove(targetPath, ec) && !ec) {
                return "TRUE";
            }
            return "FALSE";
        });

    console.addCommand(0,
                       "String::findSubStr",
                       [](Console&, Int32, std::span<const std::string> argv) -> std::string {
                           if (argv.size() != 3) {
                               return "-1";
                           }
                           std::string base = argv[1];
                           std::string sub = argv[2];
                           for (char& c : base) {
                               c = toUpperAscii(c);
                           }
                           for (char& c : sub) {
                               c = toUpperAscii(c);
                           }
                           const auto pos = base.find(sub);
                           if (pos == std::string::npos) {
                               return "-1";
                           }
                           return std::to_string(static_cast<Int32>(pos));
                       });

    console.addCommand(0,
                       "String::getSubStr",
                       [](Console&, Int32, std::span<const std::string> argv) -> std::string {
                           if (argv.size() != 4) {
                               return {};
                           }
                           const int start = parseIntAscii(argv[2]);
                           if (start < 0) {
                               return {};
                           }
                           int len = parseIntAscii(argv[3]);
                           if (len <= 0) {
                               return {};
                           }
                           if (static_cast<std::size_t>(start) > argv[1].size()) {
                               return {};
                           }
                           if (len > 255) {
                               len = 255;
                           }
                           return argv[1].substr(static_cast<std::size_t>(start),
                                                 static_cast<std::size_t>(len));
                       });

    console.addCommand(0,
                       "String::Compare",
                       [](Console&, Int32, std::span<const std::string> argv) -> std::string {
                           if (argv.size() != 3) {
                               return {};
                           }
                           const int cmp = std::string_view(argv[1]).compare(argv[2]);
                           return std::to_string(cmp < 0 ? -1 : (cmp > 0 ? 1 : 0));
                       });

    console.addCommand(0,
                       "String::NCompare",
                       [](Console&, Int32, std::span<const std::string> argv) -> std::string {
                           if (argv.size() != 4) {
                               return {};
                           }
                           const int n = parseIntAscii(argv[3]);
                           if (n <= 0) {
                               return "0";
                           }
                           const auto count = static_cast<std::size_t>(n);
                           const int cmp = std::string_view(argv[1]).substr(0, count).compare(
                               std::string_view(argv[2]).substr(0, count));
                           return std::to_string(cmp < 0 ? -1 : (cmp > 0 ? 1 : 0));
                       });

    console.addCommand(0,
                       "String::ICompare",
                       [](Console&, Int32, std::span<const std::string> argv) -> std::string {
                           if (argv.size() != 3) {
                               return {};
                           }
                           return std::to_string(asciiStricmp(argv[1], argv[2]));
                       });

    console.addCommand(
        0, "String::empty", [](Console&, Int32, std::span<const std::string> argv) -> std::string {
            if (argv.size() != 2) {
                return {};
            }
            std::size_t i = 0;
            while (i < argv[1].size() && argv[1][i] == ' ') {
                ++i;
            }
            return (i == argv[1].size()) ? "true" : "false";
        });

    console.addCommand(0,
                       "String::convertSpaces",
                       [](Console&, Int32, std::span<const std::string> argv) -> std::string {
                           if (argv.size() != 2) {
                               return {};
                           }
                           std::string out = argv[1];
                           for (char& c : out) {
                               if (c == ' ') {
                                   c = '_';
                               }
                           }
                           return out;
                       });

    console.addCommand(0,
                       "BanList::add",
                       [this](Console&, Int32, std::span<const std::string> argv) -> std::string {
                           if (argv.size() != 3) {
                               return "false";
                           }
                           banList_->addBanRelative(argv[1], parseIntAscii(argv[2]));
                           return "true";
                       });

    console.addCommand(0,
                       "BanList::addAbsolute",
                       [this](Console&, Int32, std::span<const std::string> argv) -> std::string {
                           if (argv.size() != 3) {
                               return "false";
                           }
                           banList_->addBan(argv[1], parseIntAscii(argv[2]));
                           return "true";
                       });

    console.addCommand(0,
                       "BanList::remove",
                       [this](Console&, Int32, std::span<const std::string> argv) -> std::string {
                           if (argv.size() != 2) {
                               return "false";
                           }
                           banList_->removeBan(argv[1]);
                           return "true";
                       });

    console.addCommand(0,
                       "BanList::export",
                       [this](Console&, Int32, std::span<const std::string> argv) -> std::string {
                           if (argv.size() != 2) {
                               return "false";
                           }
                           if (!writeRoot_.empty()) {
                               if (!isValidRelativeWriteFileName(argv[1])) {
                                   return "false";
                               }
                               banList_->exportToFile(resolveFilePath(writeRoot_, argv[1]),
                                                      argv[1]);
                           } else {
                               banList_->exportToFile(std::string_view(argv[1]));
                           }
                           return "true";
                       });

    console.addCommand(
        0, "File::getExt", [](Console&, Int32, std::span<const std::string> argv) -> std::string {
            if (argv.size() != 2) {
                return {};
            }
            return legacyGetExt(argv[1]);
        });

    console.addCommand(
        0, "File::getBase", [](Console&, Int32, std::span<const std::string> argv) -> std::string {
            if (argv.size() != 2) {
                return {};
            }
            return legacyGetBase(argv[1]);
        });

    console.addCommand(
        0, "File::getTitle", [](Console&, Int32, std::span<const std::string> argv) -> std::string {
            if (argv.size() != 2) {
                return {};
            }
            return legacyGetTitle(argv[1]);
        });

    console.addCommand(
        0, "File::getPath", [](Console&, Int32, std::span<const std::string> argv) -> std::string {
            if (argv.size() != 2) {
                return {};
            }
            return legacyGetPath(argv[1]);
        });
}

void FilePlugin::shutdown() noexcept {
    if (console_) {
        console_->removeCommand("File::findFirst");
        console_->removeCommand("File::findNext");
        console_->removeCommand("File::copy");
        console_->removeCommand("File::delete");
        console_->removeCommand("String::findSubStr");
        console_->removeCommand("String::getSubStr");
        console_->removeCommand("String::Compare");
        console_->removeCommand("String::NCompare");
        console_->removeCommand("String::ICompare");
        console_->removeCommand("String::empty");
        console_->removeCommand("String::convertSpaces");
        console_->removeCommand("BanList::add");
        console_->removeCommand("BanList::addAbsolute");
        console_->removeCommand("BanList::remove");
        console_->removeCommand("BanList::export");
        console_->removeCommand("File::getExt");
        console_->removeCommand("File::getBase");
        console_->removeCommand("File::getTitle");
        console_->removeCommand("File::getPath");
    }
    console_ = nullptr;
    manager_ = nullptr;
}

} // namespace opent1::game
