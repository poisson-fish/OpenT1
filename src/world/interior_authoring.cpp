#include <opent1/world/interior_authoring.hpp>
#include <ranges>
#include <opent1/core/lzh_stream.hpp>
#include <opent1/core/mem_stream.hpp>
#include <algorithm>
#include <bit>

namespace opent1::world {
namespace {
constexpr std::size_t MaxAssetBytes = 64U * 1024U * 1024U;

struct Bytes {
    std::vector<std::byte> data;

    template <class T> void put(T value) {
        if constexpr (std::is_floating_point_v<T>) {
            put(std::bit_cast<UInt32>(value));
        } else {
            for (auto i :
                 std::views::iota(std::size_t{0}) | std::views::take_while([&](std::size_t i) {
                     return i < sizeof(T);
                 })) {
                data.push_back(std::byte((std::make_unsigned_t<T>(value) >> (i * 8)) & 255));
            }
        }
    }

    void append(std::span<const std::byte> bytes) {
        data.insert(data.end(), bytes.begin(), bytes.end());
    }

    void text(std::string_view value) {
        for (auto c : value) {
            put(UInt8(c));
        }
    }

    UInt32 name(std::string_view value) {
        const auto offset = UInt32(data.size());
        text(value);
        put(UInt8{});
        return offset;
    }

    void point(Point3F value) {
        put(value.x);
        put(value.y);
        put(value.z);
    }

    std::vector<std::byte> object(std::string_view type, Int32 version, UInt32 tag = 0) const {
        Bytes body, result;
        if (!tag) {
            body.put(Int16(type.size()));
            body.text(type);
            if (type.size() & 1) {
                body.put(UInt8{});
            }
        }
        body.put(version);
        body.append(data);
        result.put(tag ? tag : makeFourCC('P', 'E', 'R', 'S'));
        result.put(UInt32(body.data.size()));
        result.append(body.data);
        if (body.data.size() & 1) {
            result.put(UInt8{});
        }
        return result.data;
    }
};

template <class T> std::optional<std::vector<std::byte>> validated(std::vector<std::byte> bytes) {
    if (bytes.size() > MaxAssetBytes) {
        return {};
    }
    MemRWStream input{std::span<const std::byte>(bytes)};
    if (!T::load(input, UInt32(bytes.size()))) {
        return {};
    }
    return bytes;
}

bool textValid(std::string_view text) {
    return text.size() <= 255 && text.find('\0') == text.npos;
}

std::string extension(std::string value, std::string_view suffix) {
    const auto dot = value.find_last_of('.'), slash = value.find_last_of("/\\");
    if (dot != value.npos && (slash == value.npos || dot > slash)) {
        value.resize(dot);
    }
    value += suffix;
    return value;
}
}

std::optional<std::vector<std::byte>> writeInteriorGeometry(const InteriorGeometry& geometry) {
    if (geometry.surfaces.size() > 65536 || geometry.nodes.size() > 65536 ||
        geometry.leaves.size() < 43 || geometry.leaves.size() > 65536 ||
        geometry.bits.size() > 16U * 1024U * 1024U || geometry.vertices.size() > 1'048'576 ||
        geometry.points.size() > 65536 || geometry.texturePoints.size() > 65536 ||
        geometry.planes.size() > 65536) {
        return {};
    }
    std::size_t solids = 0;
    for (auto index : std::views::iota(std::size_t{0}, geometry.leaves.size())) {
        if (geometry.leaves[index].solid) {
            if (index != 43 + solids) {
                return {};
            }
            ++solids;
        }
    }
    Bytes out;
    out.put(geometry.buildId);
    out.put(geometry.textureScale);
    out.point(geometry.bounds.fMin);
    out.point(geometry.bounds.fMax);
    for (const auto size : {geometry.surfaces.size(),
                            geometry.nodes.size(),
                            solids,
                            geometry.leaves.size() - solids,
                            geometry.bits.size(),
                            geometry.vertices.size(),
                            geometry.points.size(),
                            geometry.texturePoints.size(),
                            geometry.planes.size()}) {
        out.put(UInt32(size));
    }
    for (const auto& surface : geometry.surfaces) {
        out.put(surface.flags);
        out.put(surface.material);
        out.put(surface.width);
        out.put(surface.height);
        out.put(surface.offsetX);
        out.put(surface.offsetY);
        out.put(surface.plane);
        out.put(surface.firstVertex);
        out.put(surface.pointIndex);
        out.put(surface.vertexCount);
        out.put(surface.pointCount);
        out.put(UInt16{});
    }
    for (const auto& node : geometry.nodes) {
        if (node.plane > 65535 || node.front < -32768 || node.front > 32767 || node.back < -32768 ||
            node.back > 32767) {
            return {};
        }
        out.put(UInt16(node.plane));
        out.put(Int16(node.front));
        out.put(Int16(node.back));
        out.put(UInt16{});
    }
    for (const auto& leaf : geometry.leaves) {
        if (leaf.solid) {
            out.put(leaf.surfaceIndex);
            out.put(leaf.planeIndex);
            out.put(leaf.surfaceCount);
            out.put(leaf.planeCount);
        }
    }
    for (const auto& leaf : geometry.leaves) {
        if (!leaf.solid) {
            if (leaf.flags > 1 || leaf.pvsCount > 32767) {
                return {};
            }
            out.put(UInt16(leaf.flags | (leaf.pvsCount << 1)));
            out.put(leaf.surfaceCount);
            out.put(leaf.pvsIndex);
            out.put(leaf.surfaceIndex);
            out.put(leaf.planeIndex);
            out.point(leaf.bounds.fMin);
            out.point(leaf.bounds.fMax);
            out.put(leaf.planeCount);
            out.put(UInt16{});
        }
    }
    out.append(geometry.bits);
    for (const auto& vertex : geometry.vertices) {
        out.put(vertex.point);
        out.put(vertex.texture);
    }
    for (const auto& point : geometry.points) {
        out.point(point);
    }
    for (const auto& point : geometry.texturePoints) {
        out.put(point.x);
        out.put(point.y);
    }
    for (const auto& plane : geometry.planes) {
        out.point(plane);
        out.put(plane.d);
    }
    out.put(geometry.highestMipLevel);
    out.put(geometry.flags);
    return validated<InteriorGeometry>(out.object("ITRGeometry", 7));
}

std::optional<std::vector<std::byte>> writeInteriorShape(const InteriorShape& shape) {
    if (shape.states.empty() || shape.states.size() > 256 || shape.lods.empty() ||
        shape.lods.size() > 4096 || shape.lightFiles.size() > 65536 ||
        shape.lightStateNames.empty() || shape.lightStateNames.size() > 256 ||
        !textValid(shape.materialList)) {
        return {};
    }
    Bytes names, out;
    (void)names.name("");
    std::vector<UInt32> stateNames(shape.states.size()), geometries(shape.lods.size()),
        files(shape.lightFiles.size()), profiles;
    std::vector<bool> seenLods(shape.lods.size()), seenFiles(shape.lightFiles.size());
    for (auto stateIndex : std::views::iota(std::size_t{0}, shape.states.size())) {
        const auto& state = shape.states[stateIndex];
        if (!textValid(state.name) || !state.lodCount || state.firstLod > shape.lods.size() ||
            state.lodCount > shape.lods.size() - state.firstLod) {
            return {};
        }
        stateNames[stateIndex] = names.name(state.name);
        for (auto index : std::views::iota(static_cast<UInt32>(state.firstLod)) |
                              std::views::take_while([&](UInt32 index) {
                                  return index < state.firstLod + state.lodCount;
                              })) {
            const auto& lod = shape.lods[index];
            if (!textValid(lod.geometry) || lod.firstLightState > shape.lightFiles.size() ||
                shape.lightStateNames.size() > shape.lightFiles.size() - lod.firstLightState) {
                return {};
            }
            if (!seenLods[index]) {
                geometries[index] = names.name(lod.geometry);
                seenLods[index] = true;
            }
            for (auto light : std::views::iota(static_cast<UInt32>(lod.firstLightState)) |
                                  std::views::take_while([&](UInt32 light) {
                                      return light <
                                             lod.firstLightState + shape.lightStateNames.size();
                                  })) {
                if (!textValid(shape.lightFiles[light])) {
                    return {};
                }
                if (!seenFiles[light]) {
                    files[light] = names.name(shape.lightFiles[light]);
                    seenFiles[light] = true;
                }
            }
        }
    }
    if (!std::ranges::all_of(seenLods,
                             [](bool seen) {
                                 return seen;
                             }) ||
        !std::ranges::all_of(seenFiles, [](bool seen) {
            return seen;
        })) {
        return {};
    }
    const UInt32 material = names.name(shape.materialList);
    for (const auto& name : shape.lightStateNames) {
        if (!textValid(name)) {
            return {};
        }
        profiles.push_back(names.name(name));
    }
    out.put(UInt32(shape.states.size()));
    for (auto index : std::views::iota(std::size_t{0}, shape.states.size())) {
        out.put(stateNames[index]);
        out.put(shape.states[index].firstLod);
        out.put(shape.states[index].lodCount);
    }
    out.put(UInt32(shape.lods.size()));
    for (auto index : std::views::iota(std::size_t{0}, shape.lods.size())) {
        const auto& lod = shape.lods[index];
        out.put(lod.minPixels);
        out.put(geometries[index]);
        out.put(lod.firstLightState);
        out.put(lod.linkableFaces);
    }
    out.put(UInt32(files.size()));
    for (const auto offset : files) {
        out.put(offset);
    }
    out.put(UInt32(profiles.size()));
    for (const auto offset : profiles) {
        out.put(offset);
    }
    out.put(UInt32(names.data.size()));
    out.append(names.data);
    out.put(material);
    out.put(UInt8(shape.linked));
    return validated<InteriorShape>(out.object({}, 3, makeFourCC('I', 'T', 'R', 's')));
}

std::optional<std::vector<std::byte>> writeInteriorLighting(const InteriorLighting& lighting) {
    if (lighting.states.size() > 65536 || lighting.stateData.size() > 65536 ||
        lighting.lights.size() > 65536 || lighting.surfaces.size() > 65536 ||
        lighting.mapData.size() > 32U * 1024U * 1024U || lighting.names.size() > 1'048'576 ||
        lighting.huffmanNodes.size() > 65536 || lighting.huffmanLeaves.size() > 65536 ||
        lighting.missionEntries.size() > 65536) {
        return {};
    }
    Bytes out;
    out.put(lighting.buildId);
    out.put(lighting.scaleShift);
    out.put(lighting.mapSlots);
    for (const auto size : {lighting.states.size(),
                            lighting.stateData.size(),
                            lighting.lights.size(),
                            lighting.surfaces.size(),
                            lighting.mapData.size()}) {
        out.put(UInt32(size));
    }
    for (const auto& state : lighting.states) {
        for (const auto color : state.color) {
            out.put(color);
        }
        out.put(state.time);
        out.put(state.dataCount);
        out.put(state.dataIndex);
    }
    for (const auto& data : lighting.stateData) {
        out.put(data.surface);
        out.put(data.slot);
        out.put(data.map);
    }
    for (const auto& light : lighting.lights) {
        out.put(light.id);
        out.put(light.name);
        out.put(light.stateCount);
        out.put(light.firstState);
        out.put(light.duration);
        out.put(light.flags);
    }
    for (const auto& surface : lighting.surfaces) {
        out.put(surface.map);
        out.put(surface.lightCount);
        out.put(surface.lightIndex);
        out.put(surface.width);
        out.put(surface.height);
        out.put(surface.offsetX);
        out.put(surface.offsetY);
    }
    out.append(lighting.mapData);
    out.put(UInt32(lighting.names.size()));
    out.append(lighting.names);
    const bool compressed = !lighting.huffmanNodes.empty();
    out.put(UInt8(compressed));
    if (compressed) {
        out.put(UInt32(lighting.huffmanNodes.size()));
        out.put(UInt32(lighting.huffmanLeaves.size()));
        for (const auto& node : lighting.huffmanNodes) {
            out.put(node[0]);
            out.put(node[1]);
        }
        for (const auto color : lighting.huffmanLeaves) {
            out.put(color);
        }
    } else if (!lighting.huffmanLeaves.empty()) {
        return {};
    }
    if (lighting.mission) {
        out.put(UInt32(lighting.missionEntries.size()));
        Bytes raw;
        for (const auto& entry : lighting.missionEntries) {
            raw.put(entry[0]);
            raw.put(entry[1]);
        }
        std::vector<std::byte> packed(raw.data.size() * 2 + 4096);
        MemRWStream stream{std::span<std::byte>(packed)};
        LZHWStream encoder(stream);
        if (!encoder.writeBytes(raw.data) || !encoder.flush()) {
            return {};
        }
        encoder.detach();
        if (stream.getStatus() != STRM_OK) {
            return {};
        }
        packed.resize(std::size_t(stream.getPosition()));
        out.append(packed);
    } else if (!lighting.missionEntries.empty()) {
        return {};
    }
    return validated<InteriorLighting>(
        out.object(lighting.mission ? "ITRMissionLighting" : "ITRLighting", 7));
}

std::optional<InteriorShape> exportInteriorShape(std::span<const InteriorEditState> states,
                                                 std::span<const std::string> lightStateNames,
                                                 std::string_view materials,
                                                 bool linked) {
    if (states.empty() || states.size() > 256 || lightStateNames.empty() ||
        lightStateNames.size() > 256) {
        return {};
    }
    InteriorShape shape;
    shape.materialList = materials;
    shape.linked = linked;
    shape.lightStateNames.assign(lightStateNames.begin(), lightStateNames.end());
    for (const auto& state : states) {
        if (state.lods.empty() || state.lods.size() > 4096 - shape.lods.size()) {
            return {};
        }
        shape.states.push_back({state.name, UInt32(shape.lods.size()), UInt32(state.lods.size())});
        const auto first = shape.lods.size();
        for (const auto& lod : state.lods) {
            if (lod.lightFiles.size() != lightStateNames.size()) {
                return {};
            }
            shape.lods.push_back({lod.minPixels,
                                  UInt32(shape.lightFiles.size()),
                                  lod.linkableFaces,
                                  extension(lod.geometry, ".dig")});
            for (const auto& file : lod.lightFiles) {
                shape.lightFiles.push_back(extension(file, ".dil"));
            }
        }
        std::ranges::stable_sort(shape.lods.begin() + std::ptrdiff_t(first),
                                 shape.lods.end(),
                                 std::greater<UInt32>{},
                                 &InteriorLod::minPixels);
    }
    return writeInteriorShape(shape) ? std::optional(std::move(shape)) : std::nullopt;
}
} // namespace opent1::world
