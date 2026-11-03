#include <opent1/gui/edit_control.hpp>
#include <ranges>

namespace opent1::gui {
EditControl::~EditControl() {
    try {
        close();
    } catch (...) {
    }
}

void EditControl::onRemove() {
    close();
    Control::onRemove();
}

bool EditControl::addNewControl(std::unique_ptr<Control> control) {
    auto* set = currentAddSet();
    if (!set || !control || control->parent() || control->getManager()) {
        return false;
    }
    auto* raw = control.get();
    if (auto* manager = set->getManager()) {
        if (manager->isDispatching()) {
            return false;
        }
        if (!manager->addObject(std::move(control))) {
            return false;
        }
        if (!set->addObject(*raw)) {
            manager->deleteObject(*raw);
            return false;
        }
    } else {
        // Allocate ownership storage before publishing a borrowed group link.
        pasted_.reserve(pasted_.size() + 1);
        if (!set->addObject(*raw)) {
            return false;
        }
        pasted_.push_back(std::move(control));
    }
    const bool selected = select(*raw);
    if (canvas()) {
        canvas()->rebuildTabs();
    }
    return selected;
}

UInt8 EditControl::sizingKnobs(Point2I p, RectI r) {
    const auto cx = (r.left + r.right) >> 1, cy = (r.top + r.bottom) >> 1;
    const std::array<Point2I, 8> points = {Point2I{r.left, r.top},
                                           {cx, r.top},
                                           {r.right, r.top},
                                           {r.left, r.bottom},
                                           {cx, r.bottom},
                                           {r.right, r.bottom},
                                           {r.left, cy},
                                           {r.right, cy}};
    const std::array<UInt8, 8> modes = {5, 4, 6, 9, 8, 10, 1, 2};
    for (auto i : std::views::iota(std::size_t{0}, points.size())) {
        if (std::abs(Int64(p.x) - points[i].x) <= 3 && std::abs(Int64(p.y) - points[i].y) <= 3) {
            return modes[i];
        }
    }
    return 0;
}

Control* EditControl::currentAddSet() const {
    if (addSet_ && !addSetLifetime_.expired()) {
        for (auto* c = addSet_; c; c = c->parent()) {
            if (c == parent()) {
                return addSet_;
            }
        }
    }
    return parent();
}

bool EditControl::addSet(Control& c) {
    Item item{&c, c.lifetime(), c.fields(), {}};
    if (&c != parent() && !live(item)) {
        return false;
    }
    if (currentAddSet() != &c) {
        clear();
    }
    addSet_ = &c;
    addSetLifetime_ = c.lifetime();
    return true;
}

bool EditControl::selectAll() {
    auto* set = currentAddSet();
    if (!set) {
        return false;
    }
    clear();
    for (auto* o : set->objects()) {
        if (o != this && !select(*static_cast<Control*>(o), true)) {
            return false;
        }
    }
    return true;
}

bool EditControl::selectBox(RectI box, bool add) {
    auto* set = currentAddSet();
    if (!set || box.left > box.right || box.top > box.bottom) {
        return false;
    }
    if (!add) {
        clear();
    }
    for (auto* o : set->objects()) {
        auto* c = static_cast<Control*>(o);
        if (c == this) {
            continue;
        }
        const auto p = c->localToGlobal({}), e = c->fields().extent;
        if (p.x >= box.left && p.y >= box.top && p.x + e.x <= box.right &&
            p.y + e.y <= box.bottom) {
            if (!select(*c, true)) {
                return false;
            }
        }
    }
    return true;
}

bool EditControl::bringToFront() {
    auto controls = selection();
    if (controls.size() != 1 || !controls.front()->parent()) {
        return false;
    }
    auto* set = controls.front()->parent();
    const bool result = set->reOrder(*controls.front());
    if (set == parent()) {
        set->reOrder(*this);
    }
    if (canvas()) {
        canvas()->rebuildTabs();
    }
    return result;
}

bool EditControl::sendToBack() {
    auto controls = selection();
    if (controls.size() != 1 || !controls.front()->parent()) {
        return false;
    }
    auto* set = controls.front()->parent();
    const bool result = set->reOrder(*controls.front(), set->objects().front());
    if (canvas()) {
        canvas()->rebuildTabs();
    }
    return result;
}

bool EditControl::erase() {
    auto controls = selection();
    if (controls.empty()) {
        return false;
    }
    for (auto* c : controls) {
        if (!c->parent() || (c->getManager() && c->getManager()->isDispatching())) {
            return false;
        }
    }
    if (destroySelection) {
        if (!destroySelection(controls)) {
            return false;
        }
        clear();
        return true;
    }
    clear();
    for (auto* c : controls) {
        if (c->getManager()) {
            if (!c->deleteObject()) {
                return false;
            }
        } else {
            std::vector<Control*> subtree;
            const auto collect = [&](auto&& self, Control& item) -> void {
                subtree.push_back(&item);
                for (auto* child : item.objects()) {
                    self(self, *static_cast<Control*>(child));
                }
            };
            collect(collect, *c);
            if (!c->parent()->removeObject(*c)) {
                return false;
            }
            std::erase_if(pasted_, [&](const auto& object) {
                return std::ranges::find(subtree, object.get()) != subtree.end();
            });
        }
    }
    return true;
}

bool EditControl::loadSelection(StreamIO& stream,
                                const Persistent::ClassRegistry& registry,
                                const std::function<bool(Control&)>& bind) {
    auto* set = currentAddSet();
    UInt32 count{};
    if (!set || !stream.read(count) || count == 0 || count > 4096) {
        return false;
    }
    Persistent::LoadContext load(registry);
    std::vector<Control*> roots;
    roots.reserve(count);
    for ([[maybe_unused]] auto i :
         std::views::iota(UInt32{0}) | std::views::take_while([&](UInt32 i) {
             return i < count;
         })) {
        const auto result = load.load(stream);
        auto* c = result ? dynamic_cast<Control*>(result.object) : nullptr;
        if (!c || c->parent()) {
            return false;
        }
        try {
            if (bind && !bind(*c)) {
                return false;
            }
        } catch (...) {
            return false;
        }
        roots.push_back(c);
    }
    // Decode and bind the complete file before publishing any root. Hierarchy
    // admission failures detach earlier roots while LoadContext still owns them.
    auto objects = load.takeObjects();
    Record selected;
    selected.reserve(roots.size());
    for (auto* c : roots) {
        selected.push_back({c, c->lifetime(), c->fields(), {}});
    }
    std::vector<Control*> attached;
    attached.reserve(roots.size());
    pasted_.reserve(pasted_.size() + objects.size());
    const auto detach = [&] {
        for (auto* c : roots) {
            if (c->parent() == set) {
                set->removeObject(*c);
            }
        }
    };
    try {
        for (auto* c : roots) {
            if (!set->addObject(*c)) {
                detach();
                return false;
            }
            attached.push_back(c);
        }
    } catch (...) {
        detach();
        return false;
    }
    if (auto* manager = set->getManager()) {
        // Adoption owns every decoded descendant; the manager preserves their
        // already established exclusive group links and rolls back failed onAdd.
        for (auto* c : attached) {
            set->removeObject(*c);
        }
        std::vector<std::unique_ptr<sim::SimObject>> simObjects;
        simObjects.reserve(objects.size());
        for (auto& o : objects) {
            auto* p = dynamic_cast<sim::SimObject*>(o.get());
            if (!p) {
                return false;
            }
            simObjects.emplace_back(p);
            o.release();
        }
        try {
            if (!manager->adoptObjects(std::move(simObjects), *roots.front())) {
                return false;
            }
        } catch (...) {
            return false;
        }
        try {
            for (auto* c : roots) {
                if (!set->addObject(*c)) {
                    for (auto* root : roots) {
                        manager->deleteObject(*root);
                    }
                    return false;
                }
            }
        } catch (...) {
            for (auto* root : roots) {
                manager->deleteObject(*root);
            }
            return false;
        }
    } else {
        for (auto& o : objects) {
            pasted_.push_back(std::move(o));
        }
    }
    selected_ = std::move(selected);
    addSet_ = set;
    addSetLifetime_ = set->lifetime();
    return true;
}

bool EditControl::live(const Item& item) const {
    if (item.lifetime.expired() || !parent() || item.control == this) {
        return false;
    }
    for (auto* c = item.control; c; c = c->parent()) {
        if (c == parent()) {
            return true;
        }
    }
    return false;
}

std::vector<Control*> EditControl::selection() const {
    std::vector<Control*> out;
    for (const auto& item : selected_) {
        if (live(item)) {
            out.push_back(item.control);
        }
    }
    return out;
}

bool EditControl::select(Control& c, bool add) {
    Item item{&c, c.lifetime(), c.fields(), {}};
    if (!live(item) || !c.parent() || &c == parent()) {
        return false;
    }
    if (!add || currentAddSet() != c.parent()) {
        selected_.clear();
    }
    addSet_ = c.parent();
    addSetLifetime_ = addSet_->lifetime();
    if (std::ranges::none_of(selected_, [&](const Item& old) {
            return old.control == &c && !old.lifetime.expired();
        })) {
        selected_.push_back(std::move(item));
    }
    return true;
}

void EditControl::clear() {
    selected_.clear();
}

EditControl::Item EditControl::snapshot(Control& c) const {
    Item item{&c, c.lifetime(), c.fields(), {}};
    const auto visit = [&](auto&& self, Control& p) -> void {
        for (auto* o : p.objects()) {
            auto* child = static_cast<Control*>(o);
            if (child == this) {
                continue;
            }
            item.descendants.push_back({child, child->lifetime(), child->fields(), {}});
            self(self, *child);
        }
    };
    visit(visit, c);
    return item;
}

EditControl::Record EditControl::capture() const {
    Record out;
    for (const auto& item : selected_) {
        if (live(item)) {
            out.push_back(snapshot(*item.control));
        }
    }
    return out;
}

bool EditControl::apply(const Record& record, bool exact) {
    if (record.empty() || std::ranges::any_of(record, [&](const Item& item) {
            return !live(item);
        })) {
        return false;
    }
    std::vector<std::function<void()>> restore;
    const auto save = [&](auto&& self, Control& c) -> void {
        restore.push_back(c.checkpoint());
        for (auto* o : c.objects()) {
            self(self, *static_cast<Control*>(o));
        }
    };
    for (const auto& item : record) {
        save(save, *item.control);
    }
    for (const auto& item : record) {
        if (std::ranges::any_of(item.descendants, [&](const Item& child) {
                return !live(child);
            })) {
            return false;
        }
    }
    for (const auto& item : record) {
        bool accepted = item.control->configure(item.fields);
        if (accepted && exact) {
            for (const auto& child : item.descendants) {
                if (!child.control->configure(child.fields)) {
                    accepted = false;
                    break;
                }
            }
        }
        if (!accepted) {
            for (auto& undo : restore) {
                undo();
            }
            return false;
        }
    }
    return true;
}

bool EditControl::remember(Record record) {
    if (record.empty()) {
        return false;
    }
    undo_.push_back(std::move(record));
    if (undo_.size() > 128) {
        undo_.erase(undo_.begin());
    }
    redo_.clear();
    return true;
}

bool EditControl::properties(Control& c, Fields fields) {
    auto before = snapshot(c);
    if (!live(before)) {
        return false;
    }
    Record record;
    record.push_back(std::move(before));
    undo_.reserve(undo_.size() + 1);
    auto next = record;
    next.front().fields = std::move(fields);
    return apply(next) && remember(std::move(record));
}

bool EditControl::move(Point2I delta, bool useGrid) {
    if (grid.x < 1 || grid.y < 1 || grid.x > 32768 || grid.y > 32768) {
        return false;
    }
    auto before = capture(), next = before;
    if (snap && useGrid && !next.empty()) {
        Point2I min = next.front().fields.position;
        for (const auto& item : next) {
            min.x = std::min(min.x, item.fields.position.x);
            min.y = std::min(min.y, item.fields.position.y);
        }
        const auto x = Int64(min.x) + delta.x, y = Int64(min.y) + delta.y;
        if (std::abs(x) > MaxCoordinate || std::abs(y) > MaxCoordinate) {
            return false;
        }
        delta.x -= Int32(x) % grid.x;
        delta.y -= Int32(y) % grid.y;
    }
    for (auto& item : next) {
        auto& p = item.fields.position;
        const auto x = Int64(p.x) + delta.x, y = Int64(p.y) + delta.y;
        if (std::abs(x) > MaxCoordinate || std::abs(y) > MaxCoordinate) {
            return false;
        }
        p = {Int32(x), Int32(y)};
    }
    return apply(next) && remember(std::move(before));
}

bool EditControl::size(Point2I delta) {
    auto before = capture(), next = before;
    for (auto& item : next) {
        auto& e = item.fields.extent;
        const auto x = Int64(e.x) + delta.x, y = Int64(e.y) + delta.y;
        if (x < 1 || y < 1 || x > 32768 || y > 32768) {
            return false;
        }
        e = {Int32(x), Int32(y)};
    }
    return apply(next) && remember(std::move(before));
}

bool EditControl::align(Align mode) {
    auto before = capture(), next = before;
    if (next.size() < 2) {
        return false;
    }
    auto left = next.front().fields.position.x, top = next.front().fields.position.y,
         right = left + next.front().fields.extent.x, bottom = top + next.front().fields.extent.y;
    for (const auto& item : next) {
        const auto& f = item.fields;
        left = std::min(left, f.position.x);
        top = std::min(top, f.position.y);
        right = std::max(right, f.position.x + f.extent.x);
        bottom = std::max(bottom, f.position.y + f.extent.y);
    }
    if (mode == Align::HorizontalSpacing || mode == Align::VerticalSpacing) {
        const bool horizontal = mode == Align::HorizontalSpacing;
        std::stable_sort(next.begin(), next.end(), [&](const Item& a, const Item& b) {
            return horizontal ? a.fields.position.x < b.fields.position.x
                              : a.fields.position.y < b.fields.position.y;
        });
        Int32 total = 0;
        for (const auto& item : next) {
            total += horizontal ? item.fields.extent.x : item.fields.extent.y;
        }
        const auto space =
            ((horizontal ? right - left : bottom - top) - total) / Int32(next.size() - 1);
        Int32 position = horizontal ? left : top;
        for (auto& item : next) {
            if (horizontal) {
                item.fields.position.x = position;
                position += item.fields.extent.x + space;
            } else {
                item.fields.position.y = position;
                position += item.fields.extent.y + space;
            }
        }
    } else {
        for (auto& item : next) {
            auto& f = item.fields;
            switch (mode) {
                case Align::Left:
                    f.position.x = left;
                    break;
                case Align::Right:
                    f.position.x = right - f.extent.x;
                    break;
                case Align::Top:
                    f.position.y = top;
                    break;
                case Align::Bottom:
                    f.position.y = bottom - f.extent.y;
                    break;
                case Align::HorizontalCenter:
                    f.position.x = left + ((right - left - f.extent.x) >> 1);
                    break;
                case Align::VerticalCenter:
                    f.position.y = top + ((bottom - top - f.extent.y) >> 1);
                    break;
                default:
                    break;
            }
        }
    }
    return apply(next) && remember(std::move(before));
}

bool EditControl::undo() {
    if (undo_.empty()) {
        return false;
    }
    Record current;
    for (const auto& item : undo_.back()) {
        if (!live(item)) {
            return false;
        }
        current.push_back(snapshot(*item.control));
    }
    redo_.reserve(redo_.size() + 1);
    if (!apply(undo_.back(), true)) {
        return false;
    }
    redo_.push_back(std::move(current));
    undo_.pop_back();
    return true;
}

bool EditControl::redo() {
    if (redo_.empty()) {
        return false;
    }
    Record current;
    for (const auto& item : redo_.back()) {
        if (!live(item)) {
            return false;
        }
        current.push_back(snapshot(*item.control));
    }
    undo_.reserve(undo_.size() + 1);
    if (!apply(redo_.back(), true)) {
        return false;
    }
    undo_.push_back(std::move(current));
    redo_.pop_back();
    return true;
}

bool EditControl::event(const Event& e) {
    if (e.type == EventType::Cancel) {
        if (dragging_ && !boxSelecting_) {
            apply(dragBefore_, true);
        }
        dragging_ = boxSelecting_ = false;
        dragBefore_.clear();
        return true;
    }
    if (e.type == EventType::KeyDown) {
        if (e.control) {
            switch (e.key) {
                case Key::Z:
                    return e.shift ? redo() : undo();
                case Key::A:
                    return selectAll();
                case Key::C:
                    return copySelection && copySelection(false);
                case Key::X:
                    return copySelection && copySelection(true);
                case Key::V:
                    return pasteSelection && pasteSelection();
                default:
                    break;
            }
        }
        const auto delta = e.shift ? 10 : 1;
        switch (e.key) {
            case Key::Left:
                return move({-delta, 0}, false);
            case Key::Right:
                return move({delta, 0}, false);
            case Key::Up:
                return move({0, -delta}, false);
            case Key::Down:
                return move({0, delta}, false);
            case Key::Delete:
            case Key::Backspace:
                return erase();
            case Key::F1:
                return align(Align::Left);
            case Key::F2:
                return align(Align::HorizontalCenter);
            case Key::F3:
                return align(Align::Right);
            case Key::F4:
                return align(Align::Top);
            case Key::F5:
                return align(Align::Bottom);
            case Key::F6:
                return align(Align::VerticalSpacing);
            case Key::F7:
                return align(Align::HorizontalSpacing);
            case Key::F9:
                return bringToFront();
            case Key::F10:
                return sendToBack();
            default:
                break;
        }
    }
    if (e.type == EventType::Down && e.button == 3 && parent()) {
        for (auto* o : parent()->objects() | std::views::reverse) {
            auto* c = static_cast<Control*>(o);
            if (c == this || !c->visible()) {
                continue;
            }
            const auto p = c->globalToLocal(e.point);
            if (c->pointInControl(p)) {
                return addSet(*c->hit(p));
            }
        }
        return addSet(*parent());
    }
    if (e.type == EventType::Down && e.button == 1 && parent()) {
        sizing_ = 0;
        const auto controls = selection();
        if (controls.size() == 1) {
            const auto* c = controls.front();
            const auto p = c->localToGlobal({}), ext = c->fields().extent;
            sizing_ = sizingKnobs(e.point, {p.x, p.y, p.x + ext.x, p.y + ext.y});
        }
        Control* target = parent();
        selectingOnly_ = false;
        for (auto* o : parent()->objects() | std::views::reverse) {
            auto* c = static_cast<Control*>(o);
            if (c == this || !c->visible()) {
                continue;
            }
            const auto p = c->globalToLocal(e.point), ext = c->fields().extent;
            if (p.x >= 0 && p.y >= 0 && p.x < ext.x && p.y < ext.y) {
                target = c->hit(p);
                break;
            }
        }
        if (!canvas() || !canvas()->makeFirstResponder(this) || !canvas()->lockMouse(this)) {
            return false;
        }
        boxSelecting_ = !sizing_ && (!target || target == currentAddSet());
        if (boxSelecting_) {
            if (!e.shift) {
                clear();
            }
        } else if (!sizing_) {
            const auto found = std::ranges::find(controls, target);
            if (e.shift && found != controls.end()) {
                std::erase_if(selected_, [&](const Item& i) {
                    return i.control == target;
                });
                selectingOnly_ = true;
            } else if (found == controls.end()) {
                selectingOnly_ = e.shift && target->parent() == currentAddSet();
                if (target == parent()) {
                    clear();
                    addSet(*parent());
                    selectingOnly_ = true;
                } else if (!select(*target, e.shift)) {
                    return false;
                }
            }
        }
        mouse_ = last_ = e.point;
        dragBefore_ = capture();
        dragging_ = true;
        return true;
    }
    if (e.type == EventType::Drag && e.button == 1 && dragging_) {
        last_ = e.point;
        if (boxSelecting_) {
            return true;
        }
        if (selectingOnly_) {
            return true;
        }
        if (grid.x < 1 || grid.y < 1 || grid.x > 32768 || grid.y > 32768) {
            return false;
        }
        const auto delta = e.point - mouse_;
        auto next = dragBefore_;
        auto movement = delta;
        if (snap && !next.empty() && !sizing_) {
            Point2I minimum = next.front().fields.position;
            for (const auto& item : next) {
                minimum.x = std::min(minimum.x, item.fields.position.x);
                minimum.y = std::min(minimum.y, item.fields.position.y);
            }
            movement.x -= (minimum.x + movement.x) % grid.x;
            movement.y -= (minimum.y + movement.y) % grid.y;
        }
        for (auto& item : next) {
            auto& f = item.fields;
            if (sizing_) {
                auto point = e.point;
                if (snap) {
                    point.x -= point.x % grid.x;
                    point.y -= point.y % grid.y;
                }
                point = item.control->parent()->globalToLocal(point);
                const auto right = f.position.x + f.extent.x, bottom = f.position.y + f.extent.y;
                const auto minimum = item.control->minimumExtent();
                if (sizing_ & 1) {
                    f.position.x = std::min(point.x, right - minimum.x);
                    f.extent.x = right - f.position.x;
                }
                if (sizing_ & 2) {
                    f.extent.x = std::max(minimum.x, point.x - f.position.x);
                }
                if (sizing_ & 4) {
                    f.position.y = std::min(point.y, bottom - minimum.y);
                    f.extent.y = bottom - f.position.y;
                }
                if (sizing_ & 8) {
                    f.extent.y = std::max(minimum.y, point.y - f.position.y);
                }
            } else {
                const auto x = Int64(f.position.x) + movement.x,
                           y = Int64(f.position.y) + movement.y;
                if (std::abs(x) > MaxCoordinate || std::abs(y) > MaxCoordinate) {
                    return false;
                }
                f.position = {Int32(x), Int32(y)};
            }
        }
        return apply(next);
    }
    if (e.type == EventType::Up && e.button == 1 && dragging_ && canvas()) {
        canvas()->unlockMouse();
        dragging_ = false;
        if (boxSelecting_) {
            boxSelecting_ = false;
            selectBox({std::min(mouse_.x, e.point.x),
                       std::min(mouse_.y, e.point.y),
                       std::max(mouse_.x, e.point.x),
                       std::max(mouse_.y, e.point.y)},
                      true);
        } else if (!selectingOnly_) {
            remember(std::move(dragBefore_));
        }
        if (inspect) {
            const auto controls = selection();
            return inspect(controls.size() == 1 ? controls.front()
                           : controls.empty()   ? currentAddSet()
                                                : nullptr);
        }
        return true;
    }
    return Control::event(e);
}

void EditControl::close() {
    if (dragging_ && !boxSelecting_) {
        apply(dragBefore_, true);
    }
    dragging_ = boxSelecting_ = false;
    dragBefore_.clear();
}

bool EditControl::draw(gfx::SoftwareSurface& s, Point2I, RectI) const {
    if (const auto* set = currentAddSet()) {
        const auto p = set->localToGlobal({}), e = set->fields().extent;
        s.rectangle({p.x + 1, p.y + 1, p.x + e.x - 2, p.y + e.y - 2}, 255);
        s.rectangle({p.x, p.y, p.x + e.x - 1, p.y + e.y - 1}, 0);
    }
    const auto controls = selection();
    const bool multi = controls.size() > 1;
    for (auto* c : controls) {
        const auto p = c->localToGlobal({}), e = c->fields().extent;
        const auto nut = [&](Point2I q) {
            s.rectangle({q.x - 3, q.y - 3, q.x + 3, q.y + 3}, multi ? 0 : 255);
            s.fill({q.x - 2, q.y - 2, q.x + 2, q.y + 2}, multi ? 255 : 0);
        };
        for (auto q : std::array{p,
                                 Point2I{p.x, p.y + e.y / 2},
                                 Point2I{p.x, p.y + e.y},
                                 Point2I{p.x + e.x, p.y},
                                 Point2I{p.x + e.x, p.y + e.y / 2},
                                 p + e,
                                 Point2I{p.x + e.x / 2, p.y},
                                 Point2I{p.x + e.x / 2, p.y + e.y}}) {
            nut(q);
        }
    }
    if (boxSelecting_) {
        s.rectangle({std::min(mouse_.x, last_.x),
                     std::min(mouse_.y, last_.y),
                     std::max(mouse_.x, last_.x),
                     std::max(mouse_.y, last_.y)},
                    255);
    }
    return true;
}

std::function<void()> EditControl::checkpoint() {
    auto base = Control::checkpoint();
    auto s = selected_;
    auto u = undo_, r = redo_;
    auto d = dragBefore_;
    const auto dragging = dragging_, box = boxSelecting_, selecting = selectingOnly_;
    const auto sizing = sizing_;
    const auto m = mouse_, last = last_;
    auto* set = addSet_;
    auto lifetime = addSetLifetime_;
    return [this,
            base = std::move(base),
            s = std::move(s),
            u = std::move(u),
            r = std::move(r),
            d = std::move(d),
            dragging,
            sizing,
            box,
            selecting,
            m,
            last,
            set,
            lifetime] {
        base();
        selected_ = s;
        undo_ = u;
        redo_ = r;
        dragBefore_ = d;
        dragging_ = dragging;
        sizing_ = sizing;
        boxSelecting_ = box;
        selectingOnly_ = selecting;
        mouse_ = m;
        last_ = last;
        addSet_ = set;
        addSetLifetime_ = lifetime;
    };
}

bool EditControl::saveSelection(StreamIO& s, const Persistent::ClassRegistry& registry) const {
    const auto controls = selection();
    if (controls.empty() || !s.write(UInt32(controls.size()))) {
        return false;
    }
    Persistent::StoreContext store(registry);
    for (const auto* c : controls) {
        if (store.store(s, *c) != Persistent::Error::Ok) {
            return false;
        }
    }
    return true;
}
} // namespace opent1::gui
