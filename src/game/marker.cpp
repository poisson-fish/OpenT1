#include <opent1/game/marker.hpp>
#include <opent1/game/string_table.hpp>
#include <opent1/sim/sim_world_game.hpp>

#include <charconv>

namespace opent1::game {

namespace {

[[nodiscard]] Flt32 parseFloatArg(std::string_view arg) noexcept {
    while (!arg.empty() && (arg.front() == ' ' || arg.front() == '\t')) {
        arg.remove_prefix(1);
    }
    Flt32 val = 0.0f;
    std::from_chars(arg.data(), arg.data() + arg.size(), val);
    return val;
}

[[nodiscard]] Point3F parsePoint3FField(std::string_view text) noexcept {
    Point3F pt{0.0f, 0.0f, 0.0f};
    std::array<Flt32*, 3> coords{&pt.x, &pt.y, &pt.z};
    for (Flt32* dst : coords) {
        while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) {
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

} // namespace

Marker::Marker() {
    setType(FearMarkerObjectType);
    setCollisionMask(0);
    setLocalBoundingBox(bbox_);
    collisionImage_ = std::make_shared<sim::SimCollisionBoxImage>();
    collisionImage_->box = bbox_;
    collisionImage_->trigger = false;
}

Marker::~Marker() = default;

void Marker::initPersistFields() {}

void Marker::setLinearPosition(const Point3F& pos) {
    TMat3F mat(EulerF{angPosition_.x, angPosition_.y, angPosition_.z}, pos);
    setPosition(mat, true);
    renderImage_.transform = getTransform();
}

void Marker::setAngPosition(const Point3F& rot) {
    angPosition_ = rot;
    TMat3F mat(EulerF{angPosition_.x, angPosition_.y, angPosition_.z}, getLinearPosition());
    setPosition(mat, true);
    renderImage_.transform = getTransform();
}

void Marker::setTeam(Int32 team) {
    Parent::setTeam(team);
    if (team != -1) {
        setAlwaysVisibleToTeam(team, true);
    }
}

bool Marker::initResources(GameBaseData* in_data) {
    if (!Parent::initResources(in_data)) {
        return false;
    }
    return dynamic_cast<MarkerData*>(in_data) != nullptr;
}

bool Marker::processArguments(std::span<const std::string> args) {
    if (args.empty()) {
        return false;
    }
    setDatFileName(args[0]);
    if (getManager() != nullptr && !loadDatFile()) {
        return false;
    }
    Point3F pos{0.0f, 0.0f, 0.0f};
    Point3F rot{0.0f, 0.0f, 0.0f};
    if (args.size() > 1) {
        for (std::size_t i = 0; i < args.size() - 1 && i < 3; ++i) {
            pos[i] = parseFloatArg(args[i + 1]);
        }
    }
    if (args.size() > 4) {
        for (std::size_t i = 0; i < args.size() - 4 && i < 3; ++i) {
            rot[i] = parseFloatArg(args[i + 4]);
        }
    }
    setLinearPosition(pos);
    setAngPosition(rot);
    return true;
}

bool Marker::onAdd() {
    if (const auto posField = getFieldValue("position"); posField && !posField->empty()) {
        setLinearPosition(parsePoint3FField(*posField));
    }
    if (const auto rotField = getFieldValue("rotation"); rotField && !rotField->empty()) {
        setAngPosition(parsePoint3FField(*rotField));
    }
    if (!Parent::onAdd()) {
        return false;
    }
    if (!isGhost()) {
        setNetFlags(Ghostable);
        addToSet(SensorVisibleSetId);
        if (getGroup() != nullptr) {
            const auto groupName = getGroup()->getName();
            if (groupName == "DropPoints" || groupName == "Random" || groupName == "MustChange") {
                setNetFlags(0);
            }
        }
    } else {
        addToSet(SimRenderSetId);
    }
    if (auto* mgr = getManager()) {
        if (auto* root = dynamic_cast<sim::SimContainer*>(mgr->findObject(sim::RootContainerId))) {
            root->addObject(*this);
        }
    }
    renderImage_.transform = getTransform();
    return true;
}

void Marker::onRemove() {
    if (auto* cont = getContainer()) {
        cont->removeObject(*this);
    }
    Parent::onRemove();
}

bool Marker::onSimEditEvent(const sim::SimEditEvent& event) {
    if (event.editor == sim::MissionEditorId) {
        inEditMode_ = event.state;
        if (inEditMode_) {
            setType(getType() | SimMissionObjectType);
            addToSet(SimRenderSetId);
        } else {
            setType(getType() & ~SimMissionObjectType);
            removeFromSet(SimRenderSetId);
        }
        return true;
    }
    return false;
}

bool Marker::onSimObjectTransformEvent(const sim::SimObjectTransformEvent& event) {
    setPosition(event.transform, true);
    EulerF euler{};
    event.transform.angles(euler);
    angPosition_.set(euler.x, euler.y, euler.z);
    renderImage_.transform = getTransform();
    setMaskBits(PositionMask | RotationMask);
    return true;
}

bool Marker::onSimObjectTransformQuery(sim::SimObjectTransformQuery& query) {
    query.transform = getTransform();
    return true;
}

bool Marker::onSimCollisionImageQuery(sim::SimCollisionImageQuery& query) {
    if ((inEditMode_ || isGhost()) && collisionImage_) {
        collisionImage_->transform = getTransform();
        query.images.push_back(collisionImage_);
        return true;
    }
    return false;
}

bool Marker::processEvent(const sim::SimEvent& event) {
    if (const auto* editEv = dynamic_cast<const sim::SimEditEvent*>(&event)) {
        return onSimEditEvent(*editEv);
    }
    if (const auto* xformEv = dynamic_cast<const sim::SimObjectTransformEvent*>(&event)) {
        return onSimObjectTransformEvent(*xformEv);
    }
    return Parent::processEvent(event);
}

bool Marker::processQuery(sim::SimQuery& query) {
    if (auto* xq = dynamic_cast<sim::SimObjectTransformQuery*>(&query)) {
        return onSimObjectTransformQuery(*xq);
    }
    if (auto* cq = dynamic_cast<sim::SimCollisionImageQuery*>(&query)) {
        return onSimCollisionImageQuery(*cq);
    }
    return Parent::processQuery(query);
}

bool Marker::inspectWrite(sim::Inspect& inspector) const {
    if (!Parent::inspectWrite(inspector)) {
        return false;
    }
    inspector.write(IDITG_POS, getLinearPosition());
    inspector.write(IDITG_ROTATION, getAngPosition());
    return true;
}

bool Marker::inspectRead(sim::Inspect& inspector) {
    if (!Parent::inspectRead(inspector)) {
        return false;
    }
    sim::Inspect::Value v{};
    if (!inspector.read(sim::Inspect::Type::Point3F, IDITG_POS, v)) {
        return false;
    }
    const Point3F pos = std::get<Point3F>(v);

    if (!inspector.read(sim::Inspect::Type::Point3F, IDITG_ROTATION, v)) {
        return false;
    }
    const Point3F rot = std::get<Point3F>(v);

    setLinearPosition(pos);
    setAngPosition(rot);
    setMaskBits(PositionMask | RotationMask);
    return true;
}

UInt32 Marker::packUpdate(sim::network::GhostManager& gm, UInt32 mask, BitStream& stream) const {
    Parent::packUpdate(gm, mask, stream);
    if (stream.writeFlag((mask & InitialUpdate) != 0)) {
        packDatFile(stream);
    }
    if (stream.writeFlag((mask & (PositionMask | RotationMask)) != 0)) {
        const Point3F& pos = getLinearPosition();
        stream.writeBytes(std::as_bytes(std::span(&pos, 1)));
        stream.writeBytes(std::as_bytes(std::span(&angPosition_, 1)));
    }
    return 0;
}

bool Marker::unpackUpdate(sim::network::GhostManager& gm, BitStream& stream) {
    const auto savedPos = stream.getCurPos();
    Marker scratch{};
    scratch.bindDataBlockManager(resolveDataBlockManager());
    if (!scratch.Parent::unpackUpdate(gm, stream)) {
        stream.setCurPos(savedPos);
        return false;
    }
    bool hasDat = false;
    if (stream.readFlag()) {
        if (!scratch.unpackDatFile(stream)) {
            stream.setCurPos(savedPos);
            return false;
        }
        hasDat = true;
    }
    Point3F newPos = getLinearPosition();
    Point3F newRot = angPosition_;
    bool hasTransform = false;
    if (stream.readFlag()) {
        if (!stream.readBytes(std::as_writable_bytes(std::span(&newPos, 1))) ||
            !stream.readBytes(std::as_writable_bytes(std::span(&newRot, 1)))) {
            stream.setCurPos(savedPos);
            return false;
        }
        hasTransform = true;
    }
    if (!stream.isValid()) {
        stream.setCurPos(savedPos);
        return false;
    }

    setTeam(scratch.getTeam());
    setName(scratch.getName());
    if (hasDat) {
        setDatFileId(scratch.getDatFileId());
        setDatFileName(scratch.getDatFileName());
        if (auto* dat = scratch.getDatPtr()) {
            initResources(dat);
        }
    }
    if (hasTransform) {
        setLinearPosition(newPos);
        setAngPosition(newRot);
    }
    return true;
}

Persistent::Error
Marker::read(StreamIO& sio, Int32 version, Int32 user, Persistent::LoadContext& ctx) {
    if (Parent::read(sio, version, user, ctx) != Persistent::Error::Ok) {
        return Persistent::Error::ReadError;
    }
    Point3F pos{};
    Point3F rot{};
    if (!sio.readBytes(std::as_writable_bytes(std::span(&pos, 1))) ||
        !sio.readBytes(std::as_writable_bytes(std::span(&rot, 1)))) {
        return Persistent::Error::ReadError;
    }
    setLinearPosition(pos);
    setAngPosition(rot);
    return (sio.getStatus() == STRM_OK) ? Persistent::Error::Ok : Persistent::Error::ReadError;
}

Persistent::Error
Marker::write(StreamIO& sio, Int32 version, Int32 user, Persistent::StoreContext& ctx) const {
    if (Parent::write(sio, version, user, ctx) != Persistent::Error::Ok) {
        return Persistent::Error::WriteError;
    }
    const Point3F pos = getLinearPosition();
    const Point3F rot = angPosition_;
    sio.writeBytes(std::as_bytes(std::span(&pos, 1)));
    sio.writeBytes(std::as_bytes(std::span(&rot, 1)));
    return (sio.getStatus() == STRM_OK) ? Persistent::Error::Ok : Persistent::Error::WriteError;
}

} // namespace opent1::game
