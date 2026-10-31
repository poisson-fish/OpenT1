#include <opent1/world/ts_shape_pose.hpp>
#include <algorithm>
#include <cmath>
#include <ranges>

namespace opent1::world {
bool TsShapePose::overrideParentTransform(Int32 node, const TMat3F& parent) {
    if (!shape_ || node < 0 || std::size_t(node) >= transforms_.size()) {
        return false;
    }
    const auto previousParent = shape_->nodes[node].parent;
    if (previousParent < 0) {
        return false;
    }
    auto inverse = transforms_[previousParent];
    inverse.inverse();
    TMat3F delta;
    m_mul(inverse, parent, delta);
    for (std::size_t i = std::size_t(node); i < transforms_.size(); ++i) {
        auto ancestor = Int32(i);
        while (ancestor > node) {
            ancestor = shape_->nodes[ancestor].parent;
        }
        if (ancestor == node) {
            TMat3F transform;
            m_mul(transforms_[i], delta, transform);
            transforms_[i] = transform;
        }
    }
    return true;
}

namespace {
bool at(Int32 n, std::size_t size) {
    return n >= 0 && std::size_t(n) < size;
}

bool range(Int32 first, Int32 count, std::size_t size) {
    return count >= 0 && (count == 0 || (first >= 0 && std::size_t(first) <= size &&
                                         std::size_t(count) <= size - std::size_t(first)));
}

bool finite(Point3F p) {
    return std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z);
}

bool finite(const TMat3F& m) {
    return finite(m.p) && std::ranges::all_of(m.m, [](const auto& row) {
               return std::ranges::all_of(row, [](auto x) {
                   return std::isfinite(x);
               });
           });
}

bool valid(const TsShape& shape, const TsAnimationSample& s) {
    if (!at(s.sequence, shape.sequences.size()) || !std::isfinite(s.position) || s.position < 0 ||
        s.position > 1 || s.transition < -1) {
        return false;
    }
    if (s.transition < 0 && !s.runtimeTransition) {
        return true;
    }
    if (!s.runtimeTransition && !at(s.transition, shape.transitions.size())) {
        return false;
    }
    const auto& t =
        s.runtimeTransition ? *s.runtimeTransition : shape.transitions[std::size_t(s.transition)];
    return at(t.startSequence, shape.sequences.size()) &&
           at(t.endSequence, shape.sequences.size()) && std::isfinite(t.startPosition) &&
           t.startPosition >= 0 && t.startPosition <= 1 && std::isfinite(t.endPosition) &&
           t.endPosition >= 0 && t.endPosition <= 1;
}

struct Evaluator {
    const TsShape& shape;
    std::size_t remaining{4'194'304};
    bool ok{true};

    bool spend(std::size_t n) {
        if (n > remaining) {
            ok = false;
            return false;
        }
        remaining -= n;
        return true;
    }

    const TsSubSequence* track(Int32 first, Int32 count, Int32 sequence) {
        if (!range(first, count, shape.subSequences.size()) ||
            !spend(std::size_t(std::max(count, 0)))) {
            ok = false;
            return nullptr;
        }
        if (!count) {
            return nullptr;
        }
        const auto tracks =
            std::span(shape.subSequences).subspan(std::size_t(first), std::size_t(count));
        const auto found = std::ranges::find(tracks, sequence, &TsSubSequence::sequence);
        return found == tracks.end() ? nullptr : &*found;
    }

    bool controls(Int32 first, Int32 count, const TsAnimationSample& s) {
        if (s.transition < 0 && !s.runtimeTransition) {
            return track(first, count, s.sequence) != nullptr;
        }
        const auto& t = s.runtimeTransition ? *s.runtimeTransition
                                            : shape.transitions[std::size_t(s.transition)];
        return track(first, count, t.startSequence) != nullptr ||
               track(first, count, t.endSequence) != nullptr;
    }

    std::span<const TsKeyframe> keys(const TsSubSequence& sub) {
        if (sub.keyCount <= 0 || !range(sub.firstKey, sub.keyCount, shape.keyframes.size()) ||
            !spend(std::size_t(sub.keyCount))) {
            ok = false;
            return {};
        }
        const auto result = std::span(shape.keyframes)
                                .subspan(std::size_t(sub.firstKey), std::size_t(sub.keyCount));
        if (!std::ranges::all_of(result, [](const auto& key) {
                return std::isfinite(key.position) && key.position >= 0 && key.position <= 1;
            })) {
            ok = false;
            return {};
        }
        return result;
    }

    std::optional<TsTransform>
    transform(const TsNode& node, const TsAnimationSample* sample, bool& visible) {
        if (!at(node.defaultTransform, shape.transforms.size())) {
            ok = false;
            return {};
        }
        if (sample && (sample->transition >= 0 || sample->runtimeTransition)) {
            const auto& t = sample->runtimeTransition
                                ? *sample->runtimeTransition
                                : shape.transitions[std::size_t(sample->transition)];
            const TsAnimationSample a{t.startSequence, t.startPosition},
                b{t.endSequence, t.endPosition};
            bool ignored = true;
            const auto from = transform(node, &a, visible), to = transform(node, &b, ignored);
            if (!from || !to) {
                return {};
            }
            TsTransform result;
            result.rotation.interpolate(from->rotation, to->rotation, sample->position);
            result.translation =
                from->translation + (to->translation - from->translation) * sample->position;
            return result;
        }
        auto result = shape.transforms[std::size_t(node.defaultTransform)];
        if (sample) {
            const auto* sub = track(node.firstSubSequence, node.subSequenceCount, sample->sequence);
            if (!ok) {
                return {};
            }
            if (sub) {
                const auto list = keys(*sub);
                if (!ok) {
                    return {};
                }
                // Original transform lookup snaps within .001 of a key. Cel
                // lookup below deliberately has no such tolerance.
                // Preserve the original bounded binary bracket, including
                // duplicate keys and appended authoring tails in flyer.dts.
                Int32 previous = -1, next = Int32(list.size());
                auto check = std::min(Int32(list.size() - 1),
                                      Int32(sample->position * (Flt32(list.size()) - .01f)));
                while (previous + 1 < next) {
                    const auto position = list[std::size_t(check)].position;
                    if (position <= sample->position + .001f) {
                        previous = check;
                    }
                    if (position >= sample->position - .001f) {
                        next = check;
                    }
                    check = (previous + next) / 2;
                }
                const TsKeyframe *a{}, *b{};
                Flt32 t{};
                if (previous == next) {
                    a = b = &list[std::size_t(previous)];
                } else if (previous < 0 || next == Int32(list.size())) {
                    if (shape.sequences[std::size_t(sample->sequence)].cyclic) {
                        a = &list.back();
                        b = &list.front();
                        const auto duration = b->position + 1 - a->position;
                        const auto distance =
                            sample->position - a->position + (previous < 0 ? 1.f : 0.f);
                        t = duration > 0 ? distance / duration : 0;
                    } else {
                        a = b = previous < 0 ? &list.front() : &list.back();
                    }
                } else {
                    a = &list[std::size_t(previous)];
                    b = &list[std::size_t(next)];
                    if (b->position <= a->position) {
                        ok = false;
                        return {};
                    }
                    t = (sample->position - a->position) / (b->position - a->position);
                }
                if (!std::isfinite(t) || t < 0 || t > 1) {
                    ok = false;
                    return {};
                }
                if (!at(a->value, shape.transforms.size()) ||
                    !at(b->value, shape.transforms.size())) {
                    ok = false;
                    return {};
                }
                const auto& from = shape.transforms[a->value];
                const auto& to = shape.transforms[b->value];
                if (!finite(from.translation) || !finite(to.translation) ||
                    !std::isfinite(from.rotation.dot(from.rotation)) ||
                    !std::isfinite(to.rotation.dot(to.rotation))) {
                    ok = false;
                    return {};
                }
                result.rotation.interpolate(from.rotation, to.rotation, t);
                result.translation = from.translation + (to.translation - from.translation) * t;
                visible = (a->materialFlags & 0x8000U) != 0;
            }
        }
        if (!finite(result.translation) || !std::isfinite(result.rotation.dot(result.rotation))) {
            ok = false;
            return {};
        }
        return result;
    }
};

bool updateIfl(Evaluator& eval,
               const TsShape& shape,
               const TsAnimationSample& s,
               std::vector<Int32>& remap) {
    // The original thread updates the currently bound sequence at its
    // position even during a transition; IFL does not blend or switch at .5.
    const auto& seq = shape.sequences[std::size_t(s.sequence)];
    if (!range(seq.firstIfl, seq.iflCount, shape.subSequences.size()) ||
        !eval.spend(std::size_t(std::max(seq.iflCount, 0)))) {
        return false;
    }
    if (!seq.iflCount) {
        return true;
    }
    const auto tracks =
        std::span(shape.subSequences).subspan(std::size_t(seq.firstIfl), std::size_t(seq.iflCount));
    return std::ranges::all_of(tracks, [&](const auto& sub) {
        if (!at(sub.sequence, shape.sequences.size())) {
            return false;
        }
        const auto keys = eval.keys(sub);
        if (!eval.ok) {
            return false;
        }
        const auto next = std::ranges::find_if(keys, [&](const auto& k) {
            return k.position > s.position;
        });
        const auto& key =
            next == keys.begin()
                ? (shape.sequences[std::size_t(sub.sequence)].cyclic ? keys.back() : keys.front())
                : *(next - 1);
        const auto source = key.materialFlags & 0xfffU;
        if (remap.empty()) {
            return true;
        } // original disabled-remap policy
        if (source >= remap.size()) {
            return false;
        }
        // External DML lists are legal: target bounds are checked against
        // the actual list by the render adapter, not embedded materials.
        remap[source] = key.value;
        return true;
    });
}
} // namespace

std::optional<TsAnimationSample>
TsAnimationSample::atTime(const TsShape& shape, Int32 sequence, Flt64 seconds) {
    if (!at(sequence, shape.sequences.size()) || !std::isfinite(seconds)) {
        return {};
    }
    const auto& seq = shape.sequences[std::size_t(sequence)];
    if (!std::isfinite(seq.duration) || seq.duration < 0) {
        return {};
    }
    Flt64 position{};
    if (seq.duration > 0) {
        if (seq.cyclic) {
            position = std::fmod(seconds, Flt64(seq.duration)) / seq.duration;
            if (position < 0) {
                position += 1;
            }
        } else {
            position = std::clamp(seconds / Flt64(seq.duration), 0., 1.);
        }
    }
    return TsAnimationSample{sequence, Flt32(position)};
}

bool TsShapePose::advanceIfl(const TsShape& shape,
                             const TsAnimationSample& s,
                             std::vector<Int32>& remap) {
    if (!valid(shape, s) || shape.defaultMaterials < 0 || shape.defaultMaterials > 65536 ||
        remap.size() != std::size_t(shape.defaultMaterials)) {
        return false;
    }
    Evaluator eval{shape};
    auto next = remap;
    if (!updateIfl(eval, shape, s, next)) {
        return false;
    }
    remap = std::move(next);
    return true;
}

std::optional<TsShapePose>
TsShapePose::sample(Resource<TsShape> resource,
                    std::span<const TsAnimationSample> samples,
                    std::optional<std::span<const Int32>> materialRemap) {
    if (!resource || resource->nodes.empty() || resource->nodes.size() > 65536 ||
        resource->objects.size() > 65536 || samples.size() > 32) {
        return {};
    }
    const auto& shape = *resource;
    Evaluator eval{shape};
    std::vector<TsAnimationSample> ordered(samples.begin(), samples.end());
    if (!std::ranges::all_of(ordered, [&](const auto& s) {
            return valid(shape, s);
        })) {
        return {};
    }
    std::ranges::stable_sort(ordered, [&](const auto& a, const auto& b) {
        return a.priority.value_or(shape.sequences[std::size_t(a.sequence)].priority) <
               b.priority.value_or(shape.sequences[std::size_t(b.sequence)].priority);
    });
    TsShapePose result;
    result.transforms_.resize(shape.nodes.size());
    result.visible_.assign(shape.nodes.size(), 1);
    result.objects_.resize(shape.objects.size());
    if (shape.defaultMaterials < 0 || shape.defaultMaterials > 65536) {
        return {};
    }
    result.materialRemap_.resize(std::size_t(shape.defaultMaterials));
    std::ranges::copy(std::views::iota(Int32{0}, shape.defaultMaterials),
                      result.materialRemap_.begin());
    if (materialRemap) {
        if (materialRemap->size() != result.materialRemap_.size() ||
            !std::ranges::all_of(*materialRemap, [](Int32 slot) {
                return slot >= 0 && slot <= 65535;
            })) {
            return {};
        }
        std::ranges::copy(*materialRemap, result.materialRemap_.begin());
    } else {
        for (const auto& s : ordered | std::views::reverse) {
            if (!updateIfl(eval, shape, s, result.materialRemap_)) {
                return {};
            }
        }
    }
    if (!std::ranges::all_of(std::views::iota(std::size_t{0}, shape.nodes.size()), [&](auto i) {
            const auto& node = shape.nodes[i];
            if (node.parent != -1 && !at(node.parent, i)) {
                return false;
            }
            const auto active = std::ranges::find_if(ordered, [&](const auto& s) {
                return eval.controls(node.firstSubSequence, node.subSequenceCount, s);
            });
            bool visible = true;
            const auto local =
                eval.transform(node, active == ordered.end() ? nullptr : &*active, visible);
            if (!eval.ok || !local) {
                return false;
            }
            const TMat3F matrix(local->rotation, local->translation);
            if (i == 0) {
                result.root_ = matrix;
            }
            if (node.parent >= 0) {
                m_mul(matrix, result.transforms_[std::size_t(node.parent)], result.transforms_[i]);
            }
            // Root matrices remain identity; root motion is a separate caller policy.
            result.visible_[i] = UInt8(visible);
            return finite(matrix) && finite(result.transforms_[i]);
        })) {
        return {};
    }
    if (!std::ranges::all_of(std::views::iota(std::size_t{0}, shape.objects.size()), [&](auto i) {
            const auto& object = shape.objects[i];
            auto& pose = result.objects_[i];
            if (!at(object.node, shape.nodes.size()) || !at(object.mesh, shape.meshes.size())) {
                return false;
            }
            pose.visible = (object.flags & 1) == 0;
            if (!std::ranges::all_of(ordered | std::views::reverse, [&](const auto& s) {
                    auto sequence = s.sequence;
                    auto position = s.position;
                    if (s.transition >= 0 || s.runtimeTransition) {
                        const auto& t = s.runtimeTransition
                                            ? *s.runtimeTransition
                                            : shape.transitions[std::size_t(s.transition)];
                        sequence = s.position < .5f ? t.startSequence : t.endSequence;
                        position = s.position < .5f ? t.startPosition : t.endPosition;
                    }
                    const auto* sub =
                        eval.track(object.firstSubSequence, object.subSequenceCount, sequence);
                    if (!eval.ok) {
                        return false;
                    }
                    if (!sub) {
                        return true;
                    }
                    const auto keys = eval.keys(*sub);
                    if (!eval.ok) {
                        return false;
                    }
                    // Original cel lookup stops at the first later key. Some original
                    // flyer tracks append another run of keys; do not sort that tail.
                    const auto next = std::ranges::find_if(keys, [&](const auto& k) {
                        return k.position > position;
                    });
                    const auto& key =
                        next == keys.begin()
                            ? (shape.sequences[std::size_t(sequence)].cyclic ? keys.back()
                                                                             : keys.front())
                            : *(next - 1);
                    if (key.materialFlags & 0x1000U) {
                        pose.frame = key.value;
                    }
                    if (key.materialFlags & 0x2000U) {
                        pose.textureFrame = key.materialFlags & 0xfffU;
                    }
                    if (key.materialFlags & 0x4000U) {
                        pose.visible = (key.materialFlags & 0x8000U) != 0;
                    }
                    return true;
                })) {
                return false;
            }
            const auto& mesh = shape.meshes[std::size_t(object.mesh)];
            if (!at(pose.frame, mesh.frames.size())) {
                return false;
            }
            if (!mesh.faces.empty() &&
                (mesh.textureVerticesPerFrame < 0 ||
                 UInt64(pose.textureFrame) * UInt64(mesh.textureVerticesPerFrame) >=
                     mesh.textureVertices.size())) {
                return false;
            }
            return true;
        })) {
        return {};
    }
    result.shape_ = std::move(resource);
    return result;
}

std::optional<TMat3F> TsShapePose::sampleRoot(const TsShape& shape,
                                              const TsAnimationSample& sample) {
    if (shape.nodes.empty() || !valid(shape, sample)) {
        return {};
    }
    TsTransform transform;
    if (sample.transition >= 0 || sample.runtimeTransition) {
        const auto& t =
            (sample.runtimeTransition ? *sample.runtimeTransition
                                      : shape.transitions[std::size_t(sample.transition)])
                .transform;
        if (!finite(t.translation) || !std::isfinite(t.rotation.dot(t.rotation))) {
            return {};
        }
        transform.rotation.interpolate(QuatF{}, t.rotation, sample.position);
        transform.translation = t.translation * sample.position;
    } else {
        Evaluator eval{shape};
        bool visible = true;
        const auto value = eval.transform(shape.nodes[0], &sample, visible);
        if (!value || !eval.ok) {
            return {};
        }
        transform = *value;
    }
    TMat3F result(transform.rotation, transform.translation);
    const auto determinant = result.determinant();
    if (!finite(result) || !std::isfinite(determinant) || std::abs(determinant) < 1e-8f) {
        return {};
    }
    return result;
}

std::optional<Box3F> TsShapePose::bounds(Int32 detail) const {
    if (!shape_ || transforms_.size() != shape_->nodes.size() ||
        objects_.size() != shape_->objects.size() ||
        (shape_->details.empty() ? detail != 0 : !at(detail, shape_->details.size()))) {
        return {};
    }
    const auto& shape = *shape_;
    const auto root = shape.details.empty() ? 1 : shape.details[std::size_t(detail)].root;
    if (!at(root, shape.nodes.size()) ||
        (shape.alwaysNode != -1 && !at(shape.alwaysNode, shape.nodes.size()))) {
        return {};
    }
    std::vector<UInt8> selected(shape.nodes.size());
    if (!std::ranges::all_of(std::views::iota(std::size_t{0}, shape.nodes.size()), [&](auto i) {
            const auto parent = shape.nodes[i].parent;
            if (parent != -1 && !at(parent, i)) {
                return false;
            }
            selected[i] = UInt8(Int32(i) == root || Int32(i) == shape.alwaysNode ||
                                (parent >= 0 && selected[std::size_t(parent)]));
            return true;
        })) {
        return {};
    }
    std::optional<Box3F> result;
    std::size_t remaining = 4'194'304;
    if (!std::ranges::all_of(std::views::iota(std::size_t{0}, objects_.size()), [&](auto i) {
            const auto& object = shape.objects[i];
            const auto& state = objects_[i];
            if (!at(object.node, selected.size()) || !at(object.mesh, shape.meshes.size())) {
                return false;
            }
            if (!selected[std::size_t(object.node)] || !visible_[std::size_t(object.node)] ||
                !state.visible) {
                return true;
            }
            const auto& mesh = shape.meshes[std::size_t(object.mesh)];
            if (mesh.faces.size() > remaining / 3) {
                return false;
            }
            remaining -= mesh.faces.size() * 3;
            return std::ranges::all_of(mesh.faces, [&](const auto& face) {
                return std::ranges::all_of(face.corners, [&](const auto& corner) {
                    auto p = mesh.point(corner.vertex, state.frame);
                    if (!p) {
                        return false;
                    }
                    *p += object.offset;
                    m_mul(*p, transforms_[std::size_t(object.node)], *p);
                    if (!finite(*p)) {
                        return false;
                    }
                    if (!result) {
                        result = Box3F{*p, *p};
                    } else {
                        result->fMin.setMin(*p);
                        result->fMax.setMax(*p);
                    }
                    return true;
                });
            });
        })) {
        return {};
    }
    return result;
}
} // namespace opent1::world
