#include <opent1/game/mine.hpp>
#include <iterator>
#include <ranges>
#include <opent1/gfx/bitmap_content.hpp>
#include <opent1/core/text.hpp>
#include <opent1/core/filename.hpp>
#include <opent1/sim/bayes_ai.hpp>
#include <opent1/game/flier.hpp>
#include <opent1/game/ai.hpp>
#include <opent1/game/chat_inventory_huds.hpp>
#include <opent1/game/chat_menu.hpp>
#include <opent1/game/commander_huds.hpp>
#include <opent1/game/debris.hpp>
#include <opent1/game/decal_manager.hpp>
#include <opent1/game/drop_points.hpp>
#include <opent1/game/edit_camera.hpp>
#include <opent1/game/explosion.hpp>
#include <opent1/game/f_base.hpp>
#include <opent1/game/fear_mission_editor.hpp>
#include <opent1/game/mission_editor_controls.hpp>
#include <opent1/game/fear_player_psc.hpp>
#include <opent1/game/fear_plugin.hpp>
#include <opent1/game/flying_camera.hpp>
#include <opent1/game/game_app.hpp>
#include <opent1/sim/sim_native_media.hpp>
#include <opent1/sim/sim_resource_volume.hpp>
#include <opent1/game/game_base.hpp>
#include <opent1/game/hud_controls.hpp>
#include <opent1/game/interior_shape.hpp>
#include <opent1/game/item.hpp>
#include <opent1/game/marker.hpp>
#include <opent1/game/moveable.hpp>
#include <opent1/game/observer_camera.hpp>
#include <opent1/game/part_debris.hpp>
#include <opent1/game/planet.hpp>
#include <opent1/game/player.hpp>
#include <opent1/game/player_manager.hpp>
#include <opent1/game/proj_bullet.hpp>
#include <opent1/game/proj_grenade.hpp>
#include <opent1/game/proj_laser.hpp>
#include <opent1/game/proj_lightning.hpp>
#include <opent1/game/proj_repair.hpp>
#include <opent1/game/proj_rocket.hpp>
#include <opent1/game/proj_seek_missile.hpp>
#include <opent1/game/proj_target_laser.hpp>
#include <opent1/game/projectile_factory.hpp>
#include <opent1/game/sensor.hpp>
#include <opent1/game/sensor_manager.hpp>
#include <opent1/game/shape_base.hpp>
#include <opent1/game/shell_dialogs.hpp>
#include <opent1/game/server_irc_controls.hpp>
#include <opent1/game/sky.hpp>
#include <opent1/game/snowfall.hpp>
#include <opent1/game/smoke_puff.hpp>
#include <opent1/game/sound.hpp>
#include <opent1/game/star_field.hpp>
#include <opent1/game/static_base.hpp>
#include <opent1/game/status_combat_huds.hpp>
#include <opent1/game/team_group.hpp>
#include <opent1/game/trigger.hpp>
#include <opent1/game/turret.hpp>
#include <opent1/game/vehicle.hpp>
#include <opent1/game/version.hpp>
#include <opent1/gfx/bitmap_palette_font.hpp>
#include <opent1/gfx/palette.hpp>
#include <opent1/gui/ts_control.hpp>
#include <opent1/sim/sim_input_action.hpp>
#include <opent1/sim/sim_palette.hpp>
#include <opent1/world/interior_shape.hpp>
#include <opent1/world/scene_objects.hpp>
#include <opent1/world/terrain_grid.hpp>
#include <opent1/world/terrain_editor.hpp>
#include <opent1/world/ts_shape.hpp>
#include <opent1/world/world_mission.hpp>
#include <opent1/world/world_scene.hpp>
#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cstring>
#include <cmath>
#include <numbers>
#include <format>
#include <fstream>
#include <sstream>
#include <functional>
#include <iostream>
#include <vector>

namespace opent1::game {

namespace {

template <class T> T* findBoundControl(gui::Control* root) {
    if (!root) {
        return nullptr;
    }
    if (auto* match = dynamic_cast<T*>(root)) {
        return match;
    }
    for (auto* object : root->objects()) {
        if (auto* match = findBoundControl<T>(dynamic_cast<gui::Control*>(object))) {
            return match;
        }
    }
    return nullptr;
}

[[nodiscard]] Int32 parseIntSv(std::string_view sv) noexcept {
    while (!sv.empty() && std::isspace(static_cast<unsigned char>(sv.front())) != 0) {
        sv.remove_prefix(1);
    }
    Int32 out = 0;
    (void)std::from_chars(sv.data(), sv.data() + sv.size(), out);
    return out;
}

[[nodiscard]] Flt32 parseFloatSv(std::string_view sv) noexcept {
    while (!sv.empty() && std::isspace(static_cast<unsigned char>(sv.front())) != 0) {
        sv.remove_prefix(1);
    }
    Flt32 out = 0.0f;
    (void)std::from_chars(sv.data(), sv.data() + sv.size(), out);
    return out;
}

[[nodiscard]] Point3F parsePoint3FSv(std::string_view text) noexcept {
    Point3F pt{0.0f, 0.0f, 0.0f};
    std::array<Flt32*, 3> coords{&pt.x, &pt.y, &pt.z};
    for (Flt32* dst : coords) {
        while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front())) != 0) {
            text.remove_prefix(1);
        }
        if (text.empty()) {
            break;
        }
        const auto [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), *dst);
        if (ec != std::errc{}) {
            break;
        }
        text.remove_prefix(static_cast<std::size_t>(ptr - text.data()));
    }
    return pt;
}

[[nodiscard]] bool parseBoolSv(std::string_view text) noexcept {
    if (text == "true" || text == "True" || text == "TRUE" || text == "1") {
        return true;
    }
    if (text == "false" || text == "False" || text == "FALSE" || text == "0") {
        return false;
    }
    return parseIntSv(text) != 0;
}

void collectGroupObjectsRecursive(sim::SimGroup& group, std::vector<sim::SimObject*>& out) {
    for (sim::SimObject* child : group.objects()) {
        if (!child || child->isDeleted() || child->isRemoved()) {
            continue;
        }
        out.push_back(child);
        if (auto* subGroup = dynamic_cast<sim::SimGroup*>(child)) {
            collectGroupObjectsRecursive(*subGroup, out);
        }
    }
}

void addLocalPlayer(PlayerManager& players, Console& console, Int32 clientId) {
    const auto preference = [&](std::string_view name, std::string_view fallback) {
        const auto value = console.getVariable(name);
        return value.empty() ? std::string(fallback) : value;
    };
    players.clientAdded(preference("PCFG::Name", "PlayerOne"),
                        preference("PCFG::Voice", "male1"),
                        preference("PCFG::SkinBase", "base"),
                        !equalIgnoreCase(preference("PCFG::Gender", "Male"), "Female"),
                        clientId);
}

[[nodiscard]] sim::SimObject* resolveSimObject(sim::SimManager* mgr,
                                               std::string_view nameOrId) noexcept {
    if (!mgr || nameOrId.empty()) {
        return nullptr;
    }
    const auto clientId = parseIntSv(nameOrId);
    if (auto* players = dynamic_cast<PlayerManager*>(mgr->findObject(PlayerManagerId))) {
        if (const auto* client = players->findBaseRep(clientId); client && client->ownedObject) {
            return client->ownedObject;
        }
    }
    if (auto* byName = mgr->findObject(nameOrId)) {
        return byName;
    }
    const auto id = static_cast<sim::SimObjectId>(parseIntSv(nameOrId));
    if (id != 0) {
        if (auto* byId = mgr->findObject(id)) {
            return byId;
        }
    }
    std::vector<sim::SimObject*> allObjs;
    collectGroupObjectsRecursive(*mgr, allObjs);
    for (sim::SimObject* obj : allObjs) {
        if (obj && equalIgnoreCase(obj->getName(), nameOrId)) {
            return obj;
        }
    }
    return nullptr;
}

[[nodiscard]] Int32 resolveItemTypeIndex(const DataBlockManager* dbm,
                                         std::string_view itemOrId) noexcept {
    if (itemOrId.empty()) {
        return -1;
    }
    if (dbm) {
        Int32 idx = -1;
        if (dbm->lookupDataBlock(itemOrId, DataBlockManager::ItemDataType, &idx)) {
            return idx;
        }
    }
    return parseIntSv(itemOrId);
}

void ensureNamedSet(sim::SimManager& mgr, sim::SimObjectId id, std::string_view name = {}) {
    if (mgr.findObject(id)) {
        return;
    }
    auto set = std::make_unique<sim::SimSet>();
    set->setId(id);
    if (!name.empty()) {
        set->assignName(name);
    }
    mgr.addObject(std::move(set));
}

class FearSimPalette final : public sim::SimPalette {
public:
    using PaletteLoadedCallback = std::function<void(std::shared_ptr<gfx::Palette>)>;

    FearSimPalette(std::shared_ptr<ResourceManager> rm, PaletteLoadedCallback cb = {})
        : rm_(std::move(rm)), onPaletteLoaded_(std::move(cb)) {}

    bool onAdd() override {
        if (!sim::SimPalette::onAdd()) {
            return false;
        }
        if (const auto fn = getFieldValue("fileName"); fn && !fn->empty()) {
            if (open(*fn) && getPalette()) {
                if (onPaletteLoaded_) {
                    onPaletteLoaded_(std::make_shared<gfx::Palette>(*getPalette()));
                }
            } else if (rm_) {
                if (auto pal = rm_->load<gfx::Palette>(*fn)) {
                    if (onPaletteLoaded_) {
                        onPaletteLoaded_(std::make_shared<gfx::Palette>(*pal));
                    }
                }
            }
        }
        return true;
    }

private:
    std::shared_ptr<ResourceManager> rm_{};
    PaletteLoadedCallback onPaletteLoaded_{};
};

Resource<gfx::Palette> toResourcePalette(const std::shared_ptr<gfx::Palette>& pal) {
    if (!pal) {
        return {};
    }
    ResourceManager rm;
    (void)rm.add<gfx::Palette>("active_palette.ppl", std::make_unique<gfx::Palette>(*pal));
    return rm.load<gfx::Palette>("active_palette.ppl");
}

bool renderSkyPass(const Sky& sky,
                   gfx::SoftwareSurface& surface,
                   const sim::EnvironmentCamera& cam,
                   int pass,
                   const gfx::Palette* pal,
                   gfx::HardwareBatch* hardware = nullptr,
                   Flt32 paletteGamma = 1,
                   gfx::HardwareBatch* bands = nullptr) {
    if (pass != 0) {
        return true;
    }
    gfx::ScopedRenderWorkTimer timer(&gfx::RenderWorkStats::skyMs);
    const Int32 w = surface.bitmap().width;
    const Int32 h = surface.bitmap().height;
    if (w <= 0 || h <= 0) {
        return false;
    }
    const ColorF sc = sky.getSkyColor();
    const ColorF hc = sky.getHazeColor();
    UInt8 topIdx = 148;
    UInt8 midIdx = 152;
    UInt8 horizonIdx = 160;
    if (pal && !pal->palettes.empty()) {
        topIdx = pal->nearest(
            gfx::PaletteColor{static_cast<UInt8>(std::clamp(sc.red * 255.0f, 0.0f, 255.0f)),
                              static_cast<UInt8>(std::clamp(sc.green * 255.0f, 0.0f, 255.0f)),
                              static_cast<UInt8>(std::clamp(sc.blue * 255.0f, 0.0f, 255.0f)),
                              0});
        horizonIdx = pal->nearest(
            gfx::PaletteColor{static_cast<UInt8>(std::clamp(hc.red * 255.0f, 0.0f, 255.0f)),
                              static_cast<UInt8>(std::clamp(hc.green * 255.0f, 0.0f, 255.0f)),
                              static_cast<UInt8>(std::clamp(hc.blue * 255.0f, 0.0f, 255.0f)),
                              0});
        midIdx = pal->nearest(gfx::PaletteColor{
            static_cast<UInt8>(std::clamp((sc.red + hc.red) * 127.5f, 0.0f, 255.0f)),
            static_cast<UInt8>(std::clamp((sc.green + hc.green) * 127.5f, 0.0f, 255.0f)),
            static_cast<UInt8>(std::clamp((sc.blue + hc.blue) * 127.5f, 0.0f, 255.0f)),
            0});
    }
    const auto* skyMaterials = sky.materials();
    if (skyMaterials && pal && !skyMaterials->textures().empty() && skyMaterials->textures().front()) {
        const auto& texture = *skyMaterials->textures().front();
        if (texture.bitDepth == 8 && !texture.level().empty()) {
            const auto& sub = pal->find(texture.paletteIndex);
            const auto remap = [&](std::byte pixel) {
                const auto index = std::to_integer<UInt8>(pixel);
                return sub.type == gfx::PaletteType::ShadeHaze
                           ? std::to_integer<UInt8>(
                                 sub.remap[((std::size_t{1} << pal->shadeShift) - 1) * 256 + index])
                           : sub.identity[index];
            };
            topIdx = remap(texture.row(0).front());
            horizonIdx = remap(texture.row(texture.height - 1).front());
            midIdx = topIdx;
        }
    }
    Point3F horizonCam{};
    m_mul(Point3F{0.0f, 1000.0f, 0.0f}, static_cast<const RMat3F&>(cam.worldToCamera), horizonCam);
    Int32 horizonY = h / 2;
    if (horizonCam.y > 1.0f) {
        horizonY = std::clamp(
            static_cast<Int32>(cam.center.y - (horizonCam.z / horizonCam.y) * cam.focal.y),
            0,
            h - 1);
    }
    const Int32 band1 = std::max(1, horizonY / 2);
    if (bands && pal && surface.clip() == surface.bitmap().bounds()) {
        // Match the indexed OpenGL texture expansion, including its byte
        // truncation after gamma. Continuous alpha-fill gamma is different.
        if (!pal->valid()) {
            return false;
        }
        const auto& colors = pal->find(surface.bitmap().paletteIndex).colors;
        const auto channel = [&](UInt8 value) {
            const auto corrected =
                UInt8(std::pow(Flt32(value) / 255.f, double(1.f / paletteGamma)) * 255.);
            return Flt32(corrected) / 255.f;
        };
        std::array<std::array<gfx::PolygonVertex, 4>, 3> vertices;
        std::vector<gfx::HardwarePolygon> polygons;
        polygons.reserve(vertices.size());
        const auto add = [&](RectI rect, UInt8 index) {
            const auto clip = gfx::intersect(rect, surface.bitmap().bounds());
            if (!clip) {
                return;
            }
            auto& quad = vertices[polygons.size()];
            quad = {{{{Flt32(clip->left), Flt32(clip->top)}, {0, 0}},
                     {{Flt32(clip->right + 1), Flt32(clip->top)}, {1, 0}},
                     {{Flt32(clip->right + 1), Flt32(clip->bottom + 1)}, {1, 1}},
                     {{Flt32(clip->left), Flt32(clip->bottom + 1)}, {0, 1}}}};
            const auto& color = colors[index];
            gfx::HardwareMaterial material;
            material.color = {channel(color.red), channel(color.green), channel(color.blue)};
            material.depth = gfx::HardwareDepth::Disabled;
            material.perspective = false;
            material.alphaSource = gfx::HardwareAlphaSource::Opaque;
            polygons.push_back({quad, material});
        };
        add({0, 0, w - 1, band1}, topIdx);
        if (horizonY > band1) {
            add({0, band1, w - 1, horizonY}, midIdx);
        }
        if (horizonY < h - 1) {
            add({0, horizonY, w - 1, h - 1}, horizonIdx);
        }
        auto prepared = gfx::HardwareBatch::prepare(polygons);
        if (!prepared) {
            return false;
        }
        *bands = std::move(*prepared);
    } else {
        if (band1 > 0) {
            surface.fill(RectI{0, 0, w - 1, band1}, topIdx);
        }
        if (horizonY > band1) {
            surface.fill(RectI{0, band1, w - 1, horizonY}, midIdx);
        }
        if (horizonY < h - 1) {
            surface.fill(RectI{0, horizonY, w - 1, h - 1}, horizonIdx);
        }
    }
    if (skyMaterials && !skyMaterials->textures().empty()) {
        std::vector<world::StaticSurfaceTriangle> triangles;
        // Original Sky::calcPoints/renderSkyBand: sixteen 22.5 degree panels,
        // camera-centered, from -size/8 to 3*size/4 above the horizon.
        const float distance = cam.farDistance * 1.5f;
        for (auto i : std::views::iota(std::size_t{0}) | std::views::take_while([&](std::size_t i) {
                          return i < Sky::TEXTURES;
                      })) {
            const auto angle = [&](std::size_t j) {
                return (90.f + sky.featurePosition() - 22.5f - float(j) * 22.5f) *
                       std::numbers::pi_v<float> / 180.f;
            };
            const auto a = angle(i), b = angle(i + 1);
            const std::array<Point3F, 4> points{
                {{std::cos(a) * distance, std::sin(a) * distance, -sky.size() * .125f},
                 {std::cos(a) * distance, std::sin(a) * distance, sky.size() * .75f},
                 {std::cos(b) * distance, std::sin(b) * distance, sky.size() * .75f},
                 {std::cos(b) * distance, std::sin(b) * distance, -sky.size() * .125f}}};
            constexpr std::array<Point2F, 4> uv{{{0, 1}, {0, 0}, {1, 0}, {1, 1}}};
            for (const auto ids : {std::array{0, 1, 2}, std::array{0, 2, 3}}) {
                world::StaticSurfaceTriangle triangle;
                triangle.material = std::size_t(sky.textures()[i]) % skyMaterials->textures().size();
                for (auto j : std::views::iota(std::size_t{0}, std::size_t{3})) {
                    triangle.vertices[j].position = points[ids[j]];
                    triangle.vertices[j].texture = uv[ids[j]];
                }
                triangles.push_back(std::move(triangle));
            }
        }
        world::TsStaticView view;
        view.width = w;
        view.height = h;
        view.focal = cam.focal;
        view.center = cam.center;
        view.shapeToCamera = cam.worldToCamera;
        view.shapeToCamera.p = {};
        view.shapeToCamera.flags = RMat3F::Matrix_HasRotation;
        view.farDistance = 5000;
        view.twoSided = true;
        if (const auto skyFrame =
                world::TsStaticShapeFrame::prepareSurfaces(*skyMaterials, triangles, view)) {
            if (hardware) {
                auto polygons = skyFrame->hardwarePolygons(pal, paletteGamma);
                if (!polygons) {
                    return false;
                }
                // Environment images precede the world and must never seed its depth buffer.
                for (auto& polygon : *polygons) {
                    polygon.material.depth = gfx::HardwareDepth::Disabled;
                }
                auto batch = gfx::HardwareBatch::prepare(*polygons, pal);
                if (!batch) {
                    return false;
                }
                if (auto* stats = gfx::currentRenderWorkStats) {
                    stats->hardwareSkyPolygons += polygons->size();
                }
                *hardware = std::move(*batch);
            } else {
                (void)skyFrame->draw(surface);
            }
        }
    }
    return true;
}

void renderStarFieldPass(const StarField& stars,
                         gfx::SoftwareSurface& surface,
                         const sim::EnvironmentCamera& cam,
                         int pass,
                         const gfx::Palette* pal) {
    if (!stars.isVisible()) {
        return;
    }
    const int targetPass = stars.isInFrontOfSky() ? 2 : 0;
    if (pass != targetPass) {
        return;
    }
    const Int32 w = surface.bitmap().width;
    const Int32 h = surface.bitmap().height;
    std::array<UInt8, 3> cols{254, 248, 240};
    if (pal && !pal->palettes.empty()) {
        const auto& sc = stars.colors();
        for (auto i : std::views::iota(std::size_t{0}, std::size_t{3})) {
            cols[i] = pal->nearest(gfx::PaletteColor{
                static_cast<UInt8>(std::clamp(sc[i].red * 255.0f, 0.0f, 255.0f)),
                static_cast<UInt8>(std::clamp(sc[i].green * 255.0f, 0.0f, 255.0f)),
                static_cast<UInt8>(std::clamp(sc[i].blue * 255.0f, 0.0f, 255.0f)),
                0});
        }
    }
    const RMat3F& rotOnly = cam.worldToCamera;
    for (const auto& reg : stars.regions()) {
        for (const auto& star : reg.stars) {
            Point3F cp{};
            m_mul(star.pos, rotOnly, cp);
            if (cp.y <= 1.0f) {
                continue;
            }
            const Int32 sx = static_cast<Int32>(cam.center.x + (cp.x / cp.y) * cam.focal.x);
            const Int32 sy = static_cast<Int32>(cam.center.y - (cp.z / cp.y) * cam.focal.y);
            if (sx >= 0 && sx < w && sy >= 0 && sy < h) {
                const std::size_t cIdx =
                    (star.brightness > 0.66f) ? 0 : (star.brightness > 0.33f ? 1 : 2);
                surface.point(Point2I{sx, sy}, cols[cIdx]);
            }
        }
    }
}

bool renderPlanetPass(const Planet& planet,
                       gfx::SoftwareSurface& surface,
                       const sim::EnvironmentCamera& cam,
                       int pass,
                       ResourceManager& resources) {
    if (pass != 1 && pass != 3) {
        return true;
    }
    const auto& img = planet.planetImage();
    // Missions such as RPGMap5 use an untextured Planet only for lighting.
    // Native Planet::load never adds an absent/failed bitmap to the render set.
    if (img.fileName.empty()) {
        return true;
    }
    const auto texture = resources.load<gfx::Bitmap>(img.fileName);
    if (!texture) {
        return true;
    }
    Point3F cp{};
    m_mul(img.position, static_cast<const RMat3F&>(cam.worldToCamera), cp);
    if (cp.y <= cam.nearDistance || !std::isfinite(img.size) || img.size <= 0) {
        return true;
    }

    // Native planets face the view plane with their top toward celestial north.
    // Work in camera space so translation cannot move a distant sun or moon.
    Point3F north{}, normal{}, up{}, right{};
    m_mul(Point3F{0, 0, 1}, static_cast<const RMat3F&>(cam.worldToCamera), north);
    m_cross(cp, north, normal);
    m_cross(normal, Point3F{0, 1, 0}, up);
    if (up.lenf() < .00001f) {
        return true;
    }
    up.normalize();
    m_cross(Point3F{0, 1, 0}, up, right);
    right *= img.size;
    up *= img.size;
    const std::array<Point3F, 4> points{
        cp - right + up, cp + right + up, cp + right - up, cp - right - up};
    constexpr std::array<Point2F, 4> uv{{{0, 0}, {1, 0}, {1, 1}, {0, 1}}};
    std::array<world::StaticSurfaceTriangle, 2> triangles;
    constexpr std::array indices{std::array{0, 1, 2}, std::array{0, 2, 3}};
    for (std::size_t i = 0; i < triangles.size(); ++i) {
        for (std::size_t j = 0; j < 3; ++j) {
            triangles[i].vertices[j].position = points[indices[i][j]];
            triangles[i].vertices[j].texture = uv[indices[i][j]];
        }
    }
    world::TsMaterialList list;
    list.details = list.materialsPerDetail = 1;
    world::TsMaterial material;
    material.flags = 3 | ((texture->attributes & gfx::BitmapTransparent) ? 0x1000U : 0U);
    list.materials.push_back(material);
    world::TsStaticView view;
    view.width = surface.bitmap().width;
    view.height = surface.bitmap().height;
    view.focal = cam.focal;
    view.center = cam.center;
    view.nearDistance = cam.nearDistance;
    view.farDistance = 65536;
    view.shapeToCamera.identity();
    view.texturePerspective = false;
    const auto frame = world::TsStaticShapeFrame::prepareSurfaces(
        world::TsMaterialResources{std::move(list), {texture}}, triangles, view);
    return frame && frame->draw(surface);
}

struct WeatherGeometry {
    std::deque<std::array<gfx::PolygonVertex, 4>> vertices;
    std::vector<gfx::SortedPolygon> software;
    std::vector<gfx::HardwarePolygon> hardware;

    void append(const Snowfall& snow,
                const sim::EnvironmentCamera& camera,
                const gfx::Palette& palette,
                Flt32 gamma) {
        if (snow.isRenderingSuspended()) {
            return;
        }
        const auto color = snow.isRain() ? gfx::PaletteColor{185, 205, 235, 0}
                                         : gfx::PaletteColor{245, 248, 255, 0};
        gfx::PolygonMaterial sw;
        sw.color = palette.nearest(color);
        gfx::HardwareMaterial hw;
        hw.color = {color.red / 255.f, color.green / 255.f, color.blue / 255.f};
        if (gamma != 1) {
            for (auto channel : std::views::iota(std::size_t{0}, std::size_t{3})) {
                hw.color[channel] = Flt32(std::pow(hw.color[channel], double(1.f / gamma)));
            }
        }
        hw.depth = gfx::HardwareDepth::TestOnly;
        hw.blend = gfx::HardwareBlend::Alpha;
        hw.opacity = .65f;
        // Deque storage keeps spans stable if the mission has multiple emitters.
        const auto first = vertices.size();
        for (auto i : std::views::iota(Int32{0}) | std::views::take_while([&](Int32 i) {
                          return i < std::min(snow.numFlakes(), 512);
                      })) {
            const auto& flake = snow.flakes()[i];
            if (!(flake.flags & Snowfall::FlakeActive)) {
                continue;
            }
            Point3F view;
            m_mul(flake.pos, camera.worldToCamera, view);
            if (view.y <= camera.nearDistance || view.y >= camera.farDistance) {
                continue;
            }
            const auto x = camera.center.x + view.x / view.y * camera.focal.x;
            const auto y = camera.center.y - view.z / view.y * camera.focal.y;
            const auto length =
                snow.isRain() ? std::clamp(camera.focal.y * .4f / view.y, 2.f, 10.f) : 1.5f;
            const auto left = std::max(x, Flt32(camera.viewport.left));
            const auto right = std::min(x + 1.f, Flt32(camera.viewport.right + 1));
            const auto top = std::max(y, Flt32(camera.viewport.top));
            const auto bottom = std::min(y + length, Flt32(camera.viewport.bottom + 1));
            if (left >= right || top >= bottom) {
                continue;
            }
            std::array<gfx::PolygonVertex, 4> quad;
            quad[0].position = {left, top};
            quad[1].position = {right, top};
            quad[2].position = {right, bottom};
            quad[3].position = {left, bottom};
            for (auto& vertex : quad) {
                vertex.inverseDepth = 1.f / view.y;
            }
            vertices.push_back(quad);
        }
        for (auto i : std::views::iota(static_cast<std::size_t>(first)) |
                          std::views::take_while([&](std::size_t i) {
                              return i < vertices.size();
                          })) {
            software.push_back({vertices[i], sw});
            hardware.push_back({vertices[i], hw});
        }
    }
};

class EmfInterior final : public InteriorShape {
public:
    bool prepareArguments(std::span<const std::string> args) override {
        if (args.size() != 1 || args[0].empty()) {
            return false;
        }
        // createObject registers before processArguments. Set the filename
        // before onAdd so geometry is actually loaded and validated.
        setFileName(args[0]);
        return true;
    }
};

class FearMissionPath final : public world::SimPath {
public:
    FearMissionPath() {
        world::PathState state;
        state.compressed = false;
        (void)setState(std::move(state));
    }

    bool onAdd() override {
        if (!world::SimPath::onAdd()) {
            return false;
        }
        auto next = state();
        if (auto value = getFieldValue("isLooping")) {
            next.looping = equalIgnoreCase(*value, "True") || *value == "1";
        }
        return setState(std::move(next));
    }
};

class FearMissionTerrain final : public world::SimMissionTerrain {
public:
    bool onAdd() override {
        auto next = state();
        if (const auto value = getFieldValue("tedFileName"); value && !value->empty()) {
            next.filename = *value;
        }
        const auto scalar = [&](std::string_view field, Flt32& target) {
            if (const auto value = getFieldValue(field); value && !value->empty()) {
                target = parseFloatSv(*value);
            }
        };
        scalar("visibleDistance", next.visibleDistance);
        scalar("hazeDistance", next.hazeDistance);
        scalar("hazeVerticalMin", next.hazeMin);
        scalar("hazeVerticalMax", next.hazeMax);
        // Vertical haze was disabled in the native terrain renderer. Exported
        // RPG missions contain uninitialized NaNs in these dormant fields.
        // Keep finite defaults rather than passing them into validated state.
        if (!std::isfinite(next.hazeMin)) {
            next.hazeMin = state().hazeMin;
        }
        if (!std::isfinite(next.hazeMax)) {
            next.hazeMax = state().hazeMax;
        }
        scalar("perspectiveDistance", next.perspectiveDistance);
        scalar("screenSize", next.screenSize);
        scalar("contDrag", next.drag);
        scalar("contHeight", next.height);
        for (auto entry : {std::pair{"position", &next.position},
                           {"rotation", &next.rotation},
                           {"contGravity", &next.gravity}}) {
            if (const auto value = getFieldValue(entry.first); value && !value->empty()) {
                *entry.second = parsePoint3FSv(*value);
            }
        }
        return setState(next) && world::SimMissionTerrain::onAdd();
    }
};

} // namespace

// -----------------------------------------------------------------------------
// TimeDemoPlugin
// -----------------------------------------------------------------------------

void TimeDemoPlugin::init(Console& console, sim::SimManager* /*manager*/) {
    console_ = &console;
}

void TimeDemoPlugin::shutdown() noexcept {
    active_ = false;
    console_ = nullptr;
}

void TimeDemoPlugin::startFrame() {}

void TimeDemoPlugin::endFrame() {
    if (active_) {
        ++frames_;
    }
}

void TimeDemoPlugin::beginTimeDemo(std::string demoFile) {
    demoFile_ = std::move(demoFile);
    frames_ = 0;
    active_ = true;
}

void TimeDemoPlugin::finishTimeDemo() {
    if (active_ && console_) {
        console_->setVariable("TimeDemo::Frames", std::to_string(frames_));
    }
    active_ = false;
}

// -----------------------------------------------------------------------------
// FearPlugin
// -----------------------------------------------------------------------------

FearPlugin::FearPlugin(sim::SimGame& game,
                       std::shared_ptr<ResourceManager> resources,
                       std::shared_ptr<BanList> banList,
                       std::shared_ptr<FearDynamicDataPlugin> dynamicData,
                       std::filesystem::path writeRoot,
                       std::shared_ptr<ScriptPlugin> scriptPlugin)
    : game_(game),
      resources_(resources ? std::move(resources) : std::make_shared<ResourceManager>()),
      banList_(banList ? std::move(banList) : std::make_shared<BanList>()),
      dynamicData_(std::move(dynamicData)), scriptPlugin_(std::move(scriptPlugin)),
      netRegistry_(std::make_shared<sim::network::Registry>()), writeRoot_(std::move(writeRoot)) {
    sim::network::registerConsoleEvent(*netRegistry_);
    sim::network::registerRemoteCreateEvent(*netRegistry_);
    sim::network::registerServiceGhosts(*netRegistry_);
    world::registerSceneObjectNetwork(*netRegistry_);
    registerDataBlockEvent(*netRegistry_);
    netRegistry_->registerGhost<StaticShape>(static_cast<UInt16>(StaticShapePersTag));
    netRegistry_->registerGhost<Moveable>(static_cast<UInt16>(FearMoveablePersTag));
    netRegistry_->registerGhost<InteriorShape>(static_cast<UInt16>(InteriorShapePersTag));
    netRegistry_->registerGhost<FearMissionTerrain>(2);
    registerBaseSimNetwork(*netRegistry_);
    registerPlayerNetwork(*netRegistry_);
    registerPlayerManagerEvents(*netRegistry_);
    registerProjectileNetwork(*netRegistry_);
    registerExplosionNetwork(*netRegistry_);
    registerDebrisNetwork(*netRegistry_);
    registerItemNetwork(*netRegistry_);
    registerTurretNetwork(*netRegistry_);
    registerMineNetwork(*netRegistry_);
    registerSensorNetwork(*netRegistry_);
    registerSensorManagerEvents(*netRegistry_);
    registerVehicleNetwork(*netRegistry_);
    registerFlierNetwork(*netRegistry_);
    registerSoundEvents(*netRegistry_);
    netRegistry_->registerGhost<Sky>(static_cast<UInt16>(TribesSkyPersTag));
    netRegistry_->registerGhost<StarField>(static_cast<UInt16>(StarFieldPersTag));
    netRegistry_->registerGhost<Planet>(static_cast<UInt16>(PlanetPersTag));
    netRegistry_->registerGhost<Snowfall>(static_cast<UInt16>(SnowfallPersTag));
    netRegistry_->registerGhost<Marker>(static_cast<UInt16>(MarkerPersTag));
    netRegistry_->registerGhost<Trigger>(static_cast<UInt16>(TriggerPersTag));
    netRegistry_->registerGhost<ObserverCamera>(static_cast<UInt16>(ObserverCamPersTag));
    netRegistry_->registerGhost<MissionCenterPos>(static_cast<UInt16>(MissionCenterPosPersTag));
}

void FearPlugin::refreshActiveGlobals() {
    auto* curMgr = game_.manager();
    if (sg_.manager && curMgr == sg_.manager) {
        wg_ = &sg_;
    } else if (cg_.manager && curMgr == cg_.manager) {
        wg_ = &cg_;
    } else if (sg_.manager) {
        wg_ = &sg_;
    } else {
        wg_ = &cg_;
    }
    if (dynamicData_) {
        dynamicData_->setManager(curMgr);
    }
    if (scriptPlugin_ && sg_.manager) {
        scriptPlugin_->setServerManager(sg_.manager);
    }
}

bool FearPlugin::createServerWorld(std::string_view missionName, bool dedicated) {
    destroyServerWorld();
    auto srvWorld = std::make_unique<sim::SimWorld>(&game_.console(), resources_);
    auto* rawWorld = srvWorld.get();
    auto& mgr = srvWorld->manager();

    ensureNamedSet(mgr, MoveableSetId, "MoveableSet");
    ensureNamedSet(mgr, PlayerSetId, "PlayerSet");
    ensureNamedSet(mgr, TargetableSetId, "TargetableSet");
    ensureNamedSet(mgr, ObserverSetId, "ObserverSet");
    ensureNamedSet(mgr, SensorVisibleSetId, "SensorVisibleSet");
    ensureNamedSet(mgr, sim::RenderSetId, "RenderSet");
    ensureNamedSet(mgr, sim::LightSetId, "LightSet");

    auto dbm = std::make_unique<DataBlockManager>(true, resources_);
    auto* rawDbm = dynamic_cast<DataBlockManager*>(mgr.addObject(std::move(dbm)));

    auto pm = std::make_unique<world::SimPathManager>();
    mgr.addObject(std::move(pm));

    const auto port = console_ && parseBoolSv(console_->getVariable("OpenT1::ConsoleCSExecuted"))
                          ? parseIntSv(console_->getVariable("Server::Port"))
                          : 0;
    auto transport =
        net::makeGameTransport({{0, 0, 0, 0}, static_cast<UInt16>(port > 0 ? port : 0)});
    auto csd = std::make_unique<FearCSDelegate>(true, banList_, netRegistry_, std::move(transport));
    csd->config().dedicated = dedicated;
    game_.console().setVariable("Server::Dedicated", dedicated ? "true" : "false");
    csd->configureGameStream = [this](auto& stream) {
        return configureRemoteStream(stream);
    };
    auto* rawCsd = dynamic_cast<FearCSDelegate*>(mgr.addObject(std::move(csd)));
    if (rawCsd) {
        rawCsd->bindConsole(game_.console());
    }

    auto playerMgr = std::make_unique<PlayerManager>(true);
    playerMgr->bindConsole(&game_.console());
    auto* rawPlayerMgr = dynamic_cast<PlayerManager*>(mgr.addObject(std::move(playerMgr)));

    auto sensorMgr = std::make_unique<SensorManager>();
    auto* rawSensorMgr = dynamic_cast<SensorManager*>(mgr.addObject(std::move(sensorMgr)));

    auto decalMgr = std::make_unique<DecalManager>();
    auto* rawDecalMgr = dynamic_cast<DecalManager*>(mgr.addObject(std::move(decalMgr)));

    auto aiMgr = std::make_unique<AIManager>();
    auto* rawAiMgr = dynamic_cast<AIManager*>(mgr.addObject(std::move(aiMgr)));

    auto srvPsc = std::make_unique<PlayerPSC>(true);
    srvPsc->deferMoveProcessing(true);
    auto* rawSrvPsc = dynamic_cast<PlayerPSC*>(mgr.addObject(std::move(srvPsc)));

    auto srvGhostMgr = std::make_unique<sim::network::GhostManager>();
    auto* rawSrvGhostMgr =
        dynamic_cast<sim::network::GhostManager*>(mgr.addObject(std::move(srvGhostMgr)));
    if (rawSrvPsc) {
        rawSrvPsc->setLocalGhostManager(rawSrvGhostMgr);
    }

    auto cleanup = std::make_unique<sim::SimGroup>();
    cleanup->assignName("MissionCleanup");
    mgr.addObject(std::move(cleanup));

    auto missionGroup = std::make_unique<sim::SimGroup>();
    missionGroup->assignName("MissionGroup");
    mgr.addObject(std::move(missionGroup));

    if (!game_.setWorld(sim::WorldType::Server, std::move(srvWorld))) {
        return false;
    }

    sg_.world = rawWorld;
    sg_.manager = &rawWorld->manager();
    sg_.dbm = rawDbm;
    sg_.csDelegate = rawCsd;
    sg_.playerManager = rawPlayerMgr;
    sg_.sensorManager = rawSensorMgr;
    sg_.decalManager = rawDecalMgr;
    sg_.aiManager = rawAiMgr;
    sg_.playerPSC = rawSrvPsc;
    sg_.ghostManager = rawSrvGhostMgr;
    sg_.resManager = resources_.get();
    sg_.timeBase = static_cast<UInt32>(game_.simulationTime() * 1000.0);
    sg_.currentTime = sg_.timeBase;
    serverSimulationTime_ = sg_.timeBase;
    sg_.lastTime = sg_.timeBase;

    if (!missionName.empty()) {
        game_.console().setVariable("MissionName", missionName);
        InteriorShape::sm_missionName = std::string(missionName);
    }
    game_.focus(sim::WorldType::Server);
    refreshActiveGlobals();
    return true;
}

bool FearPlugin::bindServerPort(UInt16 port) {
    if (!sg_.manager || !sg_.csDelegate || !sg_.csDelegate->delegate()) {
        return false;
    }
    const auto address = net::parseIPv4Address(sg_.csDelegate->delegate()->localAddress());
    if (port == 0 || (address && address->port == port)) {
        return true;
    }
    if (!sg_.csDelegate->connectedPeers().empty()) {
        return false;
    }
    // console.cs creates the world before it reads server preferences. Replace
    // only its unused listener after startup, preserving the mission and bots.
    auto transport = net::makeGameTransport({{0, 0, 0, 0}, port});
    auto replacement =
        std::make_unique<FearCSDelegate>(true, banList_, netRegistry_, std::move(transport));
    replacement->configureGameStream = [this](auto& stream) {
        return configureRemoteStream(stream);
    };
    replacement->config() = sg_.csDelegate->config();
    replacement->bindConsole(game_.console());
    auto* previous = sg_.csDelegate;
    sg_.csDelegate = nullptr;
    previous->deleteObject();
    sg_.csDelegate =
        dynamic_cast<FearCSDelegate*>(sg_.manager->addObject(std::move(replacement)));
    return sg_.csDelegate != nullptr;
}

bool FearPlugin::createClientWorld() {
    destroyClientWorld();
    auto cliWorld = std::make_unique<sim::SimWorld>(&game_.console(), resources_);
    auto* rawWorld = cliWorld.get();
    auto& mgr = cliWorld->manager();

    ensureNamedSet(mgr, MoveableSetId, "MoveableSet");
    ensureNamedSet(mgr, PlayerSetId, "PlayerSet");
    ensureNamedSet(mgr, TargetableSetId, "TargetableSet");
    ensureNamedSet(mgr, ObserverSetId, "ObserverSet");
    ensureNamedSet(mgr, SensorVisibleSetId, "SensorVisibleSet");
    ensureNamedSet(mgr, sim::RenderSetId, "RenderSet");
    ensureNamedSet(mgr, sim::LightSetId, "LightSet");
    ensureNamedSet(mgr, sim::CameraMountSetId, "CameraMountSet");
    ensureNamedSet(mgr, sim::FrameEndNotifySetId, "FrameEndNotifySet");
    {
        auto hudSet = std::make_unique<sim::SimSet>();
        hudSet->assignName("HudSet");
        mgr.addObject(std::move(hudSet));
    }

    auto irc = std::make_unique<IRCClient>();
    irc->assignName("IRCClient");
    mgr.addObject(std::move(irc));

    auto dbm = std::make_unique<DataBlockManager>(false, resources_);
    auto* rawDbm = dynamic_cast<DataBlockManager*>(mgr.addObject(std::move(dbm)));

    auto pm = std::make_unique<world::SimPathManager>();
    mgr.addObject(std::move(pm));

    auto csd =
        std::make_unique<FearCSDelegate>(false, banList_, netRegistry_, net::makeGameTransport());
    csd->onConnection = [this](const net::Event& event) {
        if (event.type == net::EventType::Accepted && onConnectionAccepted) {
            onConnectionAccepted();
        }
        if (event.type == net::EventType::Disconnected || event.type == net::EventType::TimedOut ||
            event.type == net::EventType::Rejected) {
            dataFinishedFlag_ = false;
            cg_.playerPSC = nullptr;
            cg_.ghostManager = nullptr;
            connectAddress_.clear();
            game_.console().setVariable("ConnectedToServer", "false");
            if (cg_.dbm) {
                cg_.dbm->onDataFinished = {};
                cg_.dbm->reset();
            }
        }
    };
    csd->configureGameStream = [this](auto& stream) {
        return configureRemoteStream(stream);
    };
    auto* rawCsd = dynamic_cast<FearCSDelegate*>(mgr.addObject(std::move(csd)));

    auto playerMgr = std::make_unique<PlayerManager>(false);
    playerMgr->bindConsole(&game_.console());
    playerMgr->onChatMessage = [this](Int32 type, std::string_view message) {
        if (onChatMessage) {
            onChatMessage(type, message);
        }
    };
    auto* rawPlayerMgr = dynamic_cast<PlayerManager*>(mgr.addObject(std::move(playerMgr)));

    auto sensorMgr = std::make_unique<SensorManager>();
    auto* rawSensorMgr = dynamic_cast<SensorManager*>(mgr.addObject(std::move(sensorMgr)));

    auto decalMgr = std::make_unique<DecalManager>();
    auto* rawDecalMgr = dynamic_cast<DecalManager*>(mgr.addObject(std::move(decalMgr)));

    mgr.addObject(std::make_unique<SmokeManager>());

    auto cliPsc = std::make_unique<PlayerPSC>(false);
    auto* rawCliPsc = dynamic_cast<PlayerPSC*>(mgr.addObject(std::move(cliPsc)));

    auto chatMenu = std::make_unique<ChatMenu>("Options:");
    chatMenu->bindConsole(&game_.console());
    chatMenu->assignName("ChatMenu");
    auto* rawChatMenu = dynamic_cast<ChatMenu*>(mgr.addObject(std::move(chatMenu)));

    auto hudMgr = std::make_unique<HudManager>();
    hudMgr->assignName("HudManager");
    auto* rawHudMgr = dynamic_cast<HudManager*>(mgr.addObject(std::move(hudMgr)));

    auto cliGhostMgr = std::make_unique<sim::network::GhostManager>();
    auto* rawCliGhostMgr =
        dynamic_cast<sim::network::GhostManager*>(mgr.addObject(std::move(cliGhostMgr)));
    if (rawCliPsc) {
        rawCliPsc->setLocalGhostManager(rawCliGhostMgr);
    }

    if (!game_.setWorld(sim::WorldType::Client, std::move(cliWorld))) {
        return false;
    }

    cg_.world = rawWorld;
    cg_.manager = &rawWorld->manager();
    cg_.dbm = rawDbm;
    cg_.csDelegate = rawCsd;
    cg_.playerManager = rawPlayerMgr;
    cg_.sensorManager = rawSensorMgr;
    cg_.decalManager = rawDecalMgr;
    cg_.playerPSC = rawCliPsc;
    cg_.chatMenu = rawChatMenu;
    cg_.hudManager = rawHudMgr;
    cg_.ghostManager = rawCliGhostMgr;
    cg_.resManager = resources_.get();
    cg_.timeBase = static_cast<UInt32>(game_.simulationTime() * 1000.0);
    cg_.currentTime = cg_.timeBase;
    cg_.lastTime = cg_.timeBase;
    // Local hosting receives the same PSC headers as remote connections.
    bindClientControlCallbacks();

    // Packet timestamps and queued input must use the same integer clock.
    // The manager's float event time may still refer to the previous frame
    // while its delegate is receiving packets.
    if (rawCsd && rawCsd->delegate()) {
        rawCsd->delegate()->clock = [this] {
            return cg_.currentTime;
        };
    }

    game_.focus(sim::WorldType::Client);
    refreshActiveGlobals();
    return true;
}

void FearPlugin::destroyServerWorld() {
    if (console_) console_->setVariable("ME::Loaded", "False");
    if (terrainEditor_) {
        terrainEditor_->focused = false;
        terrainEditor_->detach();
    }
    localClientId_ = 0;
    pendingLocalKick_.reset();
    if (scriptPlugin_) {
        scriptPlugin_->clearClients();
        scriptPlugin_->setServerManager(nullptr);
    }
    pendingRemoteControls_.clear();
    serverToClientGhosts_.clear();
    loopbackSourceGenerations_.clear();
    dataFinishedFlag_ = false;
    sg_.clear();
    game_.setWorld(sim::WorldType::Server, nullptr);
    refreshActiveGlobals();
}

void FearPlugin::destroyClientWorld() {
    localClientId_ = 0;
    connectAddress_.clear();
    pendingLocalKick_.reset();
    serverToClientGhosts_.clear();
    loopbackSourceGenerations_.clear();
    dataFinishedFlag_ = false;
    cg_.clear();
    game_.setWorld(sim::WorldType::Client, nullptr);
    refreshActiveGlobals();
}

bool FearPlugin::focusServer() {
    if (!game_.world(sim::WorldType::Server)) {
        return false;
    }
    if (!game_.focus(sim::WorldType::Server)) {
        return false;
    }
    refreshActiveGlobals();
    return true;
}

bool FearPlugin::focusClient() {
    if (!game_.world(sim::WorldType::Client)) {
        return false;
    }
    if (!game_.focus(sim::WorldType::Client)) {
        return false;
    }
    refreshActiveGlobals();
    return true;
}

std::optional<sim::SimObjectId> FearPlugin::spawnPlayer(std::string_view armorName,
                                                        const Point3F& pos,
                                                        const Point3F& rot,
                                                        Int32 clientId,
                                                        Int32 teamId) {
    if (!sg_.manager || !sg_.dbm) {
        return std::nullopt;
    }
    if (!clientId) {
        clientId = localClientId_ ? localClientId_
                                 : sg_.playerManager ? sg_.playerManager->getFreeId() : 0;
    }
    const auto* representative =
        sg_.playerManager ? sg_.playerManager->findBaseRep(clientId) : nullptr;
    if (!clientId || (representative && !representative->isClientRep())) {
        return std::nullopt;
    }

    PlayerData* playerData = nullptr;
    Int32 playerDatId = -1;
    if (!armorName.empty()) {
        playerData = dynamic_cast<PlayerData*>(
            sg_.dbm->lookupDataBlock(armorName, DataBlockManager::PlayerDataType, &playerDatId));
    }
    if (!playerData && armorName.empty() &&
        sg_.dbm->getDataGroupSize(DataBlockManager::PlayerDataType) > 0) {
        playerData = dynamic_cast<PlayerData*>(
            sg_.dbm->lookupDataBlock(0, DataBlockManager::PlayerDataType));
        playerDatId = 0;
    }
    if (!playerData) {
        return std::nullopt;
    }

    Point3F spawnPos = pos;
    Point3F spawnRot = rot;
    if (spawnPos.x == 0.0f && spawnPos.y == 0.0f && spawnPos.z == 0.0f && spawnRot.x == 0.0f &&
        spawnRot.y == 0.0f && spawnRot.z == 0.0f) {
        std::vector<sim::SimObject*> allObjs;
        collectGroupObjectsRecursive(*sg_.manager, allObjs);
        for (sim::SimObject* obj : allObjs) {
            if (auto* dpg = dynamic_cast<DropPointGroup*>(obj)) {
                DropPointQuery dpQuery{};
                if (dpg->processQuery(dpQuery)) {
                    spawnPos = dpQuery.tmat.p;
                    break;
                }
                for (sim::SimObject* child : dpg->objects()) {
                    if (auto* marker = dynamic_cast<Marker*>(child)) {
                        spawnPos = marker->getLinearPosition();
                        spawnRot = marker->getAngPosition();
                        break;
                    }
                }
                if (spawnPos.x != 0.0f || spawnPos.y != 0.0f || spawnPos.z != 0.0f) {
                    break;
                }
            }
        }
    }

    auto player = std::make_unique<Player>();
    player->bindDataBlockManager(sg_.dbm);
    player->bindConsole(&game_.console());
    player->setDatFileId(playerDatId);
    player->setDatFileName(playerData->dbmName);
    player->setTeam(teamId);
    player->setOwnerClient(clientId);
    player->setControlClient(clientId);
    player->setRot(spawnRot);
    player->setPos(spawnPos);
    if (!player->initResources(playerData)) {
        return std::nullopt;
    }

    auto* cleanup = dynamic_cast<sim::SimGroup*>(sg_.manager->findObject("MissionCleanup"));
    auto* added = dynamic_cast<Player*>(sg_.manager->addObject(std::move(player), cleanup));
    if (!added) {
        return std::nullopt;
    }
    added->setRot(spawnRot);
    added->setPos(spawnPos);

    if (sg_.playerManager) {
        if (!sg_.playerManager->findClient(clientId)) {
            addLocalPlayer(*sg_.playerManager, game_.console(), clientId);
        }
        if (auto* cl = sg_.playerManager->findClient(clientId)) {
            cl->ownedObject = added;
            cl->controlObject = added;
            if (!cl->playerPSC) {
                cl->playerPSC = sg_.playerPSC;
            }
            if (cl->playerPSC) {
                cl->playerPSC->setControlObject(added);
            }
            sg_.playerManager->serverClientTeamChange(clientId, teamId);
        }
    }

    if (scriptPlugin_) {
        auto& clRep = scriptPlugin_->ensureClient(clientId);
        if (clRep.observerCameraId == 0) {
            auto obsCam = std::make_unique<ObserverCamera>(clientId);
            if (auto* addedCam = sg_.manager->addObject(std::move(obsCam), cleanup)) {
                clRep.observerCameraId = addedCam->getId();
            }
        }
    }

    return added->getId();
}

sim::SimObject* FearPlugin::findGhostForServerObject(sim::SimObjectId serverId) const noexcept {
    if (!cg_.manager) {
        return nullptr;
    }
    const auto it = serverToClientGhosts_.find(serverId);
    if (it == serverToClientGhosts_.end()) {
        return nullptr;
    }
    return cg_.manager->findObject(it->second);
}

std::size_t FearPlugin::syncLoopbackState(bool deliverMoves) {
    if (!sg_.world || !cg_.world || connectAddress_ != "LOOPBACK:28001") {
        return 0;
    }

    // 1. Replicate server datablocks to client DataBlockManager via BitStream
    if (sg_.dbm && cg_.dbm) {
        auto events = sg_.dbm->buildClientEvents();
        for (auto& evt : events) {
            if (evt->block < 0) {
                if (dataFinishedFlag_) {
                    continue;
                }
            } else {
                const Int32 curSize = cg_.dbm->getDataGroupSize(evt->group);
                if (evt->block < curSize &&
                    cg_.dbm->lookupDataBlock(evt->block, evt->group) != nullptr) {
                    continue;
                }
            }
            std::array<std::byte, 4096> buf{};
            BitStream wbs(std::span<std::byte>(buf.data(), buf.size()));
            if (evt->pack(wbs, *sg_.dbm) && wbs.isValid()) {
                const auto bitCount = static_cast<std::size_t>(wbs.getCurPos());
                BitStream rbs(std::span<const std::byte>(buf.data(), buf.size()));
                rbs.setLimits(bitCount, 0);
                DataBlockEvent clientEv;
                if (clientEv.unpack(rbs, *cg_.dbm) && rbs.isValid() &&
                    cg_.dbm->processEvent(clientEv)) {
                    if (clientEv.block >= 0 && console_) {
                        console_->setVariable("DataBlock::LastGroup",
                                              std::to_string(clientEv.group));
                        console_->setVariable("DataBlock::LastProgress",
                                              std::to_string(clientEv.block + 1));
                    }
                } else {
                    game_.console().setVariable("OpenT1::LastNetworkError",
                                                "Datablock receive failed: " +
                                                    cg_.dbm->lastDataBlockError());
                    return 0;
                }
            } else {
                game_.console().setVariable("OpenT1::LastNetworkError",
                                            "Datablock serialization failed");
                return 0;
            }
        }
        if (!events.empty() && !dataFinishedFlag_) {
            dataFinishedFlag_ = true;
            if (console_) {
                console_->setVariable("DataBlock::Finished", "true");
            }
        }
    }

    if (sg_.sensorManager && cg_.sensorManager) {
        for (auto key : std::views::iota(Int32{0}) | std::views::take_while([&](Int32 key) {
                            return key < SensorManager::MaxTargets;
                        })) {
            const auto& name = sg_.sensorManager->target(key).name;
            if (cg_.sensorManager->target(key).name != name) {
                TargetNameEvent event;
                event.targId = key;
                event.name = name;
                cg_.sensorManager->processEvent(event);
            }
        }
    }

    // 2. Replicate server PlayerManager teams and clients to client PlayerManager
    if (sg_.playerManager && cg_.playerManager) {
        cg_.playerManager->setTeamScoreHeading(sg_.playerManager->teamScoreHeading());
        cg_.playerManager->setClientScoreHeading(sg_.playerManager->clientScoreHeading());
        if (sg_.playerManager->getNumTeams() <= 0) {
            sg_.playerManager->teamAdded("Blood Eagle", "beagle");
            sg_.playerManager->teamAdded("Diamond Sword", "dsword");
        }
        for (auto t : std::views::iota(Int32{-1}) | std::views::take_while([&](Int32 t) {
                          return t < sg_.playerManager->getNumTeams();
                      })) {
            const auto* srvTeam = sg_.playerManager->findTeam(t);
            if (!srvTeam) {
                continue;
            }
            TeamAddEvent tEv;
            tEv.id = srvTeam->id;
            tEv.name = srvTeam->name;
            tEv.skinBase = srvTeam->skinBase;
            tEv.energy = srvTeam->energy;
            std::array<std::byte, 1024> tBuf{};
            BitStream twbs(std::span<std::byte>(tBuf.data(), tBuf.size()));
            tEv.pack(twbs);
            BitStream trbs(std::span<const std::byte>(tBuf.data(), tBuf.size()));
            trbs.setLimits(static_cast<std::size_t>(twbs.getCurPos()), 0);
            auto unpackedTeamEv = std::make_unique<TeamAddEvent>();
            if (unpackedTeamEv->unpack(trbs)) {
                cg_.playerManager->processEvent(*unpackedTeamEv);
            }
            if (auto* cliTeam = cg_.playerManager->findTeam(t)) {
                cliTeam->score = srvTeam->score;
                cliTeam->scoreString = srvTeam->scoreString;
                cliTeam->scoreSortValue = srvTeam->scoreSortValue;
                cliTeam->objectives = srvTeam->objectives;
            }
        }

        for (auto* srvRep = sg_.playerManager->getBaseRepList(); srvRep != nullptr;
             srvRep = srvRep->nextClient) {
            const auto* srvClient = static_cast<const PlayerManager::ClientRep*>(srvRep);
            if (!cg_.playerManager->findBaseRep(srvRep->id)) {
                PlayerAddEvent pEv;
                pEv.id = srvRep->id;
                pEv.isBaseRepType = srvRep->isBaseRepType;
                pEv.name = srvRep->name;
                pEv.skinBase = srvRep->skinBase;
                pEv.voiceBase = srvClient->voiceBase;
                pEv.gender = srvClient->gender == "FEMALE" ? 0 : 1;
                pEv.commanderId = srvClient->commanderId;
                pEv.team = srvRep->team;
                std::array<std::byte, 1024> pBuf{};
                BitStream pwbs(std::span<std::byte>(pBuf.data(), pBuf.size()));
                pEv.pack(pwbs);
                BitStream prbs(std::span<const std::byte>(pBuf.data(), pBuf.size()));
                prbs.setLimits(static_cast<std::size_t>(pwbs.getCurPos()), 0);
                auto unpackedPlayerEv = std::make_unique<PlayerAddEvent>();
                if (unpackedPlayerEv->unpack(prbs)) {
                    cg_.playerManager->processEvent(*unpackedPlayerEv);
                }
            }
            if (auto* cliRep = cg_.playerManager->findBaseRep(srvRep->id)) {
                cliRep->team = srvRep->team;
                cliRep->score = srvRep->score;
                cliRep->scoreString = srvRep->scoreString;
                cliRep->scoreSortValue = srvRep->scoreSortValue;
                cliRep->ping = srvClient->ping;
                cliRep->packetLoss = srvClient->packetLoss;
            }
        }
    }

    // Only deliver input at a server tick. A partial-frame move has not been
    // simulated yet; acknowledging it early discards required prediction data.
    if (deliverMoves && cg_.playerPSC && sg_.playerPSC && !cg_.playerPSC->moves().empty()) {
        std::array<std::byte, 2048> moveBuf{};
        BitStream mwbs(std::span<std::byte>(moveBuf.data(), moveBuf.size()));
        UInt32 moveKey = 0;
        if (cg_.playerPSC->writePacket(mwbs, moveKey)) {
            const auto bitCount = static_cast<std::size_t>(mwbs.getCurPos());
            BitStream mrbs(std::span<const std::byte>(moveBuf.data(), moveBuf.size()));
            mrbs.setLimits(bitCount, 0);
            if (sg_.playerPSC->readPacket(mrbs, sg_.currentTime)) {
                cg_.playerPSC->packetReceived(moveKey);
            } else {
                cg_.playerPSC->packetDropped(moveKey);
            }
        }
    }

    if (auto* sourcePaths =
            dynamic_cast<world::SimPathManager*>(sg_.manager->findObject(world::PathManagerId))) {
        if (auto* targetPaths = dynamic_cast<world::SimPathManager*>(
                cg_.manager->findObject(world::PathManagerId))) {
            for (const auto& event : sourcePaths->fullSync()) {
                (void)targetPaths->processEvent(event);
            }
        }
    }

    // 4. Replicate & update ghosts from server world to client world
    std::vector<sim::SimObject*> srvObjects;
    collectGroupObjectsRecursive(*sg_.manager, srvObjects);

    if (!sg_.ghostManager || !cg_.ghostManager) {
        return 0;
    }
    auto& sourceManager = *sg_.ghostManager;
    auto& targetManager = *cg_.ghostManager;
    for (auto it = serverToClientGhosts_.begin(); it != serverToClientGhosts_.end();) {
        const auto* source = sg_.manager->findObject(it->first);
        if (!source || source->generation() != loopbackSourceGenerations_[it->first]) {
            if (auto* ghost = cg_.manager->findObject(it->second)) {
                ghost->deleteObject();
            }
            loopbackSourceGenerations_.erase(it->first);
            it = serverToClientGhosts_.erase(it);
        } else {
            ++it;
        }
    }
    std::size_t syncedGhosts = 0;
    for (sim::SimObject* object : srvObjects) {
        auto* source = dynamic_cast<sim::SimNetObject*>(object);
        if (!source || !source->getGhostTag() ||
            !(source->netFlags() & sim::SimNetObject::Ghostable)) {
            continue;
        }
        sim::SimNetObject* target = nullptr;
        if (const auto found = serverToClientGhosts_.find(source->getId());
            found != serverToClientGhosts_.end()) {
            target = dynamic_cast<sim::SimNetObject*>(cg_.manager->findObject(found->second));
        }
        const bool initial = !target;
        // Establish the shooter's client object before initializing a shot:
        // collision prediction must exclude the client player, not a server ID.
        ShapeBase* projectileShooter = nullptr;
        if (initial) {
            if (const auto* projectile = dynamic_cast<const Projectile*>(source);
                projectile && projectile->shooter()) {
                projectileShooter = dynamic_cast<ShapeBase*>(
                    findGhostForServerObject(projectile->shooter()->getId()));
                if (!projectileShooter) {
                    continue;
                }
            }
        }
        if (!sourceManager.objectInScope(*source)) {
            continue;
        }
        UInt32 mask = sourceManager.takeUpdateMask(*source);
        if (initial) {
            mask = ~UInt32{0};
        } else if (dynamic_cast<GameBase*>(source)) {
            mask &= ~UInt32(GameBase::InitialUpdate);
        }
        std::unique_ptr<sim::SimNetObject> created;
        if (initial) {
            created = netRegistry_->createGhost(source->getGhostTag());
            if (!created) {
                source->setMaskBits(mask);
                game_.console().setVariable(
                    "OpenT1::LastNetworkError",
                    std::format("Unregistered ghost class: {}", source->getGhostTag()));
                continue;
            }
            created->setNetFlags(sim::SimNetObject::IsGhost);
            if (auto* base = dynamic_cast<GameBase*>(created.get())) {
                base->bindDataBlockManager(cg_.dbm);
                base->bindConsole(&game_.console());
            }
            if (auto* moveable = dynamic_cast<MoveableBase*>(created.get())) {
                moveable->bindPathManager(dynamic_cast<world::SimPathManager*>(
                    cg_.manager->findObject(world::PathManagerId)));
            }
            target = created.get();
        }
        if (mask) {
            std::array<std::byte, 65536> bytes{};
            BitStream output{std::span<std::byte>(bytes)};
            const auto remaining = source->packUpdate(sourceManager, mask, output);
            BitStream input{std::span<const std::byte>(bytes)};
            input.setLimits(static_cast<std::size_t>(output.getCurPos()), 0);
            if (!output.isValid() || !target->unpackUpdate(targetManager, input) ||
                !input.isValid() || input.getCurPos() != output.getCurPos()) {
                source->setMaskBits(mask);
                game_.console().setVariable("OpenT1::LastNetworkError",
                                            std::format("Ghost update failed: tag {} object {}",
                                                        source->getGhostTag(),
                                                        source->getId()));
                continue;
            }
            source->setMaskBits(remaining);
        }
        if (created) {
            if (auto* projectile = dynamic_cast<Projectile*>(created.get());
                projectile && projectileShooter) {
                projectile->setShooter(projectileShooter);
                projectile->setOwnerId(projectileShooter->getId());
            }
            if (!source->getName().empty()) {
                created->assignName(source->getName());
            }
            target = dynamic_cast<sim::SimNetObject*>(cg_.manager->addObject(std::move(created)));
            if (!target) {
                source->setMaskBits(mask);
                game_.console().setVariable(
                    "OpenT1::LastNetworkError",
                    std::format("Ghost initialization failed: tag {} object {}",
                                source->getGhostTag(),
                                source->getId()));
                continue;
            }
            if (!sourceManager.registerDirectGhost(*source, targetManager, *target)) {
                target->deleteObject();
                source->setMaskBits(mask);
                game_.console().setVariable(
                    "OpenT1::LastNetworkError",
                    std::format("Ghost registration failed: tag {} object {}",
                                source->getGhostTag(),
                                source->getId()));
                continue;
            }
            serverToClientGhosts_[source->getId()] = target->getId();
            loopbackSourceGenerations_[source->getId()] = source->generation();
        }
        if (const auto* serverPlayer = dynamic_cast<const Player*>(source)) {
            if (auto* clientPlayer = dynamic_cast<Player*>(target)) {
                for (auto item :
                     std::views::iota(Int32{0}) | std::views::take_while([&](Int32 item) {
                         return item < Player::MaxItemTypes;
                     })) {
                    clientPlayer->setItemCount(item, serverPlayer->getItemCount(item));
                }
            }
        }
        ++syncedGhosts;
    }

    if (sg_.playerPSC && cg_.playerPSC) {
        // The PSC header acknowledges input sequence numbers and deliberately
        // retains the newest move for delayed server simulation. Transport
        // receipt alone cannot advance that queue.
        std::array<std::byte, 8192> controlBytes{};
        BitStream output{std::span<std::byte>(controlBytes)};
        UInt32 controlKey{};
        if (sg_.playerPSC->writePacket(output, controlKey)) {
            BitStream input{std::span<const std::byte>(controlBytes)};
            input.setLimits(std::size_t(output.getCurPos()), 0);
            if (cg_.playerPSC->readPacket(input, cg_.currentTime)) {
                sg_.playerPSC->packetReceived(controlKey);
            } else {
                sg_.playerPSC->packetDropped(controlKey);
                game_.console().setVariable("OpenT1::LastNetworkError",
                                            "Loopback player control packet rejected");
            }
        }
    }
    return syncedGhosts;
}

void FearPlugin::stepServerSimulation() {
    if (!sg_.manager) {
        return;
    }

    // 1. Spawn Player bodies for any newly added AI objects and step AIManager
    if (sg_.aiManager) {
        auto* cleanup = dynamic_cast<sim::SimGroup*>(sg_.manager->findObject("MissionCleanup"));
        PlayerData* defaultPlayerData = nullptr;
        Int32 defaultPlayerDatId = -1;
        if (sg_.dbm && sg_.dbm->getDataGroupSize(DataBlockManager::PlayerDataType) > 0) {
            defaultPlayerData = dynamic_cast<PlayerData*>(
                sg_.dbm->lookupDataBlock(0, DataBlockManager::PlayerDataType));
            defaultPlayerDatId = 0;
        }
        for (const auto& ai : sg_.aiManager->aiList()) {
            if (!ai->player() && ai->spawnBodyOnTick && defaultPlayerData) {
                auto aiPl = std::make_unique<Player>();
                aiPl->bindDataBlockManager(sg_.dbm);
                aiPl->bindConsole(&game_.console());
                aiPl->setDatFileId(defaultPlayerDatId);
                aiPl->setDatFileName(defaultPlayerData->dbmName);
                aiPl->setTeam(ai->team);
                aiPl->setAiControlled(true);
                const Point3F startPos =
                    ai->marker() ? ai->marker()->getLinearPosition() : ai->curDest();
                if (ai->marker()) {
                    aiPl->setPosition(ai->marker()->getTransform(), true);
                } else {
                    aiPl->setPos(startPos);
                }
                if (aiPl->initResources(defaultPlayerData)) {
                    if (auto* addedPl = dynamic_cast<Player*>(
                            sg_.manager->addObject(std::move(aiPl), cleanup))) {
                        if (ai->marker()) {
                            const auto transform = ai->marker()->getTransform();
                            EulerF rotation;
                            transform.angles(rotation);
                            addedPl->setPosition(transform, true);
                            addedPl->setRot({rotation.x, rotation.y, rotation.z});
                        } else {
                            addedPl->setPos(startPos);
                        }
                        ai->setPlayer(addedPl);
                        addedPl->setOwnerClient(ai->id);
                        sg_.aiManager->deleteNotify(*addedPl);
                    }
                }
            }
        }
        sg_.aiManager->doServerProcesses(static_cast<SimTime>(sg_.currentTime) * 0.001f);
    }

    // Deferred input belongs to the connection's current control object. Death
    // changes that object to an ObserverCamera, whose FIRE release runs the
    // original respawn script. Remote PSCs already dispatch during readPacket.
    const auto processControlMoves = [](PlayerPSC* psc) {
        if (psc && psc->moveProcessingDeferred() && psc->getControlObject() &&
            !psc->moves().empty()) {
            psc->getControlObject()->serverUpdateMove(psc->moves());
            psc->clearMoves();
        }
    };
    processControlMoves(sg_.playerPSC);
    if (sg_.playerManager) {
        for (auto* rep = sg_.playerManager->getBaseRepList(); rep; rep = rep->nextClient) {
            if (auto* client = sg_.playerManager->findClient(rep->id);
                client && client->playerPSC != sg_.playerPSC) {
                processControlMoves(client->playerPSC);
            }
        }
    }

    // 2. Step all server simulation objects
    std::vector<sim::SimObject*> srvObjects;
    collectGroupObjectsRecursive(*sg_.manager, srvObjects);

    for (sim::SimObject* obj : srvObjects) {
        if (!obj || obj->isDeleted() || obj->isRemoved()) {
            continue;
        }
        if (auto* pl = dynamic_cast<Player*>(obj)) {
            pl->serverProcess(sg_.currentTime);
        } else if (auto* mv = dynamic_cast<Moveable*>(obj)) {
            mv->serverProcess(sg_.currentTime);
        } else if (auto* tu = dynamic_cast<Turret*>(obj)) {
            tu->serverProcess(sg_.currentTime);
        } else if (auto* sn = dynamic_cast<Sensor*>(obj)) {
            sn->serverProcess(sg_.currentTime);
        } else if (auto* it = dynamic_cast<Item*>(obj)) {
            it->serverProcess(sg_.currentTime);
        } else if (auto* mn = dynamic_cast<Mine*>(obj)) {
            mn->serverProcess(sg_.currentTime);
        } else if (auto* fl = dynamic_cast<Flier*>(obj)) {
            fl->serverProcess(sg_.currentTime);
        } else if (auto* vh = dynamic_cast<Vehicle*>(obj)) {
            vh->serverProcess(sg_.currentTime);
        } else if (auto* pr = dynamic_cast<Projectile*>(obj)) {
            pr->serverProcess(sg_.currentTime);
        } else if (auto* ss = dynamic_cast<StaticShape*>(obj)) {
            ss->serverProcess(sg_.currentTime);
        } else if (auto* tr = dynamic_cast<Trigger*>(obj)) {
            tr->serverProcess(sg_.currentTime);
            const auto nowSec = static_cast<SimTime>(sg_.currentTime) * 0.001f;
            if (auto* playerSet =
                    dynamic_cast<sim::SimSet*>(sg_.manager->findObject(PlayerSetId))) {
                const Box3F& trigBox = tr->getBoundingBox();
                for (sim::SimObject* cand : playerSet->objects()) {
                    if (auto* gb = dynamic_cast<GameBase*>(cand)) {
                        const Point3F pos = gb->getLinearPosition();
                        if (pos.x >= trigBox.fMin.x && pos.x <= trigBox.fMax.x &&
                            pos.y >= trigBox.fMin.y && pos.y <= trigBox.fMax.y &&
                            pos.z >= trigBox.fMin.z && pos.z <= trigBox.fMax.z) {
                            tr->handleCollision(gb, nowSec);
                        }
                    }
                }
            }
            tr->expireContacts(nowSec);
        }
    }

    // 3. Step SensorManager target detection
    if (sg_.sensorManager) {
        for (sim::SimObject* obj : srvObjects) {
            if (auto* gb = dynamic_cast<GameBase*>(obj)) {
                if (dynamic_cast<Player*>(gb) != nullptr || dynamic_cast<Vehicle*>(gb) != nullptr ||
                    dynamic_cast<Turret*>(gb) != nullptr) {
                    sg_.sensorManager->testObject(gb);
                }
            }
        }
    }
}

void FearPlugin::stepClientSimulation() {
    if (!cg_.manager) {
        return;
    }

    std::vector<sim::SimObject*> cliObjects;
    collectGroupObjectsRecursive(*cg_.manager, cliObjects);

    for (sim::SimObject* obj : cliObjects) {
        if (!obj || obj->isDeleted() || obj->isRemoved()) {
            continue;
        }
        if (auto* base = dynamic_cast<GameBase*>(obj)) {
            base->clientProcess(cg_.currentTime);
        } else if (auto* is = dynamic_cast<InteriorShape*>(obj)) {
            is->clientProcess(cg_.currentTime);
        } else if (auto* ex = dynamic_cast<Explosion*>(obj)) {
            ex->clientProcess(cg_.currentTime);
        } else if (auto* smoke = dynamic_cast<SmokeManager*>(obj)) {
            smoke->clientProcess(cg_.currentTime);
        } else if (auto* sn = dynamic_cast<Snowfall*>(obj)) {
            if (auto* control = cg_.playerPSC ? cg_.playerPSC->getControlObject() : nullptr) {
                TMat3F camera;
                control->getCameraTransform(cg_.playerPSC->camDist(), &camera);
                sn->setCameraState(camera.p, control->getLinearVelocity(), 1.0f);
            }
            sn->snowLogicFrame(static_cast<Flt32>(cg_.currentTime - cg_.lastTime) * 0.001f,
                               static_cast<Flt32>(cg_.currentTime) * 0.001f);
        }
    }
}

void FearPlugin::installTerrainEditing(Console& console) {
    terrainEditor_ = std::make_shared<world::TerrainEditor>();
    const auto selected = [this]() -> world::SimMissionTerrain* {
        if (game_.worldRevision(editedTerrainWorld_) != editedTerrainWorldRevision_) {
            return nullptr;
        }
        auto* currentWorld = game_.world(editedTerrainWorld_);
        return currentWorld ? dynamic_cast<world::SimMissionTerrain*>(
                                  editedTerrain_.resolve(currentWorld->manager()))
                            : nullptr;
    };
    terrainEditor_->terrainLoader =
        [this](std::string_view name) -> std::optional<world::TerrainDocument> {
        // The normal ted.cs attach has no name and runs with client focus.
        auto* server = game_.world(sim::WorldType::Server);
        auto* activeManager = server ? &server->manager() : game_.manager();
        auto* terrain = dynamic_cast<world::SimMissionTerrain*>(
            name.empty() ? (activeManager ? activeManager->findObject(sim::TerrainId) : nullptr)
                         : resolveSimObject(activeManager, name));
        if (!terrain) {
            return std::nullopt;
        }
        const auto resources = terrain->editableResources();
        if (!resources || !resources->grid) {
            return std::nullopt;
        }
        world::TerrainDocument document;
        document.grid = *resources->grid;
        document.gridName = terrain->state().filename;
        for (const auto& block : resources->blocks) {
            if (!block) {
                return std::nullopt;
            }
            document.blocks.push_back(*block);
        }
        if (!document.valid()) {
            return std::nullopt;
        }
        editedTerrainWorld_ =
            activeManager == sg_.manager ? sim::WorldType::Server : sim::WorldType::Client;
        editedTerrainWorldRevision_ = game_.worldRevision(editedTerrainWorld_);
        editedTerrain_ = terrain->address();
        // currFile names the enclosing terrain volume, not its .dtf entry.
        // Keep writes under the configured write root, using the mission's
        // relative directory instead of an installed asset's absolute path.
        const auto source = resources_ ? resources_->getFullPath(document.gridName) : std::nullopt;
        FileName mission(console_->getVariable("missionFile"));
        auto filename = source ? source->filename()
                               : std::filesystem::path(document.gridName).replace_extension(".ted");
        terrainEditor_->attachedFile =
            (FileName(mission.getPath()).toFilesystemPath() / filename).generic_string();
        if (source) {
            const auto assetRoot = console_->getVariable("OpenT1::AssetRoot");
            if (!assetRoot.empty()) {
                for (const auto& root : {std::filesystem::path(assetRoot), console_->filePath("")}) {
                    const auto relative = source->lexically_relative(root);
                    if (!relative.empty() && !relative.is_absolute() && *relative.begin() != "..") {
                        terrainEditor_->attachedFile = relative.generic_string();
                        break;
                    }
                }
            }
        }
        return document;
    };
    terrainEditor_->publisher([selected](const world::TerrainDocument& document) {
        auto* terrain = selected();
        if (!terrain) {
            return false;
        }
        const auto previous = terrain->editableResources();
        if (!previous) {
            return false;
        }
        auto resources = document.resources(previous->materials);
        return resources && terrain->publishEditedResources(std::move(*resources));
    });
    terrainEditor_->relight = [selected, this](world::TerrainDocument& document, bool hires) {
        auto* terrain = selected();
        if (!terrain) {
            return false;
        }
        auto* lights =
            dynamic_cast<sim::SimLightSet*>(terrain->getManager()->findObject(sim::LightSetId));
        if (!lights) {
            return false;
        }
        const auto& state = terrain->state();
        const auto lighting = world::tsSceneLighting(
            lights->getSceneLighting(),
            TMat3F(EulerF(state.rotation.x, state.rotation.y, state.rotation.z), state.position));
        if (!lighting) {
            return false;
        }
        if (!hires) {
            return world::relightTerrainLowResolution(
                document, *lighting, {state.position.x, state.position.y});
        }
        world::TerrainRelightOptions options;
        const auto enabled = [this](std::string_view name) {
            const auto value = console_ ? console_->getVariable(name) : std::string{};
            return value == "1" || value == "True" || value == "true";
        };
        options.interiorShadows = enabled("TED::castInteriorShadows");
        options.hiresShadows = enabled("TED::castHiresShadows");
        const auto maskValue =
            console_ ? console_->getVariable("SimTerrain::InteriorMask") : std::string{};
        const UInt32 mask =
            maskValue.empty() ? sim::InteriorObjectType : UInt32(parseIntSv(maskValue));
        if (auto* mission =
                dynamic_cast<sim::SimSet*>(terrain->getManager()->findObject("MissionGroup"))) {
            const auto visit = [&](auto&& self, sim::SimSet& set) -> void {
                for (auto* object : set.objects()) {
                    if (auto* container = dynamic_cast<sim::SimContainer*>(object);
                        container && (container->getType() & mask)) {
                        options.interiorBounds.push_back(container->getBoundingBox());
                    }
                    if (auto* children = dynamic_cast<sim::SimSet*>(object)) {
                        self(self, *children);
                    }
                }
            };
            visit(visit, *mission);
        }
        options.interiorLOS = [terrain, mask](Point3F start, Point3F end) {
            auto* root = dynamic_cast<sim::SimContainer*>(
                terrain->getManager()->findObject(sim::RootContainerId));
            if (!root) {
                return false;
            }
            sim::SimContainerQuery query;
            query.id = terrain->getId();
            query.mask = mask;
            query.box.fMin = start;
            query.box.fMax = end;
            sim::SimCollisionInfo hit;
            return root->findLOS(query, hit);
        };
        return world::relightTerrainDetailed(document, *lighting, options);
    };
    world::installTerrainEditorCommands(console, terrainEditor_);
    installTerrainEditorFocus(game_, terrainEditor_);
}

void FearPlugin::init(Console& console, sim::SimManager* manager) {
    console_ = &console;
    manager_ = manager;
    IRCShell::registerConsoleCommands(console, [this] {
        return IRCClient::find(cg_.manager);
    });
    installRemoteCommands(console);
    installNetworkCommands(console);
    installGameplayCommands(console);
    installPlayerCommands(console);
    installTerrainEditing(console);
    console.bindVariable("net::interpolateTime", cg_.interpolateTime);
    console.bindVariable("net::predictForwardTime", cg_.predictForwardTime);

    console.setVariable("Fear::Version", FearVersion);
    console.setVariable("Version", FearVersion);
    console.addCommand(0,
                       "setHudTimer",
                       [this](Console&, Int32, std::span<const std::string> args) -> std::string {
                           if (args.size() != 2) {
                               return "False";
                           }
                           Flt32 value{};
                           const auto [end, error] = std::from_chars(
                               args[1].data(), args[1].data() + args[1].size(), value);
                           if (error != std::errc{} || end != args[1].data() + args[1].size() ||
                               !std::isfinite(value)) {
                               return "False";
                           }
                           cg_.clockTime = value;
                           return "True";
                       });
    for (const auto* command : {"enableCacheNoise", "disableCacheNoise"}) {
        console.addCommand(
            0,
            command,
            [command](Console& con, Int32, std::span<const std::string> args) -> std::string {
                if (args.size() != 1) {
                    return "False";
                }
                con.setVariable("pref::TerrainCacheNoise",
                                std::string_view(command) == "enableCacheNoise" ? "True" : "False");
                return "True";
            });
    }

    console.addCommand(
        0, "nameToID", [this](Console&, Int32, std::span<const std::string> args) -> std::string {
            auto* object = args.size() == 2 ? game_.findObject(args[1]) : nullptr;
            return object ? std::to_string(object->getId()) : "-1";
        });
    for (const auto name : {"Group::objectCount", "Group::getObject", "Group::iterateRecursive"}) {
        console.addCommand(
            0,
            name,
            [this, name](Console& con, Int32, std::span<const std::string> args) -> std::string {
                auto* group = args.size() >= 2
                                  ? dynamic_cast<sim::SimSet*>(game_.findObject(args[1]))
                                  : nullptr;
                if (std::string_view(name) == "Group::objectCount") {
                    return group ? std::to_string(group->objects().size()) : "0";
                }
                if (!group || args.size() < 3) {
                    return "-1";
                }
                if (std::string_view(name) == "Group::getObject") {
                    const auto index = parseIntSv(args[2]);
                    return index >= 0 && std::size_t(index) < group->objects().size()
                               ? std::to_string(group->objects()[index]->getId())
                               : "-1";
                }
                std::vector<sim::SimAddress> objects;
                const auto collect = [&](auto&& self, sim::SimSet& set) -> void {
                    for (auto* object : set.objects()) {
                        objects.push_back(object->address());
                        if (auto* child = dynamic_cast<sim::SimGroup*>(object)) {
                            self(self, *child);
                        }
                    }
                };
                collect(collect, *group);
                auto* manager = group->getManager();
                for (const auto address : objects) {
                    if (auto* object = address.resolve(*manager)) {
                        std::vector<std::string> callbackArgs{std::to_string(object->getId())};
                        callbackArgs.insert(callbackArgs.end(), args.begin() + 3, args.end());
                        if (!con.execute(args[2], callbackArgs).ok()) {
                            return "False";
                        }
                    }
                }
                return "True";
            });
    }
    for (const auto name : {"getNumTeams",
                            "getNumClients",
                            "getTeamName",
                            "setClientScoreHeading",
                            "setTeamScoreHeading",
                            "Team::setObjective",
                            "Team::setScore"}) {
        console.addCommand(
            0,
            name,
            [this, name](Console&, Int32, std::span<const std::string> args) -> std::string {
                auto* manager = wg_ ? wg_->playerManager : nullptr;
                if (!manager) {
                    return "0";
                }
                const std::string_view command(name);
                if (command == "getNumTeams") {
                    return std::to_string(manager->getNumTeams());
                }
                if (command == "getNumClients") {
                    return std::to_string(manager->getNumClients());
                }
                if (args.size() < 2) {
                    return "False";
                }
                if (command == "getTeamName") {
                    auto* team = manager->findTeam(parseIntSv(args[1]));
                    return team ? team->name : "";
                }
                if (command == "setClientScoreHeading") {
                    manager->setClientScoreHeading(args[1]);
                } else if (command == "setTeamScoreHeading") {
                    manager->setTeamScoreHeading(args[1]);
                } else if (command == "Team::setObjective" && args.size() == 4) {
                    manager->setTeamObjective(parseIntSv(args[1]), parseIntSv(args[2]), args[3]);
                } else if (command == "Team::setScore" && args.size() == 4) {
                    manager->setTeamScore(parseIntSv(args[1]), args[2], parseIntSv(args[3]));
                } else {
                    return "False";
                }
                return "True";
            });
    }
    console.addCommand(0,
                       "GameBase::getMapName",
                       [this](Console&, Int32, std::span<const std::string> args) -> std::string {
                           auto* base = args.size() == 2
                                            ? dynamic_cast<GameBase*>(game_.findObject(args[1]))
                                            : nullptr;
                           return base ? std::string(base->getName()) : "";
                       });
    console.addCommand(
        0,
        "GameBase::virtual",
        [this](Console& con, Int32, std::span<const std::string> args) -> std::string {
            auto* manager = sg_.manager ? sg_.manager : game_.manager();
            auto* base = args.size() >= 3
                             ? dynamic_cast<GameBase*>(resolveSimObject(manager, args[1]))
                             : nullptr;
            if (!base) {
                return "False";
            }
            const auto callback = base->scriptName(args[2]);
            if (!con.isFunction(callback)) {
                return "False";
            }
            std::vector<std::string> values{std::to_string(base->getId())};
            values.insert(values.end(), args.begin() + 3, args.end());
            return con.execute(callback, values).value;
        });
    console.addCommand(
        0,
        "addCMCommand",
        [this](Console&, Int32, std::span<const std::string> args) -> std::string {
            auto* menu = args.size() >= 4 && cg_.manager
                             ? dynamic_cast<ChatMenu*>(cg_.manager->findObject(args[1]))
                             : nullptr;
            return menu && menu->addMenuString(args[2], args.subspan(3)) ? "True" : "False";
        });
    console.addCommand(
        0, "setCMMode", [this](Console&, Int32, std::span<const std::string> args) -> std::string {
            auto* menu = args.size() == 3 && cg_.manager
                             ? dynamic_cast<ChatMenu*>(cg_.manager->findObject(args[1]))
                             : nullptr;
            if (!menu) {
                return "False";
            }
            menu->setMode(parseIntSv(args[2]));
            return "True";
        });
    for (const auto name : {"Client::getFirst",
                            "Client::getNext",
                            "Client::getTeam",
                            "Client::getName",
                            "Client::setScore"}) {
        console.addCommand(
            0,
            name,
            [this, name](Console&, Int32, std::span<const std::string> args) -> std::string {
                auto* manager = sg_.playerManager;
                if (!manager) {
                    return "-1";
                }
                const std::string_view command(name);
                if ((command == "Client::getFirst" && args.size() != 1) ||
                    (command == "Client::getNext" && args.size() != 2)) {
                    return "-1";
                }
                auto* client =
                    command == "Client::getFirst"
                        ? manager->getClientList()
                        : (args.size() >= 2 ? (command == "Client::getNext"
                                                   ? manager->findClient(parseIntSv(args[1]))
                                                   : manager->findBaseRep(parseIntSv(args[1])))
                                            : nullptr);
                if (command == "Client::getNext") {
                    client = client ? client->nextClient : nullptr;
                }
                if (!client) {
                    return "-1";
                }
                if (command == "Client::getFirst" || command == "Client::getNext") {
                    return std::to_string(client->id);
                }
                if (command == "Client::getTeam") {
                    return std::to_string(client->team);
                }
                if (command == "Client::getName") {
                    return client->name;
                }
                if (args.size() == 4) {
                    manager->setClientScore(client->id, args[2], parseIntSv(args[3]));
                    return "True";
                }
                return "False";
            });
    }
    console.addCommand(
        0, "getWord", [](Console&, Int32, std::span<const std::string> args) -> std::string {
            if (args.size() != 3) {
                return "-1";
            }
            const auto index = parseIntSv(args[2]);
            if (index < 0) {
                return "-1";
            }
            std::string_view text = args[1];
            for (auto word : std::views::iota(Int32{0})) {
                const auto start = text.find_first_not_of(' ');
                if (start == text.npos) {
                    return "-1";
                }
                text.remove_prefix(start);
                const auto end = text.find(' ');
                if (word == index) {
                    return std::string(text.substr(0, end));
                }
                if (end == text.npos) {
                    return "-1";
                }
                text.remove_prefix(end);
            }
            return "-1";
        });

    console.addCommand(
        0, "newServer", [this](Console& con, Int32, std::span<const std::string> argv) -> std::string {
            const std::string_view mission =
                (argv.size() >= 2) ? std::string_view(argv[1]) : std::string_view{};
            const bool dedicated = argv.size() >= 3
                                       ? parseBoolSv(argv[2])
                                       : parseBoolSv(con.getVariable("dedicated"));
            return createServerWorld(mission, dedicated) ? "True" : "False";
        });

    console.addCommand(
        0, "newClient", [this](Console&, Int32, std::span<const std::string>) -> std::string {
            return createClientWorld() ? "True" : "False";
        });

    console.addCommand(
        0, "deleteServer", [this](Console&, Int32, std::span<const std::string>) -> std::string {
            destroyServerWorld();
            return "True";
        });

    console.addCommand(
        0, "deleteClient", [this](Console&, Int32, std::span<const std::string>) -> std::string {
            destroyClientWorld();
            return "True";
        });

    console.addCommand(
        0, "focusServer", [this](Console&, Int32, std::span<const std::string>) -> std::string {
            return focusServer() ? "True" : "False";
        });

    console.addCommand(
        0, "focusClient", [this](Console&, Int32, std::span<const std::string>) -> std::string {
            return focusClient() ? "True" : "False";
        });

    console.addCommand(0,
                       "preloadServerDataBlocks",
                       [this](Console&, Int32, std::span<const std::string>) -> std::string {
                           if (sg_.dbm) {
                               return sg_.dbm->serverPreload() ? "True" : "False";
                           }
                           return "False";
                       });

    console.addCommand(
        0, "getManagerId", [this](Console&, Int32, std::span<const std::string>) -> std::string {
            if (auto* mgr = game_.manager()) {
                return std::to_string(mgr->getId());
            }
            return "0";
        });

    console.addCommand(
        0, "connect", [this](Console&, Int32, std::span<const std::string> argv) -> std::string {
            if (argv.size() < 2) {
                return "False";
            }
            game_.console().setVariable("OpenT1::LastNetworkError", "");
            game_.console().setVariable("errorString", "");
            if ((!cg_.world || !cg_.playerPSC) && !createClientWorld()) {
                return "False";
            }
            if (argv[1].starts_with("LOOPBACK:")) {
                return game_.console().execute("localConnect").value;
            }
            auto* native = cg_.csDelegate ? cg_.csDelegate->delegate() : nullptr;
            if (!native) {
                return "False";
            }
            const auto address = native->translateAddress(argv[1], kStandardServerPort);
            const auto preference = [this](std::string_view name, std::string_view fallback) {
                const auto value = game_.console().getVariable(name);
                return value.empty() ? std::string(fallback) : value;
            };
            const auto request = cg_.csDelegate->buildConnectRequestPacket(
                game_.console().getVariable("Server::JoinPassword"),
                preference("PCFG::Name", "TribesPlayer"),
                preference("PCFG::Gender", "Male"),
                preference("PCFG::Voice", "male1"),
                preference("PCFG::SkinBase", "base"));
            // Console and +connect calls also need the remote loading lifecycle.
            // The accepted script uses JOIN while datablocks and ghosts arrive.
            game_.console().setVariable("pref::PlayGameMode", "JOIN");
            game_.console().setVariable("SinglePlayer", "false");
            game_.console().setVariable("ConnectedToServer", "false");
            game_.console().setVariable("dataFinished", "false");
            game_.console().setVariable("Server::Address", address.empty() ? argv[1] : address);
            if (onConnecting) {
                onConnecting();
            }
            if (address.empty() ||
                native->connect(address, cg_.currentTime, request) != net::Result::Ok) {
                const auto error = "Could not connect to " + argv[1];
                game_.console().setVariable("OpenT1::LastNetworkError", error);
                game_.console().setVariable("errorString", error);
                if (game_.console().isFunction("onConnection")) {
                    const std::array<std::string, 1> message{"Rejected"};
                    (void)game_.console().execute("onConnection", message);
                }
                return "False";
            }
            connectAddress_ = address;
            game_.console().setVariable("Server::Address", address);
            return "True";
        });

    console.addCommand(
        0, "localConnect", [this](Console&, Int32, std::span<const std::string>) -> std::string {
            if (!sg_.world) {
                return "False";
            }
            if ((!cg_.world || !cg_.playerPSC) && !createClientWorld()) {
                return "False";
            }
            if (!sg_.manager || !sg_.playerManager) {
                return "False";
            }
            const bool connecting = connectAddress_ != "LOOPBACK:28001" ||
                                    !sg_.playerManager->findClient(localClientId_);
            if (connecting) {
                localClientId_ = sg_.playerManager->getFreeId();
                if (!localClientId_) {
                    return "False";
                }
                // Match the accepted remote connection: the new roster may
                // reuse an old AI ID for a player (or the reverse).
                cg_.playerManager->reset();
                cg_.dbm->reset();
                dataFinishedFlag_ = false;
            }
            connectAddress_ = "LOOPBACK:28001";
            game_.console().setVariable("Server::Address", connectAddress_);
            game_.console().setVariable("ConnectedToServer", "true");
            if (cg_.manager) {
                (void)cg_.manager->setNetworkId(localClientId_);
            }
            if (sg_.playerManager) {
                sg_.playerManager->onLocalEvent = [this](std::unique_ptr<sim::SimEvent> event) {
                    if (event && event->address.managerId == UInt32(localClientId_) &&
                        connectAddress_ == "LOOPBACK:28001" && cg_.playerManager) {
                        (void)cg_.playerManager->processEvent(*event);
                    }
                };
            }
            if (!sg_.manager->findObject(localClientId_)) {
                auto client = std::make_unique<sim::SimObject>();
                client->setId(localClientId_);
                client->assignName("LocalClient");
                if (!sg_.manager->addObject(std::move(client))) {
                    return "False";
                }
            }
            if (!sg_.playerManager->findClient(localClientId_)) {
                addLocalPlayer(*sg_.playerManager, game_.console(), localClientId_);
                if (auto* cl = sg_.playerManager->findClient(localClientId_)) {
                    cl->playerPSC = sg_.playerPSC;
                }
                sg_.playerManager->serverClientTeamChange(localClientId_, 0);
            }
            if (scriptPlugin_) {
                auto& clRep = scriptPlugin_->ensureClient(localClientId_);
                if (clRep.observerCameraId == 0 && sg_.manager) {
                    auto* cleanup =
                        dynamic_cast<sim::SimGroup*>(sg_.manager->findObject("MissionCleanup"));
                    auto obsCam = std::make_unique<ObserverCamera>(localClientId_);
                    if (auto* addedCam = sg_.manager->addObject(std::move(obsCam), cleanup)) {
                        clRep.observerCameraId = addedCam->getId();
                    }
                }
            }
            if (connecting) {
                // Match the remote join order. The server initializes/clears
                // client state first; onConnection then sends PCFG::Info and
                // the other profile fields through the script's SetCLInfo.
                // Both must precede datablock completion and mission drop.
                game_.withWorld(sim::WorldType::Server, [&] {
                    if (game_.console().isFunction("Server::onClientConnect")) {
                        const std::array<std::string, 1> args{std::to_string(localClientId_)};
                        (void)game_.console().execute("Server::onClientConnect", args);
                    }
                });
                game_.withWorld(sim::WorldType::Client, [&] {
                    if (game_.console().isFunction("onConnection")) {
                        const std::array<std::string, 1> args{"Accepted"};
                        (void)game_.console().execute("onConnection", args);
                    }
                });
                if (onConnectionAccepted) {
                    onConnectionAccepted();
                }
            }
            (void)syncLoopbackState();
            return "True";
        });

    console.addCommand(
        0, "disconnect", [this](Console&, Int32, std::span<const std::string>) -> std::string {
            if ((cg_.manager && cg_.manager->isDispatching()) ||
                (sg_.manager && sg_.manager->isDispatching())) {
                return "False";
            }
            // Startup mounts and canvases belong to the shell. Keep their
            // script identities when resetting the client; EndGame still needs
            // the shell archives to load the next menu after disconnect().
            std::vector<sim::SimObject*> retained;
            if (cg_.manager) {
                for (auto* object : cg_.manager->objects()) {
                    const auto* volume = dynamic_cast<sim::SimVolume*>(object);
                    if ((volume && !volume->isGhost()) || dynamic_cast<sim::SimCanvas*>(object)) {
                        retained.push_back(object);
                    }
                }
            }
            std::vector<std::unique_ptr<sim::SimObject>> shell;
            for (auto* object : retained) {
                auto owned = cg_.manager->unregisterObject(*object);
                if (!owned) {
                    return "False";
                }
                shell.push_back(std::move(owned));
            }
            const bool local = connectAddress_ == "LOOPBACK:28001";
            connectAddress_.clear();
            destroyClientWorld();
            if (local) {
                destroyServerWorld();
            }
            if (!createClientWorld()) {
                return "False";
            }
            for (auto& object : shell) {
                if (!cg_.manager->addObject(std::move(object))) {
                    return "False";
                }
            }
            return "True";
        });

    console.addCommand(
        0, "kick", [this](Console&, Int32, std::span<const std::string> argv) -> std::string {
            if (argv.size() < 2 || !sg_.csDelegate) {
                return "False";
            }
            std::string_view sv(argv[1]);
            while (!sv.empty() && std::isspace(static_cast<unsigned char>(sv.front())) != 0) {
                sv.remove_prefix(1);
            }
            sim::SimObjectId peerId = 0;
            (void)std::from_chars(sv.data(), sv.data() + sv.size(), peerId);
            const std::string_view reason =
                (argv.size() >= 3) ? std::string_view(argv[2])
                                   : std::string_view("You were kicked from the server.");
            if (peerId == UInt32(localClientId_) && connectAddress_ == "LOOPBACK:28001") {
                // The hosted client has no PacketStream. Retire its worlds
                // after the frame, outside a running script/manager dispatch.
                pendingLocalKick_ = std::string(reason);
                return "True";
            }
            return sg_.csDelegate->kick(peerId, reason, sg_.currentTime) ? "True" : "False";
        });
    console.addCommand(
        0, "Net::kick", [](Console& con, Int32, std::span<const std::string> args) -> std::string {
            return args.size() == 2 || args.size() == 3
                       ? con.execute("kick", args.subspan(1)).value
                       : "False";
        });

    console.addCommand(
        0, "Sad", [this](Console& con, Int32, std::span<const std::string> argv) -> std::string {
            if (argv.size() != 2) {
                return "False";
            }
            sadPassword_ = argv[1];
            con.setVariable("Server::SADPassword", sadPassword_);
            return "True";
        });

    console.addCommand(
        0,
        "SadSetPassword",
        [this](Console& con, Int32, std::span<const std::string> argv) -> std::string {
            if (argv.size() != 2) {
                return "False";
            }
            sadPassword_ = argv[1];
            con.setVariable("Server::SADPassword", sadPassword_);
            return "True";
        });

    console.addCommand(
        0, "quit", [this](Console&, Int32, std::span<const std::string>) -> std::string {
            quitRequested_ = true;
            return "True";
        });

    console.addCommand(
        0, "cursorOn", [this](Console&, Int32, std::span<const std::string>) -> std::string {
            cursorVisible_ = true;
            return "True";
        });

    console.addCommand(
        0, "cursorOff", [this](Console&, Int32, std::span<const std::string>) -> std::string {
            cursorVisible_ = false;
            return "True";
        });

    console.addCommand(
        0, "purgeResources", [this](Console&, Int32, std::span<const std::string>) -> std::string {
            if (resources_) {
                resources_->purge(true);
            }
            return "True";
        });

    console.addCommand(
        0,
        "setFullscreenMode",
        [this](Console&, Int32, std::span<const std::string> argv) -> std::string {
            const auto value = argv.size() > 2 ? argv[2] : argv.size() > 1 ? argv[1] : "true";
            fullscreenMode_ = value == "true" || value == "True" || value == "TRUE" || value == "1";
            return "True";
        });

    console.addCommand(
        0, "setWindowedMode", [this](Console&, Int32, std::span<const std::string>) -> std::string {
            fullscreenMode_ = false;
            return "True";
        });

    console.addCommand(
        0, "nextRes", [this](Console&, Int32, std::span<const std::string>) -> std::string {
            ++resolutionIndex_;
            return std::to_string(resolutionIndex_);
        });

    console.addCommand(
        0, "prevRes", [this](Console&, Int32, std::span<const std::string>) -> std::string {
            if (resolutionIndex_ > 0) {
                --resolutionIndex_;
            }
            return std::to_string(resolutionIndex_);
        });

    console.addCommand(
        0,
        "dataGotBlock",
        [this](Console& con, Int32, std::span<const std::string> argv) -> std::string {
            ++downloadedBlocks_;
            if (argv.size() >= 3) {
                con.setVariable("DataBlock::LastGroup", argv[1]);
                con.setVariable("DataBlock::LastProgress", argv[2]);
            }
            return "True";
        });

    console.addCommand(0,
                       "dataFinished",
                       [this](Console& con, Int32, std::span<const std::string>) -> std::string {
                           dataFinishedFlag_ = true;
                           con.setVariable("DataBlock::Finished", "true");
                           return "True";
                       });

    console.addCommand(
        0,
        "missionSaveObjectPersist",
        [this](Console&, Int32, std::span<const std::string> argv) -> std::string {
            if (argv.size() < 3) {
                return "False";
            }
            auto* obj = game_.findObject(argv[1]);
            if (!obj) {
                obj = resolveSimObject(sg_.manager ? sg_.manager : game_.manager(), argv[1]);
            }
            if (!obj) {
                return "False";
            }
            const auto path = writeRoot_.empty() ? std::filesystem::path(argv[2])
                                                 : (writeRoot_ / std::filesystem::path(argv[2]));
            return game_.storeObject(*obj, path) ? "True" : "False";
        });

    console.addCommand(0,
                       "missionLoadObjectPersist",
                       [this](Console&, Int32, std::span<const std::string> argv) -> std::string {
                           if (argv.size() < 3) {
                               return "False";
                           }
                           if (game_.loadObject(argv[1], argv[2]) != nullptr) {
                               return "True";
                           }
                           const auto path = writeRoot_.empty()
                                                 ? std::filesystem::path(argv[2])
                                                 : (writeRoot_ / std::filesystem::path(argv[2]));
                           std::ifstream in(path, std::ios::binary);
                           if (!in.is_open()) {
                               return "False";
                           }
                           const std::vector<char> raw((std::istreambuf_iterator<char>(in)),
                                                       std::istreambuf_iterator<char>());
                           const std::span<const std::byte> byteSpan(
                               reinterpret_cast<const std::byte*>(raw.data()), raw.size());
                           return game_.loadObject(argv[1], byteSpan) != nullptr ? "True" : "False";
                       });

    console.addCommand(
        0, "rebuildCommandMap", [](Console&, Int32, std::span<const std::string>) -> std::string {
            return "False"; // the application installs the live commander-map owner
        });

    console.addCommand(
        0, "flushTextureCache", [](Console&, Int32, std::span<const std::string>) -> std::string {
            return "True";
        });

    console.addCommand(
        0, "lightTerrain", [](Console&, Int32, std::span<const std::string>) -> std::string {
            return "True";
        });

    // Gameplay & script object manipulation commands
    console.addCommand(0,
                       "GameBase::getPosition",
                       [this](Console&, Int32, std::span<const std::string> argv) -> std::string {
                           auto* mgr = sg_.manager ? sg_.manager : game_.manager();
                           if (argv.size() < 2 || !mgr) {
                               return "0 0 0";
                           }
                           auto* obj = resolveSimObject(mgr, argv[1]);
                           if (const auto* gb = dynamic_cast<const GameBase*>(obj)) {
                               const Point3F p = gb->getLinearPosition();
                               return std::format("{:g} {:g} {:g}", p.x, p.y, p.z);
                           }
                           if (const auto* mk = dynamic_cast<const Marker*>(obj)) {
                               const Point3F p = mk->getLinearPosition();
                               return std::format("{:g} {:g} {:g}", p.x, p.y, p.z);
                           }
                           if (const auto* tr = dynamic_cast<const Trigger*>(obj)) {
                               const Point3F p = tr->getLinearPosition();
                               return std::format("{:g} {:g} {:g}", p.x, p.y, p.z);
                           }
                           return "0 0 0";
                       });

    console.addCommand(0,
                       "GameBase::setPosition",
                       [this](Console&, Int32, std::span<const std::string> argv) -> std::string {
                           auto* mgr = sg_.manager ? sg_.manager : game_.manager();
                           if (argv.size() < 3 || !mgr) {
                               return "False";
                           }
                           const Point3F pos = parsePoint3FSv(argv[2]);
                           auto* obj = resolveSimObject(mgr, argv[1]);
                           if (auto* sb = dynamic_cast<ShapeBase*>(obj)) {
                               sb->setPos(pos);
                               return "True";
                           }
                           if (auto* gb = dynamic_cast<GameBase*>(obj)) {
                               TMat3F mat = gb->getTransform();
                               mat.p = pos;
                               gb->setPosition(mat, true);
                               return "True";
                           }
                           if (auto* mk = dynamic_cast<Marker*>(obj)) {
                               TMat3F mat = mk->getTransform();
                               mat.p = pos;
                               mk->setPosition(mat, true);
                               return "True";
                           }
                           if (auto* tr = dynamic_cast<Trigger*>(obj)) {
                               TMat3F mat = tr->getTransform();
                               mat.p = pos;
                               tr->setPosition(mat, true);
                               return "True";
                           }
                           return "False";
                       });

    console.addCommand(0,
                       "GameBase::getRotation",
                       [this](Console&, Int32, std::span<const std::string> argv) -> std::string {
                           auto* mgr = sg_.manager ? sg_.manager : game_.manager();
                           if (argv.size() < 2 || !mgr) {
                               return "0 0 0";
                           }
                           if (const auto* object =
                                   dynamic_cast<const GameBase*>(resolveSimObject(mgr, argv[1]))) {
                               EulerF r;
                               object->getTransform().angles(r);
                               return std::format("{:g} {:g} {:g}", r.x, r.y, r.z);
                           }
                           return "0 0 0";
                       });

    console.addCommand(0,
                       "GameBase::setRotation",
                       [this](Console&, Int32, std::span<const std::string> argv) -> std::string {
                           auto* mgr = sg_.manager ? sg_.manager : game_.manager();
                           if (argv.size() < 3 || !mgr) {
                               return "False";
                           }
                           const Point3F rot = parsePoint3FSv(argv[2]);
                           if (auto* sb =
                                   dynamic_cast<ShapeBase*>(resolveSimObject(mgr, argv[1]))) {
                               sb->setRot(rot);
                               return "True";
                           }
                           return "False";
                       });

    console.addCommand(0,
                       "GameBase::getTeam",
                       [this](Console&, Int32, std::span<const std::string> argv) -> std::string {
                           auto* mgr = sg_.manager ? sg_.manager : game_.manager();
                           if (argv.size() < 2 || !mgr) {
                               return "-1";
                           }
                           if (const auto* gb =
                                   dynamic_cast<const GameBase*>(mgr->findObject(argv[1]))) {
                               return std::to_string(gb->getTeam());
                           }
                           const auto clientId = parseIntSv(argv[1]);
                           if (clientId >= 2049 && clientId <= 2176 && sg_.playerManager) {
                               if (const auto* client = sg_.playerManager->findBaseRep(clientId)) {
                                   return std::to_string(client->team);
                               }
                           }
                           return "-1";
                       });

    console.addCommand(0,
                       "GameBase::setTeam",
                       [this](Console&, Int32, std::span<const std::string> argv) -> std::string {
                           auto* mgr = sg_.manager ? sg_.manager : game_.manager();
                           if (argv.size() < 3 || !mgr) {
                               return "False";
                           }
                           if (auto* gb = dynamic_cast<GameBase*>(resolveSimObject(mgr, argv[1]))) {
                               gb->setTeam(parseIntSv(argv[2]));
                               return "True";
                           }
                           return "False";
                       });

    console.addCommand(0,
                       "GameBase::getDamageLevel",
                       [this](Console&, Int32, std::span<const std::string> argv) -> std::string {
                           auto* mgr = sg_.manager ? sg_.manager : game_.manager();
                           if (argv.size() < 2 || !mgr) {
                               return "0";
                           }
                           if (const auto* sb =
                                   dynamic_cast<const ShapeBase*>(resolveSimObject(mgr, argv[1]))) {
                               return std::format("{:g}", sb->getDamageLevel());
                           }
                           return "0";
                       });

    console.addCommand(0,
                       "GameBase::setDamageLevel",
                       [this](Console&, Int32, std::span<const std::string> argv) -> std::string {
                           auto* mgr = sg_.manager ? sg_.manager : game_.manager();
                           if (argv.size() < 3 || !mgr) {
                               return "False";
                           }
                           if (auto* sb =
                                   dynamic_cast<ShapeBase*>(resolveSimObject(mgr, argv[1]))) {
                               sb->setDamageLevel(parseFloatSv(argv[2]));
                               return "True";
                           }
                           return "False";
                       });

    console.addCommand(0,
                       "GameBase::applyDamage",
                       [this](Console&, Int32, std::span<const std::string> argv) -> std::string {
                           auto* mgr = sg_.manager ? sg_.manager : game_.manager();
                           if (argv.size() != 8 || !mgr) {
                               return "False";
                           }
                           if (auto* sb =
                                   dynamic_cast<ShapeBase*>(resolveSimObject(mgr, argv[1]))) {
                               Point3F pos{}, vec{}, momentum{};
                               std::istringstream positionInput(argv[4]), vectorInput(argv[5]),
                                   momentumInput(argv[6]);
                               if (!(positionInput >> pos.x >> pos.y >> pos.z) ||
                                   !(vectorInput >> vec.x >> vec.y >> vec.z) ||
                                   !(momentumInput >> momentum.x >> momentum.y >> momentum.z)) {
                                   return "False";
                               }
                               sb->applyDamage(parseIntSv(argv[2]),
                                               parseFloatSv(argv[3]),
                                               pos,
                                               vec,
                                               momentum,
                                               static_cast<sim::SimObjectId>(parseIntSv(argv[7])));
                               return "True";
                           }
                           return "False";
                       });

    console.addCommand(0,
                       "GameBase::getEnergy",
                       [this](Console&, Int32, std::span<const std::string> argv) -> std::string {
                           auto* mgr = sg_.manager ? sg_.manager : game_.manager();
                           if (argv.size() < 2 || !mgr) {
                               return "0";
                           }
                           if (const auto* sb =
                                   dynamic_cast<const ShapeBase*>(resolveSimObject(mgr, argv[1]))) {
                               return std::format("{:g}", sb->getEnergy());
                           }
                           return "0";
                       });

    console.addCommand(0,
                       "GameBase::setEnergy",
                       [this](Console&, Int32, std::span<const std::string> argv) -> std::string {
                           auto* mgr = sg_.manager ? sg_.manager : game_.manager();
                           if (argv.size() < 3 || !mgr) {
                               return "False";
                           }
                           if (auto* sb =
                                   dynamic_cast<ShapeBase*>(resolveSimObject(mgr, argv[1]))) {
                               sb->setEnergy(parseFloatSv(argv[2]));
                               return "True";
                           }
                           return "False";
                       });

    console.addCommand(0,
                       "GameBase::getDataName",
                       [this](Console&, Int32, std::span<const std::string> argv) -> std::string {
                           auto* mgr = sg_.manager ? sg_.manager : game_.manager();
                           if (argv.size() < 2 || !mgr) {
                               return "";
                           }
                           if (const auto* gb =
                                   dynamic_cast<const GameBase*>(resolveSimObject(mgr, argv[1]))) {
                               return std::string(gb->getDatFileName());
                           }
                           return "";
                       });

    console.addCommand(0,
                       "Player::setItemCount",
                       [this](Console&, Int32, std::span<const std::string> argv) -> std::string {
                           auto* mgr = sg_.manager ? sg_.manager : game_.manager();
                           if (argv.size() < 4 || !mgr) {
                               return "False";
                           }
                           if (auto* pl = dynamic_cast<Player*>(resolveSimObject(mgr, argv[1]))) {
                               const Int32 typeIdx = resolveItemTypeIndex(sg_.dbm, argv[2]);
                               return pl->setItemCount(typeIdx, parseIntSv(argv[3])) ? "True"
                                                                                     : "False";
                           }
                           return "False";
                       });

    console.addCommand(0,
                       "Player::getItemCount",
                       [this](Console&, Int32, std::span<const std::string> argv) -> std::string {
                           auto* mgr = sg_.manager ? sg_.manager : game_.manager();
                           if (argv.size() < 3 || !mgr) {
                               return "0";
                           }
                           if (const auto* pl =
                                   dynamic_cast<const Player*>(resolveSimObject(mgr, argv[1]))) {
                               const Int32 typeIdx = resolveItemTypeIndex(sg_.dbm, argv[2]);
                               return std::to_string(pl->getItemCount(typeIdx));
                           }
                           return "0";
                       });

    console.addCommand(0,
                       "Player::useItem",
                       [this](Console&, Int32, std::span<const std::string> argv) -> std::string {
                           auto* mgr = sg_.manager ? sg_.manager : game_.manager();
                           if (argv.size() < 3 || !mgr) {
                               return "False";
                           }
                           if (auto* pl = dynamic_cast<Player*>(resolveSimObject(mgr, argv[1]))) {
                               const Int32 typeIdx = resolveItemTypeIndex(sg_.dbm, argv[2]);
                               return pl->useItem(typeIdx) ? "True" : "False";
                           }
                           return "False";
                       });

    console.addCommand(0,
                       "Player::mountItem",
                       [this](Console&, Int32, std::span<const std::string> argv) -> std::string {
                           auto* mgr = sg_.manager ? sg_.manager : game_.manager();
                           if (argv.size() < 4 || !mgr) {
                               return "False";
                           }
                           if (auto* pl = dynamic_cast<Player*>(resolveSimObject(mgr, argv[1]))) {
                               const Int32 typeIdx = resolveItemTypeIndex(sg_.dbm, argv[2]);
                               const Int32 slot = parseIntSv(argv[3]);
                               const Int32 team =
                                   (argv.size() >= 5) ? parseIntSv(argv[4]) : pl->getTeam();
                               return pl->mountItem(typeIdx, slot, team) ? "True" : "False";
                           }
                           return "False";
                       });

    console.addCommand(
        0,
        "Player::getMountedItem",
        [this](Console&, Int32, std::span<const std::string> argv) -> std::string {
            auto* mgr = sg_.manager ? sg_.manager : game_.manager();
            if (argv.size() < 3 || !mgr) {
                return "-1";
            }
            if (const auto* pl = dynamic_cast<const Player*>(resolveSimObject(mgr, argv[1]))) {
                const Int32 itemIdx = pl->getMountedItem(parseIntSv(argv[2]));
                if (itemIdx >= 0 && sg_.dbm) {
                    const auto name =
                        sg_.dbm->lookupBlockName(itemIdx, DataBlockManager::ItemDataType);
                    if (!name.empty()) {
                        return std::string(name);
                    }
                }
                return std::to_string(itemIdx);
            }
            return "-1";
        });

    console.addCommand(0,
                       "Player::trigger",
                       [this](Console&, Int32, std::span<const std::string> argv) -> std::string {
                           auto* mgr = sg_.manager ? sg_.manager : game_.manager();
                           if (argv.size() < 3 || !mgr) {
                               return "False";
                           }
                           if (auto* pl = dynamic_cast<Player*>(resolveSimObject(mgr, argv[1]))) {
                               const Int32 slot = parseIntSv(argv[2]);
                               const bool down = (argv.size() < 4) || parseBoolSv(argv[3]);
                               if (down) {
                                   pl->setImageTriggerDown(slot);
                               } else {
                                   pl->setImageTriggerUp(slot);
                               }
                               return "True";
                           }
                           return "False";
                       });

    console.addCommand(0,
                       "Player::isDead",
                       [this](Console&, Int32, std::span<const std::string> argv) -> std::string {
                           auto* mgr = sg_.manager ? sg_.manager : game_.manager();
                           if (argv.size() < 2 || !mgr) {
                               return "False";
                           }
                           if (const auto* pl =
                                   dynamic_cast<const Player*>(resolveSimObject(mgr, argv[1]))) {
                               return pl->isDead() ? "True" : "False";
                           }
                           return "False";
                       });

    console.addCommand(0,
                       "Player::kill",
                       [this](Console&, Int32, std::span<const std::string> argv) -> std::string {
                           auto* mgr = sg_.manager ? sg_.manager : game_.manager();
                           if (argv.size() < 2 || !mgr) {
                               return "False";
                           }
                           if (auto* pl = dynamic_cast<Player*>(resolveSimObject(mgr, argv[1]))) {
                               pl->setDamageLevel(1.0f);
                               pl->setDead(true);
                               return "True";
                           }
                           return "False";
                       });

    console.addCommand(0,
                       "Client::getOwnedObject",
                       [this](Console&, Int32, std::span<const std::string> argv) -> std::string {
                           if (argv.size() < 2 || !sg_.playerManager) {
                               return "-1";
                           }
                           if (const auto* cl =
                                   sg_.playerManager->findBaseRep(parseIntSv(argv[1]))) {
                               if (cl->ownedObject) {
                                   return std::to_string(cl->ownedObject->getId());
                               }
                           }
                           return "-1";
                       });

    console.addCommand(0,
                       "Client::getControlObject",
                       [this](Console&, Int32, std::span<const std::string> argv) -> std::string {
                           if (argv.size() < 2 || !sg_.playerManager) {
                               return "-1";
                           }
                           if (const auto* cl =
                                   sg_.playerManager->findClient(parseIntSv(argv[1]))) {
                               if (cl->controlObject) {
                                   return std::to_string(cl->controlObject->getId());
                               }
                           }
                           return "-1";
                       });

    console.addCommand(0,
                       "Client::setControlObject",
                       [this](Console&, Int32, std::span<const std::string> argv) -> std::string {
                           if (argv.size() < 3 || !sg_.playerManager || !sg_.manager) {
                               return "False";
                           }
                           auto* cl = sg_.playerManager->findClient(parseIntSv(argv[1]));
                           auto* gb =
                               dynamic_cast<GameBase*>(resolveSimObject(sg_.manager, argv[2]));
                           if (!cl) {
                               return "False";
                           }
                           if (cl->controlObject && cl->controlObject != gb) {
                               cl->controlObject->setControlClient(0);
                           }
                           if (gb) {
                               gb->setControlClient(cl->id);
                           }
                           if (cl->playerPSC) {
                               cl->playerPSC->setControlObject(gb);
                           }
                           return "True";
                       });
}

void FearPlugin::shutdown() noexcept {
    if (console_) {
        world::uninstallTerrainEditorCommands(*console_);
        console_->removeVariable("net::interpolateTime");
        console_->removeVariable("net::predictForwardTime");
        console_->removeCommand("newServer");
        console_->removeCommand("newClient");
        console_->removeCommand("deleteServer");
        console_->removeCommand("deleteClient");
        console_->removeCommand("focusServer");
        console_->removeCommand("focusClient");
        console_->removeCommand("preloadServerDataBlocks");
        console_->removeCommand("getManagerId");
        console_->removeCommand("connect");
        console_->removeCommand("localConnect");
        console_->removeCommand("disconnect");
        console_->removeCommand("kick");
        console_->removeCommand("Net::kick");
        console_->removeCommand("Sad");
        console_->removeCommand("SadSetPassword");
        console_->removeCommand("quit");
        console_->removeCommand("cursorOn");
        console_->removeCommand("cursorOff");
        console_->removeCommand("purgeResources");
        console_->removeCommand("setFullscreenMode");
        console_->removeCommand("setWindowedMode");
        console_->removeCommand("nextRes");
        console_->removeCommand("prevRes");
        console_->removeCommand("dataGotBlock");
        console_->removeCommand("dataFinished");
        console_->removeCommand("missionSaveObjectPersist");
        console_->removeCommand("missionLoadObjectPersist");
        console_->removeCommand("rebuildCommandMap");
        console_->removeCommand("flushTextureCache");
        console_->removeCommand("lightTerrain");
        console_->removeCommand("GameBase::getPosition");
        console_->removeCommand("GameBase::setPosition");
        console_->removeCommand("GameBase::getRotation");
        console_->removeCommand("GameBase::setRotation");
        console_->removeCommand("GameBase::getTeam");
        console_->removeCommand("GameBase::setTeam");
        console_->removeCommand("GameBase::getDamageLevel");
        console_->removeCommand("GameBase::setDamageLevel");
        console_->removeCommand("GameBase::applyDamage");
        console_->removeCommand("GameBase::getEnergy");
        console_->removeCommand("GameBase::setEnergy");
        console_->removeCommand("GameBase::getDataName");
        console_->removeCommand("Player::setItemCount");
        console_->removeCommand("Player::getItemCount");
        console_->removeCommand("Player::useItem");
        console_->removeCommand("Player::mountItem");
        console_->removeCommand("Player::getMountedItem");
        console_->removeCommand("Player::trigger");
        console_->removeCommand("Player::isDead");
        console_->removeCommand("Player::kill");
        console_->removeCommand("Client::getOwnedObject");
        console_->removeCommand("Client::getControlObject");
        console_->removeCommand("Client::setControlObject");
    }
    terrainEditor_.reset();
    editedTerrain_ = {};
    serverToClientGhosts_.clear();
    loopbackSourceGenerations_.clear();
    dataFinishedFlag_ = false;
    sg_.clear();
    cg_.clear();
    wg_ = &cg_;
    console_ = nullptr;
    manager_ = nullptr;
}

void FearPlugin::setManager(sim::SimManager* manager) {
    manager_ = manager;
    refreshActiveGlobals();
}

void FearPlugin::startFrame() {
    const auto nowMs = static_cast<UInt32>(game_.simulationTime() * 1000.0);
    if (sg_.world) {
        sg_.lastTime = sg_.currentTime;
        sg_.currentTime = nowMs;
    }
    if (cg_.world) {
        cg_.lastTime = cg_.currentTime;
        cg_.currentTime = nowMs;
        cg_.clockTime += Flt32(cg_.currentTime - cg_.lastTime) * .001f;
    }
    refreshActiveGlobals();
    // Legacy collects this frame's moves before receiving corrections and
    // sending packets. Replay then maps the remaining queue to this same time.
    if (cg_.playerPSC && cg_.playerPSC->getControlObject()) {
        cg_.playerPSC->clientCollectInput(cg_.lastTime, nowMs, .032f);
    }
}

void FearPlugin::endFrame() {
    updateRemoteControls();
    const auto nowMs = static_cast<UInt32>(game_.simulationTime() * 1000.0);
    // Limit catch-up after a pause; simulation and input share one 32 ms clock.
    if (nowMs - serverSimulationTime_ > 256u) {
        serverSimulationTime_ = nowMs - 256u;
    }
    while (nowMs - serverSimulationTime_ >= 32u) {
        const auto next = serverSimulationTime_ + 32u;
        if (sg_.world) {
            sg_.currentTime = next;
            if (connectAddress_ == "LOOPBACK:28001") {
                (void)syncLoopbackState();
            }
            game_.withWorld(sim::WorldType::Server, [this] {
                stepServerSimulation();
            });
        }
        serverSimulationTime_ = next;
    }
    if (cg_.world) {
        cg_.currentTime = nowMs;
        // Authoritative packets reset the controlled transform and replay clock.
        // Apply them before prediction so the rendered frame keeps its fraction
        // of the current tick, rather than snapping back to the server pose.
        if (connectAddress_ == "LOOPBACK:28001") {
            (void)syncLoopbackState(false);
        }
        game_.withWorld(sim::WorldType::Client, [this] {
            stepClientSimulation();
        });
    }
    refreshActiveGlobals();
}

// -----------------------------------------------------------------------------
// FearGameApp
// -----------------------------------------------------------------------------

FearGameApp::FearGameApp() : FearGameApp(Config{}) {}

FearGameApp::FearGameApp(Config config)
    : config_(std::move(config)), resources_(std::make_shared<ResourceManager>()),
      banList_(std::make_shared<BanList>()), game_(resources_) {
    game_.console().setFileRoot(config_.writeRoot);
    game_.console().bindVariable("OpenT1::MountedVolumes",
                                 [this] {
                                     return std::to_string(resources_->mountedVolumes().size());
                                 },
                                 {});
    game_.console().bindVariable(
        "ConsoleWorld::DefaultSearchPath",
        [this] {
            return searchPath_;
        },
        [this](std::string_view value) {
            if (applySearchPath(value)) {
                searchPath_ = value;
            } else {
                game_.console().print("Cannot apply resource search path: " + std::string(value));
            }
        });
    filePlugin_ = std::make_shared<FilePlugin>(resources_, banList_, config_.writeRoot);
    scriptPlugin_ = std::make_shared<ScriptPlugin>();
    dynamicDataPlugin_ = std::make_shared<FearDynamicDataPlugin>(resources_, config_.writeRoot);
    fearPlugin_ = std::make_shared<FearPlugin>(
        game_, resources_, banList_, dynamicDataPlugin_, config_.writeRoot, scriptPlugin_);
    aiPlugin_ = std::make_shared<AIPlugin>();
    missionPlugin_ = std::make_shared<FearMissionPlugin>(&game_);
    timeDemoPlugin_ = std::make_shared<TimeDemoPlugin>();

    scriptPlugin_->setSpawnPlayerCallback(
        [this](sim::SimManager* /*srvMgr*/,
               std::string_view armorName,
               const Point3F& pos,
               const Point3F& rot) -> std::optional<sim::SimObjectId> {
            return fearPlugin_->spawnPlayer(armorName, pos, rot);
        });

    listenerTransform_.identity();
    fearPlugin_->onGuiMode = [this](UInt32 mode) {
        (void)setGameplayGuiMode(mode);
    };
    fearPlugin_->onScoresVisible = [this](bool visible) {
        showGameplayScores(visible);
    };
    fearPlugin_->onChatMessage = [this](Int32 type, std::string_view message) {
        displayChatMessage(type, message);
    };
    fearPlugin_->onConnecting = [this] {
        if (!guiRuntime_) {
            return;
        }
        guiRuntime_->canvas.cursorVisible = true;
        gameplayGuiMode_ = 1;
        showingScores_ = false;
        if (guiRuntime_->canvas.top() && guiRuntime_->canvas.top()->getName() == "ConnectingGui") {
            return;
        }
        if (!guiRuntime_->open("gui/Connecting.gui")) {
            game_.console().setVariable("OpenT1::LastGuiError", "Cannot load gui/Connecting.gui");
        }
    };
    fearPlugin_->onConnectionAccepted = [this] {
        // Preserve a loading graph opened by the client script, including any
        // custom controls. Supply the native transition when it leaves the
        // connecting graph in place.
        if (guiRuntime_ && guiRuntime_->canvas.top() &&
            guiRuntime_->canvas.top()->getName() == "ConnectingGui" &&
            !guiRuntime_->open("gui/Loading.gui")) {
            game_.console().setVariable("OpenT1::LastGuiError", "Cannot load gui/Loading.gui");
        }
    };
    fearPlugin_->onPlayerControl = [this] {
        (void)setGameplayGuiMode(gameplayGuiMode_ >= 1 && gameplayGuiMode_ <= 6 ? gameplayGuiMode_
                                                                                : 1);
        updateGameplayPalette();
    };
    registerGameClassesAndFactories();
}

void FearGameApp::registerGameClassesAndFactories() {
    // 1. Register resource loaders
    gfx::registerPaletteLoaders(*resources_);
    gfx::registerBitmapFontLoaders(*resources_);
    sim::registerSimActionMapResource(*resources_);
    sim::registerBayesResource(*resources_);
    world::registerTsLoaders(*resources_);
    world::registerTerrainLoaders(*resources_);
    world::registerInteriorLoaders(*resources_);
    video::registerSmackerLoader(*resources_);

    // 2. Register persistent classes on SimGame's ClassRegistry
    auto& classes = game_.classes();
    sim::registerSimInputClasses(classes);
    classes.registerClass<FearMissionTerrain>("SimTerrain", Persistent::kPersTag, 2);
    classes.registerClass<InteriorShape>(
        "InteriorShape", Persistent::kPersTag, InteriorShapePersTag);
    world::registerWorldMissionClasses(classes);
    world::registerSceneObjectClasses(classes);
    registerBaseSimClasses(classes);
    registerPlayerClass(classes);
    registerChatMenuClass(classes);
    registerProjectileClasses(classes);
    registerDebrisClasses(classes);
    registerItemClass(classes);
    registerTurretClass(classes);
    registerMineClass(classes);
    registerSensorClass(classes);
    registerVehicleClass(classes);
    registerFlierClass(classes);
    registerExplosionClass(classes);
    registerFearGuiClasses(classes);
    registerMissionEditorGuiClasses(classes);
    gui::registerViewportClass(classes);

    classes.registerClass<Sky>("Sky", TribesSkyPersTag);
    classes.registerClass<StarField>("StarField", StarFieldPersTag);
    classes.registerClass<Planet>("Planet", PlanetPersTag);
    classes.registerClass<Snowfall>("Snowfall", SnowfallPersTag);
    classes.registerClass<Trigger>("Trigger", TriggerPersTag);
    classes.registerClass<DropPointGroup>("DropPointGroup");
    classes.registerClass<TeamGroup>("TeamGroup");
    classes.registerClass<DecalManager>("DecalManager");
    classes.registerClass<FlyingCamera>("FlyingCamera");
    classes.registerClass<ObserverCamera>("ObserverCamera", ObserverCamPersTag);
    classes.registerClass<EditCamera>("EditCamera");
    classes.registerClass<AIManager>("AIManager");
    classes.registerClass<FearMissionEditor>("FearMissionEditor");
    classes.registerClass<Marker>("Marker", MarkerPersTag);
    classes.registerClass<Marker>("DropPointMarker", FearDropPointMarkerPersTag);
    classes.registerClass<Marker>("MapMarker", FearMissionMarkerPersTag);
    classes.registerClass<EmfInterior>("SimInterior");
    classes.registerClass<SensorManager>("SensorManager");

    // 3. Register script/console factories that auto-bind active world managers
    auto bindGameBase = [this](auto uniqObj) -> std::unique_ptr<sim::SimObject> {
        if (auto* mgr = game_.manager()) {
            if (auto* dbm = dynamic_cast<DataBlockManager*>(mgr->findObject(DataBlockManagerId))) {
                uniqObj->bindDataBlockManager(dbm);
            }
        } else if (fearPlugin_ && fearPlugin_->activeGlobals() &&
                   fearPlugin_->activeGlobals()->dbm) {
            uniqObj->bindDataBlockManager(fearPlugin_->activeGlobals()->dbm);
        }
        uniqObj->bindConsole(&game_.console());
        return uniqObj;
    };

    game_.registerFactory("GameBase", [bindGameBase]() {
        return bindGameBase(std::make_unique<GameBase>());
    });
    game_.registerFactory("ShapeBase", [bindGameBase]() {
        return bindGameBase(std::make_unique<ShapeBase>());
    });
    game_.registerFactory("StaticBase", [bindGameBase]() {
        return bindGameBase(std::make_unique<StaticBase>());
    });
    game_.registerFactory("StaticShape", [bindGameBase]() {
        return bindGameBase(std::make_unique<StaticShape>());
    });
    game_.registerFactory("MoveableBase", [this, bindGameBase]() {
        auto obj = std::make_unique<MoveableBase>();
        if (auto* mgr = game_.manager()) {
            if (auto* pm =
                    dynamic_cast<world::SimPathManager*>(mgr->findObject(world::PathManagerId))) {
                obj->bindPathManager(pm);
            }
        }
        return bindGameBase(std::move(obj));
    });
    game_.registerFactory("Moveable", [this, bindGameBase]() {
        auto obj = std::make_unique<Moveable>();
        if (auto* mgr = game_.manager()) {
            if (auto* pm =
                    dynamic_cast<world::SimPathManager*>(mgr->findObject(world::PathManagerId))) {
                obj->bindPathManager(pm);
            }
        }
        return bindGameBase(std::move(obj));
    });
    game_.registerFactory("Player", [bindGameBase]() {
        return bindGameBase(std::make_unique<Player>());
    });
    game_.registerFactory("Item", [bindGameBase]() {
        return bindGameBase(std::make_unique<Item>());
    });
    game_.registerFactory("Sensor", [bindGameBase]() {
        return bindGameBase(std::make_unique<Sensor>());
    });
    game_.registerFactory("Turret", [bindGameBase]() {
        return bindGameBase(std::make_unique<Turret>());
    });
    game_.registerFactory("Mine", [bindGameBase]() {
        return bindGameBase(std::make_unique<Mine>());
    });
    game_.registerFactory("Vehicle", [bindGameBase]() {
        return bindGameBase(std::make_unique<Vehicle>());
    });
    game_.registerFactory("Flier", [bindGameBase]() {
        return bindGameBase(std::make_unique<Flier>());
    });
    game_.registerFactory("Explosion", [bindGameBase]() {
        return bindGameBase(std::make_unique<Explosion>());
    });
    game_.registerFactory("Debris", [bindGameBase]() {
        return bindGameBase(std::make_unique<Debris>());
    });
    game_.registerFactory("PartDebrisCloud", [this]() -> std::unique_ptr<sim::SimObject> {
        auto obj = std::make_unique<PartDebrisCloud>(std::vector<std::unique_ptr<PartDebris>>{});
        if (auto* mgr = game_.manager()) {
            if (auto* dbm = dynamic_cast<DataBlockManager*>(mgr->findObject(DataBlockManagerId))) {
                obj->bindDataBlockManager(dbm);
            }
        }
        return obj;
    });
    game_.registerFactory("Bullet", [bindGameBase]() {
        return bindGameBase(std::make_unique<Bullet>());
    });
    game_.registerFactory("Grenade", [bindGameBase]() {
        return bindGameBase(std::make_unique<Grenade>());
    });
    game_.registerFactory("Rocket", [bindGameBase]() {
        return bindGameBase(std::make_unique<RocketDumb>());
    });
    game_.registerFactory("RocketDumb", [bindGameBase]() {
        return bindGameBase(std::make_unique<RocketDumb>());
    });
    game_.registerFactory("LaserProjectile", [bindGameBase]() {
        return bindGameBase(std::make_unique<LaserProjectile>());
    });
    game_.registerFactory("TargetLaser", [bindGameBase]() {
        return bindGameBase(std::make_unique<TargetLaser>());
    });
    game_.registerFactory("SeekingMissile", [bindGameBase]() {
        return bindGameBase(std::make_unique<SeekingMissile>());
    });
    game_.registerFactory("SeekerProjectile", [bindGameBase]() {
        return bindGameBase(std::make_unique<SeekingMissile>());
    });
    game_.registerFactory("Lightning", [bindGameBase]() {
        return bindGameBase(std::make_unique<Lightning>());
    });
    game_.registerFactory("RepairEffect", [bindGameBase]() {
        return bindGameBase(std::make_unique<RepairEffect>());
    });

    game_.registerFactory("Sky", []() {
        return std::make_unique<Sky>();
    });
    game_.registerFactory("StarField", []() {
        return std::make_unique<StarField>();
    });
    game_.registerFactory("Planet", []() {
        return std::make_unique<Planet>();
    });
    game_.registerFactory("Snowfall", []() {
        auto obj = std::make_unique<Snowfall>();
        obj->setId(0);
        return obj;
    });
    game_.registerFactory("MissionCenterPos", []() {
        return std::make_unique<MissionCenterPos>();
    });
    game_.registerFactory("InteriorShape", [bindGameBase]() {
        return bindGameBase(std::make_unique<InteriorShape>());
    });
    game_.registerFactory("SimInterior", [bindGameBase]() {
        return bindGameBase(std::make_unique<EmfInterior>());
    });
    game_.registerFactory("SimPath", []() {
        return std::make_unique<FearMissionPath>();
    });
    game_.registerFactory("SimLight", []() {
        return std::make_unique<world::SimSceneLight>();
    });
    game_.registerFactory("Marker", [bindGameBase]() {
        return bindGameBase(std::make_unique<Marker>());
    });
    game_.registerFactory("DropPointMarker", [bindGameBase]() {
        return bindGameBase(std::make_unique<Marker>());
    });
    game_.registerFactory("MapMarker", [bindGameBase]() {
        return bindGameBase(std::make_unique<Marker>());
    });
    game_.registerFactory("Trigger", [bindGameBase]() {
        return bindGameBase(std::make_unique<Trigger>());
    });
    game_.registerFactory("DropPointGroup", []() {
        return std::make_unique<DropPointGroup>();
    });
    game_.registerFactory("TeamGroup", []() {
        return std::make_unique<TeamGroup>();
    });
    game_.registerFactory("DecalManager", []() {
        return std::make_unique<DecalManager>();
    });
    game_.registerFactory("FlyingCamera", []() {
        return std::make_unique<FlyingCamera>();
    });
    game_.registerFactory("ObserverCamera", []() {
        return std::make_unique<ObserverCamera>();
    });
    game_.registerFactory("EditCamera", [this]() {
        auto camera = std::make_unique<EditCamera>();
        camera->loadActionMap = [this](std::string_view name) {
            return inputMaps_.find(name);
        };
        return camera;
    });
    game_.registerFactory("AIManager", []() {
        return std::make_unique<AIManager>();
    });
    game_.registerFactory("FearMissionEditor", []() {
        return std::make_unique<FearMissionEditor>();
    });
    game_.registerFactory("PlayerManager", [this]() {
        const bool isSrv = (fearPlugin_ && game_.manager() == fearPlugin_->serverGlobals().manager);
        auto pm = std::make_unique<PlayerManager>(isSrv);
        pm->bindConsole(&game_.console());
        return pm;
    });
    game_.registerFactory("SensorManager", []() {
        return std::make_unique<SensorManager>();
    });
    game_.registerFactory("IRCClient", [] {
        return std::make_unique<IRCClient>();
    });
    game_.registerFactory("ChatMenu", [this]() {
        auto cm = std::make_unique<ChatMenu>("Options:");
        cm->bindConsole(&game_.console());
        return cm;
    });
    game_.registerFactory("SimPalette", [this]() {
        return std::make_unique<FearSimPalette>(resources_,
                                                [this](std::shared_ptr<gfx::Palette> pal) {
                                                    setActivePalette(std::move(pal));
                                                });
    });
    game_.registerFactory("SimTerrain", []() {
        return std::make_unique<FearMissionTerrain>();
    });

    game_.console().addCommand(
        0,
        "loadMission",
        [this](Console&, Int32, std::span<const std::string> argv) -> std::string {
            if (argv.size() < 2 || argv[1].empty()) {
                return "False";
            }
            return loadMission(argv[1]) ? "True" : "False";
        });
}

FearGameApp::~FearGameApp() {
    shutdown();
}

std::filesystem::path FearGameApp::discoverAssetRoot(const std::filesystem::path& startDir) {
    std::error_code ec;
    std::filesystem::path cur = startDir.empty() ? std::filesystem::current_path(ec) : startDir;
    if (cur.empty()) {
        return {};
    }

    auto hasBaseVolumes = [](const std::filesystem::path& candidate) noexcept -> bool {
        std::error_code localEc;
        if (candidate.empty() || !std::filesystem::is_directory(candidate, localEc)) {
            return false;
        }
        const auto baseDir = candidate / "base";
        return std::filesystem::is_regular_file(baseDir / "entities.vol", localEc) ||
               std::filesystem::is_regular_file(baseDir / "Entities.vol", localEc) ||
               std::filesystem::is_regular_file(baseDir / "darkstar.vol", localEc);
    };

    if (std::filesystem::is_regular_file(cur / "entities.vol", ec) ||
        std::filesystem::is_regular_file(cur / "Entities.vol", ec)) {
        if (hasBaseVolumes(cur.parent_path())) {
            return cur.parent_path().lexically_normal();
        }
    }

    for ([[maybe_unused]] auto depth : std::views::iota(0) | std::views::take_while([&](int depth) {
                                           return depth < 7 && !cur.empty();
                                       })) {
        if (hasBaseVolumes(cur)) {
            return cur.lexically_normal();
        }
        if (hasBaseVolumes(cur / "TRIBES")) {
            return (cur / "TRIBES").lexically_normal();
        }
        if (hasBaseVolumes(cur / "PCRPG")) {
            return (cur / "PCRPG").lexically_normal();
        }
        const auto parent = cur.parent_path();
        if (parent == cur) {
            break;
        }
        cur = parent;
    }
    return {};
}

bool FearGameApp::applySearchPath(std::string_view path) {
    std::vector<std::filesystem::path> roots;
    const auto anchor =
        mountedAssetRoot_.empty() ? std::filesystem::current_path() : mountedAssetRoot_;
    while (!path.empty()) {
        const auto end = path.find(';');
        const auto part = path.substr(0, end);
        if (!part.empty()) {
            // A separate write root overlays only the corresponding selected
            // directories, never the entire installation or inactive mods.
            if (!config_.writeRoot.empty() && config_.writeRoot != anchor) {
                const auto writable = resolveFilePath(config_.writeRoot, part);
                std::error_code ec;
                if (std::filesystem::is_directory(writable, ec)) {
                    roots.push_back(writable);
                }
            }
            const auto directory = resolveFilePath(anchor, part);
            std::error_code ec;
            if (std::filesystem::is_directory(directory, ec)) {
                roots.push_back(directory);
            }
        }
        if (end == path.npos) {
            break;
        }
        path.remove_prefix(end + 1);
    }
    return resources_->setSearchPaths(roots);
}

bool FearGameApp::mountAssetRoot(const std::filesystem::path& assetRoot) {
    if (!resources_) {
        return false;
    }
    std::error_code ec;
    auto root = std::filesystem::absolute(assetRoot, ec).lexically_normal();
    if (ec) {
        return false;
    }
    if (std::filesystem::is_regular_file(resolveFilePath(root, "entities.vol"), ec)) {
        root = root.parent_path();
    }
    if (!std::filesystem::is_directory(resolveFilePath(root, "base"), ec)) {
        return false;
    }
    if (mountedAssetRoot_ == root) {
        return applySearchPath(searchPath_);
    }
    if (!mountedAssetRoot_.empty() && mountedAssetRoot_ != root) {
        fearPlugin_->destroyClientWorld();
        fearPlugin_->destroyServerWorld();
        mountedVolumes_.clear();
    }
    const auto previousRoot = mountedAssetRoot_;
    mountedAssetRoot_ = root;
    if (!applySearchPath("config;base;base\\missions;base\\fonts;base\\skins;base\\voices;"
                         "base\\scripts;recordings;temp")) {
        mountedAssetRoot_ = previousRoot;
        return false;
    }
    const auto writeRoot =
        config_.writeRoot.empty() ? root : std::filesystem::absolute(config_.writeRoot);
    game_.console().setFileRoot(writeRoot);
    filePlugin_->setWriteRoot(writeRoot);
    dynamicDataPlugin_->setWriteRoot(writeRoot);
    fearPlugin_->setWriteRoot(writeRoot);
    game_.console().setVariable("CommandLinePath", root.string());
    game_.console().setVariable("ConsoleWorld::DefaultSearchPath",
                                "config;base;base\\missions;base\\fonts;base\\skins;base\\voices;"
                                "base\\scripts;recordings;temp");
    game_.console().setVariable("Console::GFXFont", "interface.pft");
    game_.console().setVariable("OpenT1::AssetRoot", root.string());
    game_.console().setVariable("OpenT1::MountedVolumes", "0");
    // console.cs and mission SimVolumes own all archive mounts.
    ensureFallbackRenderAssets();
    return true;
}

void FearGameApp::setActivePalette(std::shared_ptr<gfx::Palette> palette) {
    if (!palette || !palette->valid()) {
        return;
    }
    defaultPalette_ = std::move(palette);
    InitializePaletteTable(defaultPalette_.get());
    const auto palRes = toResourcePalette(defaultPalette_);
    if (fearPlugin_ && palRes) {
        if (auto* cw = fearPlugin_->clientGlobals().world) {
            (void)cw->setPalette(palRes);
        }
        if (auto* sw = fearPlugin_->serverGlobals().world) {
            (void)sw->setPalette(palRes);
        }
    }
}

void FearGameApp::updateGameplayPalette() {
    auto* manager = fearPlugin_->clientGlobals().manager;
    if (!manager) {
        return;
    }
    // Native dlgPlay::preRender discovers the palette independently of player
    // control and forgets it when its ghost is deleted. Do not retain a pointer
    // into a previous connection, or copy large remap tables every frame.
    const auto find = [&](auto&& self, sim::SimGroup& group) -> sim::SimPalette* {
        for (auto* object : group.objects()) {
            if (auto* palette = dynamic_cast<sim::SimPalette*>(object);
                palette && palette->isGhost() && palette->getPalette()) {
                return palette;
            }
            if (auto* child = dynamic_cast<sim::SimGroup*>(object)) {
                if (auto* palette = self(self, *child)) {
                    return palette;
                }
            }
        }
        return nullptr;
    };
    if (const auto* object = find(find, *manager)) {
        const auto palette = object->getPalette();
        if (!gameplayPalette_ || *gameplayPalette_ != *palette) {
            gameplayPalette_ = std::make_shared<gfx::Palette>(*palette);
        }
        if (!defaultPalette_ || *defaultPalette_ != *gameplayPalette_) {
            setActivePalette(gameplayPalette_);
        }
    }
}

namespace {
bool shellBindingMatches(const CFGButton& button, const sim::SimActionMap::Node& node) {
    const bool release = node.event.action == sim::InputBreak;
    const auto& command = release ? button.fields().alternateCommand : button.fields().command;
    if (node.action.consoleCommand) {
        return !command.empty() && *node.action.consoleCommand == command;
    }
    const auto id = release ? button.breakAction() : button.makeAction();
    const auto value = release ? button.breakValue() : button.makeValue();
    return id != 0 && node.action.action == id &&
           (!(node.flags & sim::ActionValue) || std::abs(node.action.fValue - value) < 0.00001f);
}

void displayShellBinding(CFGButton& button, const sim::SimActionMap* map) {
    button.clearBinding();
    if (!map) {
        return;
    }
    for (const auto& node : map->nodes()) {
        if (node.event.action != sim::InputMake || !shellBindingMatches(button, node)) {
            continue;
        }
        const auto& e = node.event;
        std::string name;
        if (e.objType == sim::InputKey) {
            if (const auto* key = sim::inputKeyDescription(e.objInst)) {
                name =
                    key->name.empty() ? std::string(1, char(key->upper)) : std::string(key->name);
            }
        } else if (e.objType == sim::InputButton) {
            name = (e.deviceType == sim::InputMouse ? "Mouse " : "Button ") +
                   std::to_string(e.objInst + 1);
        }
        const UInt16 mods =
            UInt16((e.modifier & sim::InputShift ? 1 : 0) | (e.modifier & sim::InputCtrl ? 2 : 0) |
                   (e.modifier & sim::InputAlt ? 4 : 0));
        button.setBinding({e.deviceType, UInt8(e.deviceInst), e.objType, e.objInst, mods, true},
                          name);
        return;
    }
}

class ScriptShellDelegate final : public gui::Delegate {
public:
    ScriptShellDelegate(Console& console,
                        std::unique_ptr<ShellDialogDelegate> delegate,
                        std::weak_ptr<gui::Runtime> runtime,
                        world::InputMaps& maps)
        : console_(console), delegate_(std::move(delegate)), runtime_(std::move(runtime)),
          maps_(maps) {}

    void contentChanged(gui::Control*, gui::Control* root) noexcept override {
        if (!root) {
            return;
        }
        try {
            if (dynamic_cast<ConnectDelegate*>(delegate_.get())) {
                const auto mode = console_.getVariable("pref::PlayGameMode");
                const auto tag = mode == "HOST" ? 100105 : mode == "TRAIN" ? 100998 : 100103;
                if (auto* button = dynamic_cast<FGUniversalButton*>(root->findTag(tag))) {
                    button->action();
                }
            }
            if (dynamic_cast<OptionsDelegate*>(delegate_.get())) {
                savedMaps_.clear();
                const auto save = [&](auto&& self, gui::Control& c) -> void {
                    if (auto* button = dynamic_cast<CFGButton*>(&c)) {
                        if (auto map = maps_.find(button->actionMapName())) {
                            savedMaps_.emplace(button->actionMapName(), std::move(map));
                        }
                    }
                    for (auto* child : c.objects()) {
                        self(self, *static_cast<gui::Control*>(child));
                    }
                };
                save(save, *root);
                for (const auto tag : {140400, 140401}) {
                    if (auto* combo = dynamic_cast<FGComboBox*>(root->findTag(tag))) {
                        combo->addEntry("Software", 0);
                        if (console_.getVariable("OpenT1::HardwareAvailable") == "True") {
                            combo->addEntry("Hardware", 1);
                        }
                    }
                }
                for (const auto pref :
                     {"pref::VideoWindowedDriver", "pref::VideoFullScreenDriver"}) {
                    const auto value = console_.getVariable(pref);
                    if (value != "Software" && value != "Hardware") {
                        console_.setVariable(pref, "Software");
                    }
                }
                if (auto* combo = dynamic_cast<FGComboBox*>(root->findTag(140402))) {
                    int id = 0;
                    combo->addEntry("640x480", id++);
                    const auto modes = console_.execute("OpenT1::videoModes", {}).value;
                    for (std::size_t start = 0; start < modes.size();) {
                        const auto end = modes.find('\t', start);
                        const auto mode = modes.substr(
                            start, end == std::string::npos ? modes.size() - start : end - start);
                        if (!mode.empty() && combo->findEntry(mode) < 0) {
                            combo->addEntry(mode, id++);
                        }
                        if (end == std::string::npos) {
                            break;
                        }
                        start = end + 1;
                    }
                    const auto pref = console_.getVariable("pref::VideoFullScreenRes");
                    if (!pref.empty() && combo->findEntry(pref) < 0) {
                        combo->addEntry(pref, id);
                    }
                }
            }
        } catch (...) {
            console_.print("Could not initialize shell screen controls");
        }
    }

    void opened(gui::Control& root) noexcept override {
        if (!dynamic_cast<OptionsDelegate*>(delegate_.get())) {
            return;
        }
        try {
            // The shipped script enumerates obsolete DirectSound/Aureal
            // drivers. The SDL host owns a single modern audio backend.
            if (auto* combo = dynamic_cast<FGComboBox*>(root.findNamed("OptionsSound3DDriver"))) {
                const auto backend = console_.getVariable("OpenT1::AudioBackend");
                combo->clearEntries();
                combo->addEntry(backend.empty() ? "OpenAL" : backend, 0);
                combo->setSelectedEntry(0);
                combo->active = false;
            }
        } catch (...) {
            console_.print("Could not initialize audio options");
        }
    }

    std::optional<bool> routeMessage(gui::Control& sender, UInt32 message) override {
        if (dynamic_cast<JoinGameDelegate*>(delegate_.get()) &&
            (message == 140006 || message == 140019)) {
            auto runtime = runtime_.lock();
            if (!runtime || console_.getVariable("Server::Address").empty()) {
                return false;
            }
            if (message == 140006) {
                const auto* list = findBoundControl<ServerListCtrl>(runtime->canvas.top());
                const auto* server = list ? list->selectedServer() : nullptr;
                if (!server) {
                    return false;
                }
                if (server->password) {
                    runtime->canvas.afterDispatch([runtime] {
                        (void)runtime->open("gui/Password.gui", true);
                    });
                    return true;
                }
            }
            auto* console = &console_;
            runtime->canvas.afterDispatch([runtime, console, message] {
                if (message == 140019) {
                    (void)runtime->pop();
                }
                (void)console->execute("JoinGame");
            });
            return true;
        }
        if (auto runtime = runtime_.lock()) {
            if (auto* commander = findBoundControl<TSCommander>(runtime->canvas.top())) {
                const auto* button = dynamic_cast<FGUniversalButton*>(&sender);
                if (message == 140211) {
                    commander->setZoomMode(button && button->isSet());
                    return true;
                }
                if (message == 140216) {
                    commander->centerMode = button && button->isSet();
                    return true;
                }
                if (message == 140115 || message == 140116 || message == 140117) {
                    commander->selectType = message == 140115 ? DataBlockManager::PlayerDataType
                                            : message == 140116
                                                ? DataBlockManager::TurretDataType
                                                : DataBlockManager::StaticShapeDataType;
                    return true;
                }
                if (message == 140213 || message == 140214 || message == 160243) {
                    const auto index = commander->selectedTargetKey();
                    const auto selected = message == 140214   ? -1
                                          : message == 140213 ? commander->selectedPlayerId()
                                                              : index;
                    if (message != 140214 && selected < 0) {
                        return false;
                    }
                    const std::array args{
                        std::string("2048"),
                        std::string(message == 160243 ? "CmdrMountObject" : "SelectCommander"),
                        std::to_string(selected)};
                    (void)console_.execute("remoteEval", args);
                    return true;
                }
            }
        }
        if (dynamic_cast<OptionsDelegate*>(delegate_.get()) &&
            (message == 140426 || message == 140427)) {
            auto runtime = runtime_.lock();
            if (!runtime || !runtime->canvas.top()) {
                return false;
            }
            if (message == 140427) {
                for (const auto& [name, map] : savedMaps_) {
                    maps_.replace(name, *map);
                }
            } else {
                const std::array<std::string, 1> args{"sae.cs"};
                if (!console_.execute("exec", args).ok()) {
                    return false;
                }
            }
            const auto refresh = [&](auto&& self, gui::Control& c) -> void {
                if (auto* button = dynamic_cast<CFGButton*>(&c)) {
                    displayShellBinding(*button, maps_.find(button->actionMapName()).get());
                }
                if (auto* cancel = dynamic_cast<gui::Active*>(&c);
                    cancel && c.fields().tag == 140427) {
                    cancel->active = message != 140427;
                }
                for (auto* child : c.objects()) {
                    self(self, *static_cast<gui::Control*>(child));
                }
            };
            refresh(refresh, *runtime->canvas.top());
            (void)console_.execute("OptionsMovement::init");
            return true;
        }
        if (dynamic_cast<PlayerSetupDelegate*>(delegate_.get()) &&
            (message == 140318 || message == 140060)) {
            auto runtime = runtime_.lock();
            auto* root = runtime ? runtime->canvas.top() : nullptr;
            auto* edit = root ? dynamic_cast<gui::TextEdit*>(root->findTag(140318)) : nullptr;
            auto* done = root ? dynamic_cast<gui::Active*>(root->findTag(140060)) : nullptr;
            if (!edit || !done) {
                return false;
            }
            const bool valid = edit->text.find_first_not_of(" \t\r\n") != std::string::npos;
            done->active = valid;
            if (message == 140060 && valid) {
                const auto name = edit->text;
                auto* console = &console_;
                runtime->canvas.afterDispatch([runtime, console, name] {
                    if (runtime->pop()) {
                        const std::array<std::string, 1> args{name};
                        (void)console->execute("AddPlayerConfig", args);
                    }
                });
            }
            return true;
        }
        if (!delegate_->onMessage(&sender, message)) {
            return std::nullopt;
        }
        const auto& script = delegate_->lastScriptAction();
        return script.empty() || console_.evaluate(script).ok();
    }

private:
    Console& console_;
    std::unique_ptr<ShellDialogDelegate> delegate_;
    std::weak_ptr<gui::Runtime> runtime_;
    world::InputMaps& maps_;
    std::map<std::string, std::shared_ptr<const sim::SimActionMap>> savedMaps_;
};

// Script-created canvas identity delegates presentation to the application's
// persistent runtime, whose graph survives replacement of the server world.
class MainCanvasHost final : public sim::SimCanvas {
public:
    explicit MainCanvasHost(std::shared_ptr<gui::Runtime> runtime) : runtime_(std::move(runtime)) {}

    bool processArguments(std::span<const std::string> args) override {
        return args.empty() || setTitle(args[0]);
    }

    bool setTitle(std::string_view title) override {
        return runtime_->canvas.setTitle(title);
    }

    void renderFrame() override {
        runtime_->canvas.renderFrame();
    }

private:
    std::shared_ptr<gui::Runtime> runtime_;
};
}

void FearGameApp::installRuntimeServices() {
    auto& console = game_.console();
    if (guiRuntime_ && guiRuntime_->running()) {
        return;
    }
    // The tag dictionary belongs to this console and outlives the runtime.
    auto tags = std::shared_ptr<const TagDictionary>(&console.tags(), [](const TagDictionary*) {});
    auto bindings = gui::resourceBindings(resources_, tags);
    bindings.custom = [this](gui::Control& control) {
        if (!bindMissionEditorGuiControl(control, game_, *resources_)) {
            return false;
        }
        bindMissionEditorViewport(control, game_, fearPlugin_->terrainEditor());
        if (auto* button = dynamic_cast<FGUniversalButton*>(&control)) {
            button->playSound = [this](std::string_view name) {
                if (playShellSound) {
                    playShellSound(name);
                }
            };
        }
        if (auto* center = dynamic_cast<CenterPrint*>(&control)) {
            center->resolveMessage = [this] {
                return std::pair{fearPlugin_->centerPrintText(),
                                 fearPlugin_->centerPrintPosition()};
            };
            const std::array<std::string_view, 3> large{
                "sf_orange214_10.pft", "sf_orange255_10.pft", "sf_white_9b.pft"};
            const std::array<std::string_view, 3> small{
                "sf_orange_6.pft", "sf_yellow_6.pft", "sf_white_6.pft"};
            for (auto i : std::views::iota(std::size_t{0}, large.size())) {
                if (auto font = resources_->load<gfx::BitmapFont>(large[i])) {
                    center->messageFonts[i] = std::make_shared<gfx::BitmapFont>(*font);
                }
                if (auto font = resources_->load<gfx::BitmapFont>(small[i])) {
                    auto lowerCaps = std::make_shared<gfx::BitmapFont>(*font);
                    lowerCaps->info.flags |= gfx::FontLowerCaps;
                    center->smallMessageFonts[i] = std::move(lowerCaps);
                }
            }
        }
        if (auto* list = dynamic_cast<ServerListCtrl*>(&control)) {
            list->discoverServers = [this] {
                const auto* delegate = fearPlugin_->clientGlobals().csDelegate;
                return delegate ? std::vector<ServerListCtrl::ServerInfo>(
                                      delegate->discoveredServers().begin(),
                                      delegate->discoveredServers().end())
                                : std::vector<ServerListCtrl::ServerInfo>{};
            };
            list->refreshServers = [this] {
                auto* delegate = fearPlugin_->clientGlobals().csDelegate;
                if (!delegate) {
                    return;
                }
                if (delegate->discoveredServers().empty()) {
                    std::ifstream saved(game_.console().filePath("config/GameServerList.cs"));
                    if (saved) {
                        std::ostringstream text;
                        text << saved.rdbuf();
                        (void)game_.console().evaluate(text.str());
                    } else {
                        (void)game_.console().executeFile("GameServerList.cs");
                    }
                }
                if (game_.console().getVariable("pref::AutoRefresh") != "FALSE" &&
                    game_.console().getVariable("pref::AutoRefresh") != "false") {
                    (void)game_.console().execute("rebuildServerList");
                }
            };
            list->saveServers = [this] {
                if (auto* delegate = fearPlugin_->clientGlobals().csDelegate) {
                    (void)delegate->saveServerDatabase(
                        game_.console().filePath("config/GameServerList.cs"));
                }
            };
            list->serverSelected = [this, list](const auto& server, bool doubleClick) {
                game_.console().setVariable("Server::Address", server.transportAddress);
                if (auto* button = dynamic_cast<gui::Active*>(list->findRootTag(140006))) {
                    button->active = !server.transportAddress.empty();
                }
                if (auto* button = dynamic_cast<gui::Active*>(list->findRootTag(140018))) {
                    button->active = server.status == ServerListCtrl::STAT_RESPONDING;
                }
                if (doubleClick) {
                    if (auto* delegate = fearPlugin_->clientGlobals().csDelegate) {
                        (void)delegate->queryBrowserServer(server.transportAddress);
                    }
                }
            };
            list->preferenceChanged = [this](const auto& server) {
                if (auto* delegate = fearPlugin_->clientGlobals().csDelegate) {
                    (void)delegate->setBrowserPreference(
                        server.transportAddress, server.favorite, server.neverPing);
                    (void)delegate->saveServerDatabase(
                        game_.console().filePath("config/GameServerList.cs"));
                }
            };
        }
        if (auto* master = dynamic_cast<FGMasterList*>(&control)) {
            master->loadFromConsole(game_.console());
        }
        const auto irc = [this] {
            return IRCClient::find(fearPlugin_->clientGlobals().manager);
        };
        if (auto* c = dynamic_cast<FGIRCChannelCtrl*>(&control)) {
            c->resolveClient = irc;
            auto font = resources_->load<gfx::BitmapFont>("sf_yellow200_9b.pft");
            if (font) {
                c->tabFont = std::shared_ptr<const gfx::BitmapFont>(
                    font.get(), [font](const gfx::BitmapFont*) mutable {
                        font.reset();
                    });
            }
        }
        if (auto* c = dynamic_cast<FGIRCChannelList*>(&control)) {
            c->resolveClient = irc;
        }
        if (auto* c = dynamic_cast<FGIRCNicks*>(&control)) {
            c->resolveClient = irc;
        }
        if (auto* c = dynamic_cast<FGIRCTopic*>(&control)) {
            c->resolveClient = irc;
        }
        if (auto* c = dynamic_cast<FGIRCTextEdit*>(&control);
            c && !dynamic_cast<FGIRCActiveTextEdit*>(c)) {
            c->resolveClient = irc;
        }
        if (auto* c = dynamic_cast<FGIRCActiveTextFormat*>(&control)) {
            c->resolveClient = irc;
        }
        if (auto* c = dynamic_cast<FGIRCJoinCtrl*>(&control)) {
            c->resolveClient = irc;
        }
        if (auto* c = dynamic_cast<FGIRCChannelPropsCtrl*>(&control)) {
            c->resolveClient = irc;
        }
        if (auto* c = dynamic_cast<FGIRCBanCtrl*>(&control)) {
            c->resolveClient = irc;
        }

        // IDCTG_PLAY_GUI: resource binding precedes the root's script name.
        if (auto* viewport = dynamic_cast<gui::TSControl*>(&control);
            viewport && control.fields().tag == 140114) {
            viewport->sceneRenderedExternally = true;
        }
        if (auto* skin = dynamic_cast<SkinView*>(&control)) {
            skin->bindResources(resources_);
        } else if (auto* shape = dynamic_cast<ShapeView*>(&control)) {
            shape->bindResources(resources_);
        }
        if (auto* movie = dynamic_cast<SmackerCtrl*>(&control)) {
            movie->loadMovie = [this](Int32 tag) -> std::shared_ptr<const video::Movie> {
                const auto name = game_.console().tags().idToString(tag);
                auto resource =
                    name ? resources_->load<video::Movie>(*name) : Resource<video::Movie>{};
                return resource ? std::shared_ptr<const video::Movie>(
                                      resource.get(),
                                      [resource](const video::Movie*) mutable {
                                          resource.reset();
                                      })
                                : nullptr;
            };
            movie->applyPalette = [this](const gfx::Palette& palette) {
                setActivePalette(std::make_shared<gfx::Palette>(palette));
            };
            if (bindMovieAudio) {
                bindMovieAudio(*movie);
            }
        }
        const auto bitmap = [this](std::string_view name,
                                   bool transparent) -> std::shared_ptr<const gfx::Bitmap> {
            auto loaded = resources_->load<gfx::Bitmap>(name);
            if (!loaded) {
                return {};
            }
            auto copy = std::make_shared<gfx::Bitmap>(*loaded);
            if (transparent) {
                copy->attributes |= gfx::BitmapTransparent;
            }
            return copy;
        };
        const auto font = [this](Int32 tag) -> std::shared_ptr<const gfx::BitmapFont> {
            const auto name = game_.console().tags().idToString(tag);
            auto loaded =
                name ? resources_->load<gfx::BitmapFont>(*name) : Resource<gfx::BitmapFont>{};
            return loaded ? std::make_shared<gfx::BitmapFont>(*loaded) : nullptr;
        };
        const auto chatFont = [this](std::string_view name,
                                     bool small = false) -> std::shared_ptr<const gfx::BitmapFont> {
            auto loaded = resources_->load<gfx::BitmapFont>(name);
            if (!loaded) {
                return {};
            }
            auto copy = std::make_shared<gfx::BitmapFont>(*loaded);
            if (small) {
                copy->info.flags |= gfx::FontLowerCaps;
            }
            return copy;
        };
        const auto bitmapArray = [this](Int32 tag) -> std::shared_ptr<const gfx::BitmapArray> {
            const auto name = game_.console().tags().idToString(tag);
            auto loaded =
                name ? resources_->load<gfx::BitmapArray>(*name) : Resource<gfx::BitmapArray>{};
            if (!loaded) {
                return {};
            }
            auto copy = std::make_shared<gfx::BitmapArray>(*loaded);
            for (auto& frame : copy->bitmaps) {
                frame.attributes |= gfx::BitmapTransparent;
            }
            return copy;
        };
        if (auto* formatted = dynamic_cast<gui::TextFormat*>(&control)) {
            formatted->variable = [this](std::string_view name) -> std::optional<std::string> {
                return game_.console().getVariable(name);
            };
            formatted->bitmap = [bitmap](std::string_view name) {
                return bitmap(name, true);
            };
            if (dynamic_cast<FearGuiFormattedText*>(formatted)) {
                constexpr std::array<Int32, 3> tags{150111, 150121, 150161};
                for (auto i : std::views::iota(std::size_t{0}, tags.size())) {
                    formatted->fonts[i] = {tags[i], font(tags[i])};
                }
            }
        }
        if (auto* lag = dynamic_cast<FGLagIndicator*>(&control)) {
            lag->bitmap = bitmap("H_Lag.bmp", true);
        }
        if (auto* radio = dynamic_cast<FGCommandRadio*>(&control)) {
            const auto byTag = [&](Int32 tag) {
                const auto name = game_.console().tags().idToString(tag);
                return name ? bitmap(*name, true) : std::shared_ptr<const gfx::Bitmap>{};
            };
            radio->setCommandBitmaps({byTag(radio->bmpOnTag()),
                                      byTag(radio->bmpOffTag()),
                                      byTag(radio->loResBmpOnTag()),
                                      byTag(radio->loResBmpOffTag())});
            radio->showCommand = [this](Int32 tag) {
                const auto text = game_.console().tags().idToString(tag);
                if (text && !text->empty()) {
                    if (auto* current = guiRuntime_ ? findBoundControl<FGCurrentCommand>(
                                                          guiRuntime_->canvas.top())
                                                    : nullptr) {
                        current->setCommandDisplay(*text);
                    }
                }
            };
        }
        if (auto* commander = dynamic_cast<TSCommander*>(&control)) {
            commander->bindResources(resources_);
            commander->executeCommand = [this](std::string_view name,
                                               std::span<const std::string> args) {
                (void)game_.console().execute(name, args);
            };
            commander->setVariable = [this](std::string_view name, std::string_view value) {
                game_.console().setVariable(name, value);
            };
            commander->playerSelectionChanged = [this](Int32 id, bool selected) {
                if (auto* players = fearPlugin_->clientGlobals().playerManager) {
                    if (auto* player = players->findClient(id)) {
                        player->selected = selected;
                    }
                }
            };
            commander->commandDisplayChanged = [this](std::string_view text) {
                if (auto* current =
                        guiRuntime_ ? findBoundControl<FGCurrentCommand>(guiRuntime_->canvas.top())
                                    : nullptr) {
                    current->setCommandDisplay(text);
                }
            };
        }
        if (auto* observer = dynamic_cast<CMDObserve*>(&control)) {
            observer->requestObservation = [this](Int32 player) {
                const std::array args{
                    std::string("2048"), std::string("SCOM"), std::to_string(player)};
                (void)game_.console().execute("remoteEval", args);
            };
            observer->status = [this] {
                const auto* psc = fearPlugin_->clientGlobals().playerPSC;
                const auto* object = psc ? psc->getControlObject() : nullptr;
                return object
                           ? std::pair{1 - object->getDamageLevel(), 1 - object->getEnergyLevel()}
                           : std::pair{0.f, 0.f};
            };
        }
        const auto selectTarget = [this](Int32 index, bool additive, bool doubleClick) {
            auto* commander =
                guiRuntime_ ? findBoundControl<TSCommander>(guiRuntime_->canvas.top()) : nullptr;
            if (!commander || index < 0 || std::size_t(index) >= commander->targets().size()) {
                return;
            }
            const bool waypoint = commander->commandMode() == TSCommander::WaypointMode;
            if (!commander->selectObject(index, additive)) {
                return;
            }
            if (waypoint) {
                commander->setCommandMode(TSCommander::SelectingMode);
            } else if (doubleClick && commander->targets()[std::size_t(index)].datGroup ==
                                          DataBlockManager::TurretDataType) {
                const std::array args{
                    std::string("2048"), std::string("CmdrMountObject"), std::to_string(index)};
                (void)game_.console().execute("remoteEval", args);
            }
        };
        if (auto* turrets = dynamic_cast<CommandTurretCtrl*>(&control)) {
            turrets->clicked = selectTarget;
        }
        if (auto* items = dynamic_cast<CommandItemCtrl*>(&control)) {
            items->clicked = selectTarget;
        }
        if (auto* team = dynamic_cast<CommandTeamCtrl*>(&control)) {
            team->clicked = [this](Int32 id, bool additive, bool branch, bool expand) {
                auto* commander = guiRuntime_
                                      ? findBoundControl<TSCommander>(guiRuntime_->canvas.top())
                                      : nullptr;
                auto* players = fearPlugin_->clientGlobals().playerManager;
                auto* player = players ? players->findClient(id) : nullptr;
                if (!commander || !player) {
                    return;
                }
                if (expand) {
                    player->expanded = !player->expanded;
                    return;
                }
                if (commander->commandMode() == TSCommander::WaypointMode) {
                    for (auto i : std::views::iota(std::size_t{0}, commander->targets().size())) {
                        if (commander->targets()[i].playerId == id) {
                            (void)commander->issueWaypoint(commander->targets()[i].pos, Int32(i));
                            commander->setCommandMode(TSCommander::SelectingMode);
                            return;
                        }
                    }
                }
                const bool value = !additive || !player->selected;
                (void)commander->selectPlayer(id, value, additive);
                if (branch) {
                    const auto select = [&](auto&& self, PlayerManager::ClientRep* child) -> void {
                        for (auto* p = child; p; p = p->nextPeon) {
                            (void)commander->selectPlayer(p->id, value);
                            self(self, p->firstPeon);
                        }
                    };
                    select(select, player->firstPeon);
                }
            };
        }
        if (auto* objectives = dynamic_cast<FGHCommandObjectivePage*>(&control)) {
            constexpr std::array names{"sf_white_10b.pft",
                                       "sf_green_10b.pft",
                                       "sf_yellow_10b.pft",
                                       "sf_orange214_10.pft",
                                       "sf_red_10b.pft",
                                       "sf_green_12.pft",
                                       "sf_yellow_12.pft",
                                       "sf_orange_12.pft",
                                       "sf_green_14.pft",
                                       "sf_yellow_14.pft"};
            for (auto i : std::views::iota(std::size_t{0}, names.size())) {
                objectives->objectiveFonts[i].resource = chatFont(names[i]);
            }
            objectives->objectiveBitmap = [bitmap](std::string_view name) {
                return bitmap(name, true);
            };
            objectives->objectiveVariable =
                [this](std::string_view name) -> std::optional<std::string> {
                return game_.console().getVariable(name);
            };
        }
        if (auto* hud = dynamic_cast<HudCtrl*>(&control)) {
            const auto add = [&](std::string name, bool transparent = true) {
                hud->sprites[name] = bitmap(name, transparent);
            };
            for (const std::string prefix : {"", "lr_"}) {
                if (dynamic_cast<HealthHud*>(hud)) {
                    add(prefix + "hudTrans.bmp", false);
                    for (const auto suffix : {"HH_Icon_Green.bmp",
                                              "HH_Icon_Red.bmp",
                                              "HH_End_Green.bmp",
                                              "HH_End_Yellow.bmp",
                                              "HH_End_Red.bmp"}) {
                        add(prefix + suffix);
                    }
                } else if (dynamic_cast<JetPackHud*>(hud)) {
                    add(prefix + "hudTrans.bmp", false);
                    add(prefix + "HJ_Icon_Green.bmp");
                    add(prefix + "HJ_End_Green.bmp");
                } else if (dynamic_cast<Crosshair*>(hud)) {
                    add(prefix + "H_Reticle.bmp");
                    add(prefix + "H_Friend.bmp");
                    add(prefix + "H_Foe.bmp");
                } else if (dynamic_cast<CompassHud*>(hud)) {
                    add(prefix + "Compass.bmp", false);
                } else if (dynamic_cast<FGHSensorPing*>(hud)) {
                    add(prefix + "ping.bmp", false);
                    for (const auto* name : {"HP_PingedLo.bmp",
                                             "HP_PingedMed.bmp",
                                             "HP_PingedHi.bmp",
                                             "HP_Supressed.bmp"}) {
                        add(prefix + name);
                    }
                }
            }
            const auto loadFont =
                [this](std::string_view name) -> std::shared_ptr<const gfx::BitmapFont> {
                auto loaded = resources_->load<gfx::BitmapFont>(name);
                return loaded ? std::make_shared<gfx::BitmapFont>(*loaded) : nullptr;
            };
            hud->hudFont =
                loadFont(dynamic_cast<ClockHud*>(hud) || dynamic_cast<FearGuiChatDisplay*>(hud)
                             ? "if_w_10.pft"
                             : "if_dg_8.pft");
            hud->activeFont = loadFont("if_g_8.pft");
            if (auto* crosshair = dynamic_cast<Crosshair*>(hud)) {
                hud->hudFont = loadFont("sf_white_7.pft");
                crosshair->lowResFont = loadFont("sf_white_5.pft");
            }
            if (dynamic_cast<CurrentWeaponHud*>(hud)) {
                for (const auto name : {"ammoSh.bmp",
                                        "I_weapon_off.bmp",
                                        "I_Infinity_on.bmp",
                                        "I_Infinity_off.bmp"}) {
                    add(name);
                }
                auto* dbm = fearPlugin_->clientGlobals().dbm;
                for (auto i : std::views::iota(Int32{0}) | std::views::take_while([&](Int32 i) {
                                  return dbm &&
                                         i < dbm->getDataGroupSize(DataBlockManager::ItemDataType);
                              })) {
                    const auto* item = dynamic_cast<ItemData*>(
                        dbm->lookupDataBlock(i, DataBlockManager::ItemDataType));
                    if (item && !item->hudIcon.empty()) {
                        add("I_" + item->hudIcon + "_on.bmp");
                        add("I_" + item->hudIcon + "_off.bmp");
                    }
                }
            }
        }
        if (auto* button = dynamic_cast<CFGButton*>(&control)) {
            button->font = font(150114);
            button->highlightFont = font(150124);
            button->disabledFont = font(150134);
            button->setBitmaps({bitmap("CFG_Controls_DF.BMP", true),
                                bitmap("CFG_Controls_ON.BMP", true),
                                bitmap("CFG_Controls_NA.BMP", true)});
            auto map = inputMaps_.find(button->actionMapName());
            if (!map) {
                auto loaded = resources_->load<sim::SimActionMap>(button->actionMapName());
                if (loaded) {
                    map = std::make_shared<sim::SimActionMap>(*loaded);
                }
            }
            displayShellBinding(*button, map.get());
            button->rebind =
                [this](CFGButton& button, const CFGButton::BindingSpec& binding, std::string_view) {
                    auto current = inputMaps_.find(button.actionMapName());
                    if (!current) {
                        return false;
                    }
                    sim::SimActionMap next = *current;
                    for (const auto& node : current->nodes()) {
                        if (shellBindingMatches(button, node)) {
                            next.unmapEvent(node.event);
                        }
                    }
                    sim::SimInputEvent event;
                    event.deviceType = binding.deviceType;
                    event.deviceInst = binding.deviceInst;
                    event.objType = binding.objType;
                    event.objInst = binding.objInst;
                    event.modifier = UInt8((binding.modifier & 1 ? sim::InputShift : 0) |
                                           (binding.modifier & 2 ? sim::InputCtrl : 0) |
                                           (binding.modifier & 4 ? sim::InputAlt : 0));
                    for (const bool release : {false, true}) {
                        sim::SimActionEvent action;
                        action.action = release ? button.breakAction() : button.makeAction();
                        action.fValue = release ? button.breakValue() : button.makeValue();
                        const auto& command =
                            release ? button.fields().alternateCommand : button.fields().command;
                        if (!action.action && command.empty()) {
                            continue;
                        }
                        if (!command.empty()) {
                            action.consoleCommand = command;
                        }
                        event.action = release ? sim::InputBreak : sim::InputMake;
                        if (!next.mapEvent(
                                event, action, UInt32(button.actionFlags()) | sim::ActionValue)) {
                            return false;
                        }
                    }
                    if (!inputMaps_.replace(button.actionMapName(), next)) {
                        return false;
                    }
                    const auto refresh = [&](auto&& self, gui::Control& c) -> void {
                        if (auto* cfg = dynamic_cast<CFGButton*>(&c);
                            cfg && cfg->actionMapName() == button.actionMapName()) {
                            displayShellBinding(*cfg, &next);
                        }
                        if (auto* cancel = dynamic_cast<gui::Active*>(&c);
                            cancel && c.fields().tag == 140427) {
                            cancel->active = true;
                        }
                        for (auto* child : c.objects()) {
                            self(self, *static_cast<gui::Control*>(child));
                        }
                    };
                    if (guiRuntime_ && guiRuntime_->canvas.top()) {
                        refresh(refresh, *guiRuntime_->canvas.top());
                    }
                    return true;
                };
        }
        if (auto* slider = dynamic_cast<FGSlider*>(&control)) {
            slider->setBitmaps(bitmapArray(170020));
        }
        if (auto* list = dynamic_cast<FGArrayCtrl*>(&control)) {
            list->shellFont = font(150114);
            list->shellHighlightFont = font(150124);
        }
        if (auto* list = dynamic_cast<FearGuiInventory*>(&control)) {
            list->shellHighlightFont = font(150164);
            list->disabledFont = font(150134);
            list->favoriteBitmap = bitmap("I_Favorite.bmp", true);
            if (list->shellFont) {
                list->cellSize.y = list->shellFont->info.height + 6;
            }
        }
        if (auto* menu = dynamic_cast<MenuDisplayCtrl*>(&control)) {
            const bool small = menu->getName() == "LowResServerMenu";
            menu->font = chatFont(small ? "sf_Yellow_6.pft" : "sf_Orange255_10.pft", small);
            menu->highlightFont = chatFont(small ? "sf_white_6.pft" : "sf_white_10.pft", small);
        }
        if (auto* menu = dynamic_cast<FGChatMenu*>(&control)) {
            menu->font = font(150111);
            menu->resolveMenu = [this, name = menu->menuName()]() -> const ChatMenu* {
                const auto* manager = fearPlugin_->clientGlobals().manager;
                return manager && !name.empty()
                           ? dynamic_cast<const ChatMenu*>(manager->findObject(name))
                           : nullptr;
            };
        }
        if (auto* list = dynamic_cast<FGTextList*>(&control)) {
            list->font = font(150114);
            list->highlightFont = font(150124);
        }
        if (auto* list = dynamic_cast<ColumnListCtrl*>(&control)) {
            list->changed = [this](const ColumnListCtrl& c) {
                for (const auto& column : c.serverColumns()) {
                    game_.console().setVariable("pref::ServerColumnDisplay" +
                                                    std::to_string(column.id),
                                                ColumnListCtrl::formatColumnPrefString(column));
                }
            };
        }
        if (auto* edit = dynamic_cast<TestEdit*>(&control)) {
            edit->font = font(150111);
            edit->highlightFont = font(150161);
            edit->disabledFont = font(150131);
            edit->cursorColor = 240;
        }
        if (auto* chat = dynamic_cast<FearGuiChat*>(&control)) {
            chat->font = chatFont("sf_yellow_10b.pft");
            chat->teamFont = chatFont("if_g_10b.pft");
            chat->smallFont = chatFont("sf_yellow_6.pft", true);
            chat->smallTeamFont = chatFont("sf_green_6.pft", true);
            chat->visible(false);
        }
        if (auto* display = dynamic_cast<FearGuiChatDisplay*>(&control)) {
            constexpr std::array names{
                "if_w_10b.pft", "sf_red_10b.pft", "sf_yellow_10b.pft", "if_g_10b.pft"};
            constexpr std::array smallNames{
                "sf_white_6.pft", "sf_red_6.pft", "sf_yellow_6.pft", "sf_green_6.pft"};
            for (auto i : std::views::iota(std::size_t{0}, names.size())) {
                display->messageFonts[i] = chatFont(names[i]);
                display->smallMessageFonts[i] = chatFont(smallNames[i], true);
            }
            // The shipped play.gui stores the compact three-line size. Open
            // gameplay chat at the five-line height used by the reference HUD.
            if (!display->isCommanderChat() && display->lineCount() == 3) {
                display->setSize(
                    5, display->messageFonts[2] ? display->messageFonts[2]->info.height : 12);
            }
        }
        if (auto* item = dynamic_cast<FGMenuItem*>(&control)) {
            item->playSound = [this](std::string_view name) {
                if (playShellSound) {
                    playShellSound(name);
                }
            };
            item->font = font(150111);
            item->highlightFont = font(150161);
            item->disabledFont = font(150131);
        }
        if (dynamic_cast<FGMenuCtrl*>(&control) || dynamic_cast<FGMenuItem*>(&control)) {
            auto fields = control.fields();
            fields.style.opaque = 0;
            fields.style.border = 0;
            if (!control.configure(std::move(fields))) {
                return false;
            }
        }
        if (auto* combo = dynamic_cast<FGComboBox*>(&control)) {
            // Native onAdd discards the editor's saved display text. Entries
            // and selection are populated by the screen's onOpen script.
            combo->clearEntries();
            combo->font = font(150114);
            combo->highlightFont = font(150124);
            combo->disabledFont = font(150134);
            std::array<std::shared_ptr<const gfx::Bitmap>, 8> sprites;
            constexpr std::array suffixes{"_DF.BMP",
                                          "_HI.BMP",
                                          "_MD.BMP",
                                          "_MH.BMP",
                                          "_ON.BMP",
                                          "_NA.BMP",
                                          "_BD.BMP",
                                          "_BO.BMP"};
            for (auto i : std::views::iota(std::size_t{0}, sprites.size())) {
                sprites[i] = bitmap(std::string("POP_Arrow") + suffixes[i], true);
            }
            combo->setBitmaps(std::move(sprites));
            auto& scroll = combo->popupScroll();
            auto fields = scroll.fields();
            fields.style.opaque = 1;
            fields.style.fill = 244;
            fields.style.border = 1;
            fields.style.borderColor = 240;
            if (!scroll.configure(std::move(fields))) {
                return false;
            }
            scroll.frames = bitmapArray(170019);
        }
        if (auto* dialog = dynamic_cast<FGDlgBox*>(&control)) {
            std::array<std::shared_ptr<const gfx::Bitmap>, 5> sprites;
            constexpr std::array names{"DLG_TopLeft.BMP",
                                       "DLG_TopEdge.BMP",
                                       "DLG_LeftEdge.BMP",
                                       "DLG_BottomLeft.BMP",
                                       "DLG_BottomEdge.BMP"};
            for (auto i : std::views::iota(std::size_t{0}, sprites.size())) {
                sprites[i] = bitmap(names[i], true);
            }
            dialog->setBitmaps(std::move(sprites));
        }
        if (auto* commandView = dynamic_cast<FGCommandView*>(&control)) {
            commandView->setBorder(bitmapArray(170012));
        }
        if (auto* border = dynamic_cast<FGShellBorder*>(&control)) {
            if (!border->bitmapName().empty()) {
                border->setBackground(bitmap(border->bitmapName(), false));
            }
            border->setBorder(bitmapArray(170018));
        }
        if (auto* button = dynamic_cast<FGUniversalButton*>(&control)) {
            button->setRadioBitmaps(bitmap("RAD_CheckX_ON.BMP", true),
                                    bitmap("RAD_CheckX_NA.BMP", true));
            std::string root = button->bitmapRootName();
            if (root.empty() && button->bitmapTag()) {
                if (auto value = game_.console().tags().idToString(button->bitmapTag())) {
                    root = *value;
                }
            }
            if (!root.empty()) {
                std::array<std::shared_ptr<const gfx::Bitmap>, 8> sprites;
                constexpr std::array suffixes{"_DF.BMP",
                                              "_HI.BMP",
                                              "_MD.BMP",
                                              "_MH.BMP",
                                              "_ON.BMP",
                                              "_NA.BMP",
                                              "_BD.BMP",
                                              "_BO.BMP"};
                for (auto i : std::views::iota(std::size_t{0}, sprites.size())) {
                    sprites[i] = bitmap(root + suffixes[i], true);
                }
                button->setBitmaps(std::move(sprites));
            }
        }
        if (auto* palette = dynamic_cast<gui::PaletteControl*>(&control)) {
            palette->activate = [this](auto value) {
                setActivePalette(std::make_shared<gfx::Palette>(*value));
                return true;
            };
        }
        return true;
    };
    guiRuntime_ = std::make_shared<gui::Runtime>(console, game_.classes(), std::move(bindings));
    guiRuntime_->canvas.assignName("MainWindow");
    guiRuntime_->loadFile = [this](std::string_view name) -> std::optional<std::vector<std::byte>> {
        std::vector<std::byte> bytes;
        if (!resources_->read(name, bytes, 16U * 1024U * 1024U)) {
            game_.console().print(std::format("GUI resource missing: {}", name));
            return std::nullopt;
        }
        // Native GUI readers consume one root block. Some VOL entries omit its
        // final alignment bytes; others append an unused process-memory tail.
        // Keep only the complete declared root and restore its alignment.
        if (bytes.size() >= 8) {
            UInt32 flags{};
            std::memcpy(&flags, bytes.data() + 4, sizeof(flags));
            const auto size = flags & 0x7fffffffu;
            const auto end = std::size_t(size) + 8;
            if (end > bytes.size()) {
                return std::nullopt;
            }
            const std::size_t alignment = (flags & 0x80000000u) ? 4 : 2;
            bytes.resize((end + alignment - 1) & ~(alignment - 1));
        }
        return bytes;
    };
    guiRuntime_->loadDelegate = [this](std::string_view name) -> std::shared_ptr<gui::Delegate> {
        if (name.starts_with("SimGui::")) {
            name.remove_prefix(8);
        }
        const auto* descriptor = game_.classes().find(name);
        auto object = descriptor ? descriptor->create() : nullptr;
        auto* delegate = dynamic_cast<ShellDialogDelegate*>(object.get());
        if (!delegate) {
            return {};
        }
        object.release();
        return std::make_shared<ScriptShellDelegate>(game_.console(),
                                                     std::unique_ptr<ShellDialogDelegate>(delegate),
                                                     guiRuntime_,
                                                     inputMaps_);
    };
    guiRuntime_->loadCursor = [this](std::string_view name) -> std::optional<gui::Canvas::Cursor> {
        auto bitmap = resources_->load<gfx::Bitmap>(name);
        if (!bitmap) {
            return std::nullopt;
        }
        auto owner = std::make_shared<Resource<gfx::Bitmap>>(std::move(bitmap));
        gui::Canvas::Cursor cursor;
        cursor.bitmap = std::shared_ptr<const gfx::Bitmap>(owner, owner->get());
        return cursor;
    };
    console.removeCommand("cursorOn");
    console.removeCommand("cursorOff");
    guiRuntime_->install();
    installMissionEditorGuiCommands(console, guiRuntime_, game_);
    installShellCommands();
    legacySound.install(console);
    // Track explicitly required exec failures without replacing script functions.
    console.addCommand(
        0, "exec", [this](Console& con, Int32, std::span<const std::string> args) -> std::string {
            if (args.size() != 2) {
                return "False";
            }
            const auto result = con.executeFile(args[1]);
            if (!result.ok() || result.value == "False") {
                const auto count = parseIntSv(con.getVariable("cargc"));
                for (auto i : std::views::iota(Int32{1}) | std::views::take_while([&](Int32 i) {
                                  return i + 1 < count;
                              })) {
                    if (con.getVariable(std::format("cargv{}", i)) == "+exec" &&
                        con.getVariable(std::format("cargv{}", i + 1)) == args[1]) {
                        con.setVariable("OpenT1::StartupError",
                                        "Required startup script failed: " + args[1]);
                    }
                }
            }
            if (resourceType(args[1]) == resourceType(".mis")) {
                if (!result.ok() || result.value == "False") {
                    const auto error = result.diagnostics.empty()
                                           ? "Mission script failed: " + args[1]
                                           : result.diagnostics.front().format();
                    con.setVariable("OpenT1::LastMissionEvalError", error);
                    con.setVariable("OpenT1::LastMissionError", error);
                    return "False";
                }
                // Native services depend on the completed declaration graph.
                // Preserve these registrations while the script owns its game
                // callbacks and server initialization order.
                auto* manager = fearPlugin_->serverGlobals().manager;
                if (manager) {
                    std::vector<sim::SimObject*> objects;
                    collectGroupObjectsRecursive(*manager, objects);
                    std::vector<sim::SimAddress> addresses;
                    for (auto* object : objects) {
                        addresses.push_back(object->address());
                    }
                    for (const auto address : addresses) {
                        auto* path = dynamic_cast<world::SimPath*>(address.resolve(*manager));
                        if (path && !path->publish()) {
                            con.setVariable("OpenT1::LastMissionError",
                                            "Mission path publication failed");
                            return "False";
                        }
                    }
                    for (const auto address : addresses) {
                        auto* object = address.resolve(*manager);
                        if (auto* base = dynamic_cast<GameBase*>(object)) {
                            base->updateTeamId();
                            base->updatePowerCount();
                        }
                        if (auto* moveable = dynamic_cast<Moveable*>(address.resolve(*manager))) {
                            moveable->onGroupAdd();
                        }
                    }
                    loadedMissionName_ = FileName(args[1]).getBase();
                }
                con.setVariable("OpenT1::LastMissionEvalError", "OK");
            }
            return result.value;
        });
    game_.registerFactory("SimGui::Canvas", [this] {
        return std::make_unique<MainCanvasHost>(guiRuntime_);
    });
    for (const auto name : {"editActionMap", "newActionMap", "bindAction", "bindCommand", "bind"}) {
        console.addCommand(
            0,
            name,
            [this, name](Console&, Int32, std::span<const std::string> argv) -> std::string {
                const auto args = argv.subspan(1);
                if (std::string_view(name) == "editActionMap" ||
                    std::string_view(name) == "newActionMap") {
                    return args.size() == 1 &&
                                   inputMaps_.edit(args[0],
                                                   *resources_,
                                                   std::string_view(name) == "newActionMap")
                               ? "True"
                               : "False";
                }
                return inputMaps_.bind(args,
                                       std::string_view(name) != "bindAction",
                                       std::string_view(name) == "bind")
                           ? "True"
                           : "False";
            });
    }
}

bool FearGameApp::executeConsoleCS() {
    if (mountedAssetRoot_.empty()) {
        const auto root = !config_.assetRoot.empty() ? config_.assetRoot : discoverAssetRoot();
        if (!root.empty()) {
            (void)mountAssetRoot(root);
        }
    }
    auto& console = game_.console();
    console.setVariable("OpenT1::ConsoleCSExecuted", "false");
    console.setVariable("OpenT1::StartupError", "");
    console.setVariable("OpenT1::LastMissionError", "");
    // Native Tribes bootstraps this physical file, before resource scripts.
    std::ifstream bootstrap(resolveFilePath(mountedAssetRoot_, "console.cs"), std::ios::binary);
    const std::string source((std::istreambuf_iterator<char>(bootstrap)), {});
    auto result = console.evaluate(source, "console.cs");
    if (!bootstrap.is_open()) {
        result.value = "False";
    } else if (result.ok()) {
        result.value = "True";
    }
    std::vector<std::string> reported;
    for (const auto& line : console.log()) {
        if ((line.find("Unknown command") != line.npos || line.find("GUI resource") != line.npos) &&
            std::ranges::find(reported, line) == reported.end()) {
            reported.push_back(line);
            std::cerr << line << '\n';
        }
    }
    // User mission descriptions are optional catalogue entries. Keep their
    // diagnostics visible without preventing the stock shell from opening.
    const auto requiredFailure =
        std::ranges::find_if(result.diagnostics, [](const auto& diagnostic) {
            return !diagnostic.sourceName.ends_with(".dsc");
        });
    if (requiredFailure != result.diagnostics.end() ||
        (result.diagnostics.empty() && result.value != "True")) {
        const std::string error = result.diagnostics.empty()
                                      ? "console.cs failed; check the script diagnostics"
                                      : requiredFailure->format();
        console.setVariable("OpenT1::StartupError", error);
        console.print(error);
        std::cerr << error << '\n';
        return false;
    }
    if (!console.getVariable("OpenT1::LastMissionError").empty()) {
        return false;
    }
    if (!parseBoolSv(console.getVariable("dedicated")) &&
        (!guiRuntime_ || !guiRuntime_->canvas.top())) {
        console.setVariable("OpenT1::StartupError", "console.cs did not open a GUI");
    }
    if (const auto error = console.getVariable("OpenT1::StartupError"); !error.empty()) {
        console.print(error);
        std::cerr << error << '\n';
        return false;
    }
    if (fearPlugin_->serverGlobals().manager) {
        const auto port = parseIntSv(console.getVariable("Server::Port"));
        std::string error;
        try {
            if (port < 0 || port > 65535 || !fearPlugin_->bindServerPort(static_cast<UInt16>(port))) {
                error = "Cannot bind game server port " + std::to_string(port);
            }
        } catch (const std::runtime_error& exception) {
            error = "Cannot bind game server port " + std::to_string(port) + ": " + exception.what();
        }
        if (!error.empty()) {
            console.setVariable("OpenT1::LastNetworkError", error);
            console.setVariable("OpenT1::StartupError", error);
            console.print(error);
            std::cerr << error << '\n';
            return false;
        }
    }
    console.setVariable("OpenT1::ConsoleCSExecuted", "true");
    if (auto font = resources_->load<gfx::BitmapFont>("interface.pft")) {
        defaultFont_ = std::make_shared<gfx::BitmapFont>(*font);
    }
    return true;
}

void FearGameApp::ensureFallbackRenderAssets() {
    if (!defaultPalette_) {
        auto pal = std::make_shared<gfx::Palette>();
        pal->shadeShift = 5;
        pal->hazeLevels = 8;
        pal->hazeColor = 128;
        pal->palettes.resize(1);
        auto& sub = pal->palettes[0];
        sub.index = 0xffffffffU;
        sub.type = gfx::PaletteType::ShadeHaze;
        const std::size_t remapBytes = 256u * (std::size_t{1} << pal->shadeShift) *
                                       (static_cast<std::size_t>(pal->hazeLevels) + 1u);
        sub.remap.resize(remapBytes);
        for (auto i : std::views::iota(std::size_t{0}, std::size_t{256})) {
            const auto c = static_cast<UInt8>(i);
            sub.colors[i] = gfx::PaletteColor{static_cast<UInt8>((c * 37u) & 0xFFu),
                                              static_cast<UInt8>((c * 73u) & 0xFFu),
                                              static_cast<UInt8>((c * 109u) & 0xFFu),
                                              0};
            sub.identity[i] = c;
            sub.components[0][i] = static_cast<Flt32>(sub.colors[i].red) / 255.0f;
            sub.components[1][i] = static_cast<Flt32>(sub.colors[i].green) / 255.0f;
            sub.components[2][i] = static_cast<Flt32>(sub.colors[i].blue) / 255.0f;
            sub.components[3][i] = 1.0f;
        }
        for (auto idx :
             std::views::iota(std::size_t{0}) | std::views::take_while([&](std::size_t idx) {
                 return idx < remapBytes;
             })) {
            const auto rawIdx = static_cast<UInt8>(idx & 0xFFu);
            sub.remap[idx] = static_cast<std::byte>(rawIdx == 0 ? 1u : rawIdx);
        }
        InitializePaletteTable(pal.get());
        defaultPalette_ = std::move(pal);
    }
    if (!defaultFont_) {
        auto font = std::make_shared<gfx::BitmapFont>();
        font->info.flags = gfx::FontProportional | gfx::FontBitmap;
        font->info.height = 10;
        font->info.width = 6;
        font->info.baseline = 8;
        font->info.foreColor = 250;
        font->info.backColor = 0;
        font->info.spacing = 1;
        font->lookupOffset = 32;
        font->lookup.assign(96, 0);
        if (auto sheet = gfx::Bitmap::create(8, 10, 8)) {
            std::fill(sheet->pixels.begin(), sheet->pixels.end(), std::byte{200});
            font->sheets.bitmaps.push_back(std::move(*sheet));
        }
        gfx::CharInfo ci{};
        ci.bitmap = 0;
        ci.x = 0;
        ci.y = 0;
        ci.width = 6;
        ci.height = 8;
        ci.baseline = 7;
        font->characters.push_back(ci);
        defaultFont_ = std::move(font);
    }
}

bool FearGameApp::loadMission(std::string_view missionNameOrFile, bool dedicated) {
    if (missionNameOrFile.empty()) {
        return false;
    }
    shadowContext_.clear();
    if (!initialized_ && !init()) {
        return false;
    }
    if (mountedAssetRoot_.empty()) {
        const auto root = !config_.assetRoot.empty() ? config_.assetRoot : discoverAssetRoot();
        if (!root.empty()) {
            (void)mountAssetRoot(root);
        }
    }

    const auto fail = [this](std::string message) {
        game_.console().setVariable("OpenT1::LastMissionError", message);
        game_.console().print(message);
        std::cerr << message << '\n';
        return false;
    };
    game_.console().setVariable("OpenT1::LastMissionError", "");
    std::string stem(missionNameOrFile);
    if (const auto slash = stem.find_last_of("/\\"); slash != std::string::npos) {
        stem = stem.substr(slash + 1);
    }
    if (stem.size() > 4 && (stem.ends_with(".mis") || stem.ends_with(".MIS"))) {
        stem.resize(stem.size() - 4);
    }
    const std::string misFile = stem + ".mis";

    std::vector<std::byte> missionBytes;
    if (!resources_ || !resources_->read(misFile, missionBytes)) {
        return fail("Mission file not found: " + misFile);
    }

    if (deferMissionLoading_ && !dedicated) {
        if (!guiRuntime_ || !guiRuntime_->open("gui/Loading.gui")) {
            return fail("Cannot open the mission loading screen");
        }
        const std::array<std::string, 2> cursor{"MainWindow", "Cur_Arrow.bmp"};
        (void)game_.console().execute("setCursor", cursor);
        (void)game_.console().execute("cursorOn", std::array<std::string, 1>{"MainWindow"});
        pendingMission_ = PendingMission{stem, dedicated};
        game_.console().setVariable("loadingMission", "true");
        return true;
    }

    if (game_.console().isFunction("createServer")) {
        world::clearTsHardwareMeshes();
        const std::array<std::string, 2> arguments{stem, dedicated ? "true" : "false"};
        const auto result = game_.console().execute("createServer", arguments);
        auto* manager = fearPlugin_->serverGlobals().manager;
        if (!result.ok() || result.value == "False" || !manager ||
            !manager->findObject("MissionGroup") ||
            !game_.console().getVariable("OpenT1::LastMissionError").empty()) {
            return fail("Script server startup failed: " + stem +
                        (result.diagnostics.empty() ? std::string{}
                                                    : ": " + result.diagnostics.front().format()));
        }
        loadedMissionName_ = stem;
        game_.console().setVariable("MissionName", stem);
        return true;
    }
    return fail("No script createServer function is loaded for the selected installation");
}

bool FearGameApp::loadPendingMission() {
    if (!pendingMission_) {
        return false;
    }
    auto request = std::move(*pendingMission_);
    pendingMission_.reset();
    struct RestoreDeferral {
        bool& flag;
        bool value;

        ~RestoreDeferral() {
            flag = value;
        }
    } restore{deferMissionLoading_, deferMissionLoading_};
    deferMissionLoading_ = false;
    const bool result = loadMission(request.name, request.dedicated);
    game_.console().setVariable("loadingMission", "false");
    if (!result && guiRuntime_) {
        (void)guiRuntime_->open("gui/MainMenu.gui");
    }
    return result;
}

bool FearGameApp::bindGameplayActions() {
    auto& cg = fearPlugin_->clientGlobals();
    const auto revision = game_.worldRevision(sim::WorldType::Client);
    if (revision != inputWorldRevision_) {
        shadowContext_.clear();
        inputWorldRevision_ = revision;
        inputTarget_ = {};
        boundInputMaps_ = {};
    }
    auto* handler =
        cg.manager
            ? dynamic_cast<sim::SimActionHandler*>(cg.manager->findObject(sim::ActionHandlerId))
            : nullptr;
    if (!handler || !cg.playerPSC) {
        return false;
    }
    cg.playerPSC->guiAction = [this](const sim::SimActionEvent& event) {
        return handleGameplayAction(event);
    };
    const std::array names{
        "simgame.sae",
        "actionMap.sae",
        gameplayGuiMode_ == 2 ? "pdaMap.sae" : gameplayGuiMode_ == 1 ? "playMap.sae" : ""};
    std::array<std::shared_ptr<const sim::SimActionMap>, 3> maps;
    for (auto i : std::views::iota(std::size_t{0}, names.size())) {
        maps[i] = inputMaps_.find(names[i]);
    }
    if (inputTarget_.resolve(*cg.manager) == cg.playerPSC && maps == boundInputMaps_) {
        return bool(maps[1]);
    }
    if (auto* previous = inputTarget_.resolve(*cg.manager)) {
        handler->pop(*previous);
    }
    for (const auto& map : maps) {
        if (map && !handler->push(*cg.playerPSC, map)) {
            return false;
        }
    }
    inputTarget_ = cg.playerPSC->address();
    boundInputMaps_ = std::move(maps);
    return bool(boundInputMaps_[1]);
}

bool FearGameApp::gameplayWorldVisible() const {
    const auto* root = guiRuntime_ ? guiRuntime_->canvas.top() : nullptr;
    return !root || root->getName() == "playGui" || showingScores_;
}

gui::Presentation FearGameApp::guiPresentation(Point2I pixels) const {
    return gui::Presentation::fit(
        pixels, gui::Presentation::requestedScale(console().getVariable("pref::UI::scale")));
}

void FearGameApp::trimRenderCaches() {
    shadowContext_.clear();
    softwareTextureCache_.clear();
    gfx::clearHardwareTextureConversions();
    world::clearTsHardwareMeshes();
    const auto* client = fearPlugin_ ? fearPlugin_->clientGlobals().world : nullptr;
    const auto* server = fearPlugin_ ? fearPlugin_->serverGlobals().world : nullptr;
    if (client)
        world::trimWorldRenderCaches(*fearPlugin_->clientGlobals().world);
    if (server && server != client)
        world::trimWorldRenderCaches(*fearPlugin_->serverGlobals().world);
    if (resources_)
        resources_->purge(true);
}

bool FearGameApp::renderFrame(gfx::SoftwareSurface& surface,
                              gfx::HardwareBatch* hardware,
                              Flt32 paletteGamma) {
    if (!std::isfinite(paletteGamma) || paletteGamma < .05f || paletteGamma > 20) {
        return false;
    }
    const auto frameStart = std::chrono::steady_clock::now();
    const auto elapsed = [](auto start) {
        return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start)
            .count();
    };
    renderStats_ = {};
    gfx::ScopedRenderWorkStats workStats(renderStats_.work);
    hardwareHazeColor_ = {};
    game_.console().setVariable("OpenT1::LastRenderError", "");
    std::optional<gfx::HardwareBatch> background;
    std::optional<gfx::HardwareBatch> worldBatch;
    std::optional<gfx::HardwareBatch> hardwareForeground;
    std::vector<gfx::HardwareBatch> guiLayers;
    ensureFallbackRenderAssets();
    const auto* root = guiRuntime_ ? guiRuntime_->canvas.top() : nullptr;
    const auto guiName = root ? std::string_view(root->getName()) : std::string_view{};
    const bool worldVisible = gameplayWorldVisible();
    if (worldVisible || guiName == "CommandGui" || guiName == "commandGui" ||
        guiName == "CmdInventoryGui" || guiName == "CmdObjectivesGui" || guiName == "ScoreGui") {
        // Select before setting the framebuffer palette, haze color, texture
        // conversions or software lighting caches for this frame.
        updateGameplayPalette();
    }
    // Original OpenGL Surface::_setPalette / SetHazeColorI use palette zero's
    // authored haze entry. Sky::hazeColor is only a solid-sky fallback and can
    // contain unused, out-of-range values in shipped textured-sky missions.
    if (defaultPalette_ && !defaultPalette_->palettes.empty() && defaultPalette_->hazeColor >= 0 &&
        defaultPalette_->hazeColor < 256) {
        const auto color =
            defaultPalette_->palettes.front().colors[std::size_t(defaultPalette_->hazeColor)];
        hardwareHazeColor_ = {color.red / 255.f, color.green / 255.f, color.blue / 255.f};
        if (paletteGamma != 1) {
            for (auto channel : std::views::iota(std::size_t{0}, std::size_t{3})) {
                hardwareHazeColor_[channel] =
                    Flt32(std::pow(hardwareHazeColor_[channel], double(1.f / paletteGamma)));
            }
        }
    }
    if (defaultPalette_ && (!surface.palette() || *surface.palette() != *defaultPalette_)) {
        (void)surface.setPalette(*defaultPalette_);
    }
    surface.clear(0);

    const Int32 w = surface.bitmap().width;
    const Int32 h = surface.bitmap().height;
    const auto presentation = guiPresentation({w, h});
    const bool scaled = presentation.extent != Point2I{w, h};
    std::size_t layerIndex{};
    const auto layer = [&](const gfx::Bitmap& bitmap, bool transparent, bool logical = false) {
        const auto slot = layerIndex++;
        RectI bounds = bitmap.bounds();
        const gfx::Bitmap* cropped{};
        if (transparent) {
            renderStats_.work.layerScanPixels += UInt64(bitmap.width) * bitmap.height;
            // HUD panels flush in draw order, but their sparse indexed pixels
            // do not need a full-screen RGBA conversion, hash and upload.
            const auto content = gfx::indexedContentBounds(bitmap);
            if (!content) {
                return std::optional<gfx::HardwareBatch>{};
            }
            bounds = *content;
            if (!bounds.valid()) {
                return gfx::HardwareBatch::prepare({});
            }
            if (bounds != bitmap.bounds()) {
                if (hardwareLayerCrops_.size() <= slot) {
                    hardwareLayerCrops_.resize(slot + 1);
                }
                auto& storage = hardwareLayerCrops_[slot];
                const auto width = Int32(bounds.width()), height = Int32(bounds.height());
                if (!storage || storage->width != width || storage->height != height) {
                    auto next = gfx::Bitmap::create(width, height);
                    if (!next) {
                        return std::optional<gfx::HardwareBatch>{};
                    }
                    if (storage) {
                        *storage = std::move(*next);
                    } else {
                        storage = std::move(next);
                    }
                }
                storage->attributes = bitmap.attributes;
                storage->paletteIndex = bitmap.paletteIndex;
                storage->palette = bitmap.palette;
                for (auto y : std::views::iota(Int32{0}) | std::views::take_while([&](Int32 y) {
                                  return y < height;
                              })) {
                    const auto row = bitmap.row(y + bounds.top)
                                         .subspan(std::size_t(bounds.left), std::size_t(width));
                    std::memcpy(storage->row(y).data(), row.data(), row.size());
                }
                cropped = storage.get();
                renderStats_.work.bitmapCopyBytes += cropped->pixels.size();
            }
        }
        std::array<gfx::PolygonVertex, 4> quad{
            {{{Flt32(bounds.left), Flt32(bounds.top)}, {0, 0}},
             {{Flt32(bounds.right + 1), Flt32(bounds.top)}, {1, 0}},
             {{Flt32(bounds.right + 1), Flt32(bounds.bottom + 1)}, {1, 1}},
             {{Flt32(bounds.left), Flt32(bounds.bottom + 1)}, {0, 1}}}};
        if (logical) {
            for (auto& vertex : quad) {
                vertex.position = presentation.toPixels(vertex.position);
            }
        }
        gfx::HardwareMaterial material;
        material.texture = cropped ? cropped : &bitmap;
        material.depth = gfx::HardwareDepth::Disabled;
        material.perspective = false;
        material.transparent = transparent;
        material.blend = transparent ? gfx::HardwareBlend::Alpha : gfx::HardwareBlend::Opaque;
        material.paletteMode = gfx::HardwarePaletteMode::OpenGL;
        material.paletteGamma = paletteGamma;
        material.dynamicTexture = true;
        material.alphaSource =
            transparent ? gfx::HardwareAlphaSource::Texture : gfx::HardwareAlphaSource::Opaque;
        const std::array<gfx::HardwarePolygon, 1> polygons{{{quad, material}}};
        return gfx::HardwareBatch::prepare(polygons, defaultPalette_.get());
    };
    if (w <= 0 || h <= 0) {
        return false;
    }
    const RectI fullClip{0, 0, w - 1, h - 1};

    auto& cg = fearPlugin_->clientGlobals();
    auto& sg = fearPlugin_->serverGlobals();
    const auto shadowMask = game_.console().getVariable("pref::shadowDetailMask");
    if (!shadowMask.empty()) {
        ShapeBase::prefShadowDetailMask = parseIntSv(shadowMask);
    }
    const auto playerShadows = game_.console().getVariable("pref::playerShadows");
    Player::prefPlayerShadows = playerShadows.empty() || script::toBool(playerShadows);
    sim::SimWorld* activeWorld = cg.world ? cg.world : sg.world;
    sim::SimManager* activeMgr = cg.manager ? cg.manager : sg.manager;

    lastSceneSurfaceCount_ = 0;
    lastSceneShapeCount_ = 0;
    lastScenePolygonCount_ = 0;
    lastSceneLightCount_ = 0;

    std::optional<sim::EnvironmentCamera> hudCamera;
    if (activeWorld && activeMgr && !pendingMission_ && worldVisible) {
        const auto worldPalette = activeWorld->getPalette();
        if (defaultPalette_ && (!worldPalette || *worldPalette != *defaultPalette_)) {
            if (const auto palRes = toResourcePalette(defaultPalette_)) {
                (void)activeWorld->setPalette(palRes);
            }
        }

        const GameBase* ctrlObj = cg.playerPSC ? cg.playerPSC->getControlObject() : nullptr;
        if (!ctrlObj && sg.playerPSC) {
            ctrlObj = sg.playerPSC->getControlObject();
        }
        TMat3F camToWorld;
        camToWorld.identity();
        if (ctrlObj) {
            ctrlObj->getCameraTransform(cg.playerPSC ? cg.playerPSC->camDist() : 0.f, &camToWorld);
        }
        const auto camPos = camToWorld.p;
        TMat3F worldToCam = camToWorld;
        worldToCam.inverse();
        worldToCam.flags = RMat3F::Matrix_HasRotation | RMat3F::Matrix_HasTranslation;

        sim::EnvironmentCamera envCam{};
        envCam.worldToCamera = worldToCam;
        envCam.viewport = RectI{0, 0, w - 1, h - 1};
        envCam.center = Point2F{static_cast<Flt32>(w) * 0.5f, static_cast<Flt32>(h) * 0.5f};
        const auto fov = cg.playerPSC ? std::clamp(cg.playerPSC->getFov(), 1.f, 179.f) : 90.f;
        const auto focal = Flt32(w) * .5f / std::tan(fov * std::numbers::pi_v<Flt32> / 360.f);
        envCam.focal = {focal, focal};
        envCam.nearDistance = 0.5f;
        const auto visibility = world::sceneVisibility(*activeMgr);
        envCam.farDistance = visibility.visibleDistance;
        envCam.perspective = true;
        hudCamera = envCam;

        std::vector<sim::SimObject*> envObjects{};
        collectGroupObjectsRecursive(*activeMgr, envObjects);

        // Flush indexed environment images around hardware sky geometry in the
        // original object/pass order. Each sky's full-screen bands replace all
        // earlier background pixels; later stars/planets overlay its geometry.
        std::vector<gfx::HardwareBatch> environmentLayers;
        // 2. Render 4-pass Sky / StarField / Planet backdrop
        for (auto pass : std::views::iota(0, 4)) {
            for (sim::SimObject* obj : envObjects) {
                if (auto* sky = dynamic_cast<Sky*>(obj)) {
                    gfx::HardwareBatch skyBatch;
                    gfx::HardwareBatch skyBands;
                    if (!renderSkyPass(*sky,
                                       surface,
                                       envCam,
                                       pass,
                                       defaultPalette_.get(),
                                       hardware ? &skyBatch : nullptr,
                                       paletteGamma,
                                       hardware ? &skyBands : nullptr)) {
                        game_.console().setVariable("OpenT1::LastRenderError",
                                                    "Sky preparation failed");
                        return false;
                    }
                    if (hardware && pass == 0) {
                        auto bands = !skyBands.draws().empty() ? std::optional(std::move(skyBands))
                                                               : layer(surface.bitmap(), false);
                        if (!bands) {
                            return false;
                        }
                        environmentLayers.push_back(std::move(*bands));
                        if (!skyBatch.draws().empty()) {
                            environmentLayers.push_back(std::move(skyBatch));
                        }
                        surface.clear(0);
                    }
                } else if (auto* stars = dynamic_cast<StarField*>(obj)) {
                    renderStarFieldPass(*stars, surface, envCam, pass, defaultPalette_.get());
                } else if (auto* planet = dynamic_cast<Planet*>(obj)) {
                    if (!renderPlanetPass(*planet, surface, envCam, pass, *resources_)) {
                        game_.console().setVariable("OpenT1::LastRenderError",
                                                    "Planet preparation failed");
                        return false;
                    }
                }
            }
        }
        if (hardware) {
            auto remaining = layer(surface.bitmap(), !environmentLayers.empty());
            if (!remaining) {
                return false;
            }
            environmentLayers.push_back(std::move(*remaining));
            std::vector<const gfx::HardwareBatch*> layers;
            for (const auto& image : environmentLayers) {
                layers.push_back(&image);
            }
            background = gfx::HardwareBatch::compose(layers);
            if (!background) {
                return false;
            }
            surface.clear(0);
        }

        // 3. Capture & draw 3D World Scene (SimMissionTerrain, InteriorShape, posed TS3 Player/Turret/StaticShape/Item/Projectile)
        const sim::VolumetricView volView{
            camPos, visibility.visibleDistance, visibility.hazeDistance};
        const auto cacheNoise = game_.console().getVariable("pref::TerrainCacheNoise");
        softwareTextureCache_.setNoiseEnabled(cacheNoise.empty() || parseBoolSv(cacheNoise));
        const auto captureStart = std::chrono::steady_clock::now();
        // Select the current camera mip on hardware. Native GL callbacks retain
        // their first download size until eviction, leaving approached terrain
        // blurry and making a newly lit copy appear sharper than its base tile.
        const auto terrainTiming = hardware ? world::TerrainTextureTiming::RetainedLru
                                            : world::TerrainTextureTiming::NativeSoftware;
        const bool retainedInteriors = hardware && !std::getenv("OPENT1_PROJECTED_INTERIORS");
        auto sceneFrame = world::WorldSceneFrame::captureContainers(
            *activeWorld, envCam, volView, true, terrainTiming, retainedInteriors);
        if (!sceneFrame) {
            renderStats_.lightingFallback = true;
            sceneFrame = world::WorldSceneFrame::captureContainers(
                *activeWorld, envCam, volView, false, terrainTiming, retainedInteriors);
        }
        if (sceneFrame) {
            renderStats_.captureMs = elapsed(captureStart);
            lastSceneSurfaceCount_ = sceneFrame->surfaces().size();
            lastSceneShapeCount_ = sceneFrame->shapes().size();
            std::size_t polyCount = 0;
            for (const auto& s : sceneFrame->surfaces()) {
                if (s.mesh) {
                    polyCount += s.mesh->triangleCount();
                }
            }
            for (const auto& sh : sceneFrame->shapes()) {
                polyCount += sh.polygonCount();
            }
            lastScenePolygonCount_ = polyCount;
            lastSceneLightCount_ = sceneFrame->lighting().lights.size();
            if (lastSceneLightCount_ == 0) {
                if (auto* lset =
                        dynamic_cast<sim::SimLightSet*>(activeMgr->findObject(sim::LightSetId))) {
                    lastSceneLightCount_ = lset->getSceneLighting(camPos, true).lights.size();
                }
            }
            WeatherGeometry weather;
            std::optional<world::TsStaticShapeFrame> decalFrame;
            if (cg.decalManager && cg.decalManager->activeDecalCount()) {
                decalFrame = cg.decalManager->prepareFrame(*resources_, envCam);
                if (decalFrame) {
                    if (hardware) {
                        if (auto polygons =
                                decalFrame->hardwarePolygons(defaultPalette_.get(), paletteGamma)) {
                            for (auto& polygon : *polygons) {
                                polygon.material.depth = gfx::HardwareDepth::TestOnly;
                            }
                            weather.hardware.insert(
                                weather.hardware.end(), polygons->begin(), polygons->end());
                        }
                    } else if (auto polygons =
                                   decalFrame->softwarePolygons(defaultPalette_.get())) {
                        weather.software.insert(
                            weather.software.end(), polygons->begin(), polygons->end());
                    }
                }
            }
            for (auto* object : envObjects) {
                if (auto* snow = dynamic_cast<Snowfall*>(object)) {
                    snow->setCameraState(camPos, listenerVelocity_, 1.0f);
                    weather.append(*snow, envCam, *defaultPalette_, paletteGamma);
                }
            }
            const auto worldStart = std::chrono::steady_clock::now();
            Point3F shadowDirection{0, 0, -1};
            for (const auto& light : sceneFrame->lighting().lights) {
                if (light.type == sim::SimLight::Type::Directional && light.direction.z < -.01f) {
                    shadowDirection = light.direction;
                    break;
                }
            }
            for (const auto* object : envObjects) {
                const auto* planet = dynamic_cast<const Planet*>(object);
                if (!planet || !planet->planetImage().castShadows ||
                    planet->planetImage().lightDirection.z >= -.01f) {
                    continue;
                }
                shadowDirection = planet->planetImage().lightDirection;
                break;
            }
            auto ownedShadows = sceneFrame->shadows(
                shadowDirection, .5f, &shadowContext_, game_.simulationTime());
            if (!ownedShadows) {
                ownedShadows.emplace();
            }
            std::vector<sim::EnvironmentShadow> shadows;
            shadows.reserve(ownedShadows->size());
            for (const auto& shadow : *ownedShadows) {
                shadows.push_back({shadow.geometry, shadow.bitmap});
            }
            if (hardware) {
                worldBatch = sceneFrame->hardware(*defaultPalette_, shadows, paletteGamma);
                if (!worldBatch) {
                    game_.console().setVariable("OpenT1::LastRenderError",
                                                "Hardware scene preparation failed");
                    return false;
                }
                if (!weather.hardware.empty()) {
                    const auto rain =
                        gfx::HardwareBatch::prepare(weather.hardware, defaultPalette_.get());
                    if (!rain) {
                        return false;
                    }
                    const std::array<const gfx::HardwareBatch*, 2> layers{&*worldBatch, &*rain};
                    auto combined = gfx::HardwareBatch::compose(layers);
                    if (!combined) {
                        return false;
                    }
                    worldBatch = std::move(combined);
                }
                surface.clear(0);
            } else if (!sceneFrame->draw(
                           surface, shadows, &softwareTextureCache_, weather.software)) {
                game_.console().setVariable("OpenT1::LastRenderError", "World scene draw failed");
                return false;
            }
            renderStats_.worldMs = elapsed(worldStart);
        } else if (ctrlObj) {
            game_.console().setVariable("OpenT1::LastRenderError", "World scene capture failed");
            return false;
        }

    } else {
        MMShellBorder border;
        border.setBitmapName("MainMenuBkgnd.bmp");
        (void)border.resize({0, 0}, {w, h});
        (void)border.draw(surface, {0, 0}, fullClip);
    }

    const auto guiStart = std::chrono::steady_clock::now();
    if (guiRuntime_ && guiRuntime_->canvas.top()) {
        if (!guiRuntime_->canvas.fitContent(presentation.extent, true)) {
            return false;
        }
        updateGameplayGui();
        guiRuntime_->canvas.cursorVisible = !gameplayInputActive();
        if (auto* player = cg.playerPSC ? cg.playerPSC->getControlObject() : nullptr) {
            const bool lowRes = presentation.extent.x < 512 || presentation.extent.y < 384;
            const auto update = [&](auto&& self, gui::Control& control) -> void {
                if (auto* health = dynamic_cast<HealthHud*>(&control)) {
                    health->updateFromPSC(cg.playerPSC, lowRes, cg.currentTime);
                }
                if (auto* energy = dynamic_cast<JetPackHud*>(&control)) {
                    energy->updateFromPSC(cg.playerPSC, lowRes);
                }
                if (auto* weapons = dynamic_cast<CurrentWeaponHud*>(&control)) {
                    weapons->updateFromPSC(cg.playerPSC, cg.dbm, lowRes);
                }
                if (auto* ping = dynamic_cast<FGHSensorPing*>(&control)) {
                    ping->updatePing(
                        player->getSensorPinged(), Flt32(cg.currentTime) * .001f, lowRes);
                }
                if (auto* lag = dynamic_cast<FGLagIndicator*>(&control)) {
                    const auto* stream = cg.playerPSC ? cg.playerPSC->owner() : nullptr;
                    const auto preference = game_.console().getVariable("pref::LagTimeout");
                    if (!preference.empty()) {
                        lag->setLagTimeout(UInt32(std::max(0, parseIntSv(preference))));
                    }
                    (void)lag->evaluateLag(cg.currentTime,
                                           stream ? stream->lastReceivedTick() : cg.currentTime,
                                           !stream ||
                                               stream->demoState() ==
                                                   sim::network::PacketStream::DemoState::Playback);
                    if (lag->parent()) {
                        (void)lag->resize(
                            {lag->parent()->fields().extent.x - 1 - lag->fields().extent.x, 0},
                            lag->fields().extent);
                    }
                }
                if (auto* compass = dynamic_cast<CompassHud*>(&control)) {
                    EulerF rotation;
                    player->getEyeTransform().angles(rotation);
                    const auto* client =
                        cg.playerManager ? cg.playerManager->findClient(player->getOwnerClient())
                                         : nullptr;
                    auto point = cg.playerPSC->waypoint();
                    if (client && client->curCommandTarget >= 0 &&
                        std::size_t(client->curCommandTarget) <
                            cg.playerPSC->commandTargets().size()) {
                        const auto& target =
                            cg.playerPSC->commandTargets()[std::size_t(client->curCommandTarget)];
                        if (target.dataId >= 0) {
                            point = target.position;
                        }
                    }
                    const auto* center = cg.manager
                                             ? dynamic_cast<MissionCenterPos*>(
                                                   cg.manager->findObject(MissionCenterPosId))
                                             : nullptr;
                    const Point2F origin = center ? center->pos : Point2F{1230, 1230};
                    const Point2F extent = center ? center->extent : Point2F{1400, 1400};
                    const Point2F waypoint{origin.x + Flt32(point.x) * extent.x / 1024,
                                           origin.y + Flt32(point.y) * extent.y / 1024};
                    compass->updateState(player->getLinearPosition(),
                                         rotation.z,
                                         waypoint,
                                         client && client->curCommand != -1,
                                         lowRes);
                }
                if (auto* clock = dynamic_cast<ClockHud*>(&control)) {
                    clock->setClockTime(cg.clockTime);
                }
                if (auto* crosshair = dynamic_cast<Crosshair*>(&control)) {
                    crosshair->resize({0, 0}, presentation.extent);
                    if (hudCamera && cg.manager) {
                        // Project world targets into the same logical space as
                        // their markers. The scene itself stays native-sized.
                        auto camera = *hudCamera;
                        const auto sx =
                            Flt32(presentation.extent.x) / presentation.viewport.width();
                        const auto sy =
                            Flt32(presentation.extent.y) / presentation.viewport.height();
                        camera.center = {(camera.center.x - presentation.viewport.left) * sx,
                                         (camera.center.y - presentation.viewport.top) * sy};
                        camera.focal = {camera.focal.x * sx, camera.focal.y * sy};
                        camera.viewport = {
                            0, 0, presentation.extent.x - 1, presentation.extent.y - 1};
                        crosshair->updateLive(
                            *cg.manager,
                            *cg.playerPSC,
                            cg.playerManager,
                            cg.dbm,
                            camera,
                            cg.currentTime,
                            !equalIgnoreCase(game_.console().getVariable("pref::SniperCrosshair"),
                                             "false"),
                            !equalIgnoreCase(game_.console().getVariable("pref::sensorOn"),
                                             "false"),
                            !script::toBool(game_.console().getVariable("pref::ignoreTargets")));
                    }
                }
                for (auto* child : control.objects()) {
                    self(self, *static_cast<gui::Control*>(child));
                }
            };
            update(update, *guiRuntime_->canvas.top());
        }
        (void)guiRuntime_->synchronizeVariables();
        gfx::SoftwareSurface::BlendRenderer blendRenderer;
        if ((hardware && background) || (scaled && worldVisible)) {
            blendRenderer = [&](gfx::SoftwareSurface& foreground,
                                std::span<const gfx::PolygonVertex> vertices,
                                const gfx::PolygonMaterial& fill) {
                // Flush in draw order: text already drawn must sit underneath a
                // later translucent panel, including nested dialogs.
                if (vertices.size() != 4) {
                    return false;
                }
                auto clipped = std::vector<gfx::PolygonVertex>(vertices.begin(), vertices.end());
                const auto clip = foreground.clip();
                for (auto& vertex : clipped) {
                    vertex.position.x =
                        std::clamp(vertex.position.x, Flt32(clip.left), Flt32(clip.right + 1));
                    vertex.position.y =
                        std::clamp(vertex.position.y, Flt32(clip.top), Flt32(clip.bottom + 1));
                    vertex.position = presentation.toPixels(vertex.position);
                }
                if (hardware && background) {
                    auto pixels = layer(foreground.bitmap(), true, true);
                    auto material = gfx::hardwarePaletteFill(
                        *defaultPalette_, fill.color, fill.paletteIndex, paletteGamma);
                    if (!pixels || !material) {
                        game_.console().setVariable("OpenT1::LastRenderError",
                                                    "GUI panel palette conversion failed");
                        return false;
                    }
                    material->depth = gfx::HardwareDepth::Disabled;
                    material->perspective = false;
                    const std::array<gfx::HardwarePolygon, 1> polygons{{{clipped, *material}}};
                    auto panel = gfx::HardwareBatch::prepare(polygons, defaultPalette_.get());
                    if (!panel) {
                        game_.console().setVariable(
                            "OpenT1::LastRenderError",
                            std::format("GUI panel preparation failed: {} {} to {} {}",
                                        clipped[0].position.x,
                                        clipped[0].position.y,
                                        clipped[2].position.x,
                                        clipped[2].position.y));
                        return false;
                    }
                    guiLayers.push_back(std::move(*pixels));
                    guiLayers.push_back(std::move(*panel));
                } else {
                    auto pixels = foreground.bitmap();
                    pixels.attributes = gfx::BitmapTransparent;
                    if (!surface.stretch(pixels, presentation.viewport) ||
                        !surface.polygon(clipped, fill)) {
                        return false;
                    }
                }
                foreground.clear(0);
                return true;
            };
        }
        // Software alpha tables must read the world pixels, not a zero-filled
        // temporary that is subsequently keyed out by the overlay blit.
        const auto* seed = scaled || (hardware && background) ? nullptr : &surface.bitmap();
        if (const auto* frame = guiRuntime_->canvas.renderRetained(presentation.extent.x,
                                                                   presentation.extent.y,
                                                                   defaultPalette_,
                                                                   seed,
                                                                   std::move(blendRenderer),
                                                                   true)) {
            const bool integerScale = presentation.viewport.width() % presentation.extent.x == 0 &&
                                      presentation.viewport.height() % presentation.extent.y == 0;
            if (hardware && background && integerScale) {
                // Keep the indexed HUD at logical resolution. Nearest GPU
                // sampling reproduces integer pixel replication and preserves
                // the same final overlay order without a CPU stretch/scan.
                hardwareForeground = layer(*frame, true, true);
                if (!hardwareForeground) {
                    return false;
                }
            } else if (scaled) {
                // Scale the finished indexed UI, including fonts and cursor,
                // once. No persisted layout or bitmap/font resource is edited.
                auto overlay = *frame;
                if (worldVisible) {
                    overlay.attributes = gfx::BitmapTransparent;
                }
                if (!surface.stretch(overlay, presentation.viewport)) {
                    return false;
                }
            } else if (!surface.blit(*frame, {0, 0})) {
                return false;
            }
        } else {
            if (game_.console().getVariable("OpenT1::LastRenderError").empty()) {
                game_.console().setVariable("OpenT1::LastRenderError", "GUI rendering failed");
            }
            return false;
        }
    }
    ++renderedFrames_;
    if (hardware) {
        auto foreground = hardwareForeground ? std::move(hardwareForeground)
                                             : layer(surface.bitmap(), background.has_value());
        if (!foreground) {
            return false;
        }
        if (background) {
            std::vector<const gfx::HardwareBatch*> layers{&*background};
            if (worldBatch) {
                layers.push_back(&*worldBatch);
            }
            for (const auto& guiLayer : guiLayers) {
                layers.push_back(&guiLayer);
            }
            layers.push_back(&*foreground);
            auto combined = gfx::HardwareBatch::compose(layers);
            if (!combined) {
                return false;
            }
            *hardware = std::move(*combined);
        } else {
            *hardware = std::move(*foreground);
        }
    }
    renderStats_.guiMs = elapsed(guiStart);
    renderStats_.totalMs = elapsed(frameStart);
    return true;
}

bool FearGameApp::init(std::span<const std::string_view> commandLineArgs) {
    if (initialized_) {
        return true;
    }
    cpuMetrics_ = detectCpuMetrics(static_cast<Flt32>(config_.overrideCpuLoops));
    setCpuConsoleVars(game_.console(), cpuMetrics_);

    game_.setDataBlockManager(dynamicDataPlugin_);
    if (config_.registerDefaultPlugins) {
        game_.registerPlugin(filePlugin_);
        game_.registerPlugin(scriptPlugin_);
        game_.registerPlugin(dynamicDataPlugin_);
        game_.registerPlugin(fearPlugin_);
        game_.registerPlugin(aiPlugin_);
        game_.registerPlugin(missionPlugin_);
        game_.registerPlugin(timeDemoPlugin_);
        game_.registerPlugin(std::make_shared<sim::NativeMediaPlugin>());
    }

    installRuntimeServices();

    // Pre-scan asset mounting flags so volumes and search paths are mounted before +exec scripts run
    bool mountAssets = config_.mountDiscoveredAssets || !config_.assetRoot.empty();
    bool noAssets = false;
    bool execConsoleCs = false;
    bool spawnDemo = false;
    std::string cliMission{};
    std::filesystem::path cliAssetRoot = config_.assetRoot;

    for (auto argumentIt = commandLineArgs.begin(); argumentIt != commandLineArgs.end();
         ++argumentIt) {
        const auto arg = *argumentIt;
        if (arg == "--mount-assets") {
            mountAssets = true;
        } else if (arg == "--no-assets") {
            noAssets = true;
        } else if (arg == "--assets" && std::next(argumentIt) != commandLineArgs.end()) {
            cliAssetRoot = std::filesystem::path(*++argumentIt);
            mountAssets = true;
        } else if (arg == "--exec-console-cs") {
            execConsoleCs = true;
            mountAssets = true;
        } else if (arg == "--spawn-demo") {
            spawnDemo = true;
        } else if (arg == "--mission" && std::next(argumentIt) != commandLineArgs.end()) {
            cliMission = std::string(*++argumentIt);
            mountAssets = true;
        }
    }

    if (mountAssets && !noAssets) {
        const auto resolvedRoot = !cliAssetRoot.empty() ? cliAssetRoot : discoverAssetRoot();
        if (!resolvedRoot.empty()) {
            (void)mountAssetRoot(resolvedRoot);
        }
    }

    game_.console().setVariable("cargc", std::to_string(commandLineArgs.size() + 1));
    game_.console().setVariable("cargv0", "Tribes.exe");
    for (auto i : std::views::iota(std::size_t{0}, commandLineArgs.size())) {
        game_.console().setVariable(std::format("cargv{}", i + 1), commandLineArgs[i]);
    }
    game_.console().setVariable(std::format("cargv{}", commandLineArgs.size() + 1), "");
    for (auto argumentIt = commandLineArgs.begin(); argumentIt != commandLineArgs.end();
         ++argumentIt) {
        const auto arg = *argumentIt;
        if (arg.starts_with("$") && arg.size() > 1 &&
            std::next(argumentIt) != commandLineArgs.end()) {
            game_.console().setVariable(arg.substr(1), *++argumentIt);
        }
    }
    initialized_ = true;
    execConsoleCs = execConsoleCs || (!spawnDemo && !mountedAssetRoot_.empty());
    if (execConsoleCs && !noAssets && !executeConsoleCS()) {
        shutdown();
        return false;
    }
    for (auto argumentIt = commandLineArgs.begin(); argumentIt != commandLineArgs.end();
         ++argumentIt) {
        const auto arg = *argumentIt;
        game_.console().setVariable(
            std::format("cargv{}",
                        static_cast<std::size_t>(argumentIt - commandLineArgs.begin()) + 1),
            arg);
        if (arg == "-dedicated") {
            game_.console().setVariable("Server::Dedicated", "true");
        } else if ((arg == "--assets" || arg == "--frames" || arg == "--driver" ||
                    arg == "--mission" || arg == "--renderer") &&
                   std::next(argumentIt) != commandLineArgs.end()) {
            const auto nextArg = *++argumentIt;
            game_.console().setVariable(
                std::format("cargv{}",
                            static_cast<std::size_t>(argumentIt - commandLineArgs.begin()) + 1),
                nextArg);
        } else if (!arg.empty() && arg.starts_with("--")) {
            continue;
        } else if (arg == "+exec" && std::next(argumentIt) != commandLineArgs.end()) {
            if (execConsoleCs) {
                ++argumentIt;
                continue;
            }
            const auto nextArg = *++argumentIt;
            game_.console().setVariable(
                std::format("cargv{}",
                            static_cast<std::size_t>(argumentIt - commandLineArgs.begin()) + 1),
                nextArg);
            auto execRes = game_.console().executeFile(nextArg);
            if (!execRes.ok() || execRes.value == "False") {
                const auto diskPath = config_.writeRoot.empty()
                                          ? std::filesystem::path(nextArg)
                                          : (config_.writeRoot / std::filesystem::path(nextArg));
                std::ifstream in(diskPath, std::ios::binary);
                if (!in.is_open() && !config_.writeRoot.empty()) {
                    in.open(std::filesystem::path(nextArg), std::ios::binary);
                }
                if (in.is_open()) {
                    const std::string scriptText((std::istreambuf_iterator<char>(in)),
                                                 std::istreambuf_iterator<char>());
                    execRes = game_.console().evaluate(scriptText, nextArg);
                    if (execRes.ok()) {
                        execRes.value = "True";
                    }
                } else {
                    const std::array<std::string, 1> execArgs{std::string(nextArg)};
                    execRes = game_.console().execute("exec", execArgs);
                }
            }
            if (!execRes.ok() || execRes.value == "False") {
                const auto error = "Required startup script failed: " + std::string(nextArg);
                game_.console().setVariable("OpenT1::StartupError", error);
                std::cerr << error << '\n';
                shutdown();
                return false;
            }
        } else if (execConsoleCs &&
                   (arg == "+connect" || arg == "+password" || arg == "+maxplayers")) {
            if (std::next(argumentIt) != commandLineArgs.end()) {
                ++argumentIt;
            }
        } else if (execConsoleCs && arg == "+record") {
            continue;
        } else if (!arg.empty() && arg.front() == '+' && arg.size() > 1) {
            const std::string_view cmdName = arg.substr(1);
            if (std::next(argumentIt) != commandLineArgs.end() && !std::next(argumentIt)->empty() &&
                std::next(argumentIt)->front() != '-' && std::next(argumentIt)->front() != '+') {
                const auto valArg = *++argumentIt;
                game_.console().setVariable(
                    std::format("cargv{}",
                                static_cast<std::size_t>(argumentIt - commandLineArgs.begin()) + 1),
                    valArg);
                const std::array<std::string, 1> cmdArgs{std::string(valArg)};
                (void)game_.console().execute(cmdName, cmdArgs);
            } else {
                (void)game_.console().execute(cmdName);
            }
        } else if (!arg.empty() && arg.front() == '$' && arg.size() > 1) {
            const std::string_view varName = arg.substr(1);
            if (std::next(argumentIt) != commandLineArgs.end()) {
                const auto valArg = *++argumentIt;
                game_.console().setVariable(
                    std::format("cargv{}",
                                static_cast<std::size_t>(argumentIt - commandLineArgs.begin()) + 1),
                    valArg);
                game_.console().setVariable(varName, valArg);
            }
        }
    }

    initialized_ = true;
    if (!cliMission.empty()) {
        if (!loadMission(cliMission)) {
            shutdown();
            return false;
        }
    } else if (spawnDemo && !setupLiveDemoSession()) {
        shutdown();
        return false;
    }
    return true;
}

bool FearGameApp::stepFrame(double wallTimeSeconds, bool renderCanvases) {
    if (!initialized_ && !init()) {
        return false;
    }
    if (!std::isfinite(wallTimeSeconds) || wallTimeSeconds < 0 ||
        (lastWallTime_ >= 0 && wallTimeSeconds < lastWallTime_)) {
        return false;
    }
    if (lastWallTime_ >= 0) {
        simulationWallTime_ += std::min(wallTimeSeconds - lastWallTime_, .256);
    }
    lastWallTime_ = wallTimeSeconds;
    game_.frame(simulationWallTime_, renderCanvases);
    fearPlugin_->processPendingLocalDisconnect();
    if (guiRuntime_) {
        (void)guiRuntime_->canvas.advance(game_.simulationTime());
    }
    if (fearPlugin_) {
        if (fearPlugin_->serverGlobals().manager) {
            scriptPlugin_->setServerManager(fearPlugin_->serverGlobals().manager);
        }
        const GameBase* ctrlObj = nullptr;
        if (fearPlugin_->clientGlobals().playerPSC) {
            ctrlObj = fearPlugin_->clientGlobals().playerPSC->getControlObject();
        }
        if (!ctrlObj && fearPlugin_->serverGlobals().playerPSC) {
            ctrlObj = fearPlugin_->serverGlobals().playerPSC->getControlObject();
        }
        if (ctrlObj) {
            listenerTransform_ = ctrlObj->getEyeTransform();
            listenerVelocity_ = ctrlObj->getLinearVelocity();
        }
    }
    ++steppedFrames_;
    return !fearPlugin_->quitRequested();
}

void FearGameApp::shutdown() noexcept {
    if (!initialized_) {
        return;
    }
    try {
        (void)game_.console().evaluate("onExit();");
    } catch (...) {
    }
    if (guiRuntime_) {
        guiRuntime_->stop();
        guiRuntime_.reset();
    }
    pendingMission_.reset();
    shadowContext_.clear();
    softwareTextureCache_.clear();
    hardwareLayerCrops_.clear();
    boundInputMaps_ = {};
    inputTarget_ = {};
    inputWorldRevision_ = ~UInt64{0};
    inputMaps_.reset();
    fearPlugin_->destroyClientWorld();
    fearPlugin_->destroyServerWorld();
    game_.removePlugin(timeDemoPlugin_);
    game_.removePlugin(missionPlugin_);
    game_.removePlugin(aiPlugin_);
    game_.removePlugin(fearPlugin_);
    game_.removePlugin(dynamicDataPlugin_);
    game_.removePlugin(scriptPlugin_);
    game_.removePlugin(filePlugin_);
    game_.setDataBlockManager(nullptr);
    initialized_ = false;
}

} // namespace opent1::game
