#include <opent1/game/hud_controls.hpp>

namespace opent1::game {

namespace {

constexpr std::array<HudListCtrl::HudToggleSpec, HudListCtrl::HudCount> kDefaultHudSpecs{{
    {140100, "Clock", true},
    {140101, "Compass", true},
    {140102, "Weapons", true},
    {140103, "Reticle", true},
    {140222, "Sniper Crosshair", true},
    {140104, "Health", true},
    {140111, "Jetpack", true},
    {140113, "Chat Display", true},
    {140219, "Sensor", true},
    {140221, "Logo", true},
}};

} // namespace

HudListCtrl::HudListCtrl() : specs_(kDefaultHudSpecs) {
    std::vector<ColumnInfo> cols{
        {-1, 10, 300, 0, 0, true, 110, "HUD"},
        {-1, 10, 300, 1, 1, true, 20, "Active"},
    };
    setColumns(std::move(cols));
    cellSize = {130, 24};
    setSize({1, static_cast<Int32>(HudCount)});
}

void HudListCtrl::updateResolution(Point2I surfaceExtent, Int32 hiFontHeight, Int32 loFontHeight) {
    lowRes_ = (surfaceExtent.x < 512 || surfaceExtent.y < 384);
    if (lowRes_) {
        cellSize = {85, loFontHeight + 6};
    } else {
        cellSize = {130, hiFontHeight + 12};
    }
    setSize({1, static_cast<Int32>(HudCount)});
}

bool HudListCtrl::toggleRow(std::size_t row) {
    if (row >= specs_.size()) {
        return false;
    }
    auto& spec = specs_[row];
    spec.active = !spec.active;
    if (spec.tag != 140222) {
        if (auto* target = findRootTag(static_cast<UInt32>(spec.tag))) {
            target->visible(spec.active);
        }
    } else if (getManager() != nullptr && getManager()->console() != nullptr) {
        getManager()->console()->setVariable("pref::SniperCrosshair",
                                             spec.active ? "TRUE" : "FALSE");
    }
    return true;
}

void HudListCtrl::wake() {
    setSize({1, static_cast<Int32>(HudCount)});
}

bool HudListCtrl::select(Point2I cell) {
    if (!Parent::select(cell)) {
        return false;
    }
    if (cell.y >= 0 && static_cast<std::size_t>(cell.y) < specs_.size()) {
        toggleRow(static_cast<std::size_t>(cell.y));
    }
    return true;
}

std::string HudListCtrl::getCellText(Point2I cell) const {
    if (cell.y < 0 || static_cast<std::size_t>(cell.y) >= specs_.size()) {
        return {};
    }
    return std::string(specs_[static_cast<std::size_t>(cell.y)].label);
}

} // namespace opent1::game
