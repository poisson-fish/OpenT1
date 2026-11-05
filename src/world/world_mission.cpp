#include <opent1/world/world_mission.hpp>
#include <ranges>
#include "collision_geometry.hpp"
#include <opent1/core/crc16.hpp>
#include <limits>

namespace opent1::world {
namespace {
bool nameValid(std::string_view s) {
    return !s.empty() && s.size() <= 255 && s.find('\0') == s.npos;
}

bool pointIO(StreamIO& s, Point3F& p) {
    return s.read(p.x) && s.read(p.y) && s.read(p.z);
}

bool pointIO(StreamIO& s, const Point3F& p) {
    return s.write(p.x) && s.write(p.y) && s.write(p.z);
}

bool lightIO(StreamIO& s, InteriorMissionState::Light& l) {
    if (!s.read(l.time) || !s.read(l.active)) {
        return false;
    }
    for (auto& b : l.padding) {
        if (!s.read(b)) {
            return false;
        }
    }
    return s.read(l.dummy1) && s.read(l.dummy2);
}

bool lightIO(StreamIO& s, const InteriorMissionState::Light& l) {
    if (!s.write(l.time) || !s.write(l.active)) {
        return false;
    }
    for (auto b : l.padding) {
        if (!s.write(b)) {
            return false;
        }
    }
    return s.write(l.dummy1) && s.write(l.dummy2);
}

template <class T, class F> bool vectorRead(StreamIO& s, std::vector<T>& v, UInt32 limit, F read) {
    UInt32 n{};
    if (!s.read(n) || n > limit) {
        return false;
    }
    v.resize(n);
    for (auto& entry : v) {
        if (!read(s, entry)) {
            return false;
        }
    }
    return true;
}

bool interiorBody(StreamIO& s, InteriorMissionState& v, bool packet) {
    if (!s.readString(v.filename)) {
        return false;
    }
    if (packet) {
        v.container = static_cast<BitStream&>(s).readSignedInt(2);
    } else if (!s.read(v.container)) {
        return false;
    }
    if (!pointIO(s, v.position) || !pointIO(s, v.rotation)) {
        return false;
    }
    if (packet && !vectorRead(s, v.checks, 131072, [](auto& stream, auto& x) {
            return stream.read(x);
        })) {
        return false;
    }
    if (!vectorRead(s, v.lights, 4096, [](auto& stream, auto& x) {
            return lightIO(stream, x);
        })) {
        return false;
    }
    if (packet) {
        v.disableCollisions = static_cast<BitStream&>(s).readFlag();
    }
    return s.getStatus() == STRM_OK;
}

bool interiorBody(StreamIO& s, const InteriorMissionState& v, bool packet) {
    if (!s.writeString(v.filename)) {
        return false;
    }
    if (packet) {
        static_cast<BitStream&>(s).writeSignedInt(v.container, 2);
    } else if (!s.write(v.container)) {
        return false;
    }
    if (!pointIO(s, v.position) || !pointIO(s, v.rotation)) {
        return false;
    }
    if (packet) {
        if (!s.write(UInt32(v.checks.size()))) {
            return false;
        }
        for (auto x : v.checks) {
            if (!s.write(x)) {
                return false;
            }
        }
    }
    if (!s.write(UInt32(v.lights.size()))) {
        return false;
    }
    for (const auto& l : v.lights) {
        if (!lightIO(s, l)) {
            return false;
        }
    }
    if (packet) {
        static_cast<BitStream&>(s).writeFlag(v.disableCollisions);
    }
    return s.getStatus() == STRM_OK;
}

bool visibilityIO(StreamIO& s, TerrainMissionState& v, bool vertical = true) {
    return s.read(v.visibleDistance) && s.read(v.hazeDistance) &&
           (!vertical || (s.read(v.hazeMin) && s.read(v.hazeMax)));
}

bool visibilityIO(StreamIO& s, const TerrainMissionState& v) {
    return s.write(v.visibleDistance) && s.write(v.hazeDistance) && s.write(v.hazeMin) &&
           s.write(v.hazeMax);
}

bool detailIO(StreamIO& s, TerrainMissionState& v) {
    return s.read(v.perspectiveDistance) && s.read(v.screenSize);
}

bool detailIO(StreamIO& s, const TerrainMissionState& v) {
    return s.write(v.perspectiveDistance) && s.write(v.screenSize);
}

bool contextIO(StreamIO& s, TerrainMissionState& v) {
    return pointIO(s, v.position) && pointIO(s, v.rotation);
}

bool contextIO(StreamIO& s, const TerrainMissionState& v) {
    return pointIO(s, v.position) && pointIO(s, v.rotation);
}

bool movementIO(StreamIO& s, TerrainMissionState& v) {
    return pointIO(s, v.gravity) && s.read(v.drag) && s.read(v.height);
}

bool movementIO(StreamIO& s, const TerrainMissionState& v) {
    return pointIO(s, v.gravity) && s.write(v.drag) && s.write(v.height);
}
}

bool InteriorMissionState::valid() const {
    return nameValid(filename) && container >= -1 && container <= 1 &&
           collision_detail::finite(position) && collision_detail::finite(rotation) &&
           lights.size() <= 4096 && checks.size() <= 131072 &&
           std::ranges::all_of(lights, [](const auto& l) {
               return std::isfinite(l.time) && l.time >= 0;
           });
}

bool InteriorMissionState::open(const Box3F& b) const {
    return container == 1 ||
           (container == -1 && (b.len_x() >= 6 || b.len_y() >= 6 || b.len_z() >= 6));
}

bool InteriorMissionState::pack(BitStream& s, UInt32 mask) const {
    if (!valid()) {
        return false;
    }
    if (s.writeFlag((mask & 0x80000000U) != 0)) {
        return interiorBody(s, *this, true);
    }
    if (s.writeFlag((mask & 1) != 0)) {
        pointIO(s, position);
        pointIO(s, rotation);
    }
    if (s.writeFlag((mask & 2) != 0)) {
        for (const auto& l : lights) {
            s.writeFlag(l.active);
        }
    }
    if (s.writeFlag((mask & 4) != 0)) {
        s.writeFlag(editMode);
    }
    return s.isValid();
}

bool InteriorMissionState::unpack(BitStream& s) {
    auto v = *this;
    if (s.readFlag()) {
        if (!interiorBody(s, v, true)) {
            return false;
        }
    } else {
        if (s.readFlag() && (!pointIO(s, v.position) || !pointIO(s, v.rotation))) {
            return false;
        }
        if (s.readFlag()) {
            for (auto& l : v.lights) {
                l.active = s.readFlag();
            }
        }
        if (s.readFlag()) {
            v.editMode = s.readFlag();
        }
    }
    if (!s.isValid() || !v.valid()) {
        return false;
    }
    *this = std::move(v);
    return true;
}

bool InteriorMissionState::write(StreamIO& s) const {
    return valid() && s.write(Int32{2}) && interiorBody(s, *this, false);
}

bool InteriorMissionState::read(StreamIO& s) {
    Int32 version{};
    InteriorMissionState v;
    if (!s.read(version) || version != 2 || !interiorBody(s, v, false) || !v.valid()) {
        return false;
    }
    *this = std::move(v);
    return true;
}

bool TerrainMissionState::valid() const {
    return nameValid(filename) && collision_detail::finite(position) &&
           collision_detail::finite(rotation) && collision_detail::finite(gravity) &&
           std::isfinite(visibleDistance) && visibleDistance > 0 && std::isfinite(hazeDistance) &&
           hazeDistance >= 0 && std::isfinite(hazeMin) && std::isfinite(hazeMax) &&
           std::isfinite(perspectiveDistance) && perspectiveDistance > 0 &&
           std::isfinite(screenSize) && screenSize > 0 && screenSize <= 65536 &&
           std::isfinite(drag) && drag >= 0 && std::isfinite(height) && height >= 0;
}

bool TerrainMissionState::pack(BitStream& s, UInt32 mask) const {
    if (!valid()) {
        return false;
    }
    if (s.writeFlag((mask & File) != 0)) {
        s.writeString(filename);
    }
    if (s.writeFlag((mask & Visibility) != 0)) {
        visibilityIO(s, *this);
    }
    if (s.writeFlag((mask & Detail) != 0)) {
        detailIO(s, *this);
    }
    if (s.writeFlag((mask & Context) != 0)) {
        contextIO(s, *this);
    }
    if (s.writeFlag((mask & Movement) != 0)) {
        movementIO(s, *this);
    }
    s.writeFlag(suspend);
    if (s.writeFlag((mask & Crc) != 0)) {
        s.write(crc);
    }
    return s.isValid();
}

bool TerrainMissionState::unpack(BitStream& s) {
    auto v = *this;
    if (s.readFlag() && !s.readString(v.filename)) {
        return false;
    }
    if (s.readFlag() && !visibilityIO(s, v)) {
        return false;
    }
    if (s.readFlag() && !detailIO(s, v)) {
        return false;
    }
    if (s.readFlag() && !contextIO(s, v)) {
        return false;
    }
    if (s.readFlag() && !movementIO(s, v)) {
        return false;
    }
    v.suspend = s.readFlag();
    if (s.readFlag() && !s.read(v.crc)) {
        return false;
    }
    if (!s.isValid() || !v.valid()) {
        return false;
    }
    *this = std::move(v);
    return true;
}

bool TerrainMissionState::write(StreamIO& s, Int32 version) const {
    // Native writer always writes four visibility floats, even for version 1.
    // Historical v1 is read-only: producing its broken writer is not useful.
    return valid() && version >= 2 && version <= 3 && contextIO(s, *this) &&
           visibilityIO(s, *this) && detailIO(s, *this) && s.writeString(filename) &&
           (version < 3 || movementIO(s, *this));
}

bool TerrainMissionState::read(StreamIO& s, Int32 version) {
    TerrainMissionState v;
    if (version < 1 || version > 3 || !contextIO(s, v) || !visibilityIO(s, v, version >= 2) ||
        !detailIO(s, v) || !s.readString(v.filename) || (version >= 3 && !movementIO(s, v)) ||
        !v.valid()) {
        return false;
    }
    *this = std::move(v);
    return true;
}

std::optional<TerrainMissionPolicy> TerrainMissionState::policy(const TerrainMissionPreferences& p,
                                                                Int32 details) const {
    if (!valid() || !std::isfinite(p.visibleDistance) || p.visibleDistance <= 1 ||
        !std::isfinite(p.perspectiveScale) || p.perspectiveScale <= 0 ||
        !std::isfinite(p.textureDetail) || p.textureDetail < 0 || p.textureDetail > 1 ||
        details < 1 || details > 11) {
        return {};
    }
    TerrainMissionPolicy result;
    const auto hazeFactor = hazeDistance / visibleDistance;
    result.visibleDistance = std::min(visibleDistance, p.visibleDistance);
    // Preserve the original's squared authored haze ratio, including its cap.
    result.hazeDistance = std::min(hazeDistance * hazeFactor, result.visibleDistance * .75f);
    result.perspectiveDistance = perspectiveDistance * p.perspectiveScale;
    result.screenSize = screenSize;
    result.textureDetail = Int32(Flt32(details) * (1 - p.textureDetail));
    result.containerFarDistance = result.visibleDistance * .85f;
    if (!std::isfinite(result.perspectiveDistance) || result.textureDetail > 8) {
        return {};
    }
    return result;
}

namespace {
UInt32 lightCheck(const InteriorLighting& l) {
    const auto crc = [](const auto& bytes) {
        return computeCRC16(std::span(reinterpret_cast<const UInt8*>(bytes.data()), bytes.size()));
    };
    return (UInt32(crc(l.mapData)) << 16) | crc(l.names);
}

std::vector<UInt32> checks(const InteriorShapeResources& r) {
    std::vector<UInt32> result;
    for (const auto& g : r.geometry) {
        result.push_back(UInt32(g->buildId));
    }
    for (const auto& lod : r.shape->lods) {
        result.push_back(lightCheck(*r.lighting[lod.firstLightState]));
    }
    return result;
}
}

SimMissionInterior::SimMissionInterior() {
    setType(getType() | sim::InteriorObjectType);
    setContainerMask(~UInt32{0});
    setNetFlags(Ghostable | ScopeAlways);
}

bool SimMissionInterior::apply(const InteriorMissionState& input, ResourceManager& manager) {
    if (!input.valid()) {
        return false;
    }
    auto r = InteriorShapeResources::load(manager, input.filename);
    if (!r || r->geometry.empty() || r->lighting.empty()) {
        return false;
    }
    const bool sameShape =
        resources_.shape.get() == r->shape.get() && state_.filename == input.filename;
    const auto automatic = sameShape ? automaticState_ : std::optional<UInt32>(0);
    const auto previousLinkLod = linkLod_;
    auto next = input;
    const auto expected = checks(*r);
    if (isGhost() && next.checks != expected) {
        return false;
    }
    next.checks = expected;
    const auto& authored = r->lighting[r->shape->lods[0].firstLightState]->lights;
    if (next.lights.empty()) {
        next.lights.resize(authored.size());
        for (auto i : std::views::iota(std::size_t{0}, authored.size())) {
            next.lights[i].time = authored[i].duration;
            next.lights[i].active = (authored[i].flags & 1U) != 0;
        }
    }
    if (next.lights.size() != authored.size()) {
        return false;
    }
    const TMat3F placement(EulerF(next.rotation.x, next.rotation.y, next.rotation.z),
                           next.position);
    std::unique_ptr<SimInteriorSurface> candidate;
    try {
        candidate = std::make_unique<SimInteriorSurface>(*r, placement);
    } catch (const std::invalid_argument&) {
        return false;
    }
    if (sameShape && player_) {
        candidate->player_.reset();
        if (!candidate->publish(*player_)) {
            return false;
        }
    }
    for (auto i : std::views::iota(std::size_t{0}, authored.size())) {
        if (!sameShape || i >= state_.lights.size() ||
            state_.lights[i].time != next.lights[i].time) {
            if ((authored[i].duration > 0 && next.lights[i].time <= 0) ||
                !candidate->setLightTime(authored[i].id, 0) ||
                !candidate->setLightTimeScale(
                    authored[i].id,
                    authored[i].duration > 0 ? authored[i].duration / next.lights[i].time : 0)) {
                return false;
            }
        }
        if (!candidate->setLightPlaying(authored[i].id, next.lights[i].active)) {
            return false;
        }
    }
    if (automatic ? !candidate->setAutomaticDetail(*automatic) : !candidate->setFixedDetail()) {
        return false;
    }
    const auto open = next.open(r->geometry[0]->bounds);
    next.container = open ? 1 : 0;
    const auto bounds = candidate->containerBounds(automatic);
    if (!bounds) {
        return false;
    }
    auto geometry = r->geometry[0]->collision();
    if (!geometry) {
        return false;
    }
    std::vector<sim::SimCollisionMaterial> collisionMaterials;
    for (const auto& material : r->materials.list().materials) {
        collisionMaterials.push_back(
            {material.friction, material.elasticity, material.surfaceType});
    }
    auto collision = std::make_shared<SimInteriorCollisionImage>(
        std::make_shared<const InteriorCollision>(std::move(*geometry)), collisionMaterials);
    collision->transform = placement;
    if (!setBoundingBox(*bounds)) {
        return false;
    }
    if (open && !isOpen() &&
        !installDatabase(std::make_unique<sim::SimDefaultContainerDatabase>())) {
        return false;
    }
    if (!open && isOpen()) {
        uninstallDatabase();
    }
    if (isRemoved() || isDeleted()) {
        return false;
    }
    resources_ = std::move(*r);
    image_ = std::move(candidate->image_);
    player_ = std::move(candidate->player_);
    lod_ = lightState_ = 0;
    linkLod_ = sameShape ? previousLinkLod : 0;
    automaticState_ = automatic;
    ++detailRevision_;
    cache_ = {};
    spatial_ = true;
    pvs_ = open;
    forceRender_ = next.editMode;
    state_ = std::move(next);
    collision_ = std::move(collision);
    return true;
}

bool SimMissionInterior::setState(const InteriorMissionState& state) {
    if (applying_ || !state.valid()) {
        return false;
    }
    if (!getManager()) {
        state_ = state;
        return true;
    }
    auto* service = sim::SimResource::find(*getManager());
    if (!service) {
        return false;
    }

    struct Guard {
        bool& b;

        Guard(bool& v) : b(v) {
            b = true;
        }

        ~Guard() {
            b = false;
        }
    } guard(applying_);

    const auto previous = state_;
    bool ok = false;
    getManager()->dispatch([&] {
        ok = apply(state, service->get());
    });
    if (ok) {
        UInt32 mask{};
        if (previous.position != state_.position || previous.rotation != state_.rotation) {
            mask |= 1;
        }
        if (previous.editMode != state_.editMode) {
            mask |= 4;
        }
        if (previous.filename != state_.filename || previous.container != state_.container ||
            previous.checks != state_.checks ||
            previous.disableCollisions != state_.disableCollisions ||
            previous.lights.size() != state_.lights.size()) {
            mask |= 0x80000000U;
        } else {
            for (auto i : std::views::iota(std::size_t{0}, state_.lights.size())) {
                if (previous.lights[i].time != state_.lights[i].time) {
                    mask |= 0x80000000U;
                }
                if (previous.lights[i].active != state_.lights[i].active) {
                    mask |= 2;
                }
            }
        }
        if (mask) {
            setMaskBits(mask);
        }
    }
    return ok;
}

bool SimMissionInterior::onAdd() {
    auto* service = sim::SimResource::find(*getManager());
    return service && apply(state_, service->get()) && SimInteriorSurface::onAdd();
}

bool SimMissionInterior::advanceClientTime(UInt32 now) {
    if (applying_) {
        return false;
    }
    if (!lastUpdated_) {
        lastUpdated_ = now;
        return true;
    }
    if (now <= lastUpdated_) {
        return true;
    }
    const auto elapsed = ((UInt64(now) - lastUpdated_ + 66) / 67) * 67;
    if (elapsed > std::numeric_limits<UInt32>::max() - lastUpdated_ ||
        !advanceAnimation(Flt32(elapsed) / 1000.f)) {
        return false;
    }
    lastUpdated_ += UInt32(elapsed);
    return true;
}

bool SimMissionInterior::processQuery(sim::SimQuery& query) {
    if (auto* q = dynamic_cast<sim::SimCollisionImageQuery*>(&query)) {
        q->images.clear();
        if (!state_.disableCollisions && collision_) {
            q->images.push_back(std::make_shared<SimInteriorCollisionImage>(*collision_));
        }
        return true;
    }
    return SimInteriorSurface::processQuery(query);
}

UInt32
SimMissionInterior::packUpdate(sim::network::GhostManager&, UInt32 mask, BitStream& s) const {
    if (!state_.pack(s, mask)) {
        throw std::invalid_argument("interior packet rejected");
    }
    // Initial native records have no ForceEdit bit; send its delta afterward.
    return (mask & 0x80000000U) ? mask & 4U : 0;
}

bool SimMissionInterior::unpackUpdate(sim::network::GhostManager&, BitStream& s) {
    auto next = state_;
    return next.unpack(s) && setState(next);
}

Persistent::Error
SimMissionInterior::read(StreamIO& s, Int32, Int32 user, Persistent::LoadContext& context) {
    Int32 version{};
    sim::SimNetObject parent;
    InteriorMissionState next;
    if (getManager() || !s.read(version) || version != 2 ||
        parent.read(s, 0, user, context) != Persistent::Error::Ok ||
        !interiorBody(s, next, false) || !next.valid()) {
        return Persistent::Error::ReadError;
    }
    setId(parent.getId());
    setNetFlags(parent.netFlags() | Ghostable | ScopeAlways);
    state_ = std::move(next);
    return Persistent::Error::Ok;
}

Persistent::Error
SimMissionInterior::write(StreamIO& s, Int32, Int32 user, Persistent::StoreContext& context) const {
    return state_.valid() && s.write(Int32{2}) &&
                   SimNetObject::write(s, 0, user, context) == Persistent::Error::Ok &&
                   interiorBody(s, state_, false)
               ? Persistent::Error::Ok
               : Persistent::Error::WriteError;
}

SimMissionTerrain::SimMissionTerrain() {
    setType(getType() | sim::TerrainObjectType);
    setContainerMask(~UInt32{0});
    installDatabase(std::make_unique<sim::SimXYDB>());
    setNetFlags(Ghostable | ScopeAlways);
}

bool SimMissionTerrain::apply(const TerrainMissionState& input,
                              ResourceManager& manager,
                              const TerrainMissionPreferences& preferences) {
    if (!input.valid()) {
        return false;
    }
    auto resources = editedResources_ && input.filename == state_.filename
                         ? editedResources_
                         : TerrainGridResources::load(manager, input.filename);
    if (!resources) {
        return false;
    }
    const auto effective = input.policy(preferences, resources->grid->detailCount);
    if (!effective) {
        return false;
    }
    auto geometry = TerrainCollisionSnapshot::prepare(*resources);
    if (!geometry) {
        return false;
    }
    auto next = input;
    UInt32 crc{};
    if (!manager.getCrc(input.filename, crc)) {
        return false;
    }
    for (const auto& b : resources->grid->blocks) {
        if (!manager.getCrc(TerrainGridFile::blockName(input.filename, b.id), crc, crc)) {
            return false;
        }
    }
    if (!manager.getCrc(resources->grid->materialList, crc, crc) ||
        (isGhost() && crc != input.crc)) {
        return false;
    }
    next.crc = crc;
    const TMat3F placement(EulerF(next.rotation.x, next.rotation.y, next.rotation.z),
                           next.position);
    const auto& grid = *resources->grid;
    const auto scale = std::ldexp(1.f, grid.scale + grid.detailCount);
    const auto h = next.height ? next.height : grid.maximumHeight;
    const Point3F lo{0, 0, -h}, hi{Flt32(grid.size.x) * scale, Flt32(grid.size.y) * scale, h};
    Point3F a, b;
    m_mul(lo, placement, a);
    m_mul(hi, placement, b);
    const Box3F bounds(a, b);
    if (!collision_detail::valid(bounds)) {
        return false;
    }
    TerrainLodView policy;
    policy.compositeTextures = true;
    policy.nativeGeometry.emplace();
    policy.nativeVisibility.emplace();
    policy.nativeTextures.emplace();
    policy.nativeGeometry->screenSize = effective->screenSize;
    policy.nativeTextures->detail = effective->textureDetail;
    policy.perspectiveDistance = effective->perspectiveDistance;
    policy.hazeDistance = effective->hazeDistance;
    policy.textureTiming = TerrainTextureTiming::NativeSoftware;
    policy.nativeTraversal = true;
    policy.lightMapDensity = TerrainLightMapDensity::Native;
    policy.sceneLighting = true;
    std::vector<sim::SimCollisionMaterial> collisionMaterials;
    for (const auto& material : resources->materials.list().materials) {
        collisionMaterials.push_back(
            {material.friction, material.elasticity, material.surfaceType});
    }
    const Point2F heightRange{grid.minimumHeight, grid.maximumHeight};
    auto candidate = std::make_unique<SimTerrainSurface>(
        std::move(*resources), placement, true, std::nullopt, policy);
    if (terrain2Enabled_ && !candidate->setTerrain2(true)) {
        return false;
    }
    auto collision = std::make_shared<SimTerrainCollisionImage>(
        std::make_shared<const TerrainCollisionSnapshot>(std::move(*geometry)), collisionMaterials);
    collision->transform = placement;
    if (!setBoundingBox(bounds)) {
        return false;
    }
    if (next.filename != state_.filename) {
        editedResources_.reset();
    }
    surface_ = std::move(candidate);
    state_ = std::move(next);
    collision_ = std::move(collision);
    preferences_ = preferences;
    policy_ = *effective;
    heightRange_ = heightRange;
    return true;
}

bool SimMissionTerrain::setState(const TerrainMissionState& state) {
    if (applying_ || !state.valid()) {
        return false;
    }
    if (!getManager()) {
        if (state.filename != state_.filename) {
            editedResources_.reset();
        }
        state_ = state;
        return true;
    }
    auto* service = sim::SimResource::find(*getManager());
    if (!service) {
        return false;
    }

    struct Guard {
        bool& b;

        Guard(bool& v) : b(v) {
            b = true;
        }

        ~Guard() {
            b = false;
        }
    } guard(applying_);

    bool ok = false;
    getManager()->dispatch([&] {
        ok = apply(state, service->get(), preferences_);
    });
    if (ok) {
        setMaskBits(127);
    }
    return ok;
}

std::optional<TerrainGridResources> SimMissionTerrain::editableResources() const {
    if (editedResources_) {
        return editedResources_;
    }
    auto* service = getManager() ? sim::SimResource::find(*getManager()) : nullptr;
    return service ? TerrainGridResources::load(service->get(), state_.filename) : std::nullopt;
}

bool SimMissionTerrain::publishEditedResources(TerrainGridResources resources) {
    if (applying_ || !resources.grid || !getManager()) {
        return false;
    }
    auto* service = sim::SimResource::find(*getManager());
    if (!service) {
        return false;
    }

    struct Guard {
        bool& b;

        Guard(bool& v) : b(v) {
            b = true;
        }

        ~Guard() {
            b = false;
        }
    } guard(applying_);

    auto previous = editedResources_;
    editedResources_ = std::move(resources);
    bool ok = false;
    getManager()->dispatch([&] {
        ok = apply(state_, service->get(), preferences_);
    });
    if (!ok) {
        editedResources_ = std::move(previous);
        return false;
    }
    return true;
}

bool SimMissionTerrain::onAdd() {
    auto* service = sim::SimResource::find(*getManager());
    if (!service || !apply(state_, service->get(), preferences_) || !SimContainer::onAdd() ||
        !addToSet(sim::RenderSetId)) {
        return false;
    }
    auto* root = dynamic_cast<sim::SimContainer*>(getManager()->findObject(sim::RootContainerId));
    if (!root || !root->addObject(*this)) {
        return false;
    }
    if (!setId(sim::TerrainId)) {
        return false;
    }
    if (!isGhost()) {
        auto event = std::make_unique<sim::SimMessageEvent>();
        event->message = 2;
        event->time = getManager()->getCurrentTime() + 15;
        event->address = address();
        if (!postEvent(std::move(event))) {
            return false;
        }
    }
    return true;
}

bool SimMissionTerrain::setPreferences(const TerrainMissionPreferences& preferences) {
    if (applying_ || !state_.policy(preferences, 1)) {
        return false;
    }
    if (!getManager()) {
        preferences_ = preferences;
        return true;
    }
    auto* service = sim::SimResource::find(*getManager());
    if (!service) {
        return false;
    }

    struct Guard {
        bool& b;

        Guard(bool& v) : b(v) {
            b = true;
        }

        ~Guard() {
            b = false;
        }
    } guard(applying_);

    bool ok = false;
    getManager()->dispatch([&] {
        ok = apply(state_, service->get(), preferences);
    });
    return ok;
}

bool SimMissionTerrain::setTerrain2(bool enabled) {
#ifndef OPENT1_HAS_TERRAIN2
    if (enabled) {
        return false;
    }
#endif
    if (surface_ && !surface_->setTerrain2(enabled)) {
        return false;
    }
    terrain2Enabled_ = enabled;
    return true;
}

bool SimMissionTerrain::processEvent(const sim::SimEvent& event) {
    if (const auto* message = dynamic_cast<const sim::SimMessageEvent*>(&event);
        message && message->message == 2 && !isGhost()) {
        setMaskBits(TerrainMissionState::Visibility);
        auto next = std::make_unique<sim::SimMessageEvent>();
        next->message = 2;
        next->time = getManager()->getCurrentTime() + 30;
        next->address = address();
        return postEvent(std::move(next));
    }
    return SimContainer::processEvent(event);
}

bool TerrainMissionPolicy::renderChild(const sim::EnvironmentCamera& camera,
                                       const Box3F& box) const {
    Point3F center;
    m_mul((box.fMin + box.fMax) * .5f, camera.worldToCamera, center);
    const auto extent = box.fMax - box.fMin, radii = extent * .5f;
    const auto radiusSquared = m_dot(radii, radii);
    const auto right = (Flt32(camera.viewport.width()) - camera.center.x) / camera.focal.x;
    const auto left = -camera.center.x / camera.focal.x;
    const auto top = camera.center.y / camera.focal.y;
    const auto bottom = -(Flt32(camera.viewport.height()) - camera.center.y) / camera.focal.y;
    // Original Camera::testVisibility first rejects its bounding sphere, then
    // projects the FULL box dimensions (rather than radii) on boundary planes.
    // Preserve that deliberately conservative historical admission.
    const std::array<Point3F, 6> normals{
        {{1, -right, 0}, {-1, left, 0}, {0, -top, 1}, {0, bottom, -1}, {0, -1, 0}, {0, 1, 0}}};
    for (auto i : std::views::iota(std::size_t{0}, normals.size())) {
        const auto n = normals[i];
        const auto d = m_dot(center, n) + (i == 4   ? camera.nearDistance
                                           : i == 5 ? -containerFarDistance
                                                    : 0.f);
        const auto signedSquare = d * std::abs(d), bound = m_dot(n, n) * radiusSquared;
        if (signedSquare > bound) {
            return false;
        }
        if (signedSquare > -bound) {
            Flt32 projected{};
            for (auto j : std::views::iota(std::size_t{0}, std::size_t{3})) {
                const Point3F row{camera.worldToCamera.m[j][0],
                                  camera.worldToCamera.m[j][1],
                                  camera.worldToCamera.m[j][2]};
                projected += extent[j] * std::abs(m_dot(row, n));
            }
            if (d > projected) {
                return false;
            }
        }
    }
    auto inverse = camera.worldToCamera;
    inverse.inverse();
    const auto offset = (box.fMin + box.fMax) * .5f - inverse.p;
    // Native XYDB chooses this corner even when the camera is inside an axis.
    const Point3F closest{offset.x + (offset.x > 0 ? -radii.x : radii.x),
                          offset.y + (offset.y > 0 ? -radii.y : radii.y),
                          offset.z + (offset.z > 0 ? -radii.z : radii.z)};
    return !camera.perspective ||
           m_dot(closest, closest) <= containerFarDistance * containerFarDistance;
}

bool TerrainMissionPolicy::scopeChild(Point3F camera, Flt32 distance, const Box3F& box) {
    return ((box.fMin + box.fMax) * .5f - camera).len() <= distance;
}

Flt32 TerrainMissionPolicy::haze(Point3F camera, Point3F position) const {
    const auto distance = std::min((position - camera).len(), visibleDistance);
    if (!std::isfinite(distance) || visibleDistance <= 0) {
        return 0;
    }
    if (distance < hazeDistance) {
        return .01f;
    }
    const auto delta = visibleDistance - hazeDistance;
    if (delta <= 0) {
        return 1;
    }
    const auto factor = (distance - hazeDistance) / (delta + delta * .02f) - 1.f;
    return std::clamp(1.f - factor * factor, 0.f, 1.f);
}

TerrainMissionPolicy sceneVisibility(const sim::SimManager& manager) {
    if (const auto* terrain =
            dynamic_cast<const SimMissionTerrain*>(manager.findObject(sim::TerrainId))) {
        return terrain->policy();
    }
    return {1500, 1000, 500, 120, 1500, 0};
}

bool SimMissionTerrain::processQuery(sim::SimQuery& query) {
    if (auto* q = dynamic_cast<sim::SimCollisionImageQuery*>(&query)) {
        q->images.clear();
        if (collision_) {
            auto image = std::make_shared<SimTerrainCollisionImage>(*collision_);
            image->editor = q->detail == sim::SimCollisionImageQuery::Editor;
            q->images.push_back(std::move(image));
        }
        return true;
    }
    if (auto* q = dynamic_cast<WorldSurfaceQuery*>(&query)) {
        if (q->trimCaches)
            return surface_ ? surface_->processQuery(query) : true;
        if (state_.suspend) {
            q->image.reset();
            return true;
        }
        if (!surface_) {
            q->failed = true;
            return true;
        }
        auto local = *q;
        if (local.camera && local.camera->perspective) {
            local.camera->farDistance =
                std::min(local.camera->farDistance, policy_.visibleDistance);
        }
        const auto result = surface_->processQuery(local);
        q->image = std::move(local.image);
        q->failed = local.failed;
        return result;
    }
    return SimContainer::processQuery(query);
}

void SimMissionTerrain::enterContainer(sim::SimContainer& object) {
    SimContainer::enterContainer(object);
    if (auto* mover = dynamic_cast<sim::SimMovement*>(&object)) {
        mover->addForce(sim::MovementGravityForce, state_.gravity, true);
        mover->setContainerDrag(state_.drag);
        mover->setContainerCurrent({}, 0);
    }
}

void SimMissionTerrain::leaveContainer(sim::SimContainer& object) {
    if (auto* mover = dynamic_cast<sim::SimMovement*>(&object)) {
        mover->removeForce(sim::MovementGravityForce);
    }
    SimContainer::leaveContainer(object);
}

UInt32 SimMissionTerrain::packUpdate(sim::network::GhostManager&, UInt32 mask, BitStream& s) const {
    if (!state_.pack(s, mask)) {
        throw std::invalid_argument("terrain packet rejected");
    }
    return 0;
}

bool SimMissionTerrain::unpackUpdate(sim::network::GhostManager&, BitStream& s) {
    auto next = state_;
    return next.unpack(s) && setState(next);
}

Persistent::Error
SimMissionTerrain::read(StreamIO& s, Int32 version, Int32 user, Persistent::LoadContext& context) {
    sim::SimNetObject parent;
    TerrainMissionState next;
    if (getManager() || parent.read(s, 0, user, context) != Persistent::Error::Ok ||
        !next.read(s, version)) {
        return Persistent::Error::ReadError;
    }
    setId(parent.getId());
    setNetFlags(parent.netFlags() | Ghostable | ScopeAlways);
    state_ = std::move(next);
    return Persistent::Error::Ok;
}

Persistent::Error SimMissionTerrain::write(StreamIO& s,
                                           Int32 version,
                                           Int32 user,
                                           Persistent::StoreContext& context) const {
    return state_.valid() && SimNetObject::write(s, 0, user, context) == Persistent::Error::Ok &&
                   state_.write(s, version)
               ? Persistent::Error::Ok
               : Persistent::Error::WriteError;
}

bool registerWorldGhosts(sim::network::Registry& registry) {
    const auto interior = registry.registerGhost<SimMissionInterior>(522);
    const auto terrain = registry.registerGhost<SimMissionTerrain>(2);
    return interior && terrain;
}

bool registerWorldMissionClasses(Persistent::ClassRegistry& registry) {
    const auto interior =
        registry.registerClass<SimMissionInterior>("InteriorShape", Persistent::kPersTag, 522);
    const auto terrain =
        registry.registerClass<SimMissionTerrain>("SimTerrain", Persistent::kPersTag, 2);
    return interior && terrain;
}
} // namespace opent1::world
