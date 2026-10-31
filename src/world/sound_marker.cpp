#include <opent1/world/sound_marker.hpp>
#include <opent1/sim/sim_resource_volume.hpp>
#include <cmath>

namespace opent1::world {
bool installSoundMarker(sim::SimSoundSource& source, TsShapeResources resources) {
    if (!resources.shape || !std::isfinite(resources.shape->radius) ||
        resources.shape->radius < 0) {
        return false;
    }
    const auto center = resources.shape->center;
    const auto radius = resources.shape->radius;
    if (!std::isfinite(center.x) || !std::isfinite(center.y) || !std::isfinite(center.z)) {
        return false;
    }
    TsShapeInstance instance(resources.shape);
    auto pose = instance.pose();
    if (!pose) {
        return false;
    }
    WorldShapeImage image{std::move(resources), std::move(*pose), {}, 0, true, 0};
    std::function<bool(sim::SimQuery&, const TMat3F&)> query =
        [image = std::move(image)](sim::SimQuery& query, const TMat3F& transform) {
            auto* render = dynamic_cast<WorldShapeQuery*>(&query);
            if (!render) {
                return false;
            }
            auto next = image;
            next.shapeToWorld = transform;
            render->image = std::move(next);
            return true;
        };
    if (!source.setLocalBoundingBox({{-radius, -radius, -radius}, {radius, radius, radius}})) {
        return false;
    }
    source.markerCenter = center;
    source.markerRadius = radius;
    source.marker = std::move(query);
    return true;
}

std::shared_ptr<void>
installSoundSourceFactory(sim::SimGame& game,
                          std::function<bool(const sim::SimSoundSequenceEvent&)> broadcast,
                          std::string marker) {
    if (!broadcast || marker.empty() || marker.size() > 255 || marker.find('\0') != marker.npos) {
        throw std::invalid_argument("sound source resource host");
    }
    auto leases = std::make_shared<std::vector<std::shared_ptr<void>>>();
    leases->push_back(game.scopedLoadPreparation(
        "SimSoundSource", [marker, broadcast](sim::SimObject& object, ResourceManager& resources) {
            auto* source = dynamic_cast<sim::SimSoundSource*>(&object);
            if (!source) {
                return true;
            }
            auto shape = TsShapeResources::load(resources, marker);
            if (!shape || !installSoundMarker(*source, std::move(*shape))) {
                return false;
            }
            source->broadcast = broadcast;
            return true;
        }));
    leases->push_back(game.scopedFactory(
        "SimSoundSource",
        [&game, broadcast = std::move(broadcast), marker = std::move(marker)]()
            -> std::unique_ptr<sim::SimObject> {
            auto* manager = game.manager();
            auto* resources = manager ? sim::SimResource::find(*manager) : nullptr;
            if (!resources) {
                return {};
            }
            auto shape = TsShapeResources::load(resources->get(), marker);
            auto source = std::make_unique<sim::SimSoundSource>();
            if (!shape || !installSoundMarker(*source, std::move(*shape))) {
                return {};
            }
            source->broadcast = broadcast;
            return source;
        }));
    return leases;
}
}
