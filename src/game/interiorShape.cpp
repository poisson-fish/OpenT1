#include <opent1/game/interior_shape.hpp>
#include <ranges>
#include <opent1/game/string_table.hpp>
#include <opent1/sim/sim_world_game.hpp>

#include <algorithm>
#include <bit>
#include <charconv>
#include <cmath>
#include <cstring>
#include <format>
#include <sstream>
#include <unordered_map>

namespace opent1::game {

namespace {
bool parsePoint3Field(std::string_view text, Point3F& out) noexcept {
    std::istringstream iss{std::string(text)};
    Flt32 x = 0.0f;
    Flt32 y = 0.0f;
    Flt32 z = 0.0f;
    if (!(iss >> x >> y >> z)) {
        return false;
    }
    out.set(x, y, z);
    return true;
}

std::shared_ptr<world::InteriorShapeResources>
loadInteriorResourcesWithFallback(ResourceManager& rm, std::string_view fileName) {
    if (fileName.empty()) {
        return nullptr;
    }
    auto resources = world::InteriorShapeResources::load(rm, fileName);
    return resources ? std::make_shared<world::InteriorShapeResources>(std::move(*resources))
                     : nullptr;
}

} // namespace

InteriorShape::InteriorShape() {
    setType(SimInteriorObjectType | StaticInteriorObjectType | SimMissionObjectType);
    setNetFlags(ScopeAlways | Ghostable);
}

InteriorShape::~InteriorShape() {
    shutdownLights();
}

void InteriorShape::initPersistFields() {}

void InteriorShape::setFileName(std::string_view in_pFileName) {
    fileName_.assign(in_pFileName.data(), in_pFileName.size());
    if (fileName_.size() > 4) {
        const std::string_view tail(fileName_.data() + fileName_.size() - 4, 4);
        const bool isDis = (tail[0] == '.') && (tail[1] == 'd' || tail[1] == 'D') &&
                           (tail[2] == 'i' || tail[2] == 'I') && (tail[3] == 's' || tail[3] == 'S');
        if (!isDis) {
            fileName_.append(".dis");
        }
    } else if (!fileName_.empty()) {
        fileName_.append(".dis");
    }
}

std::string InteriorShape::getBaseFilename() const {
    if (fileName_.size() <= 4) {
        return {};
    }
    return fileName_.substr(0, fileName_.size() - 4);
}

std::string InteriorShape::missionLitName() const {
    const std::string base = getBaseFilename();
    if (!sm_missionName.empty()) {
        std::string out = std::format("{}_{}{}.dil", sm_missionName, base, getId());
        for (char& ch : out) {
            if (ch == '/' || ch == '\\') {
                ch = '_';
            }
        }
        return out;
    }
    return std::format("{}.dil", base);
}

void InteriorShape::setShapePositionAndRotation(const Point3F& pos, const EulerF& rot) {
    shapePosition_ = pos;
    shapeRotation_ = rot;
    setShapeTransform();
    setMaskBits(PosRotMask);
}

void InteriorShape::setLightAnimParams(std::vector<LightAnimParam> params) {
    lightAnimParams_ = std::move(params);
    setMaskBits(LightActiveMask);
}

void InteriorShape::setShapeResources(std::shared_ptr<world::InteriorShapeResources> res) {
    shapeResources_ = std::move(res);
    cachedSurfaceMesh_.reset();
    cachedLightSlots_.clear();
    visibleSource_.reset();
    visibleMesh_.reset();
    visibleSurfaces_.clear();
    collisionImage_.reset();
    checkVals_.clear();
    if (shapeResources_ && shapeResources_->shape) {
        const auto& shape = *shapeResources_->shape;
        for (const auto& state : shape.states) {
            for (auto i : std::views::iota(UInt32{0}) | std::views::take_while([&](UInt32 i) {
                              return i < state.lodCount;
                          })) {
                const std::size_t lodIdx = static_cast<std::size_t>(state.firstLod + i);
                if (lodIdx < shapeResources_->geometry.size() &&
                    shapeResources_->geometry[lodIdx]) {
                    checkVals_.push_back(
                        static_cast<UInt32>(shapeResources_->geometry[lodIdx]->buildId));
                }
            }
        }
        if (!shapeResources_->geometry.empty() && shapeResources_->geometry[0]) {
            std::shared_ptr<const world::InteriorCollision> collPtr;
            if (auto collision = shapeResources_->geometry[0]->collision()) {
                collPtr = std::make_shared<const world::InteriorCollision>(std::move(*collision));
            }
            if (collPtr) {
                std::vector<sim::SimCollisionMaterial> materials;
                for (const auto& material : shapeResources_->materials.list().materials) {
                    materials.push_back(
                        {material.friction, material.elasticity, material.surfaceType});
                }
                auto img = std::make_shared<world::SimInteriorCollisionImage>(std::move(collPtr),
                                                                              materials);
                img->transform = getTransform();
                collisionImage_ = std::move(img);
            }
        }
        initLightParams();
        restartLights();
        setShapeTransform();
    }
}

world::InteriorMissionState InteriorShape::toMissionState() const {
    world::InteriorMissionState state{};
    state.filename = fileName_;
    state.container = isContainer_;
    state.position = shapePosition_;
    state.rotation = Point3F{shapeRotation_.x, shapeRotation_.y, shapeRotation_.z};
    state.checks = checkVals_;
    state.disableCollisions = disableCollisions_;
    state.editMode = inEditMode_;
    state.lights.reserve(lightAnimParams_.size());
    for (const auto& lp : lightAnimParams_) {
        state.lights.push_back(world::InteriorMissionState::Light{
            lp.time,
            lp.active,
            lp.padding,
            lp.dummy1,
            lp.dummy2,
        });
    }
    return state;
}

bool InteriorShape::fromMissionState(const world::InteriorMissionState& state) {
    if (!state.valid()) {
        return false;
    }
    fileName_ = state.filename;
    isContainer_ = state.container;
    shapePosition_ = state.position;
    shapeRotation_ = EulerF{state.rotation.x, state.rotation.y, state.rotation.z};
    checkVals_ = state.checks;
    disableCollisions_ = state.disableCollisions;
    inEditMode_ = state.editMode;
    lightAnimParams_.clear();
    lightAnimParams_.reserve(state.lights.size());
    for (const auto& l : state.lights) {
        lightAnimParams_.push_back(LightAnimParam{
            l.time,
            l.active,
            l.padding,
            l.dummy1,
            l.dummy2,
        });
    }
    setShapeTransform();
    return true;
}

void InteriorShape::setTeam(Int32 team) {
    Parent::setTeam(team);
    remapTeamTextures(team);
}

void InteriorShape::remapTeamTextures(Int32 in_newTeam) {
    if (in_newTeam == -1) {
        in_newTeam = 0;
    } else {
        in_newTeam += 1;
    }
    if (in_newTeam >= 9) {
        in_newTeam = 0;
    }
    teamDmlName_ = std::format("team{}.dml", in_newTeam);
}

void InteriorShape::getITRSpaceBBox(Box3F& out_rBox) const {
    if (shapeResources_ && shapeResources_->shape) {
        const auto& shape = *shapeResources_->shape;
        if (currentState_ < shape.states.size() && shape.states[currentState_].lodCount > 0) {
            const std::size_t lodIdx =
                static_cast<std::size_t>(shape.states[currentState_].firstLod);
            if (lodIdx < shapeResources_->geometry.size() && shapeResources_->geometry[lodIdx]) {
                out_rBox = shapeResources_->geometry[lodIdx]->bounds;
                return;
            }
        }
    }
    out_rBox = getLocalBoundingBox();
}

Box3F InteriorShape::buildContainerBox(const TMat3F& in_rMat) const {
    Box3F itrSpaceBBox{};
    getITRSpaceBBox(itrSpaceBBox);

    // Box3F's two-point constructor orders its endpoints. Seed the reduction
    // with a transformed corner so it cannot turn the sentinel into a huge box.
    Point3F firstPoint{};
    m_mul(itrSpaceBBox.fMin, in_rMat, firstPoint);
    Box3F wsBox{firstPoint, firstPoint};
    for (auto i : std::views::iota(Int32{0}, Int32{8})) {
        const Point3F osPoint((i & 1) ? itrSpaceBBox.fMin.x : itrSpaceBBox.fMax.x,
                              (i & 2) ? itrSpaceBBox.fMin.y : itrSpaceBBox.fMax.y,
                              (i & 4) ? itrSpaceBBox.fMin.z : itrSpaceBBox.fMax.z);
        Point3F wsPoint{};
        m_mul(osPoint, in_rMat, wsPoint);
        wsBox.fMin.setMin(wsPoint);
        wsBox.fMax.setMax(wsPoint);
    }
    // Native interior containers extend three units beyond their geometry to
    // admit adjoining interiors and cameras crossing a linked boundary.
    wsBox.fMin -= Point3F{3, 3, 3};
    wsBox.fMax += Point3F{3, 3, 3};
    return wsBox;
}

void InteriorShape::setShapeTransform() {
    TMat3F transform(shapeRotation_, shapePosition_);
    transform.flags |= TMat3F::Matrix_HasTranslation | TMat3F::Matrix_HasRotation;
    setPosition(transform, true);
    setBoundingBox(buildContainerBox(getTransform()));
}

bool InteriorShape::getWeatherDistance(const Point3F& in_rWorldPt, Flt32& out_rDistance) {
    if (disableCollisions_) {
        return false;
    }
    Point3F itrSpacePoint{};
    m_mul(in_rWorldPt, getInvTransform(), itrSpacePoint);

    Box3F bbox{};
    getITRSpaceBBox(bbox);
    if (!bbox.contains(itrSpacePoint)) {
        return false;
    }
    out_rDistance = std::max(0.0f, bbox.fMax.z - itrSpacePoint.z);
    return true;
}

bool InteriorShape::getAbuttingFace(Int32 /*in_outsideBits*/,
                                    const Point2F& in_rMinPt,
                                    const Point2F& in_rMaxPt,
                                    Point3F* out_pV0,
                                    Point3F* out_pV1,
                                    Point3F* out_pV2,
                                    Point3F* out_pV3) const {
    if (!out_pV0 || !out_pV1 || !out_pV2 || !out_pV3) {
        return false;
    }
    Box3F bbox{};
    getITRSpaceBBox(bbox);
    if (in_rMinPt.x < bbox.fMin.x || in_rMaxPt.x > bbox.fMax.x || in_rMinPt.y < bbox.fMin.y ||
        in_rMaxPt.y > bbox.fMax.y) {
        return false;
    }
    *out_pV0 = Point3F{in_rMinPt.x, in_rMinPt.y, bbox.fMin.z};
    *out_pV1 = Point3F{in_rMaxPt.x, in_rMinPt.y, bbox.fMin.z};
    *out_pV2 = Point3F{in_rMaxPt.x, in_rMaxPt.y, bbox.fMin.z};
    *out_pV3 = Point3F{in_rMinPt.x, in_rMaxPt.y, bbox.fMin.z};
    return true;
}

void InteriorShape::initLightParams() {
    if (!shapeResources_ || !shapeResources_->shape) {
        return;
    }
    const auto& shape = *shapeResources_->shape;
    if (lightAnimParams_.size() < shape.lightStateNames.size()) {
        lightAnimParams_.resize(shape.lightStateNames.size());
    }
}

void InteriorShape::shutdownLights() {
    lightPlayer_.reset();
}

void InteriorShape::restartLights() {
    shutdownLights();
    if (!shapeResources_ || !shapeResources_->shape) {
        return;
    }
    const auto& shape = *shapeResources_->shape;
    if (currentState_ < shape.states.size() && shape.states[currentState_].lodCount > 0) {
        const std::size_t lodIdx = static_cast<std::size_t>(shape.states[currentState_].firstLod);
        if (lodIdx < shape.lods.size()) {
            const std::size_t lightIdx =
                static_cast<std::size_t>(shape.lods[lodIdx].firstLightState);
            if (lightIdx < shapeResources_->lighting.size() &&
                shapeResources_->lighting[lightIdx]) {
                if (auto playerOpt =
                        world::InteriorLightPlayer::create(*shapeResources_->lighting[lightIdx])) {
                    lightPlayer_ =
                        std::make_unique<world::InteriorLightPlayer>(std::move(*playerOpt));
                }
            }
        }
    }
}

bool InteriorShape::processArguments(std::span<const std::string> args) {
    if (args.empty()) {
        return false;
    }
    setFileName(args[0]);
    if (args.size() >= 2) {
        Int32 dc = 0;
        std::from_chars(args[1].data(), args[1].data() + args[1].size(), dc);
        disableCollisions_ = (dc != 0);
    }
    setShapeTransform();
    return true;
}

bool InteriorShape::onAdd() {
    if (fileName_.empty()) {
        if (const auto fn = getFieldValue("filename"); fn && !fn->empty()) {
            setFileName(*fn);
        } else if (const auto fn2 = getFieldValue("fileName"); fn2 && !fn2->empty()) {
            setFileName(*fn2);
        }
    }
    if (const auto ic = getFieldValue("isContainer"); ic && !ic->empty()) {
        Int32 val = isContainer_;
        std::from_chars(ic->data(), ic->data() + ic->size(), val);
        isContainer_ = val;
    }
    if (const auto posStr = getFieldValue("position"); posStr && !posStr->empty()) {
        Point3F pos{};
        if (parsePoint3Field(*posStr, pos)) {
            shapePosition_ = pos;
        }
    }
    if (const auto rotStr = getFieldValue("rotation"); rotStr && !rotStr->empty()) {
        Point3F rot{};
        if (parsePoint3Field(*rotStr, rot)) {
            shapeRotation_.set(rot.x, rot.y, rot.z);
        }
    }
    if (!Parent::onAdd()) {
        return false;
    }
    if (!shapeResources_ && !fileName_.empty() && getManager()) {
        if (auto* sr = sim::SimResource::find(*getManager())) {
            if (auto loaded = loadInteriorResourcesWithFallback(sr->get(), fileName_)) {
                setShapeResources(std::move(loaded));
            }
        }
    }
    if (!fileName_.empty() && !shapeResources_) {
        if (auto* console = resolveConsole()) {
            console->print("Unable to load interior " + fileName_);
        }
        return false;
    }
    if (shapeResources_ && !shapeResources_->geometry.empty() &&
        shapeResources_->geometry.front()) {
        const bool open = toMissionState().open(shapeResources_->geometry.front()->bounds);
        if (open && !isOpen() &&
            !installDatabase(std::make_unique<sim::SimDefaultContainerDatabase>())) {
            return false;
        }
        if (open) {
            setContainerMask(AllObjectTypes);
        }
        isContainer_ = open ? 1 : 0;
    }
    setShapeTransform();
    (void)addToSet(sim::RenderSetId);
    if (getManager() && !getContainer()) {
        if (auto* root =
                dynamic_cast<sim::SimContainer*>(getManager()->findObject(sim::RootContainerId))) {
            root->addObject(*this);
        }
    }
    return true;
}

void InteriorShape::onRemove() {
    shutdownLights();
    Parent::onRemove();
}

void InteriorShape::clientProcess(UInt32 curTime) {
    if (lastUpdatedTime_ == 0) {
        lastUpdatedTime_ = curTime;
        return;
    }
    const UInt32 timeStep = (curTime - lastUpdatedTime_) / 67U;
    if (timeStep > 0) {
        lastUpdatedTime_ += timeStep * 67U;
        const Flt32 floatStep = static_cast<Flt32>(timeStep * 67U) / 1000.0f;
        if (lightPlayer_) {
            lightPlayer_->advance(floatStep);
        }
    }
}

bool InteriorShape::processEvent(const sim::SimEvent& event) {
    if (const auto* editEv = dynamic_cast<const sim::SimEditEvent*>(&event)) {
        setInEditMode(editEv->state);
        return true;
    }
    if (const auto* xformEv = dynamic_cast<const sim::SimObjectTransformEvent*>(&event)) {
        EulerF rot{};
        xformEv->transform.angles(rot);
        setShapePositionAndRotation(xformEv->transform.p, rot);
        return true;
    }
    return Parent::processEvent(event);
}

bool InteriorShape::processQuery(sim::SimQuery& query) {
    if (auto* interior = dynamic_cast<world::WorldInteriorQuery*>(&query)) {
        if (shapeResources_ && shapeResources_->shape && !shapeResources_->geometry.empty() &&
            shapeResources_->geometry.front() && !shapeResources_->shape->lods.empty()) {
            interior->image =
                world::InteriorLinkInstance{shapeResources_->geometry.front(),
                                            getTransform(),
                                            shapeResources_->shape->lods.front().linkableFaces,
                                            shapeResources_->shape->linked,
                                            shapeResources_->geometry.front(),
                                            inEditMode_};
        }
        return true;
    }
    if (auto* xq = dynamic_cast<sim::SimObjectTransformQuery*>(&query)) {
        xq->transform = getTransform();
        return true;
    }
    if (auto* ciq = dynamic_cast<sim::SimCollisionImageQuery*>(&query)) {
        ciq->images.clear();
        if (!disableCollisions_ && collisionImage_) {
            auto img = std::make_shared<world::SimInteriorCollisionImage>(*collisionImage_);
            img->transform = getTransform();
            ciq->images.push_back(std::move(img));
        }
        return true;
    }
    if (auto* wsq = dynamic_cast<world::WorldSurfaceQuery*>(&query)) {
        if (wsq->trimCaches) {
            cachedSurfaceMesh_.reset();
            cachedLightSlots_.clear();
            visibleSource_.reset();
            visibleMesh_.reset();
            visibleSurfaces_.clear();
            wsq->image.reset();
            return true;
        }
        if (destroyed_) {
            return false;
        }
        if (!shapeResources_) {
            wsq->failed = true;
            return true;
        }
        if (!cachedSurfaceMesh_ || lightMapsInvalidated_ ||
            (lightPlayer_ && !std::ranges::equal(lightPlayer_->slots(), cachedLightSlots_))) {
            world::InteriorSurfaceLighting lighting;
            lighting.previous = cachedSurfaceMesh_.get();
            lighting.refreshMaps = lightMapsInvalidated_;
            lighting.seconds = static_cast<Flt32>(lastUpdatedTime_) * 0.001f;
            if (lightPlayer_) {
                lighting.slots = lightPlayer_->slots();
            }
            if (auto mesh =
                    world::WorldSurfaceMesh::interior(*shapeResources_, 0, 0, true, lighting)) {
                lightMapsInvalidated_ = false;
                cachedSurfaceMesh_ =
                    std::make_shared<const world::WorldSurfaceMesh>(std::move(*mesh));
                cachedLightSlots_ =
                    lightPlayer_ ? std::vector<world::InteriorLightSlot>(
                                       lightPlayer_->slots().begin(), lightPlayer_->slots().end())
                                 : std::vector<world::InteriorLightSlot>{};
            } else {
                wsq->failed = true;
                return true;
            }
        }
        if (!cachedSurfaceMesh_) {
            wsq->failed = true;
            return true;
        }
        TMat3F placement(shapeRotation_, shapePosition_);
        placement.flags = RMat3F::Matrix_HasRotation | RMat3F::Matrix_HasTranslation;
        wsq->image = world::WorldSurfaceInstance{cachedSurfaceMesh_, placement};
        if (wsq->camera && !shapeResources_->geometry.empty() &&
            shapeResources_->geometry.front()) {
            const auto& camera = *wsq->camera;
            world::InteriorViewCone cone;
            m_mul(placement, camera.worldToCamera, cone.objectToCamera);
            cone.left = (Flt32(camera.viewport.left) - camera.center.x) / camera.focal.x;
            cone.right = (Flt32(camera.viewport.right) + 1 - camera.center.x) / camera.focal.x;
            cone.top = (camera.center.y - Flt32(camera.viewport.top)) / camera.focal.y;
            cone.bottom = (camera.center.y - (Flt32(camera.viewport.bottom) + 1)) / camera.focal.y;
            cone.nearDistance = camera.nearDistance;
            cone.farDistance = camera.farDistance;
            auto inverse = cone.objectToCamera;
            inverse.inverse();
            const auto visible = shapeResources_->geometry.front()->visibility(
                inverse.p, camera.perspective ? &cone : nullptr);
            if (!visible) {
                wsq->failed = true;
                wsq->image.reset();
                return true;
            }
            if (visibleSource_ != cachedSurfaceMesh_ || visibleSurfaces_ != visible->surfaces) {
                auto mesh = cachedSurfaceMesh_->visibleInterior(visible->surfaces);
                if (!mesh) {
                    wsq->failed = true;
                    wsq->image.reset();
                    return true;
                }
                visibleMesh_ = std::make_shared<const world::WorldSurfaceMesh>(std::move(*mesh));
                visibleSource_ = cachedSurfaceMesh_;
                visibleSurfaces_ = visible->surfaces;
            }
            wsq->image->mesh = visibleMesh_;
        }
        return true;
    }
    return Parent::processQuery(query);
}

bool InteriorShape::inspectWrite(sim::Inspect& inspector) const {
    if (!Parent::inspectWrite(inspector)) {
        return false;
    }
    inspector.writeDivider();
    inspector.write(IDITG_POS, shapePosition_);
    Point3F rotDeg(static_cast<Flt32>(static_cast<RealD>(shapeRotation_.x) * 180.0 / kRealDPi),
                   static_cast<Flt32>(static_cast<RealD>(shapeRotation_.y) * 180.0 / kRealDPi),
                   static_cast<Flt32>(static_cast<RealD>(shapeRotation_.z) * 180.0 / kRealDPi));
    inspector.write(IDITG_ROTATION, rotDeg);
    inspector.writeDivider();
    inspector.write(IDITG_FILENAME, fileName_);
    return true;
}

bool InteriorShape::inspectRead(sim::Inspect& inspector) {
    if (!Parent::inspectRead(inspector)) {
        return false;
    }
    sim::Inspect::Value v{};
    if (!inspector.read(sim::Inspect::Type::Divider, 0, v)) {
        return false;
    }
    if (!inspector.read(sim::Inspect::Type::Point3F, IDITG_POS, v)) {
        return false;
    }
    const Point3F newPos = std::get<Point3F>(v);

    if (!inspector.read(sim::Inspect::Type::Point3F, IDITG_ROTATION, v)) {
        return false;
    }
    const Point3F newRotDeg = std::get<Point3F>(v);
    const EulerF newRot(static_cast<Flt32>(static_cast<RealD>(newRotDeg.x) * kRealDPi / 180.0),
                        static_cast<Flt32>(static_cast<RealD>(newRotDeg.y) * kRealDPi / 180.0),
                        static_cast<Flt32>(static_cast<RealD>(newRotDeg.z) * kRealDPi / 180.0));

    if (!inspector.read(sim::Inspect::Type::Divider, 0, v)) {
        return false;
    }
    if (!inspector.read(sim::Inspect::Type::IString, IDITG_FILENAME, v)) {
        return false;
    }
    const std::string newFile = std::get<std::string>(v);
    if (!newFile.empty() && newFile != fileName_) {
        setFileName(newFile);
    }
    setShapePositionAndRotation(newPos, newRot);
    return true;
}

void InteriorShape::writeCheckVals(BitStream& stream) const {
    const UInt32 count = static_cast<UInt32>(checkVals_.size());
    stream.write(count);
    for (const UInt32 val : checkVals_) {
        stream.write(val);
    }
}

bool InteriorShape::readCheckVals(BitStream& stream, std::vector<UInt32>& outVals) const {
    UInt32 size = 0;
    if (!stream.read(size) || size > 4096U) {
        return false;
    }
    outVals.resize(static_cast<std::size_t>(size));
    for (auto i : std::views::iota(UInt32{0}) | std::views::take_while([&](UInt32 i) {
                      return i < size;
                  })) {
        UInt32 val = 0;
        if (!stream.read(val)) {
            return false;
        }
        outVals[static_cast<std::size_t>(i)] = val;
    }
    return stream.isValid();
}

void InteriorShape::writeInitialPacket(BitStream& stream) const {
    stream.writeString(fileName_);
    stream.writeSignedInt(isContainer_, 2);
    stream.writeBytes(std::as_bytes(std::span(&shapePosition_, 1)));
    stream.writeBytes(std::as_bytes(std::span(&shapeRotation_, 1)));
    writeCheckVals(stream);

    const UInt32 count = static_cast<UInt32>(lightAnimParams_.size());
    stream.write(count);
    if (count > 0) {
        stream.writeBytes(std::as_bytes(std::span(lightAnimParams_)));
    }
    stream.writeFlag(disableCollisions_);
}

bool InteriorShape::readInitialPacket(BitStream& stream) {
    std::string nameStr{};
    if (!stream.readString(nameStr)) {
        return false;
    }
    const Int32 newIsCont = stream.readSignedInt(2);
    Point3F newPos{};
    EulerF newRot{};
    if (!stream.readBytes(std::as_writable_bytes(std::span(&newPos, 1))) ||
        !stream.readBytes(std::as_writable_bytes(std::span(&newRot, 1)))) {
        return false;
    }
    std::vector<UInt32> newChecks{};
    if (!readCheckVals(stream, newChecks)) {
        return false;
    }
    UInt32 numLightParams = 0;
    if (!stream.read(numLightParams) || numLightParams > 4096U) {
        return false;
    }
    std::vector<LightAnimParam> newLights(numLightParams);
    if (numLightParams > 0) {
        if (!stream.readBytes(std::as_writable_bytes(std::span(newLights)))) {
            return false;
        }
    }
    const bool newDisable = stream.readFlag();
    if (!stream.isValid()) {
        return false;
    }

    setFileName(nameStr);
    isContainer_ = newIsCont;
    shapePosition_ = newPos;
    shapeRotation_ = newRot;
    checkVals_ = std::move(newChecks);
    lightAnimParams_ = std::move(newLights);
    disableCollisions_ = newDisable;
    setShapeTransform();
    return true;
}

UInt32
InteriorShape::packUpdate(sim::network::GhostManager& gm, UInt32 mask, BitStream& stream) const {
    const UInt32 retMask = Parent::packUpdate(gm, mask, stream);
    if (stream.writeFlag((mask & InitialUpdate) != 0)) {
        writeInitialPacket(stream);
    } else {
        if (stream.writeFlag((mask & PosRotMask) != 0)) {
            stream.writeBytes(std::as_bytes(std::span(&shapePosition_, 1)));
            stream.writeBytes(std::as_bytes(std::span(&shapeRotation_, 1)));
        }
        if (stream.writeFlag((mask & LightActiveMask) != 0)) {
            for (const auto& lp : lightAnimParams_) {
                stream.writeFlag(lp.active);
            }
        }
        if (stream.writeFlag((mask & EditModeMask) != 0)) {
            stream.writeFlag(inEditMode_);
        }
    }
    return retMask;
}

bool InteriorShape::unpackUpdate(sim::network::GhostManager& gm, BitStream& stream) {
    const auto savedPos = stream.getCurPos();
    InteriorShape scratch{};
    scratch.lightAnimParams_ = lightAnimParams_;
    if (!scratch.Parent::unpackUpdate(gm, stream)) {
        stream.setCurPos(savedPos);
        return false;
    }

    const bool isInitial = stream.readFlag();
    if (!stream.isValid()) {
        stream.setCurPos(savedPos);
        return false;
    }

    if (isInitial) {
        if (!scratch.readInitialPacket(stream)) {
            stream.setCurPos(savedPos);
            return false;
        }
        setTeam(scratch.getTeam());
        fileName_ = std::move(scratch.fileName_);
        isContainer_ = scratch.isContainer_;
        shapePosition_ = scratch.shapePosition_;
        shapeRotation_ = scratch.shapeRotation_;
        checkVals_ = std::move(scratch.checkVals_);
        lightAnimParams_ = std::move(scratch.lightAnimParams_);
        disableCollisions_ = scratch.disableCollisions_;
        cachedSurfaceMesh_.reset();
        if (!fileName_.empty() && getManager()) {
            if (auto* sr = sim::SimResource::find(*getManager())) {
                if (auto loaded = loadInteriorResourcesWithFallback(sr->get(), fileName_)) {
                    setShapeResources(std::move(loaded));
                }
            }
        }
        setShapeTransform();
    } else {
        Point3F newPos = shapePosition_;
        EulerF newRot = shapeRotation_;
        bool posChanged = false;
        if (stream.readFlag()) {
            if (!stream.readBytes(std::as_writable_bytes(std::span(&newPos, 1))) ||
                !stream.readBytes(std::as_writable_bytes(std::span(&newRot, 1)))) {
                stream.setCurPos(savedPos);
                return false;
            }
            posChanged = true;
        }

        std::vector<LightAnimParam> newLights = lightAnimParams_;
        if (stream.readFlag()) {
            for (auto& lp : newLights) {
                lp.active = stream.readFlag();
            }
        }

        bool newEditMode = inEditMode_;
        if (stream.readFlag()) {
            newEditMode = stream.readFlag();
        }

        if (!stream.isValid()) {
            stream.setCurPos(savedPos);
            return false;
        }

        setTeam(scratch.getTeam());
        if (posChanged) {
            shapePosition_ = newPos;
            shapeRotation_ = newRot;
            setShapeTransform();
        }
        lightAnimParams_ = std::move(newLights);
        inEditMode_ = newEditMode;
    }
    return true;
}

Persistent::Error
InteriorShape::read(StreamIO& sio, Int32 /*inVersion*/, Int32 user, Persistent::LoadContext& ctx) {
    const auto savedPos = sio.getPosition();
    Int32 fileVersion = 0;
    if (!sio.read(fileVersion)) {
        sio.setPosition(savedPos);
        return Persistent::Error::ReadError;
    }
    if (fileVersion != 1 && fileVersion != sm_fileVersion) {
        sio.setPosition(savedPos);
        return Persistent::Error::ReadError;
    }
    if (fileVersion >= 2) {
        if (Parent::read(sio, fileVersion, user, ctx) != Persistent::Error::Ok) {
            sio.setPosition(savedPos);
            return Persistent::Error::ReadError;
        }
    }

    std::string fname{};
    Int32 isCont = -1;
    Point3F pos{};
    EulerF rot{};
    UInt32 numLights = 0;
    if (!sio.readString(fname) || fname.empty() || !sio.read(isCont) ||
        !sio.readBytes(std::as_writable_bytes(std::span(&pos, 1))) ||
        !sio.readBytes(std::as_writable_bytes(std::span(&rot, 1))) || !sio.read(numLights) ||
        numLights > 4096U) {
        sio.setPosition(savedPos);
        return Persistent::Error::ReadError;
    }

    std::vector<LightAnimParam> lights(numLights);
    if (numLights > 0) {
        if (!sio.readBytes(std::as_writable_bytes(std::span(lights)))) {
            sio.setPosition(savedPos);
            return Persistent::Error::ReadError;
        }
    }

    setFileName(fname);
    isContainer_ = isCont;
    shapePosition_ = pos;
    shapeRotation_ = rot;
    lightAnimParams_ = std::move(lights);
    setShapeTransform();
    return (sio.getStatus() == STRM_OK) ? Persistent::Error::Ok : Persistent::Error::ReadError;
}

Persistent::Error InteriorShape::write(StreamIO& sio,
                                       Int32 /*inVersion*/,
                                       Int32 user,
                                       Persistent::StoreContext& ctx) const {
    const Int32 fileVersion = sm_fileVersion;
    if (!sio.write(fileVersion)) {
        return Persistent::Error::WriteError;
    }
    if (Parent::write(sio, sm_fileVersion, user, ctx) != Persistent::Error::Ok) {
        return Persistent::Error::WriteError;
    }
    sio.writeString(fileName_);
    sio.write(isContainer_);
    sio.writeBytes(std::as_bytes(std::span(&shapePosition_, 1)));
    sio.writeBytes(std::as_bytes(std::span(&shapeRotation_, 1)));

    const UInt32 numLights = static_cast<UInt32>(lightAnimParams_.size());
    sio.write(numLights);
    if (numLights > 0) {
        sio.writeBytes(std::as_bytes(std::span(lightAnimParams_)));
    }
    return (sio.getStatus() == STRM_OK) ? Persistent::Error::Ok : Persistent::Error::WriteError;
}

} // namespace opent1::game
