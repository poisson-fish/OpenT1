#include <opent1/world/ts_shape.hpp>
#include <algorithm>
#include <bit>
#include <cmath>
#include <ranges>
#include <opent1/core/mem_stream.hpp>

namespace opent1::world {
namespace {
struct Reader {
    std::span<const std::byte> bytes;
    std::size_t position{};
    bool ok{true};

    std::size_t remaining() const {
        return bytes.size() - position;
    }

    std::span<const std::byte> take(std::size_t count) {
        if (!ok || count > remaining()) {
            ok = false;
            return {};
        }
        auto result = bytes.subspan(position, count);
        position += count;
        return result;
    }

    template <class T> T scalar() {
        auto raw = take(sizeof(T));
        if (!ok) {
            return {};
        }
        UInt32 value{};
        std::ranges::for_each(std::views::iota(std::size_t{0}, sizeof(T)), [&](auto i) {
            value |= UInt32(std::to_integer<UInt8>(raw[i])) << (i * 8);
        });
        if constexpr (sizeof(T) == 4) {
            return std::bit_cast<T>(value);
        } else if constexpr (sizeof(T) == 2) {
            return std::bit_cast<T>(UInt16(value));
        } else {
            return std::bit_cast<T>(UInt8(value));
        }
    }

    Int32 integer() {
        return scalar<Int32>();
    }

    Flt32 real() {
        const auto value = scalar<Flt32>();
        if (!std::isfinite(value)) {
            ok = false;
        }
        return value;
    }

    Point3F point() {
        const auto x = real(), y = real(), z = real();
        return {x, y, z};
    }

    std::string text(std::size_t count, bool terminated) {
        const auto raw = take(count);
        const auto end = std::ranges::find(raw, std::byte{});
        if (terminated && end == raw.end()) {
            ok = false;
        }
        std::string result;
        std::ranges::transform(raw.begin(), end, std::back_inserter(result), [](std::byte b) {
            return char(std::to_integer<UInt8>(b));
        });
        return result;
    }

    // Array counts are checked against both file bytes and an allocation bound.
    template <class T, class Fn>
    void array(std::vector<T>& out, Int32 count, std::size_t recordSize, Fn fn) {
        if (!ok || count < 0 || count > 1'048'576 ||
            std::size_t(count) > remaining() / recordSize) {
            ok = false;
            return;
        }
        out.resize(std::size_t(count));
        std::ranges::for_each(out, [&](T& value) {
            if (ok) {
                fn(value);
            }
        });
    }

    Reader block(std::string_view expected, Int32& version) {
        const auto tag = scalar<UInt32>(), flags = scalar<UInt32>();
        const auto count = flags & 0x7fffffffU;
        auto raw = take(count);
        const std::size_t mask = (flags & 0x80000000U) ? 3 : 1;
        take(((std::size_t(count) + mask) & ~mask) - count);
        Reader child{raw};
        const auto length = child.scalar<Int16>();
        if (tag != makeFourCC('P', 'E', 'R', 'S') || length <= 0 || length > 128 || !ok) {
            child.ok = false;
        }
        const auto name = child.text(child.ok ? std::size_t(length) : 0, false);
        if (length > 0 && (length & 1)) {
            child.take(1);
        }
        version = child.integer();
        if (name != expected) {
            child.ok = false;
        }
        return child;
    }

    bool done() const {
        return ok && remaining() == 0;
    }
};

std::optional<std::vector<std::byte>> readFile(StreamIO& stream, UInt32 size) {
    if (size < 8 || size > MaxTsFileBytes) {
        return {};
    }
    std::vector<std::byte> bytes(size);
    if (!stream.readBytes(bytes)) {
        return {};
    }
    return bytes;
}

bool index(Int32 value, std::size_t size) {
    return value >= 0 && std::size_t(value) < size;
}

bool range(Int32 first, Int32 count, std::size_t size) {
    return count >= 0 && (count == 0 || (first >= 0 && std::size_t(first) <= size &&
                                         std::size_t(count) <= size - std::size_t(first)));
}

struct Writer {
    std::vector<std::byte> bytes;
    bool ok{true};

    template <class T> void put(T value) {
        if (!ok || bytes.size() > MaxTsFileBytes - sizeof(T)) {
            ok = false;
            return;
        }
        using U = std::conditional_t<sizeof(T) == 4,
                                     UInt32,
                                     std::conditional_t<sizeof(T) == 2, UInt16, UInt8>>;
        const auto bits = std::bit_cast<U>(value);
        for (std::size_t i = 0; i < sizeof(T); ++i) {
            bytes.push_back(static_cast<std::byte>((bits >> (8 * i)) & 255));
        }
    }

    void integer(Int32 value) {
        put(value);
    }

    void shortInteger(Int32 value) {
        if (value < -32768 || value > 32767) {
            ok = false;
            return;
        }
        put(static_cast<Int16>(value));
    }

    void count(std::size_t value) {
        if (value > 1'048'576) {
            ok = false;
            return;
        }
        integer(static_cast<Int32>(value));
    }

    void point(const Point3F& p) {
        put(p.x);
        put(p.y);
        put(p.z);
    }

    void text(std::string_view value, std::size_t count) {
        if (value.size() >= count || value.find('\0') != value.npos) {
            ok = false;
            return;
        }
        for (char c : value) {
            put(static_cast<UInt8>(c));
        }
        for (std::size_t i = value.size(); i < count; ++i) {
            put(UInt8{0});
        }
    }

    void append(const Writer& source) {
        if (!ok || !source.ok || source.bytes.size() > MaxTsFileBytes - bytes.size()) {
            ok = false;
            return;
        }
        bytes.insert(bytes.end(), source.bytes.begin(), source.bytes.end());
    }

    Writer persistent(std::string_view name, Int32 version) const {
        Writer body;
        body.shortInteger(static_cast<Int32>(name.size()));
        for (char c : name) {
            body.put(static_cast<UInt8>(c));
        }
        if (name.size() & 1) {
            body.put(UInt8{0});
        }
        body.integer(version);
        body.append(*this);
        Writer result;
        result.put(makeFourCC('P', 'E', 'R', 'S'));
        result.put(static_cast<UInt32>(body.bytes.size()));
        result.append(body);
        if (body.bytes.size() & 1) {
            result.put(UInt8{0});
        }
        return result;
    }

    void transform(const TsTransform& transform) {
        for (float value : {transform.rotation.x,
                            transform.rotation.y,
                            transform.rotation.z,
                            transform.rotation.w}) {
            if (!std::isfinite(value) || value < -32768.f / 32767.f || value > 1) {
                ok = false;
                return;
            }
            put(static_cast<Int16>(std::lround(value * 32767)));
        }
        point(transform.translation);
    }
};

Writer writeMaterials(const TsMaterialList& list) {
    Writer body;
    body.integer(list.details);
    body.integer(list.materialsPerDetail);
    if (list.details < 1 || list.materialsPerDetail < 0 ||
        Int64(list.details) * list.materialsPerDetail !=
            static_cast<Int64>(list.materials.size())) {
        body.ok = false;
    }
    for (const auto& m : list.materials) {
        body.put(m.flags);
        body.put(m.alpha);
        body.integer(m.index);
        for (auto component : m.rgb) {
            body.put(component);
        }
        body.text(m.mapFile, 32);
        body.integer(m.surfaceType);
        body.put(m.elasticity);
        body.put(m.friction);
        body.put(UInt32(m.useDefaultProperties));
    }
    return body.persistent("TS::MaterialList", 4);
}

Writer writeMesh(const TsMesh& mesh) {
    Writer body;
    body.count(mesh.vertices.size());
    body.integer(mesh.verticesPerFrame);
    body.count(mesh.textureVertices.size());
    body.count(mesh.faces.size());
    body.count(mesh.frames.size());
    body.integer(mesh.textureVerticesPerFrame);
    body.put(mesh.radius);
    for (const auto& v : mesh.vertices) {
        body.put(v.x);
        body.put(v.y);
        body.put(v.z);
        body.put(v.normal);
    }
    for (const auto& v : mesh.textureVertices) {
        body.put(v.x);
        body.put(v.y);
    }
    for (const auto& face : mesh.faces) {
        for (const auto& corner : face.corners) {
            body.integer(corner.vertex);
            body.integer(corner.texture);
        }
        body.integer(face.material);
    }
    for (const auto& frame : mesh.frames) {
        body.integer(frame.firstVertex);
        body.point(frame.scale);
        body.point(frame.origin);
    }
    return body.persistent("TS::CelAnimMesh", 3);
}

TsTransform transform(Reader& r, Int32 version) {
    TsTransform result;
    if (version < 7) {
        // Original V6Transform upgrade first packs each float into Quat16.
        const auto component = [&] {
            const auto scaled = r.real() * 32767;
            if (!r.ok || scaled < -32768 || scaled > 32767) {
                r.ok = false;
                return 0.f;
            }
            return Flt32(static_cast<Int16>(scaled)) / 32767;
        };
        const auto x = component(), y = component(), z = component(), w = component();
        result.rotation = {x, y, z, w};
    } else {
        const auto x = r.scalar<Int16>(), y = r.scalar<Int16>(), z = r.scalar<Int16>(),
                   w = r.scalar<Int16>();
        result.rotation = {Flt32(x) / 32767, Flt32(y) / 32767, Flt32(z) / 32767, Flt32(w) / 32767};
    }
    result.translation = r.point();
    if (version < 8) {
        r.point();
    } // retired scale, deliberately discarded by original v8 upgrade
    return result;
}

std::optional<TsMaterialList> materialList(Reader& parent) {
    TsMaterialList result;
    auto r = parent.block("TS::MaterialList", result.version);
    if (!r.ok || result.version < 1 || result.version > 4) {
        return {};
    }
    result.details = r.integer();
    result.materialsPerDetail = r.integer();
    if (result.details < 1 || result.details > 1024 || result.materialsPerDetail < 0 ||
        result.materialsPerDetail > 65536 ||
        Int64(result.details) * result.materialsPerDetail > 65536) {
        return {};
    }
    // The active original v1 reader consumes 64-(32-16)=48 bytes,
    // including the later 32-byte filename; physics stays at constructor defaults.
    const auto size = result.version <= 2 ? 48U : result.version == 3 ? 60U : 64U;
    r.array(result.materials, result.details * result.materialsPerDetail, size, [&](TsMaterial& m) {
        m.flags = r.scalar<UInt32>();
        m.alpha = r.real();
        m.index = r.integer();
        std::ranges::for_each(m.rgb, [&](auto& c) {
            c = r.scalar<UInt8>();
        });
        m.mapFile = r.text(32, true);
        if (result.version >= 3) {
            m.surfaceType = r.integer();
            m.elasticity = r.real();
            m.friction = r.real();
        }
        if (result.version >= 4) {
            m.useDefaultProperties = r.scalar<UInt32>() != 0;
        }
        if (m.type() > 3 || m.alpha < 0 || m.alpha > 1 ||
            (m.type() == 1 && (m.index < 0 || m.index > 255)) ||
            (m.type() == 3 && m.mapFile.empty())) {
            r.ok = false;
        }
    });
    if (!r.done()) {
        return {};
    }
    return result;
}

std::optional<TsMesh> mesh(Reader& parent) {
    TsMesh result;
    auto r = parent.block("TS::CelAnimMesh", result.version);
    if (!r.ok || result.version < 1 || result.version > 3) {
        return {};
    }
    const auto vertexCount = r.integer();
    result.verticesPerFrame = r.integer();
    const auto textureCount = r.integer(), faceCount = r.integer(), frameCount = r.integer();
    result.textureVerticesPerFrame = result.version >= 2 ? r.integer() : textureCount;
    Point3F oldScale, oldOrigin;
    if (result.version < 3) {
        oldScale = r.point();
        oldOrigin = r.point();
    }
    result.radius = r.real();
    r.array(result.vertices, vertexCount, 4, [&](TsPackedVertex& v) {
        v = {r.scalar<UInt8>(), r.scalar<UInt8>(), r.scalar<UInt8>(), r.scalar<UInt8>()};
    });
    r.array(result.textureVertices, textureCount, 8, [&](Point2F& v) {
        const auto x = r.real(), y = r.real();
        v = {x, y};
    });
    r.array(result.faces, faceCount, 28, [&](TsFace& f) {
        std::ranges::for_each(f.corners, [&](auto& c) {
            c.vertex = r.integer();
            c.texture = r.integer();
        });
        f.material = r.integer();
    });
    r.array(result.frames, frameCount, result.version < 3 ? 4 : 28, [&](TsMeshFrame& f) {
        f.firstVertex = r.integer();
        f.scale = result.version < 3 ? oldScale : r.point();
        f.origin = result.version < 3 ? oldOrigin : r.point();
    });
    if (result.version < 3 && frameCount == 0) {
        result.frames.push_back({0, oldScale, oldOrigin});
    }
    // Exporters left the texture-frame count uninitialized on bounds-only meshes.
    // Preserve that word but never use it unless there are renderable faces.
    if (!r.done() || result.radius < 0 || result.verticesPerFrame < 0 ||
        (faceCount &&
         (result.textureVerticesPerFrame < 0 ||
          std::size_t(result.textureVerticesPerFrame) > result.textureVertices.size())) ||
        (faceCount && (result.frames.empty() || result.verticesPerFrame < 3))) {
        return {};
    }
    if (!std::ranges::all_of(
            result.frames,
            [&](const auto& f) {
                return range(f.firstVertex, result.verticesPerFrame, result.vertices.size()) &&
                       f.firstVertex >= 0;
            }) ||
        !std::ranges::all_of(result.faces, [&](const auto& f) {
            return f.material >= -1 && std::ranges::all_of(f.corners, [&](const auto& c) {
                       return index(c.vertex, std::size_t(result.verticesPerFrame)) &&
                              index(c.texture, result.textureVertices.size());
                   });
        })) {
        return {};
    }
    return result;
}

bool valid(const TsShape& s) {
    if (s.radius < 0 || s.bounds.fMin.x > s.bounds.fMax.x || s.bounds.fMin.y > s.bounds.fMax.y ||
        s.bounds.fMin.z > s.bounds.fMax.z || s.defaultMaterials < 0 ||
        (s.alwaysNode != -1 && !index(s.alwaysNode, s.nodes.size()))) {
        return false;
    }
    const auto named = [&](Int32 n) {
        return n == -1 || index(n, s.names.size());
    };
    if (!std::ranges::all_of(
            std::views::iota(std::size_t{0}, s.nodes.size()),
            [&](auto i) {
                const auto& n = s.nodes[i];
                return named(n.name) && (n.parent == -1 || index(n.parent, i)) &&
                       index(n.defaultTransform, s.transforms.size()) &&
                       range(n.firstSubSequence, n.subSequenceCount, s.subSequences.size());
            }) ||
        !std::ranges::all_of(
            s.objects,
            [&](const auto& o) {
                return named(o.name) && index(o.node, s.nodes.size()) &&
                       index(o.mesh, s.meshes.size()) &&
                       range(o.firstSubSequence, o.subSequenceCount, s.subSequences.size());
            }) ||
        !std::ranges::all_of(s.details,
                             [&](const auto& d) {
                                 return index(d.root, s.nodes.size());
                             }) ||
        !std::ranges::all_of(s.subSequences,
                             [&](const auto& sub) {
                                 return index(sub.sequence, s.sequences.size()) &&
                                        range(sub.firstKey, sub.keyCount, s.keyframes.size());
                             }) ||
        !std::ranges::all_of(
            s.sequences,
            [&](const auto& seq) {
                return named(seq.name) && seq.duration >= 0 &&
                       range(seq.firstTrigger, seq.triggerCount, s.triggers.size()) &&
                       range(seq.firstIfl, seq.iflCount, s.subSequences.size());
            }) ||
        !std::ranges::all_of(s.transitions, [&](const auto& t) {
            return index(t.startSequence, s.sequences.size()) &&
                   index(t.endSequence, s.sequences.size()) && t.duration >= 0;
        })) {
        return false;
    }
    if (s.materials && (!std::ranges::all_of(s.meshes, [&](const auto& m) {
            return std::ranges::all_of(m.faces, [&](const auto& f) {
                return f.material == -1 ||
                       index(f.material, std::size_t(s.materials->materialsPerDetail));
            });
        }) || s.defaultMaterials > s.materials->materialsPerDetail)) {
        return false;
    }
    return true;
}
} // namespace

std::unique_ptr<TsMaterialList> TsMaterialList::load(StreamIO& stream, UInt32 size) {
    auto bytes = readFile(stream, size);
    if (!bytes) {
        return {};
    }
    Reader r{*bytes};
    auto result = materialList(r);
    return result && r.done() ? std::make_unique<TsMaterialList>(std::move(*result)) : nullptr;
}

bool TsMaterialList::write(StreamIO& stream) const {
    const auto output = writeMaterials(*this);
    if (!output.ok) {
        return false;
    }
    MemRWStream validation{std::span<const std::byte>(output.bytes)};
    return load(validation, static_cast<UInt32>(output.bytes.size())) &&
           stream.writeBytes(output.bytes);
}

bool TsShape::write(StreamIO& stream) const {
    Writer body;
    for (auto count : {nodes.size(),
                       sequences.size(),
                       subSequences.size(),
                       keyframes.size(),
                       transforms.size(),
                       names.size(),
                       objects.size(),
                       details.size(),
                       meshes.size(),
                       transitions.size(),
                       triggers.size()}) {
        body.count(count);
    }
    body.put(radius);
    body.point(center);
    body.point(bounds.fMin);
    body.point(bounds.fMax);
    for (const auto& n : nodes) {
        for (auto value :
             {n.name, n.parent, n.subSequenceCount, n.firstSubSequence, n.defaultTransform}) {
            body.shortInteger(value);
        }
    }
    for (const auto& sequence : sequences) {
        body.integer(sequence.name);
        body.integer(sequence.cyclic);
        body.put(sequence.duration);
        body.integer(sequence.priority);
        body.integer(sequence.firstTrigger);
        body.integer(sequence.triggerCount);
        body.integer(sequence.iflCount);
        body.integer(sequence.firstIfl);
    }
    for (const auto& sub : subSequences) {
        body.shortInteger(sub.sequence);
        body.shortInteger(sub.keyCount);
        body.shortInteger(sub.firstKey);
    }
    for (const auto& key : keyframes) {
        body.put(key.position);
        body.put(key.value);
        body.put(key.materialFlags);
    }
    for (const auto& t : transforms) {
        body.transform(t);
    }
    for (const auto& name : names) {
        body.text(name, 24);
    }
    for (const auto& object : objects) {
        body.shortInteger(object.name);
        body.shortInteger(object.flags);
        body.integer(object.mesh);
        body.shortInteger(object.node);
        body.put(UInt16{0});
        body.point(object.offset);
        body.shortInteger(object.subSequenceCount);
        body.shortInteger(object.firstSubSequence);
    }
    for (const auto& detail : details) {
        body.integer(detail.root);
        body.put(detail.size);
    }
    for (const auto& transition : transitions) {
        body.integer(transition.startSequence);
        body.integer(transition.endSequence);
        body.put(transition.startPosition);
        body.put(transition.endPosition);
        body.put(transition.duration);
        body.transform(transition.transform);
    }
    for (const auto& trigger : triggers) {
        body.put(trigger.position);
        body.integer(trigger.value);
    }
    body.integer(defaultMaterials);
    body.integer(alwaysNode);
    for (const auto& mesh : meshes) {
        body.append(writeMesh(mesh));
    }
    body.integer(materials ? 1 : 0);
    if (materials) {
        body.append(writeMaterials(*materials));
    }
    const auto output = body.persistent("TS::Shape", 8);
    if (!output.ok) {
        return false;
    }
    MemRWStream validation{std::span<const std::byte>(output.bytes)};
    return load(validation, static_cast<UInt32>(output.bytes.size())) &&
           stream.writeBytes(output.bytes);
}

bool TsShape::importSequence(const TsShape& source, Int32 sequenceIndex) {
    if (!valid(*this) || !valid(source) || !index(sequenceIndex, source.sequences.size()) ||
        nodes.size() != source.nodes.size() || objects.size() != source.objects.size() ||
        details.size() != source.details.size() || meshes.size() != source.meshes.size() ||
        names.size() >= 32767 || sequences.size() >= 32767) {
        return false;
    }
    const auto sameName = [](std::string_view a, std::string_view b) {
        if (a.size() != b.size()) {
            return false;
        }
        const auto fold = [](unsigned char c) {
            return c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c;
        };
        return std::ranges::equal(a, b, {}, fold, fold);
    };
    const auto mapping =
        [&](const std::vector<Int32>& from,
            const std::vector<Int32>& to) -> std::optional<std::vector<std::size_t>> {
        std::vector<std::size_t> result;
        for (const auto name : from) {
            if (!index(name, source.names.size())) {
                return {};
            }
            std::size_t found = to.size();
            for (std::size_t j = 0; j < to.size(); ++j) {
                if (index(to[j], names.size()) && sameName(source.names[name], names[to[j]])) {
                    found = j;
                    break;
                }
            }
            if (found == to.size()) {
                return {};
            }
            result.push_back(found);
        }
        return result;
    };
    std::vector<Int32> sourceNodeNames, targetNodeNames, sourceObjectNames, targetObjectNames;
    for (const auto& n : source.nodes) {
        sourceNodeNames.push_back(n.name);
    }
    for (const auto& n : nodes) {
        targetNodeNames.push_back(n.name);
    }
    for (const auto& o : source.objects) {
        sourceObjectNames.push_back(o.name);
    }
    for (const auto& o : objects) {
        targetObjectNames.push_back(o.name);
    }
    const auto nodeMap = mapping(sourceNodeNames, targetNodeNames);
    const auto objectMap = mapping(sourceObjectNames, targetObjectNames);
    if (!nodeMap || !objectMap) {
        return false;
    }
    TsShape candidate = *this;
    const auto& originalSequence = source.sequences[sequenceIndex];
    if (!index(originalSequence.name, source.names.size())) {
        return false;
    }
    auto sequence = originalSequence;
    sequence.name = static_cast<Int32>(candidate.names.size());
    sequence.firstTrigger = static_cast<Int32>(candidate.triggers.size());
    sequence.iflCount = sequence.firstIfl = 0;
    const auto newSequence = static_cast<Int32>(candidate.sequences.size());
    candidate.names.push_back(source.names[originalSequence.name]);
    candidate.sequences.push_back(sequence);
    candidate.radius = std::max(radius, source.radius);
    candidate.bounds.fMin.setMin(source.bounds.fMin);
    candidate.bounds.fMax.setMax(source.bounds.fMax);
    const auto insertSub = [&](Int32 at, TsSubSequence sub, Int32& count) {
        if (at < 0 || static_cast<std::size_t>(at) > candidate.subSequences.size() ||
            candidate.subSequences.size() >= 32767 || count >= 32767) {
            return false;
        }
        candidate.subSequences.insert(candidate.subSequences.begin() + at, sub);
        for (auto& n : candidate.nodes) {
            if (n.subSequenceCount && n.firstSubSequence >= at) {
                ++n.firstSubSequence;
            }
        }
        for (auto& o : candidate.objects) {
            if (o.subSequenceCount && o.firstSubSequence >= at) {
                ++o.firstSubSequence;
            }
        }
        for (auto& s : candidate.sequences) {
            if (s.iflCount && s.firstIfl >= at) {
                ++s.firstIfl;
            }
        }
        ++count;
        return true;
    };
    const auto findSub = [&](const auto& item) -> const TsSubSequence* {
        const auto subs =
            std::span(source.subSequences).subspan(item.firstSubSequence, item.subSequenceCount);
        const auto found = std::ranges::find(subs, sequenceIndex, &TsSubSequence::sequence);
        return found == subs.end() ? nullptr : &*found;
    };
    std::vector<std::pair<const TsSubSequence*, std::size_t>> shared;
    for (std::size_t i = 0; i < source.nodes.size(); ++i) {
        const auto* sub = findSub(source.nodes[i]);
        if (!sub) {
            continue;
        }
        auto& target = candidate.nodes[(*nodeMap)[i]];
        const auto found =
            std::ranges::find(shared, sub, &std::pair<const TsSubSequence*, std::size_t>::first);
        if (found != shared.end()) {
            const auto& previous = candidate.nodes[found->second];
            if (target.subSequenceCount + 1 != previous.subSequenceCount) {
                return false;
            }
            if (++target.subSequenceCount == 1) {
                target.firstSubSequence = previous.firstSubSequence;
            }
            continue;
        }
        shared.emplace_back(sub, (*nodeMap)[i]);
        if (candidate.keyframes.size() + sub->keyCount > 32767 ||
            candidate.transforms.size() + sub->keyCount > 65536 ||
            !insertSub(target.firstSubSequence + target.subSequenceCount,
                       {newSequence, sub->keyCount, static_cast<Int32>(candidate.keyframes.size())},
                       target.subSequenceCount)) {
            return false;
        }
        for (Int32 k = 0; k < sub->keyCount; ++k) {
            const auto& key = source.keyframes[sub->firstKey + k];
            if (!index(key.value, source.transforms.size())) {
                return false;
            }
            candidate.keyframes.push_back(
                {key.position, static_cast<UInt16>(candidate.transforms.size()), 0x8000});
            candidate.transforms.push_back(source.transforms[key.value]);
        }
    }
    for (std::size_t i = 0; i < source.objects.size(); ++i) {
        const auto& object = source.objects[i];
        const auto* sub = findSub(object);
        if (!sub) {
            continue;
        }
        auto& target = candidate.objects[(*objectMap)[i]];
        // Native import uses the source mesh slot; node/object names may reorder.
        auto& targetMesh = candidate.meshes[object.mesh];
        const auto& sourceMesh = source.meshes[object.mesh];
        if (targetMesh.verticesPerFrame != sourceMesh.verticesPerFrame ||
            candidate.keyframes.size() + sub->keyCount > 32767 ||
            targetMesh.frames.size() + sub->keyCount > 65536 ||
            !insertSub(target.firstSubSequence + target.subSequenceCount,
                       {newSequence, sub->keyCount, static_cast<Int32>(candidate.keyframes.size())},
                       target.subSequenceCount)) {
            return false;
        }
        for (Int32 k = 0; k < sub->keyCount; ++k) {
            const auto& key = source.keyframes[sub->firstKey + k];
            if (!index(key.value, sourceMesh.frames.size())) {
                return false;
            }
            const auto& frame = sourceMesh.frames[key.value];
            if (!range(
                    frame.firstVertex, sourceMesh.verticesPerFrame, sourceMesh.vertices.size()) ||
                targetMesh.vertices.size() + sourceMesh.verticesPerFrame > 1'048'576) {
                return false;
            }
            auto newFrame = frame;
            newFrame.firstVertex = static_cast<Int32>(targetMesh.vertices.size());
            candidate.keyframes.push_back({key.position,
                                           static_cast<UInt16>(targetMesh.frames.size()),
                                           static_cast<UInt16>(key.materialFlags & 0xf000U)});
            targetMesh.frames.push_back(newFrame);
            const auto vertices = std::span(sourceMesh.vertices)
                                      .subspan(frame.firstVertex, sourceMesh.verticesPerFrame);
            targetMesh.vertices.insert(targetMesh.vertices.end(), vertices.begin(), vertices.end());
        }
    }
    // The original importer appends the source trigger table as a unit.
    candidate.triggers.insert(
        candidate.triggers.end(), source.triggers.begin(), source.triggers.end());
    if (!valid(candidate)) {
        return false;
    }
    *this = std::move(candidate);
    return true;
}

std::unique_ptr<TsShape> TsShape::load(StreamIO& stream, UInt32 size) {
    auto bytes = readFile(stream, size);
    if (!bytes) {
        return {};
    }
    Reader file{*bytes};
    auto result = std::make_unique<TsShape>();
    auto& s = *result;
    auto r = file.block("TS::Shape", s.version);
    // A few shipped player files retain stale authoring bytes after the root
    // PERS block. Original load() ignores that tail; the block itself is exact.
    if (!r.ok || s.version < 0 || s.version > 8 || !file.ok) {
        return {};
    }
    std::array<Int32, 11> counts{};
    for (std::size_t i = 0; i < 9; ++i) {
        counts[i] = r.integer();
    }
    if (s.version >= 2) {
        counts[9] = r.integer();
    }
    if (s.version >= 4) {
        counts[10] = r.integer();
    }
    s.radius = r.real();
    s.center = r.point();
    if (s.version >= 8) {
        s.bounds.fMin = r.point();
        s.bounds.fMax = r.point();
    }
    const auto old = s.version < 8;
    r.array(s.nodes, counts[0], old ? 20 : 10, [&](TsNode& n) {
        const auto word = [&] {
            return old ? r.integer() : Int32(r.scalar<Int16>());
        };
        n.name = word();
        n.parent = word();
        n.subSequenceCount = word();
        n.firstSubSequence = word();
        n.defaultTransform = word();
    });
    r.array(s.sequences,
            counts[1],
            s.version < 4   ? 16
            : s.version < 5 ? 24
                            : 32,
            [&](TsSequence& seq) {
                seq.name = r.integer();
                seq.cyclic = r.integer();
                seq.duration = r.real();
                seq.priority = r.integer();
                if (s.version >= 4) {
                    seq.firstTrigger = r.integer();
                    seq.triggerCount = r.integer();
                }
                if (s.version >= 5) {
                    seq.iflCount = r.integer();
                    seq.firstIfl = r.integer();
                }
            });
    r.array(s.subSequences, counts[2], old ? 12 : 6, [&](TsSubSequence& sub) {
        const auto word = [&] {
            return old ? r.integer() : Int32(r.scalar<Int16>());
        };
        sub.sequence = word();
        sub.keyCount = word();
        sub.firstKey = word();
    });
    r.array(s.keyframes,
            counts[3],
            s.version < 3 ? 8
            : old         ? 12
                          : 8,
            [&](TsKeyframe& k) {
                // Original marmor.dts has NaN positions in an authored animation track.
                // Keep metadata bit-for-bit; pose evaluation rejects nonfinite tracks
                // only when sampled, leaving default poses and other sequences usable.
                k.position = r.scalar<Flt32>();
                if (s.version < 3) {
                    const auto flags = r.scalar<UInt32>();
                    const auto value = flags & 0x3fffffffU;
                    if (value > 65535) {
                        r.ok = false;
                    }
                    k.value = static_cast<UInt16>(value);
                    k.materialFlags = UInt16(0x1000U | ((flags & 0x80000000U) ? 0x8000U : 0U) |
                                             ((flags & 0x40000000U) ? 0x4000U : 0U));
                } else if (old) {
                    const auto value = r.scalar<UInt32>(), flags = r.scalar<UInt32>();
                    if (value > 65535 || (flags & 0x0fffffffU) > 4095) {
                        r.ok = false;
                    }
                    k.value = UInt16(value);
                    k.materialFlags = UInt16((flags >> 16 & 0xf000U) | (flags & 0xfffU));
                } else {
                    k.value = r.scalar<UInt16>();
                    k.materialFlags = r.scalar<UInt16>();
                }
            });
    const auto transformSize = s.version < 7 ? 40 : old ? 32 : 20;
    r.array(s.transforms, counts[4], transformSize, [&](TsTransform& t) {
        t = transform(r, s.version);
    });
    r.array(s.names, counts[5], 24, [&](std::string& name) {
        name = r.text(24, true);
    });
    r.array(s.objects, counts[6], old ? 72 : 28, [&](TsObject& o) {
        o.name = r.scalar<Int16>();
        o.flags = r.scalar<Int16>();
        o.mesh = r.integer();
        if (old) {
            o.node = r.integer();
            r.take(40);
            o.offset = r.point();
        } else {
            o.node = r.scalar<Int16>();
            r.take(2);
            o.offset = r.point();
        }
        o.subSequenceCount = old ? r.integer() : Int32(r.scalar<Int16>());
        o.firstSubSequence = old ? r.integer() : Int32(r.scalar<Int16>());
    });
    r.array(s.details, counts[7], 8, [&](TsDetail& d) {
        d.root = r.integer();
        d.size = r.real();
    });
    r.array(s.transitions, counts[9], 20 + transformSize, [&](TsTransition& t) {
        t.startSequence = r.integer();
        t.endSequence = r.integer();
        t.startPosition = r.real();
        t.endPosition = r.real();
        t.duration = r.real();
        t.transform = transform(r, s.version);
    });
    r.array(s.triggers, counts[10], 8, [&](TsTrigger& t) {
        t.position = r.real();
        t.value = r.integer();
    });
    if (s.version >= 5) {
        s.defaultMaterials = r.integer();
    }
    if (s.version >= 6) {
        s.alwaysNode = r.integer();
    }
    r.array(s.meshes, counts[8], 8, [&](TsMesh& m) {
        auto value = mesh(r);
        if (value) {
            m = std::move(*value);
        } else {
            r.ok = false;
        }
    });
    const auto hasMaterials = r.integer();
    if (hasMaterials != 0 && hasMaterials != 1) {
        return {};
    }
    if (hasMaterials) {
        s.materials = materialList(r);
        if (!s.materials) {
            return {};
        }
    }
    if (old) {
        const auto boundsObject = std::ranges::find(s.objects, 0, &TsObject::node);
        if (boundsObject == s.objects.end() || !index(boundsObject->mesh, s.meshes.size())) {
            return {};
        }
        const auto& m = s.meshes[std::size_t(boundsObject->mesh)];
        const auto a = m.point(0), b = m.point(1);
        if (!a || !b) {
            return {};
        }
        s.bounds = {*a, *b};
    }
    if (!r.done() || !valid(s)) {
        return {};
    }
    return result;
}

std::optional<Point3F> TsMesh::point(Int32 vertex, Int32 frame) const {
    if (!index(frame, frames.size()) || vertex < 0 || vertex >= verticesPerFrame) {
        return {};
    }
    const auto& f = frames[std::size_t(frame)];
    const auto at = Int64(f.firstVertex) + vertex;
    if (at < 0 || UInt64(at) >= vertices.size()) {
        return {};
    }
    const auto& v = vertices[std::size_t(at)];
    Point3F p{Flt32(v.x) * f.scale.x + f.origin.x,
              Flt32(v.y) * f.scale.y + f.origin.y,
              Flt32(v.z) * f.scale.z + f.origin.z};
    if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z)) {
        return {};
    }
    return p;
}

std::vector<std::string> TsMaterialList::dependencies() const {
    std::vector<std::string> result;
    std::ranges::for_each(materials, [&](const auto& m) {
        if (m.type() == 3 && std::ranges::find(result, m.mapFile) == result.end()) {
            result.push_back(m.mapFile);
        }
    });
    return result;
}

std::vector<std::string> TsShape::dependencies() const {
    return materials ? materials->dependencies() : std::vector<std::string>{};
}

std::optional<Int32> selectTsDetail(const TsShape& shape, Flt32 projectedSize) {
    if (!std::isfinite(projectedSize) || shape.details.size() > 65536 ||
        !std::ranges::all_of(shape.details, [](const auto& d) {
            return std::isfinite(d.size);
        })) {
        return {};
    }
    if (shape.details.size() <= 1) {
        return 0;
    }
    const auto found = std::ranges::find_if(shape.details, [&](const auto& d) {
        return std::abs(projectedSize) >= d.size;
    });
    if (found == shape.details.end() || isZero(found->size)) {
        return -1;
    }
    return Int32(found - shape.details.begin());
}

bool registerTsLoaders(ResourceManager& manager) {
    const auto a = manager.registerLoader<TsShape>(".dts", TsShape::load);
    const auto b = manager.registerLoader<TsMaterialList>(".dml", TsMaterialList::load);
    if (a)
        manager.setSourceOnlyLoader(".dts");
    if (b)
        manager.setSourceOnlyLoader(".dml");
    return a && b;
}

std::optional<std::vector<Int32>>
classifyTsUtilityDetails(TsShape& shape, std::span<const std::string_view> prefixes) {
    if (shape.details.size() > 65536 || prefixes.size() > 1024 ||
        !std::ranges::all_of(prefixes, [](auto p) {
            return !p.empty() && p.size() <= 1024;
        })) {
        return {};
    }
    std::vector<Int32> order(shape.details.size());
    std::ranges::copy(std::views::iota(Int32{0}, Int32(order.size())), order.begin());
    if (prefixes.empty()) {
        return order;
    }
    std::vector<UInt8> utility(shape.details.size());
    std::size_t remaining = 4'194'304;
    if (!std::ranges::all_of(order, [&](auto i) {
            const auto& detail = shape.details[std::size_t(i)];
            if (!std::isfinite(detail.size) || !index(detail.root, shape.nodes.size())) {
                return false;
            }
            const auto name = shape.nodes[std::size_t(detail.root)].name;
            if (!index(name, shape.names.size())) {
                return false;
            }
            const auto& text = shape.names[std::size_t(name)];
            bool util = isZero(detail.size);
            if (!util) {
                bool exhausted = false;
                util = std::ranges::any_of(prefixes, [&](auto prefix) {
                    if (prefix.size() > remaining) {
                        exhausted = true;
                        return true;
                    }
                    remaining -= prefix.size();
                    return text.starts_with(prefix);
                });
                if (exhausted) {
                    return false;
                }
            }
            utility[std::size_t(i)] = UInt8(util);
            return true;
        })) {
        return {};
    }
    std::stable_partition(order.begin(), order.end(), [&](auto i) {
        return !utility[std::size_t(i)];
    });
    auto details = shape.details;
    std::ranges::transform(order, details.begin(), [&](auto i) {
        auto d = shape.details[std::size_t(i)];
        if (utility[std::size_t(i)]) {
            d.size = 0;
        }
        return d;
    });
    shape.details = std::move(details);
    return order;
}

std::optional<TsMaterialResources> TsMaterialResources::load(ResourceManager& manager,
                                                             const TsMaterialList& list) {
    Data result{list, {}};
    result.textures.resize(list.materials.size());
    const auto ok =
        std::ranges::all_of(std::views::iota(std::size_t{0}, list.materials.size()), [&](auto i) {
            if (list.materials[i].type() != 3) {
                return true;
            }
            result.textures[i] = manager.load<gfx::Bitmap>(list.materials[i].mapFile);
            return bool(result.textures[i]);
        });
    return ok ? std::optional(TsMaterialResources(std::move(result))) : std::nullopt;
}

std::optional<TsShapeResources> TsShapeResources::load(ResourceManager& manager,
                                                       std::string_view name) {
    auto shape = manager.load<TsShape>(name);
    if (!shape || !shape->materials) {
        return {};
    }
    auto materials = TsMaterialResources::load(manager, *shape->materials);
    if (!materials) {
        return {};
    }
    return TsShapeResources{std::move(shape), std::move(*materials)};
}

std::optional<TsMaterialResources> TsMaterialResources::loadSkin(ResourceManager& manager,
                                                                 const TsMaterialList& original,
                                                                 std::string_view skin,
                                                                 std::string_view shapeBase) {
    const auto base = std::string(shapeBase.substr(0, shapeBase.find_last_of('.')));
    const auto skinBase = std::string(skin.empty() ? "base" : skin);
    const auto prefix = skinBase + "." + base;
    if (manager.contains(prefix + ".dml")) {
        auto list = manager.load<TsMaterialList>(prefix + ".dml");
        if (list && list->materialsPerDetail == original.materialsPerDetail &&
            list->materials.size() == original.materials.size()) {
            if (auto loaded = load(manager, *list)) {
                return loaded;
            }
        }
    }
    Data result{original, {}};
    result.textures.resize(original.materials.size());
    for (std::size_t i = 0; i < original.materials.size(); ++i) {
        auto& material = result.list.materials[i];
        if (material.type() != 3) {
            continue;
        }
        // Legacy skins replace only base.* maps, preserving each slot's suffix.
        // Other textures and unavailable skin variants keep their authored map.
        if (material.mapFile.starts_with("base.")) {
            const auto name = skinBase + "." + material.mapFile.substr(5);
            result.textures[i] = manager.load<gfx::Bitmap>(name);
            if (result.textures[i]) {
                material.mapFile = name;
            }
        }
        if (!result.textures[i]) {
            result.textures[i] = manager.load<gfx::Bitmap>(material.mapFile);
        }
        if (!result.textures[i]) {
            return {};
        }
    }
    return TsMaterialResources(std::move(result));
}

std::optional<TsMaterialResources>
TsMaterialResources::withDamageSkin(ResourceManager& manager,
                                    TsMaterialResources sourceMaterials,
                                    Int32 damageLevel,
                                    std::span<const std::string> bitmapNames) {
    if (damageLevel < 0 || damageLevel > 3 ||
        sourceMaterials.textures().size() != sourceMaterials.list().materials.size()) {
        return {};
    }
    if (damageLevel == 0) {
        return sourceMaterials;
    }
    std::vector<Resource<gfx::Bitmap>> decals;
    for (const auto& baseName : bitmapNames) {
        if (baseName.empty()) {
            break;
        }
        auto name = baseName;
        if (!name.ends_with(".bmp") && !name.ends_with(".BMP")) {
            name += ".bmp";
        }
        auto bitmap = manager.load<gfx::Bitmap>(name);
        // Legacy DamageSkinData counts the contiguous successfully loaded prefix.
        if (!bitmap || !bitmap->valid() || bitmap->bitDepth != 8) {
            break;
        }
        decals.push_back(std::move(bitmap));
    }
    if (decals.empty()) {
        return sourceMaterials;
    }
    auto result = sourceMaterials.data();
    for (std::size_t slot = 0; slot < result.textures.size(); ++slot) {
        auto& material = result.list.materials[slot];
        if (material.type() != 3 || (material.flags & 0xf000U) != 0) {
            continue;
        }
        const auto source = result.textures[slot];
        if (!source || !source->valid()) {
            return {};
        }
        if (source->bitDepth != 8) {
            continue;
        }
        const auto name = "_d" + std::to_string(damageLevel) + "_" + material.mapFile;
        auto damaged = manager.load<gfx::Bitmap>(name);
        if (!damaged) {
            auto bitmap = std::make_unique<gfx::Bitmap>(*source);
            bitmap->palette.reset(); // legacy copy retains paletteIndex, not an embedded palette
            const UInt64 targetCoverage =
                (UInt64(bitmap->width) * UInt64(bitmap->height) * UInt64(damageLevel)) >> 2;
            UInt64 coverage = 0;
            UInt32 iteration = 1;
            std::size_t index = 0, skipped = 0;
            // Match ShapeBase::damageBitmap, including DWORD wraparound,
            // transparent palette index zero, and coverage by full decal area.
            while (coverage < targetCoverage && skipped < decals.size()) {
                const auto& decal = *decals[index];
                if (decal.width >= bitmap->width || decal.height >= bitmap->height) {
                    ++skipped;
                } else {
                    const auto x =
                        Int32((UInt32{30149123} * iteration) % UInt32(bitmap->width - decal.width));
                    const auto y = Int32((UInt32{3419238501} * iteration) %
                                         UInt32(bitmap->height - decal.height));
                    for (Int32 row = 0; row < decal.height; ++row) {
                        const auto src = decal.row(row);
                        auto dst = bitmap->row(y + row);
                        for (Int32 column = 0; column < decal.width; ++column) {
                            if (src[column] != std::byte{}) {
                                dst[x + column] = src[column];
                            }
                        }
                    }
                    ++iteration;
                    coverage += UInt64(decal.width) * UInt64(decal.height);
                }
                index = (index + 1) % decals.size();
            }
            if (!manager.add(name, std::move(bitmap))) {
                return {};
            }
            damaged = manager.load<gfx::Bitmap>(name);
            if (!damaged) {
                return {};
            }
        }
        material.mapFile = name;
        result.textures[slot] = std::move(damaged);
    }
    return TsMaterialResources(std::move(result));
}
} // namespace opent1::world
