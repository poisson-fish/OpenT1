#pragma once
#include <opent1/sim/sim_world_game.hpp>
#include <opent1/gfx/software_rasterizer.hpp>
#include <functional>
#include <array>

namespace opent1::gui {
using gfx::RectI;
class Canvas;
enum class Key {
    None,
    Return,
    Tab,
    Left,
    Right,
    Up,
    Down,
    Home,
    End,
    Delete,
    Backspace,
    A,
    C,
    V,
    X,
    Z,
    Escape,
    PageUp,
    PageDown,
    U,
    F1,
    F2,
    F3,
    F4,
    F5,
    F6,
    F7,
    F9,
    F10,
    Insert
};
enum class EventType {
    Move,
    Down,
    Up,
    Drag,
    Enter,
    Leave,
    KeyDown,
    KeyUp,
    Text,
    Wheel,
    Cancel,
    Repeat
};

struct Event {
    EventType type{};
    Point2I point{};
    Key key{};
    bool shift{}, control{}, repeat{};
    UInt8 button{1};
    std::string text;
    float wheel{};
    bool alt{};
    UInt8 clicks{1};
};

struct Style {
    bool operator==(const Style&) const = default;
    UInt8 opaque{}, fill{}, selectedFill{}, ghostFill{}, border{}, borderColor{}, selectedBorder{},
        ghostBorder{};
};

struct Fields {
    bool operator==(const Fields&) const = default;
    Point2I position{}, extent{50, 50};
    Style style;
    UInt32 flags{3}, tag{};
    Int32 horizontal{}, vertical{}, helpTag{};
    std::string command, alternateCommand, variable;
};

// Managers own controls. Groups provide exclusive parentage; canvases observe
// their roots and never retain raw pointers after removal or detachment.
class Control : public sim::SimGroup {
public:
    static constexpr Int32 MaxCoordinate = 1 << 20;
    Control();
    ~Control() override;

    [[nodiscard]] const Fields& fields() const {
        return fields_;
    }

    bool configure(Fields); // validate before mutation; layout is transactional
    bool resize(Point2I position, Point2I extent);
    bool parentResized(Point2I oldExtent, Point2I newExtent);
    // Compute child bounds without mutating controls during resize preflight.
    virtual void
    layoutInParent(Point2I oldExtent, Point2I newExtent, Point2I& position, Point2I& extent) const;

    [[nodiscard]] Control* parent() const {
        return dynamic_cast<Control*>(getGroup());
    }

    [[nodiscard]] Canvas* canvas() const {
        return canvas_;
    }

    [[nodiscard]] std::weak_ptr<const void> lifetime() const {
        return lifetime_;
    }

    bool addObject(sim::SimObject&) override;
    bool removeObject(sim::SimObject&) override;
    bool onAdd() override;
    bool processArguments(std::span<const std::string>) override;
    void onNameChange(std::string_view) override;
    void onRemove() override;
    // Runtime graphs borrow script controls owned by the client manager.
    bool acceptsObjectManager(const sim::SimObject& object) const override {
        return !getManager() || object.getManager() == getManager();
    }
    [[nodiscard]] Point2I localToGlobal(Point2I) const;
    [[nodiscard]] Point2I globalToLocal(Point2I) const;
    [[nodiscard]] virtual Control* hit(Point2I local);
    [[nodiscard]] virtual bool pointInControl(Point2I local) const;

    [[nodiscard]] virtual Point2I minimumExtent() const {
        return {4, 4};
    }

    [[nodiscard]] Control* findTag(UInt32);
    [[nodiscard]] Control* findNamed(std::string_view);
    [[nodiscard]] Control* findRootTag(UInt32);
    void tag(UInt32);
    void visible(bool);

    [[nodiscard]] bool visible() const {
        return (fields_.flags & 1) != 0;
    }

    virtual bool wantsTab() const {
        return false;
    }

    virtual bool becomeFirstResponder() const {
        return false;
    }

    virtual bool loseFirstResponder() const {
        return true;
    }

    virtual bool event(const Event&);
    virtual bool message(Control& sender, UInt32);
    bool messageSiblings(UInt32);

    virtual bool variableChanged(std::string_view) {
        return true;
    }

    virtual std::string getScriptValue() const {
        return {};
    }

    virtual bool draw(gfx::SoftwareSurface&, Point2I, RectI) const;

    virtual void wake() {}

    virtual void close() {}

    virtual void tick(double) {}

    virtual Int32 helpTag(double) const {
        return fields_.helpTag;
    }

    virtual std::optional<std::string> helpText(double) const {
        return {};
    }

    Int32 mouseCursorTag{};

    virtual Int32 cursorTag() const {
        return mouseCursorTag;
    }

    // A dispatch transaction retains a restore operation for every control.
    // Derived controls include their interaction state, including text/undo.
    virtual std::function<void()> checkpoint();
    Persistent::Error read(StreamIO&, Int32, Int32, Persistent::LoadContext&) override;
    Persistent::Error write(StreamIO&, Int32, Int32, Persistent::StoreContext&) const override;
    // Native Control-owned prefix (versions 0..4); parent graph uses SimGroup.
    static bool readFields(StreamIO&, Fields&);
    static bool writeFields(StreamIO&, const Fields&);

protected:
    friend class Canvas;
    bool drawChildren(gfx::SoftwareSurface&, Point2I, RectI) const;
    void setCanvas(Canvas*);
    Fields fields_;

private:
    Canvas* canvas_{};
    std::shared_ptr<const void> lifetime_{std::make_shared<int>(0)};
};

class Active : public Control {
public:
    bool active{}, depressed{}, over{};
    UInt32 message{};
    Int32 selectedCursorTag{};

    bool wantsTab() const override {
        return true;
    }

    bool becomeFirstResponder() const override {
        return active && visible();
    }

    bool loseFirstResponder() const override {
        return !depressed;
    }

    bool event(const Event&) override;
    virtual void action();
    std::function<void()> checkpoint() override;
    Persistent::Error read(StreamIO&, Int32, Int32, Persistent::LoadContext&) override;
    Persistent::Error write(StreamIO&, Int32, Int32, Persistent::StoreContext&) const override;
};

class Progress : public Active {
public:
    Int32 low{}, high{100}, increment{1}, value{50};
    bool setRange(Int32, Int32);
    bool percent(float);
    Int32 offset(Int32);
    Int32 step();
    std::function<void()> checkpoint() override;
    bool draw(gfx::SoftwareSurface&, Point2I, RectI) const override;
    Persistent::Error read(StreamIO&, Int32, Int32, Persistent::LoadContext&) override;
    Persistent::Error write(StreamIO&, Int32, Int32, Persistent::StoreContext&) const override;
};

class Slider : public Active {
public:
    Slider() {
        active = true;
    }

    float low{}, high{1}, value{}, line{.05f}, page{.1f};
    Int32 thumbWidth{6};
    bool setValue(float);
    Int32 thumbPosition() const;
    bool event(const Event&) override;
    bool draw(gfx::SoftwareSurface&, Point2I, RectI) const override;
    std::function<void()> checkpoint() override;
    Persistent::Error read(StreamIO&, Int32, Int32, Persistent::LoadContext&) override;
    Persistent::Error write(StreamIO&, Int32, Int32, Persistent::StoreContext&) const override;

private:
    friend class Canvas;
    bool dragging_{}, pageLeft_{};
};

class Text : public Active {
public:
    Text() {
        fields_.extent = {100, 25};
    }

    std::string text;
    std::shared_ptr<const gfx::BitmapFont> font, highlightFont, disabledFont;
    Int32 alignment{}, verticalDelta{};
    UInt32 fontTag{}, highlightFontTag{}, disabledFontTag{}, textTag{};
    bool variableChanged(std::string_view) override;
    std::function<void()> checkpoint() override;
    bool draw(gfx::SoftwareSurface&, Point2I, RectI) const override;
    Persistent::Error read(StreamIO&, Int32, Int32, Persistent::LoadContext&) override;
    Persistent::Error write(StreamIO&, Int32, Int32, Persistent::StoreContext&) const override;
};

class Button : public Text {
public:
    Button() {
        active = true;
        fields_.extent = {140, 30};
        alignment = 2;
    }

    bool draw(gfx::SoftwareSurface&, Point2I, RectI) const override;
};

class Check : public Button {
public:
    Check() {
        fields_.extent = {100, 25};
        alignment = 0;
    }

    bool checked{}, radio{};
    void action() override;
    bool variableChanged(std::string_view) override;
    std::function<void()> checkpoint() override;
    bool draw(gfx::SoftwareSurface&, Point2I, RectI) const override;
    Persistent::Error read(StreamIO&, Int32, Int32, Persistent::LoadContext&) override;
    Persistent::Error write(StreamIO&, Int32, Int32, Persistent::StoreContext&) const override;
};

class Radio : public Check {
public:
    Radio() {
        radio = true;
    }
};

// UTF-8 editing; cursor/anchor are byte boundaries. SDL text input supplies
// composed text. Clipboard IO is performed before edits are committed.
class TextEdit : public Text {
public:
    TextEdit() {
        active = true;
    }

    mutable std::size_t cursor{}, anchor{};
    std::size_t maxBytes{255};
    bool numbersOnly{};
    bool multiline{}; // runtime editor mode; native records stay single-line
    bool setText(std::string);
    bool variableChanged(std::string_view) override;

    [[nodiscard]] std::pair<std::size_t, std::size_t> selection() const {
        return mouseBlock_.value_or(std::pair{std::min(cursor, anchor), std::max(cursor, anchor)});
    }

    UInt8 highlightColor{}, cursorColor{};
    bool cursorOn{true}, insertOn{true};
    void tick(double) override;
    bool event(const Event&) override;
    bool draw(gfx::SoftwareSurface&, Point2I, RectI) const override;
    std::function<void()> checkpoint() override;
    Persistent::Error read(StreamIO&, Int32, Int32, Persistent::LoadContext&) override;
    Persistent::Error write(StreamIO&, Int32, Int32, Persistent::StoreContext&) const override;

protected:
    virtual void drawBackground(gfx::SoftwareSurface&, Point2I) const;

private:
    friend class Canvas;
    void resetCursorBlink() noexcept;
    std::string undo_;
    std::size_t undoCursor_{}, undoAnchor_{}, mouseDragAnchor_{};
    mutable std::optional<std::pair<std::size_t, std::size_t>> mouseBlock_;
    bool selecting_{};
    Int32 mouseScroll_{};
    mutable Int32 scroll_{}, lineScroll_{};
    double cursorTime_{};
    UInt32 cursorFrames_{};
};

class Bitmap : public Active {
public:
    std::shared_ptr<const gfx::Bitmap> bitmap;
    bool stretch{true}, transparent{};
    Int32 bitmapTag{};
    std::function<void()> checkpoint() override;
    bool draw(gfx::SoftwareSurface&, Point2I, RectI) const override;
    Persistent::Error read(StreamIO&, Int32, Int32, Persistent::LoadContext&) override;
    Persistent::Error write(StreamIO&, Int32, Int32, Persistent::StoreContext&) const override;
};

// Output is an owned framebuffer. Draw failure never publishes partial output.
class Canvas : public sim::SimCanvas {
public:
    Canvas() = default;
    ~Canvas();
    Canvas(const Canvas&) = delete;
    Canvas& operator=(const Canvas&) = delete;

    // The original named class inherits SimObject's unsupported read/write.
    // Persist content Control graphs; a native Canvas is a runtime host.
    Persistent::Error read(StreamIO&, Int32, Int32, Persistent::LoadContext&) override {
        return Persistent::Error::ReadError;
    }

    Persistent::Error write(StreamIO&, Int32, Int32, Persistent::StoreContext&) const override {
        return Persistent::Error::WriteError;
    }

    bool content(Control*);
    bool pushDialog(Control&);
    bool popDialog();
    // Run the final, prepared publication only after close/capture preparation
    // succeeds. A refusal/exception restores controls and interaction; the
    // callback must itself publish atomically and must not mutate the graph.
    bool commitDialog(const std::function<bool()>&);
    [[nodiscard]] Control* top() const;

    [[nodiscard]] Control* content() const {
        return roots_.empty() ? nullptr : roots_.front();
    }

    [[nodiscard]] Control* focus() const {
        return focus_;
    }

    [[nodiscard]] Control* capture() const {
        return capture_;
    }

    [[nodiscard]] Control* hover() const {
        return hover_;
    }

    [[nodiscard]] bool mouseDown(UInt8 button) const {
        return button == 1 ? leftDown_ : button == 3 ? rightDown_ : false;
    }

    [[nodiscard]] bool hoverClicked() const {
        return mouseClicked_;
    }

    // Explicit responders such as the in-game chat may receive keyboard input
    // beneath a score dialog. Mouse capture and tab navigation stay in the top graph.
    bool makeFirstResponder(Control*, bool allowCovered = false);
    bool lockMouse(Control*);
    void unlockMouse();
    void rebuildTabs();
    void tab(bool previous = false);
    bool dispatch(const Event&);
    void cancel() noexcept;
    // Window accelerators run inside the same owned-state transaction as the
    // focused control, before its ordinary key handler.
    std::function<bool(const Event&)> keyCommand;
    bool dispatch(const Event&, double seconds);
    bool performClick(Active&);
    // Defer graph/dialog changes requested by a control until successful input
    // dispatch has released its state checkpoints. Rejected events discard them.
    void afterDispatch(std::function<void()>);
    bool advance(double seconds);

    [[nodiscard]] double time() const {
        return time_;
    }

    bool editMode{}, synthesizeKeyRepeat{};

    struct Cursor {
        std::shared_ptr<const gfx::Bitmap> bitmap;
        std::shared_ptr<const gfx::BitmapArray> frames;
        Point2I hotspot;
        double frameTime{.5};
    };

    Cursor cursor;
    bool cursorVisible{};
    std::function<std::optional<Cursor>(Int32)> controlCursor;
    bool cursorEnabled{true};
    bool setCursor(Cursor);

    [[nodiscard]] Point2I cursorPosition() const {
        return cursorPoint_;
    }

    bool setCursorPosition(Point2I point) {
        if (std::abs(Int64(point.x)) > Control::MaxCoordinate ||
            std::abs(Int64(point.y)) > Control::MaxCoordinate) {
            return false;
        }
        cursorPoint_ = point;
        return true;
    }

    struct HelpState {
        Int32 tag{};
        std::optional<std::string> text;
        double elapsed{};
        bool clicked{};
    };

    [[nodiscard]] const HelpState& helpState() const {
        return help_;
    }

    // Called in native tag/text order during frame preparation. Callback side
    // effects cannot be undone; Canvas publishes its own state with the frame.
    std::function<void(const HelpState&)> onHelp;
    bool addUpdateRegion(Point2I, Point2I) const;
    void resetUpdateRegions() const;

    [[nodiscard]] std::optional<RectI> updateRegion() const {
        return displayedUpdate_;
    }

    [[nodiscard]] std::size_t dialogCount() const {
        return roots_.empty() ? 0 : roots_.size() - 1;
    }

    bool fitContent(Point2I extent, bool onlyIfChanged = false) const;
    [[nodiscard]] std::shared_ptr<const gfx::Bitmap>
    render(Int32 width,
           Int32 height,
           const gfx::Palette* = nullptr,
           const gfx::Bitmap* background = nullptr,
           gfx::SoftwareSurface::BlendRenderer blendRenderer = {},
           bool fitRoots = false) const;
    // Borrowed until the next retained render. Palette storage is immutable and
    // shared; exported render() bitmaps retain independent owned snapshots.
    // Direct changes to public control drawing state require resetUpdateRegions.
    // Timed updates, backgrounds and ordered blend callbacks redraw in full.
    [[nodiscard]] const gfx::Bitmap*
    renderRetained(Int32 width,
                   Int32 height,
                   std::shared_ptr<const gfx::Palette>,
                   const gfx::Bitmap* background = nullptr,
                   gfx::SoftwareSurface::BlendRenderer blendRenderer = {},
                   bool fitRoots = false) const;
    // Host callbacks must not mutate/destroy the tree during dispatch/render.
    // A false action leaves local interaction state rolled back.
    std::function<bool(Control&, UInt32, std::string_view)> onAction;
    std::function<bool(Control&, UInt32)> onMessage;
    std::function<bool(const Event&)> onUnhandled;
    std::function<std::optional<std::string>()> readClipboard;
    std::function<bool(std::string_view)> writeClipboard;
    std::function<bool(bool)> captureMouse;

    bool setTitle(std::string_view value) override {
        if (value.size() > 4096) {
            return false;
        }
        title = value;
        return true;
    }

    void renderFrame() override {
        if (onFrame) {
            onFrame();
        }
    }

    void onRemove() override {
        content(nullptr);
        sim::SimCanvas::onRemove();
    }

    std::string title;
    std::function<void()> onFrame;

    [[nodiscard]] bool changingContent() const {
        return changingContent_;
    }

    [[nodiscard]] bool dispatching() const {
        return dispatching_;
    }

    // True for visible controls in the active content/dialog graph.
    [[nodiscard]] bool contains(const Control*, bool includeCovered = false) const;

private:
    friend class Control;
    friend class Active;
    friend class Runtime;
    bool renderInto(gfx::SoftwareSurface&,
                    const gfx::Bitmap*,
                    gfx::SoftwareSurface::BlendRenderer,
                    bool fitRoots,
                    bool incremental,
                    bool forceFull = false) const;
    mutable bool layoutDirty_{true}, retainedDirty_{true};
    mutable Point2I fittedExtent_{};
    mutable std::vector<Control*> fittedRoots_;
    mutable std::unique_ptr<gfx::SoftwareSurface> retainedFront_, retainedBack_;
    mutable double retainedTime_{};
    void invalidate(Control&);
    std::function<void()> interactionCheckpoint();
    std::vector<Control*> roots_, tabs_;
    Control *focus_{}, *capture_{}, *hover_{};
    bool dispatching_{};
    bool changingContent_{};
    double time_{};
    std::vector<std::function<void()>> afterDispatch_;
    Event lastMouse_, lastKey_;
    std::optional<double> nextMouse_, nextKey_;
    Point2I cursorPoint_;
    double hoverTime_{};
    bool mouseClicked_{}, leftDown_{}, rightDown_{};
    mutable HelpState help_;
    mutable Point2I updateExtent_;
    mutable std::optional<RectI> currentUpdate_, displayedUpdate_, lastCursorRegion_;
    mutable std::array<std::optional<RectI>, 2> oldUpdates_;
    mutable Cursor renderedCursor_;
    mutable std::size_t cursorFrame_{};
    mutable double cursorFrameTime_{};
    mutable Int32 controlCursorTag_{};
    mutable Cursor controlCursor_;
    mutable std::size_t controlCursorFrame_{};
};

bool registerClasses(Persistent::ClassRegistry&);
} // namespace opent1::gui
