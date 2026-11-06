#include "sim_test_support.hpp"
#include <ranges>
#include <opent1/sim/sim_world_game.hpp>
#include <opent1/core/mem_stream.hpp>
#include <atomic>
#include <limits>
#include <thread>

namespace {
struct Object : SimObject {
    std::function<void()> action;
    int* destroyed{};
    bool accepted{true};

    bool onAdd() override {
        return accepted;
    }

    ~Object() override {
        if (destroyed) {
            ++*destroyed;
        }
    }

    bool processEvent(const SimEvent&) override {
        if (action) {
            action();
        }
        return true;
    }
};

void graph() {
    SimManager manager;
    auto* a = add<Object>(manager);
    auto* b = add<Object>(manager);
    CHECK(a->getId() == 8192 && b->getId() == 8193);
    CHECK(a->getNumber() == 1);
    auto* g = add<SimGroup>(manager);
    g->assignName("Group");
    a->assignName("same");
    b->assignName("SAME");
    CHECK(g->addObject(*a));
    CHECK(g->addObject(*b));
    CHECK(g->findObject("same") == b);
    b->assignName("other");
    CHECK(g->findObject("same") == a);
    CHECK(manager.findObject("group/other") == b);
    CHECK(manager.findObject("\\Group\\same") == a);
    CHECK(manager.findObject(std::to_string(g->getId()) + "/same") == a);
    auto* subgroup = add<SimGroup>(manager);
    CHECK(g->addObject(*subgroup));
    CHECK(!subgroup->addObject(*g));
    auto* set = add<SimSet>(manager);
    CHECK(set->addObject(*g));
    CHECK(!subgroup->addObject(*set));
    CHECK(!g->addObject(*g));
    CHECK(set->addObject(*g) && set->size() == 1);
    CHECK(!a->setId(b->getId()));
    const auto old = a->address();
    CHECK(a->setId(100));
    CHECK(!old.resolve(manager));
    CHECK(manager.findObject(100) == a);
    CHECK(manager.findObject(0) == &manager);
    a->setFieldValue("VALUE", "123");
    CHECK(a->getFieldValue("value") == "123");
    a->setFieldValue("value", "");
    CHECK(!a->getFieldValue("value"));
    a->setLocked(true);
    CHECK(a->isLocked());
    CHECK(manager.tags().addEntry(100, "ID_TEST", "test"));
    SimTagDictionary tags(manager);
    CHECK(tags.getString(100) == "test" && tags.getDefine("id_test") == 100);
    CHECK(g->deleteObject());
    CHECK(set->empty());
    CHECK(!manager.findObject(100));
    SimManager other;
    CHECK(!other.tags().idToString(100));
    auto* last = add<Object>(manager);
    auto extracted = manager.unregisterObject(*last);
    CHECK(extracted && !extracted->getManager());
    CHECK(manager.addObject(std::move(extracted)) == last);
}

void borrowedAndOrdering() {
    SimGroup g;
    SimSet set;
    auto a = std::make_unique<Object>();
    auto b = std::make_unique<Object>();
    auto c = std::make_unique<Object>();
    a->assignName("a");
    CHECK(g.addObject(*a));
    CHECK(set.addObject(*a));
    CHECK(set.addObject(*b));
    CHECK(set.addObject(*c));
    CHECK(set.reOrder(*a, c.get()));
    CHECK(set.objects()[0] == b.get() && set.objects()[1] == a.get());
    CHECK(set.reOrder(*b));
    CHECK(set.objects().back() == b.get());
    a.reset();
    CHECK(g.empty() && set.size() == 2);
    set.setStable(true);
    CHECK(set.removeObject(*c));
    CHECK(set.objects()[0] == b.get());
    // More than every original name-dictionary rehash boundary, with duplicates.
    SimManager manager;
    auto* group = add<SimGroup>(manager);
    Object* newest{};
    for (auto i : std::views::iota(0, 5000)) {
        auto* item = add<Object>(manager);
        item->assignName(i % 10 ? std::to_string(i) : "duplicate");
        CHECK(group->addObject(*item));
        if (i % 10 == 0) {
            newest = item;
        }
    }
    CHECK(group->findObject("DUPLICATE") == newest);
    CHECK(manager.objectCount() == 5002);
}

void lifecycle() {
    SimManager manager;
    int destroyed{};
    auto bad = std::make_unique<Object>();
    bad->accepted = false;
    bad->destroyed = &destroyed;
    CHECK(!manager.addObject(std::move(bad)));
    CHECK(destroyed == 1 && manager.objectCount() == 1);
    auto* set = add<SimSet>(manager);
    auto* a = add<Object>(manager);
    auto* b = add<Object>(manager);
    CHECK(set->addObject(*a) && set->addObject(*b));
    b->destroyed = &destroyed;
    a->action = [&] {
        CHECK(b->deleteObject());
        CHECK(destroyed == 1);
        CHECK(a->deleteObject());
        CHECK(set->deleteObject());
    };
    const auto setId = set->getId();
    CHECK(manager.postEvent(eventAt(setId, 0), true));
    CHECK(manager.advanceToTime(0) == 1);
    CHECK(destroyed == 2 && manager.objectCount() == 1);

    struct Throwing : Object {
        void onRemove() override {
            throw std::runtime_error("remove");
        }
    };

    auto* item = add<Throwing>(manager);
    const auto id = item->getId();
    try {
        item->deleteObject();
        CHECK(false);
    } catch (const std::runtime_error&) {
    }
    CHECK(!manager.findObject(id));
    auto* throwing = add<Object>(manager);
    throwing->action = [] {
        throw std::runtime_error("event");
    };
    CHECK(throwing->postEvent(eventAt(0, 0), true));
    try {
        manager.advanceToTime(0);
        CHECK(false);
    } catch (const std::runtime_error&) {
    }
    throwing->action = {};
    CHECK(throwing->postEvent(eventAt(0, 0), true));
    CHECK(manager.advanceToTime(0) == 1);

    struct SelfAdd : Object {
        bool onAdd() override {
            deleteObject();
            return true;
        }
    };

    CHECK(!manager.addObject(std::make_unique<SelfAdd>()));

    struct GroupHook : Object {
        std::function<void()> hook;

        void onGroupRemove() override {
            if (hook) {
                hook();
            }
        }
    };

    auto* hook = add<GroupHook>(manager);
    auto* destination = add<SimGroup>(manager);
    hook->hook = [&] {
        hook->deleteObject();
    };
    CHECK(!destination->addObject(*hook));
    CHECK(destination->empty());
    auto* redirect = add<GroupHook>(manager);
    auto* alternate = add<SimGroup>(manager);
    redirect->hook = [&] {
        CHECK(alternate->addObject(*redirect));
    };
    CHECK(!destination->addObject(*redirect));
    CHECK(redirect->getGroup() == alternate);
    redirect->hook = {};
}

void queue() {
    // Local callbacks survive assignment/release of a client's network ID.
    SimManager persistent;
    auto* service = add<Object>(persistent);
    unsigned delivered{};
    service->action = [&] {
        ++delivered;
    };
    CHECK(service->postEvent(eventAt(0, 1)));
    CHECK(persistent.setNetworkId(2049));
    std::vector<std::unique_ptr<SimEvent>> batch;
    auto duringJoin = eventAt(service->getId(), 1);
    duringJoin->address.managerId = 2049;
    batch.push_back(std::move(duringJoin));
    CHECK(persistent.postEvents(std::move(batch)));
    CHECK(persistent.setNetworkId(0));
    CHECK(persistent.advanceToTime(1) == 2 && delivered == 2);

    SimEventQueue queue(4);
    CHECK(queue.insert(eventAt(3, 2)));
    CHECK(queue.insert(eventAt(1, 1)));
    CHECK(queue.insert(eventAt(2, 1)));
    CHECK(queue.pop(1)->address.objectId == 1);
    CHECK(queue.pop(1)->address.objectId == 2);
    CHECK(queue.getCurrentTime() == 1);
    queue.resetTime();
    CHECK(queue.pop(1)->address.objectId == 3);
    CHECK(!queue.insert(eventAt(1, 0)));
    CHECK(!queue.insert(eventAt(1, std::numeric_limits<float>::quiet_NaN())));
    CHECK(!queue.insert(eventAt(1, std::numeric_limits<float>::infinity())));
    CHECK(!queue.wait(std::chrono::milliseconds(0)));
    queue.signal();
    CHECK(queue.wait(std::chrono::milliseconds(0)));
    CHECK(!queue.wait(std::chrono::milliseconds(0)));
    SimEventQueue concurrent;
    std::array<std::jthread, 4> threads;
    std::ranges::for_each(threads, [&](auto& thread) {
        thread = std::jthread([&] {
            for ([[maybe_unused]] auto i : std::views::iota(0, 1000)) {
                CHECK(concurrent.insert(eventAt(1, 0), true));
            }
        });
    });
    std::ranges::for_each(threads, [](auto& thread) {
        thread.join();
    });
    CHECK(concurrent.size() == 4000);
    concurrent.remove(1);
    CHECK(concurrent.size() == 0);
    SimManager manager(1);
    auto* object = add<Object>(manager);
    CHECK(object->postEvent(eventAt(0, 1)));
    CHECK(!object->postEvent(eventAt(0, 1)));
    CHECK(object->deleteObject());
    CHECK(manager.eventQueue().size() == 0);
    auto* replacement = add<Object>(manager);
    auto stale = eventAt(replacement->getId(), 1);
    stale->address.generation = replacement->generation() - 1;
    int calls{};
    replacement->action = [&] {
        ++calls;
    };
    CHECK(manager.postEvent(std::move(stale)));
    CHECK(manager.advanceToTime(1) == 1 && calls == 0);
    replacement->action = [&] {
        ++calls;
        CHECK(replacement->postEvent(eventAt(0, 1), true));
    };
    CHECK(replacement->postEvent(eventAt(0, 1), true));
    CHECK(manager.advanceToTime(1, 10) == 10 && calls == 10);
}

void legacyScale() {
    SimManager manager;
    auto* receiver = add<Object>(manager);
    std::size_t calls{};
    receiver->action = [&] {
        ++calls;
    };
    std::vector<std::unique_ptr<SimEvent>> pending;
    for ([[maybe_unused]] auto i : std::views::iota(std::size_t{0}, std::size_t{100001})) {
        pending.push_back(eventAt(receiver->getId(), 0));
    }
    CHECK(manager.postEvents(std::move(pending)));
    CHECK(manager.advanceToTime(0, 10) == 10 && calls == 10);
    CHECK(manager.advanceToTime(0) == 99991 && calls == 100001);
    auto* root = add<SimGroup>(manager);
    auto* parent = root;
    for ([[maybe_unused]] auto i : std::views::iota(std::size_t{0}, std::size_t{200})) {
        auto* child = add<SimGroup>(manager);
        CHECK(parent->addObject(*child));
        parent = child;
    }
    CHECK(!parent->addObject(*root));
    CHECK(root->deleteObject());
    CHECK(manager.objectCount() == 2);
    SimManager bounded(1);
    CHECK(bounded.postEvent(eventAt(0, 0)));
    CHECK(!bounded.postEvent(eventAt(0, 0)));
    bounded.setDispatchDepthLimit(1);
    bool rejected{};
    bounded.dispatch([&] {
        try {
            bounded.dispatch([] {});
        } catch (const std::length_error&) {
            rejected = true;
        }
    });
    CHECK(rejected);
    bounded.setDispatchDepthLimit(std::numeric_limits<std::size_t>::max());
    SimWorld world;
    auto* worldReceiver = add<Object>(world.manager());
    std::size_t worldCalls{};
    worldReceiver->action = [&] {
        ++worldCalls;
    };
    std::vector<std::unique_ptr<SimEvent>> worldEvents;
    for ([[maybe_unused]] auto i : std::views::iota(std::size_t{0}, std::size_t{100001})) {
        worldEvents.push_back(eventAt(worldReceiver->getId(), 0));
    }
    CHECK(world.manager().postEvents(std::move(worldEvents)));
    CHECK(world.advanceToTime(0) >= 100001 && worldCalls == 100001);
}

void idsAndWire() {
    SimIdGenerator ids;
    CHECK(ids.alloc() == 0);
    CHECK(ids.addBlock(3));
    const auto a = ids.alloc(), b = ids.alloc(), c = ids.alloc();
    CHECK(a == 3072 && b == a + 1 && c == b + 1);
    CHECK(ids.free(b) && !ids.free(b));
    CHECK(ids.alloc() == b);
    CHECK(ids.free(c));
    CHECK(ids.alloc() == c);
    CHECK(!ids.free(1));
    CHECK(ids.numIdsUsed() == 3);
    Object object;
    SimIdBlockManager blocks;
    CHECK(blocks.alloc(object) == 0 && blocks.alloc(object) == 1);
    CHECK(blocks.findId(1024) == &object);
    blocks.free(object);
    CHECK(!blocks.findBlock(0));
    CHECK(blocks.alloc(object) == 0);
    for (const auto flag : {0U,
                            UInt32{SimEvent::ToGhost},
                            UInt32{SimEvent::ToGhostParent},
                            UInt32{SimEvent::ToGhost | SimEvent::ToGhostParent}}) {
        SimEvent event;
        event.flags = flag;
        event.address.objectId = flag ? 1023 : 0xf1234567;
        std::array<std::byte, 8> bytes{};
        BitStream writer{std::span(bytes)};
        CHECK(event.packAddress(writer));
        CHECK(writer.getCurPos() == (flag ? (flag & SimEvent::ToGhostParent ? 11 : 12) : 34));
        BitStream reader{std::span<const std::byte>(bytes)};
        SimEvent result;
        CHECK(result.unpackAddress(reader));
        CHECK(result.address.objectId == event.address.objectId);
        CHECK(reader.setCurPos(0));
        reader.setLimits(1, 0);
        const auto before = result.address;
        CHECK(!result.unpackAddress(reader));
        CHECK(result.address.objectId == before.objectId);
    }
}

void persistence() {
    Persistent::ClassRegistry registry;
    CHECK(registerSimClasses(registry));
    SimGroup group;
    SimSet a, b;
    a.assignName("Alpha");
    b.assignName("Beta");
    CHECK(group.addObject(a) && group.addObject(b));
    std::array<std::byte, 512> bytes{};
    MemRWStream writer{std::span(bytes)};
    Persistent::StoreContext store(registry);
    CHECK(store.store(writer, group) == Persistent::Error::Ok);
    const auto size = static_cast<std::size_t>(writer.getPosition());
    for (auto n : std::views::iota(std::size_t{0}) | std::views::take_while([&](std::size_t n) {
                      return n <= size;
                  })) {
        MemRWStream reader{std::span<const std::byte>(bytes).first(n)};
        Persistent::LoadContext load(registry);
        const auto loaded = load.load(reader);
        if (n < size) {
            CHECK(!loaded && load.size() == 0);
        } else {
            CHECK(loaded);
            auto* copy = dynamic_cast<SimGroup*>(loaded.object);
            CHECK(copy && copy->size() == 2);
            CHECK(copy->findObject("alpha"));
        }
    }
}
}

namespace {
void hostOwnedRendering() {
    struct Canvas : SimCanvas {
        unsigned renders{};

        bool setTitle(std::string_view) override {
            return true;
        }

        void renderFrame() override {
            ++renders;
        }
    };

    SimGame game;
    auto world = std::make_unique<SimWorld>();
    auto* canvas = add<Canvas>(world->manager());
    CHECK(game.setWorld(WorldType::Client, std::move(world)));
    game.frame(0, false);
    CHECK(canvas->renders == 0);
    game.frame(.1);
    CHECK(canvas->renders == 1);
    game.console().addCommand(
        0, "explicitRender", [canvas](Console&, Int32, std::span<const std::string>) {
            canvas->renderFrame();
            return std::string{};
        });
    CHECK(SimConsoleEvent::schedule(*canvas, "explicitRender();", .05f));
    game.frame(.2, false);
    CHECK(canvas->renders == 2);
    game.frame(.3);
    CHECK(canvas->renders == 3);
}
}

int opent1_test_sim_base() {
    hostOwnedRendering();
    legacyScale();
    graph();
    borrowedAndOrdering();
    lifecycle();
    queue();
    idsAndWire();
    persistence();
    return 0;
}

#include "test_registry.hpp"
OPENT1_REGISTER_TEST("simulation", "sim_base", opent1_test_sim_base);
