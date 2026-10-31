#include <opent1/game/status_combat_huds.hpp>

#include <algorithm>

namespace opent1::game {

JetPackHud::JetPackHud() {
    fields_.extent = {134, 33};
}

JetPackHud::Metrics JetPackHud::computeMetrics(Flt32 energyRatio, bool lowRes) noexcept {
    Metrics m{};
    m.lowRes = lowRes;
    m.energyLevel = std::clamp(1.0f - energyRatio, 0.0f, 1.0f);
    m.color = ShellPaletteColor::LIGHT_BLUE;
    if (!lowRes) {
        m.barWidth = 84;
        m.barOffset = {37, 11};
        m.energyWidth = static_cast<Int32>(m.energyLevel * 85.0f);
    } else {
        m.barWidth = 52;
        m.barOffset = {23, 7};
        m.energyWidth = static_cast<Int32>(m.energyLevel * 51.0f);
    }
    return m;
}

void JetPackHud::updateFromPSC(const PlayerPSC* psc, bool lowRes) noexcept {
    Flt32 energyRatio = 0.0f;
    if (psc != nullptr && psc->getControlObject() != nullptr) {
        energyRatio = psc->getControlObject()->getEnergyLevel();
    }
    metrics_ = computeMetrics(energyRatio, lowRes);
    lowResMode_ = lowRes;
    resize(fields_.position, lowRes ? Point2I{83, 21} : Point2I{134, 33});
}

bool JetPackHud::onAdd() {
    if (!Parent::onAdd()) {
        return false;
    }
    fields_.extent = {134, 33};
    return true;
}

bool JetPackHud::draw(gfx::SoftwareSurface& s, Point2I offset, RectI clip) const {
    if (!Parent::draw(s, offset, clip)) {
        return false;
    }
    const std::string prefix = metrics_.lowRes ? "lr_" : "";
    if (!drawSprite(s,
                    prefix + "hudTrans.bmp",
                    offset + (metrics_.lowRes ? Point2I{1, -11} : Point2I{1, 1}),
                    gfx::Flip::Y) ||
        !drawSprite(s,
                    prefix + "HJ_Icon_Green.bmp",
                    offset + (metrics_.lowRes ? Point2I{3, 4} : Point2I{5, 8}))) {
        return false;
    }
    if (metrics_.energyWidth > 0) {
        const Point2I barStart{offset.x + metrics_.barOffset.x, offset.y + metrics_.barOffset.y};
        const Int32 barH = metrics_.lowRes ? 7 : 12;
        if (const auto* end = sprite(prefix + "HJ_End_Green.bmp")) {
            const auto saved = s.clip();
            s.setClip(gfx::intersect(saved,
                                     RectI{barStart.x,
                                           barStart.y,
                                           barStart.x + metrics_.energyWidth - 1,
                                           barStart.y + end->height - 1})
                          .value_or(RectI{}));
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
