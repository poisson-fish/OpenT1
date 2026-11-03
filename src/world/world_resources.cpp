#include <opent1/gfx/render_limits.hpp>
#include <ranges>
#include <opent1/world/world_scene.hpp>
#include <opent1/world/terrain_grid.hpp>
#include <opent1/world/interior_shape.hpp>
#include <algorithm>

namespace opent1::world {
namespace {
std::string canonical(std::string_view name) {
    std::string result(name);
    std::ranges::transform(result, result.begin(), [](unsigned char c) {
        return c == '\\' ? '/' : char(c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c);
    });
    return result;
}

template <class T>
sim::SimPreLoadManager::Loaded preload(ResourceManager& manager, std::string_view name) {
    auto resource = manager.load<T>(name);
    if (!resource) {
        return {};
    }
    auto dependencies = resource->dependencies();
    return {std::make_shared<Resource<T>>(std::move(resource)), std::move(dependencies)};
}
}

bool registerWorldPreloaders(sim::SimPreLoadManager& preloader) {
    const auto shape = preloader.registerLoader(".dts", preload<TsShape>);
    const auto material = preloader.registerLoader(".dml", preload<TsMaterialList>);
    bool bitmaps = true;
    for (const auto extension : {".bmp", ".dib", ".pbm", ".png"}) {
        bitmaps = preloader.registerType<gfx::Bitmap>(extension) && bitmaps;
    }
    const auto arrays = preloader.registerType<gfx::BitmapArray>(".pba");
    const auto terrain = preloader.registerLoader(
        ".dtf",
        [](ResourceManager& manager, std::string_view name) -> sim::SimPreLoadManager::Loaded {
            auto resource = manager.load<TerrainGridFile>(name);
            if (!resource) {
                return {};
            }
            auto dependencies = resource->dependencies(name);
            return {std::make_shared<Resource<TerrainGridFile>>(std::move(resource)),
                    std::move(dependencies)};
        });
    const auto blocks = preloader.registerType<TerrainGridBlock>(".dtb");
    const auto interiors = preloader.registerLoader(
        ".dis",
        [](ResourceManager& manager, std::string_view name) -> sim::SimPreLoadManager::Loaded {
            auto resources = InteriorShapeResources::load(manager, name);
            if (!resources) {
                return {};
            }
            std::vector<std::string> dependencies{resources->shape->materialList};
            for (const auto& g : resources->geometry) {
                dependencies.emplace_back(g.name());
            }
            for (const auto& l : resources->lighting) {
                dependencies.emplace_back(l.name());
            }
            for (const auto& l : resources->missionLighting) {
                if (l) {
                    dependencies.emplace_back(l.name());
                }
            }
            return {std::make_shared<InteriorShapeResources>(std::move(*resources)),
                    std::move(dependencies)};
        });
    const auto geometry = preloader.registerType<InteriorGeometry>(".dig");
    const auto lighting = preloader.registerType<InteriorLighting>(".dil");
    return shape && material && bitmaps && arrays && terrain && blocks && interiors && geometry &&
           lighting;
}

std::optional<WorldVolumeMaterials>
WorldVolumeMaterials::load(ResourceManager& manager,
                           std::span<const sim::SimVolumetricRenderImage> images) {
    if (images.size() > gfx::renderLimits().images) {
        return {};
    }
    WorldVolumeMaterials result;
    std::size_t total{};
    for (const auto& image : images) {
        const auto& name = image.geometry.materialName;
        if (name.size() > 255 || name.find('\0') != name.npos) {
            return {};
        }
        if (name.empty()) {
            continue;
        }
        const auto key = canonical(name);
        if (std::ranges::any_of(result.entries_, [&](const auto& entry) {
                return entry.name == key;
            })) {
            continue;
        }
        if (result.entries_.size() == gfx::renderLimits().images) {
            return {};
        }
        auto list = manager.load<TsMaterialList>(key);
        if (!list || list->details < 0 || list->materialsPerDetail < 0 ||
            list->materialsPerDetail > 32768 ||
            UInt64(list->details) * UInt64(list->materialsPerDetail) != list->materials.size() ||
            list->materials.size() > gfx::renderLimits().textures - total ||
            (!list->details && list->materialsPerDetail)) {
            return {};
        }
        total += list->materials.size();
        auto resources = TsMaterialResources::load(manager, *list);
        if (!resources) {
            return {};
        }
        Entry entry{key, std::move(*resources), {}};
        for (auto i : std::views::iota(Int32{0}) | std::views::take_while([&](Int32 i) {
                          return i < list->materialsPerDetail;
                      })) {
            const auto n = std::size_t(i);
            if (list->materials[n].type() != 3 || !entry.resources.textures()[n]) {
                return {};
            }
            entry.frames.push_back(entry.resources.textures()[n].get());
        }
        result.entries_.push_back(std::move(entry));
    }
    return result;
}

std::vector<sim::VolumeMaterialFrames> WorldVolumeMaterials::frames() const {
    std::vector<sim::VolumeMaterialFrames> result;
    result.reserve(entries_.size());
    for (const auto& entry : entries_) {
        result.push_back({entry.name, entry.frames});
    }
    return result;
}
} // namespace opent1::world
