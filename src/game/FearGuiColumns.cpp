#include <opent1/game/shell_controls.hpp>

#include <algorithm>
#include <charconv>
#include <format>

namespace opent1::game {

namespace {

constexpr std::array<ColumnListCtrl::ServerColumnPref, ColumnListCtrl::ServerColumnCount>
    kDefaultServerCols{{
        {0, "Favorite", 0, true, 24},
        {1, "Connection", 1, true, 24},
        {2, "Status", 2, true, 36},
        {3, "Server Name", 3, true, 150},
        {4, "Ping", 4, true, 40},
        {5, "Type", 5, true, 90},
        {6, "Mission", 6, true, 100},
        {7, "Players", 7, true, 56},
        {8, "CPU", 8, false, 45},
        {9, "IP Address", 9, false, 110},
        {10, "Version", 10, false, 50},
        {11, "Mod", 11, true, 75},
    }};

[[nodiscard]] std::string_view trimSpaces(std::string_view sv) noexcept {
    while (!sv.empty() && (sv.front() == ' ' || sv.front() == '\t')) {
        sv.remove_prefix(1);
    }
    while (!sv.empty() && (sv.back() == ' ' || sv.back() == '\t')) {
        sv.remove_suffix(1);
    }
    return sv;
}

} // namespace

ColumnListCtrl::ColumnListCtrl() {
    resetDefaultColumns();
    setSize({1, static_cast<Int32>(ServerColumnCount)});
}

bool ColumnListCtrl::drawCell(
    gfx::SoftwareSurface& s, Point2I p, Point2I cell, bool chosen, bool hovered) const {
    auto found = std::ranges::find(serverColumns_, cell.y, &ServerColumnPref::position);
    if (found == serverColumns_.end()) {
        return true;
    }
    const auto f = (chosen || hovered) && shellHighlightFont ? shellHighlightFont : shellFont;
    if (chosen) {
        s.fill({p.x, p.y, p.x + cellSize.x - 1, p.y + cellSize.y - 1}, BOX_FRAME);
    }
    if (f && !s.text(*f, {p.x + 6, p.y + 2}, found->name)) {
        return false;
    }
    const RectI box{p.x + cellSize.x - 22, p.y + 2, p.x + cellSize.x - 10, p.y + 14};
    s.rectangle(box, BOX_INSIDE);
    if (found->active) {
        s.line({box.left + 2, box.top + 2}, {box.right - 2, box.bottom - 2}, WHITE);
        s.line({box.right - 2, box.top + 2}, {box.left + 2, box.bottom - 2}, WHITE);
    }
    return true;
}

bool ColumnListCtrl::event(const gui::Event& e) {
    const auto p = globalToLocal(e.point);
    const auto row = p.y / cellSize.y;
    if (active && e.type == gui::EventType::Down && e.button == 1 && p.y >= 0 &&
        row < int(ServerColumnCount)) {
        if (p.x >= cellSize.x - 30) {
            toggleColumnActive(row);
            if (changed) {
                changed(*this);
            }
        } else {
            dragRow_ = row;
        }
    }
    if (e.type == gui::EventType::Drag && dragRow_ >= 0 && canvas() &&
        canvas()->capture() == this) {
        const auto next = std::clamp(row, 0, int(ServerColumnCount) - 1);
        if (moveColumnPosition(dragRow_, next)) {
            dragRow_ = next;
        }
        return true;
    }
    if (e.type == gui::EventType::Up || e.type == gui::EventType::Cancel) {
        if (dragRow_ >= 0 && changed) {
            changed(*this);
        }
        dragRow_ = -1;
    }
    return Parent::event(e);
}

std::function<void()> ColumnListCtrl::checkpoint() {
    auto base = Parent::checkpoint();
    const auto cols = serverColumns_;
    const auto drag = dragRow_;
    return [this, base = std::move(base), cols, drag] {
        base();
        serverColumns_ = cols;
        dragRow_ = drag;
    };
}

ColumnListCtrl::ServerColumnPref ColumnListCtrl::parseColumnPrefString(Int32 id,
                                                                       std::string_view name,
                                                                       std::string_view spec,
                                                                       Int32 defaultWidth) {
    ServerColumnPref out{id, name, id, true, defaultWidth};
    const auto c1 = spec.find(',');
    if (c1 == std::string_view::npos) {
        return out;
    }
    const auto c2 = spec.find(',', c1 + 1);
    if (c2 == std::string_view::npos) {
        return out;
    }
    const auto posToken = trimSpaces(spec.substr(0, c1));
    const auto actToken = trimSpaces(spec.substr(c1 + 1, c2 - c1 - 1));
    const auto widToken = trimSpaces(spec.substr(c2 + 1));

    Int32 parsedPos = id;
    std::from_chars(posToken.data(), posToken.data() + posToken.size(), parsedPos);
    out.position = std::clamp<Int32>(parsedPos, 0, static_cast<Int32>(ServerColumnCount) - 1);
    out.active = (actToken == "ON" || actToken == "on" || actToken == "TRUE" ||
                  actToken == "true" || actToken == "1");

    Int32 parsedWidth = defaultWidth;
    std::from_chars(widToken.data(), widToken.data() + widToken.size(), parsedWidth);
    out.width = std::clamp<Int32>(parsedWidth, 10, 640);
    return out;
}

std::string ColumnListCtrl::formatColumnPrefString(const ServerColumnPref& pref) {
    return std::format("{}, {}, {}", pref.position, pref.active ? "ON" : "OFF", pref.width);
}

void ColumnListCtrl::resetDefaultColumns() {
    serverColumns_ = kDefaultServerCols;
}

bool ColumnListCtrl::moveColumnPosition(Int32 fromPos, Int32 toPos) {
    if (fromPos < 0 || fromPos >= static_cast<Int32>(ServerColumnCount) || toPos < 0 ||
        toPos >= static_cast<Int32>(ServerColumnCount) || fromPos == toPos) {
        return false;
    }
    for (auto& col : serverColumns_) {
        if (col.position == fromPos) {
            col.position = toPos;
        } else if (fromPos < toPos && col.position > fromPos && col.position <= toPos) {
            --col.position;
        } else if (fromPos > toPos && col.position >= toPos && col.position < fromPos) {
            ++col.position;
        }
    }
    return true;
}

bool ColumnListCtrl::toggleColumnActive(Int32 pos) {
    for (auto& col : serverColumns_) {
        if (col.position == pos) {
            col.active = !col.active;
            return true;
        }
    }
    return false;
}

} // namespace opent1::game
