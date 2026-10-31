#include <opent1/world/world_scene.hpp>
#include "collision_geometry.hpp"
#ifdef OPENT1_HAS_TERRAIN2
#include <opent1/world/terrain2.hpp>
#endif

namespace opent1::world {
SimTerrainSurface::SimTerrainSurface(TerrainGridResources resources,
                                     const TMat3F& placement,
                                     bool baked,
                                     std::optional<Box2F> region,
                                     const TerrainLodView& policy)
    : resources_(std::move(resources)), placement_(placement), baked_(baked), region_(region),
      policy_(policy), textureCache_(resources_), lodState_(resources_) {
    if (!resources_.grid || !collision_detail::rigid(placement_)) {
        throw std::invalid_argument("invalid terrain surface");
    }
    placement_.flags = RMat3F::Matrix_HasRotation | RMat3F::Matrix_HasTranslation;
}

bool SimTerrainSurface::onAdd() {
    return SimObject::onAdd() && addToSet(sim::RenderSetId);
}

std::optional<UInt16> SimTerrainSurface::sampleLighting(Point3F position) const {
    Point3F local;
    m_mulInverse(position, placement_, local);
    return resources_.lighting({local.x, local.y});
}

bool SimTerrainSurface::setTerrain2(bool enabled) {
    if (!enabled) {
        terrain2_.reset();
        return true;
    }
#ifdef OPENT1_HAS_TERRAIN2
    if (stream_) {
        return false;
    }
    auto candidate = Terrain2::convert(resources_);
    if (!candidate) {
        return false;
    }
    terrain2_ = std::make_shared<Terrain2>(std::move(*candidate));
    return true;
#else
    return false;
#endif
}

SimTerrainSurface::SimTerrainSurface(std::unique_ptr<TerrainResourceStream> stream,
                                     const TMat3F& placement,
                                     bool baked,
                                     std::optional<Box2F> region,
                                     const TerrainLodView& policy)
    : SimTerrainSurface(std::shared_ptr<TerrainResourceStream>(std::move(stream)),
                        placement,
                        baked,
                        region,
                        policy) {}

SimTerrainSurface::SimTerrainSurface(std::shared_ptr<TerrainResourceStream> stream,
                                     const TMat3F& placement,
                                     bool baked,
                                     std::optional<Box2F> region,
                                     const TerrainLodView& policy)
    : placement_(placement), baked_(baked), region_(region), policy_(policy),
      textureCache_(resources_), lodState_(resources_), stream_(std::move(stream)) {
    if (!stream_ || !collision_detail::rigid(placement_)) {
        throw std::invalid_argument("invalid terrain stream surface");
    }
    placement_.flags = RMat3F::Matrix_HasRotation | RMat3F::Matrix_HasTranslation;
}

bool SimTerrainSurface::processQuery(sim::SimQuery& query) {
    auto* surface = dynamic_cast<WorldSurfaceQuery*>(&query);
    if (!surface) {
        return SimObject::processQuery(query);
    }
    if (!surface->camera || !collision_detail::rigid(surface->camera->worldToCamera)) {
        return false;
    }
    auto fromCamera = surface->camera->worldToCamera;
    fromCamera.inverse();
    auto toLocal = placement_;
    toLocal.inverse();
    auto policy = policy_;
    if (surface->terrainTiming) {
        if (*surface->terrainTiming != TerrainTextureTiming::RetainedLru &&
            *surface->terrainTiming != TerrainTextureTiming::NativeSoftware &&
            *surface->terrainTiming != TerrainTextureTiming::NativeOpenGL) {
            surface->failed = true;
            surface->image.reset();
            return true;
        }
        if (policy.compositeTextures &&
            (policy.lightMapDensity == TerrainLightMapDensity::Native ||
             policy.lightMapDensity == TerrainLightMapDensity::NativeOpenGL)) {
            policy.textureTiming = *surface->terrainTiming;
            policy.lightMapDensity = *surface->terrainTiming == TerrainTextureTiming::NativeSoftware
                                         ? TerrainLightMapDensity::Native
                                         : TerrainLightMapDensity::NativeOpenGL;
        }
    }
    m_mul(fromCamera.p, toLocal, policy.camera);
    policy.focalLength = std::max(surface->camera->focal.x, surface->camera->focal.y);
    policy.minimumDistance = surface->camera->nearDistance;
    // Command maps look down from z=500 and retain their authored far plane.
    // Perspective distance haze would turn the whole overhead map gray.
    if (!surface->camera->perspective) {
        policy.hazeDistance.reset();
    }
    if (policy.nativeTextures) {
        policy.nativeTextures->focalLength = surface->camera->focal.x;
        policy.nativeTextures->perspective = surface->camera->perspective;
    }
    if (policy.nativeGeometry) {
        policy.nativeGeometry->focalLength = surface->camera->focal.x;
        policy.nativeGeometry->pixelScale = 640.f / Flt32(surface->camera->viewport.width());
        policy.nativeGeometry->nearDistance = surface->camera->nearDistance;
        policy.nativeGeometry->farDistance = surface->camera->farDistance;
        policy.nativeGeometry->perspective = surface->camera->perspective;
    }
    if (policy.nativeVisibility) {
        auto& visibility = *policy.nativeVisibility;
        m_mul(static_cast<const RMat3F&>(fromCamera),
              static_cast<const RMat3F&>(toLocal),
              visibility.cameraToGrid);
        visibility.nearDistance = surface->camera->nearDistance;
        visibility.visibleDistance = surface->camera->farDistance;
        visibility.perspective = surface->camera->perspective;
        const auto extentScale = visibility.perspective ? visibility.nearDistance : 1.f;
        visibility.halfExtent = {Flt32(surface->camera->viewport.width()) * .5f * extentScale /
                                     surface->camera->focal.x,
                                 Flt32(surface->camera->viewport.height()) * .5f * extentScale /
                                     surface->camera->focal.y};
        visibility.viewportCenter = {
            (Flt32(surface->camera->viewport.left) +
             Flt32(surface->camera->viewport.width()) * .5f - surface->camera->center.x) /
                surface->camera->focal.x,
            (surface->camera->center.y - Flt32(surface->camera->viewport.top) -
             Flt32(surface->camera->viewport.height()) * .5f) /
                surface->camera->focal.y};
    }
    std::optional<TsLighting> lights;
    if (policy.sceneLighting && surface->lighting) {
        lights = tsSceneLighting(*surface->lighting, placement_);
        if (!lights) {
            throw std::invalid_argument("invalid terrain scene lights");
        }
    }
#ifdef OPENT1_HAS_TERRAIN2
    if (terrain2_ && surface->camera->perspective) {
        Terrain2View view;
        view.camera = policy.camera;
        view.focalLength = surface->camera->focal.x;
        view.pixelScale = 640.f / Flt32(surface->camera->viewport.width());
        view.screenSize = policy.nativeGeometry ? policy.nativeGeometry->screenSize : 45;
        view.visibleDistance = surface->camera->farDistance;
        view.hazeDistance = policy.hazeDistance.value_or(view.visibleDistance * .5f);
        view.maximumTriangles = 4096;
        if (lights) {
            for (const auto& light : lights->lights) {
                if (view.lights.size() == 31) {
                    break;
                }
                if (light.type == TsLight::Type::Point) {
                    view.lights.push_back({light.position, light.range, light.intensity});
                }
            }
        }
        // Original clipping planes are centered on the viewport, including
        // their near/far winding. Transform the authored camera points to grid.
        TMat3F cameraToGrid;
        m_mul(fromCamera, toLocal, cameraToGrid);
        const auto near = surface->camera->nearDistance;
        const auto left = (Flt32(surface->camera->viewport.left) - surface->camera->center.x) *
                          near / surface->camera->focal.x,
                   right =
                       (Flt32(surface->camera->viewport.right) + 1 - surface->camera->center.x) *
                       near / surface->camera->focal.x,
                   top = (surface->camera->center.y - Flt32(surface->camera->viewport.top)) * near /
                         surface->camera->focal.y,
                   bottom =
                       (surface->camera->center.y - Flt32(surface->camera->viewport.bottom) - 1) *
                       near / surface->camera->focal.y;
        std::array<Point3F, 6> p{{{0, 0, 0},
                                  {0, view.visibleDistance, 0},
                                  {left, near, top},
                                  {right, near, top},
                                  {left, near, bottom},
                                  {right, near, bottom}}};
        for (auto& point : p) {
            Point3F local;
            m_mul(point, cameraToGrid, local);
            point = local;
        }
        std::array<PlaneF, 6> planes;
        planes[0].set(p[3], p[2], p[5]);
        planes[1] = {-planes[0].x, -planes[0].y, -planes[0].z, 0};
        planes[1].d = -m_dot(p[1], planes[1]);
        planes[2].set(p[4], p[0], p[2]);
        planes[3].set(p[3], p[0], p[5]);
        planes[4].set(p[2], p[0], p[3]);
        planes[5].set(p[5], p[0], p[4]);
        view.planes = planes;
        auto frame = terrain2_->render(view);
        if (!frame) {
            throw std::invalid_argument("Terrain2 preparation failed");
        }
        auto mesh = std::make_shared<WorldSurfaceMesh>();
        mesh->materials = std::move(frame->materials);
        mesh->triangles = std::move(frame->triangles);
        mesh->bounds = {{0, 0, 0}, {6144, 6144, 2047}};
        for (const auto& q : frame->draws) {
            mesh->terrainSquareIds.push_back({q.x, q.y, 1U << q.level, 0});
        }
        surface->image = WorldSurfaceInstance{std::move(mesh), placement_};
        return true;
    }
#endif
    auto mesh = stream_ ? stream_->prepare(policy, baked_, region_, lights ? &*lights : nullptr)
                        : WorldSurfaceMesh::terrainLod(resources_,
                                                       policy,
                                                       baked_,
                                                       region_,
                                                       &textureCache_,
                                                       &lodState_,
                                                       lights ? &*lights : nullptr);
    if (!mesh) {
        throw std::invalid_argument("terrain LOD preparation failed");
    }
    surface->image = WorldSurfaceInstance{
        std::make_shared<const WorldSurfaceMesh>(std::move(*mesh)), placement_};
    return true;
}
} // namespace opent1::world
