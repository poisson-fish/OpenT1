#include <opent1/game/shell_controls.hpp>
#include <ranges>

#include <format>
#include <cmath>
#include <opent1/world/ts_static_shape.hpp>

namespace opent1::game {

SkinView::SkinView() {
    shapeName_ = "marmor.dts";
    active = true;
    autoRotate_ = false;
    rotation_ = {-.25f, 0, 3.14159265f};
}

void SkinView::setSkin(std::string_view skinName, bool male) {
    skinName_ = skinName.empty() ? "base" : std::string(skinName);
    male_ = male;
    shapeName_ = resolvedShapeFilename();
}

void SkinView::cycleArmor() noexcept {
    curArmor_ =
        static_cast<ArmorType>((static_cast<Int32>(curArmor_) + 1) % static_cast<Int32>(NumArmors));
    shapeName_ = resolvedShapeFilename();
}

std::string SkinView::resolvedShapeFilename() const {
    switch (curArmor_) {
        case Light:
            return male_ ? "larmor.dts" : "lfemale.dts";
        case Medium:
            return male_ ? "marmor.dts" : "mfemale.dts";
        case Heavy:
        default:
            return "harmor.dts";
    }
}

std::string SkinView::resolvedMaterialListName() const {
    std::string_view base = "marmor";
    switch (curArmor_) {
        case Light:
            base = male_ ? "larmor" : "lfemale";
            break;
        case Medium:
            base = male_ ? "marmor" : "mfemale";
            break;
        case Heavy:
        default:
            base = "harmor";
            break;
    }
    return std::format("{}.{}.dml", skinName_, base);
}

void SkinView::tick(double time) {
    if (std::isfinite(time)) {
        animationTime_ = time;
    }
}

bool SkinView::draw(gfx::SoftwareSurface& surface, Point2I offset, RectI clip) const {
    if (!resources_ || fields_.extent.x <= 0 || fields_.extent.y <= 0) {
        return Parent::draw(surface, offset, clip);
    }
    const auto shapeName = resolvedShapeFilename();
    const auto key = skinName_ + ":" + shapeName;
    if (!preview_ || previewKey_ != key) {
        auto shape = resources_->load<world::TsShape>(shapeName);
        if (!shape || !shape->materials) {
            return false;
        }
        auto materials = world::TsMaterialResources::loadSkin(
            *resources_, *shape->materials, skinName_, shapeName);
        if (!materials) {
            return false;
        }
        preview_ = world::TsShapeResources{shape, std::move(*materials)};
        previewKey_ = key;
    }
    std::vector<world::TsAnimationSample> samples;
    const auto& shape = *preview_->shape;
    for (auto i : std::views::iota(std::size_t{0}) | std::views::take_while([&](std::size_t i) {
                      return i < shape.sequences.size();
                  })) {
        const auto name = shape.sequences[i].name;
        if (name >= 0 && std::size_t(name) < shape.names.size() && shape.names[name] == "run") {
            if (auto sample = world::TsAnimationSample::atTime(shape, Int32(i), animationTime_)) {
                samples.push_back(*sample);
            }
            break;
        }
    }
    auto pose = world::TsShapePose::sample(preview_->shape, samples);
    auto target = gfx::SoftwareSurface::create(fields_.extent.x, fields_.extent.y);
    if (!pose || !target || (surface.palette() && !target->setPalette(*surface.palette()))) {
        return false;
    }
    // Render into a local viewport so partial GUI clipping cannot distort the
    // projection or overwrite neighboring controls.
    target->clear(0);
    world::TsStaticView view;
    view.width = fields_.extent.x;
    view.height = fields_.extent.y;
    view.center = {Flt32(view.width) * .5f, Flt32(view.height) * .5f};
    view.focal = {Flt32(view.width) * .5f / std::tan(.3f), Flt32(view.width) * .5f / std::tan(.3f)};
    // Authored bounds include the entire animation envelope (death/tumble
    // poses). Use the native standing armor dimensions to frame a run preview.
    const auto center = shape.center;
    const auto fit = std::max((male_ ? 1.7f : 1.5f) * view.focal.x / view.width,
                              (male_ ? 2.65f : 2.5f) * view.focal.y / view.height);
    view.shapeToCamera.set(EulerF{rotation_.x, rotation_.y, rotation_.z}, {});
    Point3F rotatedCenter;
    m_mul(center, static_cast<const RMat3F&>(view.shapeToCamera), rotatedCenter);
    view.shapeToCamera.p = Point3F{0, std::max(2.f, fit) * dist_ / 10.f, 0} - rotatedCenter;
    view.shapeToCamera.flags |= RMat3F::Matrix_HasTranslation;
    view.lighting.emplace();
    view.lighting->ambient = {.7f, .7f, .7f};
    world::TsLight light;
    light.type = world::TsLight::Type::Directional;
    light.direction = {0, 1, 0};
    light.intensity = {.6f, .6f, .6f};
    view.lighting->lights.push_back(light);
    auto frame = world::TsStaticShapeFrame::prepare(*preview_, view, *pose);
    if (!frame || !frame->draw(*target)) {
        return false;
    }
    auto bitmap = target->bitmap();
    bitmap.attributes |= gfx::BitmapTransparent;
    const auto saved = surface.clip();
    surface.setClip(clip);
    const bool drawn = surface.blit(bitmap, offset);
    surface.setClip(saved);
    return drawn && drawChildren(surface, offset, clip);
}

bool SkinView::event(const gui::Event& event) {
    if (event.type == gui::EventType::Down) {
        if (canvas() && !canvas()->lockMouse(this)) {
            return false;
        }
        dragButton_ = event.button;
        lastDrag_ = event.point;
        return true;
    }
    if (event.type == gui::EventType::Up || event.type == gui::EventType::Cancel) {
        if (canvas() && canvas()->capture() == this) {
            canvas()->unlockMouse();
        }
        dragButton_ = 0;
        return true;
    }
    if (event.type == gui::EventType::Drag && dragButton_) {
        if (dragButton_ == 1) {
            dragRotate(lastDrag_, event.point);
        } else {
            dragZoom(lastDrag_, event.point);
        }
        lastDrag_ = event.point;
        return true;
    }
    return Parent::event(event);
}

std::function<void()> SkinView::checkpoint() {
    auto base = Parent::checkpoint();
    const auto skin = skinName_;
    const auto male = male_;
    const auto armor = curArmor_;
    const auto time = animationTime_;
    const auto drag = lastDrag_;
    const auto button = dragButton_;
    return [this, base = std::move(base), skin, male, armor, time, drag, button] {
        base();
        skinName_ = skin;
        male_ = male;
        curArmor_ = armor;
        animationTime_ = time;
        lastDrag_ = drag;
        dragButton_ = button;
    };
}

} // namespace opent1::game
