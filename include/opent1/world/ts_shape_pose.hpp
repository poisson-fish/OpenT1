#pragma once
#include <opent1/world/ts_shape.hpp>

namespace opent1::world {
struct TsAnimationSample {
    Int32 sequence{};
    Flt32 position{};                // normalized, inclusive 0..1
    Int32 transition{-1};            // authored transition index; position is blend progress
    std::optional<Int32> priority{}; // thread override; otherwise authored sequence
    // Instance-owned external native transition; takes precedence over index.
    std::shared_ptr<const TsTransition> runtimeTransition{};
    // Cyclic time wraps (including negative time); other sequences clamp.
    // Zero-duration sequences hold at position zero.
    [[nodiscard]] static std::optional<TsAnimationSample>
    atTime(const TsShape&, Int32 sequence, Flt64 seconds);
};

struct TsObjectPose {
    Int32 frame{}, textureFrame{};
    bool visible{true};
};

// An owned snapshot of instance animation, independent of any renderer. Smaller
// authored priority values win; equal priorities use the earlier input sample.
// Object frame/material/visibility tracks override independently. At most 32
// samples and 4,194,304 track/key visits; invalid sampled tracks fail atomically.
class TsShapePose {
public:
    [[nodiscard]] static std::optional<TsShapePose>
        sample(Resource<TsShape>,
               std::span<const TsAnimationSample> = {},
               std::optional<std::span<const Int32>> materialRemap = std::nullopt);
    // One native AdvancePosition IFL update. Atomic; untouched slots persist.
    static bool advanceIfl(const TsShape&, const TsAnimationSample&, std::vector<Int32>&);
    // Root path evaluation avoids mesh/object tracks. Transitions interpolate
    // identity to their authored displacement, independently of the node pose.
    [[nodiscard]] static std::optional<TMat3F> sampleRoot(const TsShape&, const TsAnimationSample&);

    [[nodiscard]] const Resource<TsShape>& shape() const noexcept {
        return shape_;
    }

    [[nodiscard]] std::span<const TMat3F> nodeTransforms() const noexcept {
        return transforms_;
    }

    [[nodiscard]] std::span<const UInt8> nodeVisibility() const noexcept {
        return visible_;
    }

    [[nodiscard]] std::span<const TsObjectPose> objects() const noexcept {
        return objects_;
    }

    // Identity for default materials, with sampled IFL substitutions. Smaller
    // priority wins, then earlier sample, then later track within a sequence.
    [[nodiscard]] std::span<const Int32> materialRemap() const noexcept {
        return materialRemap_;
    }

    // Shape-space bounds of visible triangle vertices in one detail + always
    // hierarchy. Empty for no geometry or invalid inputs; capped at 4M corners.
    [[nodiscard]] std::optional<Box3F> bounds(Int32 detail = 0) const;

    // Absolute authored node-zero transform, kept separate from render geometry.
    // Accumulated root motion across cycles/transitions is not evaluated here.
    [[nodiscard]] const TMat3F& rootTransform() const noexcept {
        return root_;
    }

    // Replace a node's parent transform while retaining its sampled local pose
    // and all descendant animation, as used by the legacy player look override.
    bool overrideParentTransform(Int32 node, const TMat3F& parent);

private:
    Resource<TsShape> shape_;
    std::vector<TMat3F> transforms_;
    std::vector<UInt8> visible_;
    std::vector<TsObjectPose> objects_;
    std::vector<Int32> materialRemap_;
    TMat3F root_;
};
} // namespace opent1::world
