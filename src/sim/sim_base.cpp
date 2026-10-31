#include <opent1/sim/sim_base.hpp>
#include <charconv>
#include <cmath>
#include <exception>
#include <limits>
#include <stdexcept>

namespace opent1::sim {
namespace {
std::string_view text(std::string_view value) {
    return value.substr(0, value.find('\0'));
}

std::string key(std::string_view value) {
    std::string result(text(value));
    std::ranges::transform(result, result.begin(), [](unsigned char c) {
        return static_cast<char>(c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c);
    });
    return result;
}

bool separator(char c) {
    return c == '/' || c == '\\';
}

void eraseMember(std::vector<SimObject*>& list, SimObject& object, bool stable) {
    const auto it = std::ranges::find(list, &object);
    if (it == list.end()) {
        return;
    }
    if (!stable) {
        *it = list.back();
        list.pop_back();
    } else {
        list.erase(it);
    }
}
}

SimObject* SimAddress::resolve(SimManager& manager) const {
    if (managerId != 0 && managerId != manager.getId()) {
        return nullptr;
    }
    auto* object = manager.findObject(objectId);
    return object && (!generation || generation == object->generation()) ? object : nullptr;
}

bool SimEvent::packAddress(BitStream& stream) const {
    const bool parent = (flags & ToGhostParent) != 0;
    const bool ghost = (flags & ToGhost) != 0;
    if ((parent || ghost) && address.objectId >= 1024) {
        return false;
    }
    stream.writeFlag(parent);
    if (!parent) {
        stream.writeFlag(ghost);
    }
    stream.writeInt(std::bit_cast<Int32>(address.objectId), parent || ghost ? 10 : 32);
    return stream.isValid();
}

bool SimEvent::unpackAddress(BitStream& stream) {
    const bool parent = stream.readFlag();
    const bool ghost = !parent && stream.readFlag();
    const auto id = std::bit_cast<UInt32>(stream.readInt(parent || ghost ? 10 : 32));
    if (!stream.isValid()) {
        return false;
    }
    flags = (flags & ~(ToGhost | ToGhostParent)) | (parent ? ToGhostParent : ghost ? ToGhost : 0U);
    address = {id, 0, 0};
    return true;
}

bool SimEventQueue::insert(std::unique_ptr<SimEvent> event, bool current) {
    if (!event) {
        return false;
    }
    std::scoped_lock lock(mutex_);
    if (current) {
        event->time = currentTime_;
    }
    if (!std::isfinite(event->time) || event->time < currentTime_ ||
        events_.size() - first_ >= capacity_) {
        return false;
    }
    if (first_ == events_.size() || events_.back()->time <= event->time) {
        events_.push_back(std::move(event));
        return true;
    }
    const auto where =
        std::ranges::upper_bound(events_.begin() + static_cast<std::ptrdiff_t>(first_),
                                 events_.end(),
                                 event->time,
                                 {},
                                 [](const auto& e) {
                                     return e->time;
                                 });
    events_.insert(where, std::move(event));
    return true;
}

bool SimEventQueue::insertBatch(std::vector<std::unique_ptr<SimEvent>> pending, bool current) {
    std::scoped_lock lock(mutex_);
    if (pending.size() > capacity_ - (events_.size() - first_)) {
        return false;
    }
    for (auto& event : pending) {
        if (!event) {
            return false;
        }
        if (current) {
            event->time = currentTime_;
        }
        if (!std::isfinite(event->time) || event->time < currentTime_) {
            return false;
        }
    }
    // Stable sort once and merge: repeated sorted insertion is quadratic for a
    // large batch (including debug-library range validation). Existing events
    // retain precedence at equal timestamps, as with individual insertion.
    std::ranges::stable_sort(pending, {}, [](const auto& e) {
        return e->time;
    });
    std::vector<std::unique_ptr<SimEvent>> merged;
    merged.reserve(events_.size() - first_ + pending.size());
    auto old = first_;
    std::size_t next{};
    while (old < events_.size() || next < pending.size()) {
        if (next == pending.size() ||
            (old < events_.size() && events_[old]->time <= pending[next]->time)) {
            merged.push_back(std::move(events_[old++]));
        } else {
            merged.push_back(std::move(pending[next++]));
        }
    }
    events_ = std::move(merged);
    first_ = 0;
    return true;
}

std::unique_ptr<SimEvent> SimEventQueue::pop(SimTime target) {
    std::scoped_lock lock(mutex_);
    if (!std::isfinite(target) || target < currentTime_) {
        throw std::invalid_argument("simulation time moved backwards");
    }
    if (first_ == events_.size() || events_[first_]->time > target) {
        currentTime_ = target;
        return {};
    }
    auto event = std::move(events_[first_++]);
    if (first_ == events_.size()) {
        events_.clear();
        first_ = 0;
    } else if (first_ >= 4096 && first_ >= events_.size() / 2) {
        events_.erase(events_.begin(), events_.begin() + static_cast<std::ptrdiff_t>(first_));
        first_ = 0;
    }
    currentTime_ = event->time;
    return event;
}

void SimEventQueue::remove(SimObjectId id) {
    std::vector<std::unique_ptr<SimEvent>> removed;
    {
        std::scoped_lock lock(mutex_);
        removed.reserve(events_.size());
        std::erase_if(events_, [&](auto& event) {
            if (!event) {
                return true;
            }
            if (event->address.objectId != id) {
                return false;
            }
            removed.push_back(std::move(event));
            return true;
        });
        first_ = 0;
    }
}

void SimEventQueue::resetTime() {
    std::scoped_lock lock(mutex_);
    std::ranges::for_each(events_, [&](auto& event) {
        if (event) {
            event->time -= currentTime_;
        }
    });
    currentTime_ = 0;
}

SimTime SimEventQueue::getCurrentTime() const {
    std::scoped_lock lock(mutex_);
    return currentTime_;
}

std::size_t SimEventQueue::size() const {
    std::scoped_lock lock(mutex_);
    return events_.size() - first_;
}

void SimEventQueue::signal() {
    {
        std::scoped_lock lock(mutex_);
        signaled_ = true;
    }
    condition_.notify_one();
}

bool SimEventQueue::wait(std::chrono::milliseconds timeout) {
    std::unique_lock lock(mutex_);
    if (!condition_.wait_for(lock, timeout, [&] {
            return signaled_;
        })) {
        return false;
    }
    signaled_ = false;
    return true;
}

void SimIdGenerator::reset() {
    base_ = limit_ = next_ = 0;
    pool_.clear();
    used_.clear();
    blocks_.clear();
}

bool SimIdGenerator::addBlock(UInt32 block) {
    if (!block || block > (std::numeric_limits<UInt32>::max() >> IdBlockShift) || next_ != limit_ ||
        !pool_.empty() || std::ranges::find(blocks_, block) != blocks_.end()) {
        return false;
    }
    blocks_.push_back(block);
    base_ = UInt64{block} << IdBlockShift;
    limit_ = base_ + ReservedIdLimit;
    next_ = base_;
    return true;
}

bool SimIdGenerator::inRange(SimObjectId id) const {
    return std::ranges::find(blocks_, id >> IdBlockShift) != blocks_.end();
}

SimObjectId SimIdGenerator::alloc() {
    if (!pool_.empty()) {
        const auto id = pool_.back();
        used_.insert(id);
        pool_.pop_back();
        while (!pool_.empty() && next_ > base_ && pool_.back() == next_ - 1) {
            --next_;
            pool_.pop_back();
        }
        return id;
    }
    if (next_ == limit_) {
        return 0;
    }
    const auto id = static_cast<SimObjectId>(next_);
    used_.insert(id);
    ++next_;
    return id;
}

bool SimIdGenerator::free(SimObjectId id) {
    if (!used_.contains(id)) {
        return false;
    }
    pool_.push_back(id);
    used_.erase(id);
    while (!pool_.empty() && next_ > base_ && pool_.back() == next_ - 1) {
        --next_;
        pool_.pop_back();
    }
    return true;
}

UInt32 SimIdBlockManager::alloc(SimObject& object) {
    if (pool_.empty() && next_ > (std::numeric_limits<UInt32>::max() >> IdBlockShift)) {
        throw std::length_error("simulation ID blocks exhausted");
    }
    const auto block = pool_.empty() ? next_ : pool_.back();
    allocated_.emplace(block, &object);
    if (pool_.empty()) {
        ++next_;
    } else {
        pool_.pop_back();
    }
    return block;
}

bool SimIdBlockManager::free(UInt32 block) {
    if (!allocated_.contains(block)) {
        return false;
    }
    pool_.push_back(block);
    allocated_.erase(block);
    while (!pool_.empty() && pool_.back() == next_ - 1) {
        --next_;
        pool_.pop_back();
    }
    return true;
}

void SimIdBlockManager::free(SimObject& object) {
    std::vector<UInt32> blocks;
    std::ranges::for_each(allocated_, [&](const auto& entry) {
        if (entry.second == &object) {
            blocks.push_back(entry.first);
        }
    });
    std::ranges::sort(blocks);
    std::ranges::for_each(blocks, [&](auto block) {
        free(block);
    });
}

SimObject* SimIdBlockManager::findBlock(UInt32 block) const {
    const auto it = allocated_.find(block);
    return it == allocated_.end() ? nullptr : it->second;
}

bool SimNameDictionary::insert(SimObject& object) {
    if (object.getName().empty()) {
        return true;
    }
    auto& list = entries_[key(object.getName())];
    if (std::ranges::find(list, &object) != list.end()) {
        return false;
    }
    list.push_back(&object);
    return true;
}

void SimNameDictionary::remove(SimObject& object) {
    const auto it = entries_.find(key(object.getName()));
    if (it == entries_.end()) {
        return;
    }
    std::erase(it->second, &object);
    if (it->second.empty()) {
        entries_.erase(it);
    }
}

SimObject* SimNameDictionary::find(std::string_view name) const {
    const auto it = entries_.find(key(name));
    if (it == entries_.end()) {
        return nullptr;
    }
    const auto candidates = it->second | std::views::reverse;
    const auto found = std::ranges::find_if(candidates, [](const auto* object) {
        return !object->isRemoved();
    });
    return found == candidates.end() ? nullptr : *found;
}

bool SimIdDictionary::insert(SimObject& object) {
    return entries_.emplace(object.getId(), &object).second;
}

void SimIdDictionary::remove(SimObject& object) {
    const auto it = entries_.find(object.getId());
    if (it != entries_.end() && it->second == &object) {
        entries_.erase(it);
    }
}

SimObject* SimIdDictionary::find(SimObjectId id) const {
    const auto it = entries_.find(id);
    return it == entries_.end() ? nullptr : it->second;
}

SimObject::~SimObject() {
    // Manager teardown has already delivered these notifications. Standalone
    // borrowed graphs still need link cleanup, regardless of destruction order.
    try {
        if (group_) {
            group_->removeObject(*this);
        }
        notifyRemoval();
    } catch (...) {
    }
}

bool SimObject::setId(SimObjectId id) {
    if (manager_) {
        return manager_->assignId(*this, id);
    }
    id_ = id;
    return true;
}

SimAddress SimObject::address() const {
    return {manager_ == this ? 0U : id_, 0, generation_};
}

void SimObject::assignName(std::string_view name) {
    std::string replacement(text(name));
    if (replacement.size() > 255) {
        throw std::length_error("simulation name exceeds 255 bytes");
    }
    if (group_) {
        group_->names_.remove(*this);
    }
    name_ = std::move(replacement);
    if (group_) {
        group_->names_.insert(*this);
    }
    if (!name_.empty()) {
        if (manager_) {
            manager_->dispatch([&] {
                onNameChange(name_);
            });
        } else {
            onNameChange(name_);
        }
    }
}

void SimObject::setFieldValue(std::string_view slot, std::string_view value) {
    const auto name = key(slot);
    if (text(value).empty()) {
        fields_.erase(name);
    } else {
        fields_.insert_or_assign(name, std::string(text(value)));
    }
}

std::optional<std::string_view> SimObject::getFieldValue(std::string_view slot) const {
    const auto it = fields_.find(key(slot));
    return it == fields_.end() ? std::nullopt : std::optional<std::string_view>(it->second);
}

void SimObject::setLocked(bool value) {
    setFieldValue("locked", value ? "true" : "false");
}

bool SimObject::isLocked() const {
    const auto value = getFieldValue("locked");
    if (!value) {
        return false;
    }
    if (key(*value) == "true") {
        return true;
    }
    double number{};
    const auto parsed = std::from_chars(value->data(), value->data() + value->size(), number);
    return parsed.ec == std::errc{} && number != 0;
}

void SimObject::deleteNotify(SimObject& object) {
    if (&object == this || object.deleted_ || object.removed_) {
        return;
    }
    if (std::ranges::find(observed_, &object) == observed_.end()) {
        observed_.reserve(observed_.size() + 1);
        object.observers_.reserve(object.observers_.size() + 1);
        observed_.push_back(&object);
        object.observers_.push_back(this);
    }
}

void SimObject::clearNotify(SimObject& object) {
    std::erase(observed_, &object);
    std::erase(object.observers_, this);
}

void SimObject::clearAllNotifications() {
    std::ranges::for_each(observed_, [&](auto* object) {
        std::erase(object->observers_, this);
    });
    observed_.clear();
}

void SimObject::notifyRemoval() {
    clearAllNotifications();
    std::exception_ptr error;
    while (!observers_.empty()) {
        auto* observer = observers_.back();
        observers_.pop_back();
        std::erase(observer->observed_, this);
        try {
            observer->onDeleteNotify(*this);
        } catch (...) {
            if (!error) {
                error = std::current_exception();
            }
        }
    }
    if (error) {
        std::rethrow_exception(error);
    }
}

bool SimObject::deleteObject() {
    return manager_ && manager_->deleteObject(*this);
}

bool SimObject::addToSet(SimObjectId id) {
    auto* set = manager_ ? dynamic_cast<SimSet*>(manager_->findObject(id)) : nullptr;
    return set && set->addObject(*this);
}

bool SimObject::removeFromSet(SimObjectId id) {
    auto* set = manager_ ? dynamic_cast<SimSet*>(manager_->findObject(id)) : nullptr;
    return set && set->removeObject(*this);
}

bool SimObject::postEvent(std::unique_ptr<SimEvent> event, bool current) {
    if (!manager_ || !event) {
        return false;
    }
    event->address = address();
    return manager_->postEvent(std::move(event), current);
}

SimSet::~SimSet() {
    clearAllNotifications();
}

bool SimSet::addObject(SimObject& object) {
    if (&object == this || object.isDeleted() || object.isRemoved() ||
        object.getManager() != getManager()) {
        return false;
    }
    if (std::ranges::find(objects_, &object) != objects_.end()) {
        return true;
    }
    // Reject set cycles as well as group cycles, keeping broadcasts bounded.
    std::vector<const SimSet*> pending;
    if (const auto* set = dynamic_cast<const SimSet*>(&object)) {
        pending.push_back(set);
    }
    std::unordered_set<const SimSet*> visited;
    while (!pending.empty()) {
        const auto* set = pending.back();
        pending.pop_back();
        if (set == this) {
            return false;
        }
        if (!visited.insert(set).second) {
            continue;
        }
        std::ranges::for_each(set->objects(), [&](auto* item) {
            if (auto* child = dynamic_cast<const SimSet*>(item)) {
                pending.push_back(child);
            }
        });
    }
    objects_.reserve(objects_.size() + 1);
    deleteNotify(object);
    objects_.push_back(&object);
    return true;
}

bool SimSet::removeObject(SimObject& object) {
    const bool found = std::ranges::find(objects_, &object) != objects_.end();
    eraseMember(objects_, object, stable_);
    clearNotify(object);
    return found;
}

bool SimSet::reOrder(SimObject& object, SimObject* before) {
    const auto source = std::ranges::find(objects_, &object);
    if (source == objects_.end()) {
        return false;
    }
    if (&object == before) {
        return true;
    }
    if (before && std::ranges::find(objects_, before) == objects_.end()) {
        return false;
    }
    objects_.erase(source);
    objects_.insert(before ? std::ranges::find(objects_, before) : objects_.end(), &object);
    return true;
}

void SimSet::onDeleteNotify(SimObject& object) {
    removeObject(object);
}

bool SimSet::processEvent(const SimEvent& event) {
    if (SimObject::processEvent(event)) {
        return true;
    }
    const auto copy = objects_;
    const auto broadcast = [&] {
        std::ranges::for_each(copy, [&](auto* object) {
            if (std::ranges::find(objects_, object) != objects_.end() && !object->isRemoved()) {
                object->processEvent(event);
            }
        });
    };
    if (getManager()) {
        getManager()->dispatch(broadcast);
    } else {
        broadcast();
    }
    return false;
}

void SimSet::onPreLoad(SimPreLoadManager& preloader) {
    const auto copy = objects_;
    const auto broadcast = [&] {
        std::ranges::for_each(copy, [&](auto* object) {
            if (std::ranges::find(objects_, object) != objects_.end() && !object->isRemoved()) {
                object->onPreLoad(preloader);
            }
        });
    };
    if (getManager()) {
        getManager()->dispatch(broadcast);
    } else {
        broadcast();
    }
}

Persistent::Error
SimSet::write(StreamIO& stream, Int32, Int32 user, Persistent::StoreContext& context) const {
    if (objects_.size() > static_cast<std::size_t>(std::numeric_limits<Int32>::max()) ||
        !stream.write(static_cast<Int32>(objects_.size()))) {
        return Persistent::Error::WriteError;
    }
    for (const auto* object : objects_) {
        const auto result = context.store(stream, *object, user);
        if (result != Persistent::Error::Ok) {
            return result;
        }
    }
    return Persistent::Error::Ok;
}

Persistent::Error SimSet::read(StreamIO& stream, Int32, Int32, Persistent::LoadContext& context) {
    Int32 count{};
    if (getManager() || !empty() || !stream.read(count) || count < 0) {
        return Persistent::Error::ReadError;
    }
    for (Int32 i = 0; i < count; ++i) {
        const auto result = context.load(stream);
        if (!result) {
            return result.error;
        }
        auto* object = dynamic_cast<SimObject*>(result.object);
        if (!object || !addObject(*object)) {
            return Persistent::Error::IncorrectTypeFound;
        }
    }
    return Persistent::Error::Ok;
}

SimGroup::~SimGroup() {
    std::ranges::for_each(objects_, [&](auto* object) {
        names_.remove(*object);
        object->group_ = nullptr;
    });
}

bool SimGroup::addObject(SimObject& object) {
    bool result{};
    const auto attach = [&] {
        if (isRemoved() || object.isRemoved()) {
            return;
        }
        if (object.group_ == this) {
            result = true;
            return;
        }
        // SimSet's graph check precedes removing the previous group.
        if (!SimSet::addObject(object)) {
            return;
        }
        try {
            names_.insert(object);
            if (object.group_) {
                object.group_->removeObject(object);
            }
        } catch (...) {
            names_.remove(object);
            SimSet::removeObject(object);
            throw;
        }
        if (object.isRemoved() || isRemoved() || object.group_) {
            names_.remove(object);
            SimSet::removeObject(object);
            return;
        }
        object.group_ = this;
        object.onGroupAdd();
        result = !object.isRemoved() && object.group_ == this;
    };
    if (getManager()) {
        getManager()->dispatch(attach);
    } else {
        attach();
    }
    return result;
}

bool SimGroup::removeObject(SimObject& object) {
    if (object.group_ != this) {
        return false;
    }
    const auto detach = [&] {
        // Publish removal before calling user code, allowing deletion/reparenting
        // in the hook without recursively removing the same membership.
        names_.remove(object);
        object.group_ = nullptr;
        SimSet::removeObject(object);
        object.onGroupRemove();
    };
    if (getManager()) {
        getManager()->dispatch(detach);
    } else {
        detach();
    }
    return true;
}

SimObject* SimGroup::findObject(std::string_view path) const {
    path = text(path);
    while (!path.empty() && (path.front() == ' ' || path.front() == '\t' || path.front() == '\n' ||
                             path.front() == '\r')) {
        path.remove_prefix(1);
    }
    const SimSet* current = this;
    if (!path.empty() && separator(path.front())) {
        current = getManager();
        path.remove_prefix(1);
    } else if (!path.empty() && path.front() >= '0' && path.front() <= '9') {
        UInt32 id{};
        const auto end = path.find_first_of("/\\");
        const auto first = path.substr(0, end);
        const auto parsed = std::from_chars(first.data(), first.data() + first.size(), id);
        if (parsed.ec != std::errc{} || !getManager()) {
            return nullptr;
        }
        auto* object = getManager()->findObject(id);
        if (end == std::string_view::npos) {
            return object;
        }
        current = dynamic_cast<SimSet*>(object);
        path.remove_prefix(end + 1);
    }
    while (current && !path.empty()) {
        const auto end = path.find_first_of("/\\");
        const auto component = path.substr(0, end);
        SimObject* object{};
        if (const auto* group = dynamic_cast<const SimGroup*>(current)) {
            object = group->names_.find(component);
        } else {
            const auto folded = key(component);
            const auto it = std::ranges::find_if(current->objects(), [&](auto* item) {
                return !item->isRemoved() && key(item->getName()) == folded;
            });
            if (it != current->objects().end()) {
                object = *it;
            }
        }
        if (end == std::string_view::npos) {
            return object;
        }
        current = dynamic_cast<SimSet*>(object);
        path.remove_prefix(end + 1);
    }
    return nullptr;
}

Persistent::Error SimGroup::write(StreamIO& stream,
                                  Int32 version,
                                  Int32 user,
                                  Persistent::StoreContext& context) const {
    const auto result = SimSet::write(stream, version, user, context);
    if (result != Persistent::Error::Ok) {
        return result;
    }
    for (const auto* object : objects_) {
        if (!stream.writeString(object->getName())) {
            return Persistent::Error::WriteError;
        }
    }
    return Persistent::Error::Ok;
}

Persistent::Error
SimGroup::read(StreamIO& stream, Int32 version, Int32 user, Persistent::LoadContext& context) {
    const auto result = SimSet::read(stream, version, user, context);
    if (result != Persistent::Error::Ok) {
        return result;
    }
    for (auto* object : objects_) {
        std::string name;
        if (!stream.readString(name)) {
            return Persistent::Error::ReadError;
        }
        object->assignName(name);
    }
    return Persistent::Error::Ok;
}

SimManager::SimManager(std::size_t capacity) : queue_(capacity) {
    manager_ = this;
    added_ = true;
    generator_.addBlock(8);
    auto lookup = std::make_unique<SimSet>();
    lookup->setId(LookupSetId);
    addObject(std::move(lookup));
}

SimManager::~SimManager() {
    shuttingDown_ = true;
    while (!owned_.empty()) {
        const auto it = std::ranges::max_element(owned_, {}, [](const auto& object) {
            return object->getNumber();
        });
        try {
            deleteObject(**it);
        } catch (...) {
        }
    }
    retired_.clear();
    manager_ = nullptr;
}

SimObjectId SimManager::allocateId() {
    for (;;) {
        auto id = generator_.alloc();
        if (!id) {
            if (!generator_.addBlock(nextBlock_++)) {
                return 0;
            }
            id = generator_.alloc();
        }
        if (!ids_.find(id)) {
            return id;
        }
        // Keep explicit IDs reserved in the generator so a later allocation
        // cannot accidentally alias one. They are released on manager teardown.
    }
}

void SimManager::dispatch(const std::function<void()>& callback) {
    if (dispatchDepth_ >= dispatchDepthLimit_) {
        throw std::length_error("simulation callback nesting exceeds configured limit");
    }
    ++dispatchDepth_;
    try {
        callback();
    } catch (...) {
        if (!--dispatchDepth_) {
            retired_.clear();
        }
        throw;
    }
    if (!--dispatchDepth_) {
        retired_.clear();
    }
}

bool SimManager::sendEvent(const SimAddress& address, const SimEvent& event) {
    bool handled = false;
    dispatch([&] {
        if (auto* object = address.resolve(*this)) {
            handled = object->processEvent(event);
        }
    });
    return handled;
}

bool SimManager::sendQuery(const SimAddress& address, SimQuery& query) {
    bool handled = false;
    dispatch([&] {
        if (auto* object = address.resolve(*this)) {
            handled = object->processQuery(query);
        }
    });
    return handled;
}

SimObject* SimManager::addObject(std::unique_ptr<SimObject> object, SimGroup* group) {
    if (!object || shuttingDown_ || object->manager_ || object->group_ ||
        (group && group->getManager() != this)) {
        return nullptr;
    }
    // Prebuilt groups stay with their LoadContext; central ownership is explicit.
    if (const auto* set = dynamic_cast<const SimSet*>(object.get()); set && !set->empty()) {
        return nullptr;
    }
    if (object->id_ && ids_.find(object->id_)) {
        return nullptr;
    }
    owned_.reserve(owned_.size() + 1);
    if (!object->id_) {
        object->id_ = allocateId();
        object->dynamicId_ = true;
    }
    if (!object->id_) {
        return nullptr;
    }
    auto* pointer = object.get();
    pointer->manager_ = this;
    pointer->number_ = nextNumber_++;
    pointer->generation_ = nextGeneration_++;
    pointer->deleted_ = pointer->removed_ = false;
    ids_.insert(*pointer);
    owned_.push_back(std::move(object));
    bool success{};
    dispatch([&] {
        try {
            success = pointer->onAdd() && !pointer->removed_;
            if (success) {
                pointer->added_ = true;
                success = (group ? group : this)->addObject(*pointer) && !pointer->removed_;
            }
        } catch (...) {
            if (!pointer->removed_) {
                deleteObject(*pointer);
            }
            throw;
        }
        if (!success && !pointer->removed_) {
            deleteObject(*pointer);
        }
    });
    return success ? pointer : nullptr;
}

SimObject* SimManager::adoptObjects(std::vector<std::unique_ptr<SimObject>> objects,
                                    SimObject& root,
                                    SimSet* destination,
                                    const std::function<bool(SimObject&)>& accept) {
    if (shuttingDown_ || objects.empty() ||
        (destination && (destination->getManager() != this || destination->isRemoved()))) {
        return nullptr;
    }
    const auto target = destination ? destination->address() : SimAddress{};
    std::unordered_set<SimObject*> members;
    std::unordered_set<SimObjectId> explicitIds;
    if (!std::ranges::all_of(objects,
                             [&](const auto& p) {
                                 return p && !p->manager_ && !p->removed_ && !p->deleted_ &&
                                        members.insert(p.get()).second &&
                                        (!p->id_ ||
                                         (!ids_.find(p->id_) && explicitIds.insert(p->id_).second));
                             }) ||
        !members.contains(&root) || root.group_) {
        return nullptr;
    }
    std::vector<SimObject*> ordered;
    ordered.reserve(objects.size());
    const auto depth = [](const SimObject* p) {
        std::unordered_set<const SimObject*> parents;
        while (p) {
            if (!parents.insert(p).second) {
                return std::numeric_limits<std::size_t>::max();
            }
            p = p->getGroup();
        }
        return parents.size();
    };
    if (!std::ranges::all_of(objects, [&](const auto& p) {
            if ((p->group_ && !members.contains(p->group_)) ||
                depth(p.get()) == std::numeric_limits<std::size_t>::max()) {
                return false;
            }
            const auto* set = dynamic_cast<const SimSet*>(p.get());
            return !set || std::ranges::all_of(set->objects(), [&](auto* child) {
                return members.contains(child);
            });
        })) {
        return nullptr;
    }
    std::ranges::transform(objects, std::back_inserter(ordered), [](const auto& p) {
        return p.get();
    });
    std::ranges::stable_sort(ordered, {}, depth);
    owned_.reserve(owned_.size() + objects.size());
    bool success = true;
    dispatch([&] {
        const auto rollback = [&] {
            std::exception_ptr error;
            std::ranges::for_each(ordered | std::views::reverse, [&](auto* p) {
                if (p->manager_ == this && !p->removed_) {
                    try {
                        deleteObject(*p);
                    } catch (...) {
                        if (!error) {
                            error = std::current_exception();
                        }
                    }
                }
            });
            return error;
        };
        try {
            // All ownership is centralized before any user initialization runs.
            std::ranges::for_each(objects, [&](auto& p) {
                if (!p->id_) {
                    do {
                        p->id_ = allocateId();
                    } while (p->id_ && explicitIds.contains(p->id_));
                    p->dynamicId_ = true;
                }
                if (!p->id_) {
                    throw std::length_error("simulation IDs exhausted");
                }
                auto* pointer = p.get();
                owned_.push_back(std::move(p));
                pointer->manager_ = this;
                pointer->number_ = nextNumber_++;
                pointer->generation_ = nextGeneration_++;
                if (!ids_.insert(*pointer)) {
                    throw std::logic_error("adopted duplicate simulation ID");
                }
            });
            std::ranges::for_each(ordered, [&](auto* p) {
                if (!success || p->removed_) {
                    success = false;
                    return;
                }
                success = p->onAdd() && !p->removed_;
                if (success) {
                    p->added_ = true;
                }
            });
            std::ranges::for_each(ordered, [&](auto* p) {
                if (!success || p->removed_) {
                    success = false;
                    return;
                }
                if (!p->group_) {
                    success = SimGroup::addObject(*p) && !p->removed_;
                }
            });
            if (success && destination) {
                // Resolve again after onAdd/group callbacks. A refused target
                // retires the entire candidate graph, including set members.
                auto* current = dynamic_cast<SimSet*>(target.resolve(*this));
                success =
                    current && current->addObject(root) && !root.removed_ && !current->isRemoved();
            }
            if (success && accept) {
                success = accept(root) && !root.removed_;
            }
        } catch (...) {
            (void)rollback();
            throw;
        }
        if (!success) {
            if (const auto error = rollback()) {
                std::rethrow_exception(error);
            }
        }
    });
    return success ? &root : nullptr;
}

void SimManager::detach(SimObject& object) {
    object.removed_ = true;
    std::exception_ptr error;
    const auto attempt = [&](const auto& action) {
        try {
            action();
        } catch (...) {
            if (!error) {
                error = std::current_exception();
            }
        }
    };
    attempt([&] {
        object.onBeforeRemove();
    });
    if (auto* group = dynamic_cast<SimGroup*>(&object)) {
        std::vector<SimObject*> children(group->objects().begin(), group->objects().end());
        std::ranges::sort(children, std::greater{}, &SimObject::getNumber);
        std::ranges::for_each(children, [&](auto* child) {
            if (child->getManager() == this && !child->isRemoved()) {
                attempt([&] {
                    deleteObject(*child);
                });
            }
        });
    }
    attempt([&] {
        object.onRemove();
    });
    object.added_ = false;
    object.clearAllNotifications();
    if (object.group_) {
        attempt([&] {
            object.group_->removeObject(object);
        });
    }
    attempt([&] {
        object.notifyRemoval();
    });
    ids_.remove(object);
    queue_.remove(object.id_);
    if (object.dynamicId_) {
        generator_.free(object.id_);
        object.id_ = 0;
        object.dynamicId_ = false;
    }
    object.manager_ = nullptr;
    if (error) {
        std::rethrow_exception(error);
    }
}

std::unique_ptr<SimObject> SimManager::unregisterObject(SimObject& object) {
    if (dispatchDepth_ || object.manager_ != this || &object == this || object.removed_) {
        return {};
    }
    if (const auto* group = dynamic_cast<SimGroup*>(&object); group && !group->empty()) {
        return {};
    }
    std::exception_ptr error;
    dispatch([&] {
        try {
            detach(object);
        } catch (...) {
            error = std::current_exception();
        }
    });
    const auto it = std::ranges::find_if(owned_, [&](const auto& p) {
        return p.get() == &object;
    });
    auto result = std::move(*it);
    owned_.erase(it);
    if (error) {
        std::rethrow_exception(error);
    }
    return result;
}

bool SimManager::deleteObject(SimObject& object) {
    if (object.manager_ != this || &object == this || object.removed_) {
        return false;
    }
    retired_.reserve(retired_.size() + owned_.size());
    dispatch([&] {
        object.deleted_ = true;
        std::exception_ptr error;
        try {
            detach(object);
        } catch (...) {
            error = std::current_exception();
        }
        const auto it = std::ranges::find_if(owned_, [&](const auto& p) {
            return p.get() == &object;
        });
        retired_.push_back(std::move(*it));
        owned_.erase(it);
        if (error) {
            std::rethrow_exception(error);
        }
    });
    return true;
}

bool SimManager::assignId(SimObject& object, SimObjectId id) {
    if (object.manager_ != this || object.removed_ || &object == this) {
        return false;
    }
    if (id == object.id_ && id) {
        return true;
    }
    if (id && ids_.find(id)) {
        return false;
    }
    const auto replacement = id ? id : allocateId();
    if (!replacement) {
        return false;
    }
    queue_.remove(object.id_);
    ids_.remove(object);
    if (object.dynamicId_) {
        generator_.free(object.id_);
    }
    object.id_ = replacement;
    object.dynamicId_ = id == 0;
    object.generation_ = nextGeneration_++;
    ids_.insert(object);
    return true;
}

bool SimManager::setNetworkId(SimObjectId id) {
    if (id && (id < 2048 || id >= 3072)) {
        return false;
    }
    id_ = id;
    return true;
}

SimObject* SimManager::findObject(SimObjectId id) const {
    if (id == 0) {
        return const_cast<SimManager*>(this);
    }
    auto* object = ids_.find(id);
    return object && !object->isRemoved() ? object : nullptr;
}

bool SimManager::postEvent(std::unique_ptr<SimEvent> event, bool current) {
    if (!event || event->type < EventTypeFirst ||
        event->type >= EventTypeFirst + Int32{ReservedIdLimit} ||
        (event->address.managerId != 0 && event->address.managerId != getId())) {
        return false;
    }
    return queue_.insert(std::move(event), current);
}

bool SimManager::postEvents(std::vector<std::unique_ptr<SimEvent>> pending, bool current) {
    for (const auto& event : pending) {
        if (!event || event->type < EventTypeFirst ||
            event->type >= EventTypeFirst + Int32{ReservedIdLimit} ||
            (event->address.managerId && event->address.managerId != getId())) {
            return false;
        }
    }
    return queue_.insertBatch(std::move(pending), current);
}

std::size_t SimManager::advanceToTime(SimTime target, std::size_t budget) {
    std::scoped_lock lock(mutex_);
    if (advancing_ || !std::isfinite(target) || target < targetTime_) {
        throw std::invalid_argument("invalid simulation advance");
    }
    advancing_ = true;
    targetTime_ = target;
    std::size_t count{};
    try {
        while (count < budget) {
            auto event = queue_.pop(target);
            if (!event) {
                break;
            }
            ++count;
            if (auto* object = event->address.resolve(*this)) {
                dispatch([&] {
                    object->processEvent(*event);
                });
            }
        }
    } catch (...) {
        advancing_ = false;
        throw;
    }
    advancing_ = false;
    return count;
}

void SimManager::resetTime() {
    std::scoped_lock lock(mutex_);
    if (advancing_) {
        throw std::logic_error("cannot reset simulation during dispatch");
    }
    queue_.resetTime();
    targetTime_ = 0;
}

bool registerSimClasses(Persistent::ClassRegistry& registry) {
    return registry.registerClass<SimSet>("SimSet", makeFourCC('S', 'I', 'M', 'S')) &&
           registry.registerClass<SimGroup>("SimGroup", makeFourCC('S', 'I', 'M', 'G'));
}
} // namespace opent1::sim
