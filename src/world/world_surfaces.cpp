#include <opent1/world/world_surfaces.hpp>
#include <ranges>
#include <opent1/gfx/render_stats.hpp>
#include "collision_geometry.hpp"
#include "interior_scene_lighting.hpp"

namespace opent1::world {
StaticSurfaceTriangle WorldSurfaceMesh::terrainTriangle(std::size_t index) const {
    const auto at =
        std::ranges::upper_bound(terrainPatches->patches, index, {}, &TerrainPatch::firstTriangle);
    const auto& patch = *std::prev(at);
    StaticSurfaceTriangle result;
    result.material = patch.material;
    result.lightMap = patch.lightMap;
    result.lit = bool(patch.lightMap);
    result.nativeFarSphere = true;
    const auto first = (index - patch.firstTriangle) * 3;
    for (auto j : std::views::iota(std::size_t{0}, std::size_t{3})) {
        const auto id = patch.topology->indices()[first + j];
        const auto& input = patch.topology->vertices()[id];
        const auto& h = patch.heights[id];
        auto& v = result.vertices[j];
        v.position = patch.origin + Point3F{input.position.x * patch.extent,
                                            input.position.y * patch.extent,
                                            h[2] >= 1 ? h[1] : h[0] + (h[1] - h[0]) * h[2]};
        v.texture = input.texture;
        v.lightMap = {patch.mapOrigin.x + v.texture.x * patch.mapScale.x,
                      patch.mapOrigin.y + v.texture.y * patch.mapScale.y};
        v.farSphereDistance = h[3];
        if (terrainPatches->hazeDistance < terrainPatches->farDistance)
            v.haze = std::clamp((h[3] - terrainPatches->hazeDistance) /
                                    (terrainPatches->farDistance - terrainPatches->hazeDistance),
                                0.f,
                                1.f);
    }
    return result;
}

std::optional<UInt16> WorldSurfaceMesh::sampleLighting(Point3F point) const {
    if (!std::isfinite(point.x) || !std::isfinite(point.y) || !std::isfinite(point.z)) {
        return {};
    }
    const auto sample = [&](const std::array<StaticSurfaceVertex, 3>& vertices,
                            const gfx::LightMap& map) -> std::optional<UInt16> {
        if (!map.valid())
            return {};
        const auto& a = vertices[0];
        const auto& b = vertices[1];
        const auto& c = vertices[2];
        const auto u = b.position - a.position, v = c.position - a.position, p = point - a.position;
        const auto uu = m_dot(u, u), uv = m_dot(u, v), vv = m_dot(v, v), pu = m_dot(p, u),
                   pv = m_dot(p, v);
        const auto det = uu * vv - uv * uv;
        if (det <= 1e-12f) {
            return {};
        }
        const auto y = (pu * vv - pv * uv) / det, z = (pv * uu - pu * uv) / det, x = 1 - y - z;
        if (x < -.001f || y < -.001f || z < -.001f || (p - u * y - v * z).len() > .05f) {
            return {};
        }
        const auto tex = a.lightMap * x + b.lightMap * y + c.lightMap * z;
        // Native getSurfaceInfo subtracts half a light texel before indexing;
        // the render UV already includes a positive half-texel origin.
        const auto ix = std::clamp(Int32(std::floor(tex.x * map.size.x - 1)), 0, map.size.x - 1);
        const auto iy = std::clamp(Int32(std::floor(tex.y * map.size.y - 1)), 0, map.size.y - 1);
        return map.samples[std::size_t(iy) * map.size.x + ix];
    };
    if (interiorMesh && interiorMaps) {
        const auto vertices = interiorMesh->hardware ? interiorMesh->hardware->vertices()
                                                     : std::span<const gfx::HardwareVertex>{};
        const auto indices =
            interiorMesh->hardware ? interiorMesh->hardware->indices() : std::span<const UInt32>{};
        for (auto id : std::views::iota(std::size_t{0}, interiorMesh->ranges.size())) {
            const auto* map = (*interiorMaps)[id].get();
            if (!map ||
                (interiorSelection && !std::ranges::binary_search(*interiorSelection, UInt32(id))))
                continue;
            const auto& box = interiorMesh->surfaceBounds[id];
            // Conservative bound for the original barycentric and plane tolerances.
            const auto margin = (box.fMax - box.fMin) * .003f + Point3F{.051f, .051f, .051f};
            if (!Box3F{box.fMin - margin, box.fMax + margin}.contains(point))
                continue;
            const auto range = interiorMesh->ranges[id];
            for (UInt32 i = range[0]; i < range[0] + range[1]; i += 3) {
                std::array<StaticSurfaceVertex, 3> triangle;
                for (auto j : std::views::iota(std::size_t{0}, std::size_t{3})) {
                    const auto& v = vertices[indices[i + j]];
                    triangle[j].position = {v.position.x, v.position.y, v.inverseDepth};
                    triangle[j].lightMap = v.lightMap;
                }
                if (auto color = sample(triangle, *map))
                    return color;
            }
        }
        return {};
    }
    for (const auto& triangle : surfaceTriangles()) {
        if (triangle.lightMap) {
            if (auto color = sample(triangle.vertices, *triangle.lightMap))
                return color;
        }
    }
    return {};
}

namespace {
Point3F rgb(UInt16 color) {
    return {Flt32((color >> 8) & 15) / 15, Flt32((color >> 4) & 15) / 15, Flt32(color & 15) / 15};
}

bool computeBounds(WorldSurfaceMesh& mesh) {
    if (mesh.triangles.empty()) {
        mesh.bounds = {};
        return true;
    }
    mesh.bounds = {mesh.triangles.front().vertices[0].position,
                   mesh.triangles.front().vertices[0].position};
    return std::ranges::all_of(mesh.triangles, [&](const auto& t) {
        return std::ranges::all_of(t.vertices, [&](const auto& v) {
            if (!collision_detail::finite(v.position)) {
                return false;
            }
            mesh.bounds.fMin.setMin(v.position);
            mesh.bounds.fMax.setMax(v.position);
            return true;
        });
    });
}

struct XY {
    Flt64 x{}, y{};
};

struct Half {
    Flt64 x{}, y{}, c{};

    Flt64 at(XY p) const {
        return x * p.x + y * p.y + c;
    }
};

using Poly = std::vector<XY>;

Flt64 area(const Poly& p) {
    if (p.size() < 3) {
        return 0;
    }
    const auto origin = p.front();
    Flt64 result{};
    std::ranges::for_each(std::views::iota(std::size_t{1}, p.size() - 1), [&](auto i) {
        result += (p[i].x - origin.x) * (p[i + 1].y - origin.y) -
                  (p[i].y - origin.y) * (p[i + 1].x - origin.x);
    });
    return result;
}

Poly clip(const Poly& p, Half h, std::size_t& work) {
    if (p.empty()) {
        return {};
    }
    collision_detail::spend(work, p.size());
    Poly result;
    auto a = p.back();
    auto da = h.at(a);
    for (auto b : p) {
        const auto db = h.at(b);
        if ((da >= 0) != (db >= 0)) {
            const auto t = da / (da - db);
            result.push_back({a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t});
        }
        if (db >= 0) {
            result.push_back(b);
        }
        a = b;
        da = db;
    }
    result.erase(std::unique(result.begin(),
                             result.end(),
                             [](auto a0, auto b0) {
                                 return std::hypot(a0.x - b0.x, a0.y - b0.y) < 1e-9;
                             }),
                 result.end());
    if (result.size() > 1 &&
        std::hypot(result.front().x - result.back().x, result.front().y - result.back().y) < 1e-9) {
        result.pop_back();
    }
    if (std::abs(area(result)) < 1e-10) {
        return {};
    }
    if (result.size() > 32) {
        throw std::length_error("shadow fragment vertices");
    }
    return result;
}

std::vector<Half> edges(const Poly& p) {
    std::vector<Half> result;
    const auto sign = area(p) > 0 ? 1. : -1.;
    std::ranges::for_each(std::views::iota(std::size_t{}, p.size()), [&](auto i) {
        const auto a = p[i], b = p[(i + 1) % p.size()];
        const auto x = -(b.y - a.y) * sign, y = (b.x - a.x) * sign;
        result.push_back({x, y, -x * a.x - y * a.y});
    });
    return result;
}

struct Candidate {
    Poly polygon;
    std::vector<Half> edges;
    Half depth;
    Point3F normal;
    Flt32 constant{};
};
}

std::optional<WorldSurfaceMesh> WorldSurfaceMesh::terrain(const TerrainGridResources& resources,
                                                          bool baked,
                                                          std::optional<Box2F> region) {
    if (!resources.grid || resources.blocks.size() != resources.grid->blocks.size() ||
        resources.grid->scale < 0 || resources.grid->scale > 16 || resources.grid->size.x <= 0 ||
        resources.grid->size.y <= 0 || resources.grid->size.x > 256 ||
        resources.grid->size.y > 256 || resources.grid->blockMap.size() > 4096 ||
        resources.grid->blockMap.size() !=
            UInt64(resources.grid->size.x) * resources.grid->size.y) {
        return {};
    }
    if (region && (!collision_detail::finite({region->fMin.x, region->fMin.y, 0}) ||
                   !collision_detail::finite({region->fMax.x, region->fMax.y, 0}) ||
                   region->fMin.x >= region->fMax.x || region->fMin.y >= region->fMax.y)) {
        return {};
    }
    WorldSurfaceMesh result;
    result.materials = resources.materials;
    const auto square = Flt32(1U << resources.grid->scale);
    for (auto i : std::views::iota(std::size_t{0}) | std::views::take_while([&](std::size_t i) {
                      return i < resources.grid->blockMap.size();
                  })) {
        const auto id = resources.grid->blockMap[i];
        if (id == -1) {
            continue;
        }
        const auto entry =
            std::ranges::find(resources.grid->blocks, id, &TerrainGridBlockEntry::id);
        if (entry == resources.grid->blocks.end()) {
            return {};
        }
        const auto& handle = resources.blocks[std::size_t(entry - resources.grid->blocks.begin())];
        if (!handle) {
            return {};
        }
        const auto& b = *handle;
        const auto n = b.width;
        if (!n || n > 1024 || b.heights.size() != UInt64(n + 1) * (n + 1) ||
            b.materials.size() != UInt64(n) * n) {
            return {};
        }
        const Point3F origin{Flt32(i % std::size_t(resources.grid->size.x)) * square * n,
                             Flt32(i / std::size_t(resources.grid->size.x)) * square * n,
                             0};
        UInt32 x0{}, y0{}, x1 = n, y1 = n;
        if (region) {
            const auto index = [&](Flt32 value, Flt32 base, bool ceiling) {
                const auto v = std::clamp((Flt64(value) - base) / square, 0., Flt64(n));
                return UInt32(ceiling ? std::ceil(v) : std::floor(v));
            };
            x0 = index(region->fMin.x, origin.x, false);
            y0 = index(region->fMin.y, origin.y, false);
            x1 = index(region->fMax.x, origin.x, true);
            y1 = index(region->fMax.y, origin.y, true);
        }
        if (UInt64(x1 - x0) * (y1 - y0) * 2 > 1'048'576 - result.triangles.size()) {
            return {};
        }
        for (auto y :
             std::views::iota(static_cast<UInt32>(y0)) | std::views::take_while([&](UInt32 y) {
                 return y < y1;
             })) {
            for (auto x :
                 std::views::iota(static_cast<UInt32>(x0)) | std::views::take_while([&](UInt32 x) {
                     return x < x1;
                 })) {
                const auto material = b.materials[std::size_t(y) * n + x];
                if (material.hole()) {
                    continue;
                }
                if (material.index >= result.materials.list().materialsPerDetail) {
                    return {};
                }
                std::array<StaticSurfaceVertex, 4> v;
                std::ranges::for_each(std::views::iota(0U, 4U), [&](auto corner) {
                    const auto dx = corner & 1U, dy = corner >> 1;
                    auto& p = v[corner];
                    p.position =
                        origin + Point3F{Flt32(x + dx) * square,
                                         Flt32(y + dy) * square,
                                         b.heights[std::size_t(y + dy) * (n + 1) + x + dx]};
                    auto u = Flt32(dx), w = 1 - Flt32(dy);
                    if (material.flags & 2U) {
                        u = 1 - u;
                    }
                    if (material.flags & 4U) {
                        w = 1 - w;
                    }
                    if (material.flags & 1U) {
                        const auto old = u;
                        u = w;
                        w = 1 - old;
                    }
                    p.texture = {u, w};
                    if (baked && b.lightScale >= 0 && b.lightScale <= 10) {
                        const auto scale = 1U << b.lightScale, side = n * scale + 1;
                        if (b.lights.size() == UInt64(side) * side) {
                            p.intensity = rgb(
                                b.lights[std::size_t((y + dy) * scale) * side + (x + dx) * scale]);
                        }
                    }
                });
                const std::array<std::array<UInt32, 3>, 2> indices =
                    ((x ^ y) & 1U) ? std::array<std::array<UInt32, 3>, 2>{{{0, 2, 1}, {1, 2, 3}}}
                                   : std::array<std::array<UInt32, 3>, 2>{{{0, 2, 3}, {0, 3, 1}}};
                std::ranges::for_each(indices, [&](auto indices0) {
                    result.triangles.push_back({{v[indices0[0]], v[indices0[1]], v[indices0[2]]},
                                                material.index,
                                                baked && !b.lights.empty()});
                });
            }
        }
    }
    return computeBounds(result) ? std::optional(std::move(result)) : std::nullopt;
}

std::optional<WorldSurfaceMesh> WorldSurfaceMesh::interior(const InteriorShapeResources& resources,
                                                           UInt32 lod,
                                                           UInt32 state,
                                                           bool baked,
                                                           const InteriorSurfaceLighting& options) {
    if (!resources.shape || lod >= resources.geometry.size() ||
        lod >= resources.shape->lods.size() || !resources.geometry[lod]) {
        return {};
    }
    const auto& g = *resources.geometry[lod];
    const InteriorLighting* lighting = nullptr;
    const InteriorLighting* mission = options.mission;
    if (baked) {
        if (state >= resources.shape->lightStateNames.size()) {
            return {};
        }
        const auto index = UInt64(resources.shape->lods[lod].firstLightState) + state;
        if (index >= resources.lighting.size() || !resources.lighting[std::size_t(index)]) {
            return {};
        }
        lighting = resources.lighting[std::size_t(index)].get();
        if (lighting->buildId != g.buildId || lighting->surfaces.size() != g.surfaces.size()) {
            return {};
        }
        const auto missionIndex = UInt64(lod) * resources.shape->lightStateNames.size() + state;
        if (!mission && missionIndex < resources.missionLighting.size()) {
            mission = resources.missionLighting[std::size_t(missionIndex)].get();
        }
    }
    WorldSurfaceMesh result;
    result.materials = resources.materials;
    if (g.highestMipLevel < 0 || g.highestMipLevel > 15) {
        return {};
    }
    std::optional<std::vector<InteriorLightSlot>> slots;
    if (lighting) {
        if (options.slots) {
            if (options.slots->size() != std::size_t(lighting->mapSlots) ||
                !options.controls.empty() ||
                std::ranges::any_of(*options.slots, [](const auto& slot) {
                    return slot.map < -1;
                })) {
                return {};
            }
            slots = std::vector<InteriorLightSlot>(options.slots->begin(), options.slots->end());
        } else {
            slots = lighting->animation(options.seconds, options.controls);
        }
        if (!slots || lighting->scaleShift < 0 || lighting->scaleShift > 15) {
            return {};
        }
    }
    std::optional<InteriorSceneLights> scene;
    if (options.scene) {
        if (!lighting) {
            return {};
        }
        scene = InteriorSceneLights::prepare(
            g, *lighting, *options.scene, !resources.shape->linked && !mission);
        if (!scene) {
            return {};
        }
    }
    const auto* previous = options.previous;
    const bool reuse = previous && previous->interiorMesh &&
                       previous->interiorMesh->source.get() == resources.geometry[lod].get() &&
                       previous->interiorMesh->lighting.get() == lighting;
    if (reuse) {
        result.interiorMesh = previous->interiorMesh;
        result.bounds = previous->bounds;
    }
    auto maps =
        std::make_shared<std::vector<std::shared_ptr<const gfx::LightMap>>>(g.surfaces.size());
    result.interiorMissionHandle = options.missionHandle;
    for (const auto& handle : resources.missionLighting) {
        if (handle.get() == mission)
            result.interiorMissionHandle = handle;
    }
    result.interiorSceneAffected.resize(g.surfaces.size());
    if (options.scene)
        result.interiorSceneLighting = *options.scene;
    std::vector<gfx::HardwareVertex> hardwareVertices;
    std::vector<UInt32> hardwareIndices;
    std::vector<std::array<UInt32, 2>> hardwareRanges(reuse ? 0 : g.surfaces.size());
    std::vector<Box3F> hardwareBounds(reuse ? 0 : g.surfaces.size());
    std::vector<std::shared_ptr<const NativeSurfaceTexture>> hardwareSoftware(
        reuse ? 0 : g.surfaces.size());
    std::size_t samples{};
    for (auto i : std::views::iota(std::size_t{0}) | std::views::take_while([&](std::size_t i) {
                      return i < g.surfaces.size();
                  })) {
        const auto& s = g.surfaces[i];
        if (!s.renderable()) {
            continue;
        }
        if (s.material >= result.materials.textures().size() ||
            s.material >= result.materials.list().materials.size() ||
            s.firstVertex > g.vertices.size() || s.vertexCount < 3 ||
            s.vertexCount > g.vertices.size() - s.firstVertex) {
            return {};
        }
        const auto* texture = result.materials.textures()[s.material].get();
        if (result.materials.list().materials[s.material].type() == 3 &&
            (!texture || texture->width <= 0 || texture->height <= 0)) {
            return {};
        }
        std::shared_ptr<const gfx::LightMap> map;
        if (lighting) {
            const auto& ls = lighting->surfaces[i];
            if (ls.lightCount < 0) {
                return {};
            }
            samples += std::max(std::size_t{1}, std::size_t(ls.width) * ls.height) *
                       (std::size_t(ls.lightCount) + 1);
            if (samples > 16'777'216) {
                return {};
            }
            const bool sceneAffected =
                scene && (!scene->pointLights(UInt32(i)).empty() ||
                          (!resources.shape->linked && !mission && (s.flags & 0x40U)));
            result.interiorSceneAffected[i] = sceneAffected;
            const bool sameScene =
                previous && ((!scene && !previous->interiorSceneLighting) ||
                             (options.scene && previous->interiorSceneLighting &&
                              *options.scene == *previous->interiorSceneLighting));
            const bool unchanged =
                reuse && !options.refreshMaps && previous->interiorMaps &&
                (sameScene || (!sceneAffected && i < previous->interiorSceneAffected.size() &&
                               !previous->interiorSceneAffected[i])) &&
                previous->interiorMission == mission &&
                (!mission ||
                 (result.interiorMissionHandle &&
                  result.interiorMissionHandle.get() == previous->interiorMissionHandle.get())) &&
                ls.lightIndex >= 0 && ls.lightCount >= 0 &&
                std::size_t(ls.lightIndex) + ls.lightCount <= slots->size() &&
                previous->interiorSlots.size() == slots->size() &&
                std::equal(slots->begin() + ls.lightIndex,
                           slots->begin() + ls.lightIndex + ls.lightCount,
                           previous->interiorSlots.begin() + ls.lightIndex);
            if (unchanged) {
                map = (*previous->interiorMaps)[i];
            } else {
                auto colors = lighting->lightmap(i, mission, *slots);
                if (!colors) {
                    return {};
                }
                if (scene && !scene->merge(UInt32(i), *colors, samples)) {
                    return {};
                }
                auto prepared = std::make_shared<gfx::LightMap>();
                prepared->dynamic = ls.lightCount > 0 || sceneAffected;
                prepared->offset = {ls.offsetX, ls.offsetY};
                const auto width = std::max(1U, UInt32(ls.width)),
                           height = std::max(1U, UInt32(ls.height));
                prepared->size = {Int32(std::max(2U, width)), Int32(std::max(2U, height))};
                for (auto y : std::views::iota(Int32{0}) | std::views::take_while([&](Int32 y) {
                                  return y < prepared->size.y;
                              })) {
                    for (auto x : std::views::iota(Int32{0}) | std::views::take_while([&](Int32 x) {
                                      return x < prepared->size.x;
                                  })) {
                        const auto index = std::size_t(std::min(UInt32(y), height - 1)) * width +
                                           std::min(UInt32(x), width - 1);
                        if (index >= colors->size()) {
                            return {};
                        }
                        prepared->samples.push_back((*colors)[index]);
                    }
                }
                map = std::move(prepared);
                if (gfx::currentRenderWorkStats) {
                    ++gfx::currentRenderWorkStats->interiorLightMapBuilds;
                    gfx::currentRenderWorkStats->interiorLightMapBytes +=
                        map->samples.size() * sizeof(UInt16);
                }
            }
        }
        (*maps)[i] = map;
        if (reuse)
            continue;
        std::vector<StaticSurfaceVertex> vertices;
        for (const auto v : std::span(g.vertices).subspan(s.firstVertex, s.vertexCount)) {
            if (v.point >= g.points.size() || v.texture >= g.texturePoints.size()) {
                return {};
            }
            const auto uv = g.texturePoints[v.texture];
            const auto scale = Flt32(1U << g.highestMipLevel);
            const auto tx = uv.x * Flt32(UInt32(s.width) + 1) * scale + s.offsetX,
                       ty = uv.y * Flt32(UInt32(s.height) + 1) * scale + s.offsetY;
            StaticSurfaceVertex p{g.boundaryPoint(g.points[v.point]),
                                  texture ? Point2F{tx / texture->width, ty / texture->height}
                                          : uv};
            if (lighting) {
                const auto& ls = lighting->surfaces[i];
                const auto lightScale = Flt32(1U << lighting->scaleShift);
                // Original RegisterTexture uses the base-mip surface rectangle
                // and a half-texel lightmap origin. Our maps need no POT padding.
                p.lightMap = {
                    ((uv.x * Flt32(UInt32(s.width) + 1) * scale + ls.offsetX) / lightScale + .5f) /
                        map->size.x,
                    ((uv.y * Flt32(UInt32(s.height) + 1) * scale + ls.offsetY) / lightScale + .5f) /
                        map->size.y};
            }
            vertices.push_back(p);
        }
        if (vertices.size() - 2 > 1'048'576 - result.interiorSurfaceIds.size()) {
            return {};
        }
        const auto first = UInt32(hardwareVertices.size());
        auto& surfaceBounds = hardwareBounds[i];
        surfaceBounds = {vertices.front().position, vertices.front().position};
        for (const auto& vertex : vertices) {
            surfaceBounds.fMin.setMin(vertex.position);
            surfaceBounds.fMax.setMax(vertex.position);
        }
        hardwareRanges[i] = {UInt32(hardwareIndices.size()), UInt32((vertices.size() - 2) * 3)};
        for (const auto& vertex : vertices) {
            gfx::HardwareVertex v;
            v.position = {vertex.position.x, vertex.position.y};
            v.inverseDepth = vertex.position.z;
            v.texture = texture ? vertex.texture : Point2F{};
            v.lightMap = vertex.lightMap;
            hardwareVertices.push_back(v);
        }
        for (auto j : std::views::iota(UInt32{1}) | std::views::take_while([&](UInt32 j) {
                          return j + 1 < vertices.size();
                      })) {
            hardwareIndices.insert(hardwareIndices.end(), {first, first + j, first + j + 1});
        }
        std::shared_ptr<NativeSurfaceTexture> software;
        if (lighting && texture) {
            software = std::make_shared<NativeSurfaceTexture>();
            const auto scale = Int32{1} << g.highestMipLevel;
            software->settings.size = {(Int32(s.width) + 1) * scale, (Int32(s.height) + 1) * scale};
            software->settings.offset = {s.offsetX, s.offsetY};
            software->settings.lightScale = lighting->scaleShift;
            const auto shift = (s.flags >> 1) & 15U;
            software->texelScale =
                (shift >= 8 ? Flt32(1U << (shift - 8)) : 1.f / Flt32(1U << shift)) / g.textureScale;
            software->minimumMip = g.highestMipLevel;
            for (const auto& vertex : vertices) {
                software->points.push_back(vertex.position);
            }
        }
        hardwareSoftware[i] = std::move(software);
        result.interiorSurfaceIds.insert(
            result.interiorSurfaceIds.end(), vertices.size() - 2, UInt32(i));
    }
    if (!reuse) {
        if (!hardwareVertices.empty()) {
            const auto position = [](const auto& v) {
                return Point3F{v.position.x, v.position.y, v.inverseDepth};
            };
            result.bounds = {position(hardwareVertices.front()),
                             position(hardwareVertices.front())};
            for (const auto& vertex : hardwareVertices) {
                result.bounds.fMin.setMin(position(vertex));
                result.bounds.fMax.setMax(position(vertex));
            }
        }
        auto geometry = std::make_shared<InteriorMesh>();
        geometry->source = resources.geometry[lod];
        if (lighting)
            geometry->lighting = resources.lighting[std::size_t(
                UInt64(resources.shape->lods[lod].firstLightState) + state)];
        geometry->software = std::move(hardwareSoftware);
        geometry->surfaceIds = std::move(result.interiorSurfaceIds);
        geometry->materials = resources.materials;
        geometry->ranges = std::move(hardwareRanges);
        geometry->surfaceBounds = std::move(hardwareBounds);
        if (!hardwareIndices.empty()) {
            geometry->hardware =
                gfx::HardwareMesh::create(std::move(hardwareVertices), std::move(hardwareIndices));
            if (!geometry->hardware)
                return {};
        }
        // Weak canonical cache preserves repeated instances without retaining old missions.
        static thread_local std::vector<std::weak_ptr<const InteriorMesh>> canonical;
        std::erase_if(canonical, [](const auto& entry) {
            return entry.expired();
        });
        for (const auto& weak : canonical) {
            const auto entry = weak.lock();
            if (entry->source.get() == geometry->source.get() &&
                entry->lighting.get() == geometry->lighting.get() &&
                std::ranges::equal(entry->materials.textures(),
                                   resources.materials.textures(),
                                   [](const auto& a, const auto& b) {
                                       return a.get() == b.get();
                                   }) &&
                std::ranges::equal(entry->materials.list().materials,
                                   resources.materials.list().materials,
                                   [](const auto& a, const auto& b) {
                                       return a.flags == b.flags;
                                   })) {
                result.interiorMesh = entry;
                break;
            }
        }
        if (!result.interiorMesh) {
            result.interiorMesh = std::move(geometry);
            if (canonical.size() < 4096)
                canonical.push_back(result.interiorMesh);
        }
        if (gfx::currentRenderWorkStats)
            ++gfx::currentRenderWorkStats->interiorGeometryBuilds;
    }
    result.interiorMaps = std::move(maps);
    if (slots)
        result.interiorSlots = std::move(*slots);
    result.interiorMission = mission;
    return result;
}

std::optional<TsStaticShapeFrame> WorldSurfaceMesh::prepare(const TsStaticView& view) const {
    if (!interiorMesh && !terrainPatches)
        return TsStaticShapeFrame::prepareSurfaces(materials, triangles, view);
    std::vector<StaticSurfaceTriangle> selected;
    selected.reserve(triangleCount());
    for (auto triangle : surfaceTriangles())
        selected.push_back(std::move(triangle));
    return TsStaticShapeFrame::prepareSurfaces(materials, selected, view);
}

bool WorldSurfaceMesh::retainedHardwareCompatible() const {
    return terrainPatches || (interiorMesh && interiorMesh->hardware &&
                              std::ranges::none_of(materials.list().materials, [](const auto& m) {
                                  return (m.flags & 0x2000U) != 0;
                              }));
}

std::optional<std::vector<gfx::HardwarePolygon>> WorldSurfaceMesh::hardwareInterior(
    const TsStaticView& view, const gfx::Palette& palette, Flt32 gamma) const {
    std::vector<gfx::HardwarePolygon> polygons;
    return appendHardwareInterior(polygons, view, palette, gamma)
               ? std::optional(std::move(polygons))
               : std::nullopt;
}

bool WorldSurfaceMesh::appendHardwareInterior(std::vector<gfx::HardwarePolygon>& polygons,
                                              const TsStaticView& view,
                                              const gfx::Palette& palette,
                                              Flt32 gamma) const {
    const auto first = polygons.size();
    const auto fail = [&] {
        polygons.resize(first);
        return false;
    };
    if (!retainedHardwareCompatible() || !view.perspective || !palette.valid() ||
        !std::isfinite(gamma) || gamma < .05f || gamma > 20)
        return fail();
    // Ordinary preparation validates all material/texture encodings. Placeholder coverage
    // carries material state only; the hardware batch uses immutable indexed local vertices.
    static const std::array<gfx::PolygonVertex, 3> placeholder{{{{0, 0}}, {{1, 0}}, {{0, 1}}}};
    static const std::array<Point2F, 3> lightUV{};
    gfx::HardwareTransform transform;
    for (auto i : std::views::iota(std::size_t{0}, std::size_t{3})) {
        transform.camera[i] = {view.shapeToCamera.m[0][i],
                               view.shapeToCamera.m[1][i],
                               view.shapeToCamera.m[2][i],
                               view.shapeToCamera.p[i]};
    }
    transform.optics = {view.focal.x, view.focal.y, view.center.x, view.center.y};
    transform.clip = {view.nearDistance, view.farDistance, 1, 0};
    if (terrainPatches) {
        static const auto sharedPatch = [] {
            std::vector<gfx::HardwareVertex> vertices(18);
            std::vector<UInt32> indices(18);
            for (auto i : std::views::iota(UInt32{0}, UInt32{18})) {
                vertices[i].inverseDepth = Flt32(i);
                indices[i] = i;
            }
            return gfx::HardwareMesh::create(std::move(vertices), std::move(indices));
        }();
        polygons.reserve(first + terrainPatches->patches.size());
        for (const auto& patch : terrainPatches->patches) {
            auto local = view.shapeToCamera;
            local.preTranslate(patch.origin);
            std::array<Point3F, 8> cameraPoints{};
            std::array<bool, 7> outside;
            outside.fill(true);
            UInt32 visited{};
            for (const auto id : patch.topology->indices()) {
                if (visited & (1U << id))
                    continue;
                visited |= 1U << id;
                const auto xy = patch.topology->vertices()[id].position;
                const auto& h = patch.heights[id];
                Point3F point{xy.x * patch.extent,
                              xy.y * patch.extent,
                              h[2] >= 1 ? h[1] : h[0] + (h[1] - h[0]) * h[2]};
                m_mul(point, local, cameraPoints[id]);
                const auto p = cameraPoints[id];
                const std::array<Flt64, 7> planes{
                    Flt64(p.y) - view.nearDistance,
                    Flt64(view.farDistance) - p.y,
                    Flt64(p.x) * view.focal.x + Flt64(p.y) * view.center.x,
                    -Flt64(p.x) * view.focal.x + Flt64(p.y) * (view.width - view.center.x),
                    -Flt64(p.z) * view.focal.y + Flt64(p.y) * view.center.y,
                    Flt64(p.z) * view.focal.y + Flt64(p.y) * (view.height - view.center.y),
                    Flt64(view.farDistance) - h[3]};
                for (auto j : std::views::iota(std::size_t{0}, planes.size()))
                    outside[j] = outside[j] && planes[j] < -.001;
            }
            if (std::ranges::any_of(outside, std::identity{}))
                continue;
            bool facing{};
            const auto indices = patch.topology->indices();
            for (std::size_t j = 0; j < indices.size(); j += 3) {
                const auto a = cameraPoints[indices[j]], b = cameraPoints[indices[j + 1]],
                           c = cameraPoints[indices[j + 2]];
                Point3F cross;
                m_cross(b - a, c - a, cross);
                facing |= m_dot(cross, a) >= -.001f;
            }
            if (!facing)
                continue;
            auto instance = transform;
            for (auto i : std::views::iota(std::size_t{0}, std::size_t{3}))
                instance.camera[i][3] = local.p[i];
            instance.parameters = patch.heights;
            instance.ambient = {terrainPatches->farDistance, 0, 0, terrainPatches->hazeDistance};
            instance.geometry = {patch.extent, view.farDistance, 1, Flt32(patch.topologyKey)};
            instance.mapping = {
                patch.mapOrigin.x, patch.mapOrigin.y, patch.mapScale.x, patch.mapScale.y};
            gfx::HardwareMaterial material;
            material.texture = materials.textures()[patch.material].get();
            material.lightMap = patch.lightMap.get();
            material.linear = true;
            material.magnificationLinear =
                (material.texture->attributes & gfx::BitmapTransparent) == 0;
            material.mipFilter = material.texture->detailLevels > 1
                                     ? gfx::HardwareMipFilter::Nearest
                                     : gfx::HardwareMipFilter::None;
            material.useSurfacePalette = true;
            material.alphaSource = gfx::HardwareAlphaSource::Opaque;
            material.paletteMode = gfx::HardwarePaletteMode::OpenGL;
            material.paletteGamma = gamma;
            material.hazeSource = gfx::PolygonSource::Vertex;
            material.deferredHaze = terrainPatches->hazeDistance < terrainPatches->farDistance;
            gfx::HardwarePolygon polygon{placeholder,
                                         material,
                                         material.lightMap ? std::span<const Point2F>(lightUV)
                                                           : std::span<const Point2F>{}};
            polygon.mesh = sharedPatch;
            polygon.transform = instance;
            polygon.indexCount = 18;
            polygons.push_back(std::move(polygon));
        }
        return true;
    }
    for (auto i : std::views::iota(std::size_t{0}) | std::views::take_while([&](std::size_t i) {
                      return i < interiorMesh->ranges.size();
                  })) {
        const auto range = interiorMesh->ranges[i];
        if (!range[1] ||
            (interiorSelection && !std::ranges::binary_search(*interiorSelection, UInt32(i))))
            continue;
        const auto id = interiorMesh->source->surfaces[i].material;
        const auto& source = materials.list().materials[id];
        gfx::HardwareMaterial material;
        material.color = {1, 1, 1};
        if (source.type() == 1) {
            if (source.index < 0 || source.index > 255)
                return fail();
            const auto c = palette.find().colors[std::size_t(source.index)];
            material.color = {Flt32(c.red) / 255, Flt32(c.green) / 255, Flt32(c.blue) / 255};
            for (auto cidx : std::views::iota(std::size_t{0}, std::size_t{3}))
                material.color[cidx] = Flt32(std::pow(material.color[cidx], double(1.f / gamma)));
        } else if (source.type() == 2) {
            material.color = {
                Flt32(source.rgb[0]) / 255, Flt32(source.rgb[1]) / 255, Flt32(source.rgb[2]) / 255};
        } else if (source.type() == 3) {
            material.texture = materials.textures()[id].get();
            if (!material.texture)
                return fail();
            material.wrap = true;
            material.linear = true;
            material.useSurfacePalette = true;
            material.magnificationLinear =
                (material.texture->attributes & gfx::BitmapTransparent) == 0;
            material.mipFilter = material.texture->detailLevels > 1
                                     ? gfx::HardwareMipFilter::Nearest
                                     : gfx::HardwareMipFilter::None;
            material.transparent = (source.flags & 0x1000U) != 0;
            material.alphaCutoff = material.transparent ? .65f : 0;
            if (!material.transparent)
                material.alphaSource = gfx::HardwareAlphaSource::Opaque;
            if (material.texture->bitDepth == 8 && gamma != 1) {
                material.paletteMode = gfx::HardwarePaletteMode::OpenGL;
                material.paletteGamma = gamma;
            }
        } else
            return fail();
        material.lightMap = interiorMaps ? (*interiorMaps)[i].get() : nullptr;
        gfx::HardwarePolygon polygon{placeholder,
                                     material,
                                     material.lightMap ? std::span<const Point2F>(lightUV)
                                                       : std::span<const Point2F>{}};
        polygon.mesh = interiorMesh->hardware;
        polygon.transform = transform;
        polygon.firstIndex = range[0];
        polygon.indexCount = range[1];
        polygons.push_back(std::move(polygon));
    }
    return true;
}

std::optional<WorldSurfaceMesh>
WorldSurfaceMesh::visibleInterior(std::span<const UInt32> surfaces) const {
    if (interiorMesh) {
        if (surfaces.size() > 65536 || !std::ranges::is_sorted(surfaces) ||
            std::ranges::adjacent_find(surfaces) != surfaces.end())
            return {};
        WorldSurfaceMesh result = *this;
        result.interiorSelection.emplace(surfaces.begin(), surfaces.end());
        return result;
    }
    if (triangles.size() != interiorSurfaceIds.size() || triangles.size() > 1'048'576 ||
        surfaces.size() > 65536 || !std::ranges::is_sorted(surfaces) ||
        std::ranges::adjacent_find(surfaces) != surfaces.end()) {
        return {};
    }
    WorldSurfaceMesh result;
    result.materials = materials;
    std::ranges::for_each(std::views::iota(std::size_t{}, triangles.size()), [&](auto i) {
        if (std::ranges::binary_search(surfaces, interiorSurfaceIds[i])) {
            result.triangles.push_back(triangles[i]);
            result.interiorSurfaceIds.push_back(interiorSurfaceIds[i]);
        }
    });
    return computeBounds(result) ? std::optional(std::move(result)) : std::nullopt;
}

std::optional<std::vector<sim::ShadowReceiver>>
collectShadowReceivers(const sim::ShadowSourceWindow& w,
                       std::span<const WorldSurfaceInstance> instances,
                       Flt32 length) {
    using namespace collision_detail;
    if (instances.size() > 512 || !std::isfinite(length) || length <= 0 || length > 65536 ||
        !finite(w.direction) || !finite(w.right) || !finite(w.up) ||
        std::abs(w.direction.len() - 1) > .0001f || std::abs(w.right.len() - 1) > .0001f ||
        std::abs(w.up.len() - 1) > .0001f || std::abs(m_dot(w.right, w.up)) > .0001f ||
        std::abs(m_dot(w.right, w.direction)) > .0001f ||
        std::abs(m_dot(w.up, w.direction)) > .0001f || !std::ranges::all_of(w.points, [](auto p) {
            return finite(p);
        })) {
        return {};
    }
    try {
        const auto origin = w.points[0];
        const auto xy = [&](Point3F p) {
            p -= origin;
            return XY{m_dot(p, w.right), m_dot(p, w.up)};
        };
        Poly window;
        std::ranges::transform(w.points, std::back_inserter(window), xy);
        if (std::abs(area(window)) < 1e-10 || std::ranges::any_of(w.points, [&](auto p) {
                return std::abs(m_dot(p - origin, w.direction)) > .0001f;
            })) {
            return {};
        }
        std::size_t work = MaxWork;
        const auto windowEdges = edges(window);
        std::vector<Candidate> candidates;
        if (std::ranges::any_of(windowEdges, [&](auto edge) {
                return std::ranges::any_of(window, [&](auto p) {
                    return edge.at(p) < -1e-8;
                });
            })) {
            return {};
        }
        for (const auto& instance : instances) {
            if (!instance.mesh || !rigid(instance.placement) ||
                instance.mesh->triangleCount() > 1'048'576) {
                return {};
            }
            for (const auto& t : instance.mesh->surfaceTriangles(true)) {
                spend(work);
                if (t.material >= instance.mesh->materials.list().materials.size()) {
                    return {};
                }
                const auto& material = instance.mesh->materials.list().materials[t.material];
                if (!material.type() || (material.flags & 0x2000U)) {
                    continue;
                }
                std::array<Point3F, 3> points;
                std::ranges::transform(t.vertices, points.begin(), [&](const auto& v) {
                    return point(v.position, instance.placement);
                });
                if (!std::ranges::all_of(points, [](auto p) {
                        return finite(p);
                    })) {
                    return {};
                }
                Point3F normal;
                m_normal(points[0], points[1], points[2], normal);
                const auto normalLength = normal.len();
                if (!std::isfinite(normalLength)) {
                    return {};
                }
                if (normalLength < 1e-10f) {
                    continue;
                }
                normal /= normalLength;
                const auto dot = m_dot(normal, w.direction);
                if (dot >= -.01f) {
                    continue;
                }
                const auto constant = m_dot(normal, points[0]);
                Candidate c;
                c.normal = normal;
                c.constant = constant;
                c.depth = {-Flt64(m_dot(normal, w.right)) / dot,
                           -Flt64(m_dot(normal, w.up)) / dot,
                           Flt64(m_dot(normal, points[0] - origin)) / dot};
                std::ranges::transform(points, std::back_inserter(c.polygon), xy);
                for (const auto& edge : windowEdges) {
                    c.polygon = clip(c.polygon, edge, work);
                }
                c.polygon = clip(c.polygon, c.depth, work);
                c.polygon = clip(c.polygon, {-c.depth.x, -c.depth.y, length - c.depth.c}, work);
                if (c.polygon.empty()) {
                    continue;
                }
                c.edges = edges(c.polygon);
                if (candidates.size() == 4096) {
                    return {};
                }
                candidates.push_back(std::move(c));
            }
        }
        std::vector<sim::ShadowReceiver> result;
        for (auto i : std::views::iota(std::size_t{0}) | std::views::take_while([&](std::size_t i) {
                          return i < candidates.size();
                      })) {
            const auto& c = candidates[i];
            std::vector<Poly> fragments{c.polygon};
            for (auto j :
                 std::views::iota(std::size_t{0}) | std::views::take_while([&](std::size_t j) {
                     return j < candidates.size() && !fragments.empty();
                 })) {
                spend(work);
                if (i == j) {
                    continue;
                }
                const auto& occluder = candidates[j];
                auto cuts = occluder.edges;
                const Half depth{c.depth.x - occluder.depth.x,
                                 c.depth.y - occluder.depth.y,
                                 c.depth.c - occluder.depth.c};
                if (std::abs(depth.x) + std::abs(depth.y) + std::abs(depth.c) < 1e-8) {
                    if (j > i) {
                        continue;
                    }
                } else {
                    cuts.push_back(depth);
                }
                std::vector<Poly> remaining;
                for (auto fragment : fragments) {
                    auto hidden = fragment;
                    for (auto cut : cuts) {
                        hidden = clip(hidden, cut, work);
                        if (hidden.empty()) {
                            break;
                        }
                    }
                    if (hidden.empty()) {
                        remaining.push_back(std::move(fragment));
                        continue;
                    }
                    // The convex intersection is hidden. Keep the disjoint
                    // outside strips as each BSP halfspace removes its part.
                    for (auto cut : cuts) {
                        auto outside = clip(fragment, {-cut.x, -cut.y, -cut.c}, work);
                        if (!outside.empty()) {
                            remaining.push_back(std::move(outside));
                        }
                        fragment = clip(fragment, cut, work);
                        if (fragment.empty()) {
                            break;
                        }
                    }
                    if (remaining.size() > 4096) {
                        return {};
                    }
                }
                fragments = std::move(remaining);
            }
            for (const auto& fragment : fragments) {
                if (result.size() == 100) {
                    return {};
                }
                sim::ShadowReceiver receiver{c.normal, c.constant, {}};
                std::ranges::transform(
                    fragment, std::back_inserter(receiver.vertices), [&](auto p) {
                        return origin + w.right * Flt32(p.x) + w.up * Flt32(p.y) +
                               w.direction * Flt32(c.depth.at(p));
                    });
                result.push_back(std::move(receiver));
            }
        }
        return result;
    } catch (const std::length_error&) {
        return {};
    }
}

std::optional<sim::ShadowGeometry>
worldShadowGeometry(const sim::ShadowSourceWindow& window,
                    std::span<const WorldSurfaceInstance> instances,
                    Point3F cameraDirection,
                    Flt32 length,
                    Flt32 lift,
                    bool nativePartition) {
    try {
        std::optional<std::vector<sim::ShadowReceiver>> receivers;
        if (!nativePartition) {
            receivers = collectShadowReceivers(window, instances, length);
        } else {
            if (instances.size() > 512 || !std::isfinite(length) || length <= 0 || length > 65536) {
                return {};
            }
            receivers.emplace();
            std::size_t work = collision_detail::MaxWork;
            for (const auto& instance : instances) {
                if (!instance.mesh || !collision_detail::rigid(instance.placement)) {
                    return {};
                }
                for (const auto& triangle : instance.mesh->surfaceTriangles(true)) {
                    collision_detail::spend(work);
                    if (triangle.material >= instance.mesh->materials.list().materials.size()) {
                        return {};
                    }
                    const auto& material =
                        instance.mesh->materials.list().materials[triangle.material];
                    if (!material.type() || (material.flags & 0x2000U)) {
                        continue;
                    }
                    sim::ShadowReceiver receiver;
                    for (const auto& vertex : triangle.vertices) {
                        const auto p = collision_detail::point(vertex.position, instance.placement);
                        if (!collision_detail::finite(p)) {
                            return {};
                        }
                        receiver.vertices.push_back(p);
                    }
                    Box2F footprint, source;
                    const auto xy = [&](Point3F p) {
                        p -= window.points[0];
                        return Point2F{m_dot(p, window.right), m_dot(p, window.up)};
                    };
                    footprint.fMin = footprint.fMax = xy(receiver.vertices[0]);
                    source.fMin = source.fMax = xy(window.points[0]);
                    for (const auto& p : receiver.vertices) {
                        const auto v = xy(p);
                        footprint.fMin.setMin(v);
                        footprint.fMax.setMax(v);
                    }
                    for (const auto& p : window.points) {
                        const auto v = xy(p);
                        source.fMin.setMin(v);
                        source.fMax.setMax(v);
                    }
                    if (footprint.fMax.x < source.fMin.x - .05f ||
                        footprint.fMin.x > source.fMax.x + .05f ||
                        footprint.fMax.y < source.fMin.y - .05f ||
                        footprint.fMin.y > source.fMax.y + .05f) {
                        continue;
                    }
                    m_normal(receiver.vertices[0],
                             receiver.vertices[1],
                             receiver.vertices[2],
                             receiver.normal);
                    if (receiver.normal.len() < 1e-8f) {
                        continue;
                    }
                    receiver.normal.normalize();
                    if (m_dot(receiver.normal, window.direction) >= -.01f) {
                        continue;
                    }
                    receiver.constant = m_dot(receiver.normal, receiver.vertices[0]);
                    const auto planeDot = m_dot(receiver.normal, window.direction);
                    const auto distance =
                        (receiver.constant - m_dot(receiver.normal, window.points[0])) / planeDot;
                    if (std::abs(m_dot(receiver.normal, window.right)) +
                                std::abs(m_dot(receiver.normal, window.up)) <
                            1e-8f &&
                        (distance < 0 || distance > length)) {
                        continue;
                    }
                    // Native projectionList contains authored polygons, not the
                    // renderer's triangulation diagonals. Reconstruct convex
                    // coplanar unions before applying the native BSP tolerance.
                    bool merged = false;
                    for (auto& previous : *receivers) {
                        collision_detail::spend(work);
                        if (previous.vertices.size() >= 32 || previous.normal != receiver.normal ||
                            previous.constant != receiver.constant) {
                            continue;
                        }
                        for (auto a : std::views::iota(std::size_t{0}) |
                                          std::views::take_while([&](std::size_t a) {
                                              return a < previous.vertices.size() && !merged;
                                          })) {
                            for (auto b : std::views::iota(std::size_t{0}) |
                                              std::views::take_while([&](std::size_t b) {
                                                  return b < 3 && !merged;
                                              })) {
                                if (previous.vertices[a] != receiver.vertices[(b + 1) % 3] ||
                                    previous.vertices[(a + 1) % previous.vertices.size()] !=
                                        receiver.vertices[b]) {
                                    continue;
                                }
                                auto combined = previous;
                                combined.vertices.clear();
                                for (auto k : std::views::iota(std::size_t{0}) |
                                                  std::views::take_while([&](std::size_t k) {
                                                      return k < previous.vertices.size();
                                                  })) {
                                    combined.vertices.push_back(
                                        previous.vertices[(a + 1 + k) % previous.vertices.size()]);
                                }
                                combined.vertices.push_back(receiver.vertices[(b + 2) % 3]);
                                bool convex = true;
                                for (auto k :
                                     std::views::iota(std::size_t{0}, combined.vertices.size())) {
                                    Point3F n;
                                    m_normal(combined.vertices[k],
                                             combined.vertices[(k + 1) % combined.vertices.size()],
                                             combined.vertices[(k + 2) % combined.vertices.size()],
                                             n);
                                    if (m_dot(n, combined.normal) < -1e-8f) {
                                        convex = false;
                                        break;
                                    }
                                }
                                if (convex) {
                                    previous = std::move(combined);
                                    merged = true;
                                }
                            }
                        }
                        if (merged) {
                            break;
                        }
                    }
                    if (!merged) {
                        if (receivers->size() == 4096) {
                            return {};
                        }
                        receivers->push_back(std::move(receiver));
                    }
                }
            }
        }
        if (!receivers) {
            return {};
        }
        auto geometry = nativePartition
                            ? sim::nativeShadowGeometry(window, *receivers, cameraDirection, lift)
                            : sim::buildShadowGeometry(window, *receivers, cameraDirection, lift);
        sim::ShadowGeometry triangles;
        triangles.maxPlaneDot = geometry.maxPlaneDot;
        for (const auto& p : geometry.polygons) {
            for (auto i :
                 std::views::iota(std::size_t{1}) | std::views::take_while([&](std::size_t i) {
                     return i + 1 < p.vertices.size();
                 })) {
                Point3F normal;
                m_normal(p.vertices[0].position,
                         p.vertices[i].position,
                         p.vertices[i + 1].position,
                         normal);
                if (normal.len() < 1e-8f) {
                    continue;
                }
                if (triangles.polygons.size() == 100) {
                    return {};
                }
                triangles.polygons.push_back(
                    {p.receiverIndex, {p.vertices[0], p.vertices[i], p.vertices[i + 1]}});
            }
        }
        return triangles;
    } catch (const std::invalid_argument&) {
        return {};
    } catch (const std::length_error&) {
        return {};
    }
}
} // namespace opent1::world
