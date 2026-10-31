#pragma once
#include <opent1/game/hud_controls.hpp>

namespace opent1::game {
// fearGuiChat{Display}, fearGuiMenu: ALPHA_FILL 254 and the two-tone brackets.
inline bool drawHudPanel(gfx::SoftwareSurface& surface, RectI rect, Int32 bracket = 4) {
    const auto right = rect.right - 1, bottom = rect.bottom - 1;
    if (right <= rect.left || bottom <= rect.top) {
        return true;
    }
    const std::array<gfx::PolygonVertex, 4> vertices{{{{Flt32(rect.left), Flt32(rect.top)}},
                                                      {{Flt32(rect.right), Flt32(rect.top)}},
                                                      {{Flt32(rect.right), Flt32(rect.bottom)}},
                                                      {{Flt32(rect.left), Flt32(rect.bottom)}}}};
    gfx::PolygonMaterial material;
    material.paletteIndex = ALPHAPAL_INDEX;
    material.color = 254;
    material.blend = true;
    const auto* palette = surface.palette();
    if (!palette || palette->find(ALPHAPAL_INDEX).type != gfx::PaletteType::Translucent) {
        // Resource-free GUI/editor previews have no legacy alpha table.
        surface.fill({rect.left, rect.top, right, bottom}, 0);
    } else if (!surface.polygon(vertices, material)) {
        return false;
    }
    if (bracket) {
        for (const auto x : {rect.left, right}) {
            const auto direction = x == rect.left ? 1 : -1;
            surface.line({x, rect.top}, {x, bottom}, GREEN_132);
            surface.line({x + direction, rect.top}, {x + direction, bottom}, GREEN);
            surface.line(
                {x + direction, rect.top}, {x + direction * (bracket + 1), rect.top}, GREEN);
            surface.line({x + direction, bottom}, {x + direction * (bracket + 1), bottom}, GREEN);
        }
    }
    return true;
}
}
