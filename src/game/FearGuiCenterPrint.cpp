#include <opent1/game/status_combat_huds.hpp>
#include <ranges>
#include <opent1/game/hud_panel.hpp>

#include <algorithm>

namespace opent1::game {

CenterPrint::CenterPrint() = default;

void CenterPrint::setMessage(std::string_view msg,
                             Placement placement,
                             Flt32 timeoutSec,
                             Flt32 nowSec) {
    messageText_ = std::string(msg);
    placement_ = placement;
    timeoutSec_ = timeoutSec;
    startTimeSec_ = nowSec;
    ++resetCounter_;
}

void CenterPrint::clearMessage() noexcept {
    messageText_.clear();
    timeoutSec_ = 0.0f;
    ++resetCounter_;
}

CenterPrint::LayoutMetrics CenterPrint::computeLayout(Point2I parentExtent,
                                                      Int32 formattedHeight,
                                                      Flt32 nowSec) const noexcept {
    LayoutMetrics m{};
    m.lowRes = (parentExtent.x < 512 || parentExtent.y < 384);
    m.posOffset = m.lowRes ? 15 : 35;
    if (messageText_.empty() || placement_ < Center || placement_ > Top) {
        m.active = false;
        return m;
    }
    if (timeoutSec_ > 0.0f && (nowSec - startTimeSec_) >= timeoutSec_) {
        m.active = false;
        return m;
    }
    m.active = true;
    m.extent.x = std::max<Int32>(0, parentExtent.x - (m.lowRes ? 30 : 70));
    m.extent.y = std::max<Int32>(1, formattedHeight) + 8;
    m.position.x = m.posOffset;
    switch (placement_) {
        case Center:
            m.position.y = (parentExtent.y - m.extent.y) >> 1;
            break;
        case Bottom:
            m.position.y = parentExtent.y - m.extent.y - m.posOffset;
            break;
        case Top:
        default:
            m.position.y = m.posOffset;
            break;
    }
    return m;
}

bool CenterPrint::onAdd() {
    return Parent::onAdd();
}

void CenterPrint::tick(double nowSec) {
    if (resolveMessage) {
        auto [text, position] = resolveMessage();
        if (text != messageText_ || position != static_cast<Int32>(placement_)) {
            setMessage(text, static_cast<Placement>(position), 0, static_cast<Flt32>(nowSec));
        }
    }
    const auto extent = parent() ? parent()->fields().extent : fields_.extent;
    const bool small = extent.x < 512 || extent.y < 384;
    const auto& fonts = small ? smallMessageFonts : messageFonts;
    for (auto i : std::views::iota(std::size_t{0}, fonts.size())) {
        formatted_.fonts[i].resource = fonts[i];
    }
    renderActive_ =
        !messageText_.empty() && placement_ >= Center && placement_ <= Top &&
        formatted_.format(messageText_, std::max<Int32>(1, extent.x - (small ? 30 : 70) - 4));
    if (!renderActive_) {
        return;
    }
    const auto layout = computeLayout(extent, formatted_.height(), static_cast<Flt32>(nowSec));
    renderActive_ = layout.active;
    if (renderActive_) {
        (void)resize(layout.position, layout.extent);
    }
}

bool CenterPrint::draw(gfx::SoftwareSurface& s, Point2I offset, RectI clip) const {
    if (!renderActive_) {
        return true;
    }
    return drawHudPanel(
               s, {offset.x, offset.y, offset.x + fields_.extent.x, offset.y + fields_.extent.y}) &&
           formatted_.draw(s, offset + Point2I{2, 2}, clip);
}

} // namespace opent1::game
