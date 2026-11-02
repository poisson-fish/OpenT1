#include <opent1/world/world_scene.hpp>
#include "collision_geometry.hpp"

namespace opent1::world {
SimInteriorSurface::SimInteriorSurface(InteriorShapeResources resources,
                                       const TMat3F& placement,
                                       UInt32 lod,
                                       UInt32 lightState,
                                       bool baked,
                                       UInt32 seed,
                                       Resource<InteriorLighting> mission)
    : resources_(std::move(resources)), mission_(std::move(mission)), lod_(lod),
      lightState_(lightState), linkLod_(lod) {
    if (!resources_.shape || !collision_detail::rigid(placement)) {
        throw std::invalid_argument("invalid interior surface");
    }
    image_.placement = placement;
    image_.placement.flags = RMat3F::Matrix_HasRotation | RMat3F::Matrix_HasTranslation;
    if (baked) {
        if (lod >= resources_.shape->lods.size() ||
            lightState >= resources_.shape->lightStateNames.size()) {
            throw std::invalid_argument("invalid interior lighting selection");
        }
        const auto index = UInt64(resources_.shape->lods[lod].firstLightState) + lightState;
        if (index >= resources_.lighting.size() || !resources_.lighting[std::size_t(index)]) {
            throw std::invalid_argument("missing interior lighting");
        }
        auto player = InteriorLightPlayer::create(*resources_.lighting[std::size_t(index)], seed);
        if (!player || !publish(std::move(*player))) {
            throw std::invalid_argument("invalid interior playback");
        }
    } else {
        auto mesh = WorldSurfaceMesh::interior(resources_, lod, lightState, false);
        if (!mesh) {
            throw std::invalid_argument("invalid interior mesh");
        }
        image_.mesh = std::make_shared<const WorldSurfaceMesh>(std::move(*mesh));
    }
}

bool SimInteriorSurface::publish(InteriorLightPlayer player) {
    if (player_ && std::ranges::equal(player_->slots(), player.slots())) {
        player_ = std::move(player);
        return true;
    }
    InteriorSurfaceLighting lighting;
    lighting.slots = player.slots();
    lighting.mission = mission_.get();
    lighting.missionHandle = mission_;
    lighting.previous = image_.mesh.get();
    auto mesh = WorldSurfaceMesh::interior(resources_, lod_, lightState_, true, lighting);
    if (!mesh) {
        return false;
    }
    image_.mesh = std::make_shared<const WorldSurfaceMesh>(std::move(*mesh));
    player_ = std::move(player);
    return true;
}

bool SimInteriorSurface::onAdd() {
    if (!SimContainer::onAdd() || !addToSet(sim::RenderSetId)) {
        return false;
    }
    if (!spatial_) {
        return true;
    }
    auto* root = dynamic_cast<sim::SimContainer*>(getManager()->findObject(sim::RootContainerId));
    return root && root->addObject(*this);
}

std::optional<UInt32> SimInteriorSurface::highestDetail(std::optional<UInt32> state) const {
    const auto& shape = *resources_.shape;
    if (shape.states.size() > 65536 || shape.lods.size() > 65536) {
        return {};
    }
    if (state) {
        if (!shape.detailByPixels(*state, 0)) {
            return {};
        }
        return shape.states[*state].firstLod;
    }
    if (shape.states.empty()) {
        return lod_ == 0 ? std::optional(0U) : std::nullopt;
    }
    const auto found = std::ranges::find_if(shape.states, [&](const auto& s) {
        const auto end = UInt64(s.firstLod) + s.lodCount;
        return s.lodCount && end <= shape.lods.size() && lod_ >= s.firstLod && lod_ < end;
    });
    return found != shape.states.end() ? std::optional(found->firstLod) : std::nullopt;
}

std::optional<Box3F> SimInteriorSurface::containerBounds(std::optional<UInt32> state) const {
    const auto highest = highestDetail(state);
    if (!highest) {
        return {};
    }
    const auto lod = *highest;
    if (lod >= resources_.geometry.size() || !resources_.geometry[lod] ||
        !collision_detail::valid(resources_.geometry[lod]->bounds)) {
        return {};
    }
    Box3F result;
    m_mul(resources_.geometry[lod]->bounds, image_.placement, result);
    result.fMin -= 3;
    result.fMax += 3;
    return collision_detail::valid(result) ? std::optional(result) : std::nullopt;
}

bool SimInteriorSurface::configureContainer(bool open, UInt32 mask, bool pvs) {
    if (getManager() || spatial_ || isOpen() || getContainer() || (open && !linkInstance())) {
        return false;
    }
    const auto bounds = containerBounds(automaticState_);
    if (!bounds) {
        return false;
    }
    if (!setBoundingBox(*bounds) || !setContainerMask(mask)) {
        return false;
    }
    if (open && !installDatabase(std::make_unique<sim::SimDefaultContainerDatabase>())) {
        return false;
    }
    setType(getType() | sim::InteriorObjectType);
    spatial_ = true;
    pvs_ = open && pvs;
    return true;
}

bool SimInteriorSurface::changeDetail(std::optional<UInt32> state) {
    if (state == automaticState_) {
        return true;
    }
    bool result = false;
    const auto work = [&] {
        const auto highest = highestDetail(state);
        if (!highest) {
            return;
        }
        const auto old = automaticState_;
        const auto oldLod = linkLod_;
        automaticState_ = state;
        linkLod_ = state ? *highest : lod_;
        if (spatial_) {
            const auto bounds = containerBounds(state);
            if ((isOpen() && !linkInstance()) || !bounds || !setBoundingBox(*bounds)) {
                automaticState_ = old;
                linkLod_ = oldLod;
                return;
            }
        }
        ++detailRevision_;
        result = !isRemoved() && !isDeleted();
    };
    if (auto* manager = getManager()) {
        manager->dispatch(work);
    } else {
        work();
    }
    return result;
}

bool SimInteriorSurface::setAutomaticDetail(UInt32 state) {
    if (!resources_.shape->detailByPixels(state, 0)) {
        return false;
    }
    return changeDetail(state);
}

bool SimInteriorSurface::setFixedDetail() {
    return changeDetail(std::nullopt);
}

std::optional<InteriorLinkInstance> SimInteriorSurface::linkInstance() const {
    const auto highest = highestDetail(automaticState_);
    if (!highest) {
        return {};
    }
    if (*highest >= resources_.geometry.size() || !resources_.geometry[*highest] ||
        linkLod_ >= resources_.geometry.size() || !resources_.geometry[linkLod_] ||
        linkLod_ >= resources_.shape->lods.size()) {
        return {};
    }
    return InteriorLinkInstance{resources_.geometry[*highest],
                                image_.placement,
                                resources_.shape->lods[linkLod_].linkableFaces,
                                resources_.shape->linked,
                                resources_.geometry[linkLod_],
                                forceRender_};
}

void SimInteriorSurface::commitLinkDetail(const WorldSurfaceQuery::LinkDetail& detail) {
    if (detail.revision == detailRevision_) {
        linkLod_ = detail.lod;
        ++detailRevision_;
    }
}

bool SimInteriorSurface::processQuery(sim::SimQuery& query) {
    if (auto* interior = dynamic_cast<WorldInteriorQuery*>(&query)) {
        interior->image = linkInstance();
        interior->pvs = pvs_;
        return true;
    }
    auto* surface = dynamic_cast<WorldSurfaceQuery*>(&query);
    if (!surface) {
        return SimContainer::processQuery(query);
    }
    if (surface->trimCaches) {
        cache_ = {};
        surface->image.reset();
        return true;
    }
    surface->linkDetail_.reset();
    surface->image = image_;
    const auto fail = [&] {
        surface->failed = true;
        surface->image.reset();
        return true;
    };
    UInt32 lod = lod_;
    std::optional<InteriorViewCone> cone;
    std::optional<TMat3F> objectToCamera;
    if ((visibilityEnabled_ || automaticState_) && surface->camera) {
        const auto& camera = *surface->camera;
        if (!collision_detail::rigid(camera.worldToCamera) || !std::isfinite(camera.focal.x) ||
            !std::isfinite(camera.focal.y) || camera.focal.x <= 0 || camera.focal.y <= 0) {
            return fail();
        }
        objectToCamera.emplace();
        m_mul(image_.placement, camera.worldToCamera, *objectToCamera);
        if (camera.perspective) {
            cone.emplace();
            cone->objectToCamera = *objectToCamera;
            cone->left = (Flt32(camera.viewport.left) - camera.center.x) / camera.focal.x;
            // Match renderer clipping at the viewport's outer right/bottom edges.
            cone->right = (Flt32(camera.viewport.right) + 1 - camera.center.x) / camera.focal.x;
            cone->top = (camera.center.y - Flt32(camera.viewport.top)) / camera.focal.y;
            cone->bottom = (camera.center.y - (Flt32(camera.viewport.bottom) + 1)) / camera.focal.y;
            cone->nearDistance = camera.nearDistance;
            cone->farDistance = camera.farDistance;
        }
        if (automaticState_) {
            auto selected = resources_.selectDetail(*automaticState_,
                                                    *objectToCamera,
                                                    camera.focal.x,
                                                    surface->linkRendering,
                                                    camera.perspective);
            if (!selected) {
                return fail();
            }
            lod = *selected;
        }
    }
    // Publish all camera-derived caches together, after lighting and visibility
    // succeed. Old frames and the last successful cache survive failed queries.
    auto candidate = cache_;
    auto base = image_.mesh;
    std::vector<InteriorLightSlot> slots;
    if (player_ && lod != lod_) {
        const auto lightingFor = [&](UInt32 detail) -> const InteriorLighting* {
            if (detail >= resources_.shape->lods.size()) {
                return nullptr;
            }
            const auto index = UInt64(resources_.shape->lods[detail].firstLightState) + lightState_;
            return index < resources_.lighting.size()
                       ? resources_.lighting[std::size_t(index)].get()
                       : nullptr;
        };
        const auto* target = lightingFor(lod);
        if (!target || !target->validAnimation()) {
            return fail();
        }
        auto mapped = player_->slotsForDetail(*target);
        if (!mapped) {
            return fail();
        }
        slots = std::move(*mapped);
    }
    if (lod != lod_) {
        if (candidate.lod != lod || !candidate.base || candidate.slots != slots) {
            InteriorSurfaceLighting options;
            options.mission = mission_.get();
            options.missionHandle = mission_;
            options.previous = candidate.lod == lod ? candidate.base.get() : nullptr;
            if (player_) {
                options.slots = slots;
            }
            auto mesh =
                WorldSurfaceMesh::interior(resources_, lod, lightState_, bool(player_), options);
            if (!mesh) {
                return fail();
            }
            base = std::make_shared<const WorldSurfaceMesh>(std::move(*mesh));
        } else {
            base = candidate.base;
        }
    }
    const bool baseChanged = candidate.base != base;
    candidate.base = base;
    candidate.lod = lod;
    candidate.slots = slots;
    surface->image->mesh = base;
    if (surface->lighting && player_) {
        auto lights = tsSceneLighting(*surface->lighting, image_.placement);
        if (!lights) {
            return fail();
        }
        if (!lights->lights.empty()) { // native ambient-only vector merging is a no-op
            if (baseChanged || !candidate.lighting || *candidate.lighting != *lights) {
                InteriorSurfaceLighting options;
                options.slots =
                    lod == lod_ ? player_->slots() : std::span<const InteriorLightSlot>(slots);
                options.mission = mission_.get();
                options.missionHandle = mission_;
                options.scene = &*lights;
                options.previous = candidate.scene ? candidate.scene.get() : base.get();
                auto mesh = WorldSurfaceMesh::interior(resources_, lod, lightState_, true, options);
                if (!mesh) {
                    return fail();
                }
                candidate.scene = std::make_shared<const WorldSurfaceMesh>(std::move(*mesh));
                candidate.lighting = std::move(lights);
            }
            surface->image->mesh = candidate.scene;
        }
    }
    if (visibilityEnabled_ && objectToCamera) {
        auto inverse = *objectToCamera;
        inverse.inverse();
        if (lod >= resources_.geometry.size() || !resources_.geometry[lod]) {
            return fail();
        }
        auto visible = resources_.geometry[lod]->visibility(inverse.p, cone ? &*cone : nullptr);
        if (!visible) {
            return fail();
        }
        const auto source = surface->image->mesh;
        if (candidate.visibleSource != source || candidate.surfaces != visible->surfaces) {
            auto mesh = source->visibleInterior(visible->surfaces);
            if (!mesh) {
                return fail();
            }
            candidate.visible = std::make_shared<const WorldSurfaceMesh>(std::move(*mesh));
            candidate.visibleSource = source;
            candidate.surfaces = std::move(visible->surfaces);
        }
        surface->image->mesh = candidate.visible;
    }
    cache_ = std::move(candidate);
    if (surface->camera) {
        surface->linkDetail_ = WorldSurfaceQuery::LinkDetail{lod, detailRevision_};
        if (!surface->deferLinkDetail_) {
            commitLinkDetail(*surface->linkDetail_);
        }
    }
    return true;
}

bool SimInteriorSurface::advanceAnimation(Flt32 seconds) {
    if (!player_) {
        return std::isfinite(seconds) && seconds >= 0;
    }
    auto next = *player_;
    return next.advance(seconds) && publish(std::move(next));
}

bool SimInteriorSurface::setLightTime(Int32 id, Flt32 seconds) {
    if (!player_) {
        return false;
    }
    auto next = *player_;
    return next.setTime(id, seconds) && publish(std::move(next));
}

bool SimInteriorSurface::setLightState(Int32 id, UInt32 state) {
    if (!player_) {
        return false;
    }
    auto next = *player_;
    return next.setState(id, state) && publish(std::move(next));
}

bool SimInteriorSurface::setLightLoopCount(Int32 id, UInt32 loops) {
    return player_ && player_->setLoopCount(id, loops);
}

bool SimInteriorSurface::setLightTimeScale(Int32 id, Flt32 scale) {
    return player_ && player_->setTimeScale(id, scale);
}

bool SimInteriorSurface::setLightPlaying(Int32 id, bool playing) {
    return player_ && player_->setPlaying(id, playing);
}
} // namespace opent1::world
