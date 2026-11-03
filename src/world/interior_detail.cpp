#include <opent1/world/interior_shape.hpp>
#include <ranges>
#include "collision_geometry.hpp"

namespace opent1::world {
std::optional<UInt32> InteriorShape::detailByPixels(UInt32 state, Flt32 pixels) const {
    if (states.size() > 65536 || lods.size() > 65536 || state >= states.size() ||
        !std::isfinite(pixels) || pixels < 0 ||
        Flt64(pixels) > std::numeric_limits<UInt32>::max()) {
        return {};
    }
    const auto& s = states[state];
    if (!s.lodCount || s.lodCount > 65536 || s.firstLod > lods.size() ||
        s.lodCount > lods.size() - s.firstLod) {
        return {};
    }
    const auto count = UInt32(pixels);
    for (auto i : std::views::iota(UInt32{0}) | std::views::take_while([&](UInt32 i) {
                      return i < s.lodCount;
                  })) {
        if (lods[s.firstLod + i].minPixels < count) {
            return s.firstLod + i;
        }
    }
    return s.firstLod + s.lodCount - 1;
}

std::optional<UInt32> InteriorShapeResources::selectDetail(UInt32 state,
                                                           const TMat3F& objectToCamera,
                                                           Flt32 focalLength,
                                                           bool linkRendering,
                                                           bool perspective) const {
    if (!shape || !collision_detail::rigid(objectToCamera) || !std::isfinite(focalLength) ||
        focalLength <= 0 || !shape->detailByPixels(state, 0)) {
        return {};
    }
    const auto& s = shape->states[state];
    const auto last = s.firstLod + s.lodCount - 1;
    if (last >= geometry.size() || !geometry[s.firstLod] || !geometry[last]) {
        return {};
    }
    const auto& high = geometry[s.firstLod]->bounds;
    const auto& low = geometry[last]->bounds;
    if (!collision_detail::valid(high) || !collision_detail::valid(low)) {
        return {};
    }
    auto inverse = objectToCamera;
    inverse.inverse();
    if (!collision_detail::finite(inverse.p)) {
        return {};
    }
    if (linkRendering || high.contains(inverse.p)) {
        return s.firstLod;
    }
    const auto extent = low.fMax - low.fMin;
    const auto radius = std::max({extent.x, extent.y, extent.z});
    Point3F center;
    m_mul((low.fMin + low.fMax) * .5f, objectToCamera, center);
    if (!collision_detail::finite(center)) {
        return {};
    }
    const auto pixels =
        2.f * (radius / (perspective ? std::max(center.y, .001f) : 1.f) * focalLength);
    // Float-to-UInt32 conversion is undefined beyond its range. Such a huge
    // projection conservatively selects highest detail, including overflow.
    if (Flt64(pixels) > std::numeric_limits<UInt32>::max()) {
        return s.firstLod;
    }
    return shape->detailByPixels(state, pixels);
}
} // namespace opent1::world
