#pragma once
#include <opent1/world/world_surfaces.hpp>

namespace opent1::world {
// Preparation-local plan. Inputs are borrowed until the enclosing mesh build
// completes; no pointers from this plan are published in a mesh or scene frame.
class InteriorSceneLights {
public:
    static std::optional<InteriorSceneLights>
    prepare(const InteriorGeometry&, const InteriorLighting&, const TsLighting&, bool exterior);
    bool merge(UInt32 surface, std::span<UInt16> colors, std::size_t& work) const;
    // Native cache iteration order, for diagnostics and independent captures.
    [[nodiscard]] std::span<const Int16> pointLights(UInt32 surface) const;

private:
    const InteriorGeometry* geometry_{};
    const InteriorLighting* lighting_{};
    const TsLighting* scene_{};
    bool exterior_{};
    std::vector<std::array<Int16, 4>> points_;
    std::array<std::array<UInt16, 64>, 64> tables_{};
};
}
