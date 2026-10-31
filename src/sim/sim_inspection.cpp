#include <opent1/sim/sim_world_game.hpp>
#include <algorithm>
#include <cmath>

namespace opent1::sim {
namespace {
bool editable(Inspect::Type type) {
    using T = Inspect::Type;
    return type == T::Int32 || type == T::RealF || type == T::Bool || type == T::IString ||
           type == T::Point2I || type == T::Point3I || type == T::Point2F || type == T::Point3F ||
           type == T::Tag || type == T::TagRange || type == T::List;
}

bool sameAddress(const SimAddress& a, const SimAddress& b) {
    return a.objectId == b.objectId && a.managerId == b.managerId && a.generation == b.generation;
}

bool sameMetadata(const Inspect::Record& a, const Inspect::Record& b) {
    if (a.type != b.type || a.description != b.description || a.value.index() != b.value.index()) {
        return false;
    }
    return std::visit(
        [&](const auto& value) {
            using V = std::decay_t<decltype(value)>;
            const auto& other = std::get<V>(b.value);
            if constexpr (std::same_as<V, Inspect::Integer>) {
                return value.display == other.display;
            } else if constexpr (std::same_as<V, Inspect::Tag>) {
                return value.listDefines == other.listDefines && value.filter == other.filter;
            } else if constexpr (std::same_as<V, Inspect::TagRange>) {
                return value.listDefines == other.listDefines && value.min == other.min &&
                       value.max == other.max;
            } else if constexpr (std::same_as<V, Inspect::TagList>) {
                return std::ranges::equal(
                    value.choices, other.choices, [](const auto& x, const auto& y) {
                        return x.tag == y.tag && x.value == y.value;
                    });
            } else if constexpr (std::same_as<V, Inspect::StringList>) {
                return std::ranges::equal(
                    value.choices, other.choices, [](const auto& x, const auto& y) {
                        return x.text == y.text && x.value == y.value;
                    });
            } else if constexpr (std::same_as<V, Inspect::Action>) {
                return value.message == other.message &&
                       sameAddress(value.receiver, other.receiver);
            } else if constexpr (std::same_as<V, Inspect::StringIndex>) {
                return value.text == other.text && value.index == other.index;
            } else if constexpr (std::same_as<V, std::string>) {
                return editable(a.type) || value == other;
            } else {
                return true;
            }
        },
        a.value);
}

bool validEdit(const Inspect::Value& input) {
    return std::visit(
        [](const auto& value) {
            using V = std::decay_t<decltype(value)>;
            if constexpr (std::same_as<V, Flt32>) {
                return std::isfinite(value);
            } else if constexpr (std::same_as<V, Point2F>) {
                return std::isfinite(value.x) && std::isfinite(value.y);
            } else if constexpr (std::same_as<V, Point3F>) {
                return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
            } else if constexpr (std::same_as<V, std::string>) {
                return value.size() <= 80 && value.find('\0') == value.npos;
            } else if constexpr (std::same_as<V, Inspect::TagRange>) {
                return value.value >= value.min && value.value <= value.max;
            } else if constexpr (std::same_as<V, Inspect::TagList> ||
                                 std::same_as<V, Inspect::StringList>) {
                return std::ranges::any_of(value.choices, [&](const auto& choice) {
                    return choice.value == value.value;
                });
            } else {
                return true;
            }
        },
        input);
}
}

bool Inspect::sameSchema(const Inspect& other) const {
    return std::ranges::equal(records_, other.records_, sameMetadata);
}

bool Inspect::setValue(std::size_t index, Value value) {
    if (index >= records_.size() || !editable(records_[index].type) || !validEdit(value)) {
        return false;
    }
    Record replacement{records_[index].type, records_[index].description, std::move(value)};
    if (!sameMetadata(records_[index], replacement)) {
        return false;
    }
    records_[index] = std::move(replacement);
    return true;
}

std::optional<SimInspection> SimGame::inspectObject(std::string_view path) {
    std::optional<SimInspection> result;
    withWorlds([&] {
        auto* object = findObject(path);
        if (!object || object->isRemoved() || object == object->getManager()) {
            return;
        }
        const auto type =
            world(WorldType::Server) && &world(WorldType::Server)->manager() == object->getManager()
                ? WorldType::Server
                : WorldType::Client;
        auto* owner = world(type);
        if (!owner || &owner->manager() != object->getManager()) {
            return;
        }
        SimInspection inspection;
        inspection.owner_ = inspectionOwner_;
        inspection.world_ = type;
        inspection.revision_ = worldRevisions_[static_cast<std::size_t>(type)];
        inspection.address_ = object->address();
        inspection.name_ = object->getName();
        if (const auto* descriptor = classes_.find(typeid(*object))) {
            inspection.className_ = descriptor->name;
        }
        if (!object->inspectWrite(inspection.fields_) || object->isRemoved() ||
            !sameAddress(object->address(), inspection.address_)) {
            return;
        }
        std::ranges::for_each(
            std::views::iota(std::size_t{0}, inspection.records().size()), [&](auto index) {
                if (const auto* action =
                        std::get_if<Inspect::Action>(&inspection.records()[index].value)) {
                    if (auto* receiver = action->receiver.resolve(owner->manager())) {
                        inspection.actions_.emplace_back(index, receiver->address());
                    }
                }
            });
        result = std::move(inspection);
    });
    return result;
}

bool SimGame::applyInspection(const SimInspection& inspection) {
    if (inspection.owner_ != inspectionOwner_ || !inspection.address_.generation ||
        inspection.revision_ != worldRevisions_[static_cast<std::size_t>(inspection.world_)]) {
        return false;
    }
    bool applied{};
    withWorlds([&] {
        auto* owner = world(inspection.world_);
        if (!owner) {
            return;
        }
        auto* object = inspection.address_.resolve(owner->manager());
        if (!object || object->isRemoved() || object->isLocked()) {
            return;
        }
        Inspect current;
        if (!object->inspectWrite(current) || object->isRemoved() || object->isLocked() ||
            !sameAddress(object->address(), inspection.address_) ||
            !current.sameSchema(inspection.fields_)) {
            return;
        }
        auto fields = inspection.fields_;
        fields.top();
        applied = object->inspectRead(fields) && !fields.peek() && !object->isRemoved();
    });
    return applied;
}

bool SimGame::activateInspection(const SimInspection& inspection, std::size_t record) {
    if (inspection.owner_ != inspectionOwner_ || !inspection.address_.generation ||
        inspection.revision_ != worldRevisions_[static_cast<std::size_t>(inspection.world_)]) {
        return false;
    }
    const auto action =
        std::ranges::find(inspection.actions_, record, &std::pair<std::size_t, SimAddress>::first);
    if (action == inspection.actions_.end()) {
        return false;
    }
    bool sent{};
    withWorlds([&] {
        auto* owner = world(inspection.world_);
        if (!owner) {
            return;
        }
        auto& manager = owner->manager();
        auto* object = inspection.address_.resolve(manager);
        if (!object || object->isRemoved() || object->isLocked()) {
            return;
        }
        Inspect current;
        if (!object->inspectWrite(current) || object->isRemoved() || object->isLocked() ||
            !sameAddress(object->address(), inspection.address_) ||
            !current.sameSchema(inspection.fields_)) {
            return;
        }
        SimMessageEvent event;
        event.message = std::get<Inspect::Action>(inspection.records()[record].value).message;
        event.time = manager.getCurrentTime();
        sent = manager.sendEvent(action->second, event);
    });
    return sent;
}

bool SimGame::setDataBlockManager(std::shared_ptr<SimDataBlockManager> provider) {
    if (driving_ || switching_ || declarationDepth_) {
        return false;
    }
    dataBlocks_ = std::move(provider);
    return true;
}
} // namespace opent1::sim
