#include <opent1/sim/sim_inspector.hpp>
#include <algorithm>
#include <bit>
#include <charconv>
#include <cmath>
#include <ranges>

namespace opent1::sim {
namespace {
using Control = InspectorControl;

bool interactive(Control c) {
    return c == Control::Text || c == Control::Checkbox || c == Control::Choice ||
           c == Control::Action;
}

std::string_view trim(std::string_view s) {
    const auto first = s.find_first_not_of(" \t");
    return first == s.npos ? std::string_view{}
                           : s.substr(first, s.find_last_not_of(" \t") - first + 1);
}

template <class T> bool number(std::string_view text, T& value, bool hex = false) {
    text = trim(text);
    if (text.empty()) {
        return false;
    }
    if constexpr (std::same_as<T, Int32>) {
        if (hex) {
            if (text.starts_with("0x") || text.starts_with("0X")) {
                text.remove_prefix(2);
            }
            UInt32 bits{};
            const auto result = std::from_chars(text.data(), text.data() + text.size(), bits, 16);
            if (result.ec != std::errc{} || result.ptr != text.data() + text.size()) {
                return false;
            }
            value = std::bit_cast<Int32>(bits);
            return true;
        }
    }
    if (text.starts_with('+')) {
        text.remove_prefix(1);
        if (text.starts_with('-')) {
            return false;
        }
    }
    if (text.empty() || text.front() == '+') {
        return false;
    }
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    if (result.ec != std::errc{} || result.ptr != text.data() + text.size()) {
        return false;
    }
    if constexpr (std::floating_point<T>) {
        return std::isfinite(value);
    } else {
        return true;
    }
}

template <class T, std::size_t N> bool components(std::string_view text, std::array<T, N>& result) {
    bool ok = true;
    std::ranges::for_each(std::views::iota(std::size_t{0}, N), [&](auto i) {
        text = trim(text);
        const auto end = text.find_first_of(" \t");
        const auto token = text.substr(0, end);
        ok = number(token, result[i]) && ok;
        text = end == text.npos ? std::string_view{} : text.substr(end);
    });
    return ok && trim(text).empty();
}

std::string tagLabel(const TagDictionary& tags, Int32 id, bool define = false) {
    const auto text = define ? tags.idToDefine(id) : tags.idToString(id);
    return text ? std::string(*text) : std::format("#{}", id);
}

std::optional<Int32> selected(const Inspect::Value& v) {
    return std::visit(
        [](const auto& value) -> std::optional<Int32> {
            using T = std::decay_t<decltype(value)>;
            if constexpr (std::same_as<T, Inspect::Tag> || std::same_as<T, Inspect::TagRange> ||
                          std::same_as<T, Inspect::TagList> ||
                          std::same_as<T, Inspect::StringList>) {
                return value.value;
            } else {
                return {};
            }
        },
        v);
}
}

bool SimInspector::open(SimInspection document, const TagDictionary& tags) {
    if (document.records().size() > MaxFields) {
        return false;
    }
    std::vector<InspectorField> fields;
    fields.reserve(document.records().size());
    std::size_t choices{}, bytes{}, work{};
    bool valid = true;
    std::ranges::for_each(document.records(), [&](const auto& record) {
        if (!valid) {
            return;
        }
        InspectorField field;
        field.label = tagLabel(tags, record.description);
        const auto addChoice = [&](std::string label, Int32 value) {
            if (!valid) {
                return;
            }
            if (choices == MaxChoices || label.size() > MaxTextBytes - bytes) {
                valid = false;
                return;
            }
            ++choices;
            bytes += label.size();
            field.choices.push_back({std::move(label), value});
        };
        std::visit(
            [&](const auto& value) {
                using T = std::decay_t<decltype(value)>;
                if constexpr (std::same_as<T, std::monostate>) {
                    field.control = record.type == Inspect::Type::Divider ? Control::Divider
                                                                          : Control::Description;
                } else if constexpr (std::same_as<T, Inspect::Action>) {
                    field.control = Control::Action;
                    field.text = "Run";
                } else if constexpr (std::same_as<T, bool>) {
                    field.control = Control::Checkbox;
                } else if constexpr (std::same_as<T, Inspect::StringIndex>) {
                    field.label = std::format("{} {}", value.text, value.index);
                    field.control = Control::Description;
                } else if constexpr (std::same_as<T, std::string>) {
                    field.control = record.type == Inspect::Type::IString ? Control::Text
                                                                          : Control::Description;
                    if (field.control == Control::Description) {
                        field.label = value;
                    }
                } else if constexpr (std::same_as<T, Inspect::Tag> ||
                                     std::same_as<T, Inspect::TagRange>) {
                    field.control = Control::Choice;
                    std::vector<Int32> ids;
                    if (tags.size() > MaxChoices) {
                        valid = false;
                        return;
                    }
                    if constexpr (std::same_as<T, Inspect::Tag>) {
                        // Original filters match defines, even when showing values.
                        std::ranges::for_each(
                            value.filter | std::views::split(','), [&](auto pattern) {
                                if (!valid || (work += tags.size()) > 16 * 1024 * 1024) {
                                    valid = false;
                                    return;
                                }
                                tags.findDefines(ids,
                                                 std::string_view(pattern.begin(), pattern.end()));
                                std::ranges::sort(ids);
                                ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
                            });
                        addChoice("<NONE>", 0);
                    } else {
                        if ((work += tags.size()) > 16 * 1024 * 1024) {
                            valid = false;
                            return;
                        }
                        tags.findIDs(ids, value.min, value.max);
                        if (value.min <= 0 && value.max >= 0) {
                            addChoice("<NONE>", 0);
                        }
                    }
                    std::ranges::for_each(ids, [&](auto id) {
                        if (valid) {
                            addChoice(tagLabel(tags, id, value.listDefines), id);
                        }
                    });
                } else if constexpr (std::same_as<T, Inspect::TagList> ||
                                     std::same_as<T, Inspect::StringList>) {
                    field.control = Control::Choice;
                    if (value.choices.size() > MaxChoices) {
                        valid = false;
                        return;
                    }
                    std::ranges::for_each(value.choices, [&](const auto& choice) {
                        if (!valid) {
                            return;
                        }
                        if constexpr (std::same_as<T, Inspect::TagList>) {
                            addChoice(tagLabel(tags, choice.tag), choice.value);
                        } else {
                            addChoice(choice.text, choice.value);
                        }
                    });
                } else {
                    field.control = Control::Text;
                }
            },
            record.value);
        const auto labelBytes = field.label.size() * 2 + 256;
        if (!valid || labelBytes > MaxTextBytes - bytes) {
            valid = false;
            return;
        }
        bytes += labelBytes;
        fields.push_back(std::move(field));
    });
    if (!valid) {
        return false;
    }
    document_ = std::move(document);
    baseline_ = document_;
    fields_ = std::move(fields);
    ++revision_;
    focus_.reset();
    first_ = cursor_ = 0;
    dirty_ = selectAll_ = choiceOpen_ = false;
    status_.clear();
    std::ranges::for_each(std::views::iota(std::size_t{0}, fields_.size()), [&](auto i) {
        sync(i);
    });
    return true;
}

void SimInspector::close() {
    ++revision_;
    document_.reset();
    baseline_.reset();
    fields_.clear();
    focus_.reset();
    first_ = cursor_ = 0;
    dirty_ = selectAll_ = choiceOpen_ = false;
    status_.clear();
}

bool SimInspector::editing() const noexcept {
    return focus_ && fields_[*focus_].control == Control::Text;
}

bool SimInspector::resize(Int32 width, Int32 height) noexcept {
    if (width < 240 || height < 128 || width > gfx::Bitmap::MaxDimension ||
        height > gfx::Bitmap::MaxDimension) {
        return false;
    }
    size_ = {width, height};
    choiceOpen_ = false;
    scroll(0);
    reveal();
    return true;
}

std::size_t SimInspector::visibleRows() const noexcept {
    return std::size_t((size_.y - HeaderHeight - FooterHeight) / RowHeight);
}

gfx::RectI SimInspector::fieldRect(std::size_t index) const noexcept {
    if (index >= fields_.size() || index < first_ || index - first_ >= visibleRows()) {
        return {};
    }
    const auto y = HeaderHeight + Int32(index - first_) * RowHeight;
    return {8, y, size_.x - 9, y + RowHeight - 3};
}

gfx::RectI SimInspector::applyRect() const noexcept {
    return {8, size_.y - 30, 103, size_.y - 7};
}

gfx::RectI SimInspector::revertRect() const noexcept {
    return {112, size_.y - 30, 207, size_.y - 7};
}

gfx::RectI SimInspector::choiceRect() const noexcept {
    if (!choiceOpen_ || !focus_) {
        return {};
    }
    const auto field = fieldRect(*focus_);
    if (!field.valid()) {
        return {};
    }
    const auto rows = std::min({std::size_t{6},
                                fields_[*focus_].choices.size(),
                                std::size_t((size_.y - HeaderHeight - FooterHeight) / 20)});
    const auto height = Int32(rows) * 20;
    const auto top = std::clamp(field.bottom + 1, HeaderHeight, size_.y - FooterHeight - height);
    return {field.left, top, field.right, top + height - 1};
}

void SimInspector::revealChoice() noexcept {
    const auto rows = std::size_t(choiceRect().height() / 20);
    if (choiceCursor_ < choiceFirst_) {
        choiceFirst_ = choiceCursor_;
    }
    if (rows && choiceCursor_ >= choiceFirst_ + rows) {
        choiceFirst_ = choiceCursor_ - rows + 1;
    }
}

void SimInspector::scroll(Int32 rows) noexcept {
    if (choiceOpen_ && focus_ && !fields_[*focus_].choices.empty()) {
        choiceSearch_.clear();
        choiceCursor_ = std::size_t(std::clamp(
            Int64(choiceCursor_) + rows, Int64{0}, Int64(fields_[*focus_].choices.size() - 1)));
        revealChoice();
        return;
    }
    const auto max = fields_.size() > visibleRows() ? fields_.size() - visibleRows() : 0;
    first_ = std::size_t(std::clamp(Int64(first_) + rows, Int64{0}, Int64(max)));
}

void SimInspector::reveal() noexcept {
    if (!focus_) {
        return;
    }
    if (*focus_ < first_) {
        first_ = *focus_;
    }
    if (*focus_ >= first_ + visibleRows()) {
        first_ = *focus_ - visibleRows() + 1;
    }
}

std::string SimInspector::valueText(std::size_t index) const {
    return std::visit(
        [&](const auto& value) -> std::string {
            using T = std::decay_t<decltype(value)>;
            if constexpr (std::same_as<T, Inspect::Integer>) {
                return value.display == Inspect::Display::Hex
                           ? std::format("0x{:08X}", std::bit_cast<UInt32>(value.value))
                           : std::format("{}", value.value);
            } else if constexpr (std::same_as<T, Flt32>) {
                return std::format("{}", value);
            } else if constexpr (std::same_as<T, bool>) {
                return value ? "[x]" : "[ ]";
            } else if constexpr (std::same_as<T, std::string>) {
                return value;
            } else if constexpr (std::same_as<T, Point2I> || std::same_as<T, Point2F>) {
                return std::format("{} {}", value.x, value.y);
            } else if constexpr (std::same_as<T, Point3I> || std::same_as<T, Point3F>) {
                return std::format("{} {} {}", value.x, value.y, value.z);
            } else if constexpr (std::same_as<T, Inspect::Action>) {
                return "Run";
            } else {
                const auto id = selected(document_->records()[index].value);
                if (id) {
                    const auto it =
                        std::ranges::find(fields_[index].choices, *id, &InspectorChoice::value);
                    return it != fields_[index].choices.end()
                               ? it->label
                               : std::format("<{} unavailable>", *id);
                }
                return {};
            }
        },
        document_->records()[index].value);
}

void SimInspector::sync(std::size_t index) {
    fields_[index].text = valueText(index);
    fields_[index].valid = true;
}

bool SimInspector::commit(std::size_t index) {
    auto& field = fields_[index];
    if (field.control != Control::Text) {
        return true;
    }
    auto value = document_->records()[index].value;
    const bool valid = std::visit(
        [&](auto& v) {
            using T = std::decay_t<decltype(v)>;
            if constexpr (std::same_as<T, Inspect::Integer>) {
                return number(field.text, v.value, v.display == Inspect::Display::Hex);
            } else if constexpr (std::same_as<T, Flt32>) {
                return number(field.text, v);
            } else if constexpr (std::same_as<T, std::string>) {
                v = field.text;
                return true;
            } else if constexpr (std::same_as<T, Point2I> || std::same_as<T, Point3I> ||
                                 std::same_as<T, Point2F> || std::same_as<T, Point3F>) {
                constexpr bool three = std::same_as<T, Point3I> || std::same_as<T, Point3F>;
                std::array<decltype(v.x), three ? 3 : 2> values{};
                if (!components(field.text, values)) {
                    return false;
                }
                v.x = values[0];
                v.y = values[1];
                if constexpr (three) {
                    v.z = values[2];
                }
                return true;
            } else {
                return false;
            }
        },
        value);
    field.valid = valid && document_->setValue(index, std::move(value));
    if (!field.valid) {
        status_ = "Invalid value; correct it or press Escape";
    }
    return field.valid;
}

bool SimInspector::focus(std::size_t index) {
    if (index >= fields_.size() || !interactive(fields_[index].control)) {
        return false;
    }
    if (focus_ && *focus_ != index && !commit(*focus_)) {
        return false;
    }
    choiceOpen_ = false;
    focus_ = index;
    cursor_ = fields_[index].text.size();
    selectAll_ = editing();
    reveal();
    return true;
}

bool SimInspector::setText(std::size_t index, std::string_view text) {
    if (index >= fields_.size() || fields_[index].control != Control::Text || text.size() > 256 ||
        std::ranges::any_of(text, [](unsigned char c) {
            return c < 32 || c == 127;
        })) {
        return false;
    }
    fields_[index].text = text;
    ++revision_;
    dirty_ = true;
    status_.clear();
    if (focus_ == index) {
        cursor_ = text.size();
        selectAll_ = false;
    }
    return commit(index);
}

bool SimInspector::select(std::size_t index, std::size_t choice) {
    if (index >= fields_.size() || fields_[index].control != Control::Choice ||
        choice >= fields_[index].choices.size()) {
        return false;
    }
    auto value = document_->records()[index].value;
    std::visit(
        [&](auto& v) {
            using T = std::decay_t<decltype(v)>;
            if constexpr (std::same_as<T, Inspect::Tag> || std::same_as<T, Inspect::TagRange> ||
                          std::same_as<T, Inspect::TagList> ||
                          std::same_as<T, Inspect::StringList>) {
                v.value = fields_[index].choices[choice].value;
            }
        },
        value);
    if (!document_->setValue(index, std::move(value))) {
        return false;
    }
    sync(index);
    ++revision_;
    choiceOpen_ = false;
    dirty_ = true;
    status_.clear();
    return true;
}

bool SimInspector::toggle(std::size_t index) {
    if (index >= fields_.size() || fields_[index].control != Control::Checkbox) {
        return false;
    }
    if (!document_->setValue(index, !std::get<bool>(document_->records()[index].value))) {
        return false;
    }
    sync(index);
    ++revision_;
    dirty_ = true;
    status_.clear();
    return true;
}

bool SimInspector::insert(std::string_view text) {
    if (choiceOpen_ && focus_ && !text.empty()) {
        if (text.size() > 256 - choiceSearch_.size() ||
            std::ranges::any_of(text, [](unsigned char c) {
                return c < 32 || c == 127;
            })) {
            return false;
        }
        auto query = choiceSearch_ + std::string(text);
        const auto& choices = fields_[*focus_].choices;
        const auto matches = [&](const auto& choice) {
            if (query.size() > choice.label.size()) {
                return false;
            }
            const auto lower = [](unsigned char c) {
                return c >= 'A' && c <= 'Z' ? c + 32 : c;
            };
            return std::ranges::equal(
                query, std::string_view(choice.label).substr(0, query.size()), [&](auto a, auto b) {
                    return lower(a) == lower(b);
                });
        };
        auto found = std::ranges::find_if(choices, matches);
        if (found == choices.end() && !choiceSearch_.empty()) {
            query = text;
            found = std::ranges::find_if(choices, matches);
        }
        if (found == choices.end()) {
            return false;
        }
        choiceSearch_ = std::move(query);
        choiceCursor_ = std::size_t(found - choices.begin());
        revealChoice();
        return true;
    }
    if (!editing() || text.empty() || std::ranges::any_of(text, [](unsigned char c) {
            return c < 32 || c == 127;
        })) {
        return false;
    }
    auto value = fields_[*focus_].text;
    if (selectAll_) {
        value.clear();
    }
    const auto at = selectAll_ ? 0 : cursor_;
    if (text.size() > 256 - value.size()) {
        return false;
    }
    value.insert(at, text);
    const bool result = setText(*focus_, value);
    cursor_ = at + text.size();
    selectAll_ = false;
    return result;
}

bool SimInspector::key(InspectorKey key) {
    if (!document_) {
        return false;
    }
    if (key == InspectorKey::Next || key == InspectorKey::Previous) {
        if (fields_.empty()) {
            return false;
        }
        const auto count = fields_.size();
        const auto start = focus_.value_or(key == InspectorKey::Next ? count - 1 : 0);
        const auto order =
            std::views::iota(std::size_t{1}, count + 1) | std::views::transform([&](auto offset) {
                return key == InspectorKey::Next ? (start + offset) % count
                                                 : (start + count - offset) % count;
            });
        const auto next = std::ranges::find_if(order, [&](auto i) {
            return interactive(fields_[i].control);
        });
        return next != order.end() && focus(*next);
    }
    if (!focus_) {
        return false;
    }
    const auto index = *focus_;
    auto& field = fields_[index];
    if (key == InspectorKey::Cancel) {
        if (choiceOpen_) {
            choiceOpen_ = false;
            return true;
        }
        sync(index);
        cursor_ = field.text.size();
        selectAll_ = false;
        status_.clear();
        return true;
    }
    if (field.control == Control::Checkbox && key == InspectorKey::Accept) {
        return toggle(index);
    }
    if (field.control == Control::Choice &&
        (key == InspectorKey::Left || key == InspectorKey::Right || key == InspectorKey::Accept)) {
        if (field.choices.empty()) {
            return false;
        }
        if (choiceOpen_) {
            if (key == InspectorKey::Accept) {
                return select(index, choiceCursor_);
            }
            scroll(key == InspectorKey::Left ? -1 : 1);
            return true;
        }
        const auto id = selected(document_->records()[index].value);
        const auto found = std::ranges::find(field.choices, *id, &InspectorChoice::value);
        if (key == InspectorKey::Accept) {
            choiceOpen_ = true;
            choiceFirst_ = 0;
            choiceSearch_.clear();
            choiceCursor_ =
                found == field.choices.end() ? 0 : std::size_t(found - field.choices.begin());
            revealChoice();
            return true;
        }
        std::size_t choice{};
        if (found != field.choices.end()) {
            const auto current = std::size_t(found - field.choices.begin()),
                       count = field.choices.size();
            choice =
                key == InspectorKey::Left ? (current + count - 1) % count : (current + 1) % count;
        }
        return select(index, choice);
    }
    if (choiceOpen_ && (key == InspectorKey::Home || key == InspectorKey::End)) {
        choiceSearch_.clear();
        choiceCursor_ = key == InspectorKey::Home ? 0 : field.choices.size() - 1;
        revealChoice();
        return true;
    }
    if (!editing()) {
        return false;
    }
    if (key == InspectorKey::Accept) {
        return commit(index);
    }
    if (key == InspectorKey::SelectAll) {
        selectAll_ = true;
        return true;
    }
    if (key == InspectorKey::Home) {
        cursor_ = 0;
        selectAll_ = false;
        return true;
    }
    if (key == InspectorKey::End) {
        cursor_ = field.text.size();
        selectAll_ = false;
        return true;
    }
    if (key == InspectorKey::Left || key == InspectorKey::Right) {
        if (selectAll_) {
            cursor_ = key == InspectorKey::Left ? 0 : field.text.size();
        } else if (key == InspectorKey::Left && cursor_) {
            --cursor_;
        } else if (key == InspectorKey::Right && cursor_ < field.text.size()) {
            ++cursor_;
        }
        selectAll_ = false;
        return true;
    }
    if (key == InspectorKey::Backspace || key == InspectorKey::Delete) {
        auto text = field.text;
        auto at = cursor_;
        if (selectAll_) {
            text.clear();
            at = 0;
        } else if (key == InspectorKey::Backspace && at) {
            text.erase(--at, 1);
        } else if (key == InspectorKey::Delete && at < text.size()) {
            text.erase(at, 1);
        }
        const auto result = setText(index, text);
        cursor_ = at;
        selectAll_ = false;
        return result;
    }
    return false;
}

bool SimInspector::click(Point2I point, SimGame& game) {
    if (!document_) {
        return false;
    }
    if (choiceOpen_) {
        const auto box = choiceRect();
        if (box.contains(point)) {
            return select(*focus_, choiceFirst_ + std::size_t((point.y - box.top) / 20));
        }
        choiceOpen_ = false;
    }
    if (applyRect().contains(point)) {
        return apply(game);
    }
    if (revertRect().contains(point)) {
        revert();
        return true;
    }
    const auto rows = std::views::iota(first_, std::min(fields_.size(), first_ + visibleRows()));
    const auto found = std::ranges::find_if(rows, [&](auto i) {
        return fieldRect(i).contains(point);
    });
    if (found != rows.end()) {
        if (!focus(*found)) {
            return false;
        }
        switch (fields_[*found].control) {
            case Control::Checkbox:
                return toggle(*found);
            case Control::Choice:
                return key(InspectorKey::Accept);
            case Control::Action:
                return activate(game, *found);
            default:
                return true;
        }
    }
    return false;
}

bool SimInspector::apply(SimGame& game) {
    if (!document_) {
        return false;
    }
    choiceOpen_ = false;
    bool valid = true;
    std::ranges::for_each(std::views::iota(std::size_t{0}, fields_.size()), [&](auto i) {
        if (!commit(i)) {
            if (valid) {
                focus_ = i;
                cursor_ = fields_[i].text.size();
                selectAll_ = false;
                reveal();
            }
            valid = false;
        }
    });
    if (!valid) {
        return false;
    }
    // Callbacks may change the application UI. Retain the exact submitted doc.
    const auto submitted = *document_;
    const auto revision = revision_;
    const bool ok = game.applyInspection(submitted);
    if (revision_ != revision) {
        return ok;
    }
    if (!ok) {
        status_ = "Apply failed: target is unavailable, locked or changed";
        return false;
    }
    baseline_ = submitted;
    dirty_ = false;
    status_ = "Applied";
    return true;
}

bool SimInspector::activate(SimGame& game, std::size_t index) {
    if (!document_ || index >= fields_.size() || fields_[index].control != Control::Action) {
        return false;
    }
    const auto submitted = *document_;
    const auto revision = revision_;
    const bool ok = game.activateInspection(submitted, index);
    if (revision_ != revision) {
        return ok;
    }
    status_ = ok ? "Action sent" : "Action failed: target is unavailable, locked or changed";
    return ok;
}

void SimInspector::revert() {
    if (!baseline_) {
        return;
    }
    document_ = baseline_;
    std::ranges::for_each(std::views::iota(std::size_t{0}, fields_.size()), [&](auto i) {
        sync(i);
    });
    ++revision_;
    dirty_ = selectAll_ = choiceOpen_ = false;
    if (focus_) {
        cursor_ = fields_[*focus_].text.size();
    }
    status_ = "Reverted local edits";
}

bool SimInspector::draw(gfx::SoftwareSurface& surface,
                        const gfx::BitmapFont& font,
                        InspectorColors colors) const {
    if (surface.bitmap().width != size_.x || surface.bitmap().height != size_.y || !font.valid() ||
        font.info.height < 1 || font.info.height > 14 || font.info.scale != Point2I{65536, 65536}) {
        return false;
    }
    const auto oldClip = surface.clip();

    struct Restore {
        gfx::SoftwareSurface& surface;
        gfx::RectI clip;

        ~Restore() {
            surface.setClip(clip);
        }
    } restore{surface, oldClip};

    surface.fill(surface.bitmap().bounds(), colors.background);
    const auto label = [&](gfx::RectI box, std::string_view text) {
        const auto clipped = gfx::intersect(box, oldClip);
        surface.setClip(clipped.value_or(gfx::RectI{}));
        const bool ok = surface.text(font, {box.left + 3, box.top + 2}, text);
        surface.setClip(oldClip);
        return ok;
    };
    if (!document_) {
        return label({8, 8, size_.x - 9, 30}, "No object selected");
    }
    if (!label({8, 4, size_.x - 9, 30},
               std::format("{} : {}{}",
                           document_->objectName(),
                           document_->className(),
                           dirty_ ? " *" : ""))) {
        return false;
    }
    for (auto i : std::views::iota(first_) | std::views::take_while([&](auto i) {
                      return i < fields_.size() && i - first_ < visibleRows();
                  })) {
        const auto& field = fields_[i];
        const auto box = fieldRect(i);
        if (field.control == Control::Divider) {
            surface.line({box.left, box.top + 18}, {box.right, box.top + 18}, colors.border);
            continue;
        }
        if (field.control == Control::Description) {
            if (!label(box, field.label)) {
                return false;
            }
            continue;
        }
        if (!label({box.left, box.top, box.right, box.top + 15}, field.label)) {
            return false;
        }
        const gfx::RectI input{box.left, box.top + 16, box.right, box.bottom};
        surface.fill(input, colors.panel);
        surface.rectangle(input,
                          !field.valid  ? colors.error
                          : focus_ == i ? colors.focus
                                        : colors.border);
        std::string text = field.text;
        if (focus_ == i && field.control == Control::Text) {
            text.insert(std::min(cursor_, text.size()), "|");
            if (selectAll_) {
                text = "[" + text + "]";
            }
            const auto caret = std::min(cursor_ + std::size_t(selectAll_), text.size() - 1);
            std::size_t start{};
            while (start < caret && font.textWidth(std::string_view(text).substr(
                                        start, caret + 1 - start)) > input.width() - 10) {
                ++start;
            }
            text.erase(0, start);
        } else if (field.control == Control::Choice) {
            text += "  [v]";
        }
        if (!label(input, text)) {
            return false;
        }
    }
    if (choiceOpen_ && focus_) {
        const auto box = choiceRect();
        const auto& choices = fields_[*focus_].choices;
        surface.fill(box, colors.panel);
        surface.rectangle(box, colors.border);
        for (auto i : std::views::iota(choiceFirst_) | std::views::take_while([&](auto i) {
                          return i < choices.size() &&
                                 i - choiceFirst_ < std::size_t(box.height() / 20);
                      })) {
            const auto y = box.top + Int32(i - choiceFirst_) * 20;
            const gfx::RectI row{box.left, y, box.right, y + 19};
            if (i == choiceCursor_) {
                surface.rectangle(row, colors.focus);
            }
            if (!label(row,
                       std::format("{}{}", i == choiceCursor_ ? "> " : "  ", choices[i].label))) {
                return false;
            }
        }
    }
    if (!label({8, size_.y - 54, size_.x - 9, size_.y - 32},
               status_.empty() ? "Tab: next  Enter: edit/run  Ctrl+Enter: apply" : status_)) {
        return false;
    }
    surface.rectangle(applyRect(), colors.border);
    surface.rectangle(revertRect(), colors.border);
    return label(applyRect(), "Apply") && label(revertRect(), "Revert");
}
} // namespace opent1::sim
