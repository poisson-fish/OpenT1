#pragma once
#include <opent1/world/ts_static_shape.hpp>
#include <list>

namespace opent1::world::ts_detail {
struct RetainedMesh {
    Resource<TsShape> source;
    Int32 mesh{}, frame{}, textureFrame{};
    std::vector<UInt8> modes;
    std::shared_ptr<const gfx::HardwareMesh> hardware;
    std::vector<UInt32> firstIndices; // one triangle per active face, UINT32_MAX for sentinels
    std::size_t bytes{};
};

struct MeshCache {
    std::list<std::shared_ptr<const RetainedMesh>> entries;
    std::size_t bytes{}, budget{32U * 1024U * 1024U};
    UInt64 hits{}, builds{};
};

inline thread_local MeshCache meshCache;

inline std::shared_ptr<const RetainedMesh> retainedMesh(const Resource<TsShape>& source,
                                                        Int32 index,
                                                        const TsObjectPose& state,
                                                        const TsMaterialResources& materials,
                                                        const TsShapePose& pose) {
    auto& cache = meshCache;
    if (cache.budget < sizeof(RetainedMesh))
        return {}; // the caller uses the projected path under a small/disabled cache policy
    const auto& mesh = source->meshes[std::size_t(index)];
    std::vector<UInt8> modes;
    modes.reserve(mesh.faces.size());
    for (const auto& face : mesh.faces) {
        if (face.material == -1) {
            modes.push_back(255);
            continue;
        }
        if (face.material < 0 || (!pose.materialRemap().empty() &&
                                  std::size_t(face.material) >= pose.materialRemap().size()))
            return {};
        const auto id = pose.materialRemap().empty()
                            ? face.material
                            : pose.materialRemap()[std::size_t(face.material)];
        if (id < 0 || id >= materials.list.materialsPerDetail ||
            std::size_t(id) >= materials.list.materials.size())
            return {};
        const auto& m = materials.list.materials[std::size_t(id)];
        const auto shading = m.flags & 0xf00U;
        modes.push_back(!m.type()                              ? 255
                        : m.type() == 3 && (m.flags & 0x3000U) ? 0
                        : shading == 0x400U                    ? 1
                        : shading == 0x200U                    ? 2
                                                               : 0);
    }
    const auto active = std::size_t(std::ranges::count_if(modes, [](auto mode) {
        return mode != 255;
    }));
    const auto plannedBytes = sizeof(RetainedMesh) + sizeof(gfx::HardwareMesh) + modes.capacity() +
                              mesh.faces.size() * sizeof(UInt32) +
                              active * 3 * (sizeof(gfx::HardwareVertex) + sizeof(UInt32));
    if (plannedBytes > cache.budget)
        return {}; // avoid expanding an oversized source before falling back
    for (auto it = cache.entries.begin(); it != cache.entries.end(); ++it) {
        const auto& e = **it;
        if (e.source.get() == source.get() && e.mesh == index && e.frame == state.frame &&
            e.textureFrame == state.textureFrame && e.modes == modes) {
            auto result = *it;
            cache.entries.splice(cache.entries.begin(), cache.entries, it);
            ++cache.hits;
            return result;
        }
    }
    auto result = std::make_shared<RetainedMesh>();
    result->source = source;
    result->mesh = index;
    result->frame = state.frame;
    result->textureFrame = state.textureFrame;
    result->modes = std::move(modes);
    result->firstIndices.resize(mesh.faces.size(), UINT32_MAX);
    std::vector<gfx::HardwareVertex> vertices;
    std::vector<UInt32> indices;
    vertices.reserve(active * 3);
    indices.reserve(active * 3);
    for (std::size_t i = 0; i < mesh.faces.size(); ++i) {
        if (result->modes[i] == 255)
            continue;
        result->firstIndices[i] = UInt32(indices.size());
        const auto& face = mesh.faces[i];
        for (const auto& corner : face.corners) {
            const auto p = mesh.point(corner.vertex, state.frame);
            const auto uv =
                Int64(corner.texture) + Int64(state.textureFrame) * mesh.textureVerticesPerFrame;
            const auto normalVertex =
                result->modes[i] == 2 ? face.corners[0].vertex : corner.vertex;
            if (!p || corner.texture < 0 || uv < 0 || UInt64(uv) >= mesh.textureVertices.size() ||
                state.frame < 0 || std::size_t(state.frame) >= mesh.frames.size())
                return {};
            const auto vertex =
                Int64(mesh.frames[std::size_t(state.frame)].firstVertex) + normalVertex;
            if (vertex < 0 || UInt64(vertex) >= mesh.vertices.size())
                return {};
            gfx::HardwareVertex v;
            v.position = {p->x, p->y};
            v.inverseDepth = p->z;
            v.texture = mesh.textureVertices[std::size_t(uv)];
            v.color = decodeTsNormal(mesh.vertices[std::size_t(vertex)].normal);
            v.opacity = Flt32(result->modes[i]);
            indices.push_back(UInt32(vertices.size()));
            vertices.push_back(v);
        }
    }
    if (!indices.empty()) {
        result->hardware = gfx::HardwareMesh::create(
            std::move(vertices), std::move(indices), gfx::HardwareMesh::Encoding::TsNormals);
        if (!result->hardware)
            return {};
    }
    result->bytes = sizeof(RetainedMesh) + result->modes.capacity() +
                    result->firstIndices.capacity() * sizeof(UInt32);
    if (result->hardware)
        result->bytes += result->hardware->residentBytes();
    ++cache.builds;
    if (result->bytes <= cache.budget) {
        while (!cache.entries.empty() &&
               (cache.entries.size() >= 512 || cache.bytes > cache.budget - result->bytes)) {
            cache.bytes -= cache.entries.back()->bytes;
            cache.entries.pop_back();
        }
        cache.bytes += result->bytes;
        cache.entries.push_front(result);
    }
    return result;
}
} // namespace opent1::world::ts_detail
