#include <opent1/game/hud_controls.hpp>

#include <algorithm>

namespace opent1::game {

HudManager::HudManager() = default;

void HudManager::addHud(HudCtrl* hud, Int32 onBmpTag, Int32 offBmpTag, Point2I bmpExtent) {
    HudEntry entry{};
    entry.hud = hud;
    entry.onBitmapTag = onBmpTag;
    entry.offBitmapTag = offBmpTag;
    entry.bmpExtent = bmpExtent;
    entries_.push_back(std::move(entry));
    recomputeLayout();
}

void HudManager::removeHud(const HudCtrl* hud) {
    entries_.erase(std::remove_if(entries_.begin(),
                                  entries_.end(),
                                  [hud](const HudEntry& e) {
                                      return e.hud == hud;
                                  }),
                   entries_.end());
    recomputeLayout();
}

void HudManager::clearHuds() noexcept {
    entries_.clear();
    minimize_ = false;
}

void HudManager::recomputeLayout() {
    if (entries_.empty()) {
        return;
    }
    const Point2I firstBmp = entries_.front().bmpExtent;
    if (entries_.size() == 1) {
        hudWidth_ = firstBmp.x;
        hudHeight_ = firstBmp.y;
        maxExtent_ = firstBmp;
        resize(fields_.position, firstBmp);
    } else {
        hudWidth_ = firstBmp.x + HUD_SPACING;
        hudHeight_ = firstBmp.y + HUD_SPACING;
        const Int32 count = static_cast<Int32>(entries_.size());
        const Int32 cols = std::min<Int32>(count, HUDS_PER_ROW);
        const Int32 rows = (count / HUDS_PER_ROW) + ((count % HUDS_PER_ROW) != 0 ? 1 : 0);
        maxExtent_ = {hudWidth_ * cols, hudHeight_ * rows};
        const Point2I ext{minimize_ ? firstBmp.x : maxExtent_.x, maxExtent_.y};
        resize(fields_.position, ext);
    }
    snap();
}

void HudManager::setMinimized(bool min) {
    minimize_ = min;
    if (!entries_.empty()) {
        const Int32 firstWidth = entries_.front().bmpExtent.x;
        resize(fields_.position, {minimize_ ? firstWidth : maxExtent_.x, maxExtent_.y});
    }
}

bool HudManager::clickIndex(std::size_t index) {
    if (index >= entries_.size()) {
        return false;
    }
    if (index == 0) {
        setMinimized(!minimize_);
        return true;
    }
    if (minimize_) {
        return false;
    }
    auto& entry = entries_[index];
    entry.pressed = true;
    if (entry.hud != nullptr) {
        entry.hud->toggleActive();
    }
    return true;
}

bool HudManager::event(const gui::Event& e) {
    if (e.type == gui::EventType::Down && e.button == 1 && !entries_.empty()) {
        const Point2I local = globalToLocal(e.point);
        if (hudWidth_ > 0 && hudHeight_ > 0 && local.x >= 0 && local.y >= 0) {
            const Int32 col = local.x / hudWidth_;
            const Int32 row = local.y / hudHeight_;
            if (col >= 0 && col < HUDS_PER_ROW && row >= 0) {
                const auto idx = static_cast<std::size_t>(row * HUDS_PER_ROW + col);
                if (clickIndex(idx)) {
                    return true;
                }
            }
        }
    }
    return Parent::event(e);
}

bool HudManager::draw(gfx::SoftwareSurface& s, Point2I offset, RectI clip) const {
    return Parent::draw(s, offset, clip);
}

} // namespace opent1::game
