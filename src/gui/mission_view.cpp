#include <opent1/gui/mission_view.hpp>
#include <cmath>
#include <charconv>
#include <unordered_set>

namespace opent1::gui {
MissionView::MissionView(std::shared_ptr<sim::SimWorld> model) : selection(model) {
    if (!model) {
        throw std::invalid_argument("mission world");
    }
    world = model;
}

std::optional<Point3F> MissionView::intercept(const Event& e) const {
    const auto r = ray(e);
    if (!r) {
        return {};
    }
    const auto divisor = vertical_ ? r->direction.y : r->direction.z;
    if (std::abs(divisor) < 1.e-7f) {
        return {};
    }
    const auto t =
        ((vertical_ ? anchor_.y : anchor_.z) - (vertical_ ? r->origin.y : r->origin.z)) / divisor;
    if (!std::isfinite(t) || t < 0) {
        return {};
    }
    return r->origin + r->direction * t;
}

bool MissionView::event(const Event& e) {
    if (e.type == EventType::Cancel) {
        close();
        return true;
    }
    if (e.type == EventType::KeyDown) {
        if (e.control && e.key == Key::Z) {
            return e.shift ? selection.redo() : selection.undo();
        }
        if (e.key == Key::Escape) {
            close();
            if (canvas() && canvas()->capture() == this) {
                canvas()->unlockMouse();
            }
            return true;
        }
    }
    if (e.type == EventType::Down && (e.button == 1 || e.button == 3)) {
        auto model = world.lock();
        if (!model || !canvas() || !canvas()->makeFirstResponder(this) ||
            !canvas()->lockMouse(this)) {
            return false;
        }
        const auto hit = pick(*model, e, grabMask);
        auto* object = hit ? hit->object.resolve(model->manager()) : nullptr;
        if (e.button == 1) {
            if (!object) {
                if (!e.shift) {
                    selection.clear();
                }
                canvas()->unlockMouse();
                return true;
            }
            const auto selected = selection.selection();
            const bool has = std::ranges::any_of(selected, [&](auto a) {
                return a.objectId == object->getId() && a.generation == object->generation();
            });
            if (!has || e.shift) {
                if (!selection.select(*object, e.shift, e.shift)) {
                    return false;
                }
            }
        }
        const auto center = selection.centroid();
        if (!center || !selection.begin()) {
            return false;
        }
        anchor_ = *center;
        vertical_ = false;
        rotating_ = e.button == 3;
        rotationSum_ = rotationApplied_ = 0;
        mouse_ = e.point;
        const auto point = intercept(e);
        if (!rotating_ && !point) {
            selection.cancel();
            return false;
        }
        last_ = point.value_or(anchor_);
        dragging_ = true;
        return true;
    }
    if (e.type == EventType::Drag && dragging_) {
        if (rotating_) {
            if (!std::isfinite(rotationSensitivity) || std::abs(rotationSensitivity) > 10) {
                return false;
            }
            rotationSum_ += float(e.point.x - mouse_.x) * rotationSensitivity;
            float target = rotationSum_;
            if (snapRotation) {
                const auto step = selection.rotationSnap.z;
                if (!std::isfinite(step) || step <= 0) {
                    return false;
                }
                target -= std::fmod(target, step);
            }
            const auto angle = target - rotationApplied_;
            if (angle != 0 && !selection.rotate({0, 0, angle}, snapRotation)) {
                return false;
            }
            rotationApplied_ = target;
            mouse_ = e.point;
            return true;
        }
        if (vertical_ != e.shift) {
            vertical_ = e.shift;
            const auto point = intercept(e);
            if (!point) {
                return false;
            }
            last_ = *point;
            return true;
        }
        const auto point = intercept(e), before = selection.centroid();
        if (!point || !before || !selection.move(*point - last_, snapMovement)) {
            return false;
        }
        const auto after = selection.centroid();
        if (!after) {
            return false;
        }
        last_ += *after - *before;
        return true;
    }
    if (e.type == EventType::Up && dragging_) {
        if ((rotating_ && e.button != 3) || (!rotating_ && e.button != 1) || !canvas()) {
            return false;
        }
        canvas()->unlockMouse();
        if (!selection.commit()) {
            return false;
        }
        dragging_ = false;
        return true;
    }
    return TSControl::event(e);
}

void MissionView::close() {
    if (dragging_) {
        selection.cancel();
        dragging_ = false;
    }
}

std::function<void()> MissionView::checkpoint() {
    auto base = TSControl::checkpoint();
    auto model = selection.checkpoint();
    const auto anchor = anchor_, last = last_;
    const auto point = mouse_;
    const bool dragging = dragging_, rotating = rotating_, vertical = vertical_;
    const auto sum = rotationSum_, applied = rotationApplied_;
    return [this,
            base = std::move(base),
            model = std::move(model),
            anchor,
            last,
            point,
            dragging,
            rotating,
            vertical,
            sum,
            applied] {
        base();
        model();
        anchor_ = anchor;
        last_ = last;
        mouse_ = point;
        dragging_ = dragging;
        rotating_ = rotating;
        vertical_ = vertical;
        rotationSum_ = sum;
        rotationApplied_ = applied;
    };
}

std::vector<std::shared_ptr<void>> MissionView::install(Console& console) {
    constexpr std::array names{"Mission::select",
                               "Mission::clearSelection",
                               "Mission::moveSelection",
                               "Mission::rotateSelection",
                               "Mission::dropSelection",
                               "Mission::undo",
                               "Mission::redo",
                               "Mission::snapMovement",
                               "Mission::snapRotation"};
    std::vector<std::shared_ptr<void>> leases;
    const auto token = lifetime();
    for (std::size_t index = 0; index < names.size(); ++index) {
        leases.push_back(console.scopedCommand(
            Int32(index),
            names[index],
            [this, token](Console&, Int32 id, std::span<const std::string> args)
                -> std::optional<std::string> {
                if (token.expired() || args.empty()) {
                    return "False";
                }
                args = args.subspan(1);
                const auto yes = [](bool value) -> std::optional<std::string> {
                    return value ? "True" : "False";
                };
                auto model = world.lock();
                if (!model) {
                    return "False";
                }
                const auto number = [](std::string_view text, auto& value) {
                    const auto r = std::from_chars(text.data(), text.data() + text.size(), value);
                    return r.ec == std::errc{} && r.ptr == text.data() + text.size();
                };
                if (id == 0) {
                    if (args.empty() || args.size() > 3) {
                        return "False";
                    }
                    auto* object = model->manager().findObject(args[0]);
                    return yes(
                        object &&
                        selection.select(*object,
                                         args.size() > 1 && (args[1] == "1" || args[1] == "True"),
                                         args.size() > 2 && (args[2] == "1" || args[2] == "True")));
                }
                if (id == 1) {
                    if (!args.empty()) {
                        return "False";
                    }
                    selection.clear();
                    return "True";
                }
                if (id == 2 || id == 3) {
                    Point3F delta;
                    if (args.size() != 3 || !number(args[0], delta.x) ||
                        !number(args[1], delta.y) || !number(args[2], delta.z)) {
                        return "False";
                    }
                    return yes(id == 2
                                   ? selection.move(delta, snapMovement)
                                   : selection.rotate({delta.x, delta.y, delta.z}, snapRotation));
                }
                if (id == 4) {
                    UInt32 mask = sim::TerrainObjectType;
                    if (args.size() > 1 || (!args.empty() && !number(args[0], mask))) {
                        return "False";
                    }
                    return yes(selection.drop(mask));
                }
                if (id == 5 || id == 6) {
                    return yes(args.empty() && (id == 5 ? selection.undo() : selection.redo()));
                }
                if (args.size() != 1 ||
                    (args[0] != "0" && args[0] != "1" && args[0] != "True" && args[0] != "False")) {
                    return "False";
                }
                const bool enabled = args[0] == "1" || args[0] == "True";
                if (id == 7) {
                    snapMovement = enabled;
                } else {
                    snapRotation = enabled;
                }
                return "True";
            }));
    }
    return leases;
}

MissionTree::MissionTree(MissionView& view)
    : view_(&view), viewLifetime_(view.lifetime()), world_(view.world) {
    cells = {1, 0};
    cellSize = {240, 16};
}

bool MissionTree::root(sim::SimGroup& group) {
    sim::SimSet* set = &group;
    return roots(std::span(&set, 1), false);
}

bool MissionTree::roots(std::span<sim::SimSet* const> sets, bool show) {
    const auto model = world_.lock();
    if (!model || sets.size() > 64) {
        return false;
    }
    std::vector<sim::SimAddress> staged;
    for (auto* set : sets) {
        if (!set || set->getManager() != &model->manager() || !set->isProperlyAdded() ||
            std::ranges::any_of(staged, [&](auto a) {
                return a.generation == set->generation();
            })) {
            return false;
        }
        staged.push_back(set->address());
    }
    auto old = std::move(roots_);
    const auto oldShow = showRoots_;
    roots_ = std::move(staged);
    showRoots_ = show;
    if (!refresh()) {
        roots_ = std::move(old);
        showRoots_ = oldShow;
        return false;
    }
    return true;
}

Int32 MissionTree::icon(const sim::SimObject& object, bool chosen) const {
    const auto match = [&](std::string_view name) -> Int32 {
        for (const auto& entry : classes) {
            if (entry.name.size() > name.size()) {
                continue;
            }
            bool same = true;
            for (std::size_t i = 0; i < entry.name.size(); ++i) {
                const auto lower = [](char c) {
                    return c >= 'A' && c <= 'Z' ? char(c - 'A' + 'a') : c;
                };
                if (lower(entry.name[i]) != lower(name[i])) {
                    same = false;
                    break;
                }
            }
            if (same) {
                return chosen ? entry.selected : entry.normal;
            }
        }
        return -1;
    };
    auto result = className ? match(className(object)) : -1;
    if (result < 0 && dynamic_cast<const sim::SimGroup*>(&object)) {
        result = match("SimGroup");
    }
    if (result < 0 && dynamic_cast<const sim::SimSet*>(&object)) {
        result = match("SimSet");
    }
    if (result < 0 && object.isLocked()) {
        result = match("Locked");
    }
    return result < 0 ? Int32(chosen) : result;
}

bool MissionTree::refresh() {
    if (viewLifetime_.expired()) {
        return false;
    }
    const auto model = world_.lock();
    if (!model) {
        return false;
    }
    const auto previous = current();
    std::vector<Row> rows;
    std::unordered_set<UInt64> seen;
    bool valid = true;
    const auto visit =
        [&](auto&& self, sim::SimObject& object, UInt8 depth, sim::SimAddress parent) -> void {
        if (depth > 64 || rows.size() >= 2048 || !seen.insert(object.generation()).second) {
            valid = false;
            return;
        }
        auto* children = dynamic_cast<sim::SimSet*>(&object);
        rows.push_back({object.address(),
                        object.getName().empty() ? std::to_string(object.getId())
                                                 : std::string(object.getName()),
                        depth,
                        children && !children->objects().empty(),
                        object.isExpanded(),
                        object.isSelected(),
                        parent});
        if (children && object.isExpanded()) {
            for (auto* child : children->objects()) {
                self(self, *child, UInt8(depth + 1), object.address());
            }
        }
        seen.erase(object.generation());
    };
    for (const auto address : roots_) {
        auto* set = address.objectId == model->manager().getId()
                        ? &model->manager()
                        : dynamic_cast<sim::SimSet*>(address.resolve(model->manager()));
        if (!set) {
            return false;
        }
        if (showRoots_) {
            visit(visit, *set, 0, {});
        } else {
            for (auto* child : set->objects()) {
                visit(visit, *child, 0, set->address());
            }
        }
    }
    if (!valid || !setSize({1, Int32(rows.size())})) {
        return false;
    }
    rows_ = std::move(rows);
    if (previous) {
        const auto it = std::ranges::find_if(rows_, [&](const auto& row) {
            return row.address.generation == previous->address.generation &&
                   row.parent.generation == previous->parent.generation;
        });
        selected = it == rows_.end() ? Point2I{-1, -1} : Point2I{0, Int32(it - rows_.begin())};
    } else if (selected.y >= cells.y) {
        selected = {-1, -1};
    }
    return true;
}

std::optional<MissionTree::Row> MissionTree::current() const {
    return selected.x == 0 && selected.y >= 0 && std::size_t(selected.y) < rows_.size()
               ? std::optional(rows_[selected.y])
               : std::nullopt;
}

bool MissionTree::rename(const Row& row, std::string_view name) {
    auto model = world_.lock();
    auto* object =
        model && !viewLifetime_.expired() ? row.address.resolve(model->manager()) : nullptr;
    if (!object || object == &model->manager() || name.size() > 255 ||
        name.find_first_of("/\\") != name.npos || name.find('\0') != name.npos) {
        return false;
    }
    const auto old = std::string(object->getName());
    bool ok = false;
    model->manager().dispatch([&] {
        try {
            object->assignName(name);
            ok = !object->isRemoved() && object->getName() == name;
        } catch (...) {
            if (!object->isRemoved()) {
                object->assignName(old);
            }
        }
    });
    return ok && refresh();
}

bool MissionTree::lock(const Row& row, bool value) {
    auto model = world_.lock();
    auto* object =
        model && !viewLifetime_.expired() ? row.address.resolve(model->manager()) : nullptr;
    if (!object || object == &model->manager()) {
        return false;
    }
    std::vector<sim::SimObject*> pending{object}, leaves;
    std::unordered_set<UInt64> seen;
    while (!pending.empty()) {
        auto* item = pending.back();
        pending.pop_back();
        if (!seen.insert(item->generation()).second) {
            continue;
        }
        if (seen.size() > 65536) {
            return false;
        }
        if (auto* set = dynamic_cast<sim::SimSet*>(item)) {
            for (auto* child : set->objects()) {
                pending.push_back(child);
            }
        } else {
            leaves.push_back(item);
        }
    }
    for (auto* item : leaves) {
        item->setLocked(value);
    }
    return refresh();
}

bool MissionTree::remove(const Row& row) {
    auto model = world_.lock();
    if (!model || viewLifetime_.expired() || !row.parent.generation) {
        return false;
    }
    auto* object = row.address.resolve(model->manager());
    auto* parent = dynamic_cast<sim::SimSet*>(row.parent.resolve(model->manager()));
    return object && parent && parent->removeObject(*object) && refresh();
}

bool MissionTree::erase(const Row& row) {
    auto model = world_.lock();
    auto* object =
        model && !viewLifetime_.expired() ? row.address.resolve(model->manager()) : nullptr;
    return object && row.parent.generation && object != &model->manager() &&
           model->manager().deleteObject(*object) && refresh();
}

bool MissionTree::drop(const Row& source, const Row& target, bool before, bool copy) {
    auto model = world_.lock();
    if (!model || viewLifetime_.expired() || !source.parent.generation) {
        return false;
    }
    auto& manager = model->manager();
    auto* object = source.address.resolve(manager);
    auto* from = dynamic_cast<sim::SimSet*>(source.parent.resolve(manager));
    auto* targetObject = target.address.resolve(manager);
    auto* to = dynamic_cast<sim::SimSet*>((before || !dynamic_cast<sim::SimSet*>(targetObject))
                                              ? target.parent.resolve(manager)
                                              : targetObject);
    if (!object || !from || !to || object == to || object == targetObject ||
        std::ranges::find(from->objects(), object) == from->objects().end()) {
        return false;
    }
    auto* next = before ? targetObject : nullptr;
    if (next && std::ranges::find(to->objects(), next) == to->objects().end()) {
        return false;
    }
    const bool already = std::ranges::find(to->objects(), object) != to->objects().end();
    if (already) {
        const std::vector<sim::SimObject*> order(to->objects().begin(), to->objects().end());
        auto restore = checkpoint();
        if (to->reOrder(*object, next) && refresh()) {
            return true;
        }
        for (auto* item : order) {
            to->reOrder(*item);
        }
        restore();
        return false;
    }
    if (dynamic_cast<sim::SimGroup*>(to) && !dynamic_cast<sim::SimGroup*>(from)) {
        return false;
    }
    // Prepare rollback ordering before callbacks. Group->set copies membership;
    // group->group moves ownership; set->set supports move/copy explicitly.
    const std::vector<sim::SimObject*> order(from->objects().begin(), from->objects().end()),
        targetOrder(to->objects().begin(), to->objects().end());
    auto restore = checkpoint();
    auto* owner = object->getGroup();
    bool ok = false;
    manager.dispatch([&] {
        try {
            ok = to->addObject(*object) && !object->isRemoved() && !to->isRemoved();
            if (ok && next) {
                ok = to->reOrder(*object, next);
            }
            if (ok && !dynamic_cast<sim::SimGroup*>(from) && !copy) {
                ok = from->removeObject(*object);
            }
            if (ok) {
                ok = refresh();
            }
        } catch (...) {
            ok = false;
        }
        if (!ok) {
            if (!object->isRemoved()) {
                try {
                    if (!to->isRemoved()) {
                        to->removeObject(*object);
                        for (auto* item : targetOrder) {
                            if (!item->isRemoved()) {
                                to->reOrder(*item);
                            }
                        }
                    }
                    if (owner && !owner->isRemoved() && object->getGroup() != owner) {
                        owner->addObject(*object);
                    }
                    if (!from->isRemoved()) {
                        from->addObject(*object);
                        for (auto* item : order) {
                            if (!item->isRemoved()) {
                                from->reOrder(*item);
                            }
                        }
                    }
                } catch (...) {
                }
            }
            restore();
        }
    });
    return ok;
}

void MissionTree::tick(double) {
    if (!viewLifetime_.expired()) {
        refresh();
    }
}

bool MissionTree::select(Point2I cell) {
    if (viewLifetime_.expired() || cell.x != 0 || cell.y < 0 ||
        std::size_t(cell.y) >= rows_.size()) {
        return false;
    }
    auto model = world_.lock();
    auto* object = model ? rows_[cell.y].address.resolve(model->manager()) : nullptr;
    if (!object) {
        return false;
    }
    auto restore = Array::checkpoint();
    if (!Array::select(cell) || !view_->selection.select(*object, additive_ || toggle_, toggle_)) {
        restore();
        return false;
    }
    for (auto& row : rows_) {
        if (auto* item = row.address.resolve(model->manager())) {
            row.chosen = item->isSelected();
        }
    }
    return true;
}

bool MissionTree::event(const Event& event) {
    if (viewLifetime_.expired()) {
        return false;
    }
    additive_ = event.shift;
    toggle_ = event.control;
    if (event.type == EventType::Cancel) {
        drag_.reset();
        dragging_ = false;
        return Array::event(event);
    }
    if (event.type == EventType::KeyDown && command && !event.repeat) {
        std::string_view action;
        if (event.key == Key::Delete) {
            action = "delete";
        } else if (event.key == Key::Return) {
            action = "inspect";
        } else if (event.control) {
            if (event.key == Key::C) {
                action = "copy";
            }
            if (event.key == Key::X) {
                action = "cut";
            }
            if (event.key == Key::V) {
                action = "paste";
            }
        }
        if (!action.empty()) {
            return command(action);
        }
    }
    if (event.type == EventType::Drag && drag_) {
        dragging_ |=
            std::abs(event.point.x - dragPoint_.x) + std::abs(event.point.y - dragPoint_.y) >= 4;
        return true;
    }
    if (event.type == EventType::Up && drag_) {
        auto source = *drag_;
        const bool moving = dragging_;
        drag_.reset();
        dragging_ = false;
        depressed = false;
        if (canvas()) {
            canvas()->unlockMouse();
        }
        if (!moving) {
            return true;
        }
        const auto p = globalToLocal(event.point);
        const auto index = cellSize.y > 0 ? p.y / cellSize.y : -1;
        if (p.y < 0 || index < 0 || std::size_t(index) >= rows_.size()) {
            return false;
        }
        const auto target = rows_[index];
        const bool before = p.y % cellSize.y < cellSize.y / 3;
        if (dropped) {
            return dropped(source, target, before, event.shift);
        }
        return drop(source, target, before, event.shift);
    }
    if (event.type == EventType::Down) {
        const auto p = globalToLocal(event.point);
        const auto index = cellSize.y > 0 ? p.y / cellSize.y : -1;
        if (event.button == 3 && command && p.y >= 0 && index >= 0 &&
            std::size_t(index) < rows_.size()) {
            return canvas() && canvas()->makeFirstResponder(this) && select({0, index}) &&
                   command("menu");
        }
        if (p.y >= 0 && index >= 0 && std::size_t(index) < rows_.size() && rows_[index].branch &&
            p.x >= Int32(rows_[index].depth) * 12 && p.x < Int32(rows_[index].depth) * 12 + 12) {
            auto model = world_.lock();
            auto* object = model ? rows_[index].address.resolve(model->manager()) : nullptr;
            if (!object) {
                return false;
            }
            const bool old = object->isExpanded();
            object->setExpanded(!old);
            if (!refresh()) {
                object->setExpanded(old);
                return false;
            }
            return true;
        }
    }
    const bool ok = Array::event(event);
    if (ok && event.type == EventType::Down) {
        if (event.button == 3 && command) {
            return command("menu");
        }
        if (event.button == 1 && current() && canvas()) {
            if (!canvas()->lockMouse(this)) {
                return false;
            }
            drag_ = current();
            dragPoint_ = event.point;
            dragging_ = false;
        }
    }
    return ok;
}

std::function<void()> MissionTree::checkpoint() {
    auto base = Array::checkpoint();
    auto rows = rows_;
    const auto drag = drag_;
    const auto point = dragPoint_;
    const auto moving = dragging_;
    std::function<void()> model;
    if (!viewLifetime_.expired()) {
        model = view_->selection.checkpoint();
    }
    return [this,
            base = std::move(base),
            rows = std::move(rows),
            drag,
            point,
            moving,
            model = std::move(model)] {
        base();
        drag_ = drag;
        dragPoint_ = point;
        dragging_ = moving;
        if (model && !viewLifetime_.expired()) {
            model();
        }
        rows_ = rows;
        if (auto world = world_.lock()) {
            for (const auto& row : rows) {
                if (auto* object = row.address.resolve(world->manager())) {
                    object->setExpanded(row.expanded);
                }
            }
        }
    };
}

bool MissionTree::drawCell(
    gfx::SoftwareSurface& s, Point2I p, Point2I cell, bool, bool hovered) const {
    if (cell.y < 0 || std::size_t(cell.y) >= rows_.size()) {
        return false;
    }
    const auto& row = rows_[cell.y];
    s.fill({p.x, p.y, p.x + cellSize.x - 1, p.y + cellSize.y - 1},
           row.chosen ? 249
           : hovered  ? 253
                      : 255);
    const auto x = p.x + Int32(row.depth) * 12;
    if (row.branch) {
        s.rectangle({x + 1, p.y + 3, x + 9, p.y + 11}, 0);
        s.line({x + 3, p.y + 7}, {x + 7, p.y + 7}, 0);
        if (!row.expanded) {
            s.line({x + 5, p.y + 5}, {x + 5, p.y + 9}, 0);
        }
    }
    auto textX = x + 14;
    const auto model = world_.lock();
    auto* object = model ? row.address.resolve(model->manager()) : nullptr;
    if (object) {
        const auto index = icon(*object, row.chosen);
        if (index >= 0 && std::size_t(index) < bitmaps.size() && bitmaps[index]) {
            if (!s.blit(*bitmaps[index], {textX, p.y})) {
                return false;
            }
            textX += bitmaps[index]->width + 2;
        }
    }
    return !font || s.text(*font, {textX, p.y + 2}, row.label);
}
} // namespace opent1::gui
