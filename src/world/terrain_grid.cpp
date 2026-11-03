#include <opent1/world/terrain_grid.hpp>
#include <opent1/core/lzh_stream.hpp>
#include <opent1/core/mem_stream.hpp>
#include <algorithm>
#include <bit>
#include <cmath>
#include <ranges>
#include <unordered_set>

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
        const auto result = bytes.subspan(position, count);
        position += count;
        return result;
    }

    template <class T> T scalar() {
        const auto raw = take(sizeof(T));
        if (!ok) {
            return {};
        }
        UInt32 bits{};
        std::ranges::for_each(std::views::iota(std::size_t{}, sizeof(T)), [&](auto i) {
            bits |= UInt32(std::to_integer<UInt8>(raw[i])) << (8 * i);
        });
        if constexpr (sizeof(T) == 4) {
            return std::bit_cast<T>(bits);
        } else if constexpr (sizeof(T) == 2) {
            return std::bit_cast<T>(UInt16(bits));
        } else {
            return std::bit_cast<T>(UInt8(bits));
        }
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

    std::string text(std::size_t count, bool fixed = false) {
        const auto raw = take(count);
        const auto end = std::ranges::find(raw, std::byte{});
        if (!fixed && end != raw.end()) {
            ok = false;
        }
        std::string result;
        std::ranges::transform(raw.begin(), end, std::back_inserter(result), [](auto b) {
            return char(std::to_integer<UInt8>(b));
        });
        return result;
    }

    std::string string() {
        const auto count = scalar<Int32>();
        if (count < 0 || count > 255) {
            ok = false;
            return {};
        }
        return text(std::size_t(count));
    }

    template <class T, class Fn>
    void array(std::vector<T>& result, std::size_t count, std::size_t stride, Fn fn) {
        if (!ok || count > 4'194'304 || count > remaining() / stride) {
            ok = false;
            return;
        }
        result.resize(count);
        std::ranges::for_each(result, [&](auto& value) {
            if (ok) {
                value = fn();
            }
        });
    }

    std::vector<std::byte> data(std::size_t count, bool compressed) {
        if (!ok || count > MaxTerrainFileBytes) {
            ok = false;
            return {};
        }
        if (!compressed) {
            const auto raw = take(count);
            return {raw.begin(), raw.end()};
        }
        if (scalar<UInt32>() != count || !ok) {
            ok = false;
            return {};
        }
        std::vector<std::byte> result(count);
        MemRWStream parent(bytes.subspan(position));
        LZHRStream decoder(parent);
        if (!decoder.readBytes(result)) {
            ok = false;
            return {};
        }
        // The original LZH filter consumes its lookahead/terminator byte. Start
        // the following map at the parent position, not the decoded byte count.
        take(std::size_t(parent.getPosition()));
        return result;
    }

    Reader object(UInt32 expected, std::string_view className, Int32& version) {
        const auto tag = scalar<UInt32>(), flags = scalar<UInt32>();
        const auto count = flags & 0x7fffffffU;
        const auto raw = take(count);
        const std::size_t mask = (flags & 0x80000000U) ? 3 : 1;
        take(((std::size_t(count) + mask) & ~mask) - count);
        Reader child{raw};
        if (!ok) {
            child.ok = false;
            return child;
        }
        if (tag == makeFourCC('P', 'E', 'R', 'S')) {
            const auto length = child.scalar<Int16>();
            if (length <= 0 || length > 128) {
                child.ok = false;
                return child;
            }
            const auto name = child.text(std::size_t(length));
            if (length & 1) {
                child.take(1);
            }
            if (name != className) {
                child.ok = false;
            }
        } else if (tag != expected) {
            child.ok = false;
        }
        version = child.scalar<Int32>();
        return child;
    }

    bool done() const {
        return ok && remaining() == 0;
    }
};

std::optional<std::vector<std::byte>> file(StreamIO& stream, UInt32 size) {
    if (size < 12 || size > MaxTerrainFileBytes) {
        return {};
    }
    std::vector<std::byte> bytes(size);
    if (!stream.readBytes(bytes)) {
        return {};
    }
    return bytes;
}

bool dimensions(Int32 details, Int32 width, Int32 height) {
    return details >= 1 && details <= 11 && width == height && width == (1 << (details - 1));
}

bool boundedSize(const TerrainGridFile& grid) {
    return grid.size.x > 0 && grid.size.y > 0 && grid.size.x <= 256 && grid.size.y <= 256 &&
           Int64(grid.size.x) * grid.size.y <= 4096;
}

bool valid(const TerrainGridFile& grid) {
    if (!boundedSize(grid) || grid.detailCount < 1 || grid.detailCount > 11 || grid.scale < 0 ||
        grid.scale > 16 || grid.pattern < 0 || grid.pattern > 2 || grid.lastBlockId < 0 ||
        grid.blocks.size() > 4096 || grid.minimumHeight > grid.maximumHeight) {
        return false;
    }
    std::unordered_set<Int32> ids;
    if (!std::ranges::all_of(grid.blocks, [&](const auto& block) {
            return block.id >= 0 && ids.insert(block.id).second;
        })) {
        return false;
    }
    return grid.blockMap.size() == std::size_t(grid.size.x) * std::size_t(grid.size.y) &&
           std::ranges::all_of(grid.blockMap, [&](auto id) {
               return id == -1 || ids.contains(id);
           });
}
} // namespace

std::unique_ptr<TerrainGridBlock> TerrainGridBlock::load(StreamIO& stream, UInt32 size) {
    auto bytes = file(stream, size);
    if (!bytes) {
        return {};
    }
    auto result = std::make_unique<TerrainGridBlock>();
    Reader outer{*bytes};
    auto r = outer.object(makeFourCC('G', 'B', 'L', 'K'), "GridBlock", result->version);
    if (!r.ok || result->version < 0 || result->version > 5) {
        return {};
    }
    result->name = r.text(16, true);
    result->detailCount = r.scalar<Int32>();
    result->lightScale = r.scalar<Int32>();
    result->minimumHeight = r.real();
    result->maximumHeight = r.real();
    const auto width = r.scalar<Int32>(), height = r.scalar<Int32>();
    if (!r.ok || !dimensions(result->detailCount, width, height) || result->lightScale < -1 ||
        result->lightScale > 10 || result->minimumHeight > result->maximumHeight ||
        (result->lightScale >= 0 && (width << result->lightScale) > 1024)) {
        return {};
    }
    result->width = UInt32(width);
    const auto side = std::size_t(width) + 1;
    const auto heights = side * side, cells = std::size_t(width) * std::size_t(width);
    if (result->version == 0 || result->version >= 4) {
        auto raw = r.data(heights * 4, result->version >= 4);
        Reader values{raw};
        values.array(result->heights, heights, 4, [&] {
            return values.real();
        });
        if (!values.done()) {
            return {};
        }
    } else {
        // RickO compression: raw border rows, leading/trailing heights per row,
        // and signed 8-bit deltas against the preceding height in that row.
        result->heights.resize(heights);
        std::ranges::for_each(std::views::iota(std::size_t{}, side), [&](auto x) {
            result->heights[x] = r.real();
        });
        std::ranges::for_each(std::views::iota(1, width), [&](auto y) {
            const auto scale = r.real();
            auto at = std::size_t(y) * side;
            result->heights[at] = r.real();
            std::ranges::for_each(std::views::iota(1, width), [&](auto) {
                const auto delta = Flt32(r.scalar<Int8>()) * scale;
                result->heights[at + 1] = result->heights[at] + delta;
                ++at;
            });
            result->heights[at + 1] = r.real();
        });
        std::ranges::for_each(std::views::iota(std::size_t{}, side), [&](auto x) {
            result->heights[std::size_t(width) * side + x] = r.real();
        });
    }
    auto materialData = r.data(cells * 2, result->version >= 4);
    Reader materials{materialData};
    materials.array(result->materials, cells, 2, [&] {
        const auto flags = materials.scalar<UInt8>(), index = materials.scalar<UInt8>();
        return TerrainGridMaterial{flags, index};
    });
    if (!materials.done()) {
        return {};
    }
    if (result->version >= 2) {
        std::ranges::for_each(result->pins, [&](auto& pin) {
            const auto count = r.scalar<UInt16>();
            const auto raw = r.take(count);
            pin.assign(raw.begin(), raw.end());
        });
    }
    if (result->lightScale >= 0) {
        const auto lightSide = (std::size_t(width) << result->lightScale) + 1;
        auto raw = r.data(lightSide * lightSide * 2, result->version >= 4);
        Reader lights{raw};
        lights.array(result->lights, lightSide * lightSide, 2, [&] {
            return lights.scalar<UInt16>();
        });
        if (!lights.done()) {
            return {};
        }
    }
    if (result->version >= 5) {
        const auto count = r.scalar<UInt32>();
        const auto raw = r.take(count);
        result->hiresDelta.assign(raw.begin(), raw.end());
    }
    if (result->version >= 3) {
        result->hiresVersion = r.scalar<Int32>();
        if (result->hiresVersion != 0 && result->hiresVersion != 3) {
            return {};
        }
        if (result->hiresVersion == 3) {
            const auto count = r.scalar<UInt32>(), colors = r.scalar<UInt32>(),
                       indices = r.scalar<UInt32>(), nodes = r.scalar<UInt32>();
            if (count > 50000) {
                return {};
            }
            r.array(result->hiresMaps, count, 12, [&] {
                TerrainHiresLightMap map;
                map.x = r.scalar<Int16>();
                map.y = r.scalar<Int16>();
                map.flags = r.scalar<UInt8>();
                map.compression = r.scalar<UInt8>();
                map.resolutions = r.scalar<UInt8>();
                map.colors = r.scalar<UInt8>();
                map.color = r.scalar<UInt32>();
                return map;
            });
            r.array(result->colorPool, colors, 2, [&] {
                return r.scalar<UInt16>();
            });
            r.array(result->indexTable, indices, 1, [&] {
                return r.scalar<UInt8>();
            });
            r.array(result->treeTable, nodes, 4, [&] {
                return r.scalar<UInt32>();
            });
        }
    }
    if (!r.done() || !std::ranges::all_of(result->heights, [](auto h) {
            return std::isfinite(h) && std::abs(h) <= 1e7f;
        })) {
        return {};
    }
    return result;
}

std::optional<TerrainCollision> TerrainGridBlock::collision(Flt32 squareSize) const {
    if (!width || width > 1024 || !dimensions(detailCount, Int32(width), Int32(width)) ||
        materials.size() != std::size_t(width) * width) {
        return {};
    }
    std::vector<TerrainCollisionCell> cells;
    cells.reserve(materials.size());
    std::ranges::transform(materials, std::back_inserter(cells), [](const auto& m) {
        return TerrainCollisionCell{m.index, m.hole()};
    });
    return TerrainCollision::prepare(width, width, squareSize, heights, cells);
}

std::unique_ptr<TerrainGridFile> TerrainGridFile::load(StreamIO& stream, UInt32 size) {
    auto bytes = file(stream, size);
    if (!bytes) {
        return {};
    }
    auto result = std::make_unique<TerrainGridFile>();
    Reader outer{*bytes};
    auto r = outer.object(makeFourCC('G', 'F', 'I', 'L'), "GridFile", result->version);
    if (!r.ok || result->version < 0 || result->version > 1) {
        return {};
    }
    result->materialList = r.string();
    if (!result->materialList.empty()) {
        const auto at = result->materialList.find_last_of('.');
        const auto slash = result->materialList.find_last_of("/\\:");
        if (at != std::string::npos && (slash == std::string::npos || at > slash)) {
            result->materialList.resize(at);
        }
        result->materialList += ".dml";
    }
    result->lastBlockId = r.scalar<Int32>();
    result->detailCount = r.scalar<Int32>();
    result->scale = r.scalar<Int32>();
    // Original files can contain uninitialized scratch bounds (including ASCII
    // fragments). Retain their bits as metadata; collision bounds come from the
    // decoded heightfields. Never use these values for spatial queries.
    const auto rawPoint = [&] {
        const auto x = r.scalar<Flt32>(), y = r.scalar<Flt32>(), z = r.scalar<Flt32>();
        return Point3F{x, y, z};
    };
    result->bounds.fMin = rawPoint();
    result->bounds.fMax = rawPoint();
    result->origin.x = r.scalar<Int32>();
    result->origin.y = r.scalar<Int32>();
    result->minimumHeight = r.real();
    result->maximumHeight = r.real();
    result->size.x = r.scalar<Int32>();
    result->size.y = r.scalar<Int32>();
    if (result->version == 1) {
        result->pattern = r.scalar<Int32>();
    }
    if (!r.ok || !boundedSize(*result)) {
        return {};
    }
    r.array(result->blockMap, std::size_t(result->size.x) * std::size_t(result->size.y), 4, [&] {
        return r.scalar<Int32>();
    });
    const auto count = r.scalar<UInt32>();
    if (count > 4096) {
        return {};
    }
    r.array(result->blocks, count, 8, [&] {
        const auto id = r.scalar<Int32>();
        return TerrainGridBlockEntry{id, r.string()};
    });
    if (!r.done() || !valid(*result)) {
        return {};
    }
    return result;
}

std::string TerrainGridFile::blockName(std::string_view name, Int32 id) {
    const auto slash = name.find_last_of("/\\:"), dot = name.find_last_of('.');
    if (dot != name.npos && (slash == name.npos || dot > slash)) {
        name = name.substr(0, dot);
    }
    return std::format("{}#{}.dtb", name, id);
}

std::vector<std::string> TerrainGridFile::dependencies(std::string_view name) const {
    std::vector<std::string> result;
    if (!materialList.empty()) {
        result.push_back(materialList);
    }
    std::ranges::transform(blocks, std::back_inserter(result), [&](const auto& block) {
        return blockName(name, block.id);
    });
    return result;
}

bool registerTerrainLoaders(ResourceManager& manager) {
    const auto grid = manager.registerLoader<TerrainGridFile>(".dtf", TerrainGridFile::load);
    const auto block = manager.registerLoader<TerrainGridBlock>(".dtb", TerrainGridBlock::load);
    if (grid)
        manager.setSourceOnlyLoader(".dtf");
    if (block)
        manager.setSourceOnlyLoader(".dtb");
    return grid && block;
}

std::optional<TerrainGridResources> TerrainGridResources::loadMetadata(ResourceManager& manager,
                                                                       std::string_view name) {
    TerrainGridResources result;
    result.grid = manager.load<TerrainGridFile>(name);
    if (!result.grid || !valid(*result.grid)) {
        return {};
    }
    if (!result.grid->materialList.empty()) {
        const auto list = manager.load<TsMaterialList>(result.grid->materialList);
        if (!list) {
            return {};
        }
        auto materials = TsMaterialResources::load(manager, *list);
        if (!materials) {
            return {};
        }
        result.materials = std::move(*materials);
    }
    result.blocks.resize(result.grid->blocks.size());
    return result;
}

std::optional<TerrainGridResources> TerrainGridResources::load(ResourceManager& manager,
                                                               std::string_view name) {
    auto metadata = loadMetadata(manager, name);
    if (!metadata) {
        return {};
    }
    auto result = std::move(*metadata);
    std::size_t total{}, index{};
    for (const auto& entry : result.grid->blocks) {
        auto block = manager.load<TerrainGridBlock>(TerrainGridFile::blockName(name, entry.id));
        if (!block || block->detailCount != result.grid->detailCount ||
            block->width != (1U << (result.grid->detailCount - 1)) ||
            block->heights.size() > 4'194'304 - total ||
            (!result.grid->materialList.empty() &&
             !std::ranges::all_of(block->materials, [&](auto m) {
                 return m.hole() || Int32(m.index) < result.materials.list().materialsPerDetail;
             }))) {
            return {};
        }
        total += block->heights.size();
        result.blocks[index++] = std::move(block);
    }
    return result;
}

std::optional<std::vector<TerrainCollisionBlock>> TerrainGridResources::collisionBlocks() const {
    if (!grid || !valid(*grid) || blocks.size() != grid->blocks.size()) {
        return {};
    }
    std::vector<std::shared_ptr<const TerrainCollision>> snapshots;
    const auto squareSize = Flt32(1U << grid->scale);
    std::size_t total{};
    for (const auto& block : blocks) {
        if (!block || block->detailCount != grid->detailCount ||
            block->heights.size() > 4'194'304 - total) {
            return {};
        }
        total += block->heights.size();
        auto geometry = block->collision(squareSize);
        if (!geometry) {
            return {};
        }
        snapshots.push_back(std::make_shared<const TerrainCollision>(std::move(*geometry)));
    }
    const auto blockSize = squareSize * Flt32(1U << (grid->detailCount - 1));
    std::vector<TerrainCollisionBlock> result;
    std::ranges::for_each(std::views::iota(std::size_t{}, grid->blockMap.size()), [&](auto i) {
        const auto id = grid->blockMap[i];
        if (id == -1) {
            return;
        }
        const auto at = std::ranges::find(grid->blocks, id, &TerrainGridBlockEntry::id);
        result.push_back({{Flt32(i % std::size_t(grid->size.x)) * blockSize,
                           Flt32(i / std::size_t(grid->size.x)) * blockSize,
                           0},
                          snapshots[std::size_t(at - grid->blocks.begin())]});
    });
    return result;
}

std::optional<UInt16> TerrainGridResources::lighting(Point2F position) const {
    if (!grid || !std::isfinite(position.x) || !std::isfinite(position.y) || position.x < 0 ||
        position.y < 0 || grid->scale < 0 || grid->scale > 20 || grid->detailCount < 1 ||
        grid->detailCount > 11) {
        return {};
    }
    const auto blockSize = std::ldexp(1.f, grid->scale + grid->detailCount - 1);
    const auto bx = std::floor(position.x / blockSize), by = std::floor(position.y / blockSize);
    if (bx >= grid->size.x || by >= grid->size.y) {
        return {};
    }
    const auto index = std::size_t(by) * grid->size.x + std::size_t(bx);
    if (index >= grid->blockMap.size()) {
        return {};
    }
    const auto entry =
        std::ranges::find(grid->blocks, grid->blockMap[index], &TerrainGridBlockEntry::id);
    if (entry == grid->blocks.end()) {
        return {};
    }
    const auto slot = std::size_t(entry - grid->blocks.begin());
    if (slot >= blocks.size() || !blocks[slot]) {
        return {};
    }
    const auto& block = *blocks[slot];
    if (block.lightScale < 0 || block.lightScale > grid->scale) {
        return {};
    }
    const auto cell = std::ldexp(1.f, grid->scale - block.lightScale);
    const auto x = (position.x - bx * blockSize) / cell, y = (position.y - by * blockSize) / cell;
    const auto side = (block.width << block.lightScale) + 1U;
    const auto ix = UInt32(x), iy = UInt32(y);
    if (ix + 1 >= side || iy + 1 >= side || block.lights.size() != std::size_t(side) * side) {
        return {};
    }
    const auto blend = [](UInt16 a, UInt16 b, Flt32 fraction) {
        UInt16 result{};
        for (UInt32 shift = 0; shift < 16; shift += 4) {
            const auto mask = UInt16(15U << shift);
            result |= UInt16(Flt32(a & mask) * fraction + Flt32(b & mask) * (1 - fraction)) & mask;
        }
        return result;
    };
    const auto offset = std::size_t(iy) * side + ix;
    return blend(blend(block.lights[offset], block.lights[offset + 1], x - ix),
                 blend(block.lights[offset + side], block.lights[offset + side + 1], x - ix),
                 y - iy);
}
} // namespace opent1::world
