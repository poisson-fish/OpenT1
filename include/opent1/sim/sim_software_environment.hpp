#pragma once
#include <opent1/gfx/software_rasterizer.hpp>
#include <opent1/sim/sim_environment_camera.hpp>
#include <opent1/sim/sim_world_game.hpp>

namespace opent1::sim {
using SoftwareShadow = EnvironmentShadow;
// A filtered shadow's byte is black opacity, not a palette color. SoftwareEnvironment
// installs/caches this palette snapshot on demand for filtered shadows. The
// reserved -2 subpalette contains a 256x256 black-alpha table; row zero preserves
// every destination index exactly. Other rows round RGB and use Palette::nearest.
// Replaces an existing -2 entry, or appends one (maximum 16 palettes). Never
// replaces the base palette. Invalid/full palettes return nullopt unchanged.
[[nodiscard]] std::optional<gfx::Palette> softwareShadowPalette(const gfx::Palette&);

// Owned, copyable/movable software draw snapshot. Volume faces retain the
// original software fallback colors (244..246), with opaque depth writes and
// backface culling. Original software ignores material animation, UVs, haze and
// opacity even when a DML is present. Hardware textured volumes remain separate.
// Shadows draw on either winding: raw silhouettes use zero transparency;
// filtered silhouettes use the reserved -2 palette table. Receiver collection,
// occlusion and TS silhouette generation remain the caller's responsibility.
class SoftwareEnvironment {
public:
    // Clips world geometry against near/far and all viewport planes before
    // dividing by depth, interpolating shadow UVs at intersections. Finite world
    // coordinates/transform elements are bounded to 1e12. Near/far are within
    // [1/65536,65536], with near < far. Focal lengths are positive and <=16384;
    // viewport endpoints lie in [-16384,16383]. Input limits: 512 volumes,
    // 32 shadows, 100 receiver polygons/shadow, 4096 resulting polygons and
    // 65536 input vertices. Invalid input returns nullopt without publishing.
    [[nodiscard]] static std::optional<SoftwareEnvironment>
    prepare(const EnvironmentCamera&,
            std::span<const SimVolumetricRenderImage>,
            std::span<const SoftwareShadow> = {});
    // Atomic batch with any scene polygons supplied by the caller. Scene
    // polygons precede volumes and shadows; all participate in the same depth
    // resolution, so silhouettes cannot overwrite nearer opaque geometry.
    // No input is borrowed after return; the snapshot may be drawn repeatedly.
    bool draw(gfx::SoftwareSurface&, std::span<const gfx::SortedPolygon> scene = {}) const;

    [[nodiscard]] std::size_t polygonCount() const noexcept {
        return polygons_.size();
    }

private:
    struct Polygon {
        std::vector<gfx::PolygonVertex> vertices;
        std::size_t texture{}; // index in textures_, or size_t(-1) for flat fills
        UInt8 color{};
        bool alpha{};
    };

    std::vector<Polygon> polygons_;
    std::vector<gfx::Bitmap> textures_;
    bool perspective_{};
};
} // namespace opent1::sim
