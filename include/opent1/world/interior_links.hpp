#pragma once
#include <opent1/world/interior_shape.hpp>

namespace opent1::world {
enum class InteriorFace : UInt32 {
    None = 0,
    MinX = 128,
    MinY = 64,
    MinZ = 32,
    MaxX = 16,
    MaxY = 8,
    MaxZ = 4,
    All = 252
};

struct InteriorLinkInstance {
    Resource<InteriorGeometry> geometry; // highest detail in the selected shape state
    TMat3F placement;
    UInt32 linkableFaces{UInt32(InteriorFace::All)};
    bool linked{}; // linked DIS instances require a contained root or a traversed link
    // Current rendered detail supplies outside bits. Boxes and object PVS keep
    // highest detail; an absent handle uses geometry for fixed single-LOD inputs.
    Resource<InteriorGeometry> detailGeometry{};
    bool forceRender{}; // native ForceEdit admits an uncontained linked root
};

struct InteriorLinkRoot {
    UInt32 instance{};
    bool cameraContained{};
};

struct InteriorLink {
    UInt32 instance{}, face{};
    bool operator==(const InteriorLink&) const = default;
};

struct InteriorLinkVisibility {
    std::vector<UInt32> instances; // depth-first, first-visit order
    bool outsideVisible{};
};

// Immutable owned graph. Runtime storage grows with the native scene;
// ScopedRenderLimits optionally bounds instances, nodes, links and traversal work.
// Only relative axis-aligned rigid placements can link, as assumed by the
// original two-corner box transform. Arbitrary rigid instances stay independent.
class InteriorLinkGraph {
public:
    [[nodiscard]] static std::optional<InteriorLinkGraph>
        prepare(std::span<const InteriorLinkInstance>);
    [[nodiscard]] std::span<const InteriorLink> links(UInt32 instance) const;
    [[nodiscard]] std::optional<InteriorLinkVisibility>
    visibility(Point3F worldCamera, std::span<const InteriorLinkRoot>) const;

private:
    friend class InteriorContainerGraph;
    std::vector<InteriorLinkInstance> instances_;
    std::vector<TMat3F> inverses_;
    std::vector<std::vector<InteriorLink>> links_;
};

// A snapshot of spatial parentage, independent of simulation ownership. Parents
// precede children; node zero is the open exterior root. A closed interior is an
// ordinary drawable leaf. Only open interiors participate in link traversal.
struct InteriorContainerNode {
    Box3F bounds;
    Int32 parent{-1}, interior{-1};
    UInt32 type{~UInt32{0}}, mask{~UInt32{0}};
    bool open{};
    bool pvs{};                // open interior contents use the native center-leaf database
    bool renderEligible{true}; // a coordinate database may reject a contents visit
};

struct InteriorContainerVisibility {
    std::vector<UInt32> containers;       // native traversal submission order
    std::vector<UInt32> linkedContainers; // submitted through a forced link
    bool outsideVisible{};
};

class InteriorContainerGraph {
public:
    [[nodiscard]] static std::optional<InteriorContainerGraph>
        prepare(std::span<const InteriorContainerNode>, std::span<const InteriorLinkInstance>);
    [[nodiscard]] std::optional<InteriorContainerVisibility>
    visibility(Point3F worldCamera, UInt32 cameraType = ~UInt32{0}) const;

private:
    InteriorLinkGraph links_;
    std::vector<InteriorContainerNode> nodes_;
    std::vector<std::vector<UInt32>> children_;
    std::vector<UInt32> interiorNodes_;
};
} // namespace opent1::world
