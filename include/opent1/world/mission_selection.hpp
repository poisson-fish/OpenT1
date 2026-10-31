#pragma once
#include <opent1/sim/sim_editor_events.hpp>
#include <opent1/sim/sim_world_game.hpp>

namespace opent1::world {
// A selection belongs to one world incarnation. Histories contain generation
// addresses, never object pointers. One completed gesture is one undo entry.
class MissionSelection {
public:
    explicit MissionSelection(std::shared_ptr<sim::SimWorld> world) : world_(std::move(world)) {}

    ~MissionSelection() {
        clear();
    }

    bool select(sim::SimObject&, bool add = false, bool toggle = false);
    void clear();
    [[nodiscard]] std::vector<sim::SimAddress> selection() const;
    [[nodiscard]] std::optional<Point3F> centroid() const;
    bool move(Point3F delta, bool snap = false);
    bool rotate(EulerF delta, bool snap = false);
    bool drop(UInt32 mask = ~UInt32{});
    bool undo();
    bool redo();
    bool begin();
    bool commit();
    bool cancel();
    [[nodiscard]] std::function<void()> checkpoint();

    [[nodiscard]] bool healthy() const {
        return healthy_;
    }

    Point3F granularity{1, 1, .1f};
    EulerF rotationSnap{1.5707963268f, 1.5707963268f, 1.5707963268f};
    UInt8 constrainedAxes{};

private:
    struct Item {
        sim::SimAddress address;
        TMat3F transform;
    };

    using Record = std::vector<Item>;
    std::optional<Record> capture() const;
    bool apply(const Record&);
    bool change(Record);
    std::weak_ptr<sim::SimWorld> world_;
    std::vector<sim::SimAddress> selected_;
    std::vector<Record> undo_, redo_;
    std::optional<Record> gesture_;
    bool healthy_{true};
};
} // namespace opent1::world
