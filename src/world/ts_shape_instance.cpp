#include <opent1/world/ts_shape_instance.hpp>
#include <algorithm>
#include <cmath>
#include <ranges>

namespace opent1::world {
namespace {
constexpr std::size_t MaxWork = 1'048'576, MaxEvents = 4096;

bool at(Int32 n, std::size_t size) {
    return n >= 0 && std::size_t(n) < size;
}

bool position(Flt32 p) {
    return std::isfinite(p) && p >= 0 && p <= 1;
}

bool finite(const TMat3F& m) {
    return std::isfinite(m.p.x) && std::isfinite(m.p.y) && std::isfinite(m.p.z) &&
           std::ranges::all_of(m.m, [](const auto& row) {
               return std::ranges::all_of(row, [](auto x) {
                   return std::isfinite(x);
               });
           });
}

bool sequence(const TsShape& s, Int32 i) {
    return at(i, s.sequences.size()) && std::isfinite(s.sequences[std::size_t(i)].duration) &&
           s.sequences[std::size_t(i)].duration >= 0;
}

bool transition(const TsShape& s, const TsTransition& t) {
    return sequence(s, t.startSequence) && sequence(s, t.endSequence) &&
           position(t.startPosition) && position(t.endPosition) && std::isfinite(t.duration) &&
           t.duration > 0;
}

bool transition(const TsShape& s, Int32 i) {
    return at(i, s.transitions.size()) && transition(s, s.transitions[std::size_t(i)]);
}

const TsTransition& edgeFor(const TsShape& s, const TsAnimationThread& t, Int32 index) {
    return t.runtimeTransition ? *t.runtimeTransition : s.transitions[std::size_t(index)];
}

struct Segment {
    TsAnimationSample from, to;
    Int32 repeats{};
};

using Path = std::vector<Segment>;

bool moveSequence(const TsShape& shape, TsAnimationThread& t, Flt32 delta, Path& path) {
    const auto from = t.sample();
    auto next = t.position + delta;
    Int32 repeats{};
    if (!std::isfinite(next)) {
        return false;
    }
    if (shape.sequences[std::size_t(t.sequence)].cyclic && (next < 0 || next > 1)) {
        const auto whole = std::floor(Flt64(next));
        if (std::abs(whole) > Flt64(MaxWork)) {
            return false;
        }
        repeats = Int32(whole);
        next = Flt32(Flt64(next) - whole);
    } else {
        next = std::clamp(next, 0.f, 1.f);
    }
    t.position = next;
    path.push_back({from, t.sample(), repeats});
    return true;
}

bool move(const TsShape& shape, TsAnimationThread& t, Flt64 seconds, Path& path) {
    // One pending edge, one transition, then one destination sequence. Clearing
    // pending edges on completion bounds this walk without recursive calls.
    for (int step = 0; step < 3; ++step) {
        if (!sequence(shape, t.sequence) || !position(t.position)) {
            return false;
        }
        if (t.state == TsThreadState::Transition) {
            if (!(t.runtimeTransition ? transition(shape, *t.runtimeTransition)
                                      : transition(shape, t.transition))) {
                return false;
            }
            const auto& edge = edgeFor(shape, t, t.transition);
            const auto delta = Flt32(seconds / Flt64(edge.duration));
            if (!std::isfinite(delta)) {
                return false;
            }
            const auto from = t.sample();
            const auto next = t.position + delta;
            if (!std::isfinite(next)) {
                return false;
            }
            t.position = std::clamp(next, 0.f, 1.f);
            path.push_back({from, t.sample(), 0});
            if (next >= 0 && next <= 1) {
                return true;
            }
            seconds = Flt64(next < 0 ? next : next - 1) * edge.duration;
            t.sequence = next < 0 ? edge.startSequence : edge.endSequence;
            t.position = next < 0 ? edge.startPosition : edge.endPosition;
            t.priority = shape.sequences[std::size_t(t.sequence)].priority;
            t.state = TsThreadState::Playing;
            t.transition = t.forwardTransition = t.backwardTransition = -1;
            t.runtimeTransition.reset();
            t.forwardRuntimeTransition.reset();
            t.backwardRuntimeTransition.reset();
        } else {
            const auto& seq = shape.sequences[std::size_t(t.sequence)];
            const auto delta = seq.duration > 0 ? Flt32(seconds / Flt64(seq.duration)) : 0.f;
            if (!std::isfinite(delta)) {
                return false;
            }
            const auto edgeIndex =
                delta > 0 ? t.forwardTransition : (delta < 0 ? t.backwardTransition : -1);
            const auto runtimeEdge = delta > 0   ? t.forwardRuntimeTransition
                                     : delta < 0 ? t.backwardRuntimeTransition
                                                 : nullptr;
            if (t.state == TsThreadState::Pending && (edgeIndex >= 0 || runtimeEdge)) {
                if (!(runtimeEdge ? transition(shape, *runtimeEdge)
                                  : transition(shape, edgeIndex))) {
                    return false;
                }
                const auto& edge =
                    runtimeEdge ? *runtimeEdge : shape.transitions[std::size_t(edgeIndex)];
                auto distance = (delta > 0 ? edge.startPosition : edge.endPosition) - t.position;
                if (seq.cyclic && delta > 0 && distance < 0) {
                    distance += 1;
                }
                if (seq.cyclic && delta < 0 && distance > 0) {
                    distance -= 1;
                }
                if ((delta > 0 && delta > distance) || (delta < 0 && delta < distance)) {
                    if (!moveSequence(shape, t, distance, path)) {
                        return false;
                    }
                    seconds = Flt64(delta - distance) * seq.duration;
                    t.transition = edgeIndex;
                    t.runtimeTransition = runtimeEdge;
                    t.state = TsThreadState::Transition;
                    t.position = delta > 0 ? 0.f : 1.f;
                    continue;
                }
            }
            return moveSequence(shape, t, delta, path);
        }
    }
    return false;
}

bool multiply(const TMat3F& a, const TMat3F& b, TMat3F& out) {
    m_mul(a, b, out);
    return finite(out);
}

bool addDelta(TMat3F from, const TMat3F& to, TMat3F& total) {
    const auto determinant = from.determinant();
    if (!finite(from) || !std::isfinite(determinant) || std::abs(determinant) < 1e-8f) {
        return false;
    }
    from.inverse();
    TMat3F delta;
    return multiply(to, from, delta) && multiply(delta, total, total);
}

bool accumulate(const TsShape& shape, const Segment& segment, TMat3F& total) {
    auto from = TsShapePose::sampleRoot(shape, segment.from),
         to = TsShapePose::sampleRoot(shape, segment.to);
    if (!from || !to) {
        return false;
    }
    if (!segment.repeats) {
        return addDelta(*from, *to, total);
    }
    auto start = segment.to, end = segment.to;
    start.position = segment.repeats > 0 ? 0.f : 1.f;
    end.position = 1 - start.position;
    const auto startMatrix = TsShapePose::sampleRoot(shape, start),
               endMatrix = TsShapePose::sampleRoot(shape, end);
    if (!startMatrix || !endMatrix || !addDelta(*from, *endMatrix, total)) {
        return false;
    }
    TMat3F cycle;
    if (!addDelta(*startMatrix, *endMatrix, cycle)) {
        return false;
    }
    auto count = UInt32(std::abs(segment.repeats) - 1);
    // Powers compose full cycles in logarithmic work, including rotation.
    while (count) {
        if ((count & 1U) && !multiply(cycle, total, total)) {
            return false;
        }
        count >>= 1;
        if (count && !multiply(cycle, cycle, cycle)) {
            return false;
        }
    }
    return addDelta(*startMatrix, *to, total);
}

std::optional<bool>
controlsRoot(const TsShape& shape, const TsAnimationThread& t, std::size_t& work) {
    const auto& node = shape.nodes[0];
    if (node.subSequenceCount < 0 || std::size_t(node.subSequenceCount) > work ||
        (node.subSequenceCount &&
         (node.firstSubSequence < 0 ||
          std::size_t(node.firstSubSequence) > shape.subSequences.size() ||
          std::size_t(node.subSequenceCount) >
              shape.subSequences.size() - std::size_t(node.firstSubSequence)))) {
        return {};
    }
    work -= std::size_t(node.subSequenceCount);
    for (Int32 i = 0; i < node.subSequenceCount; ++i) {
        const auto seq = shape.subSequences[std::size_t(node.firstSubSequence + i)].sequence;
        if (t.state == TsThreadState::Transition) {
            const auto& edge = edgeFor(shape, t, t.transition);
            if (seq == edge.startSequence || seq == edge.endSequence) {
                return true;
            }
        } else if (seq == t.sequence) {
            return true;
        }
    }
    return false;
}

bool collectTriggers(const TsShape& shape,
                     const Path& path,
                     UInt32 thread,
                     bool bidirectional,
                     std::size_t& work,
                     std::vector<TsTriggerEvent>& events) {
    for (const auto& s : path) {
        if (s.from.transition >= 0 || s.from.runtimeTransition ||
            (s.from.position == s.to.position && !s.repeats)) {
            continue;
        }
        const auto& seq = shape.sequences[std::size_t(s.from.sequence)];
        if (seq.triggerCount < 0 || std::size_t(seq.triggerCount) > work ||
            (seq.triggerCount &&
             (seq.firstTrigger < 0 || std::size_t(seq.firstTrigger) > shape.triggers.size() ||
              std::size_t(seq.triggerCount) >
                  shape.triggers.size() - std::size_t(seq.firstTrigger)))) {
            return false;
        }
        work -= std::size_t(seq.triggerCount);
        const bool forward = s.repeats > 0 || (!s.repeats && s.to.position > s.from.position);
        for (Int32 i = 0; i < seq.triggerCount; ++i) {
            const auto& trigger = shape.triggers[std::size_t(seq.firstTrigger + i)];
            if (!std::isfinite(trigger.position) || std::abs(trigger.position) > 1) {
                return false;
            }
            if (!bidirectional && forward != (trigger.position >= 0)) {
                continue;
            }
            const auto p = std::abs(trigger.position);
            auto after = p >= s.from.position, before = p < s.to.position;
            if (!forward) {
                after = !after;
                before = !before;
            }
            if (s.repeats ? (after || before) : (after && before)) {
                if (events.size() == MaxEvents) {
                    return false;
                }
                events.push_back({thread, s.from.sequence, trigger.value, forward});
            }
        }
    }
    return true;
}
} // namespace

TsShapeInstance::TsShapeInstance(Resource<TsShape> shape) : shape_(std::move(shape)) {
    if (shape_ && shape_->defaultMaterials >= 0 && shape_->defaultMaterials <= 65536) {
        for (Int32 i = 0; i < shape_->defaultMaterials; ++i) {
            materialRemap_.push_back(i);
        }
    }
}

std::optional<UInt32> TsShapeInstance::addThread(Int32 seq, Flt32 p) {
    if (!shape_ || threads_.size() >= 32 || !sequence(*shape_, seq) || !position(p)) {
        return {};
    }
    TsAnimationThread t;
    t.sequence = seq;
    t.position = p;
    t.priority = shape_->sequences[std::size_t(seq)].priority;
    threads_.push_back(t);
    invalidatePose();
    return UInt32(threads_.size() - 1);
}

bool TsShapeInstance::setSequence(UInt32 thread, Int32 seq, Flt32 p) {
    if (!shape_ || thread >= threads_.size() || !sequence(*shape_, seq) || !position(p)) {
        return false;
    }
    const auto scale = threads_[thread].timeScale;
    TsAnimationThread t;
    t.sequence = seq;
    t.position = p;
    t.timeScale = scale;
    t.priority = shape_->sequences[std::size_t(seq)].priority;
    threads_[thread] = t;
    invalidatePose();
    return true;
}

bool TsShapeInstance::setTimeScale(UInt32 thread, Flt32 scale) {
    if (thread >= threads_.size() || !std::isfinite(scale)) {
        return false;
    }
    threads_[thread].timeScale = scale;
    return true;
}

bool TsShapeInstance::setPriority(UInt32 thread, Int32 priority) {
    if (thread >= threads_.size()) {
        return false;
    }
    threads_[thread].priority = priority;
    invalidatePose();
    return true;
}

bool TsShapeInstance::cancelTransition(UInt32 thread) {
    if (thread >= threads_.size() || threads_[thread].state == TsThreadState::Transition) {
        return false;
    }
    auto& t = threads_[thread];
    t.state = TsThreadState::Playing;
    t.transition = t.forwardTransition = t.backwardTransition = -1;
    t.runtimeTransition.reset();
    t.forwardRuntimeTransition.reset();
    t.backwardRuntimeTransition.reset();
    invalidatePose();
    return true;
}

bool TsShapeInstance::setTransition(UInt32 thread, Int32 index) {
    if (!shape_ || thread >= threads_.size() || !transition(*shape_, index) ||
        threads_[thread].state == TsThreadState::Transition) {
        return false;
    }
    auto& t = threads_[thread];
    const auto& edge = shape_->transitions[std::size_t(index)];
    if (t.sequence != edge.startSequence && t.sequence != edge.endSequence) {
        return false;
    }
    // Reject unreachable noncyclic edges; the legacy caller otherwise walks
    // backwards to an edge already passed, even during forward playback.
    const bool forward = t.sequence == edge.startSequence;
    if (!shape_->sequences[std::size_t(t.sequence)].cyclic &&
        (forward ? edge.startPosition < t.position : edge.endPosition > t.position)) {
        return false;
    }
    if (t.state == TsThreadState::Playing) {
        t.forwardTransition = t.backwardTransition = -1;
        t.forwardRuntimeTransition.reset();
        t.backwardRuntimeTransition.reset();
    }
    if (forward) {
        t.forwardTransition = index;
        t.forwardRuntimeTransition.reset();
    } else {
        t.backwardTransition = index;
        t.backwardRuntimeTransition.reset();
    }
    t.state = TsThreadState::Pending;
    invalidatePose();
    return true;
}

bool TsShapeInstance::setTransition(UInt32 thread, const TsTransition& edge) {
    if (!shape_ || thread >= threads_.size() || !transition(*shape_, edge) ||
        !finite(TMat3F(edge.transform.rotation, edge.transform.translation)) ||
        threads_[thread].state == TsThreadState::Transition) {
        return false;
    }
    const auto& t = threads_[thread];
    if (t.sequence != edge.startSequence && t.sequence != edge.endSequence) {
        return false;
    }
    const bool forward = t.sequence == edge.startSequence;
    if (!shape_->sequences[std::size_t(t.sequence)].cyclic &&
        (forward ? edge.startPosition < t.position : edge.endPosition > t.position)) {
        return false;
    }
    auto owned = std::make_shared<const TsTransition>(edge);
    auto& next = threads_[thread];
    if (next.state == TsThreadState::Playing) {
        next.forwardTransition = next.backwardTransition = -1;
        next.forwardRuntimeTransition.reset();
        next.backwardRuntimeTransition.reset();
    }
    if (forward) {
        next.forwardRuntimeTransition = std::move(owned);
        next.forwardTransition = -1;
    } else {
        next.backwardRuntimeTransition = std::move(owned);
        next.backwardTransition = -1;
    }
    next.state = TsThreadState::Pending;
    invalidatePose();
    return true;
}

bool TsShapeInstance::crossfade(
    UInt32 thread, Int32 seq, Flt32 p, Flt32 duration, TsTransform displacement) {
    if (thread >= threads_.size()) {
        return false;
    }
    const auto& t = threads_[thread];
    return setTransition(thread,
                         TsTransition{t.sequence, seq, t.position, p, duration, displacement});
}

bool TsShapeInstance::transitionToSequence(UInt32 thread, Int32 seq) {
    if (!shape_ || thread >= threads_.size() || !sequence(*shape_, seq) ||
        shape_->transitions.size() > MaxWork ||
        threads_[thread].state == TsThreadState::Transition) {
        return false;
    }
    auto& t = threads_[thread];
    if (seq == t.sequence) {
        return cancelTransition(thread);
    }
    Int32 forward = -1, backward = -1;
    Flt32 forwardDistance = 2, backwardDistance = 2;
    for (std::size_t i = 0; i < shape_->transitions.size(); ++i) {
        const auto& edge = shape_->transitions[i];
        const bool f = edge.startSequence == t.sequence && edge.endSequence == seq;
        const bool b = edge.endSequence == t.sequence && edge.startSequence == seq;
        if (!f && !b) {
            continue;
        }
        if (!transition(*shape_, Int32(i))) {
            return false;
        }
        auto distance = f ? edge.startPosition - t.position : t.position - edge.endPosition;
        if (distance < 0 && shape_->sequences[std::size_t(t.sequence)].cyclic) {
            distance += 1;
        }
        if (distance < 0) {
            continue;
        }
        if (f && distance < forwardDistance) {
            forward = Int32(i);
            forwardDistance = distance;
        }
        if (b && distance < backwardDistance) {
            backward = Int32(i);
            backwardDistance = distance;
        }
    }
    if (forward < 0 && backward < 0) {
        return false;
    }
    t.runtimeTransition.reset();
    t.forwardRuntimeTransition.reset();
    t.backwardRuntimeTransition.reset();
    t.forwardTransition = forward;
    t.backwardTransition = backward;
    t.state = TsThreadState::Pending;
    invalidatePose();
    return true;
}

std::optional<TsShapePose> TsShapeInstance::pose() const {
    if (evaluatedPose_)
        return evaluatedPose_;
    std::vector<TsAnimationSample> samples;
    samples.reserve(threads_.size());
    for (const auto& thread : threads_) {
        samples.push_back(thread.sample());
    }
    evaluatedPose_ = TsShapePose::sample(shape_, samples, std::span<const Int32>(materialRemap_));
    return evaluatedPose_;
}

bool TsShapeInstance::advance(Flt64 seconds, bool bidirectional) {
    return advanceSelected(std::nullopt, seconds, bidirectional);
}

bool TsShapeInstance::advanceThread(UInt32 thread, Flt64 seconds, bool bidirectional) {
    if (thread >= threads_.size()) {
        return false;
    }
    return advanceSelected(thread, seconds, bidirectional);
}

bool TsShapeInstance::advanceSelected(std::optional<UInt32> selected,
                                      Flt64 seconds,
                                      bool bidirectional) {
    if (!shape_ || shape_->nodes.empty() || !std::isfinite(seconds)) {
        return false;
    }
    auto remap = materialRemap_;
    auto next = threads_;
    std::vector<Path> paths(next.size());
    std::vector<TsTriggerEvent> events;
    std::size_t work = MaxWork;
    std::optional<std::size_t> rootThread;
    for (std::size_t i = 0; i < next.size(); ++i) {
        if (!selected || *selected == i) {
            const auto delta = seconds * next[i].timeScale;
            if (!std::isfinite(delta) || !move(*shape_, next[i], delta, paths[i]) ||
                !collectTriggers(*shape_, paths[i], UInt32(i), bidirectional, work, events) ||
                !TsShapePose::advanceIfl(*shape_, next[i].sample(), remap)) {
                return false;
            }
        }
        const auto controlled = controlsRoot(*shape_, next[i], work);
        if (!controlled) {
            return false;
        }
        if (*controlled && (!rootThread || next[i].priority < next[*rootThread].priority)) {
            rootThread = i;
        }
    }
    TMat3F motionDelta, motion;
    if (rootThread) {
        for (const auto& segment : paths[*rootThread]) {
            if (!accumulate(*shape_, segment, motionDelta)) {
                return false;
            }
        }
    }
    if (!multiply(motionDelta, rootMotion_, motion)) {
        return false;
    }
    std::vector<TsAnimationSample> samples;
    for (const auto& t : next) {
        samples.push_back(t.sample());
    }
    const bool samePose =
        remap == materialRemap_ && samples.size() == threads_.size() &&
        std::ranges::equal(samples, threads_, [](const auto& sample, const auto& thread) {
            return sample == thread.sample();
        });
    auto evaluated = samePose && evaluatedPose_
                         ? evaluatedPose_
                         : TsShapePose::sample(shape_, samples, std::span<const Int32>(remap));
    if (!evaluated) {
        return false;
    }
    evaluatedPose_ = std::move(evaluated);
    materialRemap_ = std::move(remap);
    threads_ = std::move(next);
    triggers_ = std::move(events);
    rootDelta_ = motionDelta;
    rootMotion_ = motion;
    return true;
}
} // namespace opent1::world
