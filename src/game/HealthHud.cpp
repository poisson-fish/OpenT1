#include <opent1/game/status_combat_huds.hpp>

#include <algorithm>

namespace opent1::game {

HealthHud::HealthHud() {
    fields_.extent = {134, 33};
}

HealthHud::Metrics
HealthHud::computeMetrics(Flt32 damageLevel, bool lowRes, UInt32 timeElapsedMs) noexcept {
    Metrics m{};
    m.lowRes = lowRes;
    m.energyLevel = std::clamp(1.0f - damageLevel, 0.0f, 1.0f);
    if (!lowRes) {
        m.barWidth = 84;
        m.barOffset = {37, 10};
        m.energyWidth = m.energyLevel > 0.0f
                            ? std::max<Int32>(static_cast<Int32>(m.energyLevel * 85.0f), 1)
                            : 0;
        m.color = m.energyWidth >= 57
                      ? ShellPaletteColor::GREEN
                      : (m.energyWidth >= 28 ? ShellPaletteColor::YELLOW : ShellPaletteColor::RED);
        if (m.energyWidth >= 1 && m.energyWidth <= 27) {
            m.flashIcon = (((timeElapsedMs >> 9U) % 2U) != 0U);
        }
    } else {
        m.barWidth = 52;
        m.barOffset = {23, 7};
        m.energyWidth = static_cast<Int32>(m.energyLevel * 51.0f);
        m.color = m.energyWidth >= 34
                      ? ShellPaletteColor::GREEN
                      : (m.energyWidth >= 17 ? ShellPaletteColor::YELLOW : ShellPaletteColor::RED);
        if (m.energyWidth >= 1 && m.energyWidth <= 16) {
            m.flashIcon = (((timeElapsedMs >> 9U) % 2U) != 0U);
        }
    }
    return m;
}

void HealthHud::updateFromPSC(const PlayerPSC* psc, bool lowRes, UInt32 timeElapsedMs) noexcept {
    Flt32 damage = 0.0f;
    if (psc != nullptr && psc->getControlObject() != nullptr) {
        damage = psc->getControlObject()->getDamageLevel();
    }
    metrics_ = computeMetrics(damage, lowRes, timeElapsedMs);
    lowResMode_ = lowRes;
    resize(fields_.position, lowRes ? Point2I{83, 21} : Point2I{134, 33});
}

bool HealthHud::onAdd() {
    if (!Parent::onAdd()) {
        return false;
    }
    fields_.extent = {134, 33};
    return true;
}

bool HealthHud::draw(gfx::SoftwareSurface& s, Point2I offset, RectI clip) const {
    if (!Parent::draw(s, offset, clip)) {
        return false;
    }
    const std::string prefix = metrics_.lowRes ? "lr_" : "";
    if (!drawSprite(s, prefix + "hudTrans.bmp", offset + Point2I{1, 1})) {
        return false;
    }
    const bool red = metrics_.energyLevel <= 0 ||
                     (metrics_.color == ShellPaletteColor::RED && !metrics_.flashIcon);
    if (!drawSprite(s,
                    prefix + (red ? "HH_Icon_Red.bmp" : "HH_Icon_Green.bmp"),
                    offset + (metrics_.lowRes ? Point2I{4, 4} : Point2I{5, 3}))) {
        return false;
    }
    if (metrics_.energyWidth > 0) {
        const Point2I barStart{offset.x + metrics_.barOffset.x, offset.y + metrics_.barOffset.y};
        const Int32 barH = metrics_.lowRes ? 7 : 12;
        const auto* end =
            sprite(prefix + (metrics_.color == ShellPaletteColor::GREEN    ? "HH_End_Green.bmp"
                             : metrics_.color == ShellPaletteColor::YELLOW ? "HH_End_Yellow.bmp"
                                                                           : "HH_End_Red.bmp"));
        if (end) {
            const auto saved = s.clip();
            const auto barClip = gfx::intersect(saved,
                                                RectI{barStart.x,
                                                      barStart.y,
                                                      barStart.x + metrics_.energyWidth - 1,
                                                      barStart.y + end->height - 1});
            if (!barClip) {
                return true;
            }
            s.setClip(*barClip);
            const bool ok =
                s.blit(*end, barStart) &&
                s.blit(*end, barStart + Point2I{metrics_.barWidth - end->width, 0}, gfx::Flip::X);
            s.fill({barStart.x + end->width,
                    barStart.y,
                    barStart.x + metrics_.barWidth - end->width,
                    barStart.y + end->height - 1},
                   metrics_.color);
            s.setClip(saved);
            return ok;
        }
        s.fill(
            {barStart.x, barStart.y, barStart.x + metrics_.energyWidth - 1, barStart.y + barH - 1},
            metrics_.color);
    }
    return true;
}

} // namespace opent1::game
