#pragma once
#include <opent1/gui/ts_control.hpp>
#include <opent1/world/mission_selection.hpp>
#include <opent1/gui/controls.hpp>

namespace opent1::gui {
class MissionView : public TSControl {
public:
    explicit MissionView(std::shared_ptr<sim::SimWorld>);

    ~MissionView() override {
        close();
    }

    world::MissionSelection selection;
    UInt32 grabMask{~(sim::TerrainObjectType | sim::ContainerObjectType)};
    bool snapMovement{}, snapRotation{};
    float rotationSensitivity{.01f};

    bool wantsTab() const override {
        return true;
    }

    bool becomeFirstResponder() const override {
        return visible();
    }

    bool event(const Event&) override;
    void close() override;
    std::function<void()> checkpoint() override;
    // Scoped command leases also expire safely if this control dies first.
    [[nodiscard]] std::vector<std::shared_ptr<void>> install(Console&);

private:
    std::optional<Point3F> intercept(const Event&) const;
    Point3F anchor_, last_;
    Point2I mouse_;
    bool dragging_{}, rotating_{}, vertical_{};
    float rotationSum_{}, rotationApplied_{};
};

class MissionTree : public Array {
public:
    struct Row {
        sim::SimAddress address;
        std::string label;
        UInt8 depth{};
        bool branch{}, expanded{}, chosen{};
        sim::SimAddress parent;
    };

    explicit MissionTree(MissionView&);
    bool root(sim::SimGroup&);
    bool refresh();
    // Native tree plugins may expose several sets, including non-owning sets.
    // The complete replacement is validated before publishing any rows.
    bool roots(std::span<sim::SimSet* const>, bool showRoots = true);

    struct ClassIcon {
        std::string name;
        Int32 normal{}, selected{};
    };

    std::vector<std::shared_ptr<const gfx::Bitmap>> bitmaps;
    std::vector<ClassIcon> classes;
    std::function<std::string(const sim::SimObject&)> className;
    [[nodiscard]] Int32 icon(const sim::SimObject&, bool selected) const;

    [[nodiscard]] std::span<const Row> rows() const {
        return rows_;
    }

    std::shared_ptr<const gfx::BitmapFont> font;
    bool select(Point2I) override;
    bool event(const Event&) override;
    // Row parentage is explicit: one object can occur under multiple SimSets.
    // Generation/membership checks reject stale drags and ID reuse.
    bool drop(const Row& source, const Row& target, bool before = false, bool copy = false);
    bool rename(const Row&, std::string_view);
    bool lock(const Row&, bool);
    bool remove(const Row&);
    bool erase(const Row&);
    [[nodiscard]] std::optional<Row> current() const;
    std::function<bool(std::string_view)> command;
    std::function<bool(const Row&, const Row&, bool, bool)> dropped;
    void tick(double) override;
    std::function<void()> checkpoint() override;

protected:
    bool drawCell(gfx::SoftwareSurface&, Point2I, Point2I, bool, bool) const override;

private:
    MissionView* view_;
    std::weak_ptr<const void> viewLifetime_;
    std::weak_ptr<sim::SimWorld> world_;
    std::vector<sim::SimAddress> roots_;
    bool showRoots_{};
    std::vector<Row> rows_;
    bool additive_{}, toggle_{};
    std::optional<Row> drag_;
    Point2I dragPoint_;
    bool dragging_{};
};
} // namespace opent1::gui
