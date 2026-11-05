#pragma once
#include <opent1/gui/controls.hpp>

namespace opent1::gui {
class ScrollContent : public Control {};

class Scroll : public Control {
public:
    enum Policy { AlwaysOn, AlwaysOff, Dynamic };
    enum class Region {
        UpArrow,
        DownArrow,
        LeftArrow,
        RightArrow,
        UpPage,
        DownPage,
        LeftPage,
        RightPage,
        VerticalThumb,
        HorizontalThumb,
        None
    };

    struct Geometry {
        bool horizontal{}, vertical{}, horizontalEnabled{}, verticalEnabled{};
        Point2I childPosition{}, childExtent{}, viewport{};
        RectI up{}, down{}, left{}, right{}, verticalTrack{}, horizontalTrack{};
        Int32 horizontalThumb{}, verticalThumb{}, horizontalSize{}, verticalSize{};
        float x{}, y{}, width{}, height{};
    };

    Scroll();
    Int32 arrayTag{}, horizontalPolicy{AlwaysOn}, verticalPolicy{AlwaysOn};
    Int32 borderThickness{1}, barThickness{16}, arrowLength{16}, baseThumbSize{16};
    bool disabled{}, willFirstRespond{true}, constantThumb{};
    std::shared_ptr<const gfx::BitmapArray> frames;
    bool addObject(sim::SimObject&) override;
    bool removeObject(sim::SimObject&) override;
    [[nodiscard]] ScrollContent* content() const;
    bool computeSizes();
    bool scrollTo(float, float);
    bool scrollBy(Region);
    [[nodiscard]] Region hitRegion(Point2I) const;

    [[nodiscard]] Region pressedRegion() const {
        return region_;
    }

    [[nodiscard]] bool depressed() const {
        return depressed_;
    }

    [[nodiscard]] const Geometry& geometry() const {
        return geometry_;
    }

    void wake() override;
    void tick(double) override;

    bool wantsTab() const override {
        return true;
    }

    bool becomeFirstResponder() const override {
        return willFirstRespond && visible() && !disabled;
    }

    bool event(const Event&) override;
    bool draw(gfx::SoftwareSurface&, Point2I, RectI) const override;
    std::function<void()> checkpoint() override;
    Persistent::Error read(StreamIO&, Int32, Int32, Persistent::LoadContext&) override;
    Persistent::Error write(StreamIO&, Int32, Int32, Persistent::StoreContext&) const override;

protected:
    virtual Point2I headerSize() const {
        return {};
    }

    virtual void drawBorder(gfx::SoftwareSurface&, RectI) const;

private:
    bool ensureContent();
    bool thumbs(Point2I, Point2I, Geometry&) const;
    bool moveChildren(Point2I);
    std::unique_ptr<ScrollContent> ownedContent_;
    Geometry geometry_;
    Region region_{Region::None};
    bool depressed_{};
    Point2I mouse_{};
    float anchor_{};
    Int32 thumbDelta_{};
};

class Matrix : public Scroll {
public:
    Matrix() {
        borderThickness = 0;
    }

    Point2I header;
    Persistent::Error read(StreamIO&, Int32, Int32, Persistent::LoadContext&) override;
    Persistent::Error write(StreamIO&, Int32, Int32, Persistent::StoreContext&) const override;

protected:
    Point2I headerSize() const override {
        return header;
    }
};

// The popup graph is owned by its combo and observed by Canvas only while open.
// It is runtime state: native ComboBox persists SimpleText and no popup fields.
class ComboBox : public Text {
public:
    ComboBox();
    ~ComboBox() override;
    TextList& list();
    Scroll& popupScroll();
    bool open();
    bool dismiss(bool accept);
    [[nodiscard]] bool isOpen() const;
    void action() override;
    void close() override;
    void onRemove() override;
    bool draw(gfx::SoftwareSurface&, Point2I, RectI) const override;

protected:
    virtual void selectionCommitted() {}

private:
    class Popup;
    std::unique_ptr<Popup> popup_;
};

bool registerContainerClasses(Persistent::ClassRegistry&);
} // namespace opent1::gui
