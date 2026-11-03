#include <opent1/gfx/render_limits.hpp>
#include <opent1/sim/sim_hardware_environment.hpp>
#include <opent1/core/reusable_scratch.hpp>
#include "environment_projection.hpp"
#include <algorithm>
#include <cmath>
#include <ranges>

namespace opent1::sim {
namespace {
bool same(std::string_view a, std::string_view b) {
    const auto fold = [](unsigned char c) {
        return c == '\\' ? '/' : c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c;
    };
    return std::ranges::equal(a, b, [&](auto x, auto y) {
        return fold(static_cast<unsigned char>(x)) == fold(static_cast<unsigned char>(y));
    });
}

struct Polygon {
    std::vector<gfx::PolygonVertex> vertices;
    gfx::HardwareMaterial material;
    unsigned stage{};
    double sortValue{};
    std::span<const Point2F> lightMapCoordinates;
    std::span<const Flt32> vertexOpacity;
    std::optional<gfx::HardwarePolygon> borrowed{};
};

struct EnvironmentScratch {
    std::vector<Polygon> polygons;
    std::vector<gfx::Bitmap> shadowTextures;
    std::vector<gfx::HardwarePolygon> batch;

    void clear() {
        detail::clearScratch(polygons);
        detail::clearScratch(shadowTextures);
        detail::clearScratch(batch);
    }
};
}

std::optional<gfx::HardwareBatch>
prepareHardwareEnvironment(const EnvironmentCamera& camera,
                           std::span<const SimVolumetricRenderImage> volumes,
                           std::span<const VolumeMaterialFrames> materials,
                           const gfx::Palette& palette,
                           std::span<const EnvironmentShadow> shadows,
                           std::span<const gfx::HardwarePolygon> scene,
                           Flt32 paletteGamma) {
    using environment_detail::finite;
    using environment_detail::project;
    using environment_detail::unit;
    using environment_detail::valid;
    if (!valid(camera) || !palette.valid() || !std::isfinite(paletteGamma) || paletteGamma < .05f ||
        paletteGamma > 20 || volumes.size() > gfx::renderLimits().images ||
        materials.size() > gfx::renderLimits().images ||
        shadows.size() > gfx::renderLimits().shadows ||
        scene.size() > gfx::renderLimits().polygons ||
        !std::ranges::all_of(scene, [&](const auto& p) {
            return p.material.perspective == camera.perspective &&
                   (!p.imageSortValue || unit(*p.imageSortValue)) &&
                   ((p.material.blend == gfx::HardwareBlend::Opaque &&
                     p.material.depth == gfx::HardwareDepth::TestAndWrite) ||
                    (p.material.blend != gfx::HardwareBlend::Opaque &&
                     p.material.depth == gfx::HardwareDepth::TestOnly));
        })) {
        return {};
    }
    std::size_t frameCount{};
    for (auto i : std::views::iota(std::size_t{0}, materials.size())) {
        const auto& m = materials[i];
        if (m.name.empty() || m.name.size() > 1024 || m.name.find('\0') != m.name.npos ||
            m.frames.size() > 32768 ||
            m.frames.size() > gfx::renderLimits().textures - frameCount ||
            std::ranges::any_of(m.frames,
                                [](auto* frame) {
                                    return !frame;
                                }) ||
            std::ranges::any_of(materials.subspan(i + 1), [&](const auto& other) {
                return same(m.name, other.name);
            })) {
            return {};
        }
        frameCount += m.frames.size();
    }
    detail::ScratchLease<EnvironmentScratch> workspace;
    auto& polygons = workspace.get().polygons;
    auto& shadowTextures = workspace.get().shadowTextures;
    shadowTextures.reserve(shadows.size());
    std::size_t vertices{};
    for (const auto& input : scene) {
        if (input.vertices.size() > gfx::renderLimits().vertices - vertices) {
            return {};
        }
        vertices += input.vertices.size();
    }
    const auto add = [&](const auto& input, gfx::HardwareMaterial material, unsigned stage) {
        if (input.size() > gfx::renderLimits().vertices - vertices) {
            return false;
        }
        vertices += input.size();
        auto projected =
            project(input, camera, material.twoSided, material.texture != nullptr, material.wrap);
        if (!projected) {
            return false;
        }
        if (projected->empty()) {
            return true;
        }
        if (polygons.size() == gfx::renderLimits().polygons - scene.size()) {
            return false;
        }
        double depth{};
        std::ranges::for_each(*projected, [&](const auto& v) {
            depth += camera.perspective
                         ? 1. / v.inverseDepth
                         : double(camera.nearDistance) + camera.farDistance -
                               double(v.inverseDepth) * camera.nearDistance * camera.farDistance;
        });
        depth /= double(projected->size());
        const auto sortValue = depth <= camera.nearDistance ? .99 : camera.nearDistance / depth;
        polygons.push_back({std::move(*projected), material, stage, sortValue, {}, {}});
        return true;
    };
    if (!std::ranges::all_of(volumes, [&](const auto& image) {
            const auto& geometry = image.geometry;
            if (geometry.faces.size() > 6 || !unit(geometry.opacity) ||
                geometry.materialName.size() > 1024 ||
                geometry.materialName.find('\0') != geometry.materialName.npos ||
                !finite(image.time) || image.time < 0 || !finite(image.animationScale) ||
                image.animationScale < 0) {
                return false;
            }
            const auto found = std::ranges::find_if(materials, [&](const auto& m) {
                return same(m.name, geometry.materialName);
            });
            const gfx::Bitmap* bitmap{};
            if (found != materials.end() && !found->frames.empty()) {
                bitmap = found->frames[image.frame(found->frames.size())];
            }
            UInt16 seen{};
            return std::ranges::all_of(geometry.faces, [&](const auto& face) {
                if (!face.face || face.face > 32 || (face.face & (face.face - 1)) ||
                    (seen & face.face) || face.vertices.size() > 12) {
                    return false;
                }
                seen |= face.face;
                const UInt8 index = face.face <= 2 ? 244 : face.face <= 8 ? 245 : 246;
                if (face.colorIndex != index) {
                    return false;
                }
                gfx::HardwareMaterial material;
                material.perspective = camera.perspective;
                if (bitmap) {
                    material.texture = bitmap;
                    material.wrap = true;
                    material.opacity = geometry.opacity;
                    material.linear = true;
                    material.magnificationLinear =
                        (bitmap->attributes & gfx::BitmapTransparent) == 0;
                    material.mipFilter = bitmap->detailLevels > 1 ? gfx::HardwareMipFilter::Nearest
                                                                  : gfx::HardwareMipFilter::None;
                    // Original ALPHA_CONSTANT uses GL_MODULATE: texture opacity
                    // multiplies the volume's constant alpha, even at gamma one.
                    material.alphaSource = gfx::HardwareAlphaSource::Texture;
                    if (bitmap->bitDepth == 8) {
                        material.paletteMode = gfx::HardwarePaletteMode::OpenGL;
                        material.paletteGamma = paletteGamma;
                    }
                    material.hazeSource = gfx::PolygonSource::Vertex;
                    material.blend = gfx::HardwareBlend::Alpha;
                    // SimVolumetric::BoxRenderDmlImage retains ZTEST_AND_WRITE;
                    // original OpenGL queues vertex haze until the scene flush.
                    material.depth = gfx::HardwareDepth::TestAndWrite;
                    material.deferredHaze = true;
                } else {
                    const auto& color = palette.find().colors[index];
                    material.color = {color.red / 255.f, color.green / 255.f, color.blue / 255.f};
                    if (paletteGamma != 1) {
                        for (auto i : std::views::iota(std::size_t{0}, std::size_t{3})) {
                            material.color[i] =
                                Flt32(std::pow(material.color[i], double(1.f / paletteGamma)));
                        }
                    }
                }
                return add(face.vertices, material, bitmap ? 2U : 0U);
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
            auto texture = gfx::Bitmap::create(
                Int32(bitmap.dimension), Int32(bitmap.dimension), bitmap.translucent ? 16 : 8);
            if (!texture) {
                return false;
            }
            if (bitmap.translucent) {
                texture->attributes |= gfx::BitmapAlpha88;
                texture->palette.emplace(); // black index zero
                std::ranges::for_each(std::views::iota(std::size_t{0}, bitmap.pixels.size()),
                                      [&](auto i) {
                                          texture->pixels[i * 2 + 1] = std::byte(bitmap.pixels[i]);
                                      });
            } else {
                std::ranges::transform(bitmap.pixels, texture->pixels.begin(), [](auto v) {
                    return std::byte(v);
                });
            }
            shadowTextures.push_back(std::move(*texture));
            gfx::HardwareMaterial material;
            material.texture = &shadowTextures.back();
            material.perspective = camera.perspective;
            material.twoSided = true;
            material.transparent = !bitmap.translucent;
            material.depth = gfx::HardwareDepth::TestOnly;
            material.linear = true;
            material.magnificationLinear = bitmap.translucent;
            material.alphaCutoff = bitmap.translucent ? 0.f : .65f;
            if (!bitmap.translucent && paletteGamma != 1) {
                material.paletteMode = gfx::HardwarePaletteMode::OpenGL;
                material.paletteGamma = paletteGamma;
            }
            material.blend =
                bitmap.translucent ? gfx::HardwareBlend::Alpha : gfx::HardwareBlend::Opaque;
            return std::ranges::all_of(geometry.polygons, [&](const auto& polygon) {
                return add(polygon.vertices, material, 1);
            });
        })) {
        return {};
    }
    // SimRenderImage orders complete images by increasing near/distance. Keep
    // authored polygon order within each image, including intersecting meshes.
    auto& batch = workspace.get().batch;
    batch.reserve(scene.size() + polygons.size());
    for (const auto& input : scene) {
        if (input.material.blend == gfx::HardwareBlend::Opaque) {
            batch.push_back(input);
            continue;
        }
        if (input.vertices.size() < 3 || input.vertices.size() > 65536) {
            return {};
        }
        double depth{};
        for (const auto& vertex : input.vertices) {
            if (!std::isfinite(vertex.inverseDepth) || vertex.inverseDepth <= 0) {
                return {};
            }
            depth += camera.perspective ? 1. / vertex.inverseDepth
                                        : double(camera.nearDistance) + camera.farDistance -
                                              double(vertex.inverseDepth) * camera.nearDistance *
                                                  camera.farDistance;
        }
        depth /= double(input.vertices.size());
        const auto sortValue = input.imageSortValue           ? double(*input.imageSortValue)
                               : depth <= camera.nearDistance ? .99
                                                              : camera.nearDistance / depth;
        polygons.push_back(
            {{}, input.material, 2, sortValue, input.lightMapCoordinates, input.vertexOpacity});
        // The caller owns every scene span through synchronous prepare().
        polygons.back().borrowed = input;
    }
    std::ranges::stable_sort(polygons, [](const auto& a, const auto& b) {
        if (a.stage != b.stage) {
            return a.stage < b.stage;
        }
        return a.stage == 2 && a.sortValue < b.sortValue;
    });
    std::ranges::for_each(polygons, [&](const auto& p) {
        if (p.borrowed)
            batch.push_back(*p.borrowed);
        else
            batch.push_back({p.vertices, p.material, p.lightMapCoordinates, p.vertexOpacity});
    });
    return gfx::HardwareBatch::prepare(batch, &palette);
}
} // namespace opent1::sim
