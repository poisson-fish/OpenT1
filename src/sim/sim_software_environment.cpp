#include <opent1/gfx/render_limits.hpp>
#include <opent1/sim/sim_software_environment.hpp>
#include "environment_projection.hpp"
#include <algorithm>
#include <cmath>
#include <iterator>
#include <limits>
#include <ranges>

namespace opent1::sim {
namespace {
constexpr UInt32 shadowIndex = 0xfffffffeU;
constexpr auto noTexture = std::numeric_limits<std::size_t>::max();

using environment_detail::project;
using environment_detail::unit;
using environment_detail::valid;
}

std::optional<gfx::Palette> softwareShadowPalette(const gfx::Palette& input) {
    if (!input.valid() || input.palettes.front().index == shadowIndex) {
        return {};
    }
    auto output = input;
    auto found = std::ranges::find(output.palettes, shadowIndex, &gfx::SubPalette::index);
    if (found == output.palettes.end()) {
        if (output.palettes.size() == 16) {
            return {};
        }
        output.palettes.emplace_back();
        found = std::prev(output.palettes.end());
    }
    *found = {};
    found->index = shadowIndex;
    found->type = gfx::PaletteType::Translucent;
    found->remap.resize(65536);
    std::ranges::for_each(std::views::iota(0U, 256U), [&](auto opacity) {
        std::ranges::for_each(std::views::iota(0U, 256U), [&](auto destination) {
            const auto& color = input.palettes.front().colors[destination];
            const auto channel = [opacity](UInt8 value) {
                return UInt8((UInt32(value) * (255 - opacity) + 127) / 255);
            };
            found->remap[opacity * 256 + destination] = std::byte(
                opacity == 0
                    ? UInt8(destination)
                    : input.nearest(
                          {channel(color.red), channel(color.green), channel(color.blue), 0}));
        });
    });
    return output.valid() ? std::optional(std::move(output)) : std::nullopt;
}

std::optional<SoftwareEnvironment>
SoftwareEnvironment::prepare(const EnvironmentCamera& camera,
                             std::span<const SimVolumetricRenderImage> volumes,
                             std::span<const SoftwareShadow> shadows) {
    if (!valid(camera) || volumes.size() > gfx::renderLimits().images ||
        shadows.size() > gfx::renderLimits().shadows) {
        return {};
    }
    SoftwareEnvironment result;
    result.perspective_ = camera.perspective;
    std::size_t vertices{};
    const auto add =
        [&](const auto& input, bool twoSided, std::size_t texture, UInt8 color, bool alpha) {
            if (input.size() > gfx::renderLimits().vertices - vertices) {
                return false;
            }
            vertices += input.size();
            auto projected = project(input, camera, twoSided, texture != noTexture);
            if (!projected) {
                return false;
            }
            if (projected->empty()) {
                return true;
            }
            if (result.polygons_.size() == gfx::renderLimits().polygons) {
                return false;
            }
            result.polygons_.push_back({std::move(*projected), texture, color, alpha});
            return true;
        };
    if (!std::ranges::all_of(volumes, [&](const auto& image) {
            const auto& geometry = image.geometry;
            if (geometry.faces.size() > 6 || !unit(geometry.opacity)) {
                return false;
            }
            UInt16 seen{};
            return std::ranges::all_of(geometry.faces, [&](const auto& face) {
                if (!face.face || face.face > 32 || (face.face & (face.face - 1)) ||
                    (seen & face.face) || face.vertices.size() > 12 ||
                    !std::ranges::all_of(face.vertices, [](const auto& v) {
                        return unit(v.haze);
                    })) {
                    return false;
                }
                seen |= face.face;
                const UInt8 color = face.face <= 2 ? 244 : face.face <= 8 ? 245 : 246;
                if (face.colorIndex != color) {
                    return false;
                }
                return add(face.vertices, false, noTexture, color, false);
            });
        })) {
        return {};
    }
    if (!std::ranges::all_of(shadows, [&](const auto& shadow) {
            const auto& bitmap = shadow.bitmap;
            const auto& geometry = shadow.geometry;
            if ((bitmap.dimension != 16 && bitmap.dimension != 32 && bitmap.dimension != 64) ||
                bitmap.pixels.size() != bitmap.dimension * bitmap.dimension ||
                geometry.polygons.size() > 100 || !unit(geometry.maxPlaneDot)) {
                return false;
            }
            auto texture = gfx::Bitmap::create(Int32(bitmap.dimension), Int32(bitmap.dimension));
            if (!texture) {
                return false;
            }
            texture->paletteIndex = bitmap.translucent ? shadowIndex : 0xffffffffU;
            std::ranges::transform(bitmap.pixels, texture->pixels.begin(), [](auto v) {
                return std::byte(v);
            });
            const auto textureIndex = result.textures_.size();
            result.textures_.push_back(std::move(*texture));
            return std::ranges::all_of(geometry.polygons, [&](const auto& polygon) {
                return add(polygon.vertices, true, textureIndex, 0, bitmap.translucent);
            });
        })) {
        return {};
    }
    return result;
}

bool SoftwareEnvironment::draw(gfx::SoftwareSurface& surface,
                               std::span<const gfx::SortedPolygon> scene) const {
    if (scene.size() > gfx::renderLimits().polygons - polygons_.size()) {
        return false;
    }
    if (std::ranges::any_of(polygons_, &Polygon::alpha)) {
        const auto* palette = surface.palette();
        if (!palette) {
            return false;
        }
        const auto entry =
            std::ranges::find(palette->palettes, shadowIndex, &gfx::SubPalette::index);
        if (entry == palette->palettes.end() || entry->type != gfx::PaletteType::Translucent ||
            entry->remap.size() != 65536) {
            return false;
        }
    }
    std::vector<gfx::SortedPolygon> batch(scene.begin(), scene.end());
    batch.reserve(scene.size() + polygons_.size());
    std::ranges::for_each(polygons_, [&](const auto& polygon) {
        gfx::PolygonMaterial material;
        material.color = polygon.color;
        if (polygon.texture != noTexture) {
            material.texture = &textures_[polygon.texture];
            material.perspective = perspective_;
            material.blend = polygon.alpha;
            material.transparent = !polygon.alpha;
            material.clampTexture = polygon.alpha;
        }
        batch.push_back({polygon.vertices, material});
    });
    return surface.sortedPolygons(batch);
}
} // namespace opent1::sim
