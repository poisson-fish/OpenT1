#pragma once

#include <opent1/game/hud_controls.hpp>
#include <opent1/gui/text_format.hpp>
#include <opent1/video/smacker.hpp>
#include <opent1/world/ts_shape_instance.hpp>

#include <array>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace opent1::game {

// -----------------------------------------------------------------------------
// FGControl (fearGuiControl.cpp) & FearGuiBox (FearGuiBox.cpp)
// -----------------------------------------------------------------------------
class FGControl : public gui::Control {
    using Parent = gui::Control;

public:
    FGControl() = default;
    ~FGControl() override = default;

    bool draw(gfx::SoftwareSurface& s, Point2I offset, RectI clip) const override;
};

class FearGuiBox : public gui::Control {
    using Parent = gui::Control;

public:
    FearGuiBox() = default;
    ~FearGuiBox() override = default;

    [[nodiscard]] bool isGhosted() const noexcept {
        return ghosted_;
    }

    void setGhosted(bool g) noexcept {
        ghosted_ = g;
    }

    bool onAdd() override;
    bool draw(gfx::SoftwareSurface& s, Point2I offset, RectI clip) const override;

private:
    bool ghosted_{false};
};

// -----------------------------------------------------------------------------
// FGShellBorder & MMShellBorder (FearGuiShellBorder.cpp)
// -----------------------------------------------------------------------------
class FGShellBorder : public gui::Control {
    using Parent = gui::Control;

public:
    FGShellBorder() = default;
    ~FGShellBorder() override = default;

    [[nodiscard]] const std::string& bitmapName() const noexcept {
        return bitmapName_;
    }

    void setBitmapName(std::string_view name) {
        bitmapName_ = std::string(name);
    }

    void setBackground(std::shared_ptr<const gfx::Bitmap> bitmap) {
        background_ = std::move(bitmap);
    }

    void setBorder(std::shared_ptr<const gfx::BitmapArray> frames) {
        border_ = std::move(frames);
    }

    bool draw(gfx::SoftwareSurface&, Point2I, RectI) const override;
    std::function<void()> checkpoint() override;

    bool inspectRead(sim::Inspect& inspector);
    bool inspectWrite(sim::Inspect& inspector) const;

    Persistent::Error
    read(StreamIO& sio, Int32 version, Int32 user, Persistent::LoadContext& ctx) override;
    Persistent::Error
    write(StreamIO& sio, Int32 version, Int32 user, Persistent::StoreContext& ctx) const override;

private:
    std::string bitmapName_{};
    std::shared_ptr<const gfx::Bitmap> background_;
    std::shared_ptr<const gfx::BitmapArray> border_;
};

class MMShellBorder : public FGShellBorder {
    using Parent = FGShellBorder;

public:
    MMShellBorder() = default;
    ~MMShellBorder() override = default;
};

// -----------------------------------------------------------------------------
// FGPaletteCtrl (FearGuiPaletteCtrl.cpp)
// -----------------------------------------------------------------------------
class FGPaletteCtrl : public gui::PaletteControl {
    using Parent = gui::PaletteControl;

public:
    FGPaletteCtrl() = default;
    ~FGPaletteCtrl() override = default;
};

// -----------------------------------------------------------------------------
// FGUniversalButton (FearGuiUnivButton.cpp)
// -----------------------------------------------------------------------------
class FGUniversalButton : public gui::Active {
    using Parent = gui::Active;

public:
    static constexpr Int32 ButtonVersion = 3;

    FGUniversalButton();
    ~FGUniversalButton() override = default;

    [[nodiscard]] Int32 radioTag() const noexcept {
        return radioTag_;
    }

    void setRadioTag(Int32 tag) noexcept {
        radioTag_ = tag;
    }

    [[nodiscard]] const std::string& bitmapRootName() const noexcept {
        return bitmapRootName_;
    }

    void setBitmapRootName(std::string_view name);

    void setBitmaps(std::array<std::shared_ptr<const gfx::Bitmap>, 8> bitmaps) {
        bitmaps_ = std::move(bitmaps);
    }

    void setRadioBitmaps(std::shared_ptr<const gfx::Bitmap> normal,
                         std::shared_ptr<const gfx::Bitmap> disabled) {
        radioBitmap_ = std::move(normal);
        radioDisabledBitmap_ = std::move(disabled);
    }

    bool draw(gfx::SoftwareSurface&, Point2I, RectI) const override;

    [[nodiscard]] Int32 bitmapTag() const noexcept {
        return bitmapTag_;
    }

    void setBitmapTag(Int32 tag) noexcept {
        bitmapTag_ = tag;
    }

    [[nodiscard]] Point2I loResPosition() const noexcept {
        return loResPosition_;
    }

    void setLoResPosition(Point2I p) noexcept {
        loResPosition_ = p;
    }

    [[nodiscard]] bool isSet() const noexcept {
        return set_;
    }

    void setState(bool val) noexcept {
        set_ = val;
    }

    bool variableChanged(std::string_view value) override;

    std::string getScriptValue() const override {
        return set_ ? "True" : "False";
    }

    [[nodiscard]] bool isSwitch() const noexcept {
        return isSwitch_;
    }

    void setSwitch(bool sw) noexcept {
        isSwitch_ = sw;
    }

    [[nodiscard]] bool mirrorConsoleVar() const noexcept {
        return mirrorConsoleVar_;
    }

    void setMirrorConsoleVar(bool mirror) noexcept {
        mirrorConsoleVar_ = mirror;
    }

    [[nodiscard]] bool isFlashing() const noexcept {
        return flashing_;
    }

    void setFlashing(bool flash) noexcept {
        flashing_ = flash;
    }

    std::function<void(std::string_view)> playSound;
    bool event(const gui::Event&) override;
    void action() override;
    bool message(gui::Control& sender, UInt32 msg) override;
    std::function<void()> checkpoint() override;

    bool inspectRead(sim::Inspect& inspector);
    bool inspectWrite(sim::Inspect& inspector) const;

    Persistent::Error
    read(StreamIO& sio, Int32 version, Int32 user, Persistent::LoadContext& ctx) override;
    Persistent::Error
    write(StreamIO& sio, Int32 version, Int32 user, Persistent::StoreContext& ctx) const override;

private:
    Int32 radioTag_{0};
    std::array<std::shared_ptr<const gfx::Bitmap>, 8> bitmaps_;
    std::shared_ptr<const gfx::Bitmap> radioBitmap_, radioDisabledBitmap_;
    std::string bitmapRootName_{};
    Int32 bitmapTag_{0};
    Point2I loResPosition_{0, 0};
    bool set_{false};
    bool isSwitch_{false};
    bool mirrorConsoleVar_{false};
    bool flashing_{false};
};

// -----------------------------------------------------------------------------
// CFGButton (FearGuiCFGButton.cpp)
// -----------------------------------------------------------------------------
class CFGButton : public gui::Text {
    using Parent = gui::Text;

public:
    static constexpr Int32 CtrlVersion = 0;

    struct BindingSpec {
        UInt8 deviceType{0};
        UInt8 deviceInst{0};
        UInt8 objType{0};
        UInt8 objInst{0};
        UInt16 modifier{0};
        bool bound{false};
    };

    CFGButton();
    ~CFGButton() override = default;

    void setBitmaps(std::array<std::shared_ptr<const gfx::Bitmap>, 3> value) {
        bitmaps_ = std::move(value);
    }

    bool draw(gfx::SoftwareSurface&, Point2I, RectI) const override;
    void action() override;
    bool event(const gui::Event&) override;

    [[nodiscard]] bool capturingBinding() const noexcept {
        return capturing_;
    }

    std::function<bool(CFGButton&, const BindingSpec&, std::string_view)> rebind;

    [[nodiscard]] const std::string& actionMapName() const noexcept {
        return actionMap_;
    }

    void setActionMapName(std::string_view name) {
        actionMap_ = std::string(name);
    }

    [[nodiscard]] Int32 makeAction() const noexcept {
        return makeAction_;
    }

    void setMakeAction(Int32 act) noexcept {
        makeAction_ = act;
    }

    [[nodiscard]] Flt32 makeValue() const noexcept {
        return makeValue_;
    }

    void setMakeValue(Flt32 val) noexcept {
        makeValue_ = val;
    }

    [[nodiscard]] Int32 breakAction() const noexcept {
        return breakAction_;
    }

    void setBreakAction(Int32 act) noexcept {
        breakAction_ = act;
    }

    [[nodiscard]] Flt32 breakValue() const noexcept {
        return breakValue_;
    }

    void setBreakValue(Flt32 val) noexcept {
        breakValue_ = val;
    }

    [[nodiscard]] Int32 actionFlags() const noexcept {
        return flags_;
    }

    void setActionFlags(Int32 flg) noexcept {
        flags_ = flg;
    }

    [[nodiscard]] static UInt16 normalizeModifiers(UInt16 rawMods) noexcept;
    [[nodiscard]] static std::string formatBindingLabel(const BindingSpec& spec,
                                                        std::string_view keyName);

    void setBinding(const BindingSpec& spec, std::string_view keyName);
    void clearBinding();

    [[nodiscard]] const BindingSpec& binding() const noexcept {
        return binding_;
    }

    [[nodiscard]] const std::string& bindingLabel() const noexcept {
        return bindingLabel_;
    }

    [[nodiscard]] bool bindingConflict(const BindingSpec& candidate) const noexcept;

    std::function<void()> checkpoint() override;
    bool inspectRead(sim::Inspect& inspector);
    bool inspectWrite(sim::Inspect& inspector) const;

    Persistent::Error
    read(StreamIO& sio, Int32 version, Int32 user, Persistent::LoadContext& ctx) override;
    Persistent::Error
    write(StreamIO& sio, Int32 version, Int32 user, Persistent::StoreContext& ctx) const override;

private:
    std::string actionMap_{"playMap.sae"};
    Int32 makeAction_{0};
    Flt32 makeValue_{1.0f};
    Int32 breakAction_{0};
    Flt32 breakValue_{0.0f};
    Int32 flags_{0};
    BindingSpec binding_{};
    std::string bindingLabel_{"<None>"};
    std::array<std::shared_ptr<const gfx::Bitmap>, 3> bitmaps_{};
    bool capturing_{};
};

// -----------------------------------------------------------------------------
// FearGuiSwitch (FearGuiSwitch.cpp)
// -----------------------------------------------------------------------------
class FearGuiSwitch : public gui::Active {
    using Parent = gui::Active;

public:
    FearGuiSwitch() = default;
    ~FearGuiSwitch() override = default;

    [[nodiscard]] Int32 bmpOnTag() const noexcept {
        return bmpOnTag_;
    }

    void setBmpOnTag(Int32 t) noexcept {
        bmpOnTag_ = t;
    }

    [[nodiscard]] Int32 bmpOffTag() const noexcept {
        return bmpOffTag_;
    }

    void setBmpOffTag(Int32 t) noexcept {
        bmpOffTag_ = t;
    }

    [[nodiscard]] Int32 targetTag() const noexcept {
        return targetTag_;
    }

    void setTargetTag(Int32 t) noexcept {
        targetTag_ = t;
    }

    [[nodiscard]] bool isSet() const noexcept {
        return set_;
    }

    void setState(bool s) noexcept {
        set_ = s;
    }

    void action() override;
    std::function<void()> checkpoint() override;

    bool inspectRead(sim::Inspect& inspector);
    bool inspectWrite(sim::Inspect& inspector) const;

    Persistent::Error
    read(StreamIO& sio, Int32 version, Int32 user, Persistent::LoadContext& ctx) override;
    Persistent::Error
    write(StreamIO& sio, Int32 version, Int32 user, Persistent::StoreContext& ctx) const override;

private:
    Int32 bmpOnTag_{0};
    Int32 bmpOffTag_{0};
    Int32 targetTag_{0};
    bool set_{false};
};

// -----------------------------------------------------------------------------
// FGTab & FGTabMenu (FearGuiTabMenu.cpp)
// -----------------------------------------------------------------------------
class FGTab : public gui::Text {
    using Parent = gui::Text;

public:
    FGTab() = default;
    ~FGTab() override = default;

    [[nodiscard]] Int32 pageTag() const noexcept {
        return pageTag_;
    }

    void setPageTag(Int32 tag) noexcept {
        pageTag_ = tag;
    }

    [[nodiscard]] bool isSet() const noexcept {
        return set_;
    }

    void setState(bool on);

    bool message(gui::Control& sender, UInt32 msg) override;
    std::function<void()> checkpoint() override;

    bool inspectRead(sim::Inspect& inspector);
    bool inspectWrite(sim::Inspect& inspector) const;

    Persistent::Error
    read(StreamIO& sio, Int32 version, Int32 user, Persistent::LoadContext& ctx) override;
    Persistent::Error
    write(StreamIO& sio, Int32 version, Int32 user, Persistent::StoreContext& ctx) const override;

private:
    Int32 pageTag_{0};
    bool set_{false};
};

class FGTabMenu : public gui::Active {
    using Parent = gui::Active;

public:
    FGTabMenu() = default;
    ~FGTabMenu() override = default;

    [[nodiscard]] Int32 rowCount() const noexcept {
        return rowCount_;
    }

    void setRowCount(Int32 count) noexcept {
        rowCount_ = count;
    }

    bool activateTabIndex(std::size_t index);
    std::function<void()> checkpoint() override;

    bool inspectRead(sim::Inspect& inspector);
    bool inspectWrite(sim::Inspect& inspector) const;

    Persistent::Error
    read(StreamIO& sio, Int32 version, Int32 user, Persistent::LoadContext& ctx) override;
    Persistent::Error
    write(StreamIO& sio, Int32 version, Int32 user, Persistent::StoreContext& ctx) const override;

private:
    Int32 rowCount_{1};
};

// -----------------------------------------------------------------------------
// FGSlider (fearGuiSlider.cpp)
// -----------------------------------------------------------------------------
class FGSlider : public gui::Slider {
    using Parent = gui::Slider;

public:
    static constexpr Int32 ControlVersion = 0;

    FGSlider() = default;
    ~FGSlider() override = default;

    void setBitmaps(std::shared_ptr<const gfx::BitmapArray> images) {
        bitmaps_ = std::move(images);
    }

    void wake() override;
    bool draw(gfx::SoftwareSurface&, Point2I, RectI) const override;
    bool event(const gui::Event&) override;

    std::string getScriptValue() const override {
        return scriptValue();
    }

    bool variableChanged(std::string_view val) override {
        return setScriptValue(val);
    }

    [[nodiscard]] Int32 numDiscreteValues() const noexcept {
        return numDiscreteValues_;
    }

    void setDiscretePositions(Int32 count) noexcept;

    [[nodiscard]] Int32 minThumbX() const noexcept {
        return minThumbX_;
    }

    [[nodiscard]] Int32 maxThumbX() const noexcept {
        return maxThumbX_;
    }

    [[nodiscard]] Int32 curThumbX() const noexcept {
        return curThumbX_;
    }

    void setThumbRange(Int32 minX, Int32 maxX) noexcept {
        minThumbX_ = minX;
        maxThumbX_ = maxX;
        curThumbX_ = std::clamp(curThumbX_, minThumbX_, maxThumbX_);
    }

    [[nodiscard]] std::string scriptValue() const;
    bool setScriptValue(std::string_view val);
    std::function<void()> checkpoint() override;

    Persistent::Error
    read(StreamIO& sio, Int32 version, Int32 user, Persistent::LoadContext& ctx) override;
    Persistent::Error
    write(StreamIO& sio, Int32 version, Int32 user, Persistent::StoreContext& ctx) const override;

private:
    Int32 minThumbX_{14};
    Int32 maxThumbX_{114};
    Int32 curThumbX_{14};
    Int32 numDiscreteValues_{0};
    std::shared_ptr<const gfx::BitmapArray> bitmaps_;
    bool dragging_{};
    Int32 dragOffset_{};
};

// -----------------------------------------------------------------------------
// FearGuiScrollCtrl (FearGuiScrollCtrl.cpp)
// -----------------------------------------------------------------------------
class FearGuiScrollCtrl : public gui::Matrix {
    using Parent = gui::Matrix;

public:
    static constexpr Int32 CtrlVersion = 0;

    FearGuiScrollCtrl();
    ~FearGuiScrollCtrl() override = default;
    void drawBorder(gfx::SoftwareSurface&, RectI) const override;

    std::function<void()> checkpoint() override;
    bool inspectRead(sim::Inspect& inspector);
    bool inspectWrite(sim::Inspect& inspector) const;

    Persistent::Error
    read(StreamIO& sio, Int32 version, Int32 user, Persistent::LoadContext& ctx) override;
    Persistent::Error
    write(StreamIO& sio, Int32 version, Int32 user, Persistent::StoreContext& ctx) const override;
};

// -----------------------------------------------------------------------------
// ColumnListCtrl (FearGuiColumns.cpp)
// -----------------------------------------------------------------------------
class ColumnListCtrl : public FGArrayCtrl {
    using Parent = FGArrayCtrl;

public:
    static constexpr std::size_t ServerColumnCount = 12;

    struct ServerColumnPref {
        Int32 id{0};
        std::string_view name{};
        Int32 position{0};
        bool active{true};
        Int32 width{100};
    };

    ColumnListCtrl();
    ~ColumnListCtrl() override = default;
    bool event(const gui::Event&) override;
    std::function<void()> checkpoint() override;
    std::function<void(const ColumnListCtrl&)> changed;

    [[nodiscard]] static ServerColumnPref parseColumnPrefString(Int32 id,
                                                                std::string_view name,
                                                                std::string_view spec,
                                                                Int32 defaultWidth = 100);
    [[nodiscard]] static std::string formatColumnPrefString(const ServerColumnPref& pref);

    void resetDefaultColumns();
    bool moveColumnPosition(Int32 fromPos, Int32 toPos);
    bool toggleColumnActive(Int32 pos);

    [[nodiscard]] std::span<const ServerColumnPref, ServerColumnCount>
    serverColumns() const noexcept {
        return serverColumns_;
    }

private:
    bool drawCell(gfx::SoftwareSurface&, Point2I, Point2I, bool, bool) const override;
    Int32 dragRow_{-1};
    std::array<ServerColumnPref, ServerColumnCount> serverColumns_{};
};

// -----------------------------------------------------------------------------
// FGTextList (fearGuiTextList.cpp), FGSimpleText (FearGuiSimpleText.cpp),
// FearGuiFormattedText (FearGuiTextFMT.cpp), FGTextFormat (fearGuiTextFormat.cpp),
// TestEdit (FearGuiTestEdit.cpp), FGBitmapCtrl (FearGuiBitmapCtrl.cpp)
// -----------------------------------------------------------------------------
class FGTextList : public gui::TextList {
    using Parent = gui::TextList;

public:
    FGTextList() = default;
    ~FGTextList() override = default;
    bool drawCell(gfx::SoftwareSurface&, Point2I, Point2I, bool, bool) const override;
};

class FGSimpleText : public gui::Text {
    using Parent = gui::Text;

public:
    FGSimpleText() = default;
    ~FGSimpleText() override = default;

    [[nodiscard]] bool isLowRes() const noexcept {
        return lowRes_;
    }

    void setLowRes(bool lr) noexcept {
        lowRes_ = lr;
    }

private:
    bool lowRes_{false};
};

class FearGuiFormattedText : public gui::TextFormat {
    using Parent = gui::TextFormat;

public:
    FearGuiFormattedText() = default;
    ~FearGuiFormattedText() override = default;

    bool setScriptValue(std::string_view rawText, Int32 formatWidth = 100);

    bool variableChanged(std::string_view value) override {
        return setScriptValue(value);
    }

    std::string getScriptValue() const override {
        return text();
    }
};

class FGTextFormat : public gui::TextFormat {
    using Parent = gui::TextFormat;

public:
    static constexpr Int32 CtrlVersion = 0;

    FGTextFormat() = default;
    ~FGTextFormat() override = default;

    bool setScriptValue(std::string_view rawText);

    bool variableChanged(std::string_view value) override {
        return setScriptValue(value);
    }

    std::string getScriptValue() const override {
        return text();
    }

    Persistent::Error
    read(StreamIO& sio, Int32 version, Int32 user, Persistent::LoadContext& ctx) override;
    Persistent::Error
    write(StreamIO& sio, Int32 version, Int32 user, Persistent::StoreContext& ctx) const override;
};

class TestEdit : public gui::TextEdit {
    using Parent = gui::TextEdit;

public:
    TestEdit() = default;
    ~TestEdit() override = default;
    bool event(const gui::Event&) override;

protected:
    void drawBackground(gfx::SoftwareSurface&, Point2I) const override;
};

class FGBitmapCtrl : public gui::Bitmap {
    using Parent = gui::Bitmap;

public:
    FGBitmapCtrl() = default;
    ~FGBitmapCtrl() override = default;

    [[nodiscard]] Flt32 alphaPercentage() const noexcept {
        return alphaPercentage_;
    }

    void setAlphaPercentage(Flt32 alpha) noexcept {
        alphaPercentage_ = alpha;
    }

    std::function<void()> checkpoint() override;

    Persistent::Error
    read(StreamIO& sio, Int32 version, Int32 user, Persistent::LoadContext& ctx) override;
    Persistent::Error
    write(StreamIO& sio, Int32 version, Int32 user, Persistent::StoreContext& ctx) const override;

private:
    Flt32 alphaPercentage_{1.0f};
};

// -----------------------------------------------------------------------------
// ShapeView (fearGuiShapeView.cpp) & SkinView (fearGuiSkinView.cpp)
// -----------------------------------------------------------------------------
class ShapeView : public gui::Active {
    using Parent = gui::Active;

public:
    ShapeView();
    ~ShapeView() override = default;

    [[nodiscard]] const std::string& shapeName() const noexcept {
        return shapeName_;
    }

    void setShapeName(std::string_view name);

    [[nodiscard]] Point3F rotation() const noexcept {
        return rotation_;
    }

    void setRotation(Point3F rot) noexcept {
        rotation_ = rot;
    }

    void rotateByDrag(Point2I deltaPx) noexcept;
    void dragRotate(Point2I fromPt, Point2I toPt) noexcept;

    [[nodiscard]] Flt32 distance() const noexcept {
        return dist_;
    }

    void zoomByDrag(Int32 deltaY) noexcept;
    void dragZoom(Point2I fromPt, Point2I toPt) noexcept;
    void stepAutoRotate(Flt32 elapsedSec) noexcept;

    void bindResources(std::shared_ptr<ResourceManager> resources) {
        shapeResources_ = std::move(resources);
        shapePreview_.reset();
    }

    void tick(double time) override;
    bool draw(gfx::SoftwareSurface&, Point2I, RectI) const override;
    bool event(const gui::Event&) override;

    std::function<void()> checkpoint() override;
    bool inspectRead(sim::Inspect& inspector);
    bool inspectWrite(sim::Inspect& inspector) const;

    Persistent::Error
    read(StreamIO& sio, Int32 version, Int32 user, Persistent::LoadContext& ctx) override;
    Persistent::Error
    write(StreamIO& sio, Int32 version, Int32 user, Persistent::StoreContext& ctx) const override;

protected:
    std::string shapeName_{"logo.dts"};
    Point3F rotation_{0.0f, 0.0f, 0.0f};
    Flt32 dist_{10.0f};
    Flt32 minDist_{2.0f};
    Flt32 maxDist_{50.0f};
    bool autoRotate_{true};
    std::shared_ptr<ResourceManager> shapeResources_;
    mutable std::optional<world::TsShapeResources> shapePreview_;
    mutable std::string shapePreviewName_;
    double lastRotationTime_{};
    Point2I shapeDrag_{};
    UInt8 shapeDragButton_{};
};

class SkinView : public ShapeView {
    using Parent = ShapeView;

public:
    enum ArmorType : Int32 {
        Light = 0,
        Medium = 1,
        Heavy = 2,
        NumArmors = 3,
    };

    SkinView();
    ~SkinView() override = default;

    void setSkin(std::string_view skinName, bool male = true);
    void cycleArmor() noexcept;

    [[nodiscard]] const std::string& skinName() const noexcept {
        return skinName_;
    }

    [[nodiscard]] bool isMale() const noexcept {
        return male_;
    }

    [[nodiscard]] ArmorType currentArmor() const noexcept {
        return curArmor_;
    }

    [[nodiscard]] std::string resolvedShapeFilename() const;
    [[nodiscard]] std::string resolvedMaterialListName() const;

    void bindResources(std::shared_ptr<ResourceManager> resources) {
        resources_ = std::move(resources);
        preview_.reset();
    }

    void tick(double time) override;
    bool draw(gfx::SoftwareSurface&, Point2I, RectI) const override;
    bool event(const gui::Event&) override;
    std::function<void()> checkpoint() override;

private:
    std::string skinName_{"base"};
    bool male_{true};
    ArmorType curArmor_{Medium};
    std::shared_ptr<ResourceManager> resources_;
    mutable std::optional<world::TsShapeResources> preview_;
    mutable std::string previewKey_;
    double animationTime_{};
    Point2I lastDrag_{};
    UInt8 dragButton_{};
};

// -----------------------------------------------------------------------------
// SmackerCtrl (fearGuiSmacker.cpp)
// -----------------------------------------------------------------------------
class SmackerCtrl : public gui::Active {
    using Parent = gui::Active;

public:
    static constexpr Int32 SmackerCtrlVersion = 1;

    SmackerCtrl() {
        active = true;
    }

    ~SmackerCtrl() override = default;

    [[nodiscard]] Int32 smkTag() const noexcept {
        return smkTag_;
    }

    void setSmkTag(Int32 tag) noexcept {
        smkTag_ = tag;
    }

    [[nodiscard]] bool loop() const noexcept {
        return loop_;
    }

    void setLoop(bool l) noexcept {
        loop_ = l;
    }

    [[nodiscard]] Int32 movieDoneTag() const noexcept {
        return movieDoneTag_;
    }

    void setMovieDoneTag(Int32 tag) noexcept {
        movieDoneTag_ = tag;
    }

    [[nodiscard]] bool preload() const noexcept {
        return preload_;
    }

    void setPreload(bool p) noexcept {
        preload_ = p;
    }

    [[nodiscard]] Int32 palStart() const noexcept {
        return palStart_;
    }

    void setPalStart(Int32 s) noexcept {
        palStart_ = s;
    }

    [[nodiscard]] Int32 palColors() const noexcept {
        return palColors_;
    }

    void setPalColors(Int32 c) noexcept {
        palColors_ = c;
    }

    [[nodiscard]] bool stretch() const noexcept {
        return stretch_;
    }

    void setStretch(bool st) noexcept {
        stretch_ = st;
    }

    bool attachMovie(std::shared_ptr<const video::Movie> movie);
    bool stepFrame();
    [[nodiscard]] std::shared_ptr<const video::Frame> currentFrame() const noexcept;

    [[nodiscard]] bool isMovieFinished() const noexcept {
        return movieFinished_;
    }

    std::function<std::shared_ptr<const video::Movie>(Int32)> loadMovie;
    std::function<void(const gfx::Palette&)> applyPalette;
    std::function<void(std::shared_ptr<const video::Movie>, bool)> startAudio;
    std::function<void()> stopAudio;
    void wake() override;
    void close() override;
    void tick(double seconds) override;
    bool event(const gui::Event&) override;
    bool draw(gfx::SoftwareSurface&, Point2I, RectI) const override;

    std::function<void()> checkpoint() override;
    bool inspectRead(sim::Inspect& inspector);
    bool inspectWrite(sim::Inspect& inspector) const;

    Persistent::Error
    read(StreamIO& sio, Int32 version, Int32 user, Persistent::LoadContext& ctx) override;
    Persistent::Error
    write(StreamIO& sio, Int32 version, Int32 user, Persistent::StoreContext& ctx) const override;

private:
    Int32 smkTag_{0};
    bool loop_{false};
    Int32 movieDoneTag_{0};
    bool preload_{false};
    Int32 palStart_{0};
    Int32 palColors_{256};
    bool stretch_{false};
    std::unique_ptr<video::Decoder> decoder_{};
    bool movieFinished_{false};
    bool completionSent_{};
    bool audioStarted_{};
    std::optional<double> startTime_;
    void complete();
};

// -----------------------------------------------------------------------------
// FGPopUpMenu (fearGuiPopUpMenu.cpp)
// -----------------------------------------------------------------------------
class FGPopUpMenu : public gui::Control {
    using Parent = gui::Control;

public:
    enum MenuFlags : UInt32 {
        MENU_STRING = (1U << 0),
        MENU_SEPARATOR = (1U << 1),
        MENU_DISABLED = (1U << 2),
        MENU_CHECKED = (1U << 3),
    };

    struct MenuEntry {
        bool targeted{false};
        UInt32 tag{0};
        UInt32 flags{MENU_STRING};
        std::string text{};
        Point2I offset{0, 0};
        Point2I extent{150, 16};
    };

    FGPopUpMenu();
    ~FGPopUpMenu() override = default;

    void appendMenu(UInt32 flags, UInt32 tag, std::string_view text = {});
    void clearMenu() noexcept;

    [[nodiscard]] std::span<const MenuEntry> items() const noexcept {
        return items_;
    }

    [[nodiscard]] std::optional<UInt32> selectAt(Point2I localPt);

private:
    std::vector<MenuEntry> items_{};
};

// -----------------------------------------------------------------------------
// FGComboPopUp, FGComboList, FGComboBox (FearGuiCombo.cpp) & Derived Combos
// -----------------------------------------------------------------------------
class FGComboPopUp : public gui::Control {
    using Parent = gui::Control;

public:
    FGComboPopUp() = default;
    ~FGComboPopUp() override = default;
};

class FGComboList : public gui::Array {
    using Parent = gui::Array;

public:
    struct EntryRep {
        Int32 id{0};
        std::string name{};
    };

    FGComboList();
    ~FGComboList() override = default;

    void addEntry(std::string_view text, Int32 id);
    void deleteEntry(Int32 id);
    [[nodiscard]] Int32 findEntry(std::string_view text) const;
    void clearEntries();
    void setSelectedEntry(Int32 id);
    [[nodiscard]] Int32 getSelectedEntry() const noexcept;
    [[nodiscard]] std::string getSelectedText() const;
    void setSort(bool sortEntries);

    [[nodiscard]] std::span<const EntryRep> entries() const noexcept {
        return entries_;
    }

protected:
    void resortEntries();

    std::vector<EntryRep> entries_{};
    bool sortEntries_{true};
};

class FGComboBox : public gui::ComboBox {
    using Parent = gui::ComboBox;

public:
    static constexpr Int32 ComboBoxVersion = 0;

    FGComboBox() = default;
    ~FGComboBox() override = default;

    void setBitmaps(std::array<std::shared_ptr<const gfx::Bitmap>, 8> bitmaps) {
        bitmaps_ = std::move(bitmaps);
    }

    bool draw(gfx::SoftwareSurface&, Point2I, RectI) const override;
    bool selectAdjacent(int direction);

    [[nodiscard]] const std::string& bitmapRootName() const noexcept {
        return bitmapRootName_;
    }

    void setBitmapRootName(std::string_view name);

    void addEntry(std::string_view entryText, Int32 id);
    void deleteEntry(Int32 id);
    [[nodiscard]] Int32 findEntry(std::string_view entryText) const;
    void clearEntries();
    void setSelectedEntry(Int32 id);
    [[nodiscard]] Int32 getSelectedEntry() const noexcept;
    [[nodiscard]] std::string getSelectedText() const;

    std::function<void()> checkpoint() override;
    bool inspectRead(sim::Inspect& inspector);
    bool inspectWrite(sim::Inspect& inspector) const;

    Persistent::Error
    read(StreamIO& sio, Int32 version, Int32 user, Persistent::LoadContext& ctx) override;
    Persistent::Error
    write(StreamIO& sio, Int32 version, Int32 user, Persistent::StoreContext& ctx) const override;

protected:
    std::string bitmapRootName_{};
    FGComboList comboEntries_{};
    std::array<std::shared_ptr<const gfx::Bitmap>, 8> bitmaps_;
};

// Standard Combo (FearGuiStandardCombo.cpp)
class FGStandardPopUp : public FGComboPopUp {};

class StandardListCtrl : public FGComboList {};

class FGStandardComboBox : public FGComboBox {};

// Controls Combo (FearGuiControlsCombo.cpp)
class FGControlsPopUp : public FGComboPopUp {};

class ControlsListCtrl : public FGComboList {};

class FGControlsComboBox : public FGComboBox {};

// Gender Combo (FearGuiGenderCombo.cpp)
class FGGenderPopUp : public FGComboPopUp {};

class GenderListCtrl : public FGComboList {
public:
    GenderListCtrl();
};

class FGGenderComboBox : public FGComboBox {
public:
    FGGenderComboBox();
};

// Player List & Combo (FearGuiPlayerList.cpp / FearGuiPlayerCombo.cpp)
class PlayerListCtrl : public FGComboList {
public:
    PlayerListCtrl();

    [[nodiscard]] std::string getSelectedPlayer() const {
        return getSelectedText();
    }
};

class FGPlayerPopUp : public FGComboPopUp {};

class FGPlayerComboBox : public FGComboBox {};

// Buddy Combo (FearGuiBuddyCombo.cpp)
class FGBuddyComboBox : public FGStandardComboBox {
public:
    FGBuddyComboBox();
};

// Mission List & Combo (FearGuiMissionList.cpp / FearGuiMissionCombo.cpp)
class MissionListCtrl : public FGComboList {
public:
    MissionListCtrl();

    [[nodiscard]] std::string getSelectedMission() const {
        return getSelectedText();
    }
};

class FGMissionPopUp : public FGComboPopUp {};

class FGMissionComboBox : public FGComboBox {};

// -----------------------------------------------------------------------------
// Filter Controls (FearGuiFilters.cpp, FearGuiFilterCombo.cpp,
// FearGuiFilterCBList.cpp, FearGuiServerFilterCB.cpp, FearGuiGenericCB.cpp)
// -----------------------------------------------------------------------------
class FilterListCtrl : public FGArrayCtrl {
    using Parent = FGArrayCtrl;

public:
    FilterListCtrl();
    ~FilterListCtrl() override = default;

    void addFilterName(std::string_view name);

    [[nodiscard]] std::span<const std::string> filterNames() const noexcept {
        return filterNames_;
    }

private:
    std::vector<std::string> filterNames_{};
};

class FilterCtrl : public FGArrayCtrl {
    using Parent = FGArrayCtrl;

public:
    enum ConditionType : Int32 {
        FT_COND_NUMERICAL = 0,
        FT_COND_TEXT = 1,
        FT_COND_BOOL = 2,
    };

    struct FilterVars {
        Int32 tag{0};
        Int32 type{FT_COND_NUMERICAL};
        std::string_view label{};
    };

    struct FilterInfo {
        Int32 variable{0};
        Int32 condition{0};
        std::string value{};
    };

    FilterCtrl();
    ~FilterCtrl() override = default;

    [[nodiscard]] static std::span<const FilterVars, 8> variableTable() noexcept;
    [[nodiscard]] static std::vector<FilterInfo>
    parseFilterSpec(std::string_view spec, std::string* filterNameOut = nullptr);
    [[nodiscard]] static std::string formatFilterSpec(std::string_view filterName,
                                                      std::span<const FilterInfo> conditions);

    void loadFromSpec(std::string_view spec);
    [[nodiscard]] std::string saveToSpec() const;
    void addCondition(FilterInfo cond);
    bool removeCondition(std::size_t index);

    [[nodiscard]] const std::string& filterName() const noexcept {
        return filterName_;
    }

    void setFilterName(std::string_view name) {
        filterName_ = std::string(name);
    }

    [[nodiscard]] std::span<const FilterInfo> conditions() const noexcept {
        return conditions_;
    }

    std::string getCellText(Point2I cell) const override;

private:
    std::string filterName_{};
    std::vector<FilterInfo> conditions_{};
};

class FGFilterPopUp : public FGComboPopUp {};

class FilterVarCtrl : public FGComboList {
public:
    FilterVarCtrl();
    void setTable(std::span<const FilterCtrl::FilterVars> table);
};

class FGFilterComboBox : public FGComboBox {};

class FGGenericPopUp : public FGComboPopUp {};

class GenericListCtrl : public FGComboList {};

class FGGenericComboBox : public FGComboBox {};

class FGFilterCBListCtrl : public FGComboList {
public:
    FGFilterCBListCtrl();
    void loadFiltersFromConsole(Console& console);
    Int32 addFilterToConsole(Console& console, std::string_view filterName);
    bool removeFilterFromConsole(Console& console, Int32 filterNum);
};

class FGServerFilterPopUp : public FGComboPopUp {};

class FGServerFilterComboBox : public FGComboBox {};

// -----------------------------------------------------------------------------
// FGHelpCtrl (FearGuiHelpCtrl.cpp)
// -----------------------------------------------------------------------------
class FGHelpCtrl : public gui::Help {
    using Parent = gui::Help;

public:
    static constexpr UInt32 HoverDelayThresholdMs = 1069;

    struct TooltipLayout {
        bool visible{false};
        Point2I position{0, 0};
        Point2I extent{200, 20};
        std::string body{};
    };

    FGHelpCtrl() = default;
    ~FGHelpCtrl() override = default;

    [[nodiscard]] static TooltipLayout computeTagTooltip(std::string_view tagSpec,
                                                         UInt32 timeElapsedMs,
                                                         bool mouseClicked,
                                                         Point2I cursorBottomLeft,
                                                         Point2I surfaceExtent,
                                                         bool helpPopupsEnabled = true);

    [[nodiscard]] static TooltipLayout
    computeDirectTooltip(std::string_view rawText, Point2I cursorBottomLeft, Point2I surfaceExtent);
};

} // namespace opent1::game
