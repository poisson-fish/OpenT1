#include "opent1/game/server_irc_controls.hpp"
#include <ranges>

#include <algorithm>
#include <cctype>

namespace opent1::game {

namespace {

bool caseInsensitiveLessRec(std::string_view a, std::string_view b) noexcept {
    const std::size_t n = std::min(a.size(), b.size());
    for (auto i : std::views::iota(std::size_t{0}) | std::views::take_while([&](std::size_t i) {
                      return i < n;
                  })) {
        const auto ca = static_cast<unsigned char>(std::tolower(static_cast<unsigned char>(a[i])));
        const auto cb = static_cast<unsigned char>(std::tolower(static_cast<unsigned char>(b[i])));
        if (ca != cb) {
            return ca < cb;
        }
    }
    return a.size() < b.size();
}

std::filesystem::path recordingPath(const std::filesystem::path& directory, std::string_view name) {
    std::error_code error;
    std::filesystem::directory_iterator it(directory, error), end;
    for (; !error && it != end; it.increment(error)) {
        if (it->path().stem().string() != name || !it->is_regular_file(error)) {
            continue;
        }
        auto extension = it->path().extension().string();
        std::ranges::transform(extension, extension.begin(), [](unsigned char c) {
            return char(std::tolower(c));
        });
        if (extension == ".rec") {
            return it->path();
        }
    }
    return directory / (std::string(name) + ".rec");
}

} // namespace

FGRecordingListCtrl::FGRecordingListCtrl() {
    columnInfo_ = {
        ColumnInfo{0, 10, 640, 0, 0, true, 300, "RECORDINGS"},
    };
    columnsResizeable_ = false;
    columnsSortable_ = false;
    setSize({1, 0});
    cellSize = {300, 18};
}

bool FGRecordingListCtrl::onAdd() {
    if (!Parent::onAdd()) {
        return false;
    }
    refreshRecordings();
    return true;
}

void FGRecordingListCtrl::wake() {
    Parent::wake();
    refreshRecordings();
}

bool FGRecordingListCtrl::refreshRecordings() {
    const auto selectedName = getSelectedRecordingName();
    std::error_code error;
    std::vector<PlaybackRep> next;
    if (std::filesystem::exists(recordingDirectory_, error)) {
        std::filesystem::directory_iterator iter(recordingDirectory_, error), end;
        if (error) {
            return false;
        }
        for (; iter != end; iter.increment(error)) {
            if (error) {
                return false;
            }
            if (!iter->is_regular_file(error)) {
                if (error) {
                    return false;
                }
                continue;
            }
            auto extension = iter->path().extension().string();
            std::ranges::transform(extension, extension.begin(), [](unsigned char c) {
                return char(std::tolower(c));
            });
            if (extension != ".rec") {
                continue;
            }
            const auto name = iter->path().stem().string();
            const auto previous = std::ranges::find(recordings_, name, &PlaybackRep::name);
            next.push_back({name, previous != recordings_.end() && previous->alreadyPlayed});
        }
        if (error) {
            return false;
        }
    } else if (error) {
        return false;
    }
    recordings_ = std::move(next);
    selected = {-1, -1};
    sortRecordings();
    setSelectedRecording(selectedName);
    return true;
}

void FGRecordingListCtrl::addRecording(std::string_view name) {
    recordings_.push_back(PlaybackRep{std::string(name), false});
    sortRecordings();
}

void FGRecordingListCtrl::clearRecordings() {
    recordings_.clear();
    setSize({1, 0});
    selected = {-1, -1};
}

bool FGRecordingListCtrl::renameSelected(std::string_view newName) {
    if (newName.empty() || newName == "." || newName == ".." ||
        newName.find_first_of("\\/:*?\"<>|") != newName.npos ||
        newName.find('\0') != newName.npos || selected.y < 0 ||
        static_cast<std::size_t>(selected.y) >= recordings_.size()) {
        return false;
    }
    auto& recording = recordings_[static_cast<std::size_t>(selected.y)];
    if (recording.name == newName) {
        return true;
    }
    const auto source = recordingPath(recordingDirectory_, recording.name);
    const auto target = recordingDirectory_ / (std::string(newName) + ".rec");
    std::error_code error;
    if (!std::filesystem::copy_file(
            source, target, std::filesystem::copy_options::overwrite_existing, error)) {
        return false;
    }
    if (!std::filesystem::remove(source, error) || error) {
        return false;
    }
    recording.name = std::string(newName);
    sortRecordings();
    setSelectedRecording(newName);
    return true;
}

void FGRecordingListCtrl::setSelectedRecording(std::string_view name) {
    for (auto i : std::views::iota(std::size_t{0}, recordings_.size())) {
        if (recordings_[i].name == name) {
            selected = {0, static_cast<Int32>(i)};
            return;
        }
    }
}

std::string FGRecordingListCtrl::getSelectedRecording() {
    if (selected.y < 0 || static_cast<std::size_t>(selected.y) >= recordings_.size()) {
        return {};
    }
    auto& rec = recordings_[static_cast<std::size_t>(selected.y)];
    rec.alreadyPlayed = true;
    return recordingPath(recordingDirectory_, rec.name).string();
}

std::string FGRecordingListCtrl::getSelectedRecordingName() const {
    if (selected.y < 0 || static_cast<std::size_t>(selected.y) >= recordings_.size()) {
        return {};
    }
    return recordings_[static_cast<std::size_t>(selected.y)].name;
}

void FGRecordingListCtrl::selectRandomRecording(UInt32 seed) {
    if (recordings_.empty()) {
        return;
    }
    std::vector<std::size_t> unplayed{};
    for (auto i : std::views::iota(std::size_t{0}) | std::views::take_while([&](std::size_t i) {
                      return i < recordings_.size();
                  })) {
        if (!recordings_[i].alreadyPlayed) {
            unplayed.push_back(i);
        }
    }
    std::size_t chosen = 0;
    if (unplayed.empty()) {
        for (auto& rec : recordings_) {
            rec.alreadyPlayed = false;
        }
        chosen = static_cast<std::size_t>(seed) % recordings_.size();
    } else {
        chosen = unplayed[static_cast<std::size_t>(seed) % unplayed.size()];
    }
    selected = {0, static_cast<Int32>(chosen)};
}

void FGRecordingListCtrl::selectNextRecording() {
    if (recordings_.empty()) {
        return;
    }
    for (auto i : std::views::iota(std::size_t{0}, recordings_.size())) {
        if (!recordings_[i].alreadyPlayed) {
            selected = {0, static_cast<Int32>(i)};
            return;
        }
    }
    for (auto& rec : recordings_) {
        rec.alreadyPlayed = false;
    }
    selected = {0, 0};
}

std::string FGRecordingListCtrl::getCellText(Point2I cell) const {
    if (cell.y < 0 || static_cast<std::size_t>(cell.y) >= recordings_.size()) {
        return {};
    }
    return recordings_[static_cast<std::size_t>(cell.y)].name;
}

void FGRecordingListCtrl::sortRecordings() {
    std::stable_sort(
        recordings_.begin(), recordings_.end(), [](const PlaybackRep& a, const PlaybackRep& b) {
            return caseInsensitiveLessRec(a.name, b.name);
        });
    setSize({1, static_cast<Int32>(recordings_.size())});
    if (!recordings_.empty() && selected.y < 0) {
        selected = {0, 0};
    }
}

} // namespace opent1::game
