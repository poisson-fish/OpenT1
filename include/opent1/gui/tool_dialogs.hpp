#pragma once
#include <opent1/gui/file_browser.hpp>

namespace opent1::gui {
// Generic ToolPlugin fields/confirmation, including the two-stage browseBox.
// Console publication occurs only after outer dialog lifecycle preparation.
class ToolDialogs {
public:
    ToolDialogs();
    ~ToolDialogs();
    ToolDialogs(const ToolDialogs&) = delete;
    ToolDialogs& operator=(const ToolDialogs&) = delete;
    void font(std::shared_ptr<const gfx::BitmapFont>);
    [[nodiscard]] bool active() const;
    [[nodiscard]] Canvas& canvas();
    [[nodiscard]] FileBrowser& browser();
    [[nodiscard]] ModalView view();
    bool value(std::size_t, std::string);
    [[nodiscard]] std::string value(std::size_t) const;
    bool browse();
    bool accept();
    bool cancel();
    bool decision(bool yes);
    bool update();
    [[nodiscard]] world::ObjectHost host(std::function<bool(ModalView)>);

private:
    struct State;
    std::shared_ptr<State> state_;
    bool open(std::string_view, std::span<const std::string>, Console&);
    bool finish(std::string);
};
}
