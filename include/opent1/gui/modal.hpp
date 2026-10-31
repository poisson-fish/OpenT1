#pragma once
#include <opent1/gui/gui.hpp>

namespace opent1::gui {
// Owned dialogs can switch canvases (embedded chooser) without nesting pumps.
struct ModalView {
    std::function<Canvas&()> canvas;
    std::function<bool()> active;
    std::function<bool()> update;
    std::function<bool()> cancel;
};
}
