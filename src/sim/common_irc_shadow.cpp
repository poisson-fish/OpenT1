#include <opent1/sim/common_irc_shadow.hpp>
#include <opent1/sim/sim_world_game.hpp>
#include <algorithm>
#include <cmath>
#include <ranges>
#include <stdexcept>

namespace opent1::sim {
namespace {
bool finite(const Point3F& p) {
    return std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z) &&
           std::max({std::abs(p.x), std::abs(p.y), std::abs(p.z)}) <= 1e12f;
}

bool nonnegative(float v) {
    return std::isfinite(v) && v >= 0 && v <= 1e12f;
}

bool writeBody(StreamIO& s, const VolumetricState& v) {
    return v.position.write(s) && v.dimensions.write(s) && s.write(v.textureScale) &&
           s.write(v.transparency) && s.write(v.animationScale) && s.write(v.damagePerSecond) &&
           v.currentVelocity.write(s) && s.write(v.currentDrag) && s.write(v.containerDrag) &&
           s.write(v.density) && s.write(v.boxExtra) && s.write(v.subdivisions) &&
           s.writeString(v.materialName) && s.write(v.faces) && s.write(v.volumeType);
}

bool readBody(StreamIO& s, VolumetricState& v) {
    return v.position.read(s) && v.dimensions.read(s) && s.read(v.textureScale) &&
           s.read(v.transparency) && s.read(v.animationScale) && s.read(v.damagePerSecond) &&
           v.currentVelocity.read(s) && s.read(v.currentDrag) && s.read(v.containerDrag) &&
           s.read(v.density) && s.read(v.boxExtra) && s.read(v.subdivisions) &&
           s.readString(v.materialName) && s.read(v.faces) && s.read(v.volumeType) && v.valid();
}
}

bool VolumetricState::valid() const {
    return finite(position) && finite(dimensions) && dimensions.x >= 0 && dimensions.y >= 0 &&
           dimensions.z >= 0 && finite(currentVelocity) && nonnegative(textureScale) &&
           nonnegative(transparency) && transparency <= 1 && nonnegative(animationScale) &&
           nonnegative(damagePerSecond) && nonnegative(currentDrag) && nonnegative(containerDrag) &&
           nonnegative(density) && nonnegative(boxExtra) && subdivisions >= 1 &&
           subdivisions <= 256 && faces <= 63 && materialName.size() <= 255 &&
           materialName.find('\0') == std::string::npos;
}

SimVolumetric::SimVolumetric() {
    setNetFlags(ScopeAlways | Ghostable);
    setContainerMask(~UInt32{127});
    setBoundingBox(displayBox());
}

bool SimVolumetric::onAdd() {
    auto* root = getManager()
                     ? dynamic_cast<SimContainer*>(getManager()->findObject(RootContainerId))
                     : nullptr;
    if (!root || !root->addObject(*this) || !SimNetObject::onAdd()) {
        return false;
    }
    auto* render = dynamic_cast<SimSet*>(getManager()->findObject(RenderSetId));
    return !render || render->addObject(*this);
}

bool SimVolumetric::processQuery(SimQuery& query) {
    if (auto* render = dynamic_cast<SimVolumetricRenderQuery*>(&query)) {
        render->geometry = renderGeometry(render->view);
        render->animationScale = state_.animationScale;
        return true;
    }
    return SimDefaultOpenContainer::processQuery(query);
}

bool SimVolumetric::inspectWrite(Inspect& inspect) const {
    // Original commonEditor tag IDs and six face letters. The trailing colon
    // is punctuation, not a seventh face flag as in the legacy parser.
    std::string toggles = "Top Down Left Right Front Back:";
    constexpr std::string_view letters = "TDLRFB";
    std::ranges::for_each(std::views::iota(std::size_t{0}, letters.size()), [&](auto bit) {
        if (!(state_.faces & (UInt16{1} << bit))) {
            const auto position = toggles.find(letters[bit]);
            toggles[position] = char(letters[bit] + ('a' - 'A'));
        }
    });
    return SimObject::inspectWrite(inspect) && inspect.write(1010500, state_.position) &&
           inspect.write(1010501, state_.dimensions) &&
           inspect.write(1010502, state_.textureScale) &&
           inspect.write(1010503, state_.transparency) &&
           inspect.write(1010504, state_.animationScale) &&
           inspect.write(1010505, std::move(toggles)) &&
           inspect.write(1010515, Inspect::Tag{false, "IDITG_SV_TYPE*", state_.volumeType}) &&
           inspect.write(1010510, state_.currentVelocity) &&
           inspect.write(1010511, state_.currentDrag) && inspect.write(1010512, state_.density) &&
           inspect.write(1010513, state_.damagePerSecond) &&
           inspect.write(1010516, state_.containerDrag) &&
           inspect.write(1010509, state_.boxExtra) &&
           inspect.write(1010517, Inspect::Integer{state_.subdivisions}) &&
           inspect.write(1010514, state_.materialName);
}

bool SimVolumetric::inspectRead(Inspect& inspect) {
    // Decode/validate an owned candidate before changing either the object or
    // the caller's cursor. Actual material loading belongs to the asset layer.
    auto input = inspect;
    auto value = state_;
    std::string name, toggles;
    Inspect::Tag type;
    Inspect::Integer divisions;
    const auto read = [&]<class T>(Inspect::Type kind, Int32 tag, T& output) {
        Inspect::Value item;
        if (!input.read(kind, tag, item)) {
            return false;
        }
        const auto* decoded = std::get_if<T>(&item);
        if (!decoded) {
            return false;
        }
        output = *decoded;
        return true;
    };
    using T = Inspect::Type;
    if (!read(T::IString, 110066, name) || !read(T::Point3F, 1010500, value.position) ||
        !read(T::Point3F, 1010501, value.dimensions) ||
        !read(T::RealF, 1010502, value.textureScale) ||
        !read(T::RealF, 1010503, value.transparency) ||
        !read(T::RealF, 1010504, value.animationScale) || !read(T::IString, 1010505, toggles) ||
        !read(T::Tag, 1010515, type) || !read(T::Point3F, 1010510, value.currentVelocity) ||
        !read(T::RealF, 1010511, value.currentDrag) || !read(T::RealF, 1010512, value.density) ||
        !read(T::RealF, 1010513, value.damagePerSecond) ||
        !read(T::RealF, 1010516, value.containerDrag) || !read(T::RealF, 1010509, value.boxExtra) ||
        !read(T::Int32, 1010517, divisions) || !read(T::IString, 1010514, value.materialName)) {
        return false;
    }
    if (name.find('\0') != name.npos || name.size() > 255 || toggles.find('\0') != toggles.npos ||
        !std::isfinite(value.transparency) || !std::isfinite(value.animationScale) ||
        !std::isfinite(value.damagePerSecond)) {
        return false;
    }
    value.faces = 0;
    constexpr std::string_view letters = "TDLRFB";
    std::ranges::for_each(std::views::iota(std::size_t{0}, letters.size()), [&](auto bit) {
        if (toggles.find(letters[bit]) != toggles.npos) {
            value.faces |= UInt16(UInt16{1} << bit);
        }
    });
    value.volumeType = type.value;
    value.subdivisions = divisions.value;
    value.transparency = std::clamp(value.transparency, 0.f, 1.f);
    value.animationScale = std::max(0.f, value.animationScale);
    value.damagePerSecond = 0; // original inspectRead disables the unused periodic damage field
    if (!value.valid()) {
        return false;
    }
    bool applied{};
    const auto apply = [&] {
        if (!setState(std::move(value)) || isRemoved()) {
            return;
        }
        assignName(name);
        if (isRemoved()) {
            return;
        }
        setMaskBits(Edited | Moved);
        inspect = std::move(input);
        applied = true;
    };
    if (getManager()) {
        getManager()->dispatch(apply);
    } else {
        apply();
    }
    return applied;
}

bool SimVolumetric::setState(VolumetricState value) {
    if (!value.valid()) {
        return false;
    }
    auto box = Box3F(value.position, value.position + value.dimensions);
    box.fMin -= value.boxExtra;
    box.fMax += value.boxExtra;
    const auto apply = [&] {
        auto previous = std::move(state_);
        state_ = std::move(value);
        if (!setBoundingBox(box)) {
            state_ = std::move(previous);
            return false;
        }
        const auto moved = previous.position != state_.position;
        previous.position = state_.position;
        setMaskBits((moved ? UInt32{Moved} : 0U) | (previous != state_ ? UInt32{Edited} : 0U));
        if (getDatabase()) {
            const auto members = std::vector<SimContainer*>(getDatabase()->objects().begin(),
                                                            getDatabase()->objects().end());
            std::ranges::for_each(members, [&](auto* o) {
                if (auto* mover = dynamic_cast<SimMovement*>(o)) {
                    applyInfluence(*mover);
                }
            });
        }
        return !isRemoved();
    };
    bool result{};
    if (getManager()) {
        getManager()->dispatch([&] {
            result = apply();
        });
    } else {
        result = apply();
    }
    return result;
}

Flt32 SimVolumetric::submergedFraction(const Box3F& box) const {
    if (!finite(box.fMin) || !finite(box.fMax) || !box.isValid()) {
        throw std::invalid_argument("invalid volumetric bounds");
    }
    const auto display = displayBox();
    float fraction = 1;
    std::ranges::for_each(std::views::iota(std::size_t{0}, std::size_t{3}), [&](auto i) {
        const auto width = box.fMax[i] - box.fMin[i];
        if (width == 0) {
            if (box.fMin[i] < display.fMin[i] || box.fMin[i] >= display.fMax[i]) {
                fraction = 0;
            }
        } else {
            fraction *= std::clamp(
                (std::min(box.fMax[i], display.fMax[i]) - std::max(box.fMin[i], display.fMin[i])) /
                    width,
                0.f,
                1.f);
        }
    });
    return fraction;
}

bool SimVolumetric::applyInfluence(SimMovement& mover) {
    if (mover.getManager() != getManager() || mover.isRemoved()) {
        return false;
    }
    const auto fraction = submergedFraction(mover.getBoundingBox());
    const auto gravity = mover.getForce(MovementGravityForce);
    if (gravity && state_.density > 0 && fraction > 0) {
        if (!mover.addForce(MovementBuoyancyForce,
                            *gravity * (-state_.density * fraction / mover.coefficients().density),
                            true)) {
            return false;
        }
    } else {
        mover.removeForce(MovementBuoyancyForce);
    }
    return mover.setContainerDrag(state_.containerDrag * fraction) &&
           mover.setContainerCurrent(state_.currentVelocity * fraction,
                                     state_.currentDrag * fraction);
}

void SimVolumetric::enterContainer(SimContainer& object) {
    SimDefaultOpenContainer::enterContainer(object);
    if (auto* mover = dynamic_cast<SimMovement*>(&object)) {
        applyInfluence(*mover);
    }
}

void SimVolumetric::leaveContainer(SimContainer& object) {
    if (auto* mover = dynamic_cast<SimMovement*>(&object)) {
        mover->removeForce(MovementBuoyancyForce);
        mover->setContainerDrag(0);
        mover->setContainerCurrent({}, 0);
    }
    SimDefaultOpenContainer::leaveContainer(object);
}

bool SimVolumetric::updateObject(SimContainer& object) {
    if (auto* mover = dynamic_cast<SimMovement*>(&object)) {
        applyInfluence(*mover);
    }
    return SimDefaultOpenContainer::updateObject(object);
}

bool SimVolumetric::packUpdate(BitStream& stream, UInt32 mask) const {
    if ((mask & ~UInt32{3}) || !state_.valid()) {
        return false;
    }
    stream.writeInt(static_cast<Int32>(mask), 2);
    if ((mask & Moved) && !state_.position.write(stream)) {
        return false;
    }
    if (mask & Edited) {
        if (!(state_.dimensions.write(stream) && stream.write(state_.textureScale) &&
              stream.write(state_.transparency) && stream.write(state_.boxExtra) &&
              stream.write(state_.subdivisions) && stream.write(state_.animationScale) &&
              stream.writeString(state_.materialName))) {
            return false;
        }
        stream.writeInt(state_.faces, 6);
        if (!(state_.currentVelocity.write(stream) && stream.write(state_.currentDrag) &&
              stream.write(state_.containerDrag) && stream.write(state_.density))) {
            return false;
        }
    }
    return stream.isValid();
}

bool SimVolumetric::unpackUpdate(BitStream& stream) {
    auto value = state_;
    const auto mask = UInt32(stream.readInt(2));
    if ((mask & Moved) && !value.position.read(stream)) {
        return false;
    }
    if (mask & Edited) {
        if (!(value.dimensions.read(stream) && stream.read(value.textureScale) &&
              stream.read(value.transparency) && stream.read(value.boxExtra) &&
              stream.read(value.subdivisions) && stream.read(value.animationScale) &&
              stream.readString(value.materialName))) {
            return false;
        }
        value.faces = static_cast<UInt16>(stream.readInt(6));
        if (!(value.currentVelocity.read(stream) && stream.read(value.currentDrag) &&
              stream.read(value.containerDrag) && stream.read(value.density))) {
            return false;
        }
    }
    return stream.isValid() && setState(std::move(value));
}

Persistent::Error SimVolumetric::write(StreamIO& s, Int32, Int32, Persistent::StoreContext&) const {
    const auto flags = (netFlags() & ~UInt32{512}) | (isLocked() ? UInt32{512} : 0);
    return state_.valid() && s.write(isDynamicId() ? 0U : getId()) && s.write(flags) &&
                   s.write(UInt32{0}) && s.write(UInt32{0xffffffff}) && s.write(UInt32{0}) &&
                   writeBody(s, state_)
               ? Persistent::Error::Ok
               : Persistent::Error::WriteError;
}

Persistent::Error SimVolumetric::read(StreamIO& s, Int32, Int32, Persistent::LoadContext&) {
    UInt32 id{}, flags{}, pointer{}, objectId{}, managerId{};
    VolumetricState value;
    if (getManager() || !s.read(id) || !s.read(flags) || !s.read(pointer) || !s.read(objectId) ||
        !s.read(managerId) || pointer || !readBody(s, value) || !setState(std::move(value))) {
        return Persistent::Error::ReadError;
    }
    setId(id);
    return setNetFlags(netFlags() | flags) ? Persistent::Error::Ok : Persistent::Error::ReadError;
}

bool registerCommonClasses(Persistent::ClassRegistry& registry) {
    return registry.registerClass<SimVolumetric>(
        "SimVolumetric", makeFourCC('v', 'm', 't', 'r'), 134);
}

ShadowDetailSettings chooseShadowDetails(Flt32 size, Flt32 dot, Flt32 scale) {
    if (!nonnegative(size) || !nonnegative(scale) || !std::isfinite(dot) || dot < 0 || dot > 1) {
        throw std::invalid_argument("invalid shadow detail input");
    }
    const auto angle = dot < .5f ? 2.f * dot : 1.f;
    const auto adjustedSize = size * angle, adjustedDetail = adjustedSize * scale;
    ShadowDetailSettings settings;
    settings.visible = size * scale >= 4;
    if (adjustedDetail >= 160) {
        settings.bitmapDimension = 64;
        settings.blurMethod = 23;
    } else if (adjustedDetail >= 27) {
        settings.bitmapDimension = 64;
        settings.blurMethod = 3;
    } else if (adjustedSize >= 8) {
        settings.bitmapDimension = 32;
        settings.blurMethod = 2;
    } else if (size >= 8) {
        settings.bitmapDimension = 16;
        settings.blurMethod = 2;
    } else {
        settings.bitmapDimension = 16;
        settings.blurMethod = 0;
    }
    settings.updateDelta = size > 50 ? 0.f : size < 25 ? .1f : .1f - .1f * (size - 25.f) / 25.f;
    return settings;
}
} // namespace opent1::sim
