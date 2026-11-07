#pragma once
#include <algorithm>
#include <opent1/gui/controls.hpp>

namespace opent1::gui {
class TextFormat : public Active {
public:
    struct Font {
        Int32 tag{};
        std::shared_ptr<const gfx::BitmapFont> resource;
    };

    struct Run {
        UInt8 font{}, justification{};
        Point2I offset;
        Int32 lineWidth{};
        UInt32 start{}, length{};
        bool operator==(const Run&) const = default;
    };

    struct Image {
        std::shared_ptr<const gfx::Bitmap> bitmap;
        Point2I offset;
        UInt8 justification{};
        Int32 width{};
    };

    std::array<Font, 10> fonts;
    std::function<std::optional<std::string>(std::string_view)> variable;
    std::function<std::shared_ptr<const gfx::Bitmap>(std::string_view)> bitmap;
    // Native tags: F font, J justification, S spacing, L/R margins, N clear
    // bitmap margins, V variable and B bitmap (optional horizontal,vertical pad).
    // Unknown tags are consumed. The original has no hyperlink/span routing;
    // ordinary inherited Active actions apply to the control as a whole.
    // V tags expand names up to 124 bytes; longer native names skip lookup.
    // Layout and all resources are staged before replacing the previous result.
    bool format(std::string_view, Int32 width);
    bool variableChanged(std::string_view value) override {
        return format(value, std::max(20, fields_.extent.x)) &&
               resize(fields_.position, {fields_.extent.x, height() + 4});
    }

    [[nodiscard]] const std::string& text() const {
        return text_;
    }

    [[nodiscard]] std::span<const Run> runs() const {
        return runs_;
    }

    [[nodiscard]] std::span<const Image> images() const {
        return images_;
    }

    [[nodiscard]] Int32 width() const {
        return width_;
    }

    [[nodiscard]] Int32 minWidth() const {
        return minWidth_;
    }

    [[nodiscard]] Int32 height() const {
        return height_;
    }

    bool draw(gfx::SoftwareSurface&, Point2I, RectI) const override;
    std::function<void()> checkpoint() override;

private:
    std::string text_;
    std::vector<Run> runs_;
    std::vector<Image> images_;
    Int32 width_{}, minWidth_{}, height_{};
};
} // namespace opent1::gui
