#include <opent1/game/fear_mission_editor.hpp>
#include <ranges>
#include <opent1/game/string_table.hpp>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <format>
#include <map>
#include <opent1/core/text.hpp>
#include <opent1/sim/sim_palette.hpp>
#include <set>
#include <opent1/game/game_base.hpp>

namespace opent1::game {

namespace {

[[nodiscard]] Flt32 parseFloatVar(std::string_view sv, Flt32 fallback = 0.0f) noexcept {
    if (sv.empty()) {
        return fallback;
    }
    Flt32 val = fallback;
    const auto result = std::from_chars(sv.data(), sv.data() + sv.size(), val);
    return result.ec == std::errc{} && result.ptr == sv.data() + sv.size() && std::isfinite(val)
               ? val
               : fallback;
}

bool selectedAncestor(const FearMissionEditor::SelectionSet& selection,
                      const sim::SimObject& object) {
    std::set<const sim::SimObject*> seen;
    for (auto* group = object.getGroup(); group && seen.insert(group).second;
         group = group->getGroup()) {
        if (std::ranges::find(selection.objects(), group) != selection.objects().end())
            return true;
    }
    return false;
}

std::string quoteScript(std::string_view value) {
    std::string result{"\""};
    for (char c : value) {
        switch (c) {
            case '\\':
                result += "\\\\";
                break;
            case '"':
                result += "\\\"";
                break;
            case '\r':
                result += "\\r";
                break;
            case '\n':
                result += "\\n";
                break;
            case '\t':
                result += "\\t";
                break;
            default:
                result += c;
                break;
        }
    }
    return result + '"';
}

bool fieldIdentifier(std::string_view name) {
    if (name.empty() || (name.front() >= '0' && name.front() <= '9')) {
        return false;
    }
    return std::ranges::all_of(name, [](unsigned char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
               c == '_' || c == ':';
    });
}

// Native mission scripts describe a tree of instant declarations. Snapshot
// current transforms as well as dynamic/mod fields, rather than stale initial
// position strings. Refuse unsupported names/classes before touching a file.
bool appendObjectScript(sim::SimGame& game,
                        const sim::SimObject& object,
                        std::string& out,
                        std::set<const sim::SimObject*>& seen,
                        unsigned depth) {
    if (depth > 64 || !seen.insert(&object).second) {
        return false;
    }
    const auto* descriptor = game.classes().find(typeid(object));
    // The game factory supplies a palette subclass with a render callback. Its
    // on-disk identity and fields remain those of the native SimPalette.
    if (!descriptor && dynamic_cast<const sim::SimPalette*>(&object)) {
        descriptor = game.classes().find("SimPalette");
    }
    if (!descriptor || !fieldIdentifier(descriptor->name)) {
        return false;
    }
    std::map<std::string, std::string> fields(object.dynamicFields().begin(),
                                              object.dynamicFields().end());
    std::vector<std::pair<std::string, std::string>> arrays;
    for (const auto& field : descriptor->fields) {
        if (!fieldIdentifier(field.name)) {
            return false;
        }
        for (auto i : std::views::iota(std::size_t{0}) | std::views::take_while([&](std::size_t i) {
                          return i < field.elementCount;
                      })) {
            if (const auto value = field.get(object, i, game.dataFields())) {
                if (field.elementCount == 1) {
                    fields[field.name] = *value;
                } else {
                    arrays.emplace_back(std::format("{}[{}]", field.name, i), *value);
                }
            }
        }
    }
    sim::SimObjectTransformQuery transform;
    if (const_cast<sim::SimObject&>(object).processQuery(transform)) {
        EulerF rotation;
        transform.transform.angles(rotation);
        const auto& p = transform.transform.p;
        fields["position"] = std::format("{:.9g} {:.9g} {:.9g}", p.x, p.y, p.z);
        fields["rotation"] =
            std::format("{:.9g} {:.9g} {:.9g}", rotation.x, rotation.y, rotation.z);
    }
    if (const auto* base = dynamic_cast<const GameBase*>(&object)) {
        if (!base->getDatFileName().empty()) {
            fields["datablock"] = base->getDatFileName();
        }
        fields["team"] = std::to_string(base->getTeam());
    }
    const std::string indent(depth * 3, ' ');
    const auto* group = dynamic_cast<const sim::SimGroup*>(&object);
    // Native grammar expresses an empty object with a terminating semicolon;
    // an empty declaration block is deliberately not a valid production.
    if (fields.empty() && arrays.empty() && (!group || group->objects().empty())) {
        out +=
            indent + "instant " + descriptor->name + " " + quoteScript(object.getName()) + ";\r\n";
        return out.size() <= game.console().limits().parser.maxSourceBytes;
    }
    out += indent + "instant " + descriptor->name + " " + quoteScript(object.getName()) + " {\r\n";
    for (const auto& [name, value] : fields) {
        if (!fieldIdentifier(name)) {
            return false;
        }
        out += indent + "   " + name + " = " + quoteScript(value) + ";\r\n";
    }
    for (const auto& [name, value] : arrays) {
        out += indent + "   " + name + " = " + quoteScript(value) + ";\r\n";
    }
    if (group) {
        for (const auto* child : group->objects()) {
            if (!appendObjectScript(game, *child, out, seen, depth + 1)) {
                return false;
            }
        }
    }
    out += indent + "};\r\n";
    return out.size() <= game.console().limits().parser.maxSourceBytes;
}

} // namespace

// -----------------------------------------------------------------------------
// FearMissionEditor::SelectionSet
// -----------------------------------------------------------------------------

bool FearMissionEditor::SelectionSet::addObject(sim::SimObject& obj) {
    if (&obj == this) {
        return false;
    }
    for (const auto* member : items_) {
        if (member == &obj) {
            return false;
        }
    }
    obj.setSelected(true);
    items_.push_back(&obj);
    if (getManager() == obj.getManager()) {
        (void)Parent::addObject(obj);
    }
    if (parentEditor) {
        parentEditor->deleteNotify(obj);
    } else {
        deleteNotify(obj);
    }
    calcCentroid();
    return true;
}

bool FearMissionEditor::SelectionSet::removeObject(sim::SimObject& obj) {
    obj.setSelected(false);
    auto it = std::find(items_.begin(), items_.end(), &obj);
    const bool found = (it != items_.end());
    if (found) {
        items_.erase(it);
    }
    if (getManager() == obj.getManager()) {
        (void)Parent::removeObject(obj);
    }
    if (parentEditor) {
        if (std::ranges::none_of(parentEditor->undoEntries(), [&](const InfoEntry& entry) {
                return entry.object == &obj;
            })) {
            parentEditor->clearNotify(obj);
        }
    } else {
        clearNotify(obj);
    }
    calcCentroid();
    return found;
}

void FearMissionEditor::SelectionSet::clearSet() {
    auto copy = items_;
    for (auto* obj : copy) {
        if (obj) {
            removeObject(*obj);
        }
    }
    items_.clear();
    centroid_.set(0.0f, 0.0f, 0.0f);
}

void FearMissionEditor::SelectionSet::calcCentroid() {
    Point3F accum{0.0f, 0.0f, 0.0f};
    Int32 numTransformObjs = 0;

    std::set<const sim::SimObject*> seen;
    const auto visit = [&](const auto& self, sim::SimObject* obj) -> void {
        if (!obj || !seen.insert(obj).second) {
            return;
        }
        sim::SimObjectTransformQuery query{};
        if (obj->processQuery(query)) {
            accum += query.transform.p;
            ++numTransformObjs;
        } else if (auto* group = dynamic_cast<sim::SimGroup*>(obj)) {
            for (auto* child : group->objects()) {
                self(self, child);
            }
        }
    };
    for (auto* obj : objects()) {
        visit(visit, obj);
    }

    if (numTransformObjs > 0) {
        accum /= static_cast<Flt32>(numTransformObjs);
    }
    centroid_ = accum;
}

bool FearMissionEditor::SelectionSet::objectLocked() const {
    for (const auto* obj : objects()) {
        if (obj && obj->isLocked()) {
            return true;
        }
    }
    return false;
}

bool FearMissionEditor::SelectionSet::processEvent(const sim::SimEvent& event) {
    if (const auto* xformEv = dynamic_cast<const sim::SimObjectTransformEvent*>(&event)) {
        if (objectLocked()) {
            return false;
        }
        if (objects().size() == 1) {
            objects().front()->processEvent(*xformEv);
            calcCentroid();
            return true;
        }
        const Point3F offset = xformEv->transform.p - centroid_;
        for (auto* obj : objects()) {
            sim::SimObjectTransformQuery query{};
            if (obj && obj->processQuery(query)) {
                query.transform.p += offset;
                sim::SimObjectTransformEvent subEv{};
                subEv.transform = query.transform;
                obj->processEvent(subEv);
            }
        }
        calcCentroid();
        return true;
    }
    return Parent::processEvent(event);
}

bool FearMissionEditor::SelectionSet::processQuery(sim::SimQuery& query) {
    if (auto* xq = dynamic_cast<sim::SimObjectTransformQuery*>(&query)) {
        if (objects().size() == 1) {
            return objects().front()->processQuery(query);
        }
        xq->transform.identity();
        xq->transform.p = centroid_;
        return true;
    }
    return Parent::processQuery(query);
}

// -----------------------------------------------------------------------------
// FearMissionEditor
// -----------------------------------------------------------------------------

FearMissionEditor::FearMissionEditor() {
    selection_.parentEditor = this;
}

FearMissionEditor::~FearMissionEditor() = default;

bool FearMissionEditor::onAdd() {
    if (!Parent::onAdd()) {
        return false;
    }
    targetManager_ = getManager();
    return true;
}

void FearMissionEditor::onRemove() {
    if (focused_ && targetManager()) {
        sim::SimLoseFocusEvent event;
        processEvent(event);
    }
    selection_.clearSet();
    undoBuffer_.clear();
    targetManager_ = nullptr;
    teardownObject_ = nullptr;
    Parent::onRemove();
}

void FearMissionEditor::onDeleteNotify(sim::SimObject& object) {
    if (&object == teardownObject_) {
        teardownObject_ = nullptr;
        selection_.clearSet();
        undoBuffer_.clear();
        targetManager_ = nullptr;
        deleteObject();
        return;
    }
    selection_.removeObject(object);
    for (auto& entry : undoBuffer_) {
        if (entry.object == &object) {
            entry.object = nullptr;
        }
    }
    Parent::onDeleteNotify(object);
}

bool FearMissionEditor::init() {
    getConsoleOptions();
    return true;
}

bool FearMissionEditor::processEvent(const sim::SimEvent& event) {
    const bool gain = dynamic_cast<const sim::SimGainFocusEvent*>(&event) != nullptr;
    const bool lose = dynamic_cast<const sim::SimLoseFocusEvent*>(&event) != nullptr;
    if (!gain && !lose)
        return Parent::processEvent(event);
    auto* manager = targetManager();
    if (!manager)
        return false;
    focused_ = gain;
    sim::SimEditEvent edit;
    edit.state = focused_;
    std::vector<sim::SimAddress> objects;
    std::set<sim::SimObject*> seen;
    const auto collect = [&](auto&& self, sim::SimObject* object) -> void {
        if (!object || !seen.insert(object).second)
            return;
        objects.push_back(object->address());
        if (auto* group = dynamic_cast<sim::SimGroup*>(object)) {
            for (auto* child : group->objects())
                self(self, child);
        }
    };
    collect(collect, manager->findObject("MissionGroup"));
    for (const auto& address : objects)
        manager->sendEvent(address, edit);
    return true;
}

void FearMissionEditor::setTargetManager(sim::SimManager* manager) {
    selection_.clearSet();
    undoBuffer_.clear();
    if (teardownObject_) {
        clearNotify(*teardownObject_);
    }
    targetManager_ = manager;
    serverTarget_ = game_ && game_->world(sim::WorldType::Server) &&
                    &game_->world(sim::WorldType::Server)->manager() == manager;
    targetRevision_ = game_ ? game_->worldRevision(sim::WorldType::Server) : 0;
    teardownObject_ = manager ? manager->findObject("ConsoleScheduler") : nullptr;
    if (!teardownObject_ && manager != getManager())
        teardownObject_ = manager;
    if (teardownObject_) {
        deleteNotify(*teardownObject_);
    }
}

sim::SimManager* FearMissionEditor::targetManager() const {
    if (serverTarget_ &&
        (!game_ || game_->worldRevision(sim::WorldType::Server) != targetRevision_ ||
         !game_->world(sim::WorldType::Server))) {
        return nullptr;
    }
    return targetManager_;
}

EditCamera* FearMissionEditor::camera() const {
    return getManager() ? dynamic_cast<EditCamera*>(getManager()->findObject("editCamera"))
                        : nullptr;
}

sim::SimGroup* FearMissionEditor::getAddGroup() const {
    if (!targetManager()) {
        return nullptr;
    }
    for (auto* obj : selection_.objects()) {
        if (auto* grp = dynamic_cast<sim::SimGroup*>(obj)) {
            return grp;
        }
    }
    if (auto* mg = dynamic_cast<sim::SimGroup*>(targetManager()->findObject("MissionGroup"))) {
        return mg;
    }
    return targetManager();
}

sim::SimObject* FearMissionEditor::addObject(sim::SimObject* obj) {
    if (!obj || !targetManager()) {
        return nullptr;
    }
    if (auto* grp = getAddGroup()) {
        if (!grp->addObject(*obj)) {
            return nullptr;
        }
    }
    sim::SimEditEvent editEv{};
    editEv.editor = sim::MissionEditorId;
    editEv.state = flags_.test(ShowEditObjects);
    obj->processEvent(editEv);
    return obj;
}

sim::SimObject* FearMissionEditor::getObject(UInt32 id) const {
    if (!targetManager()) {
        return nullptr;
    }
    return targetManager()->findObject(id);
}

bool FearMissionEditor::registerType(UInt32 mask,
                                     std::string_view description,
                                     std::string_view lockedBmp,
                                     std::string_view unlockedBmp) {
    if (mask == 0 || description.empty()) {
        return false;
    }
    for (const auto& ti : typeInfos_) {
        if (ti.mask == mask) {
            return false;
        }
    }
    TypeInfo info{};
    info.mask = mask;
    info.description.assign(description.data(), description.size());
    info.lockedBmpName.assign(lockedBmp.data(), lockedBmp.size());
    info.unlockedBmpName.assign(unlockedBmp.data(), unlockedBmp.size());
    typeInfos_.push_back(std::move(info));
    return true;
}

bool FearMissionEditor::registerClassType(std::string_view className,
                                          UInt32 mask,
                                          std::string_view selectedBmp,
                                          std::string_view unselectedBmp) {
    if (className.empty() || std::ranges::any_of(typeInfos_, [&](const TypeInfo& info) {
            return equalIgnoreCase(info.className, className);
        })) {
        return false;
    }
    TypeInfo info;
    info.className = className;
    info.mask = mask;
    info.lockedBmpName = selectedBmp;
    info.unlockedBmpName = unselectedBmp;
    typeInfos_.push_back(std::move(info));
    return true;
}

bool FearMissionEditor::setClassPlaceMask(std::string_view className, std::optional<UInt32> mask) {
    const auto it = std::ranges::find_if(typeInfos_, [&](const TypeInfo& info) {
        return equalIgnoreCase(info.className, className);
    });
    if (it == typeInfos_.end()) {
        return !mask;
    }
    if (mask) {
        it->mask = *mask;
    } else {
        typeInfos_.erase(it);
    }
    return true;
}

const FearMissionEditor::TypeInfo* FearMissionEditor::getTypeInfo(sim::SimObject* obj) const {
    if (!obj) {
        return nullptr;
    }
    const UInt32 objType = obj->getType();
    if (game_) {
        const auto* descriptor = game_->classes().find(typeid(*obj));
        if (descriptor) {
            for (const auto& ti : typeInfos_) {
                if (equalIgnoreCase(ti.className, descriptor->name)) {
                    return &ti;
                }
            }
        }
    }
    for (const auto& ti : typeInfos_) {
        if (ti.className.empty() && (ti.mask & objType) != 0) {
            return &ti;
        }
    }
    return nullptr;
}

UInt32 FearMissionEditor::placementMask(sim::SimObject* object) const {
    const auto* info = getTypeInfo(object);
    return info ? info->mask : defaultPlaceMask_;
}

void FearMissionEditor::saveInfo(sim::SimObject* object) {
    for (const auto& entry : undoBuffer_) {
        if (entry.object &&
            std::ranges::find(selection_.objects(), entry.object) == selection_.objects().end()) {
            clearNotify(*entry.object);
        }
    }
    undoBuffer_.clear();
    std::set<sim::SimObject*> seen;
    const auto visit = [&](const auto& self, sim::SimObject* obj) -> void {
        if (!obj || !seen.insert(obj).second) {
            return;
        }
        sim::SimObjectTransformQuery query{};
        if (obj->processQuery(query)) {
            undoBuffer_.push_back(InfoEntry{obj->getId(), obj, query.transform, obj->getNumber()});
            deleteNotify(*obj);
        } else if (auto* group = dynamic_cast<sim::SimGroup*>(obj)) {
            for (auto* child : group->objects()) {
                self(self, child);
            }
        }
    };
    if (object) {
        visit(visit, object);
    } else {
        for (auto* obj : selection_.objects()) {
            visit(visit, obj);
        }
    }
}

void FearMissionEditor::restoreInfo() {
    const auto entries = std::exchange(undoBuffer_, {});
    for (const auto& entry : entries) {
        // Undo may outlive selection and IDs may have been reused. Resolve in
        // the original world and verify the instance before restoring anything.
        auto* manager = targetManager();
        auto* obj = manager ? manager->findObject(entry.id) : nullptr;
        if (obj != entry.object && getManager()) {
            obj = getManager()->findObject(entry.id);
        }
        if (obj && obj == entry.object && obj->getNumber() == entry.number) {
            sim::SimObjectTransformEvent ev{};
            ev.transform = entry.transform;
            obj->processEvent(ev);
        }
    }
    selection_.calcCentroid();
}

void FearMissionEditor::placeBookmark(Int32 index) {
    if (index < 0 || index >= 10 || !targetManager()) {
        return;
    }
    if (auto* cam = camera()) {
        bookmarks_[static_cast<std::size_t>(index)].valid = true;
        bookmarks_[static_cast<std::size_t>(index)].pos = cam->movement.pos;
        bookmarks_[static_cast<std::size_t>(index)].rot = EulerF{
            cam->rotation.pos.x,
            cam->rotation.pos.y,
            cam->rotation.pos.z,
        };
    }
}

void FearMissionEditor::gotoBookmark(Int32 index) {
    if (index < 0 || index >= 10 || !targetManager()) {
        return;
    }
    const Bookmark& bm = bookmarks_[static_cast<std::size_t>(index)];
    if (!bm.valid) {
        return;
    }
    if (auto* cam = camera()) {
        cam->setPosition(bm.pos, bm.rot);
    }
}

void FearMissionEditor::moveObjectRelative(sim::SimObject* obj, const Point3F& delta) {
    if (!obj) {
        return;
    }
    if (&selection_ == obj) {
        if (selection_.objectLocked()) {
            return;
        }
        Point3F movement = delta;
        const bool snap = flags_.test(SnapToGrid);
        if (snap) {
            for (std::size_t axis = 0; axis < 3; ++axis) {
                if (granularity_[axis] > 0) {
                    const auto current = selection_.centroid()[axis];
                    movement[axis] =
                        std::floor((current + movement[axis]) / granularity_[axis] + .5f) *
                            granularity_[axis] -
                        current;
                }
            }
            flags_.clear(SnapToGrid);
        }

        struct RestoreSnap {
            BitSet32& flags;
            bool enabled;

            ~RestoreSnap() {
                flags.set(SnapToGrid, enabled);
            }
        } restore{flags_, snap};

        for (auto* child : selection_.objects()) {
            if (!selectedAncestor(selection_, *child))
                moveObjectRelative(child, movement);
        }
        selection_.calcCentroid();
        return;
    }
    if (obj->isLocked()) {
        return;
    }
    sim::SimObjectTransformQuery query{};
    if (!obj->processQuery(query)) {
        if (auto* group = dynamic_cast<sim::SimGroup*>(obj)) {
            for (auto* child : group->objects()) {
                moveObjectRelative(child, delta);
            }
        }
        return;
    }
    if (!flags_.test(ConstrainX)) {
        query.transform.p.x += delta.x;
    }
    if (!flags_.test(ConstrainY)) {
        query.transform.p.y += delta.y;
    }
    if (!flags_.test(ConstrainZ)) {
        query.transform.p.z += delta.z;
    }
    if (flags_.test(SnapToGrid)) {
        if (!flags_.test(ConstrainX) && granularity_.x > 0.0f) {
            query.transform.p.x =
                std::floor((query.transform.p.x / granularity_.x) + 0.5f) * granularity_.x;
        }
        if (!flags_.test(ConstrainY) && granularity_.y > 0.0f) {
            query.transform.p.y =
                std::floor((query.transform.p.y / granularity_.y) + 0.5f) * granularity_.y;
        }
        if (!flags_.test(ConstrainZ) && granularity_.z > 0.0f) {
            query.transform.p.z =
                std::floor((query.transform.p.z / granularity_.z) + 0.5f) * granularity_.z;
        }
    }
    sim::SimObjectTransformEvent ev{};
    ev.transform = query.transform;
    obj->processEvent(ev);
    selection_.calcCentroid();
}

void FearMissionEditor::moveObjectAbsolute(sim::SimObject* obj, const Point3F& pos) {
    if (!obj) {
        return;
    }
    if (&selection_ == obj) {
        if (selection_.objectLocked()) {
            return;
        }
        const Point3F delta = pos - selection_.centroid();
        for (auto* child : selection_.objects()) {
            sim::SimObjectTransformQuery q{};
            if (child && child->processQuery(q)) {
                moveObjectAbsolute(child, q.transform.p + delta);
            }
        }
        selection_.calcCentroid();
        return;
    }
    if (obj->isLocked()) {
        return;
    }
    sim::SimObjectTransformQuery query{};
    if (!obj->processQuery(query)) {
        return;
    }
    if (!flags_.test(ConstrainX)) {
        query.transform.p.x = pos.x;
    }
    if (!flags_.test(ConstrainY)) {
        query.transform.p.y = pos.y;
    }
    if (!flags_.test(ConstrainZ)) {
        query.transform.p.z = pos.z;
    }
    sim::SimObjectTransformEvent ev{};
    ev.transform = query.transform;
    obj->processEvent(ev);
    selection_.calcCentroid();
}

void FearMissionEditor::rotateObject(sim::SimObject* obj,
                                     const Point3F& anchor,
                                     Flt32 rot,
                                     UInt32 axes) {
    if (!obj) {
        return;
    }
    if (&selection_ == obj) {
        if (selection_.objectLocked()) {
            return;
        }
        for (auto* child : selection_.objects()) {
            if (!selectedAncestor(selection_, *child))
                rotateObject(child, anchor, rot, axes);
        }
        selection_.calcCentroid();
        return;
    }
    if (obj->isLocked()) {
        return;
    }
    sim::SimObjectTransformQuery query{};
    if (!obj->processQuery(query)) {
        if (auto* group = dynamic_cast<sim::SimGroup*>(obj))
            for (auto* child : group->objects())
                rotateObject(child, anchor, rot, axes);
        return;
    }

    TMat3F tmat = query.transform;
    EulerF rotStep{0.0f, 0.0f, 0.0f};
    if (!axes)
        axes = flags_.mask() & (RotateX | RotateY | RotateZ);
    if (axes & RotateX) {
        rotStep.x = rot;
    }
    if (axes & RotateY) {
        rotStep.y = rot;
    }
    if ((axes & RotateZ) || !axes) {
        rotStep.z = rot;
    }

    TMat3F rotMat;
    rotMat.identity();
    rotMat.preRotateX(rotStep.x);
    rotMat.preRotateY(rotStep.y);
    rotMat.preRotateZ(rotStep.z);

    tmat.p -= anchor;
    TMat3F result{};
    m_mul(tmat, rotMat, result);
    result.p += anchor;
    result.flags |= TMat3F::Matrix_HasTranslation | TMat3F::Matrix_HasRotation;

    sim::SimObjectTransformEvent ev{};
    ev.transform = result;
    obj->processEvent(ev);
}

bool FearMissionEditor::dropObjectDown(sim::SimObject* obj) {
    if (!obj || !targetManager()) {
        return false;
    }
    if (&selection_ == obj) {
        bool anyDropped = false;
        for (auto* child : selection_.objects()) {
            if (!selectedAncestor(selection_, *child) && dropObjectDown(child)) {
                anyDropped = true;
            }
        }
        selection_.calcCentroid();
        return anyDropped;
    }
    if (obj->isLocked()) {
        return false;
    }
    sim::SimObjectTransformQuery query{};
    if (!obj->processQuery(query)) {
        bool dropped = false;
        if (auto* group = dynamic_cast<sim::SimGroup*>(obj))
            for (auto* child : group->objects())
                dropped = dropObjectDown(child) || dropped;
        return dropped;
    }
    auto* root = dynamic_cast<sim::SimContainer*>(targetManager()->findObject(SimRootContainerId));
    if (!root) {
        return false;
    }
    sim::SimContainerQuery cq{};
    cq.id = obj->getId();
    cq.type = static_cast<UInt32>(-1);
    cq.mask = placementMask(obj);
    cq.box.fMin = query.transform.p;
    cq.box.fMax = query.transform.p;
    cq.box.fMax.z -= 2000.0f;

    sim::SimCollisionInfo info{};
    if (!root->findLOS(cq, info) || info.surfaces.surfaces.empty()) {
        return false;
    }
    Point3F hitWorld{};
    m_mul(info.surfaces.surfaces[0].position, info.surfaces.tWorld, hitWorld);
    query.transform.p = hitWorld;

    sim::SimObjectTransformEvent ev{};
    ev.transform = query.transform;
    obj->processEvent(ev);
    return true;
}

void FearMissionEditor::onDrop(sim::SimObject* obj, UInt32 dropType) {
    if (!obj || !targetManager()) {
        return;
    }
    if (obj->isLocked()) {
        return;
    }
    if (!dropType) {
        // Vanilla chooses the last enabled drop option in this order.
        dropType = DropToScreenCenter;
        for (const auto flag : {DropAtCamera,
                                DropWithRotAtCamera,
                                DropBelowCamera,
                                DropToScreenCenter,
                                DropToSelectedObject}) {
            if (flags_.test(flag)) {
                dropType = flag;
            }
        }
    }
    if (auto* group = dynamic_cast<sim::SimGroup*>(obj)) {
        for (auto* child : group->objects()) {
            onDrop(child, dropType);
        }
        return;
    }
    auto* cam = camera();
    sim::SimObjectTransformQuery query;
    if (!cam || !obj->processQuery(query)) {
        return;
    }
    auto& transform = query.transform;
    const auto& cameraTransform = cam->getTransform();
    if (dropType == DropWithRotAtCamera) {
        const auto pos = transform.p + cameraTransform.p;
        transform = cameraTransform;
        transform.p = pos;
    } else if (dropType == DropToSelectedObject) {
        transform.p += selection_.centroid();
    } else if (dropType == DropToScreenCenter) {
        Point3F end;
        m_mul(Point3F{0, 200, 0}, cameraTransform, end);
        auto* root =
            dynamic_cast<sim::SimContainer*>(targetManager()->findObject(SimRootContainerId));
        sim::SimContainerQuery ray;
        ray.id = cam->getId();
        ray.type = ~0U;
        ray.mask = placementMask(obj);
        ray.box.fMin = cameraTransform.p;
        ray.box.fMax = end;
        sim::SimCollisionInfo hit;
        if (root && root->findLOS(ray, hit) && !hit.surfaces.surfaces.empty()) {
            m_mul(hit.surfaces.surfaces.front().position, hit.surfaces.tWorld, end);
        }
        transform.p += end;
    } else {
        transform.p += cameraTransform.p;
        if (dropType == DropBelowCamera) {
            transform.p.z -= 15.f;
        }
    }
    transform.flags |= TMat3F::Matrix_HasTranslation;
    sim::SimObjectTransformEvent event;
    event.transform = transform;
    obj->processEvent(event);
    selection_.calcCentroid();
}

void FearMissionEditor::objectsToCamera() {
    if (!targetManager()) {
        return;
    }
    if (auto* cam = camera()) {
        moveObjectAbsolute(&selection_, cam->getPos());
    }
}

void FearMissionEditor::cameraToObjects() {
    if (!targetManager() || selection_.objects().empty()) {
        return;
    }
    if (auto* cam = camera()) {
        selection_.calcCentroid();
        cam->setPos(selection_.centroid());
    }
}

void FearMissionEditor::deleteObjectRecursive(sim::SimObject* obj) {
    if (!obj || obj == &selection_ || obj->isLocked()) {
        return;
    }
    // SimManager deletes a group's children in one protected dispatch. Walking
    // borrowed child pointers here would outlive siblings deleted by callbacks.
    obj->deleteObject();
}

void FearMissionEditor::deleteSelection() {
    if (selection_.objectLocked()) {
        return;
    }
    auto* manager = targetManager();
    if (!manager)
        return;
    std::vector<sim::SimAddress> copy;
    for (auto* object : selection_.objects()) {
        if (!selectedAncestor(selection_, *object))
            copy.push_back(object->address());
    }
    selection_.clearSet();
    for (const auto& address : copy) {
        if (targetManager() != manager)
            break;
        deleteObjectRecursive(address.resolve(*manager));
    }
}

bool FearMissionEditor::copySelection() {
    if (!game_ || selection_.objects().empty()) {
        return false;
    }
    std::string script;
    std::set<const sim::SimObject*> seen;
    for (auto* object : selection_.objects()) {
        bool includedByParent = false;
        for (auto* parent = object->getGroup(); parent; parent = parent->getGroup()) {
            if (std::ranges::find(selection_.objects(), parent) != selection_.objects().end()) {
                includedByParent = true;
                break;
            }
        }
        if (!includedByParent && !appendObjectScript(*game_, *object, script, seen, 1)) {
            return false;
        }
    }
    if (script.empty()) {
        return false;
    }
    selection_.calcCentroid();
    copyPosition_ = selection_.centroid();
    clipboardScript_ = std::move(script);
    return true;
}

bool FearMissionEditor::pasteSelection() {
    return pasteScript(clipboardScript_, false);
}

bool FearMissionEditor::pasteFile(std::string_view file) {
    const auto source = game_ ? game_->console().readScript(file) : std::nullopt;
    return source && pasteScript(*source, true);
}

bool FearMissionEditor::pasteScript(std::string_view source, bool drop) {
    if (!game_ || !targetManager() || source.empty()) {
        return false;
    }
    auto* destination = getAddGroup();
    if (!destination) {
        return false;
    }
    std::string temporaryName = "MissionClipboard";
    while (targetManager()->findObject(temporaryName)) {
        temporaryName += '_';
    }
    // A single declaration makes adoption transactional, including child onAdd
    // failure. It also keeps pasted objects out of the old selection's groups.
    script::ExecutionResult result;
    const auto evaluate = [&] {
        result = game_->console().evaluate("instant SimGroup " + quoteScript(temporaryName) +
                                               " {\r\n" + std::string(source) + "};\r\n",
                                           "<mission clipboard>");
    };
    if (serverTarget_) {
        game_->withWorld(sim::WorldType::Server, evaluate);
    } else {
        evaluate();
    }
    auto* group = dynamic_cast<sim::SimGroup*>(targetManager()->findObject(temporaryName));
    if (!result.ok() || !group) {
        if (group) {
            group->deleteObject();
        }
        return false;
    }
    const std::vector<sim::SimObject*> pasted(group->objects().begin(), group->objects().end());
    if (pasted.empty()) {
        group->deleteObject();
        return false;
    }
    // The old selection defines DropToSelectedObject's destination.
    selection_.calcCentroid();
    if (drop)
        for (auto* object : pasted) {
            onDrop(object);
            if (flags_.test(DropToGround))
                dropObjectDown(object);
        }
    const auto uniqueName = [&](sim::SimObject& object) {
        const std::string original(object.getName());
        if (!original.empty()) {
            object.assignName("");
            std::string name = original;
            for (auto i : std::views::iota(UInt32{1}) |
                              std::views::take_while([&]([[maybe_unused]] UInt32 i) {
                                  return destination->findObject(name);
                              })) {
                name = original + std::to_string(i);
            }
            object.assignName(name);
        }
    };
    auto& console = game_->console();
    if (console.isFunction("MissionObjectList::ClearSelection"))
        (void)console.execute("MissionObjectList::ClearSelection");
    selection_.clearSet();
    for (auto* object : pasted) {
        uniqueName(*object);
        if (!destination->addObject(*object)) {
            selection_.clearSet();
            for (auto* previous : pasted) {
                if (previous->getGroup() == destination) {
                    previous->deleteObject();
                }
            }
            group->deleteObject();
            return false;
        }
        selection_.addObject(*object);
        sim::SimEditEvent edit;
        edit.state = focused_;
        object->processEvent(edit);
        if (console.isFunction("MissionObjectList::SelectObject"))
            (void)console.execute("MissionObjectList::SelectObject",
                                  std::array<std::string, 2>{"1", std::to_string(object->getId())});
    }
    group->deleteObject();
    selection_.calcCentroid();
    // Vanilla clipboard paste restores the copied world positions regardless
    // of the creator's current drop mode. Only PasteFile uses that mode.
    return !pasted.empty();
}

std::optional<std::string> FearMissionEditor::exportScript(const sim::SimObject& object) const {
    if (!game_) {
        return {};
    }
    std::string result;
    std::set<const sim::SimObject*> seen;
    if (!appendObjectScript(*game_, object, result, seen, 0)) {
        return {};
    }
    return result;
}

bool FearMissionEditor::duplicateSelection() {
    return copySelection() && pasteSelection();
}

void FearMissionEditor::getConsoleOptions() {
    if (!targetManager()) {
        return;
    }
    auto* con = targetManager()->console();
    if (!con) {
        return;
    }

    auto setFlagFromVar = [this,
                           con](UInt32 flag, std::string_view varName, bool fallback = false) {
        const std::string v = con->getVariable(varName);
        const bool isTrue = con->hasVariable(varName) ? script::toBool(v) : fallback;
        flags_.set(flag, isTrue);
    };

    setFlagFromVar(ConstrainX, "$ME::ConstrainX");
    setFlagFromVar(ConstrainY, "$ME::ConstrainY");
    setFlagFromVar(ConstrainZ, "$ME::ConstrainZ");
    setFlagFromVar(RotateX, "$ME::RotateXAxis");
    setFlagFromVar(RotateY, "$ME::RotateYAxis");
    setFlagFromVar(RotateZ, "$ME::RotateZAxis", true);
    setFlagFromVar(DropToGround,
                   con->hasVariable("ME::ObjectsSnapToTerrain") ? "ME::ObjectsSnapToTerrain"
                                                                : "ME::DropToGround");
    setFlagFromVar(SnapToGrid, "$ME::SnapToGrid");
    setFlagFromVar(SnapRotations, "$ME::SnapRotations");
    setFlagFromVar(DropAtCamera, "$ME::DropAtCamera");
    setFlagFromVar(DropWithRotAtCamera, "$ME::DropWithRotAtCamera");
    setFlagFromVar(DropBelowCamera, "$ME::DropBelowCamera");
    setFlagFromVar(DropToScreenCenter, "$ME::DropToScreenCenter", true);
    setFlagFromVar(DropToSelectedObject, "$ME::DropToSelectedObject");
    setFlagFromVar(UsePlaneMovement, "$ME::UsePlaneMovement");
    setFlagFromVar(AxisFollowsCamera, "$ME::AxisFollowsCamera");
    setFlagFromVar(ShowEditObjects, "$ME::ShowEditObjects", true);
    setFlagFromVar(ShowHandles,
                   con->hasVariable("ME::ShowGrabHandles") ? "ME::ShowGrabHandles"
                                                           : "ME::ShowHandles",
                   true);

    granularity_.set(parseFloatVar(con->getVariable("$ME::XGridSnap"), 1.0f),
                     parseFloatVar(con->getVariable("$ME::YGridSnap"), 1.0f),
                     parseFloatVar(con->getVariable("$ME::ZGridSnap"), 0.001f));
    rotationSnap_ = parseFloatVar(con->getVariable("$ME::RotationSnap"), 90.0f);
}

void FearMissionEditor::rebuildCommandMap() {
    if (targetManager() && targetManager()->console()) {
        const auto oldFlags = flags_;
        const bool oldFocus = focused_;
        flags_.clear(ShowEditObjects | ShowHandles);
        sim::SimLoseFocusEvent lose;
        if (oldFocus)
            processEvent(lose);

        struct Restore {
            FearMissionEditor& editor;
            BitSet32 flags;
            bool focus;

            ~Restore() {
                editor.flags_ = flags;
                if (focus) {
                    sim::SimGainFocusEvent gain;
                    editor.processEvent(gain);
                }
            }
        } restore{*this, oldFlags, oldFocus};

        (void)targetManager()->console()->execute("rebuildCommandMap");
    }
}

} // namespace opent1::game
