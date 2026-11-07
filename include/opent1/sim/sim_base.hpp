#pragma once
#include <opent1/console/tag_dictionary.hpp>
#include <opent1/core/bit_stream.hpp>
#include <opent1/core/persist.hpp>
#include <condition_variable>
#include <mutex>
#include <limits>
#include <unordered_map>
#include <unordered_set>

namespace opent1 {
class Console;
}

namespace opent1::sim {
using SimObjectId = UInt32;
using SimTime = Flt32;
inline constexpr UInt32 IdBlockShift = 10;
inline constexpr UInt32 ReservedIdLimit = 1U << IdBlockShift;
inline constexpr Int32 EventTypeFirst = 1024;
inline constexpr SimObjectId LookupSetId = 5, ResourceObjectId = 2, PreLoadManagerId = 48;
class SimObject;
class SimSet;
class SimGroup;
class SimManager;
class SimPreLoadManager;
class Inspect;

// Generation zero is an ID-only address (including addresses decoded from the
// wire). Local producers can retain a generation to reject recycled IDs.
struct SimAddress {
    SimObjectId objectId{}, managerId{};
    UInt64 generation{};
    [[nodiscard]] SimObject* resolve(SimManager&) const;
};

struct SimEvent {
    enum Flag : UInt32 {
        Guaranteed = 1,
        Priority = 2,
        Remote = 4,
        Ordered = 8,
        ToGhost = 16,
        ToGhostParent = 32
    };

    virtual ~SimEvent() = default;
    Int32 type{EventTypeFirst};
    UInt32 flags{};
    SimTime time{};
    SimAddress address;
    SimObjectId sourceManagerId{};
    bool packAddress(BitStream&) const;
    bool unpackAddress(BitStream&);
};

struct SimQuery {
    Int32 type{};
    virtual ~SimQuery() = default;
};

// Thread-safe queue with an optional capacity; callbacks and event destruction run outside its
// lock. Producers must stop before destruction. A signal is auto-reset on wait.
class SimEventQueue {
public:
    explicit SimEventQueue(std::size_t capacity = std::numeric_limits<std::size_t>::max())
        : capacity_(capacity) {}

    bool insert(std::unique_ptr<SimEvent>, bool currentEvent = false);
    bool insertBatch(std::vector<std::unique_ptr<SimEvent>>, bool currentEvent = false);
    std::unique_ptr<SimEvent> pop(SimTime target);
    void remove(SimObjectId);
    void resetTime();
    [[nodiscard]] SimTime getCurrentTime() const;
    [[nodiscard]] std::size_t size() const;
    void signal();
    bool wait(std::chrono::milliseconds timeout);

private:
    mutable std::mutex mutex_;
    std::condition_variable condition_;
    std::vector<std::unique_ptr<SimEvent>> events_;
    std::size_t capacity_;
    std::size_t first_{};
    SimTime currentTime_{};
    bool signaled_{};
};

class SimIdGenerator {
public:
    void reset();
    bool addBlock(UInt32 block); // previous block must be exhausted and freed pool empty
    [[nodiscard]] bool inRange(SimObjectId id) const;
    SimObjectId alloc(); // zero on exhaustion
    bool free(SimObjectId);

    [[nodiscard]] std::size_t numIdsUsed() const {
        return used_.size();
    }

private:
    UInt64 base_{}, limit_{}, next_{};
    std::vector<UInt32> blocks_;
    std::vector<SimObjectId> pool_;
    std::unordered_set<SimObjectId> used_;
};

class SimIdBlockManager {
public:
    UInt32 alloc(SimObject&);
    bool free(UInt32 block);
    void free(SimObject&);
    [[nodiscard]] SimObject* findBlock(UInt32) const;

    [[nodiscard]] SimObject* findId(SimObjectId id) const {
        return findBlock(id >> IdBlockShift);
    }

private:
    UInt32 next_{};
    std::vector<UInt32> pool_;
    std::unordered_map<UInt32, SimObject*> allocated_;
};

// Dictionaries borrow their objects. Names are bounded, ASCII-insensitive text;
// duplicate names resolve newest first, independent of hash-table growth.
class SimNameDictionary {
public:
    bool insert(SimObject&);
    void remove(SimObject&);
    [[nodiscard]] SimObject* find(std::string_view) const;

private:
    std::unordered_map<std::string, std::vector<SimObject*>> entries_;
};

class SimIdDictionary {
public:
    bool insert(SimObject&); // duplicate IDs are rejected
    void remove(SimObject&);
    [[nodiscard]] SimObject* find(SimObjectId) const;

private:
    std::unordered_map<SimObjectId, SimObject*> entries_;
};

// A manager exclusively owns every registered object. Groups provide exclusive
// parentage; sets borrow members. Unregistered sets/groups also borrow members
// (e.g. a Persistent::LoadContext owns a loaded graph). All graph operations need
// external synchronization; only posting to the event queue is producer-safe.
class SimObject : public Persistent::VersionedBase {
public:
    SimObject() = default;
    ~SimObject() override;
    SimObject(const SimObject&) = delete;
    SimObject& operator=(const SimObject&) = delete;

    virtual bool onAdd() {
        return true;
    }

    virtual void onBeforeRemove() {} // children still alive; removal already visible

    virtual void onRemove() {}

    virtual void onGroupAdd() {}

    virtual void onGroupRemove() {}

    virtual void onNameChange(std::string_view) {}

    virtual void onDeleteNotify(SimObject&) {}

    virtual void onPreLoad(SimPreLoadManager&) {}

    virtual bool inspectWrite(Inspect&) const;
    virtual bool inspectRead(Inspect&);
    virtual bool processEvent(const SimEvent&);

    virtual bool processQuery(SimQuery&) {
        return false;
    }

    // Called after registration/name assignment by newObject. Failure removes
    // the object; spans and strings are borrowed only for this invocation.
    // The preparation hook configures state that must precede onAdd (e.g.
    // volume scope flags) while the object still has exclusive local ownership.
    virtual bool prepareArguments(std::span<const std::string>) {
        return true;
    }

    virtual bool processArguments(std::span<const std::string> args) {
        return args.empty();
    }

    [[nodiscard]] SimObjectId getId() const {
        return id_;
    }

    [[nodiscard]] UInt64 getNumber() const {
        return number_;
    }

    [[nodiscard]] UInt64 generation() const {
        return generation_;
    }

    [[nodiscard]] std::string_view getName() const {
        return name_;
    }

    [[nodiscard]] SimManager* getManager() const {
        return manager_;
    }

    [[nodiscard]] SimGroup* getGroup() const {
        return group_;
    }

    [[nodiscard]] bool isProperlyAdded() const {
        return added_;
    }

    [[nodiscard]] bool isDeleted() const {
        return deleted_;
    }

    [[nodiscard]] bool isRemoved() const {
        return removed_;
    }

    [[nodiscard]] bool isDynamicId() const {
        return dynamicId_;
    }

    [[nodiscard]] UInt32 getType() const {
        return type_;
    }

    void setType(UInt32 type) {
        type_ = type;
    }

    [[nodiscard]] bool isSelected() const {
        return selected_;
    }

    [[nodiscard]] bool isExpanded() const {
        return expanded_;
    }

    void setSelected(bool value) {
        selected_ = value;
    }

    void setExpanded(bool value) {
        expanded_ = value;
    }

    [[nodiscard]] SimAddress address() const;
    bool setId(SimObjectId);
    void assignName(std::string_view);
    void setFieldValue(std::string_view slot, std::string_view value);
    [[nodiscard]] std::optional<std::string_view> getFieldValue(std::string_view slot) const;

    [[nodiscard]] const auto& dynamicFields() const noexcept {
        return fields_;
    }

    void setLocked(bool);
    [[nodiscard]] bool isLocked() const;
    void deleteNotify(SimObject&);
    void clearNotify(SimObject&);
    void clearAllNotifications();
    bool deleteObject();
    bool addToSet(SimObjectId);
    bool removeFromSet(SimObjectId);
    bool postEvent(std::unique_ptr<SimEvent>, bool currentEvent = false);

private:
    friend class SimSet;
    friend class SimGroup;
    friend class SimManager;
    void notifyRemoval();
    SimObjectId id_{};
    UInt64 number_{}, generation_{};
    SimManager* manager_{};
    SimGroup* group_{};
    std::string name_;
    bool added_{}, deleted_{}, removed_{}, dynamicId_{};
    UInt32 type_{1};
    bool selected_{}, expanded_{};
    std::unordered_map<std::string, std::string> fields_;
    std::vector<SimObject*> observers_, observed_;
};

class SimSet : public SimObject {
public:
    ~SimSet() override;

    // The legacy optional ownObjects argument is retired: manager ownership is
    // exclusive regardless of this compatibility argument.
    bool processArguments(std::span<const std::string> args) override {
        return args.size() <= 1;
    }

    virtual bool addObject(SimObject&);
    virtual bool removeObject(SimObject&);
    bool reOrder(SimObject&, SimObject* before = nullptr);

    void setStable(bool stable) {
        stable_ = stable;
    }

    [[nodiscard]] std::span<SimObject* const> objects() const {
        return objects_;
    }

    [[nodiscard]] std::size_t size() const {
        return objects_.size();
    }

    [[nodiscard]] bool empty() const {
        return objects_.empty();
    }

    bool processEvent(const SimEvent&) override;
    void onPreLoad(SimPreLoadManager&) override;
    void onDeleteNotify(SimObject&) override;
    Persistent::Error read(StreamIO&, Int32, Int32, Persistent::LoadContext&) override;
    Persistent::Error write(StreamIO&, Int32, Int32, Persistent::StoreContext&) const override;

protected:
    virtual bool acceptsObjectManager(const SimObject& object) const {
        return object.getManager() == getManager();
    }
    std::vector<SimObject*> objects_;
    bool stable_{};
};

class SimGroup : public SimSet {
public:
    ~SimGroup() override;

    bool processArguments(std::span<const std::string>) override {
        return true;
    }

    bool addObject(SimObject&) override;
    bool removeObject(SimObject&) override;
    [[nodiscard]] virtual SimObject* findObject(std::string_view) const;
    Persistent::Error read(StreamIO&, Int32, Int32, Persistent::LoadContext&) override;
    Persistent::Error write(StreamIO&, Int32, Int32, Persistent::StoreContext&) const override;

private:
    friend class SimObject;
    SimNameDictionary names_;
};

class SimManager : public SimGroup {
public:
    explicit SimManager(std::size_t eventCapacity = std::numeric_limits<std::size_t>::max());
    ~SimManager() override;
    using SimGroup::addObject;
    SimObject* addObject(std::unique_ptr<SimObject>, SimGroup* group = nullptr);
    // Consume one complete unregistered graph (e.g. a LoadContext transfer).
    // Publish identities before onAdd, retaining existing group/set links.
    // Failure removes every adopted object; application callback effects remain.
    SimObject* adoptObjects(std::vector<std::unique_ptr<SimObject>>,
                            SimObject& root,
                            SimSet* destination = nullptr,
                            const std::function<bool(SimObject&)>& accept = {});
    // Extraction requires an empty group and cannot occur inside a callback.
    std::unique_ptr<SimObject> unregisterObject(SimObject&);
    bool deleteObject(SimObject&);
    bool assignId(SimObject&, SimObjectId);
    // The manager identity is connection metadata, separate from local object IDs.
    bool setNetworkId(SimObjectId);
    using SimGroup::findObject;
    [[nodiscard]] SimObject* findObject(SimObjectId) const;
    bool postEvent(std::unique_ptr<SimEvent>, bool currentEvent = false);
    std::size_t advanceToTime(SimTime,
                              std::size_t eventBudget = std::numeric_limits<std::size_t>::max());
    // All event headers, times, capacity and allocations prepare before any
    // queue entry is published. Equal-time order follows the input vector.
    bool postEvents(std::vector<std::unique_ptr<SimEvent>>, bool currentEvent = false);
    void resetTime();

    [[nodiscard]] SimTime getCurrentTime() const {
        return queue_.getCurrentTime();
    }

    [[nodiscard]] SimTime getTargetTime() const {
        return targetTime_;
    }

    [[nodiscard]] std::size_t objectCount() const {
        return owned_.size();
    }

    SimEventQueue& eventQueue() {
        return queue_;
    }

    TagDictionary& tags() {
        return tags_;
    }

    const TagDictionary& tags() const {
        return tags_;
    }

    void setConsole(Console* console) {
        console_ = console;
    }

    [[nodiscard]] Console* console() const {
        return console_;
    }

    // The host registry supplies runtime class identity for native script callbacks.
    // It must outlive this manager, like the host console.
    void setClassRegistry(const Persistent::ClassRegistry* registry) {
        classRegistry_ = registry;
    }

    [[nodiscard]] const Persistent::ClassRegistry* classRegistry() const {
        return classRegistry_;
    }

    // Protect graph operations explicitly, including callbacks, with this mutex.
    std::recursive_mutex& mutex() {
        return mutex_;
    }

    // Keep removed callback targets alive until the outer dispatch completes.
    // As with graph edits, the caller holds mutex() when sharing a manager.
    void dispatch(const std::function<void()>&);

    void setDispatchDepthLimit(std::size_t limit) {
        dispatchDepthLimit_ = limit;
    }

    [[nodiscard]] bool isDispatching() const {
        return dispatchDepth_ != 0;
    }

    bool sendEvent(const SimAddress&, const SimEvent&);
    bool sendQuery(const SimAddress&, SimQuery&);

private:
    friend class SimSet;
    friend class SimGroup;
    friend class SimObject;
    void detach(SimObject&);
    SimObjectId allocateId();
    SimEventQueue queue_;
    SimIdDictionary ids_;
    std::vector<std::unique_ptr<SimObject>> owned_, retired_;
    SimIdGenerator generator_;
    UInt32 nextBlock_{9};
    UInt64 nextNumber_{}, nextGeneration_{1};
    SimTime targetTime_{};
    std::size_t dispatchDepth_{};
    std::size_t dispatchDepthLimit_{std::numeric_limits<std::size_t>::max()};
    bool advancing_{}, shuttingDown_{};
    TagDictionary tags_;
    Console* console_{}; // instance-owned host console; must outlive dispatch
    const Persistent::ClassRegistry* classRegistry_{};
    std::recursive_mutex mutex_;
};

// Instance-owned replacement for the old global forwarding facade.
class SimTagDictionary {
public:
    explicit SimTagDictionary(const SimManager& manager) : tags_(manager.tags()) {}

    [[nodiscard]] std::optional<std::string_view> getString(Int32 id) const {
        return tags_.idToString(id);
    }

    [[nodiscard]] Int32 getDefine(std::string_view text) const {
        return tags_.defineToId(text);
    }

    [[nodiscard]] std::optional<std::string_view> getDefineString(Int32 id) const {
        return tags_.idToDefine(id);
    }

private:
    const TagDictionary& tags_;
};

bool registerSimClasses(Persistent::ClassRegistry&);
} // namespace opent1::sim
