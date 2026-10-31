#include "opent1/game/shell_controls.hpp"

#include <algorithm>
#include <cctype>

namespace opent1::game {

bool FGComboBox::draw(gfx::SoftwareSurface& surface, Point2I p, RectI clip) const {
    const auto left = p.x + 3, right = p.x + fields_.extent.x - 4;
    const auto bottom = p.y + fields_.extent.y - 1;
    const UInt8 inside = active ? 240 : 236, outside = active ? 239 : 235;
    surface.line({left + 1, p.y}, {right - 1, p.y}, outside);
    surface.line({left, p.y + 1}, {right, p.y + 1}, inside);
    surface.line({left, bottom - 1}, {right, bottom - 1}, inside);
    surface.line({left + 1, bottom}, {right - 1, bottom}, outside);
    const auto index = !active ? 5 : (depressed || isOpen()) ? 4 : over ? 2 : 0;
    const auto arrow = bitmaps_[index] ? bitmaps_[index] : bitmaps_[0];
    if (arrow && !surface.blit(*arrow, {p.x + fields_.extent.x - 7 - arrow->width, p.y + 2})) {
        return false;
    }
    const auto f = !active && disabledFont ? disabledFont : font;
    int textX = p.x + 10;
    if (f && !bitmapRootName_.empty()) {
        if (!surface.text(
                *f, {textX, p.y + (fields_.extent.y - f->info.height) / 2}, bitmapRootName_)) {
            return false;
        }
        textX += static_cast<int>(f->textWidth(bitmapRootName_)) + 6;
    }
    if (f) {
        const auto oldClip = surface.clip();
        const auto textClip = gfx::intersect(
            oldClip, {textX, p.y + 2, right - (arrow ? arrow->width + 8 : 6), bottom - 2});
        if (textClip) {
            surface.setClip(*textClip);
            const bool ok =
                surface.text(*f, {textX, p.y + (fields_.extent.y - f->info.height) / 2}, text);
            surface.setClip(oldClip);
            if (!ok) {
                return false;
            }
        }
    }
    return drawChildren(surface, p, clip);
}

bool FGComboBox::selectAdjacent(int direction) {
    const auto entries = comboEntries_.entries();
    if (entries.empty()) {
        return false;
    }
    auto selected = std::ranges::find_if(entries, [&](const auto& entry) {
        return entry.id == getSelectedEntry();
    });
    const auto index = selected == entries.end() ? 0 : static_cast<int>(selected - entries.begin());
    setSelectedEntry(
        entries[(index + direction + static_cast<int>(entries.size())) % entries.size()].id);
    gui::Text::action();
    return true;
}

namespace {

inline constexpr Int32 kIdItgBmpRootTag = 200100;

bool caseInsensitiveLess(std::string_view a, std::string_view b) noexcept {
    const std::size_t n = std::min(a.size(), b.size());
    for (std::size_t i = 0; i < n; ++i) {
        const auto ca = static_cast<unsigned char>(std::tolower(static_cast<unsigned char>(a[i])));
        const auto cb = static_cast<unsigned char>(std::tolower(static_cast<unsigned char>(b[i])));
        if (ca != cb) {
            return ca < cb;
        }
    }
    return a.size() < b.size();
}

} // namespace

FGComboList::FGComboList() {
    setSize({1, 0});
    cellSize = {160, 16};
    selected = {-1, -1};
}

void FGComboList::addEntry(std::string_view text, Int32 id) {
    entries_.push_back(EntryRep{id, std::string(text)});
    resortEntries();
}

void FGComboList::deleteEntry(Int32 id) {
    const Int32 curId = getSelectedEntry();
    std::erase_if(entries_, [id](const EntryRep& e) {
        return e.id == id;
    });
    setSize({1, static_cast<Int32>(entries_.size())});
    if (curId == id) {
        selected = entries_.empty() ? Point2I{-1, -1} : Point2I{0, 0};
    } else {
        setSelectedEntry(curId);
    }
}

Int32 FGComboList::findEntry(std::string_view text) const {
    for (const auto& entry : entries_) {
        if (entry.name == text) {
            return entry.id;
        }
    }
    return -1;
}

void FGComboList::clearEntries() {
    entries_.clear();
    setSize({1, 0});
    selected = {-1, -1};
}

void FGComboList::setSelectedEntry(Int32 id) {
    for (std::size_t i = 0; i < entries_.size(); ++i) {
        if (entries_[i].id == id) {
            selected = {0, static_cast<Int32>(i)};
            return;
        }
    }
    selected = {-1, -1};
}

Int32 FGComboList::getSelectedEntry() const noexcept {
    if (selected.y < 0 || static_cast<std::size_t>(selected.y) >= entries_.size()) {
        return -1;
    }
    return entries_[static_cast<std::size_t>(selected.y)].id;
}

std::string FGComboList::getSelectedText() const {
    if (selected.y < 0 || static_cast<std::size_t>(selected.y) >= entries_.size()) {
        return {};
    }
    return entries_[static_cast<std::size_t>(selected.y)].name;
}

void FGComboList::setSort(bool sortEntries) {
    sortEntries_ = sortEntries;
    resortEntries();
}

void FGComboList::resortEntries() {
    const Int32 curId = getSelectedEntry();
    if (sortEntries_) {
        std::stable_sort(
            entries_.begin(), entries_.end(), [](const EntryRep& a, const EntryRep& b) {
                return caseInsensitiveLess(a.name, b.name);
            });
    }
    setSize({1, static_cast<Int32>(entries_.size())});
    if (curId >= 0) {
        setSelectedEntry(curId);
    } else if (!entries_.empty() && selected.y < 0) {
        selected = {0, 0};
    }
}

void FGComboBox::setBitmapRootName(std::string_view name) {
    bitmapRootName_ = std::string(name);
}

namespace {
void syncPopup(FGComboBox& combo, const FGComboList& entries) {
    auto& list = combo.list();
    list.clear();
    for (const auto& entry : entries.entries()) {
        list.insert({entry.id, entry.name});
    }
    if (!combo.text.empty()) {
        (void)list.variableChanged(combo.text);
    }
}
}

void FGComboBox::addEntry(std::string_view entryText, Int32 id) {
    comboEntries_.addEntry(entryText, id);
    text = comboEntries_.getSelectedText();
    syncPopup(*this, comboEntries_);
}

void FGComboBox::deleteEntry(Int32 id) {
    comboEntries_.deleteEntry(id);
    text = comboEntries_.getSelectedText();
    syncPopup(*this, comboEntries_);
}

Int32 FGComboBox::findEntry(std::string_view entryText) const {
    return comboEntries_.findEntry(entryText);
}

void FGComboBox::clearEntries() {
    comboEntries_.clearEntries();
    text.clear();
    list().clear();
}

void FGComboBox::setSelectedEntry(Int32 id) {
    comboEntries_.setSelectedEntry(id);
    text = comboEntries_.getSelectedText();
    (void)list().variableChanged(text);
}

Int32 FGComboBox::getSelectedEntry() const noexcept {
    return comboEntries_.findEntry(text);
}

std::string FGComboBox::getSelectedText() const {
    return text;
}

std::function<void()> FGComboBox::checkpoint() {
    auto baseSnap = Parent::checkpoint();
    const auto savedRoot = bitmapRootName_;
    const auto savedBitmaps = bitmaps_;
    const std::vector<FGComboList::EntryRep> entries(comboEntries_.entries().begin(),
                                                     comboEntries_.entries().end());
    const auto selected = getSelectedEntry();
    return [this, baseSnap = std::move(baseSnap), savedRoot, savedBitmaps, entries, selected]() {
        baseSnap();
        bitmapRootName_ = savedRoot;
        bitmaps_ = savedBitmaps;
        comboEntries_.clearEntries();
        for (const auto& entry : entries) {
            comboEntries_.addEntry(entry.name, entry.id);
        }
        comboEntries_.setSelectedEntry(selected);
        syncPopup(*this, comboEntries_);
    };
}

bool FGComboBox::inspectRead(sim::Inspect& inspector) {
    sim::Inspect::Value v{};
    if (!inspector.read(sim::Inspect::Type::IString, kIdItgBmpRootTag, v)) {
        return false;
    }
    bitmapRootName_ = std::get<std::string>(v);
    return true;
}

bool FGComboBox::inspectWrite(sim::Inspect& inspector) const {
    return inspector.write(kIdItgBmpRootTag, bitmapRootName_);
}

Persistent::Error
FGComboBox::read(StreamIO& sio, Int32 /*version*/, Int32 user, Persistent::LoadContext& ctx) {
    auto snap = checkpoint();
    Int32 currentVersion = 0;
    std::string root{};
    if (!sio.read(currentVersion) || currentVersion != ComboBoxVersion || !sio.readString(root)) {
        snap();
        return Persistent::Error::ReadError;
    }
    bitmapRootName_ = std::move(root);
    const auto err = Parent::read(sio, 0, user, ctx);
    if (err != Persistent::Error::Ok) {
        snap();
    }
    return err;
}

Persistent::Error FGComboBox::write(StreamIO& sio,
                                    Int32 /*version*/,
                                    Int32 user,
                                    Persistent::StoreContext& ctx) const {
    if (bitmapRootName_.size() > 255) {
        return Persistent::Error::WriteError;
    }
    const Int32 currentVersion = ComboBoxVersion;
    if (!sio.write(currentVersion) || !sio.writeString(bitmapRootName_)) {
        return Persistent::Error::WriteError;
    }
    return Parent::write(sio, 0, user, ctx);
}

} // namespace opent1::game
