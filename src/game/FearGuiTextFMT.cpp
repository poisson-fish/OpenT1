#include <opent1/game/shell_controls.hpp>

#include <algorithm>

namespace opent1::game {

bool FearGuiFormattedText::setScriptValue(std::string_view rawText, Int32 formatWidth) {
    const Int32 w =
        std::max<Int32>(20, parent() != nullptr ? parent()->fields().extent.x : formatWidth);
    if (!format(rawText, w)) {
        return false;
    }
    return resize(fields_.position, {w, height() + 4});
}

} // namespace opent1::game
