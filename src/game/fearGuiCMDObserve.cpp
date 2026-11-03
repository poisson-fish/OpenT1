#include <opent1/game/commander_huds.hpp>
#include <ranges>
#include <algorithm>
#include <cmath>

namespace opent1::game {
Persistent::Error
CMDObserve::read(StreamIO& stream, Int32 version, Int32 user, Persistent::LoadContext& context) {
    return gui::Control::read(stream, version, user, context);
}

Persistent::Error CMDObserve::write(StreamIO& stream,
                                    Int32 version,
                                    Int32 user,
                                    Persistent::StoreContext& context) const {
    return gui::Control::write(stream, version, user, context);
}

gui::TSControl* CMDObserve::sceneControl() {
    if (auto* child = dynamic_cast<gui::TSControl*>(findTag(140208)); child && child != this) {
        return child;
    }
    return this;
}

void CMDObserve::wake() {
    Parent::wake();
    visible(false);
    observedPlayerId_ = -1;
    startupWidth_ = 0;
    startupHeight_ = 2;
    lastFrame_ = frameRemainder_ = 0;
}

bool CMDObserve::activate(bool enabled, Int32 selectedPlayer) {
    if (enabled == visible()) {
        return true;
    }
    if (enabled && selectedPlayer < 0) {
        return false;
    }
    if (!requestObservation) {
        return false;
    }
    if (!enabled) {
        requestObservation(-1);
        visible(false);
        observedPlayerId_ = -1;
        return true;
    }
    startupWidth_ = 0;
    startupHeight_ = 2;
    snowDensity_ = 4;
    healthPixels_ = energyPixels_ = 0;
    lastFrame_ = frameRemainder_ = 0;
    observedPlayerId_ = selectedPlayer;
    visible(true);
    requestObservation(selectedPlayer);
    return true;
}

bool CMDObserve::observePlayer(Int32 player) {
    if (!visible() || player == observedPlayerId_ || !requestObservation) {
        return false;
    }
    observedPlayerId_ = player;
    snowDensity_ = 4;
    healthPixels_ = energyPixels_ = 0;
    requestObservation(player);
    return true;
}

void CMDObserve::tick(double now) {
    if (!visible()) {
        return;
    }
    Parent::tick(now);
    if (lastFrame_ != 0) {
        frameRemainder_ += std::max(0., now - lastFrame_);
    }
    lastFrame_ = now;
    const auto [health, energy] = status ? status() : std::pair{0.f, 0.f};
    const auto* scene = sceneControl();
    const auto height =
                   std::max(2, scene == this ? fields_.extent.y - 8 : scene->fields().extent.y),
               width =
                   std::max(1, scene == this ? fields_.extent.x - 16 : scene->fields().extent.x);
    for ([[maybe_unused]] auto frames :
         std::views::iota(0) | std::views::take_while([&](int frames) {
             return frameRemainder_ >= .03 && frames < 120;
         })) {
        frameRemainder_ -= .03;
        ++snowFrame_;
        if (startupWidth_ < width) {
            startupWidth_ = std::min(width, startupWidth_ + 16);
        } else if (startupHeight_ < height) {
            startupHeight_ = std::min(height, startupHeight_ + 20);
        } else {
            snowDensity_ = std::min(80, snowDensity_ + 6);
            healthPixels_ =
                std::min(Int32(std::clamp(health, 0.f, 1.f) * Flt32(height)), healthPixels_ + 4);
            energyPixels_ =
                std::min(Int32(std::clamp(energy, 0.f, 1.f) * Flt32(height)), energyPixels_ + 4);
        }
    }
}

bool CMDObserve::draw(gfx::SoftwareSurface& surface, Point2I offset, RectI clip) const {
    surface.fill(
        {offset.x, offset.y, offset.x + fields_.extent.x - 1, offset.y + fields_.extent.y - 1}, 0);
    const auto before = surface.clip();
    const auto* scene = const_cast<CMDObserve*>(this)->sceneControl();
    const auto width =
                   std::max(1, scene == this ? fields_.extent.x - 16 : scene->fields().extent.x),
               height =
                   std::max(2, scene == this ? fields_.extent.y - 8 : scene->fields().extent.y);
    const auto origin = offset + (scene == this ? Point2I{0, 4} : scene->fields().position);
    const Point2I start{origin.x + (width - startupWidth_) / 2,
                        origin.y + (height - startupHeight_) / 2};
    surface.setClip({std::max({clip.left, before.left, start.x}),
                     std::max({clip.top, before.top, start.y}),
                     std::min({clip.right, before.right, start.x + startupWidth_ - 1}),
                     std::min({clip.bottom, before.bottom, start.y + startupHeight_ - 1})});
    const bool drawn =
        scene == this ? Parent::draw(surface, offset, clip) : drawChildren(surface, offset, clip);
    if (snowDensity_ < 80) {
        UInt32 noise = 0x9e3779b9U ^ snowFrame_;
        for (auto y : std::views::iota(Int32{0}) | std::views::take_while([&](Int32 y) {
                          return y < startupHeight_;
                      })) {
            for (auto x : std::views::iota(Int32{0}) | std::views::take_while([&](Int32 x) {
                              return x < startupWidth_;
                          })) {
                noise = noise * 1664525U + 1013904223U;
                if (noise % UInt32(snowDensity_) == UInt32(snowDensity_ / 2)) {
                    surface.point(start + Point2I{x, y}, 255);
                }
            }
        }
    }
    surface.setClip(before);
    if (healthPixels_ > 0) {
        surface.fill({offset.x + width + 4,
                      offset.y + 4 + height - healthPixels_,
                      offset.x + width + 5,
                      offset.y + 3 + height},
                     healthPixels_ >= 57   ? 245
                     : healthPixels_ >= 28 ? 244
                                           : 249);
    }
    if (energyPixels_ > 0) {
        surface.fill({offset.x + width + 8,
                      offset.y + 4 + height - energyPixels_,
                      offset.x + width + 9,
                      offset.y + 3 + height},
                     254);
    }
    return drawn;
}
} // namespace opent1::game
