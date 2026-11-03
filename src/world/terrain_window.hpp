#pragma once
#include <opent1/world/world_surfaces.hpp>
#include <ranges>
#include <cmath>
#include <bit>

namespace opent1::world::terrain_detail {
using CellWindow = std::array<Flt64, 4>;

struct Selection {
    CellWindow clip;
    std::optional<CellWindow> window;
    std::optional<TerrainVisibility> visibility;
};

inline std::optional<Selection> selectWindow(const TerrainGridFile& grid,
                                             UInt32 width,
                                             const TerrainLodView& view,
                                             std::optional<Box2F> region,
                                             const std::optional<CellWindow>& previous) {
    if (!std::isfinite(view.camera.x) || !std::isfinite(view.camera.y) ||
        !std::isfinite(view.camera.z) || !std::isfinite(view.focalLength) ||
        view.focalLength <= 0 || view.focalLength > 65536 || !std::isfinite(view.pixelError) ||
        view.pixelError < 0 || view.pixelError > 16384 || !std::isfinite(view.minimumDistance) ||
        view.minimumDistance <= 0 || view.maximumLevel > 10 || !std::isfinite(view.hysteresis) ||
        view.hysteresis < 0 || view.hysteresis >= 1 ||
        (view.window && (!view.window->radiusCells || view.window->radiusCells > 1024 ||
                         view.window->guardCells > 1024 - view.window->radiusCells)) ||
        grid.scale < 0 || grid.scale > 16 || grid.size.x <= 0 || grid.size.y <= 0 ||
        grid.size.x > 256 || grid.size.y > 256) {
        return {};
    }
    if (view.compositeTextures && view.nativeTextures &&
        !selectTerrainTexture(0, grid.scale, 0, 4, *view.nativeTextures)) {
        return {};
    }
    if (view.nativeGeometry && !terrainGeometryDetails(grid.scale, *view.nativeGeometry)) {
        return {};
    }
    if (view.nativeTraversal && !view.nativeGeometry) {
        return {};
    }
    if (region && (!std::isfinite(region->fMin.x) || !std::isfinite(region->fMin.y) ||
                   !std::isfinite(region->fMax.x) || !std::isfinite(region->fMax.y) ||
                   region->fMin.x >= region->fMax.x || region->fMin.y >= region->fMax.y)) {
        return {};
    }
    const auto square = Flt64(1U << grid.scale);
    Selection result{{0, 0, Flt64(grid.size.x) * width, Flt64(grid.size.y) * width}, {}, {}};
    auto& clip = result.clip;
    if (view.nativeVisibility) {
        if (!view.nativeGeometry || !std::has_single_bit(width)) {
            return {};
        }
        const auto details = terrainGeometryDetails(grid.scale, *view.nativeGeometry);
        const auto scan =
            terrainGeometryScanLevel(*details, view.nativeVisibility->visibleDistance);
        if (!scan) {
            return {};
        }
        result.visibility =
            terrainVisibility(grid.scale,
                              std::min({*scan, view.maximumLevel, UInt32(std::countr_zero(width))}),
                              view.camera,
                              {grid.minimumHeight, grid.maximumHeight},
                              *view.nativeVisibility);
        if (!result.visibility) {
            return {};
        }
        const auto& table = result.visibility->tables[result.visibility->scanLevel];
        if (table.rows.empty()) {
            clip = {0, 0, 0, 0};
        } else {
            const auto step = Flt64(1U << result.visibility->scanLevel);
            const auto low =
                std::ranges::min(table.rows, {}, &TerrainVisibilityRow::minimum).minimum;
            const auto high =
                std::ranges::max(table.rows, {}, &TerrainVisibilityRow::maximum).maximum;
            clip = {
                std::max(clip[0], (Flt64(low) + table.camera.x) * step),
                std::max(clip[1], (Flt64(table.startY) + table.camera.y) * step),
                std::min(clip[2], (Flt64(high) + table.camera.x + 1) * step),
                std::min(clip[3],
                         (Flt64(table.startY) + table.camera.y + Flt64(table.rows.size())) * step)};
        }
    }
    if (region) {
        clip = {std::max(clip[0], Flt64(region->fMin.x) / square),
                std::max(clip[1], Flt64(region->fMin.y) / square),
                std::min(clip[2], Flt64(region->fMax.x) / square),
                std::min(clip[3], Flt64(region->fMax.y) / square)};
    }
    if (view.window) {
        const auto x = Flt64(view.camera.x) / square, y = Flt64(view.camera.y) / square;
        const auto radius = Flt64(view.window->radiusCells),
                   extent = radius + view.window->guardCells;
        if (previous && (*previous)[2] - (*previous)[0] == 2 * extent + 1 &&
            x - radius >= (*previous)[0] && y - radius >= (*previous)[1] &&
            x + radius <= (*previous)[2] && y + radius <= (*previous)[3]) {
            result.window = previous;
        } else {
            result.window = CellWindow{std::floor(x) - extent,
                                       std::floor(y) - extent,
                                       std::floor(x) + extent + 1,
                                       std::floor(y) + extent + 1};
        }
        clip = {std::max(clip[0], (*result.window)[0]),
                std::max(clip[1], (*result.window)[1]),
                std::min(clip[2], (*result.window)[2]),
                std::min(clip[3], (*result.window)[3])};
    }
    return result;
}

inline bool intersects(const CellWindow& clip, UInt32 x, UInt32 y, UInt32 width) {
    return clip[0] < clip[2] && clip[1] < clip[3] && x < clip[2] && y < clip[3] &&
           x + width > clip[0] && y + width > clip[1];
}

inline bool intersects(const Selection& selection, UInt32 x, UInt32 y, UInt32 width) {
    if (!intersects(selection.clip, x, y, width)) {
        return false;
    }
    if (!selection.visibility) {
        return true;
    }
    const auto level = selection.visibility->scanLevel;
    const auto& table = selection.visibility->tables[level];
    const auto lo = std::max(Int64(y >> level) - table.camera.y, Int64(table.startY));
    const auto hi = std::min(Int64((y + width - 1) >> level) - table.camera.y,
                             Int64(table.startY) + Int64(table.rows.size()) - 1);
    for (auto row : std::views::iota(lo) | std::views::take_while([&](auto row) {
                        return row <= hi;
                    })) {
        const auto& span = table.rows[std::size_t(row - table.startY)];
        if (Int64(x >> level) - table.camera.x <= span.maximum &&
            Int64((x + width - 1) >> level) - table.camera.x >= span.minimum) {
            return true;
        }
    }
    return false;
}
} // namespace opent1::world::terrain_detail
