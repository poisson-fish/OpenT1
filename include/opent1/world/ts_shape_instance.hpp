#pragma once
#include <opent1/world/ts_shape_pose.hpp>

namespace opent1::world {
enum class TsThreadState { Playing, Pending, Transition };

struct TsAnimationThread {
    Int32 sequence{};
    Flt32 position{}, timeScale{1};
    Int32 priority{};
    TsThreadState state{TsThreadState::Playing};
    Int32 transition{-1}, forwardTransition{-1}, backwardTransition{-1};
    std::shared_ptr<const TsTransition> runtimeTransition{};
    std::shared_ptr<const TsTransition> forwardRuntimeTransition{}, backwardRuntimeTransition{};

    [[nodiscard]] TsAnimationSample sample() const {
        return {sequence,
                position,
                state == TsThreadState::Transition ? transition : -1,
                priority,
                state == TsThreadState::Transition ? runtimeTransition : nullptr};
    }
};

struct TsTriggerEvent {
    UInt32 thread{};
    Int32 sequence{}, value{};
    bool forward{};
};

// Stateful authored TS playback, independent of renderers and simulation clocks.
// Thread indices stay stable (maximum 32). Resources must remain immutable.
class TsShapeInstance {
public:
    explicit TsShapeInstance(Resource<TsShape> shape);
    [[nodiscard]] std::optional<UInt32> addThread(Int32 sequence, Flt32 position = 0);
    bool setSequence(UInt32 thread, Int32 sequence, Flt32 position = 0);
    bool setTimeScale(UInt32 thread, Flt32 scale);
    bool setPriority(UInt32 thread, Int32 priority);
    // Select nearest reachable authored forward/backward transitions. No match
    // returns false; callers may explicitly fall back to setSequence().
    bool transitionToSequence(UInt32 thread, Int32 sequence);
    bool setTransition(UInt32 thread, Int32 transition);
    // Native SetTransition also accepts a caller-created record. Own a copy so
    // poses and instance copies never borrow a caller or mutable shape vector.
    bool setTransition(UInt32 thread, const TsTransition&);
    bool crossfade(UInt32 thread,
                   Int32 sequence,
                   Flt32 position,
                   Flt32 duration,
                   TsTransform displacement = {});
    bool cancelTransition(UInt32 thread); // pending only
    // Atomic: invalid metadata, overflow or exhausted work leaves all state,
    // motion and events unchanged. Zero-duration sequences hold their position.
    bool advance(Flt64 seconds, bool bidirectionalTriggers = false);
    bool advanceThread(UInt32 thread, Flt64 seconds, bool bidirectionalTriggers = false);
    [[nodiscard]] std::optional<TsShapePose> pose() const;

    [[nodiscard]] std::span<const TsAnimationThread> threads() const noexcept {
        return threads_;
    }

    [[nodiscard]] std::span<const TsTriggerEvent> triggers() const noexcept {
        return triggers_;
    }

    [[nodiscard]] const TMat3F& rootMotion() const noexcept {
        return rootMotion_;
    }

    [[nodiscard]] const TMat3F& rootDelta() const noexcept {
        return rootDelta_;
    }

    void resetRootMotion() noexcept {
        rootMotion_.identity();
        rootDelta_.identity();
    }

    [[nodiscard]] const Resource<TsShape>& shape() const noexcept {
        return shape_;
    }

private:
    bool advanceSelected(std::optional<UInt32>, Flt64, bool);
    Resource<TsShape> shape_;
    std::vector<TsAnimationThread> threads_;
    std::vector<TsTriggerEvent> triggers_;
    std::vector<Int32> materialRemap_;
    TMat3F rootMotion_, rootDelta_;
};
} // namespace opent1::world
