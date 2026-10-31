#include <opent1/gui/plugin.hpp>

namespace opent1::gui {
Plugin::Plugin(std::shared_ptr<const Persistent::ClassRegistry> registry, Resources resources)
    : registry_(std::move(registry)), resources_(std::move(resources)) {
    if (!registry_) {
        throw std::invalid_argument("GUI plugin registry");
    }
}

Plugin::~Plugin() {
    shutdown();
}

void Plugin::init(Console& console, sim::SimManager* manager) {
    if (runtime_) {
        throw std::logic_error("GUI plugin already installed");
    }
    auto next = std::make_shared<Runtime>(console, registry_, resources_);
    next->install();
    runtime_ = std::move(next);
    manager_ = previousManager_ = manager;
    previous_ = manager ? manager->getCurrentTime() : 0;
    elapsed_ = 0;
    healthy_ = true;
}

void Plugin::setManager(sim::SimManager* manager) {
    manager_ = manager;
}

void Plugin::shutdown() noexcept {
    try {
        if (runtime_) {
            runtime_->stop();
        }
    } catch (...) {
    }
    runtime_.reset();
    manager_ = previousManager_ = nullptr;
}

void Plugin::startFrame() {
    if (runtime_) {
        healthy_ = runtime_->synchronizeVariables();
    }
}

void Plugin::endFrame() {
    if (!runtime_) {
        return;
    }
    const auto now = manager_ ? manager_->getCurrentTime() : 0;
    if (manager_ != previousManager_) {
        runtime_->canvas.cancel();
        previous_ = now;
        previousManager_ = manager_;
    }
    const auto delta = std::max(0.0, double(now) - previous_);
    previous_ = now;
    elapsed_ += delta;
    healthy_ = runtime_->canvas.advance(elapsed_) && healthy_;
}
} // namespace opent1::gui
