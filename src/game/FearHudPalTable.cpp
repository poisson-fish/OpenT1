#include <opent1/game/hud_controls.hpp>

#include <limits>

namespace opent1::game {

namespace {

struct HudColorTarget {
    FGHPaletteTable::ColorSlot slot;
    UInt8 r;
    UInt8 g;
    UInt8 b;
    UInt8 fallback;
};

constexpr std::array<HudColorTarget, FGHPaletteTable::_COLOR_COUNT> kHudColorTargets{{
    {FGHPaletteTable::_BLACK, 1, 1, 1, ShellPaletteColor::BLACK},
    {FGHPaletteTable::_RED, 220, 0, 0, ShellPaletteColor::RED},
    {FGHPaletteTable::_GREEN_40, 0, 40, 0, ShellPaletteColor::GREEN_40},
    {FGHPaletteTable::_GREEN_78, 0, 78, 0, ShellPaletteColor::GREEN_78},
    {FGHPaletteTable::_GREEN_132, 0, 132, 0, ShellPaletteColor::GREEN_132},
    {FGHPaletteTable::_GREEN, 0, 255, 0, ShellPaletteColor::GREEN},
    {FGHPaletteTable::_BLUE, 0, 0, 255, ShellPaletteColor::BLUE},
    {FGHPaletteTable::_YELLOW, 255, 255, 0, ShellPaletteColor::YELLOW},
    {FGHPaletteTable::_BLUE_OUTLINE, 60, 140, 140, ShellPaletteColor::BLUE_OUTLINE},
    {FGHPaletteTable::_BLUE_HILITE, 87, 255, 255, ShellPaletteColor::BLUE_HILITE},
    {FGHPaletteTable::_WHITE, 255, 255, 255, ShellPaletteColor::WHITE},
}};

[[nodiscard]] UInt8
nearestPaletteIndex(const gfx::Palette& pal, UInt8 r, UInt8 g, UInt8 b) noexcept {
    if (pal.palettes.empty()) {
        return 0;
    }
    const auto& colors = pal.palettes.front().colors;
    UInt32 bestDist = std::numeric_limits<UInt32>::max();
    UInt8 bestIdx = 0;
    for (std::size_t i = 0; i < colors.size(); ++i) {
        const Int32 dr = static_cast<Int32>(colors[i].red) - static_cast<Int32>(r);
        const Int32 dg = static_cast<Int32>(colors[i].green) - static_cast<Int32>(g);
        const Int32 db = static_cast<Int32>(colors[i].blue) - static_cast<Int32>(b);
        const UInt32 dist = static_cast<UInt32>(dr * dr + dg * dg + db * db);
        if (dist < bestDist) {
            bestDist = dist;
            bestIdx = static_cast<UInt8>(i);
        }
    }
    return bestIdx;
}

} // namespace

FGHPaletteTable::FGHPaletteTable() {
    loadTable(nullptr);
}

void FGHPaletteTable::loadTable(const gfx::Palette* palette) noexcept {
    for (const auto& entry : kHudColorTargets) {
        if (palette != nullptr && !palette->palettes.empty()) {
            table_[entry.slot] = nearestPaletteIndex(*palette, entry.r, entry.g, entry.b);
        } else {
            table_[entry.slot] = entry.fallback;
        }
    }
}

UInt8 FGHPaletteTable::color(ColorSlot slot) const noexcept {
    const auto idx = static_cast<std::size_t>(slot);
    return idx < table_.size() ? table_[idx] : static_cast<UInt8>(ShellPaletteColor::WHITE);
}

Persistent::Error
FGHPaletteTable::read(StreamIO& sio, Int32 version, Int32 user, Persistent::LoadContext& ctx) {
    return Parent::read(sio, version, user, ctx);
}

Persistent::Error FGHPaletteTable::write(StreamIO& sio,
                                         Int32 version,
                                         Int32 user,
                                         Persistent::StoreContext& ctx) const {
    return Parent::write(sio, version, user, ctx);
}

} // namespace opent1::game
