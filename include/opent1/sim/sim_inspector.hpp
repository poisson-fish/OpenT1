#pragma once
#include <opent1/sim/sim_world_game.hpp>
#include <opent1/gfx/software_rasterizer.hpp>

namespace opent1::sim {
enum class InspectorControl { Description, Divider, Text, Checkbox, Choice, Action };

struct InspectorChoice {
    std::string label;
    Int32 value{};
};

struct InspectorField {
    InspectorControl control{};
    std::string label, text;
    std::vector<InspectorChoice> choices;
    bool valid{true};
};
enum class InspectorKey {
    Next,
    Previous,
    Left,
    Right,
    Home,
    End,
    Backspace,
    Delete,
    Accept,
    Cancel,
    SelectAll
};

struct InspectorColors {
    UInt8 background{0}, panel{1}, border{128}, focus{254}, error{253};
};

// Owned native-inspector controls and indexed presentation. No simulation object,
// tag dictionary, font, surface or game pointers are retained. open() snapshots
// labels/choices; apply/actions use SimGame's incarnation/schema checks. Text is
// legacy byte text (80 bytes for strings); numeric parsing consumes whole fields.
class SimInspector {
public:
    SimInspector() = default;
    SimInspector(const SimInspector&) = delete;
    SimInspector& operator=(const SimInspector&) = delete;
    static constexpr std::size_t MaxFields = 4096, MaxChoices = 65536, MaxTextBytes = 1024 * 1024;
    bool open(SimInspection, const TagDictionary&);
    void close();

    [[nodiscard]] bool isOpen() const noexcept {
        return document_.has_value();
    }

    [[nodiscard]] std::span<const InspectorField> fields() const noexcept {
        return fields_;
    }

    [[nodiscard]] std::optional<std::size_t> focused() const noexcept {
        return focus_;
    }

    [[nodiscard]] bool editing() const noexcept;

    [[nodiscard]] bool dirty() const noexcept {
        return dirty_;
    }

    [[nodiscard]] bool choicesOpen() const noexcept {
        return choiceOpen_;
    }

    [[nodiscard]] std::string_view status() const noexcept {
        return status_;
    }

    [[nodiscard]] std::size_t firstRow() const noexcept {
        return first_;
    }

    // Fixed logical viewport; SDL can scale it to a resized/high-density window.
    bool resize(Int32 width, Int32 height) noexcept;

    [[nodiscard]] Point2I size() const noexcept {
        return size_;
    }

    [[nodiscard]] gfx::RectI fieldRect(std::size_t) const noexcept;
    [[nodiscard]] gfx::RectI applyRect() const noexcept;
    [[nodiscard]] gfx::RectI revertRect() const noexcept;
    [[nodiscard]] gfx::RectI choiceRect() const noexcept;
    bool focus(std::size_t);
    bool setText(std::size_t, std::string_view);
    bool select(std::size_t field, std::size_t choice);
    bool toggle(std::size_t);
    bool key(InspectorKey);
    bool insert(std::string_view);
    void scroll(Int32 rows) noexcept;
    bool click(Point2I, SimGame&);
    bool apply(SimGame&);
    bool activate(SimGame&, std::size_t);
    void revert(); // restore the last successfully applied local document
    // Uses the supplied font/colors at its native size, 1..14 pixel height.
    // Draws only inside the surface's current clip and restores it afterwards.
    bool draw(gfx::SoftwareSurface&, const gfx::BitmapFont&, InspectorColors = {}) const;

private:
    std::optional<SimInspection> document_, baseline_;
    std::vector<InspectorField> fields_;
    std::optional<std::size_t> focus_;
    std::size_t first_{}, cursor_{};
    Point2I size_{640, 480};
    bool dirty_{}, selectAll_{}, choiceOpen_{};
    std::size_t choiceCursor_{}, choiceFirst_{};
    std::string status_, choiceSearch_;
    UInt64 revision_{};
    static constexpr Int32 RowHeight = 40, HeaderHeight = 32, FooterHeight = 56;
    [[nodiscard]] std::size_t visibleRows() const noexcept;
    [[nodiscard]] std::string valueText(std::size_t) const;
    bool commit(std::size_t);
    void reveal() noexcept;
    void sync(std::size_t);
    void revealChoice() noexcept;
};
} // namespace opent1::sim
