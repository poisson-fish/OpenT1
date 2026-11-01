#include <opent1/world/world_surfaces.hpp>
#include <opent1/gfx/render_stats.hpp>
#include "collision_geometry.hpp"
#include "terrain_window.hpp"
#include "terrain_scene_lighting.hpp"
#include <bit>
#include <functional>
#include <map>
#include <memory_resource>
#include <set>
#include <tuple>
#include <unordered_map>

namespace opent1::world {
namespace {
struct Tile {
    const TerrainGridBlock* block;
    UInt32 index, bx, by, x, y, size;
};

// Collect first, then sort/deduplicate once. Boundary queries retain the native
// coordinate order without allocating a tree node for each reached edge point.
class Edges {
    using Point = std::pair<UInt32, UInt32>;
    std::pmr::vector<Point> points_;

public:
    explicit Edges(std::pmr::memory_resource* memory) : points_(memory) {
        points_.reserve(1024);
    }

    void insert(UInt32 fixed, UInt32 along) {
        points_.emplace_back(fixed, along);
    }

    void prepare() {
        std::ranges::sort(points_);
        const auto duplicates = std::ranges::unique(points_);
        points_.erase(duplicates.begin(), duplicates.end());
    }

    [[nodiscard]] std::span<const Point> range(UInt32 fixed, UInt32 lo, UInt32 hi) const {
        const auto first = std::lower_bound(points_.begin(), points_.end(), Point{fixed, lo});
        const auto last = std::upper_bound(first, points_.end(), Point{fixed, hi});
        return {first, last};
    }
};

struct PointHash {
    std::size_t operator()(const std::pair<UInt32, UInt32>& point) const noexcept {
        UInt64 hash = (UInt64(point.first) << 32) | point.second;
        hash = (hash ^ (hash >> 30)) * 0xbf58476d1ce4e5b9ULL;
        hash = (hash ^ (hash >> 27)) * 0x94d049bb133111ebULL;
        return std::size_t(hash ^ (hash >> 31));
    }
};

Flt32 height(const Tile& t, UInt32 x, UInt32 y) {
    return t.block->heights[std::size_t(y) * (t.block->width + 1) + x];
}

Point3F rgb(UInt16 c) {
    return {Flt32((c >> 8) & 15) / 15, Flt32((c >> 4) & 15) / 15, Flt32(c & 15) / 15};
}
}

bool TerrainLodState::matches(const TerrainGridResources& resources) const {
    return resources.grid.get() == resources_.grid.get() &&
           std::ranges::equal(
               resources.blocks, resources_.blocks, [](const auto& a, const auto& b) {
                   return a.get() == b.get();
               });
}

std::optional<WorldSurfaceMesh> WorldSurfaceMesh::terrainLod(const TerrainGridResources& resources,
                                                             const TerrainLodView& view,
                                                             bool baked,
                                                             std::optional<Box2F> region,
                                                             TerrainTextureCache* cache,
                                                             TerrainLodState* state,
                                                             const TsLighting* scene) {
    using namespace collision_detail;
    gfx::ScopedRenderWorkTimer timer(&gfx::RenderWorkStats::terrainLodMs);
    auto* stats = gfx::currentRenderWorkStats;
    if (stats) {
        ++stats->terrainLodCalls;
    }
    if (!resources.grid || resources.blocks.size() != resources.grid->blocks.size() ||
        (state && !state->matches(resources))) {
        return {};
    }
    const auto& grid = *resources.grid;
    if (!std::isfinite(view.perspectiveDistance) || view.perspectiveDistance <= 0 ||
        (view.hazeDistance &&
         (!std::isfinite(*view.hazeDistance) || *view.hazeDistance < 0 || !view.nativeGeometry))) {
        return {};
    }
    if (grid.scale < 0 || grid.scale > 16 || grid.size.x <= 0 || grid.size.y <= 0 ||
        grid.size.x > 256 || grid.size.y > 256 || grid.blockMap.size() > 4096 ||
        grid.blocks.size() > 4096 || grid.blockMap.size() != UInt64(grid.size.x) * grid.size.y ||
        grid.blocks.empty()) {
        return {};
    }
    const auto first = std::ranges::find_if(resources.blocks, [](const auto& b) {
        return bool(b);
    });
    const auto n =
        first != resources.blocks.end()
            ? (*first)->width
            : (grid.detailCount >= 1 && grid.detailCount <= 11 ? 1U << (grid.detailCount - 1) : 0U);
    const auto square = Flt32(1U << grid.scale);
    if (!n || n > 1024 || !std::has_single_bit(n)) {
        return {};
    }
    const auto selection =
        terrain_detail::selectWindow(grid, n, view, region, state ? state->window_ : std::nullopt);
    if (!selection) {
        return {};
    }
    const auto geometry = view.nativeGeometry
                              ? terrainGeometryDetails(grid.scale, *view.nativeGeometry)
                              : std::nullopt;
    const auto scanLevel = selection->visibility
                               ? selection->visibility->scanLevel
                               : std::min({view.maximumLevel, UInt32(std::countr_zero(n)), 7U});
    if (scene && (!terrain_detail::validScene(*scene) || !baked || !view.compositeTextures ||
                  (view.lightMapDensity != TerrainLightMapDensity::Native &&
                   view.lightMapDensity != TerrainLightMapDensity::NativeOpenGL))) {
        return {};
    }
    if (view.compositeTextures && (!std::has_single_bit(view.texturePixels) ||
                                   view.texturePixels < 4 || view.texturePixels > 256 ||
                                   (view.textureTiming != TerrainTextureTiming::RetainedLru &&
                                    view.textureTiming != TerrainTextureTiming::NativeSoftware &&
                                    view.textureTiming != TerrainTextureTiming::NativeOpenGL) ||
                                   (view.lightMapDensity != TerrainLightMapDensity::Expanded &&
                                    view.lightMapDensity != TerrainLightMapDensity::Native &&
                                    view.lightMapDensity != TerrainLightMapDensity::NativeOpenGL) ||
                                   (cache && !cache->matches(resources)))) {
        return {};
    }
    std::optional<TerrainTextureCache> ownedCache;
    if (view.compositeTextures && !cache) {
        ownedCache.emplace(resources);
        cache = &*ownedCache;
    }

    struct CacheStats {
        gfx::RenderWorkStats* stats;
        TerrainTextureCache* cache;
        UInt64 builds, hits, evictions;

        ~CacheStats() {
            if (!stats || !cache) {
                return;
            }
            stats->terrainTileBuilds += cache->builds() - builds;
            stats->terrainTileHits += cache->hits() - hits;
            stats->terrainTileEvictions += cache->evictions() - evictions;
            stats->terrainCacheEntries += cache->size();
            stats->terrainCacheBytes += cache->residentBytes();
        }
    } cacheStats{stats,
                 cache,
                 cache ? cache->builds() : 0,
                 cache ? cache->hits() : 0,
                 cache ? cache->evictions() : 0};

    try {
        std::pmr::monotonic_buffer_resource frameMemory(32 * 1024);
        std::size_t work = MaxWork;
        std::vector<Tile> tiles;
        Edges horizontal(&frameMemory), vertical(&frameMemory);

        struct NativePoint {
            Flt32 height, distance;
        };

        std::pmr::unordered_map<std::pair<UInt32, UInt32>, NativePoint, PointHash> nativePoints(
            &frameMemory);
        if (view.nativeTraversal) {
            nativePoints.reserve(1024);
        }
        const auto introduce = [&](const Tile& t, UInt32 x, UInt32 y, UInt32 level, bool row) {
            spend(work);
            const auto key = std::pair{t.bx + x, t.by + y};
            if (nativePoints.contains(key)) {
                return true;
            }
            if (nativePoints.size() == 1'048'576) {
                return false;
            }
            const auto authored = height(t, x, y);
            const auto distance =
                m_distf(Point3F{Flt32(key.first) * square, Flt32(key.second) * square, authored},
                        view.camera);
            if (!std::isfinite(distance)) {
                return false;
            }
            const auto h =
                row ? std::optional{authored}
                    : morphTerrainHeight(*t.block, x, y, level, distance, (*geometry)[level]);
            if (!h) {
                return false;
            }
            nativePoints.emplace(key, NativePoint{*h, distance});
            horizontal.insert(key.second, key.first);
            vertical.insert(key.first, key.second);
            return true;
        };
        std::set<std::array<UInt32, 4>> nextSplit;
        const auto& clip = selection->clip;
        std::pmr::map<Int32, UInt32> blockIndices(&frameMemory);
        for (UInt32 i = 0; i < grid.blocks.size(); ++i) {
            spend(work);
            if (!blockIndices.emplace(grid.blocks[i].id, i).second) {
                return {};
            }
        }
        std::pmr::set<const TerrainGridBlock*> validated(&frameMemory);
        for (std::size_t i = 0; i < grid.blockMap.size(); ++i) {
            spend(work);
            const auto id = grid.blockMap[i];
            if (id == -1) {
                continue;
            }
            const auto found = blockIndices.find(id);
            if (found == blockIndices.end()) {
                return {};
            }
            const auto bx = UInt32(i % std::size_t(grid.size.x)) * n,
                       by = UInt32(i / std::size_t(grid.size.x)) * n;
            if (!terrain_detail::intersects(*selection, bx, by, n)) {
                continue;
            }
            const auto& handle = resources.blocks[found->second];
            if (!handle) {
                return {};
            }
            const auto& b = *handle;
            if (b.width != n || b.heights.size() != UInt64(n + 1) * (n + 1) ||
                b.materials.size() != UInt64(n) * n) {
                return {};
            }
            const auto index = [&](Flt64 v, UInt32 base, bool upper) {
                const auto at = std::clamp(v - base, 0., Flt64(n));
                return UInt32(upper ? std::ceil(at) : std::floor(at));
            };
            const auto x0 = index(clip[0], bx, false), y0 = index(clip[1], by, false),
                       x1 = index(clip[2], bx, true), y1 = index(clip[3], by, true);
            if (validated.insert(&b).second) {
                spend(work, b.heights.size());
                if (stats) {
                    stats->terrainValidatedHeights += b.heights.size();
                }
                if (std::ranges::any_of(b.heights, [](auto h) {
                        return !std::isfinite(h) || std::abs(h) > 1e12f;
                    })) {
                    return {};
                }
                if (geometry) {
                    for (UInt32 p = 0; p <= scanLevel + 1; ++p) {
                        const auto count =
                            p ? UInt64(n >> (p - 1)) * (n >> (p - 1)) : b.heights.size();
                        if (!b.pins[p].empty() && b.pins[p].size() < (count + 7) / 8) {
                            return {};
                        }
                    }
                }
            }
            const bool pinned = std::ranges::any_of(b.pins, [&](const auto& p) {
                spend(work, p.size());
                return std::ranges::any_of(p, [](auto v) {
                    return v != std::byte{};
                });
            });
            const std::function<bool(UInt32, UInt32, UInt32)> select = [&](UInt32 x,
                                                                           UInt32 y,
                                                                           UInt32 size) {
                if (stats) {
                    ++stats->terrainNodes;
                }
                spend(work);
                if (x >= x1 || y >= y1 || x + size <= x0 || y + size <= y0) {
                    return true;
                }
                const auto level = UInt32(std::countr_zero(size));
                if (selection->visibility && level <= scanLevel &&
                    !selection->visibility->tables[level].visible(Int32((bx + x) >> level),
                                                                  Int32((by + y) >> level))) {
                    return true;
                }
                const auto material = b.materials[std::size_t(y) * n + x];
                if (view.nativeTraversal && level < material.emptyLevel()) {
                    return true;
                }
                Tile tile{&b, found->second, bx, by, x, y, size};
                const std::array key{UInt32(i), x, y, size};
                const bool composite =
                    view.compositeTextures && cache->compatible(material.index, material.index);
                bool merge = x >= x0 && y >= y0 && x + size <= x1 && y + size <= y1 &&
                             (!material.hole() || view.nativeTraversal) &&
                             size <= (1U << view.maximumLevel) && (geometry || !pinned) &&
                             (!geometry || size <= (1U << scanLevel)) &&
                             (!composite || size <= std::min(128U, view.texturePixels));
                if (merge && size > 1) {
                    if (geometry && !b.pins[level + 1].empty()) {
                        const auto pin = std::size_t(y >> level) * (n >> level) + (x >> level);
                        merge = !(std::to_integer<UInt32>(b.pins[level + 1][pin >> 3]) &
                                  (1U << (pin & 7)));
                    }
                    UInt64 heightSamples{}, materialChecks{};
                    const auto materialMatches = [&](UInt32 dx, UInt32 dy) {
                        ++materialChecks;
                        const auto m = b.materials[std::size_t(y + dy) * n + x + dx];
                        return !((!view.nativeTraversal && m.hole()) ||
                                 (composite ? !cache->compatible(m.index, material.index)
                                            : (view.nativeTraversal
                                                   ? (m.flags & 7U) != (material.flags & 7U)
                                                   : m.flags != material.flags) ||
                                                  m.index != material.index));
                    };
                    if (geometry) {
                        // Native selection uses corner distances, not interior
                        // height error. Reject near patches before material work;
                        // point introduction and child visitation stay unchanged.
                        const auto at = [&](UInt32 xx, UInt32 yy) {
                            if (view.nativeTraversal) {
                                return nativePoints.at({bx + xx, by + yy}).distance;
                            }
                            return m_distf(Point3F{Flt32(bx + xx) * square,
                                                   Flt32(by + yy) * square,
                                                   height(tile, xx, yy)},
                                           view.camera);
                        };
                        auto average = at(x, y);
                        average += at(x, y + size);
                        average += at(x + size, y + size);
                        average += at(x + size, y);
                        average *= .25f;
                        if (!std::isfinite(average)) {
                            return false;
                        }
                        if (stats && average < (*geometry)[level].detailDistance) {
                            ++stats->terrainDistanceRejects;
                        }
                        merge = merge && average >= (*geometry)[level].detailDistance;
                        for (UInt32 dy = 0; dy < size && merge; ++dy) {
                            for (UInt32 dx = 0; dx < size; ++dx) {
                                spend(work);
                                if (!materialMatches(dx, dy)) {
                                    merge = false;
                                    break;
                                }
                            }
                        }
                    } else {
                        const auto a = height(tile, x, y), c = height(tile, x + size, y + size),
                                   bottom = height(tile, x + size, y),
                                   left = height(tile, x, y + size);
                        Flt64 error{};
                        Flt32 low = a, high = a;
                        for (UInt32 dy = 0; dy <= size && merge; ++dy) {
                            for (UInt32 dx = 0; dx <= size; ++dx) {
                                ++heightSamples;
                                spend(work);
                                const auto h = height(tile, x + dx, y + dy);
                                low = std::min(low, h);
                                high = std::max(high, h);
                                const auto u = Flt64(dx) / size, v = Flt64(dy) / size;
                                const auto approximate =
                                    u > v ? a + (bottom - a) * u + (c - bottom) * v
                                          : a + (c - left) * u + (left - a) * v;
                                error = std::max(error, std::abs(h - approximate));
                                if (dx < size && dy < size && !materialMatches(dx, dy)) {
                                    merge = false;
                                    break;
                                }
                            }
                        }
                        const Point3F nearest{std::clamp(view.camera.x,
                                                         Flt32(bx + x) * square,
                                                         Flt32(bx + x + size) * square),
                                              std::clamp(view.camera.y,
                                                         Flt32(by + y) * square,
                                                         Flt32(by + y + size) * square),
                                              std::clamp(view.camera.z, low, high)};
                        const auto distance = std::max(Flt64((view.camera - nearest).len()),
                                                       Flt64(view.minimumDistance));
                        // Stitch fans interpolate authored edge heights. Reserving
                        // half the error bounds their additional displacement too.
                        const auto threshold =
                            Flt64(view.pixelError) *
                            (state && state->split_.contains(key) ? 1 - Flt64(view.hysteresis) : 1);
                        merge = merge && 2 * error * view.focalLength <= threshold * distance;
                    }
                    if (stats) {
                        stats->terrainMergeHeights += heightSamples;
                        stats->terrainMaterialChecks += materialChecks;
                    }
                }
                if (size == 1 || merge) {
                    if (material.hole() && !view.nativeTraversal) {
                        return true;
                    }
                    if (material.index >= resources.materials.list.materialsPerDetail ||
                        tiles.size() == 524288) {
                        return false;
                    }
                    tiles.push_back(tile);
                    for (const auto xx : {bx + x, bx + x + size}) {
                        for (const auto yy : {by + y, by + y + size}) {
                            horizontal.insert(yy, xx);
                            vertical.insert(xx, yy);
                        }
                    }
                    return true;
                }
                if (state && !geometry) {
                    if (nextSplit.size() == 524288) {
                        return false;
                    }
                    nextSplit.insert(key);
                }
                const auto half = size / 2;
                if (view.nativeTraversal) {
                    // Original transformCross order determines ownership at
                    // discontinuous block borders and near clipped children.
                    for (const auto p : std::array{Point2I{Int32(x), Int32(y + half)},
                                                   Point2I{Int32(x + half), Int32(y + half)},
                                                   Point2I{Int32(x + size), Int32(y + half)},
                                                   Point2I{Int32(x + half), Int32(y + size)},
                                                   Point2I{Int32(x + half), Int32(y)}}) {
                        if (!introduce(tile, UInt32(p.x), UInt32(p.y), level - 1, false)) {
                            return false;
                        }
                    }
                    return select(x, y, half) && select(x + half, y, half) &&
                           select(x + half, y + half, half) && select(x, y + half, half);
                }
                if (geometry && selection->visibility && level <= scanLevel) {
                    // transformCross reaches all five introduced points before
                    // testing the children's visibility. Retain those edge
                    // midpoints even when a neighboring child is rejected.
                    for (const auto p :
                         std::array{Point2I{Int32(bx + x + half), Int32(by + y)},
                                    Point2I{Int32(bx + x), Int32(by + y + half)},
                                    Point2I{Int32(bx + x + half), Int32(by + y + half)},
                                    Point2I{Int32(bx + x + size), Int32(by + y + half)},
                                    Point2I{Int32(bx + x + half), Int32(by + y + size)}}) {
                        horizontal.insert(UInt32(p.y), UInt32(p.x));
                        vertical.insert(UInt32(p.x), UInt32(p.y));
                    }
                }
                return select(x, y, half) && select(x + half, y, half) &&
                       select(x, y + half, half) && select(x + half, y + half, half);
            };
            if (view.nativeTraversal) {
                const auto step = 1U << scanLevel;
                const Tile block{&b, found->second, bx, by, 0, 0, n};
                for (auto y = (y0 / step) * step; y < y1; y += step) {
                    UInt32 begin = (x0 / step) * step,
                           end = std::min(n, ((x1 + step - 1) / step) * step);
                    if (selection->visibility) {
                        const auto& table = selection->visibility->tables[scanLevel];
                        const auto row =
                            Int64((by + y) >> scanLevel) - table.camera.y - table.startY;
                        if (row < 0 || UInt64(row) >= table.rows.size()) {
                            continue;
                        }
                        const auto& span = table.rows[std::size_t(row)];
                        const auto lo = (Int64(span.minimum) + table.camera.x) * step - bx;
                        const auto hi = (Int64(span.maximum) + table.camera.x + 1) * step - bx;
                        begin = UInt32(std::clamp(lo, Int64(begin), Int64(end)));
                        end = UInt32(std::clamp(hi, Int64(begin), Int64(end)));
                    }
                    if (begin >= end) {
                        continue;
                    }
                    for (auto x = begin; x <= end; x += step) {
                        if (!introduce(block, x, y, scanLevel, true) ||
                            !introduce(block, x, y + step, scanLevel, true)) {
                            return {};
                        }
                    }
                    for (auto x = begin; x < end; x += step) {
                        if (!select(x, y, step)) {
                            return {};
                        }
                    }
                }
            } else if (!select(0, 0, n)) {
                return {};
            }
        }
        horizontal.prepare();
        vertical.prepare();
        WorldSurfaceMesh result;
        result.materials = resources.materials;
        using TextureKey = std::tuple<UInt32, UInt32, UInt32, UInt32, UInt32, UInt32, UInt32>;

        struct DrawTexture {
            UInt32 material;
            TerrainTextureTile tile;
        };

        std::pmr::map<TextureKey, DrawTexture> textures(&frameMemory);
        std::pmr::set<const gfx::LightMap*> countedMaps(&frameMemory);
        std::pmr::map<std::pair<UInt32, UInt32>, terrain_detail::PointLightMask> lightMasks(
            &frameMemory);
        std::size_t textureBytes{};
        UInt32 drawMaterial{};
        std::shared_ptr<const gfx::LightMap> drawMap;
        bool composite{};
        std::shared_ptr<const NativeSurfaceTexture> drawSoftware;
        Point2F mapOrigin{}, mapScale{1, 1};
        const auto emit = [&](const Tile& t, Point2I a, Point2I b, Point2I c) {
            if (result.triangles.size() == 1'048'576) {
                return false;
            }
            const auto& block = *t.block;
            const auto m = block.materials[std::size_t(t.y) * n + t.x];
            StaticSurfaceTriangle triangle;
            triangle.material = drawMaterial;
            triangle.repeatTexture = !composite;
            triangle.lightMap = drawMap;
            triangle.nativeSoftware = drawSoftware;
            triangle.nativeFarSphere =
                view.nativeTraversal && view.nativeGeometry && view.nativeGeometry->perspective;
            triangle.lit = baked && !block.lights.empty();
            const std::array points{a, b, c};
            for (std::size_t j = 0; j < 3; ++j) {
                const auto x = UInt32(points[j].x), y = UInt32(points[j].y);
                auto& v = triangle.vertices[j];
                v.position = {Flt32(t.bx + x) * square, Flt32(t.by + y) * square, height(t, x, y)};
                if (view.nativeTraversal) {
                    const auto& native = nativePoints.at({t.bx + x, t.by + y});
                    v.position.z = native.height;
                    v.farSphereDistance = native.distance;
                    if (view.hazeDistance &&
                        *view.hazeDistance < view.nativeGeometry->farDistance) {
                        v.haze =
                            std::clamp((native.distance - *view.hazeDistance) /
                                           (view.nativeGeometry->farDistance - *view.hazeDistance),
                                       0.f,
                                       1.f);
                    }
                } else if (geometry) {
                    const auto level = UInt32(std::countr_zero(x | y));
                    if (level < scanLevel) {
                        spend(work);
                        const auto morphed = morphTerrainHeight(block,
                                                                x,
                                                                y,
                                                                level,
                                                                m_distf(v.position, view.camera),
                                                                (*geometry)[level]);
                        if (!morphed) {
                            return false;
                        }
                        v.position.z = *morphed;
                    }
                }
                auto u = Flt32(x - t.x), w = Flt32(t.size) - Flt32(y - t.y);
                if (m.flags & 2U) {
                    u = Flt32(t.size) - u;
                }
                if (m.flags & 4U) {
                    w = Flt32(t.size) - w;
                }
                if (m.flags & 1U) {
                    const auto old = u;
                    u = w;
                    w = Flt32(t.size) - old;
                }
                v.texture = {u, w};
                if (composite) {
                    v.texture = {Flt32(x - t.x) / t.size, 1 - Flt32(y - t.y) / t.size};
                    v.lightMap = {mapOrigin.x + v.texture.x * mapScale.x,
                                  mapOrigin.y + v.texture.y * mapScale.y};
                }
                if (triangle.lit && !drawMap) {
                    if (block.lightScale < 0 || block.lightScale > 10) {
                        return false;
                    }
                    const auto scale = 1U << block.lightScale, side = n * scale + 1;
                    if (block.lights.size() != UInt64(side) * side) {
                        return false;
                    }
                    v.intensity = rgb(block.lights[std::size_t(y * scale) * side + x * scale]);
                }
                if (result.triangles.empty() && j == 0) {
                    result.bounds = {v.position, v.position};
                }
                result.bounds.fMin.setMin(v.position);
                result.bounds.fMax.setMax(v.position);
            }
            result.triangles.push_back(std::move(triangle));
            return true;
        };
        std::pmr::vector<Point2I> boundary(&frameMemory);
        boundary.reserve(16);
        for (const auto& t : tiles) {
            const auto blockIndex = std::ranges::find_if(resources.blocks, [&](const auto& b) {
                return b.get() == t.block;
            });
            if (blockIndex == resources.blocks.end()) {
                return {};
            }
            result.terrainSquareIds.push_back(
                {t.bx + t.x,
                 t.by + t.y,
                 t.size,
                 UInt32(grid.blocks[std::size_t(blockIndex - resources.blocks.begin())].id)});
            const auto& block = *t.block;
            drawMaterial = t.block->materials[std::size_t(t.y) * n + t.x].index;
            drawMap.reset();
            drawSoftware.reset();
            composite = view.compositeTextures && cache->compatible(drawMaterial, drawMaterial);
            if (scene && !composite) {
                return {};
            }
            if (composite) {
                const auto& bitmap = *resources.materials.textures[drawMaterial];
                auto pixels = std::min(view.texturePixels, UInt32(bitmap.width) * t.size);
                if (view.nativeTextures) {
                    const auto distance = [&](UInt32 x, UInt32 y) {
                        if (view.nativeTraversal) {
                            return nativePoints.at({t.bx + x, t.by + y}).distance;
                        }
                        return m_distf(Point3F{Flt32(t.bx + x) * square,
                                               Flt32(t.by + y) * square,
                                               height(t, x, y)},
                                       view.camera);
                    };
                    // Match getDistance's corner order and float accumulation.
                    auto average = distance(t.x, t.y);
                    average += distance(t.x, t.y + t.size);
                    average += distance(t.x + t.size, t.y + t.size);
                    average += distance(t.x + t.size, t.y);
                    average *= .25f;
                    const auto selected = selectTerrainTexture(average,
                                                               grid.scale,
                                                               UInt32(std::countr_zero(t.size)),
                                                               UInt32(bitmap.width),
                                                               *view.nativeTextures);
                    if (!selected) {
                        return {};
                    }
                    pixels = std::min(view.texturePixels, selected->pixels);
                    if (t.size == 1) {
                        // GFX's direct path clamps requests to the last authored
                        // mip. Composite builders require their exact source mip.
                        pixels = std::max(selected->pixels,
                                          UInt32(bitmap.width) >> UInt32(bitmap.detailLevels - 1));
                    }
                }
                const TextureKey key{
                    t.index, t.x, t.y, t.size, pixels, scene ? t.bx : 0U, scene ? t.by : 0U};
                auto at = textures.find(key);
                if (at == textures.end()) {
                    std::vector<terrain_detail::PointLightRegion> regions;
                    const Point3F origin{Flt32(t.bx) * square, Flt32(t.by) * square, 0};
                    if (scene) {
                        auto mask = lightMasks.find({t.bx, t.by});
                        if (mask == lightMasks.end()) {
                            auto prepared =
                                terrain_detail::pointLightMask(n, grid.scale, origin, *scene, work);
                            if (!prepared) {
                                return {};
                            }
                            mask = lightMasks.emplace(std::pair{t.bx, t.by}, std::move(*prepared))
                                       .first;
                        }
                        const auto under = mask->second.under(t.x, t.y, t.size, work);
                        if (!under) {
                            return {};
                        }
                        if (*under) {
                            auto selected = terrain_detail::pointLightRegions(
                                block, t.x, t.y, t.size, grid.scale, origin, *scene, work);
                            if (!selected) {
                                return {};
                            }
                            regions = std::move(*selected);
                        }
                    }
                    auto tile = cache->tile(t.index,
                                            t.x,
                                            t.y,
                                            t.size,
                                            pixels,
                                            baked,
                                            view.hiresLighting,
                                            view.lightMapDensity,
                                            !regions.empty(),
                                            view.nativeTraversal,
                                            view.textureTiming);
                    if (!tile) {
                        return {};
                    }
                    if (!regions.empty()) {
                        if (!tile->lightMap) {
                            return {};
                        }
                        auto merged = terrain_detail::mergePointLights(block,
                                                                       t.x,
                                                                       t.y,
                                                                       t.size,
                                                                       grid.scale,
                                                                       origin,
                                                                       *scene,
                                                                       regions,
                                                                       *tile->lightMap,
                                                                       work);
                        if (!merged) {
                            return {};
                        }
                        tile->lightMap = std::move(*merged);
                    }
                    textureBytes += tile->texture->pixels.size();
                    if (tile->lightMap && countedMaps.insert(tile->lightMap.get()).second) {
                        textureBytes += tile->lightMap->samples.size() * 2;
                    }
                    if (textureBytes > 64U * 1024U * 1024U || textures.size() == 4096) {
                        return {};
                    }
                    const auto index = UInt32(result.materials.list.materials.size());
                    TsMaterial material;
                    material.flags = 0x103;
                    result.materials.list.materials.push_back(material);
                    result.materials.textures.push_back(tile->texture);
                    at = textures.emplace(key, DrawTexture{index, std::move(*tile)}).first;
                }
                drawMaterial = at->second.material;
                drawMap = at->second.tile.lightMap;
                mapOrigin = at->second.tile.lightMapOrigin;
                mapScale = at->second.tile.lightMapScale;
                if (view.lightMapDensity == TerrainLightMapDensity::Native && drawMap) {
                    const auto& cachedBitmap = *at->second.tile.texture;
                    const auto cells = UInt32(drawMap->size.x - 1);
                    if (drawMap->size.x == drawMap->size.y && cells && std::has_single_bit(cells) &&
                        UInt32(cachedBitmap.width) >= cells &&
                        std::has_single_bit(UInt32(cachedBitmap.width) / cells)) {
                        auto native = std::make_shared<NativeSurfaceTexture>();
                        native->fixedMip = true;
                        native->settings.size = {cachedBitmap.width, cachedBitmap.height};
                        native->settings.lightScale =
                            Int32(std::countr_zero(UInt32(cachedBitmap.width) / cells));
                        if (view.nativeTraversal) {
                            auto distance = nativePoints.at({t.bx + t.x, t.by + t.y}).distance;
                            distance += nativePoints.at({t.bx + t.x, t.by + t.y + t.size}).distance;
                            distance += nativePoints.at({t.bx + t.x + t.size, t.by + t.y + t.size})
                                            .distance;
                            distance += nativePoints.at({t.bx + t.x + t.size, t.by + t.y}).distance;
                            distance *= .25f;
                            native->perspective =
                                distance < view.perspectiveDistance + Flt32(t.size) * square;
                        }
                        drawSoftware = std::move(native);
                    }
                }
            }
            const auto x = t.bx + t.x, y = t.by + t.y, s = t.size;
            boundary.clear();
            const auto edge = [&](const Edges& table,
                                  UInt32 fixed,
                                  UInt32 lo,
                                  UInt32 hi,
                                  bool reverse,
                                  bool verticalEdge) {
                const auto values = table.range(fixed, lo, hi);
                std::optional<UInt32> previous;
                const auto visit = [&](UInt32 coordinate) {
                    spend(work);
                    if (!view.nativeTraversal ||
                        ((coordinate == lo || coordinate == hi || coordinate == (lo + hi) / 2) &&
                         nativePoints.contains(verticalEdge ? std::pair{fixed, coordinate}
                                                            : std::pair{coordinate, fixed}))) {
                        // Delay one point so the last corner remains the next
                        // edge's first corner, in either traversal direction.
                        if (previous) {
                            boundary.push_back(
                                verticalEdge
                                    ? Point2I{Int32(fixed - t.bx), Int32(*previous - t.by)}
                                    : Point2I{Int32(*previous - t.bx), Int32(fixed - t.by)});
                        }
                        previous = coordinate;
                    }
                };
                if (reverse) {
                    for (auto at = values.rbegin(); at != values.rend(); ++at) {
                        visit(at->second);
                    }
                } else {
                    for (const auto& point : values) {
                        visit(point.second);
                    }
                }
            };
            edge(vertical, x, y, y + s, false, true);
            edge(horizontal, y + s, x, x + s, false, false);
            edge(vertical, x + s, y, y + s, true, true);
            edge(horizontal, y, x, x + s, true, false);
            const auto point = [&](UInt32 xx, UInt32 yy) {
                return Point2I{Int32(t.x + xx), Int32(t.y + yy)};
            };
            const std::array ring{point(0, 0),
                                  point(0, s / 2),
                                  point(0, s),
                                  point(s / 2, s),
                                  point(s, s),
                                  point(s, s / 2),
                                  point(s, 0),
                                  point(s / 2, 0)};
            if (geometry && std::ranges::all_of(boundary, [&](const auto& p) {
                    return std::ranges::find(ring, p) != ring.end();
                })) {
                // Original PolyList/FaceList triangulate the two authored
                // half-squares using only corners and reached edge midpoints.
                // Conservative material/pin refinement can introduce additional
                // edge points; those retain the general stitch fan below.
                const auto split = ((t.x / s) ^ (t.y / s)) & 1U;
                for (const auto start : split ? std::array{6U, 2U} : std::array{0U, 4U}) {
                    const auto a = ring[start], b = ring[(start + 1) & 7U],
                               c = ring[(start + 2) & 7U], d = ring[(start + 3) & 7U],
                               e = ring[(start + 4) & 7U];
                    const bool left = s > 1 && std::ranges::find(boundary, b) != boundary.end(),
                               top = s > 1 && std::ranges::find(boundary, d) != boundary.end();
                    if (left && top) {
                        if (!emit(t, a, d, e) || !emit(t, a, b, d) || !emit(t, b, c, d)) {
                            return {};
                        }
                    } else if (left) {
                        if (!emit(t, a, b, e) || !emit(t, b, c, e)) {
                            return {};
                        }
                    } else if (top) {
                        if (!emit(t, a, c, d) || !emit(t, a, d, e)) {
                            return {};
                        }
                    } else if (!emit(t, a, c, e)) {
                        return {};
                    }
                }
            } else if (boundary.size() == 4) {
                if (s == 1 && ((t.x ^ t.y) & 1U)) {
                    if (!emit(t, boundary[0], boundary[1], boundary[3]) ||
                        !emit(t, boundary[3], boundary[1], boundary[2])) {
                        return {};
                    }
                } else if (!emit(t, boundary[0], boundary[1], boundary[2]) ||
                           !emit(t, boundary[0], boundary[2], boundary[3])) {
                    return {};
                }
            } else {
                const Point2I center{Int32(t.x + s / 2), Int32(t.y + s / 2)};
                if (view.nativeTraversal) {
                    return {};
                } // strict native traversal must use the original corner/midpoint templates
                for (std::size_t i = 0; i < boundary.size(); ++i) {
                    if (!emit(t, center, boundary[i], boundary[(i + 1) % boundary.size()])) {
                        return {};
                    }
                }
            }
        }
        if (!textures.empty()) {
            result.materials.list.details = 1;
            result.materials.list.materialsPerDetail =
                Int32(result.materials.list.materials.size());
        }
        if (state) {
            state->split_.swap(nextSplit);
            state->window_ = selection->window;
        }
        return result;
    } catch (const std::length_error&) {
        return {};
    }
}
} // namespace opent1::world
