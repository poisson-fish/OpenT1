#include <opent1/gfx/render_limits.hpp>
#include <opent1/gfx/render_stats.hpp>
#include <opent1/world/ts_static_shape.hpp>
#include <map>
#include <algorithm>
#include <cmath>
#include <limits>
#include <ranges>

namespace opent1::world {
namespace {
bool finite(Point3F p) {
    return std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z);
}

bool at(Int32 n, std::size_t size) {
    return n >= 0 && std::size_t(n) < size;
}

bool validView(const TsStaticView& v) {
    return v.width > 0 && v.width <= 16384 && v.height > 0 && v.height <= 16384 &&
           std::isfinite(v.focal.x) && std::isfinite(v.focal.y) && v.focal.x > 0 && v.focal.y > 0 &&
           std::isfinite(v.center.x) && std::isfinite(v.center.y) &&
           std::abs(v.center.x) <= 32768 && std::abs(v.center.y) <= 32768 &&
           std::isfinite(v.nearDistance) && std::isfinite(v.farDistance) &&
           v.nearDistance >= 1.f / 65536 && v.farDistance > v.nearDistance &&
           v.farDistance <= 65536 && std::isfinite(v.minimumProjectedAltitude) &&
           std::isfinite(v.detailScale) && v.detailScale > 0 && std::isfinite(v.opacity) &&
           v.opacity >= 0 && v.opacity <= 1 && std::isfinite(v.haze) && v.haze >= 0 &&
           v.haze <= 1 && v.minimumProjectedAltitude >= 0 && v.minimumProjectedAltitude <= 16 &&
           (!v.lighting || v.lighting->valid()) && finite(v.shapeToCamera.p) &&
           std::ranges::all_of(v.shapeToCamera.m, [](const auto& row) {
               return std::ranges::all_of(row, [](auto x) {
                   return std::isfinite(x);
               });
           });
}

struct Vertex {
    Point3F p;
    Point2F uv;
    Point3F color{1, 1, 1};
    Point2F lightMap{};
    Flt32 farSphereDistance{}, haze{};
    UInt8 screenBoundary{};
};

Flt64 distance(const Vertex& a, Int32 plane, const TsStaticView& v) {
    const auto depth = v.perspective ? Flt64(a.p.y) : 1.;
    switch (plane) {
        case 0:
            return Flt64(a.p.y) - v.nearDistance;
        case 1:
            return Flt64(v.farDistance) - a.p.y;
        case 2:
            return Flt64(a.p.x) * v.focal.x + depth * v.center.x;
        case 3:
            return -Flt64(a.p.x) * v.focal.x + depth * (Flt64(v.width) - v.center.x);
        case 4:
            return -Flt64(a.p.z) * v.focal.y + depth * v.center.y;
        default:
            return Flt64(a.p.z) * v.focal.y + depth * (Flt64(v.height) - v.center.y);
    }
}

// Both buffers belong to one mesh preparation and retain capacity across its
// triangles. Clipping still follows the original plane/interpolation order.
std::span<const Vertex> clip(std::vector<Vertex>& vertices,
                             std::vector<Vertex>& out,
                             const TsStaticView& view,
                             bool sphere = false) {
    std::ranges::for_each(std::views::iota(0, sphere ? 7 : 6), [&](auto plane) {
        const auto signedDistance = [&](const Vertex& vertex) {
            return sphere && plane == 2
                       ? Flt64(view.farDistance) - vertex.farSphereDistance
                       : distance(vertex, sphere && plane > 2 ? plane - 1 : plane, view);
        };
        if (vertices.empty()) {
            return;
        }
        // Most triangles are wholly inside or outside each plane. Avoid six
        // heap allocations per triangle when clipping leaves it unchanged.
        bool inside = false, outside = false;
        for (const auto& vertex : vertices) {
            if (signedDistance(vertex) >= 0) {
                inside = true;
            } else {
                outside = true;
            }
        }
        if (!outside) {
            return;
        }
        if (!inside) {
            vertices.clear();
            return;
        }
        out.clear();
        out.reserve(vertices.size() + 1);
        auto previous = vertices.back();
        auto previousDistance = signedDistance(previous);
        std::ranges::for_each(vertices, [&](const Vertex& current) {
            const auto currentDistance = signedDistance(current);
            if ((currentDistance >= 0) != (previousDistance >= 0)) {
                const auto t = previousDistance / (previousDistance - currentDistance);
                const auto lerp = [&](Flt32 a, Flt32 b) {
                    return Flt32(Flt64(a) + (Flt64(b) - a) * t);
                };
                out.push_back(
                    {{lerp(previous.p.x, current.p.x),
                      lerp(previous.p.y, current.p.y),
                      lerp(previous.p.z, current.p.z)},
                     {lerp(previous.uv.x, current.uv.x), lerp(previous.uv.y, current.uv.y)},
                     {lerp(previous.color.x, current.color.x),
                      lerp(previous.color.y, current.color.y),
                      lerp(previous.color.z, current.color.z)},
                     {lerp(previous.lightMap.x, current.lightMap.x),
                      lerp(previous.lightMap.y, current.lightMap.y)},
                     lerp(previous.farSphereDistance, current.farSphereDistance),
                     lerp(previous.haze, current.haze)});
                const auto viewPlane = sphere && plane > 2 ? plane - 1 : plane;
                // A clipped edge lies exactly on its screen plane. Retain
                // that provenance instead of reprojecting a rounded float
                // intersection slightly inside and losing a boundary column.
                out.back().screenBoundary = UInt8(previous.screenBoundary & current.screenBoundary);
                if (!(sphere && plane == 2) && viewPlane >= 2) {
                    out.back().screenBoundary |= UInt8(1U << (viewPlane - 2));
                }
            }
            if (currentDistance >= 0) {
                out.push_back(current);
            }
            previous = current;
            previousDistance = currentDistance;
        });
        vertices.swap(out);
    });
    return vertices;
}

gfx::PolygonVertex project(const Vertex& v, const TsStaticView& view) {
    const auto inverse =
        view.perspective ? 1 / std::clamp(v.p.y, view.nearDistance, view.farDistance) : 1.f;
    const auto depth = view.perspective ? inverse
                                        : (view.nearDistance + view.farDistance - v.p.y) /
                                              (view.nearDistance * view.farDistance);
    auto x = std::clamp(view.center.x + v.p.x * view.focal.x * inverse, 0.f, Flt32(view.width));
    auto y = std::clamp(view.center.y - v.p.z * view.focal.y * inverse, 0.f, Flt32(view.height));
    if (v.screenBoundary & 1) {
        x = 0;
    } else if (v.screenBoundary & 2) {
        x = Flt32(view.width);
    }
    if (v.screenBoundary & 4) {
        y = 0;
    } else if (v.screenBoundary & 8) {
        y = Flt32(view.height);
    }
    return {{x, y}, v.uv, v.color, v.haze, depth, v.lightMap};
}

Flt64 projectedArea(const std::array<gfx::PolygonVertex, 3>& v) {
    const auto a = v[0].position, b = v[1].position, c = v[2].position;
    return (Flt64(b.x) - a.x) * (Flt64(c.y) - a.y) - (Flt64(b.y) - a.y) * (Flt64(c.x) - a.x);
}

bool visibleTriangle(const std::array<gfx::PolygonVertex, 3>& v, Flt32 minimumAltitude) {
    const auto a = v[0].position, b = v[1].position, c = v[2].position;
    const auto area = projectedArea(v);
    const auto length = [](Point2F p, Point2F q) {
        return std::hypot(Flt64(p.x) - q.x, Flt64(p.y) - q.y);
    };
    return area > 0 &&
           (minimumAltitude == 0 ||
            area / std::max({length(a, b), length(b, c), length(c, a)}) >= minimumAltitude);
}

Point3F color(const TsMaterial& m, const gfx::Palette* palette) {
    if (m.type() == 1 && palette) {
        const auto& c = palette->find().colors[std::size_t(m.index)];
        return {Flt32(c.red) / 255, Flt32(c.green) / 255, Flt32(c.blue) / 255};
    }
    return m.type() == 2
               ? Point3F{Flt32(m.rgb[0]) / 255, Flt32(m.rgb[1]) / 255, Flt32(m.rgb[2]) / 255}
               : Point3F{1, 1, 1};
}
} // namespace

std::optional<UInt32>
tsSphereVisibility(const TsStaticView& view, const Point3F& center, Flt32 radius) {
    if (!validView(view) || !finite(center) || !std::isfinite(radius) || radius < 0) {
        return {};
    }
    Point3F p;
    m_mul(center, view.shapeToCamera, p);
    if (!finite(p)) {
        return {};
    }
    const auto left = -view.center.x / view.focal.x,
               right = (Flt32(view.width) - view.center.x) / view.focal.x;
    const auto top = view.center.y / view.focal.y,
               bottom = -(Flt32(view.height) - view.center.y) / view.focal.y;
    if (!view.perspective) {
        return p.x > left && p.x < right && p.z < top && p.z > bottom && p.y > view.nearDistance &&
                       p.y < view.farDistance
                   ? 0U
                   : 0x80U;
    }
    const auto ll = std::sqrt(1 + left * left), rl = std::sqrt(1 + right * right),
               tl = std::sqrt(1 + top * top), bl = std::sqrt(1 + bottom * bottom);
    UInt32 result{};
    if (p.x - rl * radius > p.y * right) {
        return 0x80U;
    }
    if (p.x - ll * radius < p.y * left) {
        result |= 8;
    }
    if (p.x + ll * radius < p.y * left) {
        return 0x80U;
    }
    if (p.x + rl * radius > p.y * right) {
        result |= 16;
    }
    if (p.z - tl * radius > p.y * top) {
        return 0x80U;
    }
    if (p.z - bl * radius < p.y * bottom) {
        result |= 64;
    }
    if (p.z + bl * radius < p.y * bottom) {
        return 0x80U;
    }
    if (p.z + tl * radius > p.y * top) {
        result |= 32;
    }
    if (p.y - radius < view.nearDistance) {
        result |= 1;
    }
    if (p.y + radius < view.nearDistance || p.y - radius > view.farDistance) {
        return 0x80U;
    }
    if (p.y + radius > view.farDistance) {
        result |= 2;
    }
    return result;
}

std::optional<TsStaticShapeFrame>
TsStaticShapeFrame::prepareSurfaces(TsMaterialResources materials,
                                    std::span<const StaticSurfaceTriangle> triangles,
                                    const TsStaticView& view) {
    if (!validView(view) || triangles.size() > 1'048'576 ||
        materials.list.materials.size() != materials.textures.size()) {
        return {};
    }
    TsStaticShapeFrame result;
    result.viewport_ = {view.width, view.height};
    result.resources_.materials = std::move(materials);
    result.perspective_ = view.perspective;
    result.opacity_ = view.opacity;
    result.texturePerspective_ = view.texturePerspective;
    std::vector<Vertex> vertices, scratch;
    vertices.reserve(10);
    scratch.reserve(10);
    std::optional<TMat3F> cameraToShape;
    if (!std::ranges::all_of(triangles, [&](const auto& triangle) {
            if (triangle.material >= result.resources_.materials.list.materials.size()) {
                return false;
            }
            if (triangle.lightMap && !triangle.lightMap->valid()) {
                return false;
            }
            if (!result.resources_.materials.list.materials[triangle.material].type()) {
                return true;
            }
            vertices.clear();
            for (const auto& vertex : triangle.vertices) {
                if (!finite(vertex.position) || !finite(vertex.intensity) ||
                    !std::isfinite(vertex.texture.x) || !std::isfinite(vertex.texture.y) ||
                    vertex.intensity.x < 0 || vertex.intensity.y < 0 || vertex.intensity.z < 0 ||
                    std::max({vertex.intensity.x, vertex.intensity.y, vertex.intensity.z}) > 1 ||
                    !std::isfinite(vertex.lightMap.x) || !std::isfinite(vertex.lightMap.y) ||
                    std::abs(vertex.lightMap.x) > 16384 || std::abs(vertex.lightMap.y) > 16384 ||
                    !std::isfinite(vertex.farSphereDistance) || !std::isfinite(vertex.haze) ||
                    vertex.haze < 0 || vertex.haze > 1) {
                    return false;
                }
                Point3F p;
                m_mul(vertex.position, view.shapeToCamera, p);
                if (!finite(p)) {
                    return false;
                }
                const auto texture =
                    result.resources_.materials.list.materials[triangle.material].type() == 3
                        ? vertex.texture
                        : Point2F{};
                vertices.push_back({p,
                                    texture,
                                    vertex.intensity,
                                    vertex.lightMap,
                                    vertex.farSphereDistance,
                                    vertex.haze});
            }
            const auto clipped = clip(vertices, scratch, view, triangle.nativeFarSphere);
            if (clipped.size() < 3) {
                return true;
            }
            return std::ranges::all_of(
                std::views::iota(std::size_t{1}, clipped.size() - 1), [&](auto i) {
                    Polygon p{{project(clipped[0], view),
                               project(clipped[i], view),
                               project(clipped[i + 1], view)},
                              triangle.material,
                              triangle.lit && !triangle.lightMap ? gfx::PolygonSource::Vertex
                                                                 : gfx::PolygonSource::None,
                              1,
                              triangle.repeatTexture,
                              triangle.lightMap};
                    p.nativeSoftware = triangle.nativeSoftware;
                    if (p.nativeSoftware) {
                        const auto& native = *p.nativeSoftware;
                        if (!cameraToShape) {
                            cameraToShape = view.shapeToCamera;
                            cameraToShape->inverse();
                            if (gfx::currentRenderWorkStats) {
                                ++gfx::currentRenderWorkStats->surfaceInversions;
                            }
                        }
                        Flt32 nearest = 1e20f;
                        for (const auto point : native.points) {
                            nearest = std::min(nearest, m_dist(point, cameraToShape->p));
                        }
                        const auto ratio = nearest / (view.focal.x * native.texelScale);
                        p.nativeMip = !native.fixedMip && ratio >= 1 &&
                                              ratio < Flt32(std::numeric_limits<Int32>::max())
                                          ? Int32(std::bit_width(UInt32(ratio)))
                                          : 0;
                        p.nativeMip = std::max(native.minimumMip, p.nativeMip);
                        p.nativeMip = std::min(
                            p.nativeMip,
                            Int32(std::bit_width(
                                UInt32(std::min(native.settings.size.x, native.settings.size.y)))) -
                                1);
                        p.softwareVertices = p.vertices;
                        const auto* bitmap =
                            result.resources_.materials.textures[triangle.material].get();
                        if (!bitmap) {
                            return false;
                        }
                        for (auto& vertex : p.softwareVertices) {
                            vertex.texture = {
                                (vertex.texture.x * bitmap->width - native.settings.offset.x) /
                                    native.settings.size.x,
                                (vertex.texture.y * bitmap->height - native.settings.offset.y) /
                                    native.settings.size.y};
                        }
                    }
                    if (view.twoSided && projectedArea(p.vertices) < 0) {
                        std::swap(p.vertices[1], p.vertices[2]);
                        if (p.nativeSoftware) {
                            std::swap(p.softwareVertices[1], p.softwareVertices[2]);
                        }
                    }
                    std::ranges::transform(
                        p.vertices, p.lightCoordinates.begin(), [](const auto& vertex) {
                            return vertex.lightMap;
                        });
                    if (!visibleTriangle(p.vertices, view.minimumProjectedAltitude)) {
                        return true;
                    }
                    if (result.polygons_.size() == gfx::renderLimits().polygons) {
                        return false;
                    }
                    result.polygons_.push_back(p);
                    return true;
                });
        })) {
        return {};
    }
    return result;
}

std::optional<TsStaticShapeFrame> TsStaticShapeFrame::prepare(TsShapeResources resources,
                                                              const TsStaticView& view) {
    auto pose = TsShapePose::sample(resources.shape);
    return pose ? prepare(std::move(resources), view, *pose) : std::nullopt;
}

std::optional<TsStaticShapeFrame> TsStaticShapeFrame::prepare(TsShapeResources resources,
                                                              const TsStaticView& view,
                                                              const TsShapePose& pose) {
    if (!validView(view) || !resources.shape ||
        resources.materials.list.materials.size() != resources.materials.textures.size()) {
        return {};
    }
    const auto& shape = *resources.shape;
    if (pose.shape().get() != resources.shape.get() ||
        pose.nodeTransforms().size() != shape.nodes.size() ||
        pose.objects().size() != shape.objects.size()) {
        return {};
    }
    Int32 detail = view.detail;
    if (view.automaticDetail) {
        Point3F center;
        m_mul(shape.center, view.shapeToCamera, center);
        const auto& row = view.shapeToCamera.m[0];
        const auto scale = (view.shapeToCamera.flags & RMat3F::Matrix_HasScale)
                               ? std::hypot(Flt64(row[0]), Flt64(row[1]), Flt64(row[2]))
                               : 1.;
        const auto size = Flt64(shape.radius) * scale * view.focal.x * view.detailScale /
                          (view.perspective ? std::max(Flt64(center.y), .001) : 1.);
        if (!finite(center) || !std::isfinite(size) || size < 0) {
            return {};
        }
        const auto selected =
            selectTsDetail(shape, Flt32(std::min(size, Flt64(std::numeric_limits<Flt32>::max()))));
        if (!selected) {
            return {};
        }
        detail = *selected;
    }
    TsStaticShapeFrame result;
    result.viewport_ = {view.width, view.height};
    result.detail_ = detail;
    result.perspective_ = view.perspective;
    result.opacity_ = view.opacity;
    result.texturePerspective_ = view.texturePerspective;
    if (view.automaticDetail && detail == -1) {
        result.resources_ = std::move(resources);
        return result;
    }
    if (view.automaticDetail && !(view.shapeToCamera.flags & RMat3F::Matrix_HasScale)) {
        // Match the native shape-sphere admission before visiting animated
        // meshes. Offscreen/behind-camera objects otherwise select their most
        // expensive detail and transform/light every triangle only to clip it.
        const auto visibility = tsSphereVisibility(view, shape.center, shape.radius);
        if (!visibility) {
            return {};
        }
        if (*visibility & 0x80U) {
            result.resources_ = std::move(resources);
            return result;
        }
    }
    if (shape.nodes.empty() || shape.nodes.size() > 65536 ||
        (shape.details.empty() ? detail != 0 : !at(detail, shape.details.size()))) {
        return {};
    }
    const auto root = shape.details.empty() ? 1 : shape.details[std::size_t(detail)].root;
    if (!at(root, shape.nodes.size()) ||
        (shape.alwaysNode != -1 && !at(shape.alwaysNode, shape.nodes.size()))) {
        return {};
    }
    const auto transforms = pose.nodeTransforms();
    std::vector<bool> selected(shape.nodes.size());
    if (!std::ranges::all_of(std::views::iota(std::size_t{0}, shape.nodes.size()), [&](auto i) {
            const auto& node = shape.nodes[i];
            if (!at(node.defaultTransform, shape.transforms.size()) ||
                (node.parent != -1 && !at(node.parent, i))) {
                return false;
            }
            selected[i] = Int32(i) == root || Int32(i) == shape.alwaysNode ||
                          (node.parent >= 0 && selected[std::size_t(node.parent)]);
            return true;
        })) {
        return {};
    }
    std::size_t work{};
    std::vector<Vertex> vertices, scratch;
    vertices.reserve(10);
    scratch.reserve(10);
    const auto ok = std::ranges::all_of(
        std::views::iota(std::size_t{0}, shape.objects.size()), [&](auto objectIndex) {
            const auto& object = shape.objects[objectIndex];
            const auto& state = pose.objects()[objectIndex];
            if (!at(object.node, selected.size()) || !at(object.mesh, shape.meshes.size())) {
                return false;
            }
            if (!selected[std::size_t(object.node)] ||
                !pose.nodeVisibility()[std::size_t(object.node)] || !state.visible) {
                return true;
            }
            const auto& mesh = shape.meshes[std::size_t(object.mesh)];
            if (mesh.faces.size() > gfx::renderLimits().polygons - work) {
                return false;
            }
            work += mesh.faces.size();
            TMat3F toCamera;
            m_mul(transforms[std::size_t(object.node)], view.shapeToCamera, toCamera);
            std::optional<TsPreparedLighting> preparedLights;
            if (view.lighting) {
                const auto low = mesh.point(0, state.frame), high = mesh.point(1, state.frame);
                if (!low || !high) {
                    return false;
                }
                // Native CelAnimMesh bounds precede rendered vertices. Object offset
                // participates in the transform used by both preparation and draw.
                auto objectToShape = transforms[std::size_t(object.node)];
                objectToShape.preTranslate(object.offset);
                const auto center = (*low + *high) * .5f;
                const auto radius = (*high - *low).lenf() * .5f;
                preparedLights =
                    TsPreparedLighting::prepare(*view.lighting, center, radius, objectToShape);
                if (!preparedLights) {
                    return false;
                }
            }
            return std::ranges::all_of(mesh.faces, [&](const auto& face) {
                if (face.material == -1) {
                    return true;
                } // native unmaterialed face sentinel
                auto materialIndex = face.material;
                if (!pose.materialRemap().empty()) {
                    if (!at(face.material, pose.materialRemap().size())) {
                        return false;
                    }
                    materialIndex = pose.materialRemap()[std::size_t(face.material)];
                }
                if (!at(materialIndex, resources.materials.list.materials.size()) ||
                    materialIndex >= resources.materials.list.materialsPerDetail) {
                    return false;
                }
                const auto& material =
                    resources.materials.list.materials[std::size_t(materialIndex)];
                if (!material.type()) {
                    return true;
                }
                const auto shading = material.flags & 0xf00U;
                const bool lit = view.lighting && (shading == 0x200U || shading == 0x400U) &&
                                 !(material.type() == 3 && (material.flags & 0x3000U));
                Flt32 flatShade = 1;
                vertices.clear();
                if (!std::ranges::all_of(face.corners, [&](const auto& corner) {
                        auto p = mesh.point(corner.vertex, state.frame);
                        const auto texture =
                            Int64(corner.texture) +
                            Int64(state.textureFrame) * mesh.textureVerticesPerFrame;
                        if (!p || corner.texture < 0 || texture < 0 ||
                            UInt64(texture) >= mesh.textureVertices.size()) {
                            return false;
                        }
                        Point3F intensity{1, 1, 1};
                        const auto meshPoint = *p;
                        *p += object.offset;
                        if (lit && (shading == 0x400U || vertices.empty())) {
                            const auto vertex =
                                Int64(mesh.frames[std::size_t(state.frame)].firstVertex) +
                                corner.vertex;
                            if (vertex < 0 || UInt64(vertex) >= mesh.vertices.size()) {
                                return false;
                            }
                            const auto normal =
                                decodeTsNormal(mesh.vertices[std::size_t(vertex)].normal);
                            const auto light = preparedLights->lightVertex(meshPoint, normal);
                            if (!light) {
                                return false;
                            }
                            intensity = *light;
                        }
                        m_mul(*p, toCamera, *p);
                        const auto uv = mesh.textureVertices[std::size_t(texture)];
                        if (!finite(*p) || !std::isfinite(uv.x) || !std::isfinite(uv.y)) {
                            return false;
                        }
                        vertices.push_back(
                            {*p,
                             uv,
                             intensity,
                             {},
                             0,
                             view.perspective && !(material.flags & 0x2000U) && view.haze > .0001f
                                 ? view.haze
                                 : 0.f});
                        return true;
                    })) {
                    return false;
                }
                if (lit && shading == 0x200U) {
                    const auto c = vertices.front().color;
                    flatShade = std::min(1.f, c.x * .299f + c.z * .587f + c.y * .114f);
                }
                const auto clipped = clip(vertices, scratch, view);
                if (clipped.size() < 3) {
                    return true;
                }
                return std::ranges::all_of(
                    std::views::iota(std::size_t{1}, clipped.size() - 1), [&](auto i) {
                        Polygon polygon{{project(clipped[0], view),
                                         project(clipped[i], view),
                                         project(clipped[i + 1], view)},
                                        std::size_t(materialIndex),
                                        lit ? (shading == 0x200U ? gfx::PolygonSource::Constant
                                                                 : gfx::PolygonSource::Vertex)
                                            : gfx::PolygonSource::None,
                                        flatShade};
                        if (view.twoSided && projectedArea(polygon.vertices) < 0) {
                            std::swap(polygon.vertices[1], polygon.vertices[2]);
                        }
                        if (!visibleTriangle(polygon.vertices, view.minimumProjectedAltitude)) {
                            return true;
                        }
                        if (result.polygons_.size() >= gfx::renderLimits().polygons) {
                            return false;
                        }
                        result.polygons_.push_back(polygon);
                        return true;
                    });
            });
        });
    if (!ok) {
        return {};
    }
    result.resources_ = std::move(resources);
    result.viewport_ = {view.width, view.height};
    // Material phases retain authored object/face order. Image sorting belongs
    // to the scene; triangle sorting here would split a native render image.
    std::ranges::stable_sort(result.polygons_, [&](const auto& a, const auto& b) {
        const auto translucent = [&](const auto& p) {
            return (result.resources_.materials.list.materials[p.material].flags & 0x2000U) != 0;
        };
        if (translucent(a) != translucent(b)) {
            return !translucent(a);
        }
        return false;
    });
    return result;
}

std::optional<std::vector<gfx::HardwarePolygon>>
TsStaticShapeFrame::hardwarePolygons(const gfx::Palette* palette, Flt32 paletteGamma) const {
    if ((palette && !palette->valid()) || !std::isfinite(paletteGamma) || paletteGamma < .05f ||
        paletteGamma > 20) {
        return {};
    }
    std::vector<gfx::HardwarePolygon> inputs;
    inputs.reserve(polygons_.size());
    if (!std::ranges::all_of(polygons_, [&](const auto& p) {
            const auto& source = resources_.materials.list.materials[p.material];
            gfx::HardwareMaterial material;
            material.perspective = perspective_;
            if (source.type() == 1 && (!palette || source.index < 0 || source.index > 255)) {
                return false;
            }
            material.color = color(source, palette);
            if (source.type() == 1 && paletteGamma != 1) {
                for (std::size_t i = 0; i < 3; ++i) {
                    material.color[i] =
                        Flt32(std::pow(material.color[i], double(1.f / paletteGamma)));
                }
            }
            material.shadeSource = p.shadeSource;
            material.shade = p.shade;
            // Original OpenGL EmitPoly queues haze for every fill mode, including
            // ordinary TS shapes. Flush it after the scene's translucent draws.
            if (std::ranges::any_of(p.vertices, [](const auto& v) {
                    return v.haze > 0;
                })) {
                material.hazeSource = gfx::PolygonSource::Vertex;
                material.deferredHaze = true;
            }
            if (p.vertices[0].haze > 0 && p.vertices[0].haze == p.vertices[1].haze &&
                p.vertices[0].haze == p.vertices[2].haze) {
                material.hazeSource = gfx::PolygonSource::Constant;
                material.haze = p.vertices[0].haze;
            }
            material.lightMap = p.lightMap.get();
            if (source.type() == 3) {
                material.wrap = p.repeatTexture;
                material.texture = resources_.materials.textures[p.material].get();
                if (!material.texture) {
                    return false;
                }
                // gOGLTx::cacheBitmap uses bilinear minification and the nearest
                // authored mip. Only BMA_TRANSPARENT disables magnification
                // filtering; the TS transparent flag separately enables alpha test.
                material.linear = true;
                material.magnificationLinear =
                    (material.texture->attributes & gfx::BitmapTransparent) == 0;
                material.mipFilter = material.texture->detailLevels > 1
                                         ? gfx::HardwareMipFilter::Nearest
                                         : gfx::HardwareMipFilter::None;
                material.transparent = (source.flags & 0x1000U) != 0;
                material.alphaCutoff = material.transparent ? .65f : 0.f;
                if (material.texture->bitDepth == 8 && paletteGamma != 1) {
                    material.paletteMode = gfx::HardwarePaletteMode::OpenGL;
                    material.paletteGamma = paletteGamma;
                }
                // ALPHA_NONE ignores palette flags on ordinary opaque textures.
                // Cutouts retain sampled alpha for the original .65 alpha test.
                if (!material.transparent) {
                    material.alphaSource = gfx::HardwareAlphaSource::Opaque;
                }
                if ((source.flags & 0x2000U) != 0) {
                    material.blend = gfx::HardwareBlend::Alpha;
                    material.depth = gfx::HardwareDepth::TestOnly;
                    material.alphaSource = gfx::HardwareAlphaSource::Texture;
                    if (material.texture->attributes & gfx::BitmapAdditive) {
                        material.blend = gfx::HardwareBlend::Additive;
                    } else if (material.texture->attributes & gfx::BitmapSubtractive) {
                        material.blend = gfx::HardwareBlend::Subtractive;
                    }
                    material.paletteMode = gfx::HardwarePaletteMode::OpenGL;
                    material.paletteGamma = paletteGamma;
                }
            }
            if (opacity_ < 1) {
                if (opacity_ == 0) {
                    return true;
                }
                material.opacity = opacity_;
                material.blend = gfx::HardwareBlend::Alpha;
                material.alphaSource = gfx::HardwareAlphaSource::Constant;
                material.depth = gfx::HardwareDepth::TestOnly;
            }
            inputs.push_back({p.vertices,
                              material,
                              p.lightMap ? std::span<const Point2F>(p.lightCoordinates)
                                         : std::span<const Point2F>{}});
            return true;
        })) {
        return {};
    }
    return inputs;
}

std::optional<gfx::HardwareBatch> TsStaticShapeFrame::hardware(const gfx::Palette* palette,
                                                               Flt32 paletteGamma) const {
    auto inputs = hardwarePolygons(palette, paletteGamma);
    return inputs ? gfx::HardwareBatch::prepare(*inputs, palette) : std::nullopt;
}

void NativeSoftwareTextureCache::clear() {
    index_.clear();
    entries_.clear();
    bytes_ = 0;
}

void NativeSoftwareTextureCache::setNoiseEnabled(bool enabled) {
    if (noiseEnabled_ != enabled) {
        noiseEnabled_ = enabled;
        clear();
    }
}

bool NativeSoftwareTextureCache::beginFrame(const gfx::Palette& palette) {
    if (palette_ && *palette_ == palette) {
        return true;
    }
    if (!palette.valid()) {
        return false;
    }
    clear();
    palette_ = palette;
    return true;
}

std::shared_ptr<const gfx::Bitmap>
NativeSoftwareTextureCache::prepare(const Resource<gfx::Bitmap>& texture,
                                    std::shared_ptr<const gfx::LightMap> light,
                                    std::shared_ptr<const NativeSurfaceTexture> native,
                                    Int32 mip) {
    if (!palette_ || !texture || !light || !native) {
        return {};
    }
    // Terrain rebuilds light/placement snapshots as the view changes. Cache
    // the inputs that affect lit pixels, not the addresses of those snapshots.
    UInt64 key = 14695981039346656037ULL;
    const auto hash = [&](UInt64 value) {
        key = (key ^ value) * 1099511628211ULL;
    };
    hash(reinterpret_cast<std::uintptr_t>(texture.get()));
    hash(UInt32(mip));
    const auto& s = native->settings;
    hash(UInt32(s.size.x));
    hash(UInt32(s.size.y));
    hash(UInt32(s.offset.x));
    hash(UInt32(s.offset.y));
    hash(UInt32(s.lightScale));
    hash(s.noiseKey);
    hash(UInt32(light->size.x));
    hash(UInt32(light->size.y));
    hash(UInt32(light->offset.x));
    hash(UInt32(light->offset.y));
    hash(UInt32(light->format));
    for (const auto value : light->samples) {
        hash(value);
    }
    const auto [first, last] = index_.equal_range(key);
    for (auto i = first; i != last; ++i) {
        const auto entry = i->second;
        const auto& settings = entry->native->settings;
        if (entry->texture.get() == texture.get() && entry->mip == mip && settings.size == s.size &&
            settings.offset == s.offset && settings.lightScale == s.lightScale &&
            settings.noiseKey == s.noiseKey && entry->light->size == light->size &&
            entry->light->offset == light->offset && entry->light->format == light->format &&
            entry->light->samples == light->samples) {
            ++hits_;
            entries_.splice(entries_.begin(), entries_, entry);
            return entries_.front().bitmap;
        }
    }
    auto settings = native->settings;
    settings.mipLevel = mip;
    std::shared_ptr<const gfx::Bitmap> bitmap =
        gfx::lightTexture(*texture,
                          *light,
                          *palette_,
                          settings,
                          noiseEnabled_ ? gfx::defaultTextureNoise() : std::span<const Int32>{});
    if (!bitmap) {
        return {};
    }
    const auto bytes = bitmap->pixels.size();
    if (bytes > gfx::Bitmap::MaxFileBytes) {
        return {};
    }
    while (!entries_.empty() &&
           (entries_.size() >= 2048 || bytes_ > gfx::Bitmap::MaxFileBytes - bytes)) {
        const auto evicted = std::prev(entries_.end());
        const auto [begin, end] = index_.equal_range(evicted->key);
        for (auto i = begin; i != end; ++i) {
            if (i->second == evicted) {
                index_.erase(i);
                break;
            }
        }
        bytes_ -= entries_.back().bitmap->pixels.size();
        entries_.pop_back();
    }
    bytes_ += bytes;
    ++builds_;
    entries_.push_front({texture, std::move(light), std::move(native), mip, bitmap, key});
    index_.emplace(key, entries_.begin());
    return bitmap;
}

std::optional<std::vector<gfx::SortedPolygon>>
TsStaticShapeFrame::softwarePolygons(const gfx::Palette* palette,
                                     NativeSoftwareTextureCache* cache) const {
    if (palette && !palette->valid()) {
        return {};
    }
    std::vector<gfx::SortedPolygon> inputs;
    inputs.reserve(polygons_.size());
    std::map<std::pair<const NativeSurfaceTexture*, const gfx::LightMap*>,
             std::shared_ptr<const gfx::Bitmap>>
        cached;
    std::size_t preparedBytes{};
    if (!std::ranges::all_of(polygons_, [&](const auto& p) {
            const auto& source = resources_.materials.list.materials[p.material];
            gfx::PolygonMaterial material;
            material.shadeSource = p.shadeSource;
            material.shade = p.shade;
            if (std::ranges::any_of(p.vertices, [](const auto& v) {
                    return v.haze > 0;
                })) {
                material.hazeSource = gfx::PolygonSource::Vertex;
            }
            if (p.vertices[0].haze > 0 && p.vertices[0].haze == p.vertices[1].haze &&
                p.vertices[0].haze == p.vertices[2].haze) {
                material.hazeSource = gfx::PolygonSource::Constant;
                material.haze = p.vertices[0].haze;
            }
            material.lightMap = p.lightMap.get();
            material.perspective = bool(p.lightMap);
            if (source.type() == 3) {
                material.texture = resources_.materials.textures[p.material].get();
                if (!material.texture) {
                    return false;
                }
                material.transparent = (source.flags & 0x1000U) != 0;
                material.blend = (source.flags & 0x2000U) != 0;
                material.perspective = texturePerspective_;
                material.clampTexture = !p.repeatTexture;
                material.wrap = p.repeatTexture;
                // TS textures carry their own subpalette indices. The software
                // framebuffer uses the world's base palette, so even an unlit
                // texture needs the brightest remap row before presentation.
                if (!material.lightMap && material.shadeSource == gfx::PolygonSource::None &&
                    !material.blend && palette &&
                    palette->find(material.texture->paletteIndex).type ==
                        gfx::PaletteType::ShadeHaze) {
                    material.shadeSource = gfx::PolygonSource::Constant;
                    material.shade = 1;
                }
            } else if (source.type() == 1) {
                if (source.index < 0 || source.index > 255) {
                    return false;
                }
                material.color = UInt8(source.index);
            } else if (source.type() == 2) {
                if (!palette) {
                    return false;
                }
                material.color = palette->nearest({source.rgb[0], source.rgb[1], source.rgb[2], 0});
            } else {
                return false;
            }
            if (p.nativeSoftware) {
                if (!palette || !p.lightMap || !material.texture) {
                    return false;
                }
                const auto key = std::pair{p.nativeSoftware.get(), p.lightMap.get()};
                auto& bitmap = cached[key];
                if (!bitmap) {
                    auto settings = p.nativeSoftware->settings;
                    settings.mipLevel = p.nativeMip;
                    bitmap = cache ? cache->prepare(resources_.materials.textures[p.material],
                                                    p.lightMap,
                                                    p.nativeSoftware,
                                                    p.nativeMip)
                                   : gfx::lightTexture(*material.texture,
                                                       *p.lightMap,
                                                       *palette,
                                                       settings,
                                                       gfx::defaultTextureNoise());
                    if (bitmap) {
                        if (bitmap->pixels.size() > 64U * 1024U * 1024U - preparedBytes) {
                            return false;
                        }
                        preparedBytes += bitmap->pixels.size();
                    }
                }
                // The original cache leaves partial light-grid coverage unwritten.
                // Such authored/synthetic rectangles and non-shade palettes use the
                // defined per-fragment path instead of exposing allocation bytes.
                if (!bitmap) {
                    inputs.push_back({p.vertices, material});
                    return true;
                }
                material.texture = bitmap.get();
                material.lightMap = nullptr;
                material.transparent = material.blend = false;
                material.shadeSource = gfx::PolygonSource::None;
                material.wrap = false;
                material.clampTexture = true;
                material.perspective = p.nativeSoftware->perspective;
                inputs.push_back({p.softwareVertices, material, false, bitmap});
            } else {
                inputs.push_back({p.vertices, material});
            }
            return true;
        })) {
        return {};
    }
    return inputs;
}

bool TsStaticShapeFrame::draw(gfx::SoftwareSurface& surface) const {
    auto inputs = softwarePolygons(surface.palette());
    return inputs && surface.sortedPolygons(*inputs);
}

std::unique_ptr<gfx::Bitmap> TsStaticShapeFrame::silhouette() const {
    if (viewport_.x <= 0 || viewport_.x > 256 || viewport_.y <= 0 || viewport_.y > 256) {
        return {};
    }
    auto surface = gfx::SoftwareSurface::create(viewport_.x, viewport_.y);
    if (!surface) {
        return {};
    }
    surface->clear(0);
    gfx::PolygonMaterial solid;
    solid.color = 255;
    if (!std::ranges::all_of(polygons_, [&](const auto& p) {
            return surface->polygon(p.vertices, solid);
        })) {
        return {};
    }
    return std::make_unique<gfx::Bitmap>(surface->bitmap());
}
} // namespace opent1::world
