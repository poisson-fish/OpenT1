#pragma once
#include <opent1/world/interior_shape.hpp>

namespace opent1::world::interior_detail {
// Shared budgets keep nested container queries bounded as one operation.
std::optional<UInt32> leafAt(const InteriorGeometry&, Point3F, std::size_t& work);
std::optional<std::vector<UInt32>> visibleObjects(const InteriorGeometry&,
                                                  Point3F,
                                                  std::span<const Box3F>,
                                                  const TMat3F& worldToObject,
                                                  std::size_t& work);
}
