#include <opent1/world/world_scene.hpp>
#include <ranges>
#include <algorithm>
#include <cmath>

namespace opent1::world {
namespace {
bool normalizeMotion(TMat3F& target) {
    // Keep authored packed-quaternion rounding out of the simulation's rigid
    // instance transform. Large scale/shear/reflection remains invalid.
    if (!std::isfinite(target.determinant()) || std::abs(target.determinant() - 1) > .003f) {
        return false;
    }
    for (auto i : std::views::iota(std::size_t{0}, std::size_t{3})) {
        const Point3F a{target.m[i][0], target.m[i][1], target.m[i][2]};
        if (!std::isfinite(a.len()) || std::abs(m_dot(a, a) - 1) > .001f) {
            return false;
        }
        for (auto j : std::views::iota(std::size_t{0}) | std::views::take_while([&](std::size_t j) {
                          return j < i;
                      })) {
            const Point3F b{target.m[j][0], target.m[j][1], target.m[j][2]};
            if (std::abs(m_dot(a, b)) > .001f) {
                return false;
            }
        }
    }
    QuatF rotation(static_cast<const RMat3F&>(target));
    rotation.normalize();
    target = TMat3F(rotation, target.p);
    return bool(tsSceneLighting({}, target));
}
}

std::optional<SimTsShape::SpatialState> SimTsShape::spatialState(const TsShapePose& pose,
                                                                 Int32 detail) const {
    if (!pose.shape()) {
        return {};
    }
    SpatialState result;
    result.bounds = pose.shape()->bounds;
    const auto valid = [](const Box3F& box) {
        return box.isValid() && std::isfinite(box.fMin.len()) && std::isfinite(box.fMax.len()) &&
               box.fMin.len() <= 1e12f && box.fMax.len() <= 1e12f;
    };
    if (!valid(result.bounds)) {
        return {};
    }
    if (const auto posed = pose.bounds()) {
        result.bounds.fMin.setMin(posed->fMin);
        result.bounds.fMax.setMax(posed->fMax);
    }
    if (detail >= 0) {
        auto geometry = TsShapeCollision::prepare(pose, detail);
        if (!geometry) {
            return {};
        }
        if (const auto& bounds = geometry->bounds()) {
            result.bounds.fMin.setMin(bounds->fMin);
            result.bounds.fMax.setMax(bounds->fMax);
        }
        result.collision = std::make_shared<const TsShapeCollision>(std::move(*geometry));
    }
    return valid(result.bounds) ? std::optional(std::move(result)) : std::nullopt;
}

bool SimTsShape::applySpatialState(SpatialState state) {
    const auto previous = collision_;
    collision_ = std::move(state.collision);
    if (!setLocalBoundingBox(state.bounds)) {
        collision_ = previous;
        return false;
    }
    return !isDeleted() && !isRemoved();
}

bool SimTsShape::synchronizePose() {
    if (advancing_ || !instance_) {
        return false;
    }
    auto pose = instance_->pose();
    if (!pose) {
        return false;
    }
    auto spatial = spatialState(*pose, collisionDetail_);
    if (!spatial) {
        return false;
    }
    bool result = false;
    const auto work = [&] {
        struct Guard {
            bool& flag;
            explicit Guard(bool& value) : flag(value) {
                flag = true;
            }
            ~Guard() {
                flag = false;
            }
        } guard(advancing_);
        result = applySpatialState(std::move(*spatial));
    };
    if (auto* manager = getManager()) {
        manager->dispatch(work);
    } else {
        work();
    }
    return result;
}

bool SimTsShape::setCollisionDetail(Int32 detail, TsCollisionLevel level, bool trigger) {
    if (advancing_ || detail < -1 ||
        (level != TsCollisionLevel::Parts && level != TsCollisionLevel::Faces)) {
        return false;
    }
    std::optional<SpatialState> spatial;
    if (instance_) {
        auto pose = instance_->pose();
        if (!pose) {
            return false;
        }
        spatial = spatialState(*pose, detail);
        if (!spatial) {
            return false;
        }
    }
    bool result = false;
    const auto work = [&] {
        struct Guard {
            bool& flag;
            explicit Guard(bool& value) : flag(value) {
                flag = true;
            }
            ~Guard() {
                flag = false;
            }
        } guard(advancing_);
        const auto oldDetail = collisionDetail_;
        const auto oldLevel = collisionLevel_;
        const auto oldTrigger = collisionTrigger_;
        collisionDetail_ = detail;
        collisionLevel_ = level;
        collisionTrigger_ = trigger;
        if (spatial && !applySpatialState(std::move(*spatial))) {
            if (!isRemoved() && !isDeleted()) {
                collisionDetail_ = oldDetail;
                collisionLevel_ = oldLevel;
                collisionTrigger_ = oldTrigger;
            }
            return;
        }
        result = true;
    };
    if (auto* manager = getManager()) {
        manager->dispatch(work);
    } else {
        work();
    }
    return result;
}

bool SimTsShape::processCollision(MovementInfo& info) {
    if (!applyingRoot_) {
        return SimMovement::processCollision(info);
    }
    // Kinematic animation must block even with zero physical velocity and with
    // TS part-only contacts, which intentionally have no face normals.
    for (const auto& collision : info.collisionList) {
        if (std::ranges::none_of(info.collidedList, [&](const auto& address) {
                return address.objectId == collision.object.objectId &&
                       address.managerId == collision.object.managerId &&
                       address.generation == collision.object.generation;
            })) {
            info.collidedList.push_back(collision.object);
        }
    }
    return !info.collisionList.empty();
}

std::optional<TsAdvanceResult> SimTsShape::advanceAnimation(Flt64 seconds,
                                                            const TsAdvanceOptions& options) {
    if (advancing_ || !instance_ || !std::isfinite(seconds) || !std::isfinite(options.minStep) ||
        !std::isfinite(options.maxStep) || options.minStep <= 0 ||
        options.maxStep < options.minStep) {
        return {};
    }
    auto candidate = *instance_;
    if (!candidate.advance(seconds, options.bidirectionalTriggers)) {
        return {};
    }
    const auto pose = candidate.pose();
    if (!pose) {
        return {};
    }
    auto spatial = spatialState(*pose, collisionDetail_);
    if (!spatial) {
        return {};
    }
    auto target = getTransform();
    if (options.applyRootMotion) {
        m_mul(candidate.rootDelta(), getTransform(), target);
    }
    if (!normalizeMotion(target)) {
        return {};
    }
    // Check the worst retry step count before publishing animation or events.
    const auto distance = (target.p - getTransform().p).len();
    if (!std::isfinite(distance) || std::ceil(distance / options.minStep) > 65536) {
        return {};
    }
    std::optional<TsAdvanceResult> result;
    const auto work = [&] {
        struct Guard {
            bool& flag;
            explicit Guard(bool& value) : flag(value) {
                flag = true;
            }
            ~Guard() {
                flag = false;
            }
        } guard(advancing_);
        auto previous = *instance_;
        const auto previousCollision = collision_;
        instance_ = std::move(candidate);
        if (!applySpatialState(std::move(*spatial))) {
            if (!isRemoved() && !isDeleted()) {
                instance_ = std::move(previous);
                collision_ = previousCollision;
            }
            return;
        }
        const auto events =
            std::vector<TsTriggerEvent>(instance_->triggers().begin(), instance_->triggers().end());
        const auto shape = instance_->shape();
        TsAdvanceResult advanced;
        advanced.requestedTransform = target;
        {
            Guard moving(applyingRoot_);
            advanced.movementComplete =
                !options.applyRootMotion || stepPosition(target, options.minStep, options.maxStep);
        }
        // Spatial callbacks may remove us. dispatch() retains storage until all
        // guards and local resources are destroyed; removed targets get no events.
        advanced.appliedTransform = getTransform();
        for (const auto& trigger : events) {
            auto event = std::make_unique<SimTsAnimationTriggerEvent>();
            event->trigger = trigger;
            event->shape = shape;
            if (!isDeleted() && !isRemoved() && postEvent(std::move(event), true)) {
                ++advanced.triggersQueued;
            } else {
                ++advanced.triggersDropped;
            }
        }
        result = advanced;
    };
    if (auto* manager = getManager()) {
        manager->dispatch(work);
    } else {
        work();
    }
    return result;
}
} // namespace opent1::world
