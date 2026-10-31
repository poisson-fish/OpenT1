#pragma once
#include <opent1/gui/gui.hpp>

namespace opent1::gui {
class Array : public Active {
public:
    Array() {
        active = true;
    }

    Point2I cells{5, 30}, cellSize{80, 30}, header{}, selected{-1, -1}, hoverCell{-1, -1};
    bool setSize(Point2I);
    virtual bool select(Point2I);

    bool becomeFirstResponder() const override {
        return active && visible() && cells.x > 0 && cells.y > 0;
    }

    bool event(const Event&) override;
    bool draw(gfx::SoftwareSurface&, Point2I, RectI) const override;
    void wake() override;
    std::function<void()> checkpoint() override;

protected:
    virtual bool drawCell(gfx::SoftwareSurface&, Point2I, Point2I, bool, bool) const;
};

struct ListEntry {
    Int32 tag{};
    std::string text;
    bool operator==(const ListEntry&) const = default;
};

class TextList : public Array {
public:
    TextList() {
        cells = {1, 0};
    }

    std::vector<ListEntry> entries;
    std::shared_ptr<const gfx::BitmapFont> font, highlightFont;
    Int32 fontTag{}, highlightFontTag{}, verticalDelta{};
    bool enumerate{}, noDuplicates{true};
    bool insert(ListEntry, bool front = false);
    bool erase(std::size_t);
    void clear();
    void sort();
    bool
    insertTag(Int32, const std::function<std::optional<std::string>(Int32)>&, bool front = false);
    bool variableChanged(std::string_view) override;
    std::function<void()> checkpoint() override;
    [[nodiscard]] const ListEntry* selection() const;
    void wake() override;
    Persistent::Error read(StreamIO&, Int32, Int32, Persistent::LoadContext&) override;
    Persistent::Error write(StreamIO&, Int32, Int32, Persistent::StoreContext&) const override;

protected:
    bool drawCell(gfx::SoftwareSurface&, Point2I, Point2I, bool, bool) const override;
};

class Timer : public Control {
public:
    UInt32 message{};
    float initialAlarm{}, repeatAlarm{};
    void wake() override;

    void close() override {
        alarm_.reset();
    }

    void tick(double) override;
    std::function<void()> checkpoint() override;
    Persistent::Error read(StreamIO&, Int32, Int32, Persistent::LoadContext&) override;
    Persistent::Error write(StreamIO&, Int32, Int32, Persistent::StoreContext&) const override;

private:
    std::optional<double> alarm_;
};

class Animate : public Active {
public:
    Int32 arrayTag{}, sleep{};
    float timePerFrame{.3f};
    bool looping{true}, animating{true};
    UInt32 frame{};
    std::shared_ptr<const gfx::BitmapArray> frames;
    void start();

    void doNotLoop() {
        looping = false;
        animating = false;
    }

    bool onAdd() override;
    void wake() override;
    void tick(double) override;
    bool draw(gfx::SoftwareSurface&, Point2I, RectI) const override;
    std::function<void()> checkpoint() override;
    Persistent::Error read(StreamIO&, Int32, Int32, Persistent::LoadContext&) override;
    Persistent::Error write(StreamIO&, Int32, Int32, Persistent::StoreContext&) const override;

private:
    double last_{};
    bool clockInitialized_{};
};

class PaletteControl : public Control {
public:
    Int32 paletteTag{};
    std::shared_ptr<const gfx::Palette> palette;
    std::function<bool(std::shared_ptr<const gfx::Palette>)> activate;
    void wake() override;
    std::function<void()> checkpoint() override;
    Persistent::Error read(StreamIO&, Int32, Int32, Persistent::LoadContext&) override;
    Persistent::Error write(StreamIO&, Int32, Int32, Persistent::StoreContext&) const override;
};

// Native HelpCtrl only retains a tag or copied text; its render is a no-op.
class Help : public Control {
public:
    Int32 helpTag{};
    std::optional<std::string> helpText;
    void setText(std::optional<std::string>);
    void setTag(Int32);
};

class BitmapBox : public Control {
public:
    BitmapBox() {
        fields_.extent = {100, 100};
    }

    Int32 arrayTag{};
    std::shared_ptr<const gfx::BitmapArray> frames;
    std::function<void()> checkpoint() override;
    bool draw(gfx::SoftwareSurface&, Point2I, RectI) const override;
    Persistent::Error read(StreamIO&, Int32, Int32, Persistent::LoadContext&) override;
    Persistent::Error write(StreamIO&, Int32, Int32, Persistent::StoreContext&) const override;
};

// Native chunk division deliberately drops source remainder pixels. Destination
// remainder is assigned to the final row/column. Each chunk owns its pixels.
class ChunkedBitmap {
public:
    bool set(const gfx::Bitmap&, bool transparent);

    void clear() noexcept {
        count_ = {};
        chunks_.clear();
    }

    void setTransparent(bool) noexcept;
    bool draw(gfx::SoftwareSurface&, RectI) const;

    [[nodiscard]] std::span<const gfx::Bitmap> chunks() const {
        return chunks_;
    }

private:
    Point2I count_;
    std::vector<gfx::Bitmap> chunks_;
};

class TestControl : public Control {
public:
    bool event(const Event&) override;
    bool draw(gfx::SoftwareSurface&, Point2I, RectI) const override;
    std::function<void()> checkpoint() override;

private:
    bool highlight_{};
};

struct TextLine {
    UInt32 start{}, length{};
    bool operator==(const TextLine&) const = default;
};

// Native byte-font wrapping, including whitespace, explicit newlines and path
// backslash runs. The UTF-8 TextEdit contract remains separate.
[[nodiscard]] std::optional<std::vector<TextLine>>
wrapText(const gfx::BitmapFont&, std::string_view, Int32 width);

class TextWrap : public Active {
public:
    TextWrap() {
        fields_.extent = {100, 25};
    }

    std::string text;
    std::shared_ptr<const gfx::BitmapFont> font, disabledFont;
    Int32 fontTag{}, disabledFontTag{}, textTag{}, alignment{}, lineSpacing{5}, verticalDelta{};
    Point2I inset;
    bool enabled{true}, ignoreClick{};
    bool setText(std::string_view);

    bool variableChanged(std::string_view value) override {
        return setText(value);
    }

    std::function<void()> checkpoint() override;
    bool pointInControl(Point2I) const override;
    bool draw(gfx::SoftwareSurface&, Point2I, RectI) const override;
    Persistent::Error read(StreamIO&, Int32, Int32, Persistent::LoadContext&) override;
    Persistent::Error write(StreamIO&, Int32, Int32, Persistent::StoreContext&) const override;
};

bool registerExtendedClasses(Persistent::ClassRegistry&);
} // namespace opent1::gui
