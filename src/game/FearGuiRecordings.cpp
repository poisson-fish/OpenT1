#include "opent1/game/server_irc_controls.hpp"

namespace opent1::game {

FGRecordingList::FGRecordingList() {
    setSize({1, 0});
    cellSize = {218, 16};
    selected = {-1, -1};
}

void FGRecordingList::addRecording(std::string_view name) {
    recordings_.push_back(PlaybackRep{std::string(name), false});
    setSize({1, static_cast<Int32>(recordings_.size())});
    if (selected.y < 0) {
        selected = {0, 0};
    }
}

void FGRecordingList::clearRecordings() {
    recordings_.clear();
    setSize({1, 0});
    selected = {-1, -1};
}

void FGRecordingList::setSelectedRecording(std::string_view name) {
    for (std::size_t i = 0; i < recordings_.size(); ++i) {
        if (recordings_[i].name == name) {
            selected = {0, static_cast<Int32>(i)};
            return;
        }
    }
}

std::string FGRecordingList::getSelectedRecording() {
    if (selected.y < 0 || static_cast<std::size_t>(selected.y) >= recordings_.size()) {
        return {};
    }
    auto& rec = recordings_[static_cast<std::size_t>(selected.y)];
    rec.alreadyPlayed = true;
    return "recordings\\" + rec.name + ".rec";
}

std::string FGRecordingList::getSelectedRecordingName() const {
    if (selected.y < 0 || static_cast<std::size_t>(selected.y) >= recordings_.size()) {
        return {};
    }
    return recordings_[static_cast<std::size_t>(selected.y)].name;
}

void FGRecordingList::selectRandomRecording(UInt32 seed) {
    if (recordings_.empty()) {
        return;
    }
    std::vector<std::size_t> unplayed{};
    for (std::size_t i = 0; i < recordings_.size(); ++i) {
        if (!recordings_[i].alreadyPlayed) {
            unplayed.push_back(i);
        }
    }
    std::size_t index = 0;
    if (unplayed.empty()) {
        for (auto& rec : recordings_) {
            rec.alreadyPlayed = false;
        }
        index = static_cast<std::size_t>(seed) % recordings_.size();
    } else {
        index = unplayed[static_cast<std::size_t>(seed) % unplayed.size()];
    }
    selected = {0, static_cast<Int32>(index)};
}

std::string FGRecordingList::getCellText(Point2I cell) const {
    if (cell.y < 0 || static_cast<std::size_t>(cell.y) >= recordings_.size()) {
        return {};
    }
    return recordings_[static_cast<std::size_t>(cell.y)].name;
}

void FGRecordingComboBox::selectRandomRecording(UInt32 seed) {
    recordingList_.selectRandomRecording(seed);
    text = recordingList_.getSelectedRecordingName();
}

} // namespace opent1::game
