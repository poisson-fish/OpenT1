#include <opent1/game/trigger.hpp>
#include <ranges>
#include <opent1/game/string_table.hpp>
#include <opent1/sim/sim_world_game.hpp>

#include <algorithm>
#include <charconv>
#include <format>

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

Trigger::Trigger() {
    setType(TriggerObjectType | SimMissionObjectType);
    setCollisionMask(SimPlayerObjectType | SimProjectileObjectType | VehicleObjectType |
                     MoveableObjectType | MineObjectType);
    collisionImage_ = std::make_shared<sim::SimCollisionBoxImage>();
    collisionImage_->box = boundingBox_;
    collisionImage_->trigger = true;
    sphereCollisionImage_ = std::make_shared<sim::SimCollisionSphereImage>();
    sphereCollisionImage_->trigger = true;
}

Trigger::~Trigger() = default;

void Trigger::initPersistFields() {}

SimTime Trigger::currentSimTime() const noexcept {
    if (explicitSimTime_ >= 0.0f) {
        return explicitSimTime_;
    }
    if (const auto* mgr = getManager()) {
        return mgr->getCurrentTime();
    }
    return 0.0f;
}

void Trigger::setLinearPosition(const Point3F& pos) {
    TMat3F mat(EulerF{angPosition_.x, angPosition_.y, angPosition_.z}, pos);
    setPosition(mat, true);
    updateBoundingBox();
}

void Trigger::setAngPosition(const Point3F& rot) {
    angPosition_ = rot;
    TMat3F mat(EulerF{angPosition_.x, angPosition_.y, angPosition_.z}, getLinearPosition());
    setPosition(mat, true);
    updateBoundingBox();
}

void Trigger::setDimensions(const Point3F& dim) {
    boundingBox_.fMax = dim;
    boundingBox_.fMin = -dim;
    updateBoundingBox();
}

void Trigger::updateBoundingBox() {
    if (collisionImage_) {
        collisionImage_->box = boundingBox_;
    }
    const TMat3F& mat = getTransform();
    Point3F p = boundingBox_.fMax;
    p.x = mat.m[0][0] * p.x + mat.m[1][0] * p.y + mat.m[2][0] * p.z;
    p.y = mat.m[0][1] * p.x + mat.m[1][1] * p.y + mat.m[2][1] * p.z;
    p.z = mat.m[0][2] * p.x + mat.m[1][2] * p.y + mat.m[2][2] * p.z;
    Box3F box{};
    box.fMax = mat.p + p;
    box.fMin = mat.p - p;
    box.fMin.setMin(mat.p + p);
    box.fMax.setMax(mat.p - p);
    setBoundingBox(box);
    setLocalBoundingBox(boundingBox_);
    setMaskBits(SizedMask);
}

void Trigger::scriptExec(std::string_view fn, std::string_view arg) {
    const std::string resolvedFn = scriptName(fn);
    const std::string thisStr = scriptThis();
    eventHistory_.push_back(EventRecord{
        resolvedFn.empty() ? std::string(fn) : resolvedFn,
        thisStr,
        std::string(arg),
    });

    if (resolvedFn.empty()) {
        return;
    }
    if (auto* con = resolveConsole()) {
        if (!con->isFunction(resolvedFn)) {
            return;
        }
        if (arg.empty()) {
            const std::array<std::string, 1> argv{thisStr};
            (void)con->execute(resolvedFn, argv);
        } else {
            const std::array<std::string, 2> argv{thisStr, std::string(arg)};
            (void)con->execute(resolvedFn, argv);
        }
    }
}

bool Trigger::setActive(bool b) {
    if (active_ != b) {
        active_ = b;
        scriptExec(active_ ? "onActivate" : "onDeactivate");
    }
    return true;
}

bool Trigger::initResources(GameBaseData* in_data) {
    if (!Parent::initResources(in_data)) {
        return false;
    }
    triggerData_ = dynamic_cast<TriggerData*>(in_data);
    return triggerData_ != nullptr;
}

bool Trigger::processArguments(std::span<const std::string> args) {
    if (args.empty()) {
        return false;
    }
    setDatFileName(args[0]);
    if (getManager() != nullptr && !loadDatFile()) {
        return false;
    }
    if (args.size() > 1) {
        Point3F dim{0.0f, 0.0f, 0.0f};
        for (auto i : std::views::iota(std::size_t{0}) | std::views::take_while([&](std::size_t i) {
                          return i < args.size() - 1 && i < 3;
                      })) {
            dim[i] = parseFloatArg(args[i + 1]) * 0.5f;
        }
        boundingBox_.fMax = dim;
        boundingBox_.fMin = -dim;
    }
    updateBoundingBox();
    return true;
}

bool Trigger::onAdd() {
    if (const auto posField = getFieldValue("position"); posField && !posField->empty()) {
        setLinearPosition(parsePoint3FField(*posField));
    }
    if (const auto rotField = getFieldValue("rotation"); rotField && !rotField->empty()) {
        setAngPosition(parsePoint3FField(*rotField));
    }
    if (const auto bbField = getFieldValue("boundingBox"); bbField && !bbField->empty()) {
        std::string_view text = *bbField;
        std::array<Flt32, 6> vals{-0.5f, -0.5f, -0.5f, 0.5f, 0.5f, 0.5f};
        for (Flt32& dst : vals) {
            while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) {
                text.remove_prefix(1);
            }
            if (text.empty()) {
                break;
            }
            const auto [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), dst);
            if (ec != std::errc{}) {
                break;
            }
            text.remove_prefix(static_cast<std::size_t>(ptr - text.data()));
        }
        boundingBox_.fMin.set(vals[0], vals[1], vals[2]);
        boundingBox_.fMax.set(vals[3], vals[4], vals[5]);
    }
    if (!Parent::onAdd()) {
        return false;
    }
    if (isGhost()) {
        addToSet(SimRenderSetId);
    } else {
        scriptExec(active_ ? "onActivate" : "onDeactivate");
    }
    if (auto* mgr = getManager()) {
        if (auto* root = dynamic_cast<sim::SimContainer*>(mgr->findObject(sim::RootContainerId))) {
            root->addObject(*this);
        }
    }
    updateBoundingBox();
    return true;
}

void Trigger::onRemove() {
    if (auto* cont = getContainer()) {
        cont->removeObject(*this);
    }
    Parent::onRemove();
}

void Trigger::onDeleteNotify(sim::SimObject& object) {
    for (auto i : std::views::iota(std::size_t{0}, objectList_.size())) {
        if (objectList_[i].object == &object) {
            std::string argStr{};
            if (const auto* gb = dynamic_cast<const GameBase*>(&object)) {
                argStr = gb->scriptThis();
            } else {
                argStr = std::format("{}", object.getId());
            }
            scriptExec("onLeave", argStr);
            objectList_[i] = objectList_.back();
            objectList_.pop_back();
            break;
        }
    }
    Parent::onDeleteNotify(object);
}

void Trigger::handleCollision(sim::SimObject* obj, SimTime now) {
    if (!active_ || !obj) {
        return;
    }
    const UInt32 mask = getCollisionMask();
    if ((obj->getType() & mask) == 0) {
        return;
    }
    const Flt32 rate = triggerData_ ? triggerData_->rate : RATE;
    std::string objScriptId{};
    if (const auto* gb = dynamic_cast<const GameBase*>(obj)) {
        objScriptId = gb->scriptThis();
    } else {
        objScriptId = std::format("{}", obj->getId());
    }

    for (auto& pair : objectList_) {
        if (pair.object == obj) {
            pair.expire = now + TIMEOUT;
            if (now >= pair.contact) {
                pair.contact = now + rate;
                scriptExec("onContact", objScriptId);
            }
            return;
        }
    }

    Pair entry{};
    entry.object = obj;
    entry.objectId = obj->getId();
    entry.expire = now + TIMEOUT;
    entry.contact = now + rate;
    objectList_.push_back(entry);
    deleteNotify(*obj);
    scriptExec("onEnter", objScriptId);
}

void Trigger::onCollisionNotify(GameBase* obj) {
    handleCollision(obj, currentSimTime());
}

void Trigger::expireContacts(SimTime now) {
    for (std::size_t i = 0; i < objectList_.size();) {
        if (now > objectList_[i].expire) {
            sim::SimObject* obj = objectList_[i].object;
            std::string objScriptId{};
            if (const auto* gb = dynamic_cast<const GameBase*>(obj)) {
                objScriptId = gb->scriptThis();
            } else if (obj) {
                objScriptId = std::format("{}", obj->getId());
            } else {
                objScriptId = std::format("{}", objectList_[i].objectId);
            }
            scriptExec("onLeave", objScriptId);
            if (obj) {
                clearNotify(*obj);
            }
            objectList_[i] = objectList_.back();
            objectList_.pop_back();
        } else {
            ++i;
        }
    }
}

void Trigger::serverProcess(UInt32 curTime) {
    Parent::serverProcess(curTime);
    const SimTime now = (explicitSimTime_ >= 0.0f)
                            ? explicitSimTime_
                            : (getManager() ? getManager()->getCurrentTime()
                                            : static_cast<SimTime>(curTime) * 0.001f);
    expireContacts(now);
}

bool Trigger::onSimTriggerEvent(const sim::SimTriggerEvent& event) {
    return setActive(event.activate);
}

bool Trigger::onSimEditEvent(const sim::SimEditEvent& event) {
    if (event.editor == sim::MissionEditorId) {
        inEditMode_ = event.state;
        if (inEditMode_) {
            setNetFlags(Ghostable);
        } else {
            setNetFlags(0);
        }
    }
    return true;
}

bool Trigger::onSimObjectTransformEvent(const sim::SimObjectTransformEvent& event) {
    setPosition(event.transform, true);
    EulerF euler{};
    event.transform.angles(euler);
    angPosition_.set(euler.x, euler.y, euler.z);
    updateBoundingBox();
    setMaskBits(PositionMask | RotationMask);
    return true;
}

bool Trigger::onSimMovementCollisionEvent(const sim::SimMovementCollisionEvent& event) {
    if (auto* mgr = getManager()) {
        if (auto* obj = event.getObject(*mgr)) {
            handleCollision(obj, currentSimTime());
            return true;
        }
    }
    return false;
}

bool Trigger::onSimObjectTransformQuery(sim::SimObjectTransformQuery& query) {
    query.transform = getTransform();
    return true;
}

bool Trigger::onSimCollisionImageQuery(sim::SimCollisionImageQuery& query) {
    // Dynamic fields are also used by mission scripts after object creation.
    if (const auto field = getFieldValue("isSphere")) {
        isSphere_ = !(*field == "false" || *field == "False" || *field == "0" || field->empty());
    }
    if (isSphere_) {
        sphereCollisionImage_->radius =
            std::min({boundingBox_.len_x(), boundingBox_.len_y(), boundingBox_.len_z()});
        sphereCollisionImage_->transform = getTransform();
        sphereCollisionImage_->trigger = query.detail != sim::SimCollisionImageQuery::Editor;
        query.images.push_back(sphereCollisionImage_);
    } else {
        collisionImage_->trigger = query.detail != sim::SimCollisionImageQuery::Editor;
        collisionImage_->box = boundingBox_;
        collisionImage_->transform = getTransform();
        query.images.push_back(collisionImage_);
    }
    return true;
}

bool Trigger::processEvent(const sim::SimEvent& event) {
    if (const auto* trigEv = dynamic_cast<const sim::SimTriggerEvent*>(&event)) {
        return onSimTriggerEvent(*trigEv);
    }
    if (const auto* editEv = dynamic_cast<const sim::SimEditEvent*>(&event)) {
        return onSimEditEvent(*editEv);
    }
    if (const auto* xformEv = dynamic_cast<const sim::SimObjectTransformEvent*>(&event)) {
        return onSimObjectTransformEvent(*xformEv);
    }
    if (const auto* colEv = dynamic_cast<const sim::SimMovementCollisionEvent*>(&event)) {
        return onSimMovementCollisionEvent(*colEv);
    }
    return Parent::processEvent(event);
}

bool Trigger::processQuery(sim::SimQuery& query) {
    if (auto* xq = dynamic_cast<sim::SimObjectTransformQuery*>(&query)) {
        return onSimObjectTransformQuery(*xq);
    }
    if (auto* cq = dynamic_cast<sim::SimCollisionImageQuery*>(&query)) {
        return onSimCollisionImageQuery(*cq);
    }
    return Parent::processQuery(query);
}

bool Trigger::inspectWrite(sim::Inspect& inspector) const {
    if (!Parent::inspectWrite(inspector)) {
        return false;
    }
    inspector.write(IDITG_POS, getLinearPosition());
    inspector.write(IDITG_ROTATION, getAngPosition());
    Point3F dim = boundingBox_.fMax;
    dim *= 2.0f;
    inspector.write(IDITG_DIM, dim);
    return true;
}

bool Trigger::inspectRead(sim::Inspect& inspector) {
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

    if (!inspector.read(sim::Inspect::Type::Point3F, IDITG_DIM, v)) {
        return false;
    }
    Point3F dim = std::get<Point3F>(v);
    dim *= 0.5f;
    boundingBox_.fMax = dim;
    boundingBox_.fMin = -dim;
    setLinearPosition(pos);
    setAngPosition(rot);
    updateBoundingBox();
    setMaskBits(PositionMask | RotationMask | SizedMask);
    return true;
}

UInt32
Trigger::packUpdate(sim::network::GhostManager& /*gm*/, UInt32 mask, BitStream& stream) const {
    if (stream.writeFlag((mask & PositionMask) != 0)) {
        const TMat3F& mat = getTransform();
        stream.writeBytes(std::as_bytes(std::span(&mat, 1)));
    }
    if (stream.writeFlag((mask & SizedMask) != 0)) {
        stream.writeBytes(std::as_bytes(std::span(&boundingBox_, 1)));
    }
    return 0;
}

bool Trigger::unpackUpdate(sim::network::GhostManager& /*gm*/, BitStream& stream) {
    const auto savedPos = stream.getCurPos();
    TMat3F newMat = getTransform();
    bool hasMat = false;
    if (stream.readFlag()) {
        if (!stream.readBytes(std::as_writable_bytes(std::span(&newMat, 1)))) {
            stream.setCurPos(savedPos);
            return false;
        }
        hasMat = true;
    }

    Box3F newBox = boundingBox_;
    bool hasBox = false;
    if (stream.readFlag()) {
        if (!stream.readBytes(std::as_writable_bytes(std::span(&newBox, 1)))) {
            stream.setCurPos(savedPos);
            return false;
        }
        hasBox = true;
    }

    if (!stream.isValid()) {
        stream.setCurPos(savedPos);
        return false;
    }

    if (hasBox) {
        boundingBox_ = newBox;
    }
    if (hasMat) {
        sim::SimObjectTransformEvent ev{};
        ev.transform = newMat;
        onSimObjectTransformEvent(ev);
    }
    if (hasBox && !hasMat) {
        updateBoundingBox();
    }
    return true;
}

Persistent::Error
Trigger::read(StreamIO& sio, Int32 version, Int32 user, Persistent::LoadContext& ctx) {
    if (Parent::read(sio, version, user, ctx) != Persistent::Error::Ok) {
        return Persistent::Error::ReadError;
    }
    return (sio.getStatus() == STRM_OK) ? Persistent::Error::Ok : Persistent::Error::ReadError;
}

Persistent::Error
Trigger::write(StreamIO& sio, Int32 version, Int32 user, Persistent::StoreContext& ctx) const {
    if (Parent::write(sio, version, user, ctx) != Persistent::Error::Ok) {
        return Persistent::Error::WriteError;
    }
    return (sio.getStatus() == STRM_OK) ? Persistent::Error::Ok : Persistent::Error::WriteError;
}

} // namespace opent1::game
