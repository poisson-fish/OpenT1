#include <opent1/world/terrain_grid.hpp>
#include <opent1/core/lzh_stream.hpp>
#include <opent1/core/mem_stream.hpp>
#include <opent1/gfx/render_stats.hpp>
#include <algorithm>
#include <bit>
#include <ranges>

namespace opent1::world {
std::optional<std::vector<UInt32>>
TerrainGridBlock::hiresMapIndices(UInt32 x, UInt32 y, UInt32 cells) const {
    if (!std::has_single_bit(width) || width > 1024 || !std::has_single_bit(cells) ||
        cells > width || x > width - cells || y > width - cells || x % cells || y % cells ||
        hiresMaps.size() > 50000 || treeTable.size() > 1'398'101) {
        return {};
    }
    std::vector<UInt32> result;
    // Shipped blocks (including Frost Giant) can retain an obsolete tree after
    // all maps were removed. No map storage means there is nothing to select.
    if (treeTable.empty() || hiresMaps.empty()) {
        return result;
    }
    // Native roots are stored last. Every child run precedes its parent,
    // which also prevents cycles without an unbounded visited-node allocation.
    const auto children = [&](UInt32 index) {
        const auto node = treeTable[index], mask = node & 15U, base = node >> 4;
        return mask && base < index && UInt32(std::popcount(mask)) <= index - base;
    };
    UInt32 index = UInt32(treeTable.size() - 1), cx{}, cy{}, size = width;
    while (size > cells) {
        const auto node = treeTable[index];
        if (!node) {
            return result;
        }
        if (!children(index)) {
            return {};
        }
        size /= 2;
        const auto quadrant = UInt32(x >= cx + size) + 2U * UInt32(y >= cy + size);
        if (!(node & (1U << quadrant))) {
            return result;
        }
        index = (node >> 4) + UInt32(std::popcount((node & 15U) & ((1U << quadrant) - 1)));
        if (quadrant & 1U) {
            cx += size;
        }
        if (quadrant & 2U) {
            cy += size;
        }
    }
    std::size_t work{};
    const auto collect = [&](auto&& self, UInt32 at, UInt32 xx, UInt32 yy, UInt32 side) -> bool {
        if (++work > 1'398'101) {
            return false;
        }
        const auto node = treeTable[at];
        if (!node) {
            return true;
        }
        if (side == 1) {
            const auto map = node >> 4;
            if ((node & 15U) != 1 || map >= hiresMaps.size() || hiresMaps[map].x != Int32(xx) ||
                hiresMaps[map].y != Int32(yy) || (hiresMaps[map].flags & 1U)) {
                return false;
            }
            result.push_back(map);
            return true;
        }
        if (!children(at)) {
            return false;
        }
        const auto half = side / 2;
        auto next = node >> 4;
        bool valid = true;
        // for_each guarantees visitation order; output uses native quadrant order.
        std::ranges::for_each(std::views::iota(0U, 4U), [&](UInt32 q) {
            if (valid && (node & (1U << q))) {
                valid = self(self, next++, xx + (q & 1U) * half, yy + (q >> 1) * half, half);
            }
        });
        return valid;
    };
    if (!collect(collect, index, cx, cy, size)) {
        return {};
    }
    return result;
}

std::optional<std::span<const UInt16>> TerrainGridBlock::baseLightSamples() const {
    gfx::ScopedRenderWorkTimer timer(&gfx::RenderWorkStats::lightSamplesMs);
    auto* stats = gfx::currentRenderWorkStats;
    if (stats) {
        ++stats->lightSampleCalls;
    }
    if (!width || width > 1024 || lightScale < -1 || lightScale > 10) {
        return {};
    }
    if (lightScale == -1) {
        return lights.empty() && hiresDelta.empty() ? std::optional(std::span<const UInt16>(lights))
                                                    : std::nullopt;
    }
    const auto side = (UInt64(width) << lightScale) + 1;
    if (side > 1025 || lights.size() != side * side) {
        return {};
    }
    return std::span<const UInt16>(lights);
}

std::optional<std::vector<UInt16>> TerrainGridBlock::lightSamples(bool hires) const {
    const auto base = baseLightSamples();
    if (!base) {
        return {};
    }
    gfx::ScopedRenderWorkTimer timer(&gfx::RenderWorkStats::lightSamplesMs);
    auto* stats = gfx::currentRenderWorkStats;
    auto result = lights;
    if (stats) {
        stats->lightSampleCopyBytes += lights.size() * sizeof(UInt16);
    }
    if (hires && !hiresDelta.empty()) {
        if (hiresDelta.size() > MaxTerrainFileBytes) {
            return {};
        }
        if (stats) {
            ++stats->lightDeltaDecodes;
        }
        MemRWStream stream{std::span<const std::byte>(hiresDelta)};
        LZHRStream decoder(stream);
        std::vector<std::byte> delta(lights.size() * 2);
        if (!decoder.readBytes(delta)) {
            return {};
        }
        for (std::size_t i = 0; i < result.size(); ++i) {
            result[i] ^= UInt16(std::to_integer<UInt8>(delta[2 * i]) |
                                UInt16(std::to_integer<UInt8>(delta[2 * i + 1])) << 8);
        }
    }
    return result;
}

std::optional<std::vector<UInt16>> TerrainGridBlock::hiresSamples(UInt32 index,
                                                                  UInt32 resolution) const {
    if (index >= hiresMaps.size() || resolution > 5) {
        return {};
    }
    const auto& map = hiresMaps[index];
    if ((map.flags & 1U) || !map.resolutions || (map.resolutions & ~63U) ||
        !(map.resolutions & (1U << resolution)) || map.x < 0 || map.y < 0 ||
        UInt32(map.x) >= width || UInt32(map.y) >= width || map.compression > 5) {
        return {};
    }
    if (map.resolutions == 1) {
        return std::vector<UInt16>{UInt16(map.color)};
    }
    // StraightDump saved a process pointer, and cannot describe pooled multi-
    // sample data. Native saved files use ColorDump or one of four bit widths.
    if (!map.compression) {
        return {};
    }
    UInt32 total{}, offset{}, count{};
    for (UInt32 r = 0; r <= 5; ++r) {
        const auto side = r ? (1U << (r - 1)) + 1 : 1;
        if (r == resolution) {
            offset = total;
            count = side * side;
        }
        if (map.resolutions & (1U << r)) {
            total += side * side;
        }
    }
    if (map.color > colorPool.size()) {
        return {};
    }
    const auto pool = std::span(colorPool).subspan(map.color);
    std::vector<UInt16> result(count);
    if (map.compression == 5) {
        if (total > pool.size()) {
            return {};
        }
        std::ranges::copy(pool.subspan(offset, count), result.begin());
    } else {
        const auto bits = 1U << (4 - map.compression),
                   words = (total + (1U << map.compression) - 1) >> map.compression;
        if (!map.colors || map.colors > (1U << bits) || words > pool.size() ||
            map.colors > pool.size() - words) {
            return {};
        }
        for (UInt32 i = 0; i < count; ++i) {
            const auto bit = (offset + i) * bits,
                       code = (pool[bit / 16] >> (bit % 16)) & ((1U << bits) - 1);
            if (code >= map.colors) {
                return {};
            }
            result[i] = pool[words + code];
        }
    }
    return result;
}
} // namespace opent1::world
