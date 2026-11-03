#pragma once

#include <opent1/world/world_mission.hpp>

#include <algorithm>
#include <cmath>

namespace opent1::game {

inline Flt32 projectileBeamRange(const sim::SimManager* manager) {
    if (manager) {
        if (const auto* terrain = dynamic_cast<const world::SimMissionTerrain*>(
                manager->findObject(sim::TerrainId))) {
            return std::min(4000.0f, terrain->state().visibleDistance);
        }
    }
    return 4000.0f;
}

// Native laser beams are camera-facing ribbons, with a one-pixel minimum
// radius at each endpoint. The bitmap's X coordinate encodes beam age/energy.
inline bool appendProjectileBeam(world::WorldSurfaceMesh& mesh,
                                 const sim::EnvironmentCamera& camera,
                                 Point3F start,
                                 Point3F end,
                                 std::optional<Flt32> time,
                                 Flt32 beamRadius = .04f,
                                 bool minimumPixel = true) {
    if (start == end) {
        return false;
    }
    auto cameraToWorld = camera.worldToCamera;
    cameraToWorld.inverse();
    Point3F direction = end - start;
    direction.normalize();
    Point3F across{};
    m_cross(cameraToWorld.p - start, direction, across);
    if (across.len() < .00001f) {
        Point3F cameraRight{};
        cameraToWorld.getRow(0, cameraRight);
        m_cross(cameraRight, direction, across);
    }
    if (across.len() < .00001f) {
        return false;
    }
    across.normalize();
    const auto radius = [&](Point3F point) {
        Point3F cameraPoint{};
        m_mul(point, camera.worldToCamera, cameraPoint);
        const Flt32 projectedScale = std::abs(camera.focal.y);
        return minimumPixel && projectedScale > 0.0f
                   ? std::max(beamRadius,
                              (camera.perspective ? std::max(0.0f, cameraPoint.y) : 1.0f) /
                                  projectedScale)
                   : beamRadius;
    };
    const Point3F startWidth = across * radius(start), endWidth = across * radius(end);
    const std::array<Point3F, 4> points{
        start - startWidth, start + startWidth, end + endWidth, end - endWidth};
    const Flt32 u = time ? std::clamp(*time, 0.0f, 1.0f) : 0.0f;
    const Flt32 endU = time ? u : 1.0f;
    const std::array<Point2F, 4> texture{
        Point2F{u, 0}, Point2F{u, 1}, Point2F{endU, 1}, Point2F{endU, 0}};
    if (mesh.triangles.empty()) {
        mesh.bounds.fMin = points[0];
        mesh.bounds.fMax = points[0];
    }
    for (const auto& point : points) {
        mesh.bounds.fMin.setMin(point);
        mesh.bounds.fMax.setMax(point);
    }
    constexpr std::array<std::array<std::size_t, 3>, 4> faces{
        {{0, 1, 2}, {0, 2, 3}, {2, 1, 0}, {3, 2, 0}}};
    for (const auto& face : faces) {
        world::StaticSurfaceTriangle triangle;
        for (std::size_t i = 0; i < face.size(); ++i) {
            triangle.vertices[i].position = points[face[i]];
            triangle.vertices[i].texture = texture[face[i]];
        }
        mesh.triangles.push_back(triangle);
    }
    return true;
}

inline std::shared_ptr<world::WorldSurfaceMesh>
projectileBeamMesh(const Resource<gfx::Bitmap>& bitmap) {
    auto mesh = std::make_shared<world::WorldSurfaceMesh>();
    world::TsMaterialResources::Data materials;
    materials.list.details = 1;
    materials.list.materialsPerDetail = 1;
    world::TsMaterial material;
    material.flags = 0x2003; // unlit, translucent texture; blend comes from the bitmap
    materials.list.materials.push_back(material);
    materials.textures.push_back(bitmap);
    mesh->materials = world::TsMaterialResources(std::move(materials));
    return mesh;
}

inline bool renderProjectileBeam(world::WorldSurfaceQuery& query,
                                 const Resource<gfx::Bitmap>& bitmap,
                                 Point3F start,
                                 Point3F end,
                                 Flt32 time) {
    if (!query.camera || !bitmap) {
        return false;
    }
    auto mesh = projectileBeamMesh(bitmap);
    if (!appendProjectileBeam(*mesh, *query.camera, start, end, time)) {
        return false;
    }
    query.image = world::WorldSurfaceInstance{std::move(mesh), TMat3F{}};
    return true;
}

} // namespace opent1::game
