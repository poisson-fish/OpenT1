#pragma once
#include <opent1/sim/sim_base.hpp>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
using namespace opent1;
using namespace opent1::sim;
#define CHECK(...)                                                                                 \
    do {                                                                                           \
        if (!(__VA_ARGS__)) {                                                                      \
            std::fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #__VA_ARGS__);                 \
            std::abort();                                                                          \
        }                                                                                          \
    } while (false)

template <class T, class... Args> T* add(SimManager& manager, Args&&... args) {
    auto value = std::make_unique<T>(std::forward<Args>(args)...);
    auto* pointer = value.get();
    CHECK(manager.addObject(std::move(value)) == pointer);
    return pointer;
}

inline std::unique_ptr<SimEvent> eventAt(SimObjectId id, float time) {
    auto event = std::make_unique<SimEvent>();
    event->address.objectId = id;
    event->time = time;
    return event;
}
