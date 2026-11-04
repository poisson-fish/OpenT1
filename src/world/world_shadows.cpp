#include <opent1/world/world_scene.hpp>
#include <opent1/world/shadow_receivers.hpp>
#include <opent1/sim/common_irc_shadow.hpp>
#include <opent1/gfx/render_stats.hpp>
#include <opent1/gfx/render_limits.hpp>
#include "collision_geometry.hpp"

namespace opent1::world {
namespace {
bool sameTransform(const TMat3F& a, const TMat3F& b) {
    return a.m == b.m && a.p == b.p && a.flags == b.flags;
}

bool sameGeometry(const WorldSurfaceInstance& a, const WorldSurfaceInstance& b) {
    if (!sameTransform(a.placement, b.placement))
        return false;
    if (a.mesh == b.mesh)
        return true;
    const auto& aMaterials = a.mesh->materials.list().materials;
    const auto& bMaterials = b.mesh->materials.list().materials;
    if (aMaterials.size() != bMaterials.size() || !std::equal(aMaterials.begin(),
                                                              aMaterials.end(),
                                                              bMaterials.begin(),
                                                              [](const auto& x, const auto& y) {
                                                                  return x.flags == y.flags;
                                                              }))
        return false;
    if (a.mesh->interiorMesh && a.mesh->interiorMesh == b.mesh->interiorMesh)
        return a.mesh->interiorSelection == b.mesh->interiorSelection;
    return a.mesh->terrainPatches && a.mesh->terrainPatches == b.mesh->terrainPatches;
}
}

struct WorldShadowContext::State {
    struct Entry {
        UInt64 caster{};
        UInt32 part{};
        UInt64 seen{};
        std::weak_ptr<const TsShape> shape;
        UInt64 shapeRevision{};
        TMat3F geometryTransform;
        Point3F geometryLight;
        std::array<float, 4> geometryCrop{};
        UInt64 receiverRevision{};
        bool geometryAttempted{};
        std::optional<sim::ShadowGeometry> geometry;
        std::array<std::array<float, 3>, 3> maskRotation{};
        Point3F maskLight;
        Int32 detail{}, dimension{};
        UInt64 poseRevision{};
        double updated{};
        bool silhouetteAttempted{}, maskValid{};
        std::vector<UInt8> pixels;
        sim::ShadowBitmap bitmap;
        float bitmapAlpha{-1};
        float lastProjectedSize{};
    };

    ShadowReceiverCache indexes;
    std::vector<WorldSurfaceInstance> receivers;
    std::vector<Entry> entries;
    UInt64 receiverRevision{}, tick{};
    double seconds{-1};
};

WorldShadowContext::WorldShadowContext() : state_(std::make_unique<State>()) {}

WorldShadowContext::~WorldShadowContext() = default;

void WorldShadowContext::clear() {
    state_->indexes.clear();
    state_->receivers.clear();
    state_->entries.clear();
    state_->receiverRevision = state_->tick = 0;
    state_->seconds = -1;
}

std::optional<std::vector<WorldShadow>> WorldSceneFrame::shadows(Point3F direction,
                                                                 float opacity,
                                                                 WorldShadowContext* context,
                                                                 double seconds) const {
    using namespace collision_detail;
    if (!finite(direction) || direction.len() < .0001f || !std::isfinite(opacity) || opacity < 0 ||
        opacity > 1 || !std::isfinite(seconds))
        return {};
    direction.normalize();
    auto cameraToWorld = camera_.worldToCamera;
    cameraToWorld.inverse();
    Point3F cameraDirection;
    cameraToWorld.getRow(1, cameraDirection);
    auto* storage = context ? context->state_.get() : nullptr;
    if (storage) {
        if (seconds < storage->seconds)
            context->clear();
        storage->seconds = seconds;
        ++storage->tick;
        if (storage->receivers.size() != surfaces_.size() || !std::equal(storage->receivers.begin(),
                                                                         storage->receivers.end(),
                                                                         surfaces_.begin(),
                                                                         sameGeometry)) {
            storage->receivers = surfaces_;
            ++storage->receiverRevision;
        }
        std::erase_if(storage->entries, [&](const auto& entry) {
            return entry.shape.expired() || storage->tick - entry.seen > 120;
        });
    }
    const auto findEntry = [&](const WorldShapeImage& image) -> WorldShadowContext::State::Entry* {
        if (!storage || !image.shadowCaster)
            return nullptr;
        for (auto& entry : storage->entries)
            if (entry.caster == image.shadowCaster && entry.part == image.shadowPart)
                return &entry;
        return nullptr;
    };

    struct Caster {
        const WorldShapeImage* image;
        float distance{}, size{};
    };

    std::vector<Caster> casters;
    for (const auto& image : shapeInputs_) {
        if (!image.castShadow || image.firstPerson || image.haze > .2f || image.opacity <= 0 ||
            !image.resources.shape)
            continue;
        const auto distance = (image.shapeToWorld.p - cameraToWorld.p).len();
        const auto size = 2 * image.resources.shape->radius *
                          std::max(camera_.focal.x, camera_.focal.y) /
                          std::max(distance, camera_.nearDistance);
        // Hysteresis only applies to an already visible caster; the stateless
        // API remains an exact, unscheduled snapshot renderer for tools/tests.
        const auto* previous = findEntry(image);
        if (storage && size < (previous && previous->seen + 1 == storage->tick ? 3.f : 4.f)) {
            if (gfx::currentRenderWorkStats)
                ++gfx::currentRenderWorkStats->shadowSkipped;
            continue;
        }
        casters.push_back({&image, distance, size});
    }
    std::ranges::stable_sort(casters, [&](const auto& a, const auto& b) {
        if (storage && a.size != b.size)
            return a.size > b.size;
        if (a.distance != b.distance)
            return a.distance < b.distance;
        if (a.image->shadowCaster != b.image->shadowCaster)
            return a.image->shadowCaster < b.image->shadowCaster;
        return a.image->shadowPart < b.image->shadowPart;
    });
    const auto maximum = std::min(std::size_t{32}, gfx::renderLimits().shadows);
    std::size_t work = MaxWork;
    std::vector<WorldShadow> result;
    result.reserve(std::min(maximum, casters.size()));
    if (casters.empty() || maximum == 0)
        return result;
    const ShadowReceiverScene receivers(surfaces_, storage ? &storage->indexes : nullptr);
    for (const auto& caster : casters | std::views::take(maximum)) {
        try {
            const auto& image = *caster.image;
            const auto& shape = *image.resources.shape;
            if (gfx::currentRenderWorkStats)
                ++gfx::currentRenderWorkStats->shadowCasters;
            auto* entry = findEntry(image);
            if (!entry && storage && image.shadowCaster) {
                if (storage->entries.size() == 128) {
                    const auto oldest = std::ranges::min_element(
                        storage->entries, {}, &WorldShadowContext::State::Entry::seen);
                    storage->entries.erase(oldest);
                }
                storage->entries.emplace_back();
                entry = &storage->entries.back();
                entry->caster = image.shadowCaster;
                entry->part = image.shadowPart;
            }
            WorldShadowContext::State::Entry temporary;
            if (!entry)
                entry = &temporary;
            const bool sameShape = entry->shape.lock().get() == image.resources.shape.get() &&
                                   entry->shapeRevision == image.resources.shape.revision();
            if (!sameShape) {
                entry->geometryAttempted = entry->silhouetteAttempted = false;
                entry->geometry.reset();
                entry->maskValid = false;
                entry->shape = image.resources.shape.immutable();
                entry->shapeRevision = image.resources.shape.revision();
            }
            if (storage)
                entry->seen = storage->tick;
            auto shapeToLocal = image.shapeToWorld;
            shapeToLocal.p = {};
            Point3F center;
            m_mul(shape.center, static_cast<const RMat3F&>(shapeToLocal), center);
            const auto window = sim::makeShadowSourceWindow(
                direction, center, shape.radius, shape.bounds, shapeToLocal);
            if (window.radius <= .0001f)
                continue;
            // Conservative facing keeps resolution independent of receiver
            // discovery. Cadence still follows projected size; the legacy box
            // filter and full bitmap remain unchanged by footprint cropping.
            const auto settings = sim::chooseShadowDetails(std::max(0.f, caster.size), 1.f);
            auto dimension = storage ? settings.bitmapDimension : 64;
            if (storage && entry->dimension && caster.size >= entry->lastProjectedSize * .85f &&
                caster.size <= entry->lastProjectedSize * 1.15f)
                dimension = entry->dimension;
            const bool maskKey = entry->silhouetteAttempted &&
                                 entry->maskRotation == shapeToLocal.m &&
                                 entry->maskLight == direction && entry->detail == image.detail &&
                                 entry->dimension == dimension;
            const bool silhouetteHit =
                maskKey && (entry->poseRevision == image.pose.revision() ||
                            (storage && seconds - entry->updated < settings.updateDelta));
            if (silhouetteHit) {
                if (gfx::currentRenderWorkStats)
                    ++gfx::currentRenderWorkStats->shadowSilhouetteHits;
            } else {
                gfx::ScopedRenderWorkTimer timer(&gfx::RenderWorkStats::shadowSilhouetteMs);
                entry->maskRotation = shapeToLocal.m;
                entry->maskLight = direction;
                entry->detail = image.detail;
                entry->dimension = dimension;
                entry->poseRevision = image.pose.revision();
                entry->updated = seconds;
                entry->lastProjectedSize = caster.size;
                entry->silhouetteAttempted = true;
                entry->maskValid = false;
                entry->bitmapAlpha = -1;
                TMat3F lightToWorld;
                lightToWorld.setRow(0, window.right);
                lightToWorld.setRow(1, window.direction);
                lightToWorld.setRow(2, window.up);
                lightToWorld.p = (window.points[0] + window.points[2]) * .5f;
                lightToWorld.flags = RMat3F::Matrix_HasRotation | RMat3F::Matrix_HasTranslation;
                auto worldToLight = lightToWorld;
                worldToLight.inverse();
                TsStaticView view;
                m_mul(shapeToLocal, worldToLight, view.shapeToCamera);
                view.width = view.height = dimension + 2;
                view.center = {float(view.width) * .5f, float(view.height) * .5f};
                view.focal = {dimension / (2 * window.radius), dimension / (2 * window.radius)};
                view.perspective = false;
                view.twoSided = true;
                view.nearDistance = .001f;
                view.farDistance = 65536;
                view.detail = image.detail;
                const auto frame = TsStaticShapeFrame::prepare(image.resources, view, image.pose);
                if (!frame)
                    continue;
                const auto mask = frame->silhouette();
                if (!mask)
                    continue;
                entry->pixels.resize(std::size_t(view.width) * view.height);
                for (Int32 y = 0; y < view.height; ++y)
                    for (Int32 x = 0; x < view.width; ++x)
                        entry->pixels[std::size_t(y) * view.width + x] =
                            std::to_integer<UInt8>(mask->row(y)[x]);
                entry->maskValid = true;
            }
            if (!entry->maskValid)
                continue;
            if (entry->bitmapAlpha != opacity * image.opacity) {
                gfx::ScopedRenderWorkTimer timer(&gfx::RenderWorkStats::shadowFilterMs);
                entry->bitmap = sim::filterShadowBitmap(entry->pixels,
                                                        std::size_t(dimension + 2),
                                                        std::size_t(dimension),
                                                        sim::ShadowBlur::Box2,
                                                        opacity * image.opacity);
                entry->bitmapAlpha = opacity * image.opacity;
            }
            std::array<float, 4> crop{0, 0, 1, 1};
            if (storage) {
                int minX = dimension, minY = dimension, maxX = -1, maxY = -1;
                for (int y = 0; y < dimension; ++y)
                    for (int x = 0; x < dimension; ++x)
                        if (entry->bitmap.pixels[std::size_t(y) * dimension + x]) {
                            minX = std::min(minX, x);
                            minY = std::min(minY, y);
                            maxX = std::max(maxX, x);
                            maxY = std::max(maxY, y);
                        }
                if (maxX < 0)
                    continue;
                // One zero texel beyond the filtered support preserves linear
                // sampling at the crop boundary, including the blur fringe.
                crop = {float(std::max(0, minX - 1)) / dimension,
                        float(std::max(0, minY - 1)) / dimension,
                        float(std::min(dimension, maxX + 2)) / dimension,
                        float(std::min(dimension, maxY + 2)) / dimension};
            }
            auto source = window;
            for (auto& p : source.points)
                p += direction * (shape.radius + 2);
            const auto top = source.points[0];
            const auto right = source.points[1] - top, down = source.points[3] - top;
            source.points = {top + right * crop[0] + down * crop[1],
                             top + right * crop[2] + down * crop[1],
                             top + right * crop[2] + down * crop[3],
                             top + right * crop[0] + down * crop[3]};
            const auto revision = storage ? storage->receiverRevision : 0;
            const bool geometryHit =
                entry->geometryAttempted && entry->receiverRevision == revision &&
                entry->geometryLight == direction && entry->geometryCrop == crop &&
                sameTransform(entry->geometryTransform, image.shapeToWorld);
            if (geometryHit) {
                if (gfx::currentRenderWorkStats)
                    ++gfx::currentRenderWorkStats->shadowGeometryHits;
            } else {
                if (!work) {
                    if (gfx::currentRenderWorkStats)
                        ++gfx::currentRenderWorkStats->shadowSkipped;
                    continue;
                }
                entry->geometry = receivers.geometry(
                    source, cameraDirection, 100, .02f, image.shapeToWorld.p, &work);
                if (entry->geometry)
                    for (auto& polygon : entry->geometry->polygons)
                        for (auto& vertex : polygon.vertices) {
                            vertex.texture.x = crop[0] + vertex.texture.x * (crop[2] - crop[0]);
                            vertex.texture.y = crop[1] + vertex.texture.y * (crop[3] - crop[1]);
                        }
                entry->geometryCrop = crop;
                entry->geometryAttempted = true;
                entry->geometryTransform = image.shapeToWorld;
                entry->geometryLight = direction;
                entry->receiverRevision = revision;
            }
            if (!entry->geometry || entry->geometry->polygons.empty())
                continue;
            auto geometry = *entry->geometry;
            // Receiver geometry does not depend on the camera; recompute only
            // its small detail-policy reduction when a cached shadow is viewed.
            geometry.maxPlaneDot = 0;
            for (const auto& p : geometry.polygons) {
                Point3F n;
                m_normal(p.vertices[0].position, p.vertices[1].position, p.vertices[2].position, n);
                if (n.len() > 1e-8f) {
                    n.normalize();
                    if (m_dot(n, direction) > 0)
                        n = -n;
                    geometry.maxPlaneDot =
                        std::max(geometry.maxPlaneDot, -m_dot(cameraDirection, n));
                }
            }
            geometry.maxPlaneDot=std::clamp(geometry.maxPlaneDot,0.f,1.f);
            for (auto& p : geometry.polygons)
                for (auto& v : p.vertices)
                    v.position += image.shapeToWorld.p;
            std::erase_if(geometry.polygons, [](const auto& p) {
                Point3F n;
                m_normal(p.vertices[0].position, p.vertices[1].position, p.vertices[2].position, n);
                return n.len() < 1e-8f;
            });
            if (!geometry.polygons.empty()) {
                result.push_back({std::move(geometry), entry->bitmap});
                if (gfx::currentRenderWorkStats)
                    ++gfx::currentRenderWorkStats->shadowRendered;
            }
        } catch (const std::invalid_argument&) {
            continue;
        } catch (const std::length_error&) {
            continue;
        }
    }
    return result;
}
}
