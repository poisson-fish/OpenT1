#pragma once
#include <opent1/world/interior_shape.hpp>

namespace opent1::world {
// Original persistent wire formats: DIG v7, DIS v3 and DIL v7. Geometry uses
// authored coordinates before boundary expansion. Validation is transactional;
// malformed input yields no partially written asset.
[[nodiscard]] std::optional<std::vector<std::byte>> writeInteriorGeometry(const InteriorGeometry&);
[[nodiscard]] std::optional<std::vector<std::byte>> writeInteriorShape(const InteriorShape&);
[[nodiscard]] std::optional<std::vector<std::byte>> writeInteriorLighting(const InteriorLighting&);

struct InteriorEditLod {
    UInt32 minPixels{}, linkableFaces{252};
    std::string geometry;
    std::vector<std::string> lightFiles;
};

struct InteriorEditState {
    std::string name;
    std::vector<InteriorEditLod> lods;
};

// ITRShapeEdit export: geometry/light extensions become DIG/DIL, each state's
// LODs sort by decreasing minimum pixels, and light profile references follow.
[[nodiscard]] std::optional<InteriorShape>
exportInteriorShape(std::span<const InteriorEditState>,
                    std::span<const std::string> lightStateNames,
                    std::string_view materialList,
                    bool linked = false);
} // namespace opent1::world
