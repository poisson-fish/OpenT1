#include "test_registry.hpp"
#include "test_support.hpp"
#include <opent1/sim/sim_sdl_input.hpp>
#include <SDL3/SDL.h>

namespace {
using namespace opent1;

struct InputConsumer : sim::SimObject {
    unsigned events{};

    InputConsumer() {
        setId(sim::InputConsumerSetId);
    }

    bool processEvent(const sim::SimEvent& event) override {
        if (dynamic_cast<const sim::SimInputEvent*>(&event)) {
            ++events;
            return true;
        }
        return false;
    }
};

void inputServiceLifetime() {
    for (const bool removeService : {false, true}) {
        auto manager = std::make_unique<sim::SimManager>();
        auto* service = static_cast<sim::SimInputManager*>(
            manager->addObject(std::make_unique<sim::SimInputManager>()));
        auto* consumer = static_cast<InputConsumer*>(
            manager->addObject(std::make_unique<InputConsumer>()));
        CHECK(service && consumer);
        sim::SdlSimInput adapter(*service, 1);
        const platform::Event key{1, 0, platform::KeyEvent{SDL_SCANCODE_W, SDLK_W, 0, true}};
        CHECK(adapter.process(key));
        CHECK(consumer->events == 1);
        if (removeService) {
            CHECK(service->deleteObject());
            CHECK(manager->addObject(std::make_unique<sim::SimInputManager>()));
            CHECK(!adapter.process(key));
            CHECK(consumer->events == 1);
        }
        // Desktop GUI commands can replace the client world before the host
        // updates capture or binds its adapter to the replacement input service.
        manager.reset();
        adapter.release();
        adapter.capture(false);
        adapter.capture(true);
        CHECK(!adapter.process(key));
    }
}
}

int opent1_test_input_service_lifetime() {
    return opent1::test::run(inputServiceLifetime);
}

OPENT1_REGISTER_TEST("platform", "input_service_lifetime", opent1_test_input_service_lifetime);
