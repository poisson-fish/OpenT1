#include <opent1/world/interior_shape.hpp>
#include "asset_reader.hpp"
#include <opent1/core/lzh_stream.hpp>
#include <opent1/core/mem_stream.hpp>
#include <functional>

namespace opent1::world {
using asset_detail::Reader;

namespace {
bool box(const Box3F& b) {
    return b.fMin.x <= b.fMax.x && b.fMin.y <= b.fMax.y && b.fMin.z <= b.fMax.z;
}

std::vector<std::byte> copy(std::span<const std::byte> bytes) {
    return {bytes.begin(), bytes.end()};
}

bool digit(char c) {
    return c >= '0' && c <= '9';
}

std::string baseShapeName(std::string_view name) {
    const auto start = name.find_last_of("/\\"), extension = name.find_last_of('.');
    const auto first = start == name.npos ? 0 : start + 1;
    if (extension == name.npos || extension <= first) {
        return std::string(name);
    }
    auto marker = extension;
    while (marker > first && digit(name[marker - 1])) {
        --marker;
    }
    if (marker <= first || name[marker - 1] != '.') {
        return std::string(name);
    }
    return std::string(name.substr(0, marker - 1)) + std::string(name.substr(extension));
}

std::string baseLightName(std::string_view name) {
    const auto start = name.find_last_of("/\\"), suffix = name.find_last_of('-');
    const auto first = start == name.npos ? 0 : start + 1;
    if (suffix == name.npos || suffix <= first) {
        return std::string(name);
    }
    auto marker = suffix;
    while (marker > first && digit(name[marker - 1])) {
        --marker;
    }
    // Original unmissionLightName() keeps the numeric LOD and strips the last
    // dash suffix. Do not let directory punctuation become part of the stem.
    if (marker <= first || name[marker - 1] != '-') {
        return std::string(name);
    }
    return std::string(name.substr(0, suffix)) + ".dil";
}

bool tree(const InteriorGeometry& g) {
    if (g.nodes.empty() || g.leaves.empty() || g.planes.empty()) {
        return false;
    }
    std::vector<bool> seen(g.nodes.size());
    const std::function<bool(Int32, UInt32)> visit = [&](Int32 n, UInt32 depth) {
        if (n < 0) {
            return UInt64(-Int64(n) - 1) < g.leaves.size();
        }
        if (std::size_t(n) >= g.nodes.size() || depth > 128 || seen[std::size_t(n)]) {
            return false;
        }
        seen[std::size_t(n)] = true;
        const auto& node = g.nodes[std::size_t(n)];
        return node.plane < g.planes.size() && visit(node.front, depth + 1) &&
               visit(node.back, depth + 1);
    };
    return visit(0, 1) && std::ranges::all_of(seen, [](bool v) {
               return v;
           });
}
}

std::optional<std::vector<UInt32>>
InteriorGeometry::bitIndices(UInt32 offset, UInt32 count, UInt32 limit) const {
    if (offset > bits.size() || count > bits.size() - offset || limit > 65536) {
        return {};
    }
    Reader r{std::span(bits).subspan(offset, count)};
    std::vector<UInt32> result;
    UInt32 base{};
    while (r.position < r.bytes.size() && r.ok) {
        const auto b = r.scalar<UInt8>();
        if (!b) {
            const auto run = r.scalar<UInt8>();
            if (!run || base > 65536U * 8U - UInt32(run) * 8U) {
                return {};
            }
            base += UInt32(run) * 8U;
        } else {
            std::ranges::for_each(std::views::iota(0U, 8U), [&](auto bit) {
                if (b & (0x80U >> bit)) {
                    if (base + bit >= limit) {
                        r.ok = false;
                    } else {
                        result.push_back(base + bit);
                    }
                }
            });
            base += 8;
        }
    }
    return r.done() ? std::optional(std::move(result)) : std::nullopt;
}

std::unique_ptr<InteriorGeometry> InteriorGeometry::load(StreamIO& stream, UInt32 size) {
    auto bytes = asset_detail::file(stream, size);
    if (!bytes) {
        return {};
    }
    Reader outer{*bytes};
    Int32 version{};
    auto r = outer.object(0, "ITRGeometry", version);
    if (!outer.done() || version != 7 || !r.ok) {
        return {};
    }
    auto g = std::make_unique<InteriorGeometry>();
    g->buildId = r.scalar<Int32>();
    g->textureScale = r.real();
    g->bounds.fMin = r.point();
    g->bounds.fMax = r.point();
    const auto ns = r.count(), nn = r.count(), nsolid = r.count(), nempty = r.count(),
               nb = r.count(16U * 1024U * 1024U), nv = r.count(1'048'576), np = r.count(),
               nt = r.count(), nplane = r.count();
    if (!r.ok || nempty < 43 || nempty + nsolid > 65536 || !box(g->bounds) ||
        g->textureScale <= 0) {
        return {};
    }
    r.array(g->surfaces, ns, 20, [&] {
        InteriorSurface s;
        s.flags = r.scalar<UInt8>();
        s.material = r.scalar<UInt8>();
        s.width = r.scalar<UInt8>();
        s.height = r.scalar<UInt8>();
        s.offsetX = r.scalar<UInt8>();
        s.offsetY = r.scalar<UInt8>();
        s.plane = r.scalar<UInt16>();
        s.firstVertex = r.scalar<UInt32>();
        s.pointIndex = r.scalar<UInt32>();
        s.vertexCount = r.scalar<UInt8>();
        s.pointCount = r.scalar<UInt8>();
        r.take(2);
        return s;
    });
    r.array(g->nodes, nn, 8, [&] {
        InteriorCollisionNode n;
        n.plane = r.scalar<UInt16>();
        n.front = r.scalar<Int16>();
        n.back = r.scalar<Int16>();
        r.take(2);
        return n;
    });
    std::vector<InteriorLeaf> solid, empty;
    r.array(solid, nsolid, 12, [&] {
        InteriorLeaf l;
        l.solid = true;
        l.surfaceIndex = r.scalar<UInt32>();
        l.planeIndex = r.scalar<UInt32>();
        l.surfaceCount = r.scalar<UInt16>();
        l.planeCount = r.scalar<UInt16>();
        return l;
    });
    r.array(empty, nempty, 44, [&] {
        InteriorLeaf l;
        const auto flags = r.scalar<UInt16>();
        l.flags = flags & 1U;
        l.pvsCount = flags >> 1;
        l.surfaceCount = r.scalar<UInt16>();
        l.pvsIndex = r.scalar<UInt32>();
        l.surfaceIndex = r.scalar<UInt32>();
        l.planeIndex = r.scalar<UInt32>();
        l.bounds.fMin = r.point();
        l.bounds.fMax = r.point();
        l.planeCount = r.scalar<UInt16>();
        r.take(2);
        return l;
    });
    if (!r.ok) {
        return {};
    }
    g->leaves.insert(g->leaves.end(), empty.begin(), empty.begin() + 43);
    g->leaves.insert(g->leaves.end(), solid.begin(), solid.end());
    g->leaves.insert(g->leaves.end(), empty.begin() + 43, empty.end());
    g->bits = copy(r.take(nb));
    r.array(g->vertices, nv, 4, [&] {
        const auto p = r.scalar<UInt16>(), t = r.scalar<UInt16>();
        return InteriorVertex{p, t};
    });
    r.array(g->points, np, 12, [&] {
        return r.point();
    });
    r.array(g->texturePoints, nt, 8, [&] {
        const auto u = r.real(), v = r.real();
        return Point2F{u, v};
    });
    r.array(g->planes, nplane, 16, [&] {
        const auto p = r.point();
        const auto d = r.real();
        return PlaneF{p.x, p.y, p.z, d};
    });
    g->highestMipLevel = r.scalar<Int32>();
    g->flags = r.scalar<UInt32>();
    if (!r.done() || g->highestMipLevel < 0 || g->highestMipLevel > 15 || !tree(*g) ||
        std::ranges::any_of(g->planes,
                            [](const auto& p) {
                                return std::abs(m_dot(p, p) - 1) > .0001f;
                            }) ||
        std::ranges::any_of(g->vertices, [&](auto v) {
            return v.point >= np || v.texture >= nt;
        })) {
        return {};
    }
    if (std::ranges::any_of(g->surfaces, [&](const auto& s) {
            return s.plane >= nplane || s.vertexCount < 3 || s.firstVertex > nv ||
                   s.vertexCount > nv - s.firstVertex ||
                   !g->bitIndices(s.pointIndex, s.pointCount, np);
        })) {
        return {};
    }
    std::size_t work{};
    if (std::ranges::any_of(g->leaves, [&](const auto& l) {
            work += l.surfaceCount + l.planeCount + l.pvsCount;
            return work > 4'194'304 || !g->bitIndices(l.surfaceIndex, l.surfaceCount, ns) ||
                   !g->bitIndices(l.planeIndex, l.planeCount, nplane) ||
                   (!l.solid && !g->bitIndices(l.pvsIndex, l.pvsCount, UInt32(g->leaves.size())));
        })) {
        return {};
    }
    g->expandBoundaryVertices = true;
    return g;
}

Point3F InteriorGeometry::boundaryPoint(Point3F p) const {
    if (!expandBoundaryVertices) {
        return p;
    }
    for (Int32 axis = 0; axis < 3; ++axis) {
        if (p[axis] == bounds.fMin[axis]) {
            p[axis] -= .0125f;
        }
        if (p[axis] == bounds.fMax[axis]) {
            p[axis] += .0125f;
        }
    }
    return p;
}

std::optional<InteriorCollision> InteriorGeometry::collision() const {
    if (!tree(*this)) {
        return {};
    }
    std::vector<InteriorCollisionSurface> faces;
    faces.reserve(surfaces.size());
    if (!std::ranges::all_of(surfaces, [&](const auto& s) {
            if (s.plane >= planes.size() || s.firstVertex > vertices.size() ||
                s.vertexCount > vertices.size() - s.firstVertex) {
                return false;
            }
            InteriorCollisionSurface f{
                s.plane, s.planeFront(), s.material == 255 ? -1 : Int32(s.material), {}};
            for (const auto& v : std::span(vertices).subspan(s.firstVertex, s.vertexCount)) {
                if (v.point >= points.size()) {
                    return false;
                }
                f.vertices.push_back(boundaryPoint(points[v.point]));
            }
            faces.push_back(std::move(f));
            return true;
        })) {
        return {};
    }
    std::vector<InteriorCollisionLeaf> converted;
    converted.reserve(leaves.size());
    for (const auto& l : leaves) {
        auto indices = bitIndices(l.surfaceIndex, l.surfaceCount, UInt32(surfaces.size()));
        if (!indices) {
            return {};
        }
        converted.push_back({l.solid, std::move(*indices)});
    }
    return InteriorCollision::prepare(
        planes, nodes, converted, faces, InteriorSurfacePolicy::Native);
}

std::unique_ptr<InteriorShape> InteriorShape::load(StreamIO& stream, UInt32 size) {
    auto bytes = asset_detail::file(stream, size);
    if (!bytes) {
        return {};
    }
    Reader outer{*bytes};
    Int32 version{};
    auto r = outer.object(makeFourCC('I', 'T', 'R', 's'), "ITRShape", version);
    if (!outer.done() || !r.ok || version != 3) {
        return {};
    }
    std::vector<std::array<UInt32, 3>> states;
    const auto ns = r.count(256);
    r.array(states, ns, 12, [&] {
        const auto a = r.scalar<UInt32>(), b = r.scalar<UInt32>(), c = r.scalar<UInt32>();
        return std::array{a, b, c};
    });
    std::vector<std::array<UInt32, 4>> lods;
    const auto nl = r.count(4096);
    r.array(lods, nl, 16, [&] {
        const auto a = r.scalar<UInt32>(), b = r.scalar<UInt32>(), c = r.scalar<UInt32>(),
                   d = r.scalar<UInt32>();
        return std::array{a, b, c, d};
    });
    std::vector<UInt32> files, names;
    const auto nf = r.count(65536);
    r.array(files, nf, 4, [&] {
        return r.scalar<UInt32>();
    });
    const auto nn = r.count(256);
    r.array(names, nn, 4, [&] {
        return r.scalar<UInt32>();
    });
    const auto bytesCount = r.count(1'048'576);
    const auto buffer = r.take(bytesCount);
    const auto material = r.scalar<UInt32>();
    const auto linked = r.scalar<UInt8>();
    if (!r.done() || !ns || !nl || !nn || linked > 1) {
        return {};
    }
    auto shape = std::make_unique<InteriorShape>();
    auto text = asset_detail::name(buffer, material);
    if (!text || text->empty()) {
        return {};
    }
    shape->materialList = std::move(*text);
    shape->linked = linked != 0;
    for (const auto& s : states) {
        text = asset_detail::name(buffer, s[0]);
        if (!text || !s[2] || s[1] > nl || s[2] > nl - s[1]) {
            return {};
        }
        shape->states.push_back({*text, s[1], s[2]});
    }
    for (const auto& l : lods) {
        text = asset_detail::name(buffer, l[1]);
        if (!text || text->empty() || l[2] > nf || nn > nf - l[2]) {
            return {};
        }
        shape->lods.push_back({l[0], l[2], l[3], *text});
    }
    for (auto offset : files) {
        text = asset_detail::name(buffer, offset);
        if (!text || text->empty()) {
            return {};
        }
        shape->lightFiles.push_back(*text);
    }
    for (auto offset : names) {
        text = asset_detail::name(buffer, offset);
        if (!text) {
            return {};
        }
        shape->lightStateNames.push_back(*text);
    }
    return shape;
}

std::vector<std::string> InteriorShape::dependencies() const {
    std::vector<std::string> result{materialList};
    std::ranges::transform(lods, std::back_inserter(result), [](const auto& l) {
        return l.geometry;
    });
    result.insert(result.end(), lightFiles.begin(), lightFiles.end());
    return result;
}

std::unique_ptr<InteriorLighting> InteriorLighting::load(StreamIO& stream, UInt32 size) {
    auto bytes = asset_detail::file(stream, size);
    if (!bytes) {
        return {};
    }
    Reader outer{*bytes};
    Int32 version{};
    auto r = outer.object(0, "ITRLighting", version);
    bool mission = false;
    if (!r.ok) {
        outer = Reader{*bytes};
        r = outer.object(0, "ITRMissionLighting", version);
        mission = true;
    }
    if (!outer.done() || !r.ok || (version != 6 && version != 7)) {
        return {};
    }
    auto l = std::make_unique<InteriorLighting>();
    l->mission = mission;
    l->buildId = r.scalar<Int32>();
    l->scaleShift = r.scalar<Int32>();
    l->mapSlots = r.scalar<Int32>();
    const auto ns = r.count(), nd = r.count(), nl = r.count(), nf = r.count(),
               nm = r.count(32U * 1024U * 1024U);
    r.array(l->states, ns, 16, [&] {
        InteriorLightState s;
        for (auto& c : s.color) {
            c = r.scalar<UInt16>();
        }
        s.time = r.real();
        s.dataCount = r.scalar<Int16>();
        s.dataIndex = r.scalar<Int16>();
        return s;
    });
    r.array(l->stateData, nd, 8, [&] {
        InteriorLightStateData s;
        s.surface = r.scalar<Int16>();
        s.slot = r.scalar<Int16>();
        s.map = r.scalar<Int32>();
        return s;
    });
    r.array(l->lights, nl, 24, [&] {
        InteriorLightAnimation a;
        a.id = r.scalar<Int32>();
        a.name = r.scalar<Int32>();
        a.stateCount = r.scalar<Int32>();
        a.firstState = r.scalar<Int32>();
        a.duration = r.real();
        a.flags = r.scalar<UInt32>();
        return a;
    });
    r.array(l->surfaces, nf, 12, [&] {
        InteriorLightSurface s;
        s.map = r.scalar<UInt32>();
        s.lightCount = r.scalar<Int16>();
        s.lightIndex = r.scalar<Int16>();
        s.width = r.scalar<UInt8>();
        s.height = r.scalar<UInt8>();
        s.offsetX = r.scalar<UInt8>();
        s.offsetY = r.scalar<UInt8>();
        return s;
    });
    l->mapData = copy(r.take(nm));
    const auto nameBytes = r.count(1'048'576);
    l->names = copy(r.take(nameBytes));
    const auto compressed = r.scalar<UInt8>();
    if (compressed == 1) {
        const auto nodes = r.count(), leaves = r.count();
        r.array(l->huffmanNodes, nodes, 8, [&] {
            const auto one = r.scalar<Int32>(), zero = r.scalar<Int32>();
            return std::array{one, zero};
        });
        r.array(l->huffmanLeaves, leaves, 4, [&] {
            return r.scalar<UInt32>();
        });
        if (!nodes || !leaves) {
            return {};
        }
        std::vector<bool> seen(nodes);
        const std::function<bool(Int32, UInt32)> visit = [&](Int32 n, UInt32 depth) {
            if (n < 0) {
                return UInt64(-Int64(n) - 1) < leaves;
            }
            if (UInt32(n) >= nodes || depth > 64 || seen[std::size_t(n)]) {
                return false;
            }
            seen[std::size_t(n)] = true;
            return visit(l->huffmanNodes[std::size_t(n)][0], depth + 1) &&
                   visit(l->huffmanNodes[std::size_t(n)][1], depth + 1);
        };
        if (!visit(0, 0) || std::ranges::any_of(l->huffmanLeaves, [](auto color) {
                return color > 65535;
            })) {
            return {};
        }
    }
    if (mission && r.ok) {
        const auto count = r.count();
        std::vector<std::byte> decoded(std::size_t(count) * 8);
        MemRWStream parent(r.bytes.subspan(r.position));
        LZHRStream decoder(parent);
        if (!decoder.readBytes(decoded)) {
            return {};
        }
        r.take(std::size_t(parent.getPosition()));
        // Legacy LZHWStream::detach emits a zero terminator even when the
        // mission has no index pairs. No decoder read consumes it in that case.
        if (count == 0 && r.position + 1 == r.bytes.size() && r.bytes[r.position] == std::byte{}) {
            r.take(1);
        }
        Reader indices{decoded};
        indices.array(l->missionEntries, count, 8, [&] {
            const auto a = indices.scalar<Int32>(), b = indices.scalar<Int32>();
            return std::array{a, b};
        });
        if (!indices.done()) {
            return {};
        }
        Int32 previous = -1;
        for (const auto& entry : l->missionEntries) {
            if (entry[0] <= previous || entry[1] < 0 ||
                std::size_t(entry[1]) >= l->mapData.size()) {
                return {};
            }
            previous = entry[0];
        }
    }
    if (!r.done() || compressed > 1 || l->scaleShift < 0 || l->scaleShift > 15 || l->mapSlots < 0 ||
        l->mapSlots > 65536) {
        return {};
    }
    if (std::ranges::any_of(l->surfaces, [&](const auto& s) {
            return s.lightCount < 0 || s.lightIndex < 0 ||
                   Int32(s.lightIndex) + s.lightCount > l->mapSlots ||
                   (!(s.map & 0x80000000U) && (s.map & 0x3fffffffU) >= l->mapData.size()) ||
                   (!(s.map & 0x80000000U) &&
                    (!s.width || !s.height || ((s.map & 0x40000000U) && !compressed)));
        })) {
        return {};
    }
    return l->validAnimation() ? std::move(l) : nullptr;
}

std::optional<std::vector<UInt16>>
InteriorLighting::lightmap(std::size_t surface,
                           const InteriorLighting* override,
                           std::span<const InteriorLightSlot> slots) const {
    if (surface >= surfaces.size()) {
        return {};
    }
    const auto& s = surfaces[surface];
    const auto count = std::size_t(s.width) * s.height;
    if (override && (!override->mission || override->buildId != buildId)) {
        return {};
    }
    const InteriorLighting* source = this;
    auto offset = std::size_t(s.map & 0x3fffffffU);
    if (override && !(s.map & 0x80000000U)) {
        const auto found =
            std::ranges::lower_bound(override->missionEntries, Int32(offset), {}, [](auto entry) {
                return entry[0];
            });
        if (found != override->missionEntries.end() && (*found)[0] == Int32(offset)) {
            if ((*found)[1] < 0) {
                return {};
            }
            offset = std::size_t((*found)[1]);
            source = override;
        }
    }
    std::vector<UInt16> colors(std::max(std::size_t{1}, count), UInt16(s.map));
    if (!(s.map & 0x80000000U)) {
        if (!count || offset >= source->mapData.size()) {
            return {};
        }
        Reader r{std::span(source->mapData).subspan(offset)};
        if (!(s.map & 0x40000000U)) {
            if (count > r.bytes.size() / 2) {
                return {};
            }
            std::ranges::for_each(colors, [&](auto& color) {
                color = r.scalar<UInt16>();
            });
        } else {
            std::size_t bit{};
            bool ok = true;
            std::ranges::for_each(colors, [&](auto& color) {
                Int32 node = 0;
                UInt32 depth{};
                while (node >= 0 && ok) {
                    if (std::size_t(node) >= source->huffmanNodes.size() || ++depth > 64 ||
                        bit / 8 >= r.bytes.size()) {
                        ok = false;
                        break;
                    }
                    const auto one = (std::to_integer<UInt8>(r.bytes[bit / 8]) >> (bit % 8)) & 1U;
                    ++bit;
                    node = source->huffmanNodes[std::size_t(node)][one ? 0 : 1];
                }
                if (ok && UInt64(-Int64(node) - 1) < source->huffmanLeaves.size()) {
                    color = UInt16(source->huffmanLeaves[std::size_t(-Int64(node) - 1)]);
                } else {
                    ok = false;
                }
            });
            if (!ok) {
                return {};
            }
        }
    }
    if (!slots.empty()) {
        if (s.lightIndex < 0 || s.lightCount < 0 ||
            std::size_t(s.lightIndex) + s.lightCount > slots.size() ||
            UInt64(s.lightCount) * colors.size() > 4'194'304) {
            return {};
        }
        for (const auto& slot :
             slots.subspan(std::size_t(s.lightIndex), std::size_t(s.lightCount))) {
            if (slot.map < 0) {
                continue;
            }
            if (std::size_t(slot.map) > mapData.size() ||
                colors.size() > mapData.size() - std::size_t(slot.map)) {
                return {};
            }
            for (std::size_t i = 0; i < colors.size(); ++i) {
                const auto intensity = std::to_integer<UInt8>(mapData[std::size_t(slot.map) + i]);
                if (intensity < 16) {
                    continue;
                }
                UInt16 result{};
                for (UInt32 channel = 0; channel < 4; ++channel) {
                    const auto shift = channel == 3 ? 12U : 8U - channel * 4U;
                    const auto add = (UInt32(slot.color[channel]) * intensity) >> 12;
                    result |=
                        UInt16(std::min(15U, UInt32((colors[i] >> shift) & 15U) + add) << shift);
                }
                colors[i] = result;
            }
        }
    }
    return colors;
}

bool registerInteriorLoaders(ResourceManager& manager) {
    const auto shape = manager.registerLoader<InteriorShape>(".dis", InteriorShape::load);
    const auto geometry = manager.registerLoader<InteriorGeometry>(".dig", InteriorGeometry::load);
    const auto lighting = manager.registerLoader<InteriorLighting>(".dil", InteriorLighting::load);
    return shape && geometry && lighting;
}

std::optional<InteriorShapeResources> InteriorShapeResources::load(ResourceManager& manager,
                                                                   std::string_view name) {
    if (name.size() > 4096) {
        return {};
    } // preserve ResourceManager's virtual-name bound before deriving a fallback
    const auto base = baseShapeName(name);
    const bool mission = base != name && manager.contains(name);
    // A present but malformed mission DIS must fail; only absent files fall back.
    InteriorShapeResources result;
    result.shape = manager.load<InteriorShape>(mission ? name : std::string_view(base));
    if (!result.shape) {
        return {};
    }
    const auto list = manager.load<TsMaterialList>(result.shape->materialList);
    if (!list) {
        return {};
    }
    auto materials = TsMaterialResources::load(manager, *list);
    if (!materials) {
        return {};
    }
    result.materials = std::move(*materials);
    std::size_t work{};
    for (const auto& lod : result.shape->lods) {
        auto g = manager.load<InteriorGeometry>(lod.geometry);
        if (!g || g->surfaces.size() > 1'048'576 - work ||
            std::ranges::any_of(g->surfaces, [&](auto s) {
                return s.material != 255 && s.material >= result.materials.list.materialsPerDetail;
            })) {
            return {};
        }
        work += g->surfaces.size();
        result.geometry.push_back(std::move(g));
    }
    std::vector<Resource<InteriorLighting>> candidates;
    if (mission) {
        candidates.resize(result.shape->lightFiles.size());
    }
    for (std::size_t i = 0; i < result.shape->lightFiles.size(); ++i) {
        const auto& file = result.shape->lightFiles[i];
        auto l = manager.load<InteriorLighting>(baseLightName(file));
        if (!l || l->mission) {
            return {};
        }
        result.lighting.push_back(std::move(l));
        if (mission) {
            candidates[i] = manager.load<InteriorLighting>(file);
        }
    }
    for (std::size_t i = 0; i < result.geometry.size(); ++i) {
        const auto& lod = result.shape->lods[i];
        const auto& g = *result.geometry[i];
        if (lod.firstLightState > result.lighting.size() ||
            result.shape->lightStateNames.size() > result.lighting.size() - lod.firstLightState) {
            return {};
        }
        for (std::size_t j = 0; j < result.shape->lightStateNames.size(); ++j) {
            const auto& l = *result.lighting[lod.firstLightState + j];
            if (l.buildId != g.buildId || l.surfaces.size() != g.surfaces.size()) {
                return {};
            }
        }
    }
    if (mission) {
        // Native ITRInstance drops every LOD override when any LOD fails for
        // the selected light state. Apply that policy independently per state.
        const auto states = result.shape->lightStateNames.size();
        result.missionLighting.resize(result.geometry.size() * states);
        for (std::size_t state = 0; state < states; ++state) {
            const auto valid = std::ranges::all_of(
                std::views::iota(std::size_t{}, result.geometry.size()), [&](auto lod) {
                    const auto& override =
                        candidates[result.shape->lods[lod].firstLightState + state];
                    return override && override->mission &&
                           override->buildId == result.geometry[lod]->buildId;
                });
            if (valid) {
                for (std::size_t lod = 0; lod < result.geometry.size(); ++lod) {
                    result.missionLighting[lod * states + state] =
                        candidates[result.shape->lods[lod].firstLightState + state];
                }
            }
        }
    }
    return result;
}
} // namespace opent1::world
