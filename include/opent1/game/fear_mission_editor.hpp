#pragma once

#include <opent1/console/console.hpp>
#include <opent1/game/edit_camera.hpp>
#include <opent1/game/f_base.hpp>
#include <opent1/gui/mission_view.hpp>
#include <opent1/sim/sim_editor_events.hpp>
#include <opent1/sim/sim_resource_volume.hpp>
#include <opent1/sim/sim_world_game.hpp>

#include <array>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace opent1::game {

class FearMissionEditor : public sim::SimObject {
    using Parent = sim::SimObject;

public:
    enum Flags : UInt32 {
        SimSelected = 1U << 0,
        ConstrainX = 1U << 1,
        ConstrainY = 1U << 2,
        ConstrainZ = 1U << 3,
        RotateX = 1U << 4,
        RotateY = 1U << 5,
        RotateZ = 1U << 6,
        DropToGround = 1U << 7,
        SnapToGrid = 1U << 8,
        SnapRotations = 1U << 9,
        DropAtCamera = 1U << 10,
        DropWithRotAtCamera = 1U << 11,
        DropBelowCamera = 1U << 12,
        DropToScreenCenter = 1U << 13,
        DropToSelectedObject = 1U << 14,
        UseEditResolution = 1U << 15,
        UsePlaneMovement = 1U << 16,
        AxisFollowsCamera = 1U << 17,
        ShowEditObjects = 1U << 18,
        ShowBoundingBoxes = 1U << 19,
        ShowHandles = 1U << 20,
    };

    class SelectionSet : public sim::SimSet {
        using Parent = sim::SimSet;

    public:
        SelectionSet() = default;
        ~SelectionSet() override = default;

        bool addObject(sim::SimObject& obj) override;
        bool removeObject(sim::SimObject& obj) override;
        void clearSet();
        void calcCentroid();
        [[nodiscard]] bool objectLocked() const;

        [[nodiscard]] const Point3F& centroid() const noexcept {
            return centroid_;
        }

        [[nodiscard]] std::span<sim::SimObject* const> objects() const noexcept {
            return items_;
        }

        bool processEvent(const sim::SimEvent& event) override;
        bool processQuery(sim::SimQuery& query) override;

        FearMissionEditor* parentEditor{nullptr};

    private:
        std::vector<sim::SimObject*> items_{};
        Point3F centroid_{0.0f, 0.0f, 0.0f};
    };

    struct TypeInfo {
        UInt32 mask{0};
        std::string description{};
        std::string lockedBmpName{};
        std::string unlockedBmpName{};
    };

    struct InfoEntry {
        sim::SimObjectId id{0};
        sim::SimObject* object{nullptr};
        TMat3F transform{};
    };

    struct Bookmark {
        bool valid{false};
        Point3F pos{0.0f, 0.0f, 0.0f};
        EulerF rot{0.0f, 0.0f, 0.0f};
    };

    FearMissionEditor();
    ~FearMissionEditor() override;

    bool init();

    void bindGame(sim::SimGame* game) noexcept {
        game_ = game;
    }

    [[nodiscard]] std::optional<std::string> exportScript(const sim::SimObject& object) const;

    [[nodiscard]] SelectionSet& selection() noexcept {
        return selection_;
    }

    [[nodiscard]] const SelectionSet& selection() const noexcept {
        return selection_;
    }

    sim::SimObject* addObject(sim::SimObject* obj);
    [[nodiscard]] sim::SimObject* getObject(UInt32 id) const;
    [[nodiscard]] sim::SimGroup* getAddGroup() const;

    bool registerType(UInt32 mask,
                      std::string_view description,
                      std::string_view lockedBmp = {},
                      std::string_view unlockedBmp = {});
    [[nodiscard]] const TypeInfo* getTypeInfo(sim::SimObject* obj) const;

    [[nodiscard]] const std::vector<TypeInfo>& typeInfos() const noexcept {
        return typeInfos_;
    }

    void saveInfo();
    void restoreInfo();

    [[nodiscard]] const std::vector<InfoEntry>& undoEntries() const noexcept {
        return undoBuffer_;
    }

    void placeBookmark(Int32 index);
    void gotoBookmark(Int32 index);

    [[nodiscard]] const std::array<Bookmark, 10>& bookmarks() const noexcept {
        return bookmarks_;
    }

    void moveObjectRelative(sim::SimObject* obj, const Point3F& delta);
    void moveObjectAbsolute(sim::SimObject* obj, const Point3F& pos);
    void rotateObject(sim::SimObject* obj, const Point3F& anchor, Flt32 rot);
    bool dropObjectDown(sim::SimObject* obj);
    void onDrop(sim::SimObject* obj);
    void objectsToCamera();
    void cameraToObjects();

    bool copySelection();
    bool pasteSelection();
    bool duplicateSelection();
    void deleteSelection();

    void getConsoleOptions();
    void rebuildCommandMap();

    [[nodiscard]] UInt32 grabMask() const noexcept {
        return grabMask_;
    }

    void setGrabMask(UInt32 mask) noexcept {
        grabMask_ = mask;
    }

    [[nodiscard]] UInt32 placeMask() const noexcept {
        return placeMask_;
    }

    void setPlaceMask(UInt32 mask) noexcept {
        placeMask_ = mask;
    }

    [[nodiscard]] UInt32 defaultPlaceMask() const noexcept {
        return defaultPlaceMask_;
    }

    void setDefaultPlaceMask(UInt32 mask) noexcept {
        defaultPlaceMask_ = mask;
    }

    [[nodiscard]] UInt32 flags() const noexcept {
        return flags_.mask();
    }

    [[nodiscard]] const Point3F& granularity() const noexcept {
        return granularity_;
    }

    [[nodiscard]] Flt32 rotationSnap() const noexcept {
        return rotationSnap_;
    }

    bool onAdd() override;
    void onRemove() override;
    void onDeleteNotify(sim::SimObject& object) override;

private:
    void deleteObjectRecursive(sim::SimObject* obj);

    SelectionSet selection_{};
    std::vector<TypeInfo> typeInfos_{};
    std::vector<InfoEntry> undoBuffer_{};
    std::array<Bookmark, 10> bookmarks_{};
    std::vector<std::byte> clipboardBuffer_{};
    sim::SimGame* game_{};
    std::string clipboardScript_{};
    Point3F copyPosition_{};

    BitSet32 flags_{ShowEditObjects | ShowHandles | DropAtCamera};
    Point3F granularity_{1.0f, 1.0f, 1.0f};
    Flt32 rotationSnap_{15.0f};
    UInt32 grabMask_{~0U};
    UInt32 placeMask_{SimTerrainObjectType | SimInteriorObjectType};
    UInt32 defaultPlaceMask_{SimTerrainObjectType | SimInteriorObjectType};
    sim::SimManager* targetManager_{nullptr};
};

class FearMissionPlugin : public sim::SimConsolePlugin {
public:
    explicit FearMissionPlugin(sim::SimGame* game = nullptr) : game_(game) {}

    ~FearMissionPlugin() override = default;

    void init(Console& console, sim::SimManager* manager) override;

    void setManager(sim::SimManager* manager) override {
        if (manager_ != manager) {
            me_ = nullptr;
        }
        manager_ = manager;
    }

    void endFrame() override;

    [[nodiscard]] FearMissionEditor* editor() const noexcept {
        return me_;
    }

private:
    std::optional<std::string>
    handleCommand(Console& console, Int32 id, std::span<const std::string> argv);

    Console* console_{nullptr};
    sim::SimManager* manager_{nullptr};
    FearMissionEditor* me_{nullptr};
    sim::SimGame* game_{};
};

class MEObjectList {
public:
    enum BitmapIndices : Int32 {
        BmpNone = -1,
        BmpChildAbove = 0,
        BmpChildBelow = 1,
        BmpChildBetween = 2,
        BmpParentOpenAbove = 3,
        BmpParentOpenBelow = 4,
        BmpParentOpenBetween = 5,
        BmpParentOpenOnly = 6,
        BmpParentClosedAbove = 7,
        BmpParentClosedBelow = 8,
        BmpParentClosedBetween = 9,
        BmpParentClosedOnly = 10,
        BmpParentContinue = 11,
        BmpCount = 12,
    };

    enum SelectionModes : Int32 {
        SelNormal = 0,
        SelToggle = 1,
        SelMultiple = 2,
    };

    struct DOEntry {
        sim::SimObject* obj{nullptr};
        UInt32 id{0};
        Int32 depth{0};
        Int32 parentIndex{-1};
        Int32 lastInGroup{0};
    };

    MEObjectList() = default;
    ~MEObjectList() = default;

    void clearDisplayGroups();
    bool addDisplayGroup(sim::SimObject* obj);
    void buildObjectDisplayList();

    [[nodiscard]] Int32 getStateBmp(UInt32 index) const noexcept;
    [[nodiscard]] Int32 getBranchBmp(UInt32 index, Int32 depth) const noexcept;
    [[nodiscard]] std::string formatEntryText(const DOEntry& entry) const;

    void setSelMode(Int32 mode) noexcept {
        selMode_ = mode;
    }

    [[nodiscard]] Int32 getSelMode() const noexcept {
        return selMode_;
    }

    void unselectAll(sim::SimGroup* group = nullptr);
    void selectObject(sim::SimObject* obj, bool select = true);
    void expandToObject(sim::SimObject* obj);

    bool inspectObject(sim::SimObject* obj);
    bool applyInspection(sim::SimObject* obj);

    [[nodiscard]] const std::vector<DOEntry>& displayList() const noexcept {
        return objectDisplayList_;
    }

    [[nodiscard]] const sim::Inspect& inspectBuffer() const noexcept {
        return inspectBuffer_;
    }

    [[nodiscard]] sim::Inspect& inspectBuffer() noexcept {
        return inspectBuffer_;
    }

    static void
    registerConsoleCommands(Console& console, MEObjectList& list, sim::SimManager* manager);

private:
    void flattenObject(sim::SimObject* obj, Int32 depth, Int32 parentIndex, bool isLastInGroup);

    std::vector<sim::SimObject*> roots_{};
    std::vector<DOEntry> objectDisplayList_{};
    Int32 selMode_{SelNormal};
    sim::Inspect inspectBuffer_{};
    sim::SimObject* inspectObj_{nullptr};
};

} // namespace opent1::game
