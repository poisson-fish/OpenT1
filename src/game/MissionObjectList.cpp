#include <opent1/game/fear_mission_editor.hpp>
#include <ranges>
#include <opent1/game/game_base.hpp>

#include <algorithm>
#include <charconv>
#include <format>

namespace opent1::game {

namespace {

enum MOLCommandId : Int32 {
    ClearDisplayGroups = 100,
    AddDisplayGroup = 101,
    SetSelMode = 102,
    ExpandToObject = 103,
    Inspect = 104,
    Apply = 105,
    UnselectAll = 106,
    SelectObject = 107,
};

[[nodiscard]] Int32 parseIntArg(std::string_view sv) noexcept {
    Int32 val = 0;
    std::from_chars(sv.data(), sv.data() + sv.size(), val);
    return val;
}

} // namespace

void MEObjectList::clearDisplayGroups() {
    roots_.clear();
    objectDisplayList_.clear();
}

bool MEObjectList::addDisplayGroup(sim::SimObject* obj) {
    if (!obj) {
        return false;
    }
    for (const auto* existing : roots_) {
        if (existing == obj) {
            return false;
        }
    }
    roots_.push_back(obj);
    buildObjectDisplayList();
    return true;
}

void MEObjectList::flattenObject(sim::SimObject* obj,
                                 Int32 depth,
                                 Int32 parentIndex,
                                 bool isLastInGroup) {
    if (!obj) {
        return;
    }
    const Int32 myIndex = static_cast<Int32>(objectDisplayList_.size());
    DOEntry entry{};
    entry.obj = obj;
    entry.id = obj->getId();
    entry.depth = depth;
    entry.parentIndex = parentIndex;
    entry.lastInGroup = isLastInGroup ? 1 : 0;
    objectDisplayList_.push_back(entry);

    if (auto* set = dynamic_cast<sim::SimSet*>(obj)) {
        if (obj->isExpanded()) {
            const auto& children = set->objects();
            for (auto i : std::views::iota(std::size_t{0}, children.size())) {
                const bool childLast = (i + 1 == children.size());
                flattenObject(children[i], depth + 1, myIndex, childLast);
            }
        }
    }
}

void MEObjectList::buildObjectDisplayList() {
    objectDisplayList_.clear();
    for (auto i : std::views::iota(std::size_t{0}, roots_.size())) {
        const bool isLast = (i + 1 == roots_.size());
        flattenObject(roots_[i], 0, -1, isLast);
    }
}

Int32 MEObjectList::getStateBmp(UInt32 index) const noexcept {
    if (index >= objectDisplayList_.size()) {
        return BmpNone;
    }
    const DOEntry& entry = objectDisplayList_[index];
    if (!entry.obj) {
        return BmpNone;
    }

    if (dynamic_cast<const sim::SimSet*>(entry.obj) != nullptr) {
        if (entry.obj->isExpanded()) {
            if ((index == 0) && (index + 1 == objectDisplayList_.size())) {
                return BmpParentOpenOnly;
            }
            if (index == 0) {
                return BmpParentOpenBelow;
            }
            if (!entry.lastInGroup || (index + 1 < objectDisplayList_.size() &&
                                       objectDisplayList_[index + 1].depth > entry.depth)) {
                return BmpParentOpenBetween;
            }
            return BmpParentOpenAbove;
        }
        if ((index == 0) && entry.lastInGroup) {
            return BmpParentClosedOnly;
        }
        if (index == 0) {
            return BmpParentClosedBelow;
        }
        if (entry.lastInGroup) {
            return BmpParentClosedAbove;
        }
        return BmpParentClosedBetween;
    }

    if (index == 0) {
        return BmpNone;
    }
    if (entry.lastInGroup) {
        return BmpChildAbove;
    }
    return BmpChildBetween;
}

Int32 MEObjectList::getBranchBmp(UInt32 index, Int32 depth) const noexcept {
    if (index >= objectDisplayList_.size()) {
        return BmpNone;
    }
    Int32 curIdx = objectDisplayList_[index].parentIndex;
    while (curIdx >= 0 && static_cast<std::size_t>(curIdx) < objectDisplayList_.size()) {
        const DOEntry& parent = objectDisplayList_[static_cast<std::size_t>(curIdx)];
        if (parent.depth == depth) {
            return parent.lastInGroup ? BmpNone : BmpParentContinue;
        }
        curIdx = parent.parentIndex;
    }
    return BmpNone;
}

std::string MEObjectList::formatEntryText(const DOEntry& entry) const {
    if (!entry.obj) {
        return {};
    }
    const std::string_view objName = entry.obj->getName();
    std::string_view clsName = "SimObject";
    if (const auto* gb = dynamic_cast<const GameBase*>(entry.obj)) {
        clsName = gb->getScriptDefaultClass();
    } else if (dynamic_cast<const sim::SimGroup*>(entry.obj) != nullptr) {
        clsName = "SimGroup";
    } else if (dynamic_cast<const sim::SimSet*>(entry.obj) != nullptr) {
        clsName = "SimSet";
    }
    return std::format(
        "{}: {}, {}", entry.id, objName.empty() ? std::string_view("<unnamed>") : objName, clsName);
}

void MEObjectList::unselectAll(sim::SimGroup* group) {
    if (group) {
        group->setSelected(false);
        for (auto* child : group->objects()) {
            if (auto* subGrp = dynamic_cast<sim::SimGroup*>(child)) {
                unselectAll(subGrp);
            } else if (child) {
                child->setSelected(false);
            }
        }
        return;
    }
    for (auto* root : roots_) {
        if (auto* grp = dynamic_cast<sim::SimGroup*>(root)) {
            unselectAll(grp);
        } else if (root) {
            root->setSelected(false);
        }
    }
}

void MEObjectList::selectObject(sim::SimObject* obj, bool select) {
    if (!obj) {
        return;
    }
    obj->setSelected(select);
}

void MEObjectList::expandToObject(sim::SimObject* obj) {
    if (!obj) {
        return;
    }
    sim::SimGroup* grp = obj->getGroup();
    while (grp) {
        grp->setExpanded(true);
        sim::SimGroup* nextGrp = grp->getGroup();
        if (nextGrp == grp) {
            break;
        }
        grp = nextGrp;
    }
    buildObjectDisplayList();
}

bool MEObjectList::inspectObject(sim::SimObject* obj) {
    inspectBuffer_.clear();
    inspectObj_ = obj;
    if (!obj) {
        return false;
    }
    return obj->inspectWrite(inspectBuffer_);
}

bool MEObjectList::applyInspection(sim::SimObject* obj) {
    sim::SimObject* target = obj ? obj : inspectObj_;
    if (!target) {
        return false;
    }
    inspectBuffer_.top();
    return target->inspectRead(inspectBuffer_);
}

void MEObjectList::registerConsoleCommands(Console& console,
                                           MEObjectList& list,
                                           sim::SimManager* manager) {
    auto bindCmd = [&console, &list, manager](Int32 id, std::string_view name) {
        console.addCommand(
            id,
            name,
            [&list, manager](Console& /*c*/,
                             Int32 cmdId,
                             std::span<const std::string> argv) -> std::optional<std::string> {
                switch (cmdId) {
                    case ClearDisplayGroups:
                        list.clearDisplayGroups();
                        return "True";
                    case AddDisplayGroup:
                        if (argv.size() >= 3 && manager) {
                            if (auto* obj = manager->findObject(argv[2])) {
                                return list.addDisplayGroup(obj) ? "True" : "False";
                            }
                        }
                        return "False";
                    case SetSelMode:
                        if (argv.size() >= 3) {
                            list.setSelMode(parseIntArg(argv[2]));
                            return "True";
                        }
                        return "False";
                    case ExpandToObject:
                        if (argv.size() >= 3 && manager) {
                            if (auto* obj = manager->findObject(argv[2])) {
                                list.expandToObject(obj);
                                return "True";
                            }
                        }
                        return "False";
                    case Inspect:
                        if (argv.size() >= 3 && manager) {
                            if (auto* obj = manager->findObject(argv[2])) {
                                return list.inspectObject(obj) ? "True" : "False";
                            }
                        }
                        return "False";
                    case Apply:
                        return list.applyInspection(nullptr) ? "True" : "False";
                    case UnselectAll:
                        list.unselectAll(nullptr);
                        return "True";
                    case SelectObject:
                        if (argv.size() >= 3 && manager) {
                            if (auto* obj = manager->findObject(argv[2])) {
                                const bool sel =
                                    (argv.size() >= 4) ? (parseIntArg(argv[3]) != 0) : true;
                                list.selectObject(obj, sel);
                                return "True";
                            }
                        }
                        return "False";
                    default:
                        return "False";
                }
            });
    };

    bindCmd(ClearDisplayGroups, "MissionObjectList::ClearDisplayGroups");
    bindCmd(AddDisplayGroup, "MissionObjectList::AddDisplayGroup");
    bindCmd(SetSelMode, "MissionObjectList::SetSelMode");
    bindCmd(ExpandToObject, "MissionObjectList::ExpandToObject");
    bindCmd(Inspect, "MissionObjectList::Inspect");
    bindCmd(Apply, "MissionObjectList::Apply");
    bindCmd(UnselectAll, "MissionObjectList::UnselectAll");
    bindCmd(SelectObject, "MissionObjectList::SelectObject");
}

} // namespace opent1::game
