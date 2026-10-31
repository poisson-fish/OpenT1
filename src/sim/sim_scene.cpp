#include <opent1/sim/sim_world_game.hpp>
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace opent1::sim {
namespace {
bool valid(const VolumetricGeometry& geometry) {
    UInt16 mask{};
    return geometry.faces.size() <= 6 && geometry.materialName.size() <= 255 &&
           geometry.materialName.find('\0') == std::string::npos &&
           std::isfinite(geometry.opacity) && geometry.opacity >= 0 && geometry.opacity <= 1 &&
           std::ranges::all_of(geometry.faces, [&](const auto& face) {
               if (!face.face || face.face > 32 || (face.face & (face.face - 1)) ||
                   (mask & face.face) || face.vertices.size() < 3 || face.vertices.size() > 12) {
                   return false;
               }
               mask |= face.face;
               return std::ranges::all_of(face.vertices, [](const auto& vertex) {
                   return std::isfinite(vertex.position.x) && std::isfinite(vertex.position.y) &&
                          std::isfinite(vertex.position.z) && std::isfinite(vertex.texture.x) &&
                          std::isfinite(vertex.texture.y) && std::isfinite(vertex.haze) &&
                          vertex.haze >= 0 && vertex.haze <= 1;
               });
           });
}
}

std::vector<SimVolumetricRenderImage>
SimWorld::gatherVolumetrics(const VolumetricView& view,
                            std::optional<std::span<const SimAddress>> selection) {
    if (selection && selection->size() > 65536) {
        throw std::length_error("volumetric selection limit");
    }
    const auto bounded = [](Flt32 value) {
        return std::isfinite(value) && std::abs(value) <= 1e12f;
    };
    if (!bounded(view.cameraPosition.x) || !bounded(view.cameraPosition.y) ||
        !bounded(view.cameraPosition.z) || !bounded(view.visibleDistance) ||
        view.visibleDistance <= 0 || !bounded(view.hazeDistance) || view.hazeDistance < 0 ||
        view.hazeDistance >= view.visibleDistance) {
        throw std::invalid_argument("invalid volumetric view");
    }
    std::vector<SimVolumetricRenderImage> images;
    manager_.dispatch([&] {
        auto* set = dynamic_cast<SimSet*>(manager_.findObject(RenderSetId));
        if (!set) {
            return;
        }
        std::vector<SimAddress> sources;
        if (selection) {
            sources.assign(selection->begin(), selection->end());
        } else {
            std::ranges::transform(
                set->objects(), std::back_inserter(sources), [](const auto* object) {
                    return object->address();
                });
        }
        const auto time = manager_.getCurrentTime();
        std::ranges::for_each(sources, [&](const auto& address) {
            auto* object = address.resolve(manager_);
            if (set->isRemoved() || !object || object->isRemoved() ||
                std::ranges::find(set->objects(), object) == set->objects().end()) {
                return;
            }
            SimVolumetricRenderQuery query;
            query.view = view;
            if (!object->processQuery(query) || !query.geometry) {
                return;
            }
            if (!bounded(query.animationScale) || query.animationScale < 0 ||
                !valid(*query.geometry)) {
                throw std::invalid_argument("invalid volumetric render provider");
            }
            if (query.geometry->faces.empty()) {
                return;
            }
            if (images.size() >= 65536) {
                throw std::length_error("volumetric scene limit");
            }
            images.push_back({address, std::move(*query.geometry), time, query.animationScale});
        });
    });
    return images;
}
} // namespace opent1::sim
