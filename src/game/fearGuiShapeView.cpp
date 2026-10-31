#include <opent1/game/shell_controls.hpp>

#include <algorithm>
#include <cmath>
#include <opent1/world/ts_static_shape.hpp>

namespace opent1::game {

namespace {
inline constexpr Int32 kIdItgFilename = 1100340;
inline constexpr Flt32 kTwoPi = 6.28318530717958647692f;
} // namespace

ShapeView::ShapeView() {
    fields_.extent = {120, 120};
    active = true;
}

void ShapeView::setShapeName(std::string_view name) {
    shapeName_ = std::string(name.substr(0, 255));
}

void ShapeView::rotateByDrag(Point2I deltaPx) noexcept {
    if (deltaPx.x != 0) {
        rotation_.z += static_cast<Flt32>(deltaPx.x) * 0.01f;
        if (rotation_.z >= kTwoPi) {
            rotation_.z -= kTwoPi;
        } else if (rotation_.z <= 0.0f) {
            rotation_.z = kTwoPi;
        }
    }
    if (deltaPx.y != 0) {
        rotation_.x += static_cast<Flt32>(deltaPx.y) * 0.01f;
        if (rotation_.x >= kTwoPi) {
            rotation_.x -= kTwoPi;
        } else if (rotation_.x <= 0.0f) {
            rotation_.x = kTwoPi;
        }
    }
}

void ShapeView::dragRotate(Point2I fromPt, Point2I toPt) noexcept {
    rotateByDrag({toPt.x - fromPt.x, fromPt.y - toPt.y});
}

void ShapeView::zoomByDrag(Int32 deltaY) noexcept {
    dist_ = std::clamp(dist_ + static_cast<Flt32>(deltaY) * 0.025f, minDist_, maxDist_);
}

void ShapeView::dragZoom(Point2I fromPt, Point2I toPt) noexcept {
    zoomByDrag(fromPt.y - toPt.y);
}

void ShapeView::stepAutoRotate(Flt32 elapsedSec) noexcept {
    if (autoRotate_ && elapsedSec >= 0.033f) {
        rotation_.z += 0.1f;
    }
}

void ShapeView::tick(double time) {
    if (!std::isfinite(time) || time < lastRotationTime_) {
        lastRotationTime_ = 0;
        return;
    }
    if (time - lastRotationTime_ >= .033) {
        stepAutoRotate(Flt32(time - lastRotationTime_));
        lastRotationTime_ = time;
    }
}

bool ShapeView::draw(gfx::SoftwareSurface& surface, Point2I offset, RectI clip) const {
    if (!shapeResources_ || shapeName_.empty()) {
        return Parent::draw(surface, offset, clip);
    }
    if (!shapePreview_ || shapePreviewName_ != shapeName_) {
        auto loaded = world::TsShapeResources::load(*shapeResources_, shapeName_);
        // Armor shop entries intentionally have no shapeFile, so the stock
        // selection script requests ".dts". Native ShapeView draws an empty
        // preview for a missing model; it must not abort the whole game frame.
        if (!loaded) {
            shapePreview_.reset();
            return Parent::draw(surface, offset, clip);
        }
        shapePreview_ = std::move(*loaded);
        shapePreviewName_ = shapeName_;
    }
    if (fields_.extent.x <= 0 || fields_.extent.y <= 0) {
        return true;
    }
    auto target = gfx::SoftwareSurface::create(fields_.extent.x, fields_.extent.y);
    if (!target || (surface.palette() && !target->setPalette(*surface.palette()))) {
        return false;
    }
    target->clear(0);
    world::TsStaticView view;
    view.width = fields_.extent.x;
    view.height = fields_.extent.y;
    view.center = {view.width * .5f, view.height * .5f};
    view.focal = {view.width * .5f / std::tan(.3f), view.width * .5f / std::tan(.3f)};
    view.shapeToCamera.set(EulerF{rotation_.x, rotation_.y, rotation_.z}, {});
    Point3F center;
    m_mul(shapePreview_->shape->center, static_cast<const RMat3F&>(view.shapeToCamera), center);
    const auto radius = std::max(.01f, shapePreview_->shape->radius);
    const auto fit = radius * 2 * std::max(view.focal.x / view.width, view.focal.y / view.height);
    view.shapeToCamera.p = Point3F{0, std::max(radius + .1f, fit * dist_ / 10), 0} - center;
    view.shapeToCamera.flags |= RMat3F::Matrix_HasTranslation;
    view.lighting.emplace();
    view.lighting->ambient = {.5f, .5f, .5f};
    world::TsLight light;
    light.direction = {0, 1, 0};
    light.intensity = {.7f, .7f, .7f};
    view.lighting->lights.push_back(light);
    auto frame = world::TsStaticShapeFrame::prepare(*shapePreview_, view);
    if (!frame || !frame->draw(*target)) {
        return false;
    }
    auto bitmap = target->bitmap();
    bitmap.attributes |= gfx::BitmapTransparent;
    const auto saved = surface.clip();
    surface.setClip(clip);
    const auto ok = surface.blit(bitmap, offset);
    surface.setClip(saved);
    return ok && drawChildren(surface, offset, clip);
}

bool ShapeView::event(const gui::Event& e) {
    if (e.type == gui::EventType::Down) {
        if (canvas() && !canvas()->lockMouse(this)) {
            return false;
        }
        shapeDragButton_ = e.button;
        shapeDrag_ = e.point;
        return true;
    }
    if (e.type == gui::EventType::Up || e.type == gui::EventType::Cancel) {
        if (canvas() && canvas()->capture() == this) {
            canvas()->unlockMouse();
        }
        shapeDragButton_ = 0;
        return true;
    }
    if (e.type == gui::EventType::Drag && shapeDragButton_) {
        if (shapeDragButton_ == 1) {
            dragRotate(shapeDrag_, e.point);
        } else {
            dragZoom(shapeDrag_, e.point);
        }
        shapeDrag_ = e.point;
        return true;
    }
    return Parent::event(e);
}

std::function<void()> ShapeView::checkpoint() {
    auto base = Parent::checkpoint();
    auto sh = shapeName_;
    const auto rot = rotation_;
    const auto dst = dist_;
    const auto ar = autoRotate_;
    const auto time = lastRotationTime_;
    const auto drag = shapeDrag_;
    const auto button = shapeDragButton_;
    return [this, base = std::move(base), sh = std::move(sh), rot, dst, ar, time, drag, button] {
        base();
        shapeName_ = sh;
        rotation_ = rot;
        dist_ = dst;
        autoRotate_ = ar;
        lastRotationTime_ = time;
        shapeDrag_ = drag;
        shapeDragButton_ = button;
    };
}

bool ShapeView::inspectWrite(sim::Inspect& inspector) const {
    inspector.write(kIdItgFilename, shapeName_);
    return true;
}

bool ShapeView::inspectRead(sim::Inspect& inspector) {
    sim::Inspect::Value v{};
    if (!inspector.read(sim::Inspect::Type::IString, kIdItgFilename, v)) {
        return false;
    }
    setShapeName(std::get<std::string>(v));
    return true;
}

Persistent::Error
ShapeView::read(StreamIO& sio, Int32 version, Int32 user, Persistent::LoadContext& ctx) {
    std::string sh{};
    if (!sio.readString(sh)) {
        return Persistent::Error::ReadError;
    }
    const auto err = Parent::read(sio, version, user, ctx);
    if (err != Persistent::Error::Ok) {
        return err;
    }
    shapeName_ = std::move(sh);
    return Persistent::Error::Ok;
}

Persistent::Error
ShapeView::write(StreamIO& sio, Int32 version, Int32 user, Persistent::StoreContext& ctx) const {
    if (shapeName_.size() > 255 || !sio.writeString(shapeName_)) {
        return Persistent::Error::WriteError;
    }
    return Parent::write(sio, version, user, ctx);
}

} // namespace opent1::game
