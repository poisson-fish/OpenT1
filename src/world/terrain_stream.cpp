#include "terrain_window.hpp"
#include <bit>
#include <map>
#include <cmath>

namespace opent1::world {
namespace {
// Account retained decoded allocations, excluding allocator bookkeeping and
// independently shared grid/material inputs. Capacity accounts spare storage.
std::optional<std::size_t> blockBytes(const TerrainGridBlock& block, std::size_t limit) {
    std::size_t bytes{};
    const auto charge = [&](std::size_t count, std::size_t stride) {
        if (count > (limit - bytes) / stride) {
            return false;
        }
        bytes += count * stride;
        return true;
    };
    if (!charge(1, sizeof(block)) || !charge(block.name.capacity() + 1, 1) ||
        !charge(block.heights.capacity(), sizeof(Flt32)) ||
        !charge(block.materials.capacity(), sizeof(TerrainGridMaterial)) ||
        !charge(block.lights.capacity(), sizeof(UInt16)) ||
        !charge(block.hiresDelta.capacity(), 1) ||
        !charge(block.hiresMaps.capacity(), sizeof(TerrainHiresLightMap)) ||
        !charge(block.colorPool.capacity(), sizeof(UInt16)) ||
        !charge(block.indexTable.capacity(), 1) ||
        !charge(block.treeTable.capacity(), sizeof(UInt32))) {
        return {};
    }
    for (const auto& pins : block.pins) {
        if (!charge(pins.capacity(), 1)) {
            return {};
        }
    }
    return bytes;
}

struct Evictions {
    ResourceManager& manager;
    std::vector<std::string> names;

    ~Evictions() {
        for (const auto& name : names) {
            manager.evict(name);
        }
    }
};
}

TerrainResourceStream::TerrainResourceStream(std::shared_ptr<ResourceManager> manager,
                                             std::string name,
                                             TerrainGridResources resources,
                                             std::size_t bytes,
                                             UInt32 blocks)
    : manager_(std::move(manager)), name_(std::move(name)), resources_(std::move(resources)),
      state_(resources_), cache_(resources_), maximumBytes_(bytes), maximumBlocks_(blocks),
      renderNeeded_(resources_.blocks.size()), collisionNeeded_(resources_.blocks.size()) {}

std::unique_ptr<TerrainResourceStream>
TerrainResourceStream::open(std::shared_ptr<ResourceManager> manager,
                            std::string_view name,
                            std::size_t bytes,
                            UInt32 blocks) {
    if (!manager || !bytes || bytes > 256U * 1024U * 1024U || !blocks || blocks > 4096) {
        return {};
    }
    auto resources = TerrainGridResources::loadMetadata(*manager, name);
    if (!resources) {
        return {};
    }
    return std::unique_ptr<TerrainResourceStream>(new TerrainResourceStream(
        std::move(manager), std::string(name), std::move(*resources), bytes, blocks));
}

UInt32 TerrainResourceStream::residentBlocks() const {
    return UInt32(std::ranges::count_if(resources_.blocks, [](const auto& block) {
        return bool(block);
    }));
}

TerrainResourceStream::~TerrainResourceStream() {
    for (UInt32 i = 0; i < resources_.blocks.size(); ++i) {
        if (!resources_.blocks[i]) {
            continue;
        }
        const auto name = TerrainGridFile::blockName(name_, resources_.grid->blocks[i].id);
        resources_.blocks[i].reset();
        state_.resources_.blocks[i].reset();
        cache_.resources_.blocks[i].reset();
        manager_->evict(name);
    }
}

std::optional<WorldSurfaceMesh> TerrainResourceStream::prepare(const TerrainLodView& view,
                                                               bool baked,
                                                               std::optional<Box2F> region,
                                                               const TsLighting* scene) {
    const auto& grid = *resources_.grid;
    const auto n = 1U << (grid.detailCount - 1);
    const auto selection = terrain_detail::selectWindow(grid, n, view, region, state_.window_);
    if (!selection ||
        (view.compositeTextures && (!std::has_single_bit(view.texturePixels) ||
                                    view.texturePixels < 4 || view.texturePixels > 256 ||
                                    (view.textureTiming != TerrainTextureTiming::RetainedLru &&
                                     view.textureTiming != TerrainTextureTiming::NativeSoftware &&
                                     view.textureTiming != TerrainTextureTiming::NativeOpenGL)))) {
        return {};
    }
    std::map<Int32, UInt32> indices;
    for (UInt32 i = 0; i < grid.blocks.size(); ++i) {
        indices.emplace(grid.blocks[i].id, i);
    }
    std::vector<bool> needed(grid.blocks.size());
    for (std::size_t i = 0; i < grid.blockMap.size(); ++i) {
        const auto id = grid.blockMap[i];
        if (id == -1) {
            continue;
        }
        const auto x = UInt32(i % std::size_t(grid.size.x)) * n,
                   y = UInt32(i / std::size_t(grid.size.x)) * n;
        if (!terrain_detail::intersects(*selection, x, y, n)) {
            continue;
        }
        const auto index = indices.at(id);
        needed[index] = true;
    }
    auto combined = needed;
    for (std::size_t i = 0; i < combined.size(); ++i) {
        combined[i] = combined[i] || collisionNeeded_[i];
    }
    Evictions evictions{*manager_, {}};
    std::size_t bytes{};
    auto staged = stage(combined, evictions.names, bytes);
    if (!staged) {
        return {};
    }
    auto next = std::move(*staged);
    auto state = state_;
    state.resources_ = next;
    auto cache = cache_;
    cache.rebind(next);
    auto mesh = WorldSurfaceMesh::terrainLod(next, view, baked, region, &cache, &state, scene);
    if (!mesh) {
        return {};
    }
    resources_ = std::move(next);
    state_ = std::move(state);
    cache_ = std::move(cache);
    bytes_ = bytes;
    renderNeeded_ = std::move(needed);
    return mesh;
}

std::optional<TerrainGridResources>
TerrainResourceStream::stage(const std::vector<bool>& needed,
                             std::vector<std::string>& evictions,
                             std::size_t& bytes) const {
    const auto& grid = *resources_.grid;
    const auto n = 1U << (grid.detailCount - 1);
    if (UInt32(std::ranges::count(needed, true)) > maximumBlocks_ ||
        manager_->load<TerrainGridFile>(name_).get() != resources_.grid.get()) {
        return {};
    }
    auto next = resources_;
    std::size_t heights{};
    bytes = 0;
    for (UInt32 i = 0; i < next.blocks.size(); ++i) {
        const auto name = TerrainGridFile::blockName(name_, grid.blocks[i].id);
        if (!needed[i]) {
            if (next.blocks[i]) {
                evictions.push_back(name);
                next.blocks[i].reset();
            }
            continue;
        }
        auto& block = next.blocks[i];
        if (!block) {
            evictions.push_back(name);
            block = manager_->load<TerrainGridBlock>(name);
        }
        if (!block || block->detailCount != grid.detailCount || block->width != n ||
            block->heights.size() != UInt64(n + 1) * (n + 1) ||
            block->materials.size() != UInt64(n) * n ||
            block->heights.size() > 4'194'304 - heights ||
            (!grid.materialList.empty() && !std::ranges::all_of(block->materials, [&](auto m) {
                return m.hole() || Int32(m.index) < next.materials.list().materialsPerDetail;
            }))) {
            return {};
        }
        const auto size = blockBytes(*block, maximumBytes_ - bytes);
        if (!size) {
            return {};
        }
        bytes += *size;
        heights += block->heights.size();
    }
    return next;
}

std::shared_ptr<const TerrainCollisionSnapshot>
TerrainResourceStream::prepareCollision(const Box2F& region) {
    if (!std::isfinite(region.fMin.x) || !std::isfinite(region.fMin.y) ||
        !std::isfinite(region.fMax.x) || !std::isfinite(region.fMax.y) ||
        region.fMin.x > region.fMax.x || region.fMin.y > region.fMax.y ||
        std::max({std::abs(region.fMin.x),
                  std::abs(region.fMin.y),
                  std::abs(region.fMax.x),
                  std::abs(region.fMax.y)}) > 1e8f) {
        return {};
    }
    const auto& grid = *resources_.grid;
    const auto width = Flt64(1U << (grid.detailCount - 1 + grid.scale));
    std::map<Int32, UInt32> indices;
    for (UInt32 i = 0; i < grid.blocks.size(); ++i) {
        indices.emplace(grid.blocks[i].id, i);
    }
    std::vector<bool> needed(grid.blocks.size());
    for (std::size_t i = 0; i < grid.blockMap.size(); ++i) {
        const auto id = grid.blockMap[i];
        if (id == -1) {
            continue;
        }
        const auto x = Flt64(i % std::size_t(grid.size.x)) * width,
                   y = Flt64(i / std::size_t(grid.size.x)) * width;
        // Inclusive touching placements cover exact native block crossings.
        // Native surface queries truncate coordinates in (-1,0) before shifting.
        if (region.fMax.x < (x == 0 ? -1. : x) || region.fMax.y < (y == 0 ? -1. : y) ||
            region.fMin.x > x + width || region.fMin.y > y + width) {
            continue;
        }
        needed[indices.at(id)] = true;
    }
    auto combined = needed;
    for (std::size_t i = 0; i < combined.size(); ++i) {
        combined[i] = combined[i] || renderNeeded_[i];
    }
    Evictions evictions{*manager_, {}};
    std::size_t bytes{};
    auto next = stage(combined, evictions.names, bytes);
    if (!next) {
        return {};
    }
    auto geometry = TerrainCollisionSnapshot::prepare(*next);
    if (!geometry) {
        return {};
    }
    auto result = std::make_shared<const TerrainCollisionSnapshot>(std::move(*geometry));
    auto state = state_;
    state.resources_ = *next;
    auto cache = cache_;
    cache.rebind(*next);
    resources_ = std::move(*next);
    state_ = std::move(state);
    cache_ = std::move(cache);
    bytes_ = bytes;
    collisionNeeded_ = std::move(needed);
    return result;
}
} // namespace opent1::world
