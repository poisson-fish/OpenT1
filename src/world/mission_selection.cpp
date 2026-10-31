#include <opent1/world/mission_selection.hpp>
#include <cmath>
#include <unordered_set>

namespace opent1::world {
namespace {
bool finite(Point3F p) {
    return std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z);
}

bool valid(const TMat3F& t) {
    if (!finite(t.p)) {
        return false;
    }
    for (const auto& row : t.m) {
        for (auto f : row) {
            if (!std::isfinite(f)) {
                return false;
            }
        }
    }
    return true;
}

bool same(sim::SimAddress a, sim::SimAddress b) {
    return a.objectId == b.objectId && a.generation == b.generation && a.managerId == b.managerId;
}

float snapped(float v, float step) {
    return v - std::fmod(v, step);
}
}

std::vector<sim::SimAddress> MissionSelection::selection() const {
    std::vector<sim::SimAddress> out;
    if (auto world = world_.lock()) {
        for (auto address : selected_) {
            if (address.resolve(world->manager())) {
                out.push_back(address);
            }
        }
    }
    return out;
}

void MissionSelection::clear() {
    if (auto world = world_.lock()) {
        for (auto address : selected_) {
            if (auto* o = address.resolve(world->manager())) {
                o->setSelected(false);
            }
        }
    }
    selected_.clear();
}

bool MissionSelection::select(sim::SimObject& object, bool add, bool toggle) {
    const auto world = world_.lock();
    if (!world || object.getManager() != &world->manager() || !object.isProperlyAdded() ||
        gesture_) {
        return false;
    }
    auto found = std::ranges::find_if(selected_, [&](auto a) {
        return same(a, object.address());
    });
    if (toggle && found != selected_.end()) {
        selected_.erase(found);
        object.setSelected(false);
        return true;
    }
    if (!add) {
        clear();
    }
    if (std::ranges::none_of(selected_, [&](auto a) {
            return same(a, object.address());
        })) {
        if (selected_.size() >= 4096) {
            return false;
        }
        selected_.push_back(object.address());
    }
    object.setSelected(true);
    return true;
}

std::optional<MissionSelection::Record> MissionSelection::capture() const {
    auto world = world_.lock();
    if (!world || selected_.empty()) {
        return {};
    }
    Record result;
    std::unordered_set<UInt64> seen;
    bool failed = false;
    const auto visit = [&](auto&& self, sim::SimObject& object, std::size_t depth) -> void {
        if (depth > 64 || result.size() >= 4096) {
            failed = true;
            return;
        }
        if (!seen.insert(object.generation()).second) {
            return;
        }
        if (object.isLocked()) {
            failed = true;
            return;
        }
        if (auto* set = dynamic_cast<sim::SimSet*>(&object)) {
            for (auto* child : set->objects()) {
                self(self, *child, depth + 1);
            }
        }
        sim::SimObjectTransformQuery query;
        if (object.processQuery(query)) {
            if (!valid(query.transform)) {
                failed = true;
                return;
            }
            result.push_back({object.address(), query.transform});
        }
    };
    try {
        for (auto address : selected_) {
            auto* o = address.resolve(world->manager());
            if (!o) {
                return {};
            }
            visit(visit, *o, 0);
        }
    } catch (...) {
        return {};
    }
    return failed || result.empty() ? std::nullopt : std::optional(std::move(result));
}

std::optional<Point3F> MissionSelection::centroid() const {
    const auto record = capture();
    if (!record) {
        return {};
    }
    Point3F center;
    for (const auto& item : *record) {
        center += item.transform.p;
    }
    return center / float(record->size());
}

bool MissionSelection::apply(const Record& record) {
    auto world = world_.lock();
    if (!world || record.empty()) {
        return false;
    }
    auto& manager = world->manager();
    Record before;
    try {
        for (const auto& item : record) {
            auto* o = item.address.resolve(manager);
            if (!o || o->isLocked() || !valid(item.transform)) {
                return false;
            }
            sim::SimObjectTransformQuery q;
            if (!o->processQuery(q) || !valid(q.transform)) {
                return false;
            }
            before.push_back({item.address, q.transform});
        }
    } catch (...) {
        return false;
    }
    std::size_t attempted = 0;
    bool success = true;
    try {
        for (const auto& item : record) {
            ++attempted;
            sim::SimObjectTransformEvent event;
            event.transform = item.transform;
            if (!manager.sendEvent(item.address, event)) {
                success = false;
                break;
            }
        }
    } catch (...) {
        success = false;
    }
    if (!success) {
        while (attempted) {
            const auto& item = before[--attempted];
            sim::SimObjectTransformEvent event;
            event.transform = item.transform;
            try {
                if (!manager.sendEvent(item.address, event)) {
                    healthy_ = false;
                }
            } catch (...) {
                healthy_ = false;
            }
        }
    }
    return success;
}

bool MissionSelection::change(Record next) {
    auto before = capture();
    if (!before || !apply(next)) {
        return false;
    }
    if (!gesture_) {
        undo_.push_back(std::move(*before));
        if (undo_.size() > 128) {
            undo_.erase(undo_.begin());
        }
        redo_.clear();
    }
    return true;
}

bool MissionSelection::move(Point3F delta, bool snap) {
    if (!finite(delta) || !finite(granularity) || granularity.x <= 0 || granularity.y <= 0 ||
        granularity.z <= 0) {
        return false;
    }
    auto next = capture();
    if (!next) {
        return false;
    }
    if (constrainedAxes & 1) {
        delta.x = 0;
    }
    if (constrainedAxes & 2) {
        delta.y = 0;
    }
    if (constrainedAxes & 4) {
        delta.z = 0;
    }
    if (snap) {
        Point3F center;
        for (const auto& item : *next) {
            center += item.transform.p;
        }
        center /= float(next->size());
        const auto target = center + delta;
        const Point3F rounded{snapped(target.x + granularity.x / 2, granularity.x),
                              snapped(target.y + granularity.y / 2, granularity.y),
                              snapped(target.z + granularity.z / 2, granularity.z)};
        delta = rounded - center;
        if (constrainedAxes & 1) {
            delta.x = 0;
        }
        if (constrainedAxes & 2) {
            delta.y = 0;
        }
        if (constrainedAxes & 4) {
            delta.z = 0;
        }
    }
    for (auto& item : *next) {
        item.transform.p += delta;
        item.transform.flags |= RMat3F::Matrix_HasTranslation;
    }
    return change(std::move(*next));
}

bool MissionSelection::rotate(EulerF delta, bool snap) {
    if (!finite({delta.x, delta.y, delta.z})) {
        return false;
    }
    auto next = capture();
    if (!next) {
        return false;
    }
    Point3F center;
    for (const auto& item : *next) {
        center += item.transform.p;
    }
    center /= float(next->size());
    TMat3F rotation;
    rotation.preRotateX(delta.x);
    rotation.preRotateY(delta.y);
    rotation.preRotateZ(delta.z);
    for (auto& item : *next) {
        if (snap && next->size() == 1) {
            if (rotationSnap.x <= 0 || rotationSnap.y <= 0 || rotationSnap.z <= 0 ||
                !finite({rotationSnap.x, rotationSnap.y, rotationSnap.z})) {
                return false;
            }
            EulerF angles;
            item.transform.angles(angles);
            const auto round = [](float a, float step) {
                return snapped(a + (a < 0 ? -step : step) / 4, step);
            };
            angles = {round(angles.x, rotationSnap.x) + delta.x,
                      round(angles.y, rotationSnap.y) + delta.y,
                      round(angles.z, rotationSnap.z) + delta.z};
            item.transform.set(angles, item.transform.p);
        } else {
            item.transform.p -= center;
            m_mul(item.transform, rotation, item.transform);
            item.transform.p += center;
            item.transform.flags |= RMat3F::Matrix_HasRotation | RMat3F::Matrix_HasTranslation;
        }
    }
    return change(std::move(*next));
}

bool MissionSelection::drop(UInt32 mask) {
    auto world = world_.lock();
    auto record = capture();
    if (!world || !record) {
        return false;
    }
    auto* root =
        dynamic_cast<sim::SimContainer*>(world->manager().findObject(sim::RootContainerId));
    if (!root) {
        return false;
    }
    float highest = -1.e7f;
    for (const auto& item : *record) {
        sim::SimContainerQuery q;
        q.id = item.address.objectId;
        q.mask = mask;
        q.box = {item.transform.p, item.transform.p + Point3F{0, 0, -10000}};
        sim::SimCollisionInfo hit;
        if (root->findLOS(q, hit) && !hit.surfaces.surfaces.empty()) {
            Point3F p;
            m_mul(hit.surfaces.surfaces.front().position, hit.surfaces.tWorld, p);
            highest = std::max(highest, p.z - item.transform.p.z);
        }
    }
    return highest > -1.e7f && move({0, 0, highest});
}

bool MissionSelection::undo() {
    if (gesture_ || undo_.empty()) {
        return false;
    }
    auto world = world_.lock();
    if (!world) {
        return false;
    }
    Record current;
    for (const auto& item : undo_.back()) {
        sim::SimObjectTransformQuery q;
        if (!world->manager().sendQuery(item.address, q)) {
            return false;
        }
        current.push_back({item.address, q.transform});
    }
    if (!apply(undo_.back())) {
        return false;
    }
    redo_.push_back(std::move(current));
    undo_.pop_back();
    return true;
}

bool MissionSelection::redo() {
    if (gesture_ || redo_.empty()) {
        return false;
    }
    auto world = world_.lock();
    if (!world) {
        return false;
    }
    Record current;
    for (const auto& item : redo_.back()) {
        sim::SimObjectTransformQuery q;
        if (!world->manager().sendQuery(item.address, q)) {
            return false;
        }
        current.push_back({item.address, q.transform});
    }
    if (!apply(redo_.back())) {
        return false;
    }
    undo_.push_back(std::move(current));
    redo_.pop_back();
    return true;
}

bool MissionSelection::begin() {
    if (gesture_) {
        return false;
    }
    gesture_ = capture();
    return gesture_.has_value();
}

bool MissionSelection::commit() {
    if (!gesture_) {
        return false;
    }
    undo_.push_back(std::move(*gesture_));
    gesture_.reset();
    if (undo_.size() > 128) {
        undo_.erase(undo_.begin());
    }
    redo_.clear();
    return true;
}

bool MissionSelection::cancel() {
    if (!gesture_) {
        return false;
    }
    if (!apply(*gesture_)) {
        return false;
    }
    gesture_.reset();
    return true;
}

std::function<void()> MissionSelection::checkpoint() {
    auto current = capture();
    auto selected = selected_;
    auto undo = undo_, redo = redo_;
    auto gesture = gesture_;
    return [this,
            current = std::move(current),
            selected = std::move(selected),
            undo = std::move(undo),
            redo = std::move(redo),
            gesture = std::move(gesture)] {
        if (current && !apply(*current)) {
            healthy_ = false;
        }
        clear();
        selected_ = selected;
        if (auto world = world_.lock()) {
            for (auto address : selected_) {
                if (auto* object = address.resolve(world->manager())) {
                    object->setSelected(true);
                }
            }
        }
        undo_ = undo;
        redo_ = redo;
        gesture_ = gesture;
    };
}
} // namespace opent1::world
