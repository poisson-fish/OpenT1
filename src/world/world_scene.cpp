#include <opent1/gfx/render_limits.hpp>
#include <opent1/world/world_scene.hpp>
#include <opent1/world/world_mission.hpp>
#include <opent1/world/scene_objects.hpp>
#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <unordered_map>
#include <cstdio>
#include <cstdlib>

namespace opent1::world {
namespace {
bool finite(Point3F p) {
    return std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z) &&
           std::max({std::abs(p.x), std::abs(p.y), std::abs(p.z)}) <= 1e12f;
}

bool rigid(const TMat3F& transform, bool allowReflection = false) {
    if (!finite(transform.p)) {
        return false;
    }
    for (std::size_t i = 0; i < 3; ++i) {
        const Point3F row{transform.m[i][0], transform.m[i][1], transform.m[i][2]};
        if (!finite(row) || std::abs(m_dot(row, row) - 1) > .0001f) {
            return false;
        }
        for (std::size_t j = 0; j < i; ++j) {
            const Point3F other{transform.m[j][0], transform.m[j][1], transform.m[j][2]};
            if (std::abs(m_dot(row, other)) > .0001f) {
                return false;
            }
        }
    }
    const auto determinant = transform.determinant();
    return std::abs((allowReflection ? std::abs(determinant) : determinant) - 1) < .0001f;
}

Point3F rgb(ColorF color) {
    return {color.red, color.green, color.blue};
}

std::optional<TsLighting> imageLighting(const sim::SimSceneLighting& scene,
                                        const WorldShapeImage& image) {
    auto lights = tsSceneLighting(scene, image.shapeToWorld);
    if (!lights || !image.sunOverride) {
        return lights;
    }
    const auto factor = std::clamp(image.sunOverride->factor, 0.f, 1.f);
    const auto color = rgb(image.sunOverride->color);
    for (auto& light : lights->lights) {
        if (light.type == TsLight::Type::Point) {
            continue;
        }
        if (light.intensity.lenSq() != 0) {
            light.intensity = light.intensity * (1 - factor) + color * factor;
        }
        break;
    }
    const auto ambient =
        std::min(1.f, (color.x * .3f + color.y * .5f + color.z * .2f) * factor * 2);
    lights->ambient *= ambient;
    return lights;
}

struct ContainerSelection {
    std::vector<bool> visible, linked;
    bool outside{};
};

std::optional<ContainerSelection> selectContainers(sim::SimManager& manager,
                                                   std::span<const sim::SimAddress> sources,
                                                   const sim::EnvironmentCamera& viewCamera,
                                                   bool scope = false) {
    auto cameraToWorld = viewCamera.worldToCamera;
    cameraToWorld.inverse();
    const auto camera = cameraToWorld.p;
    auto* root = dynamic_cast<sim::SimContainer*>(manager.findObject(sim::RootContainerId));
    if (!root || root->isRemoved() || !root->isOpen()) {
        return {};
    }
    std::vector<sim::SimContainer*> objects{root};
    std::vector<InteriorContainerNode> nodes{
        {root->getBoundingBox(), -1, -1, root->getType(), root->getContainerMask(), true}};
    std::unordered_map<const sim::SimObject*, UInt32> indices{{root, 0}};
    std::vector<InteriorLinkInstance> interiors;
    for (std::size_t i = 0; i < objects.size(); ++i) {
        auto* parent = objects[i];
        if (!parent->isOpen()) {
            continue;
        }
        for (auto* child : parent->getDatabase()->objects()) {
            if (objects.size() == gfx::renderLimits().containers || !child || child->isRemoved() ||
                child->getManager() != &manager || child->getContainer() != parent ||
                !indices.emplace(child, UInt32(objects.size())).second) {
                return {};
            }
            InteriorContainerNode node{child->getBoundingBox(),
                                       Int32(i),
                                       -1,
                                       child->getType(),
                                       child->getContainerMask(),
                                       child->isOpen()};
            if (const auto* terrain = dynamic_cast<const SimMissionTerrain*>(parent)) {
                node.renderEligible =
                    scope ? terrain->scopeChild(camera, viewCamera.farDistance, node.bounds)
                          : terrain->renderChild(viewCamera, node.bounds);
            }
            if (const auto* place = dynamic_cast<const SimPlace*>(parent)) {
                node.renderEligible = scope ? TerrainMissionPolicy::scopeChild(
                                                  camera, viewCamera.farDistance, node.bounds)
                                            : place->renderChild(viewCamera, node.bounds);
            }
            WorldInteriorQuery interior;
            if (child->isOpen() && child->processQuery(interior) && interior.image) {
                if (interiors.size() == gfx::renderLimits().images) {
                    return {};
                }
                node.interior = Int32(interiors.size());
                interiors.push_back(std::move(*interior.image));
                node.pvs = interior.pvs;
            }
            objects.push_back(child);
            nodes.push_back(node);
        }
    }
    auto graph = InteriorContainerGraph::prepare(nodes, interiors);
    if (!graph) {
        if (std::getenv("OPENT1_SCENE_DIAGNOSTICS")) {
            std::fprintf(stderr,
                         "Scene container graph rejected: %zu nodes, %zu interiors\n",
                         nodes.size(),
                         interiors.size());
        }
        return {};
    }
    const auto view = graph->visibility(camera);
    if (!view) {
        if (std::getenv("OPENT1_SCENE_DIAGNOSTICS")) {
            std::fprintf(stderr,
                         "Scene container visibility rejected at %g %g %g\n",
                         double(camera.x),
                         double(camera.y),
                         double(camera.z));
        }
        return {};
    }
    std::vector<bool> visible(nodes.size()), linked(nodes.size());
    for (auto index : view->containers) {
        visible[index] = true;
    }
    for (auto index : view->linkedContainers) {
        linked[index] = true;
    }
    ContainerSelection result;
    result.outside = view->outsideVisible;
    for (const auto& address : sources) {
        const auto* object = address.resolve(manager);
        const auto found = indices.find(object);
        if (found != indices.end()) {
            result.visible.push_back(visible[found->second]);
            result.linked.push_back(linked[found->second]);
        } else {
            // Detached providers are exterior. A different spatial tree is an
            // invalid scene binding rather than an implicit exterior object.
            if (const auto* spatial = dynamic_cast<const sim::SimContainer*>(object);
                spatial && spatial->getContainer()) {
                return {};
            }
            result.visible.push_back(result.outside);
            result.linked.push_back(false);
        }
    }
    return result;
}
}

std::optional<std::vector<sim::SimAddress>>
scopedSceneObjects(sim::SimManager& manager,
                   Point3F camera,
                   Flt32 visibleDistance,
                   std::span<const sim::SimAddress> candidates) {
    if (!finite(camera) || !std::isfinite(visibleDistance) || visibleDistance <= 0) {
        return {};
    }
    sim::EnvironmentCamera view;
    view.worldToCamera.p = -camera;
    view.worldToCamera.flags = RMat3F::Matrix_HasTranslation;
    view.farDistance = visibleDistance;
    const auto selected = selectContainers(manager, candidates, view, true);
    if (!selected) {
        return {};
    }
    std::vector<sim::SimAddress> result;
    for (std::size_t i = 0; i < candidates.size(); ++i) {
        if (selected->visible[i]) {
            result.push_back(candidates[i]);
        }
    }
    return result;
}

std::optional<TsLighting> tsSceneLighting(const sim::SimSceneLighting& scene,
                                          const TMat3F& shapeToWorld) {
    if (!rigid(shapeToWorld, true) || scene.lights.size() > 64) {
        return {};
    }
    TsLighting result;
    result.ambient = rgb(scene.ambient);
    result.shapeToWorld = shapeToWorld;
    for (const auto& source : scene.lights) {
        // Keep the provider contract valid even for fields unused by its type.
        if (!finite(source.position) || !finite(source.direction) || !finite(source.attenuation) ||
            source.attenuation.x < 0 || source.attenuation.y < 0 || source.attenuation.z < 0 ||
            !std::isfinite(source.range) || source.range < 0) {
            return {};
        }
        TsLight light;
        light.intensity = rgb(source.intensity);
        light.range = source.range;
        light.attenuation = source.attenuation;
        light.shadowAttenuation = source.shadowAttenuation;
        light.shadowMap = source.shadowMap;
        switch (source.type) {
            case sim::SimLight::Type::Directional:
                light.type = TsLight::Type::Directional;
                break;
            case sim::SimLight::Type::DirectionalNonlinear:
                light.type = TsLight::Type::DirectionalNonlinear;
                break;
            case sim::SimLight::Type::DirectionalWrap:
                light.type = TsLight::Type::DirectionalWrap;
                break;
            case sim::SimLight::Type::Point:
                light.type = TsLight::Type::Point;
                break;
            default:
                return {};
        }
        if (light.type == TsLight::Type::Point) {
            m_mulInverse(source.position, shapeToWorld, light.position);
            result.worldSources.push_back({source.position, {}});
        } else {
            const auto length = source.direction.len();
            if (!std::isfinite(length) || length < 1e-12f) {
                return {};
            }
            m_mulInverse(source.direction / length,
                         static_cast<const RMat3F&>(shapeToWorld),
                         light.direction);
            result.worldSources.push_back({{}, source.direction / length});
        }
        result.lights.push_back(light);
    }
    return result.valid() ? std::optional(std::move(result)) : std::nullopt;
}

bool SimSceneLight::onAdd() {
    return SimObject::onAdd() && addToSet(sim::LightSetId);
}

bool SimSceneLight::setLighting(const sim::SimSceneLighting& candidate) {
    if (candidate.lights.size() > 5 || !tsSceneLighting(candidate, {})) {
        return false;
    }
    lighting_ = candidate;
    return true;
}

bool SimSceneLight::setLight(const sim::SimLight& light, ColorF ambient) {
    return setLighting({ambient, {light}});
}

bool SimSceneLight::processQuery(sim::SimQuery& query) {
    auto* light = dynamic_cast<sim::SimLightQuery*>(&query);
    if (!light) {
        return false;
    }
    light->ambient = enabled_ ? lighting_.ambient : ColorF{};
    light->lights = enabled_ ? lighting_.lights : std::vector<sim::SimLight>{};
    return true;
}

SimTsShape::SimTsShape() {
    setMovementFlags(movementFlags() | RotateBoundingBox);
}

bool SimTsShape::onAdd() {
    if (!SimMovement::onAdd()) {
        return false;
    }
    if (!name_.empty() && !setShapeName(name_)) {
        return false;
    }
    if (!instance_ || !synchronizePose() || !addToSet(sim::RenderSetId)) {
        return false;
    }
    auto* root = dynamic_cast<sim::SimContainer*>(getManager()->findObject(sim::RootContainerId));
    return !root || root->addObject(*this);
}

void SimTsShape::onRemove() {
    SimMovement::onRemove();
    collision_.reset();
    instance_.reset();
    resources_ = {};
}

void SimTsShape::onPreLoad(sim::SimPreLoadManager& preloader) {
    if (!name_.empty()) {
        preloader.preLoad(name_, true);
    }
}

bool SimTsShape::setShapeName(std::string_view name) {
    if (advancing_ || name.empty() || name.size() > 255 || name.find('\0') != name.npos) {
        return false;
    }
    std::string replacement(name);
    bool result = false;
    const auto work = [&] {
        if (getManager()) {
            auto* service = sim::SimResource::find(*getManager());
            if (!service) {
                return;
            }
            auto resources = TsShapeResources::load(service->get(), replacement);
            if (!resources || !setResources(std::move(*resources))) {
                return;
            }
        }
        name_ = std::move(replacement);
        result = true;
    };
    if (auto* manager = getManager()) {
        manager->dispatch(work);
    } else {
        work();
    }
    return result;
}

bool SimTsShape::setResources(TsShapeResources resources) {
    if (advancing_ || !resources.shape ||
        resources.materials.textures.size() != resources.materials.list.materials.size()) {
        return false;
    }
    if (std::ranges::any_of(resources.materials.list.materials, [](const auto& material) {
            return !material.useDefaultProperties &&
                   (!std::isfinite(material.friction) || !std::isfinite(material.elasticity) ||
                    material.friction < 0 || material.elasticity < 0 || material.friction > 1e12f ||
                    material.elasticity > 1e12f);
        })) {
        return false;
    }
    TsShapeInstance candidate(resources.shape);
    const auto pose = candidate.pose();
    if (!pose) {
        return false;
    }
    TsStaticView validation;
    validation.twoSided = true;
    if (!TsStaticShapeFrame::prepare(resources, validation, *pose)) {
        return false;
    }
    auto spatial = spatialState(*pose, collisionDetail_);
    if (!spatial) {
        return false;
    }
    bool result = false;
    const auto work = [&] {
        struct Guard {
            bool& flag;
            explicit Guard(bool& value) : flag(value) {
                flag = true;
            }
            ~Guard() {
                flag = false;
            }
        } guard(advancing_);
        // Publish the matching pose before spatial callbacks can query it.
        auto oldResources = resources_;
        auto oldInstance = instance_;
        auto oldCollision = collision_;
        resources_ = std::move(resources);
        instance_ = std::move(candidate);
        collision_ = spatial->collision;
        if (!applySpatialState(std::move(*spatial))) {
            if (!isRemoved() && !isDeleted()) {
                resources_ = std::move(oldResources);
                instance_ = std::move(oldInstance);
                collision_ = std::move(oldCollision);
            }
            return;
        }
        name_.clear();
        result = true;
    };
    if (auto* manager = getManager()) {
        manager->dispatch(work);
    } else {
        work();
    }
    return result;
}

bool SimTsShape::setTransform(const TMat3F& transform) {
    if (advancing_ || !rigid(transform)) {
        return false;
    }
    auto normalized = transform;
    normalized.flags = RMat3F::Matrix_HasRotation | RMat3F::Matrix_HasTranslation;
    return setPosition(normalized, true);
}

bool SimTsShape::setHaze(Flt32 value) {
    if (advancing_ || !std::isfinite(value) || value < 0 || value > 1) {
        return false;
    }
    haze_ = value;
    return true;
}

bool SimTsShape::processQuery(sim::SimQuery& query) {
    if (auto* collision = dynamic_cast<sim::SimCollisionImageQuery*>(&query)) {
        if (!instance_ || collisionDetail_ < 0) {
            return false;
        }
        auto geometry = collision_;
        if (!advancing_) {
            auto pose = instance_->pose();
            if (!pose) {
                throw std::invalid_argument("invalid TS collision pose");
            }
            auto snapshot = TsShapeCollision::shared(*pose, collisionDetail_);
            if (!snapshot) {
                throw std::invalid_argument("invalid TS collision detail");
            }
            geometry = std::move(snapshot);
        }
        const auto& materials = resources_.materials.list;
        if (materials.materialsPerDetail < 0 ||
            std::size_t(materials.materialsPerDetail) > materials.materials.size()) {
            throw std::invalid_argument("invalid TS collision material list");
        }
        auto image = std::make_shared<SimTsCollisionImage>(
            std::move(geometry),
            collisionLevel_,
            std::span(materials.materials).first(std::size_t(materials.materialsPerDetail)));
        image->transform = getTransform();
        image->trigger = collisionTrigger_;
        collision->images = {std::move(image)};
        return true;
    }
    auto* render = dynamic_cast<WorldShapeQuery*>(&query);
    if (!render || !instance_) {
        return false;
    }
    auto pose = instance_->pose();
    if (!pose) {
        throw std::invalid_argument("invalid TS instance pose");
    }
    render->image = WorldShapeImage{
        resources_, std::move(*pose), getTransform(), detail_, automaticDetail_, haze_};
    return true;
}

SimWorldSurface::SimWorldSurface(WorldSurfaceInstance image) : image_(std::move(image)) {
    if (!image_.mesh || !rigid(image_.placement)) {
        throw std::invalid_argument("invalid world surface instance");
    }
    image_.placement.flags = RMat3F::Matrix_HasRotation | RMat3F::Matrix_HasTranslation;
}

bool SimWorldSurface::onAdd() {
    return SimObject::onAdd() && addToSet(sim::RenderSetId);
}

bool SimWorldSurface::processQuery(sim::SimQuery& query) {
    auto* surface = dynamic_cast<WorldSurfaceQuery*>(&query);
    if (!surface) {
        return SimObject::processQuery(query);
    }
    surface->image = image_;
    return true;
}

std::optional<WorldSceneFrame>
WorldSceneFrame::capture(sim::SimWorld& world,
                         const sim::EnvironmentCamera& camera,
                         const sim::VolumetricView& volumeView,
                         bool lighting,
                         std::optional<std::span<const InteriorSceneRoot>> interiorRoots,
                         std::optional<TerrainTextureTiming> terrainTiming) {
    return captureImpl(world, camera, volumeView, lighting, interiorRoots, false, terrainTiming);
}

std::optional<WorldSceneFrame>
WorldSceneFrame::captureContainers(sim::SimWorld& world,
                                   const sim::EnvironmentCamera& camera,
                                   const sim::VolumetricView& volumeView,
                                   bool lighting,
                                   std::optional<TerrainTextureTiming> terrainTiming,
                                   bool retainedHardware) {
    return captureImpl(
        world, camera, volumeView, lighting, std::nullopt, true, terrainTiming, retainedHardware);
}

std::optional<WorldSceneFrame>
WorldSceneFrame::captureImpl(sim::SimWorld& world,
                             const sim::EnvironmentCamera& camera,
                             const sim::VolumetricView& volumeView,
                             bool lighting,
                             std::optional<std::span<const InteriorSceneRoot>> interiorRoots,
                             bool containers,
                             std::optional<TerrainTextureTiming> terrainTiming,
                             bool retainedHardware) {
    if (camera.viewport.left != 0 || camera.viewport.top != 0 || camera.viewport.width() > 16384 ||
        camera.viewport.height() > 16384 || !rigid(camera.worldToCamera) ||
        (terrainTiming && *terrainTiming != TerrainTextureTiming::RetainedLru &&
         *terrainTiming != TerrainTextureTiming::NativeSoftware &&
         *terrainTiming != TerrainTextureTiming::NativeOpenGL) ||
        !sim::SoftwareEnvironment::prepare(camera, {})) {
        if (std::getenv("OPENT1_SCENE_DIAGNOSTICS")) {
            std::fprintf(stderr,
                         "Scene camera rejected: viewport %d %d %d %d rigid %d center %g %g focal "
                         "%g %g near/far %g %g environment %d\n",
                         camera.viewport.left,
                         camera.viewport.top,
                         camera.viewport.right,
                         camera.viewport.bottom,
                         int(rigid(camera.worldToCamera)),
                         double(camera.center.x),
                         double(camera.center.y),
                         double(camera.focal.x),
                         double(camera.focal.y),
                         double(camera.nearDistance),
                         double(camera.farDistance),
                         int(sim::SoftwareEnvironment::prepare(camera, {}).has_value()));
        }
        return {};
    }
    WorldSceneFrame frame;
    frame.camera_ = camera;
    frame.lightingEnabled_ = lighting;
    frame.retainedHardware_ = retainedHardware && camera.perspective;
    auto& manager = world.manager();
    bool success = false;
    const char* stage = "resource service";
    sim::SimObjectId providerId{};
    try {
        manager.dispatch([&] {
            auto* service = sim::SimResource::find(manager);
            if (!service) {
                return;
            }
            const auto resources = service->shared();
            sim::SimSceneLighting sceneLighting;
            if (lighting) {
                stage = "scene lighting";
                auto* set = dynamic_cast<sim::SimLightSet*>(manager.findObject(sim::LightSetId));
                auto cameraToWorld = camera.worldToCamera;
                cameraToWorld.inverse();
                if (set) {
                    sceneLighting = set->getSceneLighting(cameraToWorld.p, set->ignoreDistance());
                }
                if (!tsSceneLighting(sceneLighting, {})) {
                    return;
                }
                frame.lighting_ = sceneLighting;
            }
            auto* set = dynamic_cast<sim::SimSet*>(manager.findObject(sim::RenderSetId));
            std::vector<sim::SimAddress> sources;
            if (set) {
                for (const auto* object : set->objects()) {
                    sources.push_back(object->address());
                }
            }
            std::vector<bool> selected(sources.size(), true);
            std::vector<bool> linked(sources.size(), interiorRoots.has_value());
            if (containers) {
                stage = "container traversal";
                auto cameraToWorld = camera.worldToCamera;
                cameraToWorld.inverse();
                auto selection = selectContainers(manager, sources, camera);
                if (!selection) {
                    return;
                }
                selected = std::move(selection->visible);
                linked = std::move(selection->linked);
                frame.outsideVisible_ = selection->outside;
            } else if (interiorRoots) {
                std::vector<InteriorLinkInstance> interiors;
                std::vector<std::size_t> sourceIndices;
                for (std::size_t i = 0; i < sources.size(); ++i) {
                    auto* provider = dynamic_cast<SimInteriorSurface*>(sources[i].resolve(manager));
                    if (!provider || provider->isRemoved()) {
                        continue;
                    }
                    auto instance = provider->linkInstance();
                    if (!instance || interiors.size() == gfx::renderLimits().images) {
                        return;
                    }
                    interiors.push_back(std::move(*instance));
                    sourceIndices.push_back(i);
                    selected[i] = false;
                }
                if (interiorRoots->size() > gfx::renderLimits().images) {
                    return;
                }
                std::vector<InteriorLinkRoot> roots;
                for (const auto& root : *interiorRoots) {
                    const auto* object = root.provider.resolve(manager);
                    if (!object || object->isRemoved()) {
                        return;
                    }
                    auto found = std::ranges::find_if(sourceIndices, [&](auto i) {
                        return sources[i].resolve(manager) == object;
                    });
                    if (found == sourceIndices.end()) {
                        return;
                    }
                    roots.push_back({UInt32(found - sourceIndices.begin()), root.cameraContained});
                }
                auto graph = InteriorLinkGraph::prepare(interiors);
                if (!graph) {
                    return;
                }
                auto cameraToWorld = camera.worldToCamera;
                cameraToWorld.inverse();
                auto visible = graph->visibility(cameraToWorld.p, roots);
                if (!visible) {
                    return;
                }
                for (auto index : visible->instances) {
                    selected[sourceIndices[index]] = true;
                }
                frame.outsideVisible_ = visible->outsideVisible;
            }
            std::size_t polygons{};
            std::vector<std::pair<sim::SimAddress, WorldSurfaceQuery::LinkDetail>> linkDetails;
            for (std::size_t sourceIndex = 0; sourceIndex < sources.size(); ++sourceIndex) {
                if (!selected[sourceIndex]) {
                    continue;
                }
                const auto& address = sources[sourceIndex];
                auto* object = address.resolve(manager);
                if (!set || set->isRemoved() || !object || object->isRemoved() ||
                    std::ranges::find(set->objects(), object) == set->objects().end()) {
                    continue;
                }
                WorldShapeQuery query;
                providerId = object->getId();
                stage = "shape query";
                auto cameraToWorld = camera.worldToCamera;
                cameraToWorld.inverse();
                query.cameraPosition = cameraToWorld.p;
                if (!object->processQuery(query) || (!query.image && query.attachments.empty())) {
                    // Re-resolve after callbacks: providers may remove objects.
                    object = address.resolve(manager);
                    if (!object || object->isRemoved() || set->isRemoved() ||
                        std::ranges::find(set->objects(), object) == set->objects().end()) {
                        continue;
                    }
                    WorldSurfaceQuery surface;
                    surface.camera = camera;
                    surface.linkRendering = linked[sourceIndex];
                    surface.terrainTiming = terrainTiming;
                    surface.deferLinkDetail_ = true;
                    if (lighting) {
                        surface.lighting = sceneLighting;
                    }
                    const bool provided = object->processQuery(surface);
                    stage = "surface query";
                    if (surface.failed) {
                        return;
                    }
                    if (!provided || !surface.image) {
                        continue;
                    }
                    auto& image = *surface.image;
                    if (!image.mesh || frame.surfaces_.size() == gfx::renderLimits().images ||
                        !rigid(image.placement)) {
                        return;
                    }
                    TsStaticView view;
                    m_mul(image.placement, camera.worldToCamera, view.shapeToCamera);
                    view.width = Int32(camera.viewport.width());
                    view.height = Int32(camera.viewport.height());
                    view.focal = camera.focal;
                    view.center = camera.center;
                    view.perspective = camera.perspective;
                    view.nearDistance = camera.nearDistance;
                    view.farDistance = camera.farDistance;
                    const bool retained =
                        frame.retainedHardware_ && image.mesh->retainedHardwareCompatible();
                    auto prepared =
                        retained ? std::optional(TsStaticShapeFrame{}) : image.mesh->prepare(view);
                    stage = "surface preparation";
                    if (!prepared ||
                        prepared->polygonCount() > gfx::renderLimits().polygons - polygons) {
                        return;
                    }
                    polygons += prepared->polygonCount();
                    frame.surfaceFrames_.push_back(std::move(*prepared));
                    frame.surfaces_.push_back(std::move(image));
                    if (surface.linkDetail_) {
                        linkDetails.emplace_back(address, *surface.linkDetail_);
                    }
                    continue;
                }
                if (query.image) {
                    query.attachments.insert(query.attachments.begin(), std::move(*query.image));
                }
                for (auto& image : query.attachments) {
                    // Native shield-hit images use an orthogonal reflected basis.
                    if (frame.shapes_.size() == gfx::renderLimits().images ||
                        !rigid(image.shapeToWorld, true)) {
                        return;
                    }
                    TsStaticView view;
                    m_mul(image.shapeToWorld, camera.worldToCamera, view.shapeToCamera);
                    view.width = Int32(camera.viewport.width());
                    view.height = Int32(camera.viewport.height());
                    view.focal = camera.focal;
                    view.center = camera.center;
                    view.perspective = camera.perspective;
                    view.nearDistance = camera.nearDistance;
                    view.farDistance = camera.farDistance;
                    if (image.firstPerson) {
                        view.nearDistance = .01f;
                    }
                    view.detail = image.detail;
                    view.automaticDetail = image.automaticDetail;
                    view.haze = image.haze;
                    view.opacity = image.opacity;
                    view.texturePerspective = image.texturePerspective;
                    if (lighting) {
                        view.lighting = imageLighting(sceneLighting, image);
                        if (!view.lighting) {
                            return;
                        }
                    }
                    auto prepared = TsStaticShapeFrame::prepare(image.resources, view, image.pose);
                    stage = "shape preparation";
                    if (!prepared ||
                        prepared->polygonCount() > gfx::renderLimits().polygons - polygons) {
                        return;
                    }
                    polygons += prepared->polygonCount();
                    frame.shapes_.push_back(std::move(*prepared));
                    frame.shapeInputs_.push_back(std::move(image));
                }
            }
            std::vector<sim::SimAddress> volumeSources;
            if (containers) {
                for (std::size_t i = 0; i < sources.size(); ++i) {
                    if (selected[i]) {
                        volumeSources.push_back(sources[i]);
                    }
                }
            }
            frame.volumes_ = world.gatherVolumetrics(
                volumeView,
                containers ? std::optional(std::span<const sim::SimAddress>(volumeSources))
                           : std::nullopt);
            stage = "volume materials";
            auto materials = WorldVolumeMaterials::load(*resources, frame.volumes_);
            if (!materials) {
                return;
            }
            frame.materials_ = std::move(*materials);
            std::ranges::for_each(linkDetails, [&](const auto& update) {
                const auto& [address, detail] = update;
                auto* interior = dynamic_cast<SimInteriorSurface*>(address.resolve(manager));
                if (interior && !interior->isRemoved()) {
                    interior->commitLinkDetail(detail);
                }
            });
            success = true;
        });
    } catch (const std::invalid_argument& error) {
        if (std::getenv("OPENT1_SCENE_DIAGNOSTICS")) {
            std::fprintf(stderr, "Scene argument error (%s): %s\n", stage, error.what());
        }
        return {};
    } catch (const std::overflow_error& error) {
        if (std::getenv("OPENT1_SCENE_DIAGNOSTICS")) {
            std::fprintf(stderr, "Scene overflow error (%s): %s\n", stage, error.what());
        }
        return {};
    } catch (const std::length_error& error) {
        if (std::getenv("OPENT1_SCENE_DIAGNOSTICS")) {
            std::fprintf(stderr, "Scene length error (%s): %s\n", stage, error.what());
        }
        return {};
    }
    if (!success && std::getenv("OPENT1_SCENE_DIAGNOSTICS")) {
        std::fprintf(stderr, "Scene capture failed: %s provider %u\n", stage, providerId);
    }
    return success ? std::optional(std::move(frame)) : std::nullopt;
}

std::optional<gfx::HardwareBatch>
WorldSceneFrame::hardware(const gfx::Palette& palette,
                          std::span<const sim::EnvironmentShadow> shadows,
                          Flt32 paletteGamma) const {
    std::vector<gfx::HardwarePolygon> polygons;
    for (std::size_t i = 0; i < surfaceFrames_.size(); ++i) {
        const auto& image = surfaces_[i];
        TsStaticView view;
        m_mul(image.placement, camera_.worldToCamera, view.shapeToCamera);
        view.focal = camera_.focal;
        view.center = camera_.center;
        view.nearDistance = camera_.nearDistance;
        view.farDistance = camera_.farDistance;
        auto inputs = retainedHardware_ && image.mesh->retainedHardwareCompatible()
                          ? image.mesh->hardwareInterior(view, palette, paletteGamma)
                          : surfaceFrames_[i].hardwarePolygons(&palette, paletteGamma);
        if (!inputs) {
            return {};
        }
        polygons.insert(polygons.end(), inputs->begin(), inputs->end());
    }
    for (std::size_t i = 0; i < shapes_.size(); ++i) {
        auto inputs = shapes_[i].hardwarePolygons(&palette, paletteGamma);
        if (!inputs) {
            return {};
        }
        const auto& position = shapeInputs_[i].shapeToWorld.p;
        const auto& transform = camera_.worldToCamera;
        const auto distance = transform.p.y + position.x * transform.m[0][1] +
                              position.y * transform.m[1][1] + position.z * transform.m[2][1];
        const auto sortValue =
            distance <= camera_.nearDistance ? .99f : camera_.nearDistance / distance;
        for (auto& input : *inputs) {
            input.imageSortValue = sortValue;
        }
        polygons.insert(polygons.end(), inputs->begin(), inputs->end());
    }
    const auto materials = materials_.frames();
    return sim::prepareHardwareEnvironment(
        camera_, volumes_, materials, palette, shadows, polygons, paletteGamma);
}

bool WorldSceneFrame::draw(gfx::SoftwareSurface& surface,
                           std::span<const sim::SoftwareShadow> shadows,
                           NativeSoftwareTextureCache* cache,
                           std::span<const gfx::SortedPolygon> additional) const {
    if (cache && (!surface.palette() || !cache->beginFrame(*surface.palette()))) {
        return false;
    }
    const auto environment = sim::SoftwareEnvironment::prepare(camera_, volumes_, shadows);
    if (!environment) {
        return false;
    }
    std::vector<gfx::SortedPolygon> polygons;
    // Lazy software projections must outlive the borrowed polygon spans submitted below.
    std::vector<std::optional<TsStaticShapeFrame>> softwareFrames(surfaceFrames_.size());
    for (std::size_t i = 0; i < surfaceFrames_.size(); ++i) {
        auto& software = softwareFrames[i];
        if (retainedHardware_ && surfaces_[i].mesh->retainedHardwareCompatible()) {
            TsStaticView view;
            m_mul(surfaces_[i].placement, camera_.worldToCamera, view.shapeToCamera);
            view.width = Int32(camera_.viewport.width());
            view.height = Int32(camera_.viewport.height());
            view.focal = camera_.focal;
            view.center = camera_.center;
            view.perspective = camera_.perspective;
            view.nearDistance = camera_.nearDistance;
            view.farDistance = camera_.farDistance;
            software = surfaces_[i].mesh->prepare(view);
            if (!software)
                return false;
        }
        auto inputs =
            (software ? *software : surfaceFrames_[i]).softwarePolygons(surface.palette(), cache);
        if (!inputs) {
            return false;
        }
        polygons.insert(polygons.end(), inputs->begin(), inputs->end());
    }
    for (const auto& shape : shapes_) {
        auto inputs = shape.softwarePolygons(surface.palette(), cache);
        if (!inputs) {
            return false;
        }
        polygons.insert(polygons.end(), inputs->begin(), inputs->end());
    }
    polygons.insert(polygons.end(), additional.begin(), additional.end());
    return environment->draw(surface, polygons);
}

std::optional<WorldSceneFrame>
WorldSceneFrame::reframed(const sim::EnvironmentCamera& camera) const {
    if (camera.viewport.left || camera.viewport.top || camera.viewport.width() > 16384 ||
        camera.viewport.height() > 16384 || camera.worldToCamera.m != camera_.worldToCamera.m ||
        camera.worldToCamera.p != camera_.worldToCamera.p ||
        camera.nearDistance != camera_.nearDistance || camera.farDistance != camera_.farDistance ||
        camera.perspective != camera_.perspective ||
        !sim::SoftwareEnvironment::prepare(camera, {})) {
        return {};
    }
    WorldSceneFrame result = *this;
    result.retainedHardware_ = false;
    result.camera_ = camera;
    result.shapes_.clear();
    result.surfaceFrames_.clear();
    std::size_t polygons = 0;
    const auto viewFor = [&](const TMat3F& placement) {
        TsStaticView view;
        m_mul(placement, camera.worldToCamera, view.shapeToCamera);
        view.width = Int32(camera.viewport.width());
        view.height = Int32(camera.viewport.height());
        view.focal = camera.focal;
        view.center = camera.center;
        view.perspective = camera.perspective;
        view.nearDistance = camera.nearDistance;
        view.farDistance = camera.farDistance;
        return view;
    };
    for (const auto& image : surfaces_) {
        auto frame = image.mesh->prepare(viewFor(image.placement));
        if (!frame || frame->polygonCount() > gfx::renderLimits().polygons - polygons) {
            return {};
        }
        polygons += frame->polygonCount();
        result.surfaceFrames_.push_back(std::move(*frame));
    }
    for (const auto& image : shapeInputs_) {
        auto view = viewFor(image.shapeToWorld);
        view.detail = image.detail;
        view.automaticDetail = image.automaticDetail;
        view.haze = image.haze;
        view.opacity = image.opacity;
        view.texturePerspective = image.texturePerspective;
        if (image.firstPerson) {
            view.nearDistance = .01f;
        }
        if (lightingEnabled_) {
            view.lighting = imageLighting(lighting_, image);
            if (!view.lighting) {
                return {};
            }
        }
        auto frame = TsStaticShapeFrame::prepare(image.resources, view, image.pose);
        if (!frame || frame->polygonCount() > gfx::renderLimits().polygons - polygons) {
            return {};
        }
        polygons += frame->polygonCount();
        result.shapes_.push_back(std::move(*frame));
    }
    return result;
}

std::optional<sim::ShadowGeometry>
WorldSceneFrame::shadowGeometry(const sim::ShadowSourceWindow& window,
                                Point3F direction,
                                Flt32 length,
                                Flt32 lift,
                                bool nativePartition) const {
    return worldShadowGeometry(window, surfaces_, direction, length, lift, nativePartition);
}

std::optional<std::vector<WorldShadow>> WorldSceneFrame::shadows(Point3F direction,
                                                                 Flt32 opacity) const {
    if (!finite(direction) || direction.len() < .0001f || !std::isfinite(opacity) || opacity < 0 ||
        opacity > 1) {
        return {};
    }
    direction.normalize();
    auto cameraToWorld = camera_.worldToCamera;
    cameraToWorld.inverse();
    Point3F cameraDirection;
    cameraToWorld.getRow(1, cameraDirection);
    std::vector<WorldShadow> result;
    try {
        for (const auto& image : shapeInputs_) {
            if (!image.castShadow || image.firstPerson || image.haze > .2f || image.opacity <= 0 ||
                !image.resources.shape) {
                continue;
            }
            const auto& shape = *image.resources.shape;
            Point3F center;
            m_mul(shape.center, static_cast<const RMat3F&>(image.shapeToWorld), center);
            const auto window = sim::makeShadowSourceWindow(direction,
                                                            center,
                                                            shape.radius,
                                                            shape.bounds,
                                                            image.shapeToWorld,
                                                            image.shapeToWorld.p);
            if (window.radius <= .0001f) {
                continue;
            }
            auto geometry = shadowGeometry(window, cameraDirection, 100, .1f, true);
            if (!geometry) {
                return {};
            }
            if (geometry->polygons.empty()) {
                continue;
            }
            // Render the retained animated pose from the light. A one-pixel
            // border around the 64-square window supplies the blur kernel.
            TMat3F lightToWorld;
            lightToWorld.setRow(0, window.right);
            lightToWorld.setRow(1, window.direction);
            lightToWorld.setRow(2, window.up);
            lightToWorld.p = (window.points[0] + window.points[2]) * .5f;
            lightToWorld.flags = RMat3F::Matrix_HasRotation | RMat3F::Matrix_HasTranslation;
            auto worldToLight = lightToWorld;
            worldToLight.inverse();
            TsStaticView view;
            m_mul(image.shapeToWorld, worldToLight, view.shapeToCamera);
            view.width = view.height = 66;
            view.center = {33, 33};
            view.focal = {32 / window.radius, 32 / window.radius};
            view.perspective = false;
            view.twoSided = true;
            view.nearDistance = .001f;
            view.farDistance = 65536;
            view.detail = image.detail;
            auto frame = TsStaticShapeFrame::prepare(image.resources, view, image.pose);
            if (!frame) {
                return {};
            }
            const auto mask = frame->silhouette();
            if (!mask) {
                return {};
            }
            std::vector<UInt8> pixels;
            pixels.reserve(66 * 66);
            for (Int32 y = 0; y < 66; ++y) {
                for (Int32 x = 0; x < 66; ++x) {
                    pixels.push_back(std::to_integer<UInt8>(mask->row(y)[x]));
                }
            }
            result.push_back({std::move(*geometry),
                              sim::filterShadowBitmap(
                                  pixels, 66, 64, sim::ShadowBlur::Box2, opacity * image.opacity)});
        }
    } catch (const std::invalid_argument&) {
        return {};
    }
    return result;
}
} // namespace opent1::world
