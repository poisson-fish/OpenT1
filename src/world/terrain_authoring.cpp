#include <opent1/world/terrain_authoring.hpp>
#include <opent1/world/terrain_collision_snapshot.hpp>
#include <opent1/world/ts_lighting.hpp>
#include <opent1/core/file_stream.hpp>
#include <opent1/core/lzh_stream.hpp>
#include <opent1/core/mem_stream.hpp>
#include <opent1/core/vol_stream.hpp>
#include <algorithm>
#include <atomic>
#include <bit>
#include <cmath>
#include <map>
#include <set>
#include <cstdio>
#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace opent1::world {
namespace {
constexpr std::size_t MaxDocumentBytes = 256U * 1024U * 1024U;

struct Bytes {
    std::vector<std::byte> data;

    template <class T> void put(T value) {
        if constexpr (std::is_floating_point_v<T>) {
            put(std::bit_cast<UInt32>(value));
        } else {
            for (std::size_t i = 0; i < sizeof(T); ++i) {
                data.push_back(std::byte((std::make_unsigned_t<T>(value) >> (i * 8)) & 255));
            }
        }
    }

    void append(std::span<const std::byte> bytes) {
        data.insert(data.end(), bytes.begin(), bytes.end());
    }

    void text(std::string_view s) {
        for (auto c : s) {
            put(UInt8(c));
        }
    }

    void string(std::string_view s) {
        put(Int32(s.size()));
        text(s);
    }

    void block(UInt32 tag, std::span<const std::byte> body) {
        put(tag);
        put(UInt32(body.size()));
        append(body);
        if (body.size() & 1) {
            put(UInt8{});
        }
    }

    std::vector<std::byte> object(UInt32 tag, Int32 version) const {
        Bytes b, p;
        p.put(version);
        p.append(data);
        b.block(tag, p.data);
        return b.data;
    }
};

bool textValid(std::string_view s, std::size_t max = 255) {
    return s.size() <= max && s.find('\0') == s.npos;
}

std::optional<std::vector<std::byte>> compress(std::span<const std::byte> data) {
    // Literal-heavy LZH can expand. Fixed checked storage keeps failure bounded.
    if (data.size() > MaxTerrainFileBytes) {
        return {};
    }
    std::vector<std::byte> buffer(data.size() * 2 + 4096);
    MemRWStream stream{std::span<std::byte>(buffer)};
    LZHWStream encoder(stream);
    if (!encoder.writeBytes(data) || !encoder.flush()) {
        return {};
    }
    encoder.detach();
    if (stream.getStatus() != STRM_OK) {
        return {};
    }
    buffer.resize(std::size_t(stream.getPosition()));
    return buffer;
}

bool compressed(Bytes& out, const Bytes& raw) {
    auto packed = compress(raw.data);
    if (!packed) {
        return false;
    }
    out.put(UInt32(raw.data.size()));
    out.append(*packed);
    return true;
}

bool blockShape(const TerrainGridBlock& b) {
    if (!std::has_single_bit(b.width) || b.width > 256 ||
        b.detailCount != Int32(std::bit_width(b.width)) || !textValid(b.name, 15) ||
        b.heights.size() != (std::size_t(b.width) + 1) * (b.width + 1) ||
        b.materials.size() != std::size_t(b.width) * b.width || b.lightScale < -1 ||
        b.lightScale > 10 || !std::isfinite(b.minimumHeight) || !std::isfinite(b.maximumHeight) ||
        b.minimumHeight > b.maximumHeight || !std::ranges::all_of(b.heights, [](auto h) {
            return std::isfinite(h) && std::abs(h) <= 1e7f;
        })) {
        return false;
    }
    const auto lightWidth = b.lightScale < 0 ? 0 : (b.width << b.lightScale) + 1;
    if (lightWidth > 1025 || b.lights.size() != std::size_t(lightWidth) * lightWidth ||
        b.hiresDelta.size() > MaxTerrainFileBytes || b.hiresMaps.size() > 50000 ||
        b.colorPool.size() > 4'194'304 || b.indexTable.size() > 4'194'304 ||
        b.treeTable.size() > 1'398'101) {
        return false;
    }
    for (UInt32 i = 0; i < b.pins.size(); ++i) {
        const auto side = i ? b.width >> (i - 1) : b.width + 1;
        const auto size = side * side / 8 + 2 * side / 8 + 1;
        if (!b.pins[i].empty() && b.pins[i].size() != size) {
            return false;
        }
    }
    for (const auto& m : b.hiresMaps) {
        if (m.compression > 5) {
            return false;
        }
    }
    return true;
}

bool nameValid(std::string_view name) {
    return !name.empty() && textValid(name) && name.find_first_of("/\\:") == name.npos &&
           name != "." && name != "..";
}

std::string folded(std::string s) {
    for (auto& c : s) {
        if (c >= 'A' && c <= 'Z') {
            c = char(c + 32);
        }
    }
    return s;
}

std::size_t memory(const TerrainDocument& d) {
    std::size_t n = sizeof(d) + d.gridName.size() + d.grid.materialList.size() +
                    d.grid.blockMap.size() * 4 +
                    d.grid.blocks.size() * sizeof(TerrainGridBlockEntry);
    for (const auto& entry : d.grid.blocks) {
        n += entry.name.size();
    }
    for (const auto& b : d.blocks) {
        n += sizeof(b) + b.name.size() + b.heights.size() * 4 + b.materials.size() * 2 +
             b.lights.size() * 2 + b.hiresDelta.size() + b.hiresMaps.size() * 12 +
             b.colorPool.size() * 2 + b.indexTable.size() + b.treeTable.size() * 4;
        for (const auto& p : b.pins) {
            n += p.size();
        }
    }
    for (const auto& e : d.extras) {
        n += e.bytes.size() + e.name.size() + sizeof(e);
    }
    return n;
}
} // namespace

std::optional<std::vector<std::byte>> writeTerrainBlock(const TerrainGridBlock& b) {
    if (!blockShape(b)) {
        return {};
    }
    Bytes out, heights, materials, lights;
    out.text(b.name);
    out.data.resize(16);
    out.put(b.detailCount);
    out.put(b.lightScale);
    out.put(b.minimumHeight);
    out.put(b.maximumHeight);
    out.put(Int32(b.width));
    out.put(Int32(b.width));
    for (auto h : b.heights) {
        heights.put(h);
    }
    for (auto m : b.materials) {
        materials.put(m.flags);
        materials.put(m.index);
    }
    if (!compressed(out, heights) || !compressed(out, materials)) {
        return {};
    }
    for (const auto& pin : b.pins) {
        const auto empty = std::ranges::all_of(pin, [](auto x) {
            return x == std::byte{};
        });
        out.put(UInt16(empty ? 0 : pin.size()));
        if (!empty) {
            out.append(pin);
        }
    }
    if (b.lightScale >= 0) {
        for (auto l : b.lights) {
            lights.put(l);
        }
        if (!compressed(out, lights)) {
            return {};
        }
    }
    out.put(UInt32(b.hiresDelta.size()));
    out.append(b.hiresDelta);
    out.put(Int32(3));
    out.put(UInt32(b.hiresMaps.size()));
    out.put(UInt32(b.colorPool.size()));
    out.put(UInt32(b.indexTable.size()));
    out.put(UInt32(b.treeTable.size()));
    for (auto m : b.hiresMaps) {
        out.put(m.x);
        out.put(m.y);
        out.put(m.flags);
        out.put(m.compression);
        out.put(m.resolutions);
        out.put(m.colors);
        out.put(m.color);
    }
    for (auto c : b.colorPool) {
        out.put(c);
    }
    for (auto i : b.indexTable) {
        out.put(i);
    }
    for (auto n : b.treeTable) {
        out.put(n);
    }
    if (out.data.size() + 12 > MaxTerrainFileBytes) {
        return {};
    }
    return out.object(makeFourCC('G', 'B', 'L', 'K'), 5);
}

std::optional<std::vector<std::byte>> writeTerrainGrid(const TerrainGridFile& g) {
    if (!textValid(g.materialList) || !std::ranges::all_of(g.blocks, [](const auto& b) {
            return textValid(b.name);
        })) {
        return {};
    }
    Bytes out;
    out.string(g.materialList);
    out.put(g.lastBlockId);
    out.put(g.detailCount);
    out.put(g.scale);
    for (auto p : {g.bounds.fMin, g.bounds.fMax}) {
        out.put(p.x);
        out.put(p.y);
        out.put(p.z);
    }
    out.put(g.origin.x);
    out.put(g.origin.y);
    out.put(g.minimumHeight);
    out.put(g.maximumHeight);
    out.put(g.size.x);
    out.put(g.size.y);
    out.put(g.pattern);
    if (g.blockMap.size() > 4096 || g.blocks.size() > 4096) {
        return {};
    }
    for (auto id : g.blockMap) {
        out.put(id);
    }
    out.put(UInt32(g.blocks.size()));
    for (const auto& b : g.blocks) {
        out.put(b.id);
        out.string(b.name);
    }
    auto result = out.object(makeFourCC('G', 'F', 'I', 'L'), 1);
    MemRWStream check{std::span<const std::byte>(result)};
    if (!TerrainGridFile::load(check, UInt32(result.size()))) {
        return {};
    }
    return result;
}

std::optional<TerrainGridBlock> createTerrainBlock(UInt32 width, Int32 lightScale) {
    if (!std::has_single_bit(width) || width > 256) {
        return {};
    }
    TerrainGridBlock b;
    b.version = 5;
    b.hiresVersion = 3;
    b.width = width;
    b.detailCount = Int32(std::bit_width(width));
    b.heights.resize((std::size_t(width) + 1) * (width + 1));
    b.materials.resize(std::size_t(width) * width);
    if (!setTerrainLightScale(b, lightScale)) {
        return {};
    }
    return b;
}

bool rebuildTerrainHeightRange(TerrainGridBlock& b) {
    if (b.heights.empty() || !std::ranges::all_of(b.heights, [](auto h) {
            return std::isfinite(h) && std::abs(h) <= 1e7f;
        })) {
        return false;
    }
    const auto [lo, hi] = std::ranges::minmax_element(b.heights);
    b.minimumHeight = *lo;
    b.maximumHeight = *hi;
    return true;
}

bool setTerrainLightScale(TerrainGridBlock& b, Int32 scale) {
    if (!std::has_single_bit(b.width) || b.width > 1024 || scale < -1 || scale > 10 ||
        (scale >= 0 && (b.width << scale) > 1024)) {
        return false;
    }
    const auto side = scale < 0 ? 0 : (b.width << scale) + 1;
    std::vector<UInt16> lights(std::size_t(side) * side, 0xffff);
    b.lights = std::move(lights);
    b.lightScale = scale;
    b.hiresDelta.clear();
    return true;
}

bool setTerrainHiresDelta(TerrainGridBlock& b, std::span<const UInt16> samples) {
    if (b.lightScale < 0 || samples.size() != b.lights.size() || samples.empty()) {
        return false;
    }
    Bytes raw;
    for (std::size_t i = 0; i < samples.size(); ++i) {
        raw.put(UInt16(samples[i] ^ b.lights[i]));
    }
    auto packed = compress(raw.data);
    if (!packed) {
        return false;
    }
    b.hiresDelta = std::move(*packed);
    return true;
}

bool pinTerrainSquare(TerrainGridBlock& b, UInt32 detail, Point2I pos, UInt32 maxDetail, bool set) {
    if (!blockShape(b) || detail < 1 || detail >= UInt32(b.detailCount) || detail >= 10 ||
        maxDetail < detail || maxDetail > UInt32(b.detailCount) || pos.x < 0 || pos.y < 0 ||
        UInt32(pos.x) >= b.width >> detail || UInt32(pos.y) >= b.width >> detail) {
        return false;
    }
    auto pins = b.pins;
    const auto bit = [&](UInt32 map, UInt32 index) {
        const auto side = map ? b.width >> (map - 1) : b.width + 1;
        if (pins[map].empty()) {
            pins[map].resize(side * side / 8 + 2 * side / 8 + 1);
        }
        const auto mask = std::byte(1U << (index & 7));
        auto& value = pins[map][index / 8];
        if (set) {
            value |= mask;
        } else {
            value &= ~mask;
        }
    };
    const auto recurse = [&](auto&& self, UInt32 d, Point2I p) -> void {
        bit(d + 1, UInt32(p.y) * (b.width >> d) + UInt32(p.x));
        const auto x = UInt32(p.x) << d, y = UInt32(p.y) << d, step = 1U << (d - 1);
        for (UInt32 j = 0; j < 3; ++j) {
            for (UInt32 i = 0; i < 3; ++i) {
                bit(0, (y + j * step) * (b.width + 1) + x + i * step);
            }
        }
        if (++d < maxDetail && d < UInt32(b.detailCount) && d < 10) {
            Point2I parent{p.x >> 1, p.y >> 1};
            self(self, d, parent);
            const auto neighbor = [&](bool xAxis, Int32 delta) {
                auto q = parent;
                (xAxis ? q.x : q.y) += delta;
                if (q.x >= 0 && q.y >= 0 && UInt32(q.x) < b.width >> d &&
                    UInt32(q.y) < b.width >> d) {
                    self(self, d, q);
                }
            };
            neighbor(true, (p.x & 1) ? 1 : -1);
            neighbor(false, (p.y & 1) ? 1 : -1);
        }
    };
    recurse(recurse, detail, pos);
    b.pins = std::move(pins);
    return true;
}

bool setTerrainHiresMaps(TerrainGridBlock& b, std::span<const TerrainAuthoredLight> input) {
    if (!std::has_single_bit(b.width) || b.width > 1024 || input.size() > 50000) {
        return false;
    }
    std::vector<TerrainHiresLightMap> maps;
    std::vector<UInt16> pool;
    std::vector<UInt32> tree;
    std::map<std::pair<UInt32, UInt32>, UInt32> positions;
    for (const auto& h : input) {
        if (h.x < 0 || h.y < 0 || UInt32(h.x) >= b.width || UInt32(h.y) >= b.width ||
            !positions.emplace(std::pair{UInt32(h.x), UInt32(h.y)}, UInt32(maps.size())).second) {
            return false;
        }
        TerrainHiresLightMap m{h.x, h.y, h.flags, 5, 0, 0, UInt32(pool.size())};
        std::vector<UInt16> raw;
        for (UInt32 r = 0; r < 6; ++r) {
            if (h.resolutions[r].empty()) {
                continue;
            }
            const auto side = r ? (1U << (r - 1)) + 1 : 1;
            if (h.resolutions[r].size() != side * side) {
                return false;
            }
            m.resolutions |= UInt8(1U << r);
            raw.insert(raw.end(), h.resolutions[r].begin(), h.resolutions[r].end());
        }
        if (raw.empty()) {
            return false;
        }
        std::vector<UInt16> colors = raw;
        std::ranges::sort(colors);
        const auto distinct = std::ranges::unique(colors);
        colors.erase(distinct.begin(), distinct.end());
        if (colors.size() == 1) {
            m.compression = 0;
            m.resolutions = 1;
            m.color = colors[0];
        } else if (colors.size() <= 255) {
            const UInt32 bits = colors.size() > 16  ? 8
                                : colors.size() > 4 ? 4
                                : colors.size() > 2 ? 2
                                                    : 1;
            m.compression = UInt8(5 - std::bit_width(bits));
            m.colors = UInt8(colors.size());
            const auto start = pool.size();
            pool.resize(start + (raw.size() * bits + 15) / 16);
            for (std::size_t i = 0; i < raw.size(); ++i) {
                const auto code = UInt16(std::ranges::lower_bound(colors, raw[i]) - colors.begin());
                pool[start + i * bits / 16] |= UInt16(code << (i * bits % 16));
            }
            pool.insert(pool.end(), colors.begin(), colors.end());
        } else {
            pool.insert(pool.end(), raw.begin(), raw.end());
        }
        maps.push_back(m);
        if (pool.size() > 4'194'304) {
            return false;
        }
    }
    const auto build = [&](auto&& self, UInt32 x, UInt32 y, UInt32 side) -> UInt32 {
        if (side == 1) {
            const auto at = positions.find({x, y});
            return at == positions.end() ? 0 : (at->second << 4) | 1U;
        }
        std::array<UInt32, 4> nodes{};
        UInt32 mask{};
        side /= 2;
        for (UInt32 i = 0; i < 4; ++i) {
            nodes[i] = self(self, x + (i & 1) * side, y + (i >> 1) * side, side);
            if (nodes[i]) {
                mask |= 1U << i;
            }
        }
        if (!mask) {
            return 0;
        }
        const auto node = (UInt32(tree.size()) << 4) | mask;
        for (UInt32 i = 0; i < 4; ++i) {
            if (mask & (1U << i)) {
                tree.push_back(nodes[i]);
            }
        }
        return node;
    };
    if (!maps.empty()) {
        tree.push_back(build(build, 0, 0, b.width));
    }
    b.hiresMaps = std::move(maps);
    b.colorPool = std::move(pool);
    b.treeTable = std::move(tree);
    b.indexTable.clear();
    b.hiresVersion = 3;
    return true;
}

std::optional<TerrainDocument>
TerrainDocument::create(Point2I size, UInt32 width, Int32 scale, Int32 pattern, Int32 lightScale) {
    if (size.x < 1 || size.y < 1 || size.x > 256 || size.y > 256 || Int64(size.x) * size.y > 4096 ||
        scale < 0 || scale > 16 || pattern < 0 || pattern > 2 ||
        (pattern == 2 && (size.x != size.y || size.x < 4))) {
        return {};
    }
    auto block = createTerrainBlock(width, lightScale);
    if (!block) {
        return {};
    }
    TerrainDocument d;
    auto& g = d.grid;
    g.version = 1;
    g.size = size;
    g.detailCount = block->detailCount;
    g.scale = scale;
    g.pattern = pattern;
    g.blockMap.resize(std::size_t(size.x) * size.y);
    // Native OneBlockMapsToAll, EachBlockUnique, MosaicBlockPattern are
    // represented by explicit IDs. Mosaic has 16 repeated outer blocks and
    // 16 unique central blocks (active SimTerrain::createGridBlocks).
    const auto offset = ((size.x - 4) >> 1) & 3;
    for (Int32 y = 0; y < size.y; ++y) {
        for (Int32 x = 0; x < size.x; ++x) {
            auto& id = g.blockMap[std::size_t(y) * size.x + x];
            id = pattern == 0   ? 0
                 : pattern == 1 ? y * size.x + x
                                : ((x + offset) & 3) + (((y + offset) & 3) << 2);
        }
    }
    if (pattern == 2) {
        const auto center = (size.x - 4) >> 1;
        for (Int32 i = 0; i < 16; ++i) {
            g.blockMap[std::size_t(center + (i >> 2)) * size.x + center + (i & 3)] = 16 + i;
        }
    }
    const auto count = pattern == 2 ? 32 : *std::ranges::max_element(g.blockMap) + 1;
    if (memory(d) + std::size_t(count) * (sizeof(TerrainGridBlock) + block->heights.size() * 4 +
                                          block->materials.size() * 2 + block->lights.size() * 2) >
        MaxDocumentBytes) {
        return {};
    }
    for (Int32 i = 0; i < count; ++i) {
        g.blocks.push_back({i, std::format("Block{}", i)});
        d.blocks.push_back(*block);
        d.blocks.back().name = g.blocks.back().name;
    }
    g.lastBlockId = count;
    if (!d.rebuildHeightRanges() || !d.valid()) {
        return {};
    }
    return d;
}

bool TerrainDocument::rebuildHeightRanges() {
    if (blocks.empty()) {
        return false;
    }
    auto lo = 1e20f, hi = -1e20f;
    for (auto& b : blocks) {
        if (!rebuildTerrainHeightRange(b)) {
            return false;
        }
        lo = std::min(lo, b.minimumHeight);
        hi = std::max(hi, b.maximumHeight);
    }
    grid.minimumHeight = lo;
    grid.maximumHeight = hi;
    return true;
}

bool TerrainDocument::valid() const {
    if (memory(*this) > MaxDocumentBytes || blocks.size() + extras.size() + 1 > 8192 ||
        !nameValid(gridName) || blocks.size() != grid.blocks.size() || !writeTerrainGrid(grid)) {
        return false;
    }
    std::set<std::string> names{folded(gridName)};
    for (std::size_t i = 0; i < blocks.size(); ++i) {
        if (!blockShape(blocks[i]) || blocks[i].detailCount != grid.detailCount ||
            !names.insert(folded(TerrainGridFile::blockName(gridName, grid.blocks[i].id))).second) {
            return false;
        }
    }
    for (const auto& e : extras) {
        if (!nameValid(e.name) || !names.insert(folded(e.name)).second) {
            return false;
        }
    }
    return true;
}

std::optional<TerrainDocument> TerrainDocument::load(std::span<const std::byte> ted) {
    if (ted.size() < 8 || ted.size() > MaxDocumentBytes) {
        return {};
    }
    const auto word = [&](std::size_t at) {
        UInt32 n{};
        for (UInt32 i = 0; i < 4; ++i) {
            n |= std::to_integer<UInt32>(ted[at + i]) << (i * 8);
        }
        return n;
    };
    const auto strings = std::size_t(word(4));
    if (strings > ted.size() - 8) {
        return {};
    }
    const auto flags = word(strings + 4);
    const auto mask = (flags & 0x80000000U) ? 3U : 1U;
    const auto stringBytes = flags & 0x7fffffffU;
    if (stringBytes > 8192U * 256U || UInt64(strings) + 8 + stringBytes > ted.size()) {
        return {};
    }
    const auto dictionary =
        UInt64(strings) + 8 + ((UInt64(flags & 0x7fffffffU) + mask) & ~UInt64(mask));
    if (dictionary > ted.size() - 8) {
        return {};
    }
    const auto dictionaryBytes = word(std::size_t(dictionary) + 4) & 0x7fffffffU;
    if (dictionaryBytes > 8192U * 17U || dictionaryBytes % 17 ||
        dictionary + 8 + dictionaryBytes > ted.size()) {
        return {};
    }
    for (UInt32 i = 0; i < dictionaryBytes; i += 17) {
        const auto offset = word(std::size_t(dictionary) + 8 + i + 4);
        if (offset >= stringBytes) {
            return {};
        }
        const auto name = ted.subspan(
            strings + 8 + offset, std::min(std::size_t(256), std::size_t(stringBytes - offset)));
        if (std::ranges::find(name, std::byte{}) == name.end()) {
            return {};
        }
    }
    VolumeRStream volume;
    if (!volume.openVolume(ted)) {
        return {};
    }
    TerrainDocument d;
    bool found = false;
    for (const auto& item : volume.getVolumeItems()) {
        if (!item.name) {
            return {};
        }
        const auto name = folded(*item.name);
        if (name.ends_with(".dtf")) {
            if (found || !volume.open(*item.name, UInt32(MaxDocumentBytes))) {
                return {};
            }
            auto g = TerrainGridFile::load(volume, item.size);
            if (!g) {
                return {};
            }
            d.grid = std::move(*g);
            d.gridName = *item.name;
            found = true;
        }
    }
    if (!found) {
        return {};
    }
    std::set<std::string> used{folded(d.gridName)};
    for (const auto& entry : d.grid.blocks) {
        const auto name = TerrainGridFile::blockName(d.gridName, entry.id);
        if (!volume.open(name, UInt32(MaxDocumentBytes - memory(d)))) {
            return {};
        }
        auto block = TerrainGridBlock::load(volume, volume.getSize());
        if (!block) {
            return {};
        }
        d.blocks.push_back(std::move(*block));
        used.insert(folded(name));
        if (memory(d) > MaxDocumentBytes) {
            return {};
        }
    }
    for (const auto& item : volume.getVolumeItems()) {
        if (!used.contains(folded(*item.name))) {
            if (item.size > MaxDocumentBytes - memory(d) ||
                !volume.open(*item.name, UInt32(MaxDocumentBytes - memory(d)))) {
                return {};
            }
            d.extras.push_back({item.id, *item.name, {volume.data().begin(), volume.data().end()}});
        }
    }
    if (!d.valid()) {
        return {};
    }
    return d;
}

std::optional<std::vector<std::byte>> TerrainDocument::encode() const {
    if (!valid()) {
        return {};
    }
    Bytes out, strings, dictionary;
    out.put(kVolFileId);
    out.put(UInt32{});
    const auto entry = [&](UInt32 id, const std::string& name, std::span<const std::byte> bytes) {
        dictionary.put(id);
        dictionary.put(UInt32(strings.data.size()));
        dictionary.put(UInt32(out.data.size()));
        dictionary.put(UInt32(bytes.size()));
        dictionary.put(UInt8(STRM_COMPRESS_NONE));
        strings.text(name);
        strings.put(UInt8{});
        out.block(kVolFileBlockTag, bytes);
    };
    auto gridBytes = writeTerrainGrid(grid);
    entry(0, gridName, *gridBytes);
    for (std::size_t i = 0; i < blocks.size(); ++i) {
        auto bytes = writeTerrainBlock(blocks[i]);
        if (!bytes) {
            return {};
        }
        entry(0, TerrainGridFile::blockName(gridName, grid.blocks[i].id), *bytes);
        if (out.data.size() > MaxDocumentBytes) {
            return {};
        }
    }
    for (const auto& e : extras) {
        entry(e.id, e.name, e.bytes);
        if (out.data.size() > MaxDocumentBytes) {
            return {};
        }
    }
    const auto offset = UInt32(out.data.size());
    for (UInt32 i = 0; i < 4; ++i) {
        out.data[4 + i] = std::byte((offset >> (i * 8)) & 255);
    }
    out.block(kVolStringTag, strings.data);
    out.block(kVolIdsTag, dictionary.data);
    if (out.data.size() > MaxDocumentBytes) {
        return {};
    }
    return out.data;
}

bool TerrainDocument::save(const std::filesystem::path& path) const {
    const auto bytes = encode();
    if (!bytes) {
        return false;
    }
    static std::atomic<UInt64> serial{};
#ifdef _WIN32
    const auto process = UInt64(GetCurrentProcessId());
#else
    const auto process = UInt64(getpid());
#endif
    auto temp = path;
    temp += std::format(".{}.{}.tmp", process, ++serial);
    bool ok{};
#ifdef _WIN32
    const auto file = CreateFileW(
        temp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }
    DWORD written{};
    ok = WriteFile(file, bytes->data(), DWORD(bytes->size()), &written, nullptr) &&
         written == bytes->size() && FlushFileBuffers(file);
    CloseHandle(file);
#else
    auto* file = std::fopen(temp.c_str(), "wbx");
    if (!file) {
        return false;
    }
    ok = std::fwrite(bytes->data(), 1, bytes->size(), file) == bytes->size() &&
         std::fflush(file) == 0 && fsync(fileno(file)) == 0;
    ok = std::fclose(file) == 0 && ok;
#endif
    bool replaced = false;
    if (ok) {
#ifdef _WIN32
        replaced = MoveFileExW(temp.c_str(),
                               path.c_str(),
                               MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#else
        std::error_code ec;
        std::filesystem::rename(temp, path, ec);
        replaced = !ec;
#endif
    }
    std::error_code ignored;
    std::filesystem::remove(temp, ignored);
    return replaced;
}

std::optional<TerrainGridResources>
TerrainDocument::resources(TsMaterialResources materials) const {
    if (!valid()) {
        return {};
    }
    ResourceManager owner;
    TerrainGridResources result;
    if (!owner.add(gridName, std::make_unique<TerrainGridFile>(grid))) {
        return {};
    }
    result.grid = owner.load<TerrainGridFile>(gridName);
    for (std::size_t i = 0; i < blocks.size(); ++i) {
        const auto name = TerrainGridFile::blockName(gridName, grid.blocks[i].id);
        if (!owner.add(name, std::make_unique<TerrainGridBlock>(blocks[i]))) {
            return {};
        }
        result.blocks.push_back(owner.load<TerrainGridBlock>(name));
    }
    result.materials = std::move(materials);
    return result;
}

TerrainHistory::TerrainHistory(std::size_t maxBytes, UInt32 maxSteps)
    : maxBytes_(std::min(maxBytes, MaxDocumentBytes)), maxSteps_(std::min(maxSteps, UInt32(1024))) {
}

bool TerrainHistory::attach(TerrainDocument d) {
    if (!d.valid() || memory(d) > maxBytes_) {
        return false;
    }
    auto next = std::make_shared<const TerrainDocument>(std::move(d));
    if (publish && !publish(*next)) {
        return false;
    }
    current_ = std::move(next);
    undo_.clear();
    redo_.clear();
    ++revision_;
    return true;
}

void TerrainHistory::detach() {
    current_.reset();
    undo_.clear();
    redo_.clear();
    ++revision_;
}

bool TerrainHistory::edit(const std::function<bool(TerrainDocument&)>& operation) {
    if (!current_ || !operation) {
        return false;
    }
    auto next = *current_;
    if (!operation(next) || !next.rebuildHeightRanges() || !next.valid() ||
        memory(next) > maxBytes_) {
        return false;
    }
    auto published = std::make_shared<const TerrainDocument>(std::move(next));
    auto history = undo_;
    if (maxSteps_) {
        history.push_back(current_);
    }
    std::size_t bytes = memory(*published);
    for (const auto& d : history) {
        bytes += memory(*d);
    }
    while (!history.empty() && (history.size() > maxSteps_ || bytes > maxBytes_)) {
        bytes -= memory(*history.front());
        history.erase(history.begin());
    }
    if (publish && !publish(*published)) {
        return false;
    }
    undo_ = std::move(history);
    redo_.clear();
    current_ = std::move(published);
    ++revision_;
    return true;
}

bool TerrainHistory::undo() {
    if (undo_.empty()) {
        return false;
    }
    auto nextRedo = redo_;
    nextRedo.push_back(current_);
    if (publish && !publish(*undo_.back())) {
        return false;
    }
    redo_ = std::move(nextRedo);
    current_ = undo_.back();
    undo_.pop_back();
    ++revision_;
    return true;
}

bool TerrainHistory::redo() {
    if (redo_.empty()) {
        return false;
    }
    auto nextUndo = undo_;
    nextUndo.push_back(current_);
    if (publish && !publish(*redo_.back())) {
        return false;
    }
    undo_ = std::move(nextUndo);
    current_ = redo_.back();
    redo_.pop_back();
    ++revision_;
    return true;
}

bool TerrainHistory::limit(UInt32 steps) {
    if (steps > 1024) {
        return false;
    }
    maxSteps_ = steps;
    if (undo_.size() > steps) {
        undo_.erase(undo_.begin(), undo_.end() - steps);
    }
    if (redo_.size() > steps) {
        redo_.erase(redo_.begin(), redo_.end() - steps);
    }
    return true;
}

bool relightTerrainLowResolution(TerrainDocument& document,
                                 const TsLighting& lighting,
                                 Point2F groundOrigin) {
    if (!document.valid() || !lighting.valid() || !std::isfinite(groundOrigin.x) ||
        !std::isfinite(groundOrigin.y)) {
        return false;
    }
    const auto resources = document.resources();
    if (!resources) {
        return false;
    }
    const auto collision = TerrainCollisionSnapshot::prepare(*resources);
    if (!collision) {
        return false;
    }
    auto next = document;
    const auto blockAt = [&](Int32 x, Int32 y) -> TerrainGridBlock* {
        const auto id = next.grid.blockMap[std::size_t(y) * next.grid.size.x + x];
        const auto entry = std::ranges::find(next.grid.blocks, id, &TerrainGridBlockEntry::id);
        return entry == next.grid.blocks.end()
                   ? nullptr
                   : &next.blocks[std::size_t(entry - next.grid.blocks.begin())];
    };
    for (Int32 by = 0; by < next.grid.size.y; ++by) {
        for (Int32 bx = 0; bx < next.grid.size.x; ++bx) {
            auto* block = blockAt(bx, by);
            if (!block) {
                continue;
            }
            if (block->lightScale < 0) {
                continue;
            }
            const UInt32 side = (block->width << block->lightScale) + 1;
            const auto heightSide = block->width + 1;
            for (UInt32 j = 0; j < side; ++j) {
                for (UInt32 i = 0; i < side; ++i) {
                    const UInt32 x = i >> block->lightScale, y = j >> block->lightScale;
                    const auto h = [&](UInt32 px, UInt32 py) {
                        return block->heights[std::size_t(py) * heightSide + px];
                    };
                    const Point3F point{
                        groundOrigin.x + Flt32((i << next.grid.scale) >> block->lightScale),
                        groundOrigin.y + Flt32((j << next.grid.scale) >> block->lightScale),
                        h(x, y)};
                    Point3F normal{h(x ? x - 1 : x, y) - h(x < block->width ? x + 1 : x, y),
                                   h(x, y ? y - 1 : y) - h(x, y < block->width ? y + 1 : y),
                                   Flt32(1U << (next.grid.scale + 1))};
                    normal.normalizef(1.f);
                    auto visible = lighting;
                    visible.lights.clear();
                    visible.worldSources.clear();
                    for (std::size_t lightIndex = 0; lightIndex < lighting.lights.size();
                         ++lightIndex) {
                        const auto& light = lighting.lights[lightIndex];
                        if (light.type == TsLight::Type::Directional) {
                            const auto start = point + Point3F{0, 0, .5f};
                            CollisionSurfaceList hit;
                            const auto direction =
                                lighting.worldSources.empty()
                                    ? light.direction
                                    : lighting.worldSources[lightIndex].direction;
                            if (collision->line(start, start - direction * 1000.f, hit)) {
                                continue;
                            }
                        }
                        visible.lights.push_back(light);
                    }
                    const auto color = lightTsVertex(visible, point, normal);
                    if (!color) {
                        return false;
                    }
                    const Flt32 intensity =
                        Flt32(std::min(1.0, color->x * .22 + color->y * .67 + color->z * .11));
                    const auto packed = (Int32(intensity * 65535) & 0xf000) |
                                        (Int32(color->x * 15.9) << 8) |
                                        (Int32(color->y * 15.9) << 4) | Int32(color->z * 15.9);
                    block->lights[std::size_t(j) * side + i] = UInt16(packed);
                }
            }
            if (bx) {
                if (auto* previous = blockAt(bx - 1, by);
                    previous && previous->lights.size() == block->lights.size()) {
                    for (UInt32 j = 0; j < side; ++j) {
                        previous->lights[std::size_t(j) * side + side - 1] =
                            block->lights[std::size_t(j) * side];
                    }
                }
            }
            if (by) {
                if (auto* previous = blockAt(bx, by - 1);
                    previous && previous->lights.size() == block->lights.size()) {
                    std::copy_n(block->lights.begin(),
                                side,
                                previous->lights.begin() + std::size_t(side - 1) * side);
                }
            }
        }
    }
    document = std::move(next);
    return true;
}

namespace {
UInt16 terrainColor(Point3F color) {
    const Flt32 intensity = Flt32(std::min(1.0, color.x * .22 + color.y * .67 + color.z * .11));
    return UInt16((Int32(intensity * 65535) & 0xf000) | (Int32(color.x * 15.9) << 8) |
                  (Int32(color.y * 15.9) << 4) | Int32(color.z * 15.9));
}

UInt16 interiorColor(Point3F color) {
    const auto component = [](Flt32 value) {
        return value < 0 ? 0 : value > 1 ? 15 : Int32(value * 15) & 15;
    };
    return UInt16((component(color.x * .3f + color.y * .6f + color.z * .1f) << 12) |
                  (component(color.x) << 8) | (component(color.y) << 4) | component(color.z));
}

void downsampleTerrainLight(TerrainAuthoredLight& map) {
    for (UInt32 from = 5; from; --from) {
        const UInt32 sourceWidth = (1U << (from - 1)) + 1,
                     destinationWidth = from == 1 ? 1 : (1U << (from - 2)) + 1;
        const auto& source = map.resolutions[from];
        auto& destination = map.resolutions[from - 1];
        destination.resize(destinationWidth * destinationWidth);
        for (UInt32 y = 0; y < destinationWidth; ++y) {
            for (UInt32 x = 0; x < destinationWidth; ++x) {
                std::array<Int32, 4> sum{};
                Int32 divisor = 0;
                const auto add = [&](UInt32 sx, UInt32 sy, Int32 weight) {
                    const UInt16 color = source[sy * sourceWidth + sx];
                    divisor += weight;
                    for (UInt32 channel = 0; channel < 4; ++channel) {
                        sum[channel] += Int32((color >> (channel * 4)) & 15) * 65536 * weight;
                    }
                };
                if (from == 1) {
                    for (UInt32 sy = 0; sy < 2; ++sy) {
                        for (UInt32 sx = 0; sx < 2; ++sx) {
                            add(sx, sy, 1);
                        }
                    }
                } else {
                    for (Int32 dy = -1; dy <= 1; ++dy) {
                        for (Int32 dx = -1; dx <= 1; ++dx) {
                            const Int32 sx = Int32(x * 2) + dx, sy = Int32(y * 2) + dy;
                            if (sx >= 0 && sy >= 0 && sx < Int32(sourceWidth) &&
                                sy < Int32(sourceWidth)) {
                                add(UInt32(sx), UInt32(sy), dx && dy ? 1 : dx || dy ? 2 : 4);
                            }
                        }
                    }
                }
                UInt16 color = 0;
                for (UInt32 channel = 0; channel < 4; ++channel) {
                    color |= UInt16(((sum[channel] / divisor + 32768) >> 16) << (channel * 4));
                }
                destination[y * destinationWidth + x] = color;
            }
        }
    }
}
}

bool relightTerrainDetailed(TerrainDocument& document,
                            const TsLighting& lighting,
                            const TerrainRelightOptions& options) {
    if (!document.valid() || !lighting.valid() || document.grid.scale < 1 ||
        (options.interiorShadows && !options.interiorLOS)) {
        return false;
    }
    for (const auto& light : lighting.lights) {
        if (light.type == TsLight::Type::DirectionalNonlinear ||
            ((light.type == TsLight::Type::Directional ||
              light.type == TsLight::Type::DirectionalWrap) &&
             (light.direction.z == 0 ||
              document.grid.maximumHeight <= document.grid.minimumHeight))) {
            return false;
        }
    }
    const auto resources = document.resources();
    if (!resources) {
        return false;
    }
    const auto collision = TerrainCollisionSnapshot::prepare(*resources);
    if (!collision) {
        return false;
    }
    auto next = document;
    const auto& grid = next.grid;
    const Int32 scale = grid.scale, groundShift = grid.detailCount - 1 + scale;
    const Flt32 step = Flt32(1U << scale), offset = Flt32(1U << (scale - 1)),
                blockExtent = Flt32(1U << groundShift);
    const auto context = lighting.shapeToWorld.p;
    const auto towardLight = [&](const TsLight& light) {
        auto direction = light.direction *
                         std::abs((grid.maximumHeight - grid.minimumHeight) / light.direction.z);
        direction = -direction;
        direction.normalize();
        return direction;
    };
    auto inverse = lighting.shapeToWorld;
    inverse.inverse();
    const auto blockIndex = [&](Int32 blockId) -> std::optional<std::size_t> {
        const auto at = std::ranges::find(grid.blocks, blockId, &TerrainGridBlockEntry::id);
        return at == grid.blocks.end()
                   ? std::nullopt
                   : std::optional<std::size_t>(std::size_t(at - grid.blocks.begin()));
    };
    const auto placement = [&](Int32 x, Int32 y) -> std::optional<std::size_t> {
        return x < 0 || y < 0 || x >= grid.size.x || y >= grid.size.y
                   ? std::nullopt
                   : blockIndex(grid.blockMap[std::size_t(y) * grid.size.x + x]);
    };

    struct BlockPoint {
        std::size_t index{};
        Point2F point;
    };

    const auto blockPoint = [&](Point2F point) -> std::optional<BlockPoint> {
        const Int32 x = Int32(point.x) >> groundShift, y = Int32(point.y) >> groundShift;
        const auto index = placement(x, y);
        return index ? std::optional<BlockPoint>(
                           {*index,
                            {point.x - Flt32(x) * blockExtent, point.y - Flt32(y) * blockExtent}})
                     : std::nullopt;
    };
    const auto setColor = [&](Point2F point, Point3F color) {
        auto info = blockPoint(point);
        if (!info || next.blocks[info->index].lightScale < 0) {
            return;
        }
        const UInt16 packed = terrainColor(color);
        const Int32 lightShift = scale + next.blocks[info->index].lightScale;
        const auto write = [&](BlockPoint infoToWrite) -> Point2I {
            auto& block = next.blocks[infoToWrite.index];
            const Int32 shift = scale + block.lightScale;
            const Point2I cell{Int32(infoToWrite.point.x) >> shift,
                               Int32(infoToWrite.point.y) >> shift};
            const auto side = (block.width << block.lightScale) + 1;
            if (cell.x >= 0 && cell.y >= 0 && UInt32(cell.x) < side && UInt32(cell.y) < side) {
                block.lights[std::size_t(cell.y) * side + cell.x] = packed;
            }
            return cell;
        };
        auto cell = write(*info);
        if (!cell.x) {
            auto previous = blockPoint({point.x - Flt32(1U << lightShift), point.y});
            if (previous) {
                previous->point.x += Flt32(1U << lightShift);
                cell = write(*previous);
            }
        }
        if (!cell.y) {
            auto previous = blockPoint({point.x, point.y - Flt32(1U << lightShift)});
            if (previous) {
                previous->point.y += Flt32(1U << lightShift);
                (void)write(*previous);
            }
        }
    };
    const auto copySeams = [&] {
        for (Int32 y = 0; y < grid.size.y; ++y) {
            for (Int32 x = 0; x < grid.size.x; ++x) {
                const auto at = placement(x, y);
                if (!at) {
                    continue;
                }
                const auto& block = next.blocks[*at];
                if (block.lightScale < 0) {
                    continue;
                }
                const UInt32 side = (block.width << block.lightScale) + 1;
                if (const auto left = placement(x - 1, y);
                    left && next.blocks[*left].lights.size() == block.lights.size()) {
                    for (UInt32 row = 0; row < side; ++row) {
                        next.blocks[*left].lights[std::size_t(row) * side + side - 1] =
                            block.lights[std::size_t(row) * side];
                    }
                }
                if (const auto above = placement(x, y - 1);
                    above && next.blocks[*above].lights.size() == block.lights.size()) {
                    std::copy_n(block.lights.begin(),
                                side,
                                next.blocks[*above].lights.begin() + std::size_t(side - 1) * side);
                }
            }
        }
    };

    struct Area {
        Point2F min, max;

        bool contains(Point2F p) const {
            return p.x >= min.x && p.y >= min.y && p.x <= max.x && p.y <= max.y;
        }
    };

    std::vector<Area> areas;
    if (grid.pattern == 0) {
        const Point2I center{(grid.size.x + 1) / 2, (grid.size.y + 1) / 2};
        areas.push_back({{Flt32(center.x - 1) * blockExtent, Flt32(center.y - 1) * blockExtent},
                         {Flt32(center.x) * blockExtent, Flt32(center.y) * blockExtent}});
    } else if (grid.pattern == 1) {
        areas.push_back(
            {{0, 0}, {Flt32(grid.size.x) * blockExtent, Flt32(grid.size.y) * blockExtent}});
    } else {
        const Point2I count{grid.size.x / 3, grid.size.y / 3};
        const Area center{{Flt32(count.x) * blockExtent, Flt32(count.y) * blockExtent},
                          {Flt32(count.x * 2) * blockExtent, Flt32(count.y * 2) * blockExtent}};
        areas.push_back(center);
        Point2I chosen{};
        for (const auto& bounds : options.interiorBounds) {
            const Point2F point{bounds.fMin.x - context.x, bounds.fMin.y - context.y};
            if (center.contains(point)) {
                continue;
            }
            if (point.x > center.min.x) {
                ++chosen.x;
            }
            if (point.x > center.max.x) {
                ++chosen.x;
            }
            if (point.y > center.min.y) {
                ++chosen.y;
            }
            if (point.y > center.max.y) {
                ++chosen.y;
            }
            break;
        }
        areas.push_back(
            {{Flt32(count.x * chosen.x) * blockExtent, Flt32(count.y * chosen.y) * blockExtent},
             {Flt32(count.x * (chosen.x + 1)) * blockExtent,
              Flt32(count.y * (chosen.y + 1)) * blockExtent}});
    }
    std::vector<Area> hiresBounds;
    for (const auto& bounds : options.interiorBounds) {
        for (const auto& light : lighting.lights) {
            if (light.type != TsLight::Type::Directional &&
                light.type != TsLight::Type::DirectionalWrap) {
                continue;
            }
            auto direction = towardLight(light);
            Area projected{{1e5f, 1e5f}, {-1e5f, -1e5f}};
            bool missed = false;
            for (UInt32 corner = 0; corner < 8; ++corner) {
                const Point3F point{(corner & 1 ? bounds.fMin.x : bounds.fMax.x) - context.x,
                                    (corner & 2 ? bounds.fMin.y : bounds.fMax.y) - context.y,
                                    corner & 4 ? bounds.fMin.z : bounds.fMax.z};
                const auto start = point + direction * 2000.f;
                CollisionSurfaceList hit;
                if (!collision->line(start, start - direction * 4000.f, hit, true) ||
                    hit.surfaces.empty()) {
                    missed = true;
                    continue;
                }
                const auto p = hit.surfaces[0].position;
                projected.min.x = std::min(projected.min.x, p.x);
                projected.min.y = std::min(projected.min.y, p.y);
                projected.max.x = std::max(projected.max.x, p.x);
                projected.max.y = std::max(projected.max.y, p.y);
            }
            if (!missed) {
                projected.min -= Point2F{offset, offset};
                projected.max += Point2F{offset, offset};
                hiresBounds.push_back(projected);
            }
        }
    }
    for (const auto& light : lighting.lights) {
        if (light.type == TsLight::Type::Point) {
            const auto extent = light.range + offset;
            hiresBounds.push_back({{light.position.x - extent, light.position.y - extent},
                                   {light.position.x + extent, light.position.y + extent}});
        }
    }
    const auto intensity = [&](Point3F point, Point3F normal, bool allowShadow, bool& hitObject) {
        Point3F color{};
        hitObject = false;
        for (const auto& light : lighting.lights) {
            const bool directional = light.type != TsLight::Type::Point;
            const auto direction = directional ? towardLight(light) : Point3F{};
            const auto end = directional ? point + direction * 2000.f : light.position;
            CollisionSurfaceList terrainHit;
            if (collision->line(point, end, terrainHit)) {
                continue;
            }
            bool hit =
                options.interiorShadows && options.interiorLOS(point + context, end + context);
            Flt32 factor{};
            bool apply = false;
            if (directional) {
                if (!allowShadow || !hit) {
                    factor = std::abs(m_dot(normal, direction));
                    apply = true;
                }
            } else {
                const Flt32 length = (end - point).len();
                if (light.range > length) {
                    factor = (light.range - length) / light.range;
                    apply = !hit || !allowShadow;
                } else {
                    hit = false;
                }
            }
            if (apply) {
                color += light.intensity * factor;
            }
            hitObject |= hit;
        }
        return color + lighting.ambient;
    };

    struct Sample {
        bool valid{}, hit{}, hires{};
        Int32 blockId{};
        Point3F point{}, normal{}, color{};
        Point2I blockCell;
    };

    std::vector<std::vector<TerrainAuthoredLight>> maps(next.blocks.size());
    std::vector<std::vector<UInt16>> firstPass;
    for (UInt32 pass = 0; pass < (options.hiresShadows ? 2U : 1U); ++pass) {
        const bool detailed = options.hiresShadows && pass == 0;
        for (const auto& area : areas) {
            Point3F lo, hi;
            m_mul(Point3F{area.min.x + context.x, area.min.y + context.y, 0}, inverse, lo);
            m_mul(Point3F{area.max.x + context.x, area.max.y + context.y, 0}, inverse, hi);
            const Point2I first{Int32(lo.x) >> scale, Int32(lo.y) >> scale},
                last{Int32(hi.x) >> scale, Int32(hi.y) >> scale};
            const Int32 width = last.x - first.x + 1, height = last.y - first.y + 1;
            if (width <= 0 || height <= 0 || Int64(width) * height > 16'777'216) {
                return false;
            }
            const auto row = [&](Int32 rowY) {
                std::vector<Sample> samples(std::size_t(width) + 2);
                for (Int32 x = 0; x < width + 2; ++x) {
                    const Point2F point{Flt32(first.x + x - 1) * step + offset,
                                        Flt32(rowY) * step + offset};
                    const auto surface = collision->surface(point);
                    if (!surface) {
                        continue;
                    }
                    auto& sample = samples[std::size_t(x)];
                    sample.valid = true;
                    sample.blockId = surface->part;
                    sample.normal = surface->normal;
                    auto raised = surface->position;
                    raised.z += .1f;
                    sample.point = {point.x - offset, point.y - offset, raised.z};
                    sample.blockCell = {
                        Int32(sample.point.x -
                              Flt32((Int32(sample.point.x) >> groundShift) * (1 << groundShift))) >>
                            scale,
                        Int32(sample.point.y -
                              Flt32((Int32(sample.point.y) >> groundShift) * (1 << groundShift))) >>
                            scale};
                    bool hit = false;
                    sample.color = intensity(raised, sample.normal, !detailed, hit);
                    sample.hit = std::ranges::any_of(hiresBounds, [&](const auto& box) {
                        return box.contains({raised.x, raised.y});
                    });
                }
                return samples;
            };
            auto previous = row(first.y - 1), current = row(first.y), following = row(first.y + 1);
            for (Int32 y = 0; y < height; ++y) {
                for (Int32 x = 1; x <= width; ++x) {
                    auto& sample = current[std::size_t(x)];
                    Int32 count = 0;
                    Point3F sum{};
                    for (Int32 dy = -1; dy <= 1; ++dy) {
                        for (Int32 dx = -1; dx <= 1; ++dx) {
                            const auto& neighbor = (dy < 0 ? previous
                                                    : dy   ? following
                                                           : current)[std::size_t(x + dx)];
                            if (neighbor.valid) {
                                sample.hires |= neighbor.hit;
                                if (dx <= 0 && dy <= 0) {
                                    ++count;
                                    sum += sample.color;
                                }
                            }
                        }
                    }
                    if (count) {
                        const auto reciprocal = 1.f / Flt32(count);
                        sum = {std::clamp(sum.x * reciprocal, 0.f, 1.f),
                               std::clamp(sum.y * reciprocal, 0.f, 1.f),
                               std::clamp(sum.z * reciprocal, 0.f, 1.f)};
                    }
                    setColor({Flt32(first.x + x - 1) * step + offset,
                              Flt32(first.y + y) * step + offset},
                             sum);
                }
                if (detailed) {
                    for (Int32 x = 1; x <= width; ++x) {
                        const auto& sample = current[std::size_t(x)];
                        if (!sample.valid || !sample.hires) {
                            continue;
                        }
                        const auto index = blockIndex(sample.blockId);
                        if (!index) {
                            return false;
                        }
                        TerrainAuthoredLight map;
                        map.x = Int16(sample.blockCell.x);
                        map.y = Int16(sample.blockCell.y);
                        map.resolutions[5].resize(17 * 17);
                        const Flt32 spacing = step / 17, quarter = spacing / 4;
                        bool hitObject = false;
                        for (Int32 sy = 0; sy < 17; ++sy) {
                            for (Int32 sx = 0; sx < 17; ++sx) {
                                const Point2F center{
                                    sample.point.x + Flt32(sx) * spacing + spacing / 2,
                                    sample.point.y + Flt32(sy) * spacing + spacing / 2};
                                Point3F color{};
                                const auto add = [&](Point2F point) {
                                    const auto surface = collision->surface(point);
                                    if (!surface) {
                                        return false;
                                    }
                                    bool hit = false;
                                    color +=
                                        intensity({point.x, point.y, surface->position.z + .1f},
                                                  sample.normal,
                                                  true,
                                                  hit);
                                    hitObject |= hit;
                                    return true;
                                };
                                if (options.subSample) {
                                    for (Int32 dy = -1; dy <= 1; dy += 2) {
                                        for (Int32 dx = -1; dx <= 1; dx += 2) {
                                            if (!add({center.x + quarter * Flt32(dx),
                                                      center.y + quarter * Flt32(dy)})) {
                                                return false;
                                            }
                                        }
                                    }
                                    color = {std::clamp(color.x * .25f, 0.f, 1.f),
                                             std::clamp(color.y * .25f, 0.f, 1.f),
                                             std::clamp(color.z * .25f, 0.f, 1.f)};
                                } else if (!add(center)) {
                                    return false;
                                }
                                map.resolutions[5][std::size_t(sy) * 17 + sx] =
                                    interiorColor(color);
                            }
                        }
                        if (hitObject) {
                            downsampleTerrainLight(map);
                            auto& blockMaps = maps[*index];
                            const auto existing =
                                std::ranges::find_if(blockMaps, [&](const auto& value) {
                                    return value.x == map.x && value.y == map.y;
                                });
                            if (existing == blockMaps.end()) {
                                blockMaps.push_back(std::move(map));
                            } else {
                                *existing = std::move(map);
                            }
                        }
                    }
                }
                previous = std::move(current);
                current = std::move(following);
                following = row(first.y + y + 2);
            }
        }
        copySeams();
        if (detailed) {
            for (const auto& block : next.blocks) {
                firstPass.push_back(block.lights);
            }
        }
    }
    if (options.hiresShadows) {
        for (std::size_t index = 0; index < next.blocks.size(); ++index) {
            auto& block = next.blocks[index];
            if (!setTerrainHiresMaps(block, maps[index]) ||
                (block.lightScale >= 0 && !setTerrainHiresDelta(block, firstPass[index]))) {
                return false;
            }
        }
    }
    document = std::move(next);
    return true;
}
} // namespace opent1::world
