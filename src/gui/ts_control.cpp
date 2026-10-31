#include <opent1/gui/ts_control.hpp>
#include <opent1/sim/sim_input_action.hpp>
#include <cmath>
#include <limits>

namespace opent1::gui {
namespace {
bool finite(Point3F p) {
    return std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z);
}

std::optional<sim::EnvironmentCamera> cameraFor(sim::SimWorld& model,
                                                sim::SimAddress source,
                                                Point2I e,
                                                Point2I origin,
                                                sim::SimCameraQuery& query) {
    if (!source.resolve(model.manager()) || !model.manager().sendQuery(source, query) ||
        !source.resolve(model.manager())) {
        return {};
    }
    if (e.x < 1 || e.y < 1 || !std::isfinite(query.nearPlane) || !std::isfinite(query.farPlane) ||
        query.nearPlane <= 0 || query.farPlane <= query.nearPlane) {
        return {};
    }
    sim::EnvironmentCamera view;
    view.viewport = {0, 0, e.x - 1, e.y - 1};
    view.nearDistance = query.nearPlane;
    view.farDistance = query.farPlane;
    view.perspective = query.perspective;
    view.worldToCamera = query.transform;
    try {
        view.worldToCamera.inverse();
    } catch (...) {
        return {};
    }
    if (view.perspective) {
        if (!std::isfinite(query.fov) || query.fov <= 0 || query.fov >= 1.5707963f) {
            return {};
        }
        view.center = {float(e.x) * .5f, float(e.y) * .5f};
        view.focal.x = view.focal.y = float(e.x) * .5f / std::tan(query.fov);
    } else {
        const auto width = query.lowerRight.x - query.upperLeft.x,
                   height = query.upperLeft.y - query.lowerRight.y;
        if (!std::isfinite(width) || !std::isfinite(height) || width <= 0 || height <= 0) {
            return {};
        }
        // Original TS orthographic projection divides its homogeneous screen
        // coordinates by 1/near. Scale about the screen viewport center,
        // including asymmetric world bounds. EnvironmentCamera itself uses
        // ordinary parallel projection, so adapt only the native query here.
        view.focal = {float(e.x) / width * query.nearPlane, float(e.y) / height * query.nearPlane};
        view.center = {float(e.x) * .5f * (1 - query.nearPlane) - query.upperLeft.x * view.focal.x,
                       float(e.y) * .5f * (1 - query.nearPlane) + query.upperLeft.y * view.focal.y};
    }
    // Camera::setScreenViewport stores its absolute center minus 0.00001f.
    // Preserve that float rounding before converting to our local viewport:
    // subtracting an arbitrary local epsilon loses native boundary pixels.
    view.center.x +=
        (float(origin.x) + float(e.x) * .5f - .00001f) - (float(origin.x) + float(e.x) * .5f);
    view.center.y +=
        (float(origin.y) + float(e.y) * .5f - .00001f) - (float(origin.y) + float(e.y) * .5f);
    return view;
}

sim::SimObject* nextCameraObject(sim::SimManager& manager, sim::SimAddress current) {
    auto* set = dynamic_cast<sim::SimSet*>(manager.findObject(sim::CameraSetId));
    if (!set || set->empty()) {
        return nullptr;
    }
    const auto objects = set->objects();
    auto found = std::ranges::find_if(objects, [&](auto* o) {
        return o->getId() == current.objectId && o->generation() == current.generation;
    });
    return found == objects.end() || ++found == objects.end() ? objects.front() : *found;
}

sim::EnvironmentCamera croppedCamera(const sim::EnvironmentCamera& full,
                                     const sim::SimCameraQuery* query,
                                     Point2I extent,
                                     Point2I origin,
                                     RectI clip) {
    auto view = full;
    view.viewport = {0, 0, Int32(clip.width()) - 1, Int32(clip.height()) - 1};
    view.center.x -= float(clip.left - origin.x);
    view.center.y -= float(clip.top - origin.y);
    if (!query) {
        return view;
    }
    Point2F upper = query->upperLeft, lower = query->lowerRight;
    if (query->perspective) {
        const float width = query->nearPlane * std::tan(query->fov),
                    height = float(extent.y) / float(extent.x) * width;
        upper = {-width, height};
        lower = {width, -height};
    }
    const float hscale = (lower.x - upper.x) / float(extent.x),
                vscale = (lower.y - upper.y) / float(extent.y);
    const Point2F left{upper.x + float(clip.left - origin.x) * hscale,
                       upper.y + float(clip.top - origin.y) * vscale};
    const Point2F right{upper.x + float(clip.right + 1 - origin.x) * hscale,
                        upper.y + float(clip.bottom + 1 - origin.y) * vscale};
    const Point2F half{(right.x - left.x) * .5f, (left.y - right.y) * .5f},
        center{(left.x + right.x) * .5f, (left.y + right.y) * .5f};
    const Point2F screenHalf{float(clip.width()) * .5f, float(clip.height()) * .5f};
    view.focal = {query->nearPlane * screenHalf.x / half.x,
                  query->nearPlane * screenHalf.y / half.y};
    const float depth = query->perspective ? query->nearPlane : 1.f;
    view.center = {(float(clip.left + clip.right + 1) * .5f - .00001f) - float(clip.left) -
                       center.x * view.focal.x / depth,
                   (float(clip.top + clip.bottom + 1) * .5f - .00001f) - float(clip.top) +
                       center.y * view.focal.y / depth};
    return view;
}
}

TSControl::~TSControl() {
    switching_ = true;
    try {
        detachCamera();
    } catch (...) {
    }
}

bool TSControl::selectCamera(sim::SimManager& manager,
                             sim::SimObject* object,
                             sim::SimObject* data) {
    const auto valid = [&](sim::SimObject* o) {
        return !o || (o->getManager() == &manager && o->isProperlyAdded() && !o->isRemoved());
    };
    if (switching_ || isRemoved() || !valid(object) || !valid(data) ||
        (getManager() && getManager() != &manager) ||
        (observed_ && observed_->getManager() != &manager)) {
        return false;
    }
    const auto candidate = object ? object->address() : sim::SimAddress{},
               payload = data ? data->address() : sim::SimAddress{};
    bool accepted = false;
    manager.dispatch([&] {
        switching_ = true;
        struct Reset {
            bool& value;
            ~Reset() {
                value = false;
            }
        } reset{switching_};
        if (object && object == observed_) {
            sim::SimCameraRemountEvent event;
            event.data = payload;
            manager.sendEvent(candidate, event);
            accepted = candidate.resolve(manager) && !isRemoved();
            return;
        }
        // Reserve the candidate's observation before any externally visible
        // event. Query/resource preparation has already succeeded in attach.
        if (object) {
            deleteNotify(*object);
        }
        const auto previous = attached_;
        try {
            detachCamera();
        } catch (...) {
            if (auto* target = candidate.resolve(manager)) {
                clearNotify(*target);
            }
            throw;
        }
        if (isRemoved()) {
            if (auto* target = candidate.resolve(manager)) {
                clearNotify(*target);
            }
            return;
        }
        auto* target = object ? candidate.resolve(manager) : nullptr;
        if (object && !target) {
            return;
        }
        attached_ = candidate;
        observed_ = target;
        accepted = true;
        if (target) {
            sim::SimCameraMountEvent event;
            event.prevCamera = previous;
            event.data = payload;
            manager.sendEvent(candidate, event);
            accepted = candidate.resolve(manager) && !isRemoved();
        }
    });
    return accepted;
}

void TSControl::detachCamera() {
    auto* previous = observed_;
    const auto address = attached_;
    observed_ = nullptr;
    attached_ = {};
    if (previous) {
        clearNotify(*previous);
        if (auto* manager = previous->getManager(); manager && !previous->isRemoved()) {
            sim::SimCameraUnmountEvent event;
            manager->sendEvent(address, event);
        }
    }
}

bool TSControl::attach(sim::SimWorld& model, sim::SimObject* object, sim::SimObject* data) {
    bool accepted = false;
    model.manager().dispatch([&] {
        const auto valid = [&](sim::SimObject* o) {
            return !o ||
                   (o->getManager() == &model.manager() && o->isProperlyAdded() && !o->isRemoved());
        };
        if (switching_ || isRemoved() || !valid(object) || !valid(data) ||
            (getManager() && getManager() != &model.manager()) ||
            (observed_ && observed_->getManager() != &model.manager())) {
            return;
        }
        if (!object || object == observed_) {
            accepted = selectCamera(model.manager(), object, data);
            return;
        }
        // Build the candidate in an isolated viewport, then publish its resources
        // and lifecycle only after every camera/scene preparation succeeds.
        const auto source = object->address(), payload = data ? data->address() : sim::SimAddress{};
        std::optional<sim::EnvironmentCamera> view;
        TSControl staged;
        sim::SimCameraQuery query;
        {
            switching_ = true;
            struct Reset {
                bool& value;
                ~Reset() {
                    value = false;
                }
            } reset{switching_};
            view = cameraFor(model, source, fields_.extent, localToGlobal({}), query);
            if (!view || isRemoved() || !staged.resize({}, fields_.extent) ||
                !staged.prepare(model, *view)) {
                return;
            }
        }
        auto* target = source.resolve(model.manager());
        auto* resolvedData = data ? payload.resolve(model.manager()) : nullptr;
        if (!target || (data && !resolvedData) ||
            !selectCamera(model.manager(), target, resolvedData)) {
            return;
        }
        frame_ = std::move(staged.frame_);
        camera_ = view;
        renderedCamera_.reset();
        nativeCamera_ = query;
        accepted = true;
    });
    return accepted;
}

bool TSControl::nextCamera(sim::SimWorld& model) {
    auto* next = nextCameraObject(model.manager(), attached_);
    const auto result = attach(model, next);
    return next && result;
}

bool TSControl::queryCamera(sim::SimWorld& model) {
    if (!observed_ && !attached_.objectId) {
        return nextCamera(model);
    }
    bool accepted = false;
    model.manager().dispatch([&] {
        if (!observed_ || observed_->getManager() != &model.manager() || !attached_.objectId ||
            !attached_.resolve(model.manager())) {
            return;
        }
        sim::SimCameraQuery query;
        auto view = cameraFor(model, attached_, fields_.extent, localToGlobal({}), query);
        if (view && !isRemoved()) {
            accepted = prepare(model, *view);
            if (accepted) {
                nativeCamera_ = query;
            }
        }
    });
    return accepted;
}

bool TSControl::processEvent(const sim::SimEvent& event) {
    auto* action = dynamic_cast<const sim::SimActionEvent*>(&event);
    if (!action) {
        return Control::processEvent(event);
    }
    auto* manager = getManager();
    if (!manager && observed_) {
        manager = observed_->getManager();
    }
    if (!manager) {
        return false;
    }
    if (action->action == 101) {
        return selectCamera(*manager, nextCameraObject(*manager, attached_));
    }
    if (action->action == 103) {
        if (!std::isfinite(action->fValue) || action->fValue < 0 ||
            double(action->fValue) > double(std::numeric_limits<Int32>::max())) {
            return false;
        }
        if (auto* object = manager->findObject(sim::SimObjectId(action->fValue))) {
            return selectCamera(*manager, object);
        }
        return true;
    }
    return true;
}

void TSControl::onRemove() {
    switching_ = true;
    std::exception_ptr error;
    try {
        detachCamera();
    } catch (...) {
        error = std::current_exception();
    }
    Control::onRemove();
    switching_ = false;
    if (error) {
        std::rethrow_exception(error);
    }
}

void TSControl::onDeleteNotify(sim::SimObject& object) {
    if (observed_ == &object) {
        observed_ = nullptr;
        attached_ = {};
    }
    Control::onDeleteNotify(object);
}

bool TSControl::prepare(sim::SimWorld& model, const sim::EnvironmentCamera& view, bool lighting) {
    if (view.viewport.left || view.viewport.top || view.viewport.right != fields_.extent.x - 1 ||
        view.viewport.bottom != fields_.extent.y - 1 || view.focal.x <= 0 || view.focal.y <= 0 ||
        !std::isfinite(view.focal.x) || !std::isfinite(view.focal.y) ||
        !std::isfinite(view.center.x) || !std::isfinite(view.center.y)) {
        return false;
    }
    bool accepted = false;
    model.manager().dispatch([&] {
        try {
            auto inverse = view.worldToCamera;
            inverse.inverse();
            sim::VolumetricView volume;
            volume.cameraPosition = inverse.p;
            volume.visibleDistance = view.farDistance;
            volume.hazeDistance = view.farDistance * .5f;
            auto next = world::WorldSceneFrame::captureContainers(model, view, volume, lighting);
            if (!next || isRemoved()) {
                return;
            }
            frame_ = std::make_shared<const world::WorldSceneFrame>(std::move(*next));
            camera_ = view;
            renderedCamera_.reset();
            nativeCamera_.reset();
            accepted = true;
        } catch (...) {
        }
    });
    return accepted;
}

std::optional<MouseRay> TSControl::ray(const Event& event) const {
    const auto& active = cameraView();
    if (!active || !frame_) {
        return {};
    }
    const auto local = globalToLocal(event.point);
    const auto& c = *active;
    if (local.x < 0 || local.y < 0 || local.x >= fields_.extent.x || local.y >= fields_.extent.y) {
        return {};
    }
    const auto x = (float(local.x) - c.center.x) / c.focal.x,
               z = (c.center.y - float(local.y)) / c.focal.y;
    auto inverse = c.worldToCamera;
    inverse.inverse();
    Point3F origin = inverse.p, transformed;
    if (c.perspective) {
        m_mul(Point3F{x, 1, z}, inverse, transformed);
    } else {
        m_mul(Point3F{x, c.nearDistance, z}, inverse, origin);
        m_mul(Point3F{x, c.nearDistance + 1, z}, inverse, transformed);
    }
    auto direction = transformed - origin;
    const auto length = direction.len();
    if (!finite(origin) || !finite(direction) || !std::isfinite(length) || length <= 0) {
        return {};
    }
    direction *= 1.f / length;
    return MouseRay{event, origin, direction};
}

std::optional<sim::SimCollisionInfo>
TSControl::pick(sim::SimWorld& model, const Event& e, UInt32 mask) const {
    const auto r = ray(e);
    const auto& active = cameraView();
    if (!r || !active) {
        return {};
    }
    auto* root = dynamic_cast<sim::SimContainer*>(model.manager().findObject(sim::RootContainerId));
    if (!root) {
        return {};
    }
    const auto local = globalToLocal(e.point);
    const auto& cameraView = *active;
    const auto depth = cameraView.perspective ? cameraView.farDistance : 1;
    Point3F farPoint{(float(local.x) - cameraView.center.x) * depth / cameraView.focal.x,
                     cameraView.farDistance,
                     (cameraView.center.y - float(local.y)) * depth / cameraView.focal.y};
    auto inverse = cameraView.worldToCamera;
    inverse.inverse();
    Point3F end;
    m_mul(farPoint, inverse, end);
    sim::SimContainerQuery query;
    query.mask = mask;
    query.box = {r->origin, end};
    sim::SimCollisionInfo result;
    return root->findLOS(query, result, sim::SimCollisionImageQuery::Editor)
               ? std::optional(std::move(result))
               : std::nullopt;
}

void TSControl::tick(double) {
    auto model = world.lock();
    if (!model) {
        return;
    }
    if (camera) {
        auto next = camera(fields_.extent);
        if (!next || !prepare(*model, *next)) {
            throw std::runtime_error("GUI viewport preparation");
        }
    } else if (attached_.objectId) {
        if (!attached_.resolve(model->manager())) {
            attached_ = {};
            return;
        }
        if (!queryCamera(*model)) {
            throw std::runtime_error("GUI camera query");
        }
    }
}

bool TSControl::event(const Event& e) {
    if (produce3DMouseEvents && mouse &&
        (e.type == EventType::Move || e.type == EventType::Down || e.type == EventType::Drag ||
         e.type == EventType::Up)) {
        const auto r = ray(e);
        return r && mouse(*r);
    }
    return Control::event(e);
}

bool TSControl::draw(gfx::SoftwareSurface& s, Point2I p, RectI clip) const {
    if (sceneRenderedExternally) {
        return drawChildren(s, p, clip);
    }
    const auto before = s.clip();

    struct Restore {
        gfx::SoftwareSurface& s;
        RectI clip;

        ~Restore() {
            s.setClip(clip);
        }
    } restore{s, before};

    clip = {std::max({clip.left, before.left, p.x}),
            std::max({clip.top, before.top, p.y}),
            std::min({clip.right, before.right, p.x + fields_.extent.x - 1}),
            std::min({clip.bottom, before.bottom, p.y + fields_.extent.y - 1})};
    if (clip.left > clip.right || clip.top > clip.bottom) {
        return true;
    }
    s.setClip(clip);
    auto displayed = camera_;
    if (frame_) {
        const bool cropped = clip.left != p.x || clip.top != p.y ||
                             clip.right != p.x + fields_.extent.x - 1 ||
                             clip.bottom != p.y + fields_.extent.y - 1;
        const auto origin = cropped ? Point2I{clip.left, clip.top} : p;
        const auto extent =
            cropped ? Point2I{Int32(clip.width()), Int32(clip.height())} : fields_.extent;
        std::optional<world::WorldSceneFrame> reframed;
        if (cropped) {
            if (!camera_) {
                return false;
            }
            auto projection = croppedCamera(
                *camera_, nativeCamera_ ? &*nativeCamera_ : nullptr, fields_.extent, p, clip);
            reframed = frame_->reframed(projection);
            if (!reframed) {
                return false;
            }
            projection.viewport = camera_->viewport;
            projection.center.x += float(clip.left - p.x);
            projection.center.y += float(clip.top - p.y);
            displayed = projection;
        }
        auto surface = gfx::SoftwareSurface::create(extent.x, extent.y);
        if (!surface || (s.palette() && !surface->setPalette(*s.palette()))) {
            return false;
        }
        // Native successful rendering overlays the existing framebuffer. Copy
        // it before scene submission; index zero may be an opaque scene color.
        if (!surface->blit(s.bitmap(),
                           {origin.x, origin.y, origin.x + extent.x - 1, origin.y + extent.y - 1},
                           {}) ||
            !(reframed ? reframed->draw(*surface) : frame_->draw(*surface)) ||
            !s.blit(surface->bitmap(), origin)) {
            return false;
        }
    } else {
        s.fill({p.x, p.y, p.x + fields_.extent.x - 1, p.y + fields_.extent.y - 1}, 0);
    }
    if (!Control::draw(s, p, clip)) {
        return false;
    }
    renderedCamera_ = displayed;
    return true;
}

std::function<void()> TSControl::checkpoint() {
    auto base = Control::checkpoint();
    auto frame = frame_;
    const auto view = camera_;
    const auto native = nativeCamera_;
    const auto rendered = renderedCamera_;
    // Attachment is observed simulation state. A GUI rollback must never
    // resurrect an address deleted by a callback or undo published mount events.
    return [this, base = std::move(base), frame = std::move(frame), view, native, rendered] {
        base();
        frame_ = frame;
        camera_ = view;
        nativeCamera_ = native;
        renderedCamera_ = rendered;
    };
}

Persistent::Error
TSControl::write(StreamIO& s, Int32 v, Int32 u, Persistent::StoreContext& c) const {
    if (!s.write(produce3DMouseEvents)) {
        return Persistent::Error::WriteError;
    }
    return Control::write(s, v, u, c);
}

Persistent::Error TSControl::read(StreamIO& s, Int32 v, Int32 u, Persistent::LoadContext& c) {
    bool enabled{};
    if (!s.read(enabled)) {
        return Persistent::Error::ReadError;
    }
    const auto result = Control::read(s, v, u, c);
    if (result == Persistent::Error::Ok) {
        produce3DMouseEvents = enabled;
    }
    return result;
}

bool registerViewportClass(Persistent::ClassRegistry& r) {
    return r.registerClass<TSControl>("SimGui::TSControl", makeFourCC('S', 'G', 't', 's'));
}
} // namespace opent1::gui
