#pragma once
#include <opent1/gui/gui.hpp>

namespace opent1::gui {
// An overlay edits its current parent's live controls. Lifetime tokens prevent
// undo records from following a recycled address or a detached control.
class EditControl : public Control {
public:
    ~EditControl() override;
    void onRemove() override;
    enum class Align {
        Left,
        Right,
        Top,
        Bottom,
        HorizontalCenter,
        VerticalCenter,
        HorizontalSpacing,
        VerticalSpacing
    };
    Point2I grid{1, 1};
    bool snap{true};
    bool select(Control&, bool add = false);
    void clear();
    bool addSet(Control&);
    bool selectAll();
    bool selectBox(RectI, bool add = false);
    bool bringToFront();
    bool sendToBack();
    bool erase();
    [[nodiscard]] static UInt8 sizingKnobs(Point2I, RectI);
    bool loadSelection(StreamIO&,
                       const Persistent::ClassRegistry&,
                       const std::function<bool(Control&)>& bind = {});
    bool addNewControl(std::unique_ptr<Control>);

    [[nodiscard]] std::vector<std::unique_ptr<Persistent::Base>> takeObjects() {
        return std::move(pasted_);
    }

    [[nodiscard]] std::size_t ownedObjectCount() const {
        return pasted_.size();
    }

    std::function<bool(bool cut)> copySelection;
    std::function<bool()> pasteSelection;
    std::function<bool(Control*)> inspect;
    // A graph host can retire its own decoded objects as one prepared batch.
    std::function<bool(std::span<Control*>)> destroySelection;
    [[nodiscard]] std::vector<Control*> selection() const;
    bool move(Point2I, bool useGrid = true);
    bool size(Point2I);
    bool align(Align);
    bool undo();
    bool redo();
    [[nodiscard]] Control* currentAddSet() const;
    bool properties(Control&, Fields);
    bool event(const Event&) override;
    bool draw(gfx::SoftwareSurface&, Point2I, RectI) const override;
    void close() override;

    bool wantsTab() const override {
        return true;
    }

    bool becomeFirstResponder() const override {
        return visible();
    }

    std::function<void()> checkpoint() override;
    bool saveSelection(StreamIO&, const Persistent::ClassRegistry&) const;

private:
    struct Item {
        Control* control{};
        std::weak_ptr<const void> lifetime;
        Fields fields;
        std::vector<Item> descendants;
    };

    using Record = std::vector<Item>;
    bool live(const Item&) const;
    Item snapshot(Control&) const;
    Record capture() const;
    bool apply(const Record&, bool exact = false);
    bool remember(Record);
    Record selected_;
    std::vector<Record> undo_, redo_;
    Record dragBefore_;
    Control* addSet_{};
    std::weak_ptr<const void> addSetLifetime_;
    std::vector<std::unique_ptr<Persistent::Base>> pasted_;
    bool dragging_{}, boxSelecting_{}, selectingOnly_{};
    UInt8 sizing_{};
    Point2I mouse_{}, last_{};
};
} // namespace opent1::gui
