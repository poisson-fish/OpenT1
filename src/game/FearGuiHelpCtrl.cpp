#include "opent1/game/shell_controls.hpp"

#include <algorithm>

namespace opent1::game {

FGHelpCtrl::TooltipLayout FGHelpCtrl::computeTagTooltip(std::string_view tagSpec,
                                                        UInt32 timeElapsedMs,
                                                        bool mouseClicked,
                                                        Point2I cursorBottomLeft,
                                                        Point2I surfaceExtent,
                                                        bool helpPopupsEnabled) {
    TooltipLayout layout{};
    if (!helpPopupsEnabled || tagSpec.empty()) {
        return layout;
    }
    const auto commaPos = tagSpec.find(',');
    const std::string_view firstPart =
        (commaPos == std::string_view::npos) ? tagSpec : tagSpec.substr(0, commaPos);
    const std::string_view secondPart =
        (commaPos == std::string_view::npos) ? std::string_view{} : tagSpec.substr(commaPos + 1);

    std::string_view chosen{};
    if (mouseClicked && !secondPart.empty()) {
        chosen = secondPart;
    } else if (timeElapsedMs >= HoverDelayThresholdMs) {
        chosen = firstPart;
    }
    if (chosen.empty()) {
        return layout;
    }
    return computeDirectTooltip(chosen, cursorBottomLeft, surfaceExtent);
}

FGHelpCtrl::TooltipLayout FGHelpCtrl::computeDirectTooltip(std::string_view rawText,
                                                           Point2I cursorBottomLeft,
                                                           Point2I surfaceExtent) {
    TooltipLayout layout{};
    if (rawText.empty()) {
        return layout;
    }
    layout.visible = true;
    layout.body = std::string(rawText);
    const Int32 width = std::clamp(static_cast<Int32>(rawText.size()) * 7 + 12, 80, 260);
    const Int32 lines = std::max(1, static_cast<Int32>((rawText.size() * 7) / 240) + 1);
    const Int32 height = lines * 16 + 8;
    layout.extent = {width, height};
    layout.position.x = std::clamp(cursorBottomLeft.x, 0, std::max(0, surfaceExtent.x - width));
    layout.position.y =
        std::clamp(cursorBottomLeft.y + 4, 0, std::max(0, surfaceExtent.y - height));
    return layout;
}

} // namespace opent1::game
