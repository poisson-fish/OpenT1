#include <opent1/sim/sim_world_game.hpp>
#include <algorithm>
#include <cmath>
#include <exception>
#include <ranges>
#include <stdexcept>

namespace opent1::sim {
namespace {
bool finite(const Point3F& p) {
    return std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z) &&
           std::max({std::abs(p.x), std::abs(p.y), std::abs(p.z)}) <= 1e12f;
}

const auto intensityTable = [] {
    std::array<UInt8, 256> result{};
    std::ranges::for_each(std::views::iota(std::size_t{0}, result.size()), [&](auto i) {
        auto root = static_cast<float>(std::sqrt(static_cast<double>(i) / 255.0));
        root *= 255.f;
        result[i] = static_cast<UInt8>(255U - static_cast<UInt8>(root));
    });
    return result;
}();
}

UInt8 SimDynamicLight::getIntensity(Flt32 distanceSquared) const {
    if (!std::isfinite(distanceSquared) || distanceSquared < 0) {
        throw std::invalid_argument("invalid dynamic light distance");
    }
    const auto biased = distanceSquared * (255.f / (radius_ * radius_));
    return biased > 255.f ? UInt8{0} : intensityTable[static_cast<UInt8>(biased)];
}

bool SimDynamicLight::setPosition(const Point3F& position) {
    if (updating_ || !finite(position)) {
        return false;
    }
    if ((position - position_).len() <= movementTolerance_) {
        return true;
    }
    position_ = position;
    updateLight();
    return true;
}

bool SimDynamicLight::setTransform(const TMat3F& transform) {
    if (updating_ || !finite(transform.p) || !std::ranges::all_of(transform.m, [](const auto& row) {
            return std::ranges::all_of(row, [](float v) {
                return std::isfinite(v) && std::abs(v) <= 1e6f;
            });
        })) {
        return false;
    }
    Point3F world;
    m_mul(position_, transform, world);
    if (!finite(world)) {
        return false;
    }
    transform_ = transform;
    updateLight();
    return true;
}

bool SimDynamicLight::setRadius(Flt32 radius) {
    if (updating_ || !std::isfinite(radius) || radius < 1e-6f || radius > 1e12f) {
        return false;
    }
    if (std::abs(radius - radius_) <= radiusTolerance_) {
        return true;
    }
    radius_ = radius;
    updateLight();
    return true;
}

bool SimDynamicLight::setPackedColor(UInt32 color) {
    if (updating_) {
        return false;
    }
    if (color == packedColor_) {
        return true;
    }
    packedColor_ = color;
    updateLight();
    return true;
}

bool SimDynamicLight::setTolerances(Flt32 movement, Flt32 radius) {
    if (updating_ || !std::isfinite(movement) || !std::isfinite(radius) || movement < 0 ||
        radius < 0) {
        return false;
    }
    movementTolerance_ = movement;
    radiusTolerance_ = radius;
    return true;
}

bool SimDynamicLight::onAdd() {
    updateLight();
    return !isRemoved();
}

void SimDynamicLight::onRemove() {
    clearAffectedObjects();
    SimObject::onRemove();
}

void SimDynamicLight::clearAffectedObjects() {
    if (updating_) {
        clearRequested_ = true;
        return;
    }
    auto* manager = getManager();
    if (!manager) {
        affected_.clear();
        return;
    }
    manager->dispatch([&] {
        auto previous = std::move(affected_);
        affected_.clear();
        SimDynamicLightDecRefCntEvent event;
        event.light = address();
        std::exception_ptr error;
        std::ranges::for_each(previous, [&](const auto& object) {
            try {
                manager->sendEvent(object, event);
            } catch (...) {
                if (!error) {
                    error = std::current_exception();
                }
            }
        });
        if (error) {
            std::rethrow_exception(error);
        }
    });
}

void SimDynamicLight::updateLight() {
    if (updating_) {
        throw std::logic_error("recursive dynamic light update");
    }
    Point3F world;
    m_mul(position_, transform_, world);
    if (!finite(world)) {
        throw std::overflow_error("dynamic light transform overflow");
    }
    box_ = {world - Point3F{radius_, radius_, radius_}, world + Point3F{radius_, radius_, radius_}};
    auto* manager = getManager();
    if (!manager) {
        return;
    }
    manager->dispatch([&] {
        struct Guard {
            bool& flag;
            ~Guard() {
                flag = false;
            }
        } guard{updating_};
        updating_ = true;
        clearRequested_ = false;
        std::vector<SimAddress> next;
        if (auto* root = dynamic_cast<SimContainer*>(manager->findObject(RootContainerId))) {
            SimContainerQuery q;
            q.id = getId();
            q.box = box_;
            SimContainerList found;
            root->findIntersections(q, found);
            std::ranges::for_each(found, [&](auto* o) {
                if (o->isOpen()) {
                    next.push_back(o->address());
                }
            });
        }
        const auto previous = std::move(affected_);
        affected_ = next;
        SimDynamicLightUpdateEvent update;
        update.light = address();
        update.worldPosition = world;
        update.radius = radius_;
        update.packedColor = packedColor_;
        SimDynamicLightDecRefCntEvent decrement;
        decrement.light = address();
        std::exception_ptr error;
        std::vector<SimAddress> delivered;
        std::ranges::for_each(next, [&](const auto& object) {
            if (clearRequested_ || isRemoved()) {
                return;
            }
            delivered.push_back(object);
            try {
                manager->sendEvent(object, update);
            } catch (...) {
                if (!error) {
                    error = std::current_exception();
                }
            }
        });
        const auto release = [&](const auto& list) {
            std::ranges::for_each(list, [&](const auto& object) {
                try {
                    manager->sendEvent(object, decrement);
                } catch (...) {
                    if (!error) {
                        error = std::current_exception();
                    }
                }
            });
        };
        release(previous);
        if (clearRequested_ || isRemoved()) {
            affected_.clear();
            release(delivered);
        }
        if (error) {
            std::rethrow_exception(error);
        }
    });
}
} // namespace opent1::sim
