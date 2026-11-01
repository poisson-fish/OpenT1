#include <opent1/game/game_base.hpp>
#include <opent1/game/team_group.hpp>
#include <opent1/game/player_manager.hpp>
#include <opent1/game/sensor_manager.hpp>
#include <opent1/game/observer_camera.hpp>
#include <opent1/world/scene_objects.hpp>
#include <algorithm>
#include <charconv>
#include <cmath>
#include <numbers>

namespace opent1::game {

namespace {
constexpr Flt32 kCameraRadius = 0.025f;
thread_local std::vector<std::unique_ptr<GameBase::Timer>> g_timerPool{};

struct BitStreamRollbackGuard {
    BitStream& stream;
    Int32 savedBitPos;
    bool committed{false};

    explicit BitStreamRollbackGuard(BitStream& s) noexcept
        : stream(s), savedBitPos(s.getCurPos()) {}

    ~BitStreamRollbackGuard() noexcept {
        if (!committed) {
            stream.setCurPos(savedBitPos);
        }
    }
};
} // namespace

// -----------------------------------------------------------------------------
// FearRenderImage
// -----------------------------------------------------------------------------

void FearRenderImage::updateSunOverride(bool attenuate,
                                        Flt32 factor,
                                        const ColorF& color) noexcept {
    m_attenuateSun = attenuate;
    m_attenuationFactor = factor;
    m_attenuationColor = color;
}

void FearRenderImage::applySunAttenuation(ColorF& inOutSunDirectionalColor,
                                          ColorF& inOutAmbientIntensity) const noexcept {
    if (!m_attenuateSun) {
        return;
    }

    if (inOutSunDirectionalColor.red != 0.0f || inOutSunDirectionalColor.green != 0.0f ||
        inOutSunDirectionalColor.blue != 0.0f) {
        ColorF newColor{};
        newColor.interpolate(inOutSunDirectionalColor, m_attenuationColor, m_attenuationFactor);
        inOutSunDirectionalColor = newColor;
    }

    Flt32 attenIntensity = 0.3f * m_attenuationColor.red + 0.5f * m_attenuationColor.green +
                           0.2f * m_attenuationColor.blue;
    attenIntensity *= m_attenuationFactor;
    Flt32 newAtten = attenIntensity * 2.0f;
    if (newAtten > 1.0f) {
        newAtten = 1.0f;
    }
    inOutAmbientIntensity = inOutAmbientIntensity * newAtten;
}

std::array<Point3F, 8> FearRenderImage::computeBoundingBoxVertices() const noexcept {
    std::array<Point3F, 8> bboxPts{};
    bboxPts[0].set(m_containerBox.fMin.x, m_containerBox.fMin.y, m_containerBox.fMin.z);
    bboxPts[1].set(m_containerBox.fMin.x, m_containerBox.fMax.y, m_containerBox.fMin.z);
    bboxPts[2].set(m_containerBox.fMin.x, m_containerBox.fMax.y, m_containerBox.fMax.z);
    bboxPts[3].set(m_containerBox.fMin.x, m_containerBox.fMin.y, m_containerBox.fMax.z);
    bboxPts[4].set(m_containerBox.fMax.x, m_containerBox.fMin.y, m_containerBox.fMin.z);
    bboxPts[5].set(m_containerBox.fMax.x, m_containerBox.fMax.y, m_containerBox.fMin.z);
    bboxPts[6].set(m_containerBox.fMax.x, m_containerBox.fMax.y, m_containerBox.fMax.z);
    bboxPts[7].set(m_containerBox.fMax.x, m_containerBox.fMin.y, m_containerBox.fMax.z);
    return bboxPts;
}

// -----------------------------------------------------------------------------
// GameBase::Timer
// -----------------------------------------------------------------------------

GameBase::Timer::Timer() {
    entryList_.reserve(kReservedSize);
}

std::unique_ptr<GameBase::Timer> GameBase::Timer::acquire() {
    if (!g_timerPool.empty()) {
        auto timer = std::move(g_timerPool.back());
        g_timerPool.pop_back();
        timer->reset();
        return timer;
    }
    return std::make_unique<Timer>();
}

void GameBase::Timer::release(std::unique_ptr<Timer> timer) {
    if (timer) {
        timer->reset();
        g_timerPool.push_back(std::move(timer));
    }
}

void GameBase::Timer::clearPool() noexcept {
    g_timerPool.clear();
}

void GameBase::Timer::reset() noexcept {
    maxTime_ = 0.0f;
    entryList_.clear();
}

GameBase::Timer::Entry* GameBase::Timer::find(sim::SimObject* obj, SimTime time) {
    const sim::SimObjectId objId = obj ? obj->getId() : 0;
    for (auto& entry : entryList_) {
        if (entry.object == obj && entry.objectId == objId) {
            return &entry;
        }
        if (time > entry.collisionTime) {
            entry.object = obj;
            entry.objectId = objId;
            entry.collisionTime = 0.0f;
            return &entry;
        }
    }
    entryList_.push_back(Entry{obj, objId, 0.0f});
    return &entryList_.back();
}

bool GameBase::Timer::isCollisionExpired(sim::SimObject* obj, SimTime time) {
    Entry* entry = find(obj, time);
    if (time > entry->collisionTime) {
        entry->collisionTime = time + TimerCollisionTimeout;
        if (entry->collisionTime > maxTime_) {
            maxTime_ = entry->collisionTime;
        }
        return true;
    }
    return false;
}

bool GameBase::Timer::allExpired(SimTime time) const noexcept {
    return time > maxTime_;
}

// -----------------------------------------------------------------------------
// GameBase
// -----------------------------------------------------------------------------

GameBase::GameBase() {
    teamId_ = -1;
    ownerId_ = 0;
    ctrlClientId_ = 0;
    ownerClientId_ = 0;
    lastOwnerClientId_ = 0;
    powerCount_ = 0;
    powerGenerator_ = false;
    lastAnimateTime_ = 0;
    lastProcessTime_ = 0;
    datFileId_ = -1;
    sensorInfoBits_ = 0;
    data_ = nullptr;
    sensorKey = -1;
    fov_ = std::numbers::pi_v<Flt32> * 0.5f;
}

GameBase::~GameBase() {
    if (timer_) {
        Timer::release(std::move(timer_));
    }
}

DataBlockManager* GameBase::resolveDataBlockManager() const noexcept {
    if (boundDbm_) {
        return boundDbm_;
    }
    if (auto* mgr = getManager()) {
        return dynamic_cast<DataBlockManager*>(mgr->findObject(DataBlockManagerId));
    }
    return nullptr;
}

Console* GameBase::resolveConsole() const noexcept {
    if (boundConsole_) {
        return boundConsole_;
    }
    if (auto* mgr = getManager()) {
        return mgr->console();
    }
    return nullptr;
}

void GameBase::setDatFileName(std::string_view inName) {
    const std::string oldName = datFileName_;
    datFileName_.assign(inName);
    if (data_) {
        datFileId_ = -1;
        if (!loadDatFile()) {
            datFileName_ = oldName;
            loadDatFile();
        } else {
            setMaskBits(BaseStateMask);
        }
    }
}

bool GameBase::loadDatFile() {
    auto* dbm = resolveDataBlockManager();
    if (!dbm) {
        return false;
    }

    GameBaseData* res = nullptr;
    if (datFileId_ == -1) {
        if (datFileName_.empty()) {
            return false;
        }
        res = dbm->lookupDataBlock(datFileName_, getDatGroup(), &datFileId_);
    } else {
        res = dbm->lookupDataBlock(datFileId_, getDatGroup());
        if (res && datFileName_.empty()) {
            datFileName_ = res->dbmName;
        }
    }
    if (!res) {
        return false;
    }
    if (initResources(res)) {
        if (!isGhost()) {
            scriptOnInit();
        }
        return true;
    }
    return false;
}

bool GameBase::processArguments(std::span<const std::string> arguments) {
    std::vector<std::string_view> views;
    views.reserve(arguments.size());
    for (const auto& arg : arguments) {
        views.emplace_back(arg);
    }
    return processArgumentsView(views);
}

bool GameBase::processArgumentsView(std::span<const std::string_view> arguments) {
    if (arguments.empty()) {
        return false;
    }
    datFileName_.assign(arguments[0]);
    if (loadDatFile()) {
        if (!isGhost()) {
            scriptOnAdd();
        }
        return true;
    }
    return false;
}

bool GameBase::initResources(GameBaseData* ptr) {
    data_ = ptr;
    if (ptr) {
        retainedData_ = ptr->clone();
    } else {
        retainedData_.reset();
    }

    if (!isGhost()) {
        if (ptr && ptr->visibleToSensor) {
            setName(mapName_);
        }
    } else if (sensorKey != -1 && getManager()) {
        addToSet(SensorVisibleSetId);
    }
    return true;
}

void GameBase::packDatFile(BitStream& stream) const {
    if (const auto* dbm = resolveDataBlockManager()) {
        const Int32 bits = std::max(1, dbm->getDataGroupBitSize(getDatGroup()));
        stream.writeInt(datFileId_, bits);
    } else {
        stream.writeInt(datFileId_, 6);
    }
}

bool GameBase::unpackDatFile(BitStream& stream) {
    if (const auto* dbm = resolveDataBlockManager()) {
        const Int32 bits = std::max(1, dbm->getDataGroupBitSize(getDatGroup()));
        datFileId_ = stream.readInt(bits);
    } else {
        datFileId_ = stream.readInt(6);
    }
    return stream.isValid();
}

bool GameBase::onAdd() {
    if (!Parent::onAdd()) {
        return false;
    }

    const UInt32 curTime =
        getManager() ? static_cast<UInt32>(getManager()->getCurrentTime() * 1000.0f) : 0u;
    lastProcessTime_ = curTime;
    lastAnimateTime_ = curTime;
    updateTeamId();

    if (mapName_.empty()) {
        if (const auto nm = getFieldValue("name"); nm && !nm->empty()) {
            mapName_.assign(*nm);
        }
    }
    if (datFileName_.empty()) {
        if (const auto db = getFieldValue("dataBlock"); db && !db->empty()) {
            datFileName_.assign(*db);
        }
    }
    if (!datFileName_.empty() || datFileId_ != -1) {
        const bool ok = loadDatFile();
        if (auto* console = resolveConsole(); !ok && console) {
            const auto* blocks = resolveDataBlockManager();
            console->print(std::format(
                "Missing or invalid datablock '{}' (group {} index {}) for object '{}': {}",
                datFileName_,
                getDatGroup(),
                datFileId_,
                getName(),
                blocks ? blocks->lastDataBlockError() : "no datablock manager"));
        }
        if (ok && !isGhost()) {
            scriptOnAdd();
        }
        return ok;
    }
    return true;
}

void GameBase::onRemove() {
    if (!isGhost() && sensorKey != -1 && getManager()) {
        if (auto* sensors =
                dynamic_cast<SensorManager*>(getManager()->findObject(SensorManagerId))) {
            sensors->removeTarget(sensorKey);
        }
    }
    if (data_ && !isGhost()) {
        scriptOnRemove();
    }
    data_ = nullptr;
    setControlClient(0);
    setOwnerClient(0);
    Parent::onRemove();
}

void GameBase::onGroupAdd() {
    if (!isGhost()) {
        updatePowerCount();
        if (isGenerator()) {
            updatePowerClients(getGroup(), true);
        }
    }
    updateTeamId();
}

void GameBase::onGroupRemove() {
    if (!isGhost()) {
        if (isGenerator()) {
            updatePowerClients(getGroup(), false);
        }
    }
}

void GameBase::onCollisionNotify(GameBase* /*object*/) {}

bool GameBase::processEvent(const sim::SimEvent& event) {
    if (const auto* moveCollEv = dynamic_cast<const sim::SimMovementCollisionEvent*>(&event)) {
        if (auto* mgr = getManager()) {
            if (auto* obj = dynamic_cast<GameBase*>(moveCollEv->object.resolve(*mgr))) {
                onCollisionNotify(obj);
            }
        }
        return true;
    }
    return Parent::processEvent(event);
}

bool GameBase::processQuery(sim::SimQuery& query) {
    return Parent::processQuery(query);
}

std::string_view GameBase::getScriptDefaultClass() const noexcept {
    return "GameBase";
}

void GameBase::updateTimers(SimTime explicitCurrentTime) {
    if (!timer_) {
        return;
    }
    const SimTime now = explicitCurrentTime >= 0.0f
                            ? explicitCurrentTime
                            : (getManager() ? getManager()->getCurrentTime() : 0.0f);
    if (timer_->allExpired(now)) {
        Timer::release(std::move(timer_));
    }
}

void GameBase::getThrowVector(Point3F* pos, Point3F* vec) const {
    if (pos) {
        *pos = getBoxCenter();
    }
    if (vec) {
        const Point3F vv{0.0f, 8.0f, 0.0f};
        m_mul(vv, static_cast<const RMat3F&>(getTransform()), *vec);
    }
}

void GameBase::throwObject(GameBase* /*object*/, Flt32 /*speed*/, bool /*careless*/) {}

bool GameBase::getMuzzleTransform(Int32 /*slot*/, TMat3F* mat) const {
    if (mat) {
        *mat = getTransform();
    }
    return true;
}

void GameBase::getObjectMountTransform(Int32 /*mountPoint*/, TMat3F* mat) const {
    if (mat) {
        *mat = getTransform();
    }
}

void GameBase::getCameraTransform(Flt32 /*pos*/, TMat3F* mat) const {
    if (mat) {
        *mat = getTransform();
    }
}

void GameBase::onPowerStateChange(GameBase* /*generator*/) {}

void GameBase::setTeam(Int32 team) {
    const Int32 old = teamId_;
    teamId_ = team;
    if (!isGhost() && teamId_ != old) {
        setMaskBits(BaseStateMask);
        if (auto* con = resolveConsole()) {
            const std::string cb = scriptName("onSetTeam");
            if (!cb.empty()) {
                const std::string oldStr = std::to_string(old);
                const std::string thisStr = scriptThis();
                std::array<std::string, 2> callArgs{thisStr, oldStr};
                (void)con->execute(cb, callArgs);
            }
        }
        // 1.11 keeps the owner's roster team in sync with its body. AI target
        // selection and mod damage scripts consult this representation too.
        if (auto* players = PlayerManager::get(getManager()); ownerClientId_ && players) {
            if (auto* owner = players->findBaseRep(ownerClientId_); owner && owner->team != teamId_) {
                players->serverClientTeamChange(ownerClientId_, teamId_);
            }
        }
    }
}

void GameBase::updateTeamId() {
    if (isGhost()) {
        return;
    }
    auto* parent = getGroup();
    while (parent) {
        if (parent->getName() == "MissionGroup") {
            setTeam(-1);
            return;
        }
        if (const auto* tg = dynamic_cast<const TeamGroup*>(parent)) {
            setTeam(tg->getTeam());
            return;
        }
        const std::string_view groupName = parent->getName();
        if (groupName.starts_with("TeamGroup")) {
            Int32 parsedTeam = -1;
            const auto suffix = groupName.substr(9);
            if (!suffix.empty()) {
                const auto [ptr, ec] =
                    std::from_chars(suffix.data(), suffix.data() + suffix.size(), parsedTeam);
                if (ec == std::errc{}) {
                    setTeam(parsedTeam);
                    return;
                }
            }
        }
        parent = parent->getGroup();
    }
}

void GameBase::updatePowerCount() {
    if (powerGenerator_) {
        powerCount_ = 1;
        onPowerStateChange(this);
    } else {
        powerCount_ = 0;
    }

    auto* group = getGroup();
    while (group) {
        for (auto* member : group->objects()) {
            if (auto* obj = dynamic_cast<GameBase*>(member)) {
                if (obj != this && obj->isGenerator()) {
                    ++powerCount_;
                    onPowerStateChange(obj);
                }
            }
        }
        group = group->getGroup();
    }
}

void GameBase::updatePowerClients(sim::SimGroup* group, bool power) {
    if (!group) {
        return;
    }
    for (auto* member : group->objects()) {
        if (auto* obj = dynamic_cast<GameBase*>(member)) {
            if (power) {
                ++obj->powerCount_;
                obj->onPowerStateChange(this);
            } else if (obj->powerCount_ > 0) {
                --obj->powerCount_;
                obj->onPowerStateChange(this);
            }
        } else if (auto* subGroup = dynamic_cast<sim::SimGroup*>(member)) {
            updatePowerClients(subGroup, power);
        }
    }
}

void GameBase::generatePower(bool power) {
    if (!isGhost() && powerGenerator_ != power) {
        powerGenerator_ = power;
        if (getGroup()) {
            updatePowerClients(getGroup(), power);
        } else {
            powerCount_ = power ? 1 : 0;
            onPowerStateChange(this);
        }
    }
}

void GameBase::forcePowerState(bool on) noexcept {
    if (isGhost()) {
        powerCount_ = on ? 1 : 0;
    }
}

std::string GameBase::scriptThis() const {
    return std::to_string(getId());
}

std::string GameBase::scriptName(std::string_view callback) const {
    auto* con = resolveConsole();
    if (!con) {
        return {};
    }

    // 1. Based off object name (mapName_ or SimObject::getName())
    const std::string_view objName =
        !mapName_.empty() ? std::string_view(mapName_) : SimObject::getName();
    if (!objName.empty()) {
        std::string candidate = std::format("{}::{}", objName, callback);
        if (con->isFunction(candidate)) {
            return candidate;
        }
    }

    // 2. Based off the dat file name
    const auto* datPtr = getDatPtr();
    const std::string_view datName = (datPtr && !datPtr->dbmName.empty())
                                         ? std::string_view(datPtr->dbmName)
                                         : std::string_view(datFileName_);
    if (!datName.empty()) {
        std::string candidate = std::format("{}::{}", datName, callback);
        if (con->isFunction(candidate)) {
            return candidate;
        }
    }

    // 3. ClassName specified in the dat file
    if (datPtr && !datPtr->className.empty()) {
        std::string candidate = std::format("{}::{}", datPtr->className, callback);
        if (con->isFunction(candidate)) {
            return candidate;
        }
    }

    // 4. Default C++ class name
    const std::string_view defClass = getScriptDefaultClass();
    if (!defClass.empty()) {
        std::string candidate = std::format("{}::{}", defClass, callback);
        if (con->isFunction(candidate)) {
            return candidate;
        }
    }

    // 5. Global callback
    if (con->isFunction(callback)) {
        return std::string(callback);
    }

    return {};
}

void GameBase::scriptOnAdd() {
    auto* con = resolveConsole();
    if (!con) {
        return;
    }
    const std::string cb = scriptName("onAdd");
    if (!cb.empty()) {
        std::array<std::string, 1> argv{scriptThis()};
        (void)con->execute(cb, argv);
    }
}

void GameBase::scriptOnInit() {
    auto* con = resolveConsole();
    if (!con) {
        return;
    }
    const std::string cb = scriptName("onInit");
    if (!cb.empty()) {
        std::array<std::string, 1> argv{scriptThis()};
        (void)con->execute(cb, argv);
    }
}

void GameBase::scriptOnRemove() {
    auto* con = resolveConsole();
    if (!con) {
        return;
    }
    const std::string cb = scriptName("onRemove");
    if (!cb.empty()) {
        std::array<std::string, 1> argv{scriptThis()};
        (void)con->execute(cb, argv);
    }
}

void GameBase::scriptOnCollision(GameBase* obj, SimTime explicitCurrentTime) {
    if (!obj) {
        return;
    }
    if (!timer_) {
        timer_ = Timer::acquire();
    }
    const SimTime now = explicitCurrentTime >= 0.0f
                            ? explicitCurrentTime
                            : (getManager() ? getManager()->getCurrentTime() : 0.0f);
    if (timer_->isCollisionExpired(obj, now)) {
        if (auto* con = resolveConsole()) {
            const std::string cb = scriptName("onCollision");
            if (!cb.empty()) {
                std::array<std::string, 2> argv{scriptThis(), obj->scriptThis()};
                (void)con->execute(cb, argv);
            }
        }
    }
}

Flt32 GameBase::getUpdatePriority(const sim::SimNetObject::CameraInfo& camInfo,
                                  UInt32 /*updateMask*/,
                                  UInt32 updateSkips) const {
    if (camInfo.camera.objectId != 0 && camInfo.camera.objectId == getId()) {
        return 10.0f;
    }

    Point3F pos = getBoxCenter();
    pos -= camInfo.position;
    Flt32 dist = pos.len();
    if (dist == 0.0f) {
        dist = 0.001f;
    }
    pos *= 1.0f / dist;

    const Flt32 wDistance = (dist < camInfo.visibleDistance && camInfo.visibleDistance > 0.0f)
                                ? 1.0f - (dist / camInfo.visibleDistance)
                                : 0.0f;

    const Flt32 dot = m_dot(pos, camInfo.orientation);
    const bool inFov = dot > camInfo.cosFov;
    const Flt32 wFov = inFov ? 1.0f : 0.0f;

    Flt32 wVelocity = 0.0f;
    if (inFov && camInfo.fov != 0.0f && camInfo.visibleDistance > 0.0f) {
        Point3F vec{};
        m_cross(camInfo.orientation, getLinearVelocity(), vec);
        wVelocity = (vec.lenf() * camInfo.fov) / (camInfo.fov * camInfo.visibleDistance);
        if (wVelocity > 1.0f) {
            wVelocity = 1.0f;
        }
    }

    Flt32 wInterest = 0.0f;
    if ((getType() & SimPlayerObjectType) != 0) {
        wInterest = 0.75f;
    } else if ((getType() & SimProjectileObjectType) != 0) {
        wInterest = 0.30f;
        const Flt32 projDot = -m_dot(pos, getLinearVelocity());
        if (projDot > 0.0f) {
            wInterest += 0.20f * projDot;
        }
    } else if ((getType() & ItemObjectType) != 0) {
        wInterest = 0.25f;
    }

    const Flt32 wSkips = static_cast<Flt32>(updateSkips) * 0.5f;
    const Flt32 wOwnership =
        (camInfo.camera.objectId != 0 && ownerId_ == camInfo.camera.objectId) ? 1.0f : 0.0f;

    return wFov * upFov + wDistance * upDistance + wVelocity * upVelocity + wSkips * upSkips +
           wOwnership * upOwnership + wInterest * upInterest;
}

void GameBase::setOwnerClient(Int32 clientId) {
    if (clientId != 0) {
        lastOwnerClientId_ = clientId;
    }
    if (clientId == ownerClientId_) {
        return;
    }
    if (auto* players = PlayerManager::get(getManager())) {
        if (auto* old = players->findClient(ownerClientId_); old && old->ownedObject == this) {
            old->ownedObject = nullptr;
        }
        if (auto* next = players->findClient(clientId)) {
            next->ownedObject = this;
        }
    }
    ownerClientId_ = clientId;
    setMaskBits(BaseStateMask);
}

void GameBase::setControlClient(Int32 clientId) {
    if (clientId == ctrlClientId_) {
        return;
    }
    if (auto* players = PlayerManager::get(getManager())) {
        if (auto* old = players->findClient(ctrlClientId_); old && old->controlObject == this) {
            old->controlObject = nullptr;
        }
        if (auto* next = players->findClient(clientId)) {
            next->controlObject = this;
        }
    }
    ctrlClientId_ = clientId;
    setMaskBits(BaseStateMask);
}

bool GameBase::packGameBaseUpdate(UInt32 mask, BitStream& stream) const {
    if (stream.writeFlag((mask & BaseStateMask) != 0)) {
        stream.writeInt(teamId_, TeamIdBits);
        if (stream.writeFlag(ctrlClientId_ != 0)) {
            stream.writeInt(ctrlClientId_ - 2048, 7);
        }
        if (stream.writeFlag(ownerClientId_ != 0)) {
            stream.writeInt(ownerClientId_ - 2048, 7);
        }
    }
    return stream.isValid();
}

bool GameBase::unpackGameBaseUpdate(BitStream& stream) {
    BitStreamRollbackGuard streamGuard(stream);
    const Int32 savedTeam = teamId_;
    const Int32 savedCtrl = ctrlClientId_;
    const Int32 savedOwner = ownerClientId_;
    const Int32 savedLastOwner = lastOwnerClientId_;

    if (stream.readFlag()) {
        const Int32 newTeam = stream.readInt(TeamIdBits);
        Int32 newCtrlClient = 0;
        Int32 newOwnerClient = 0;
        if (stream.readFlag()) {
            newCtrlClient = stream.readInt(7) + 2048;
        }
        if (stream.readFlag()) {
            newOwnerClient = stream.readInt(7) + 2048;
        }
        if (!stream.isValid()) {
            teamId_ = savedTeam;
            ctrlClientId_ = savedCtrl;
            ownerClientId_ = savedOwner;
            lastOwnerClientId_ = savedLastOwner;
            return false;
        }
        setControlClient(newCtrlClient);
        setOwnerClient(newOwnerClient);
        setTeam(newTeam);
    }

    if (!stream.isValid()) {
        teamId_ = savedTeam;
        ctrlClientId_ = savedCtrl;
        ownerClientId_ = savedOwner;
        lastOwnerClientId_ = savedLastOwner;
        return false;
    }

    streamGuard.committed = true;
    return true;
}

UInt32
GameBase::packUpdate(sim::network::GhostManager& /*gm*/, UInt32 mask, BitStream& stream) const {
    packGameBaseUpdate(mask, stream);
    return 0;
}

bool GameBase::unpackUpdate(sim::network::GhostManager& /*gm*/, BitStream& stream) {
    return unpackGameBaseUpdate(stream);
}

void GameBase::setName(std::string_view name, Console* explicitConsole) {
    auto* con = explicitConsole ? explicitConsole : resolveConsole();
    std::string expanded;
    expanded.reserve(255);

    std::size_t idx = 0;
    while (idx < name.size() && expanded.size() < 255) {
        if (idx + 1 < name.size() && name[idx] == '<' && name[idx + 1] == 'v') {
            const auto closePos = name.find('>', idx + 2);
            if (closePos != std::string_view::npos) {
                const std::string_view varName = name.substr(idx + 2, closePos - (idx + 2));
                const std::string varVal = con ? con->getVariable(varName) : std::string{};
                if (expanded.size() + varVal.size() > 254) {
                    break;
                }
                expanded.append(varVal);
                idx = closePos + 1;
                continue;
            }
        }
        expanded.push_back(name[idx]);
        ++idx;
    }
    mapName_ = std::move(expanded);
    if (!isGhost() && data_ && data_->visibleToSensor && getManager()) {
        if (auto* sensors =
                dynamic_cast<SensorManager*>(getManager()->findObject(SensorManagerId))) {
            sensorKey = sensors->addTarget(this, mapName_);
        }
    }
}

Persistent::Error GameBase::readLegacy(StreamIO& sio, Int32 version, Int32 user) {
    Persistent::ClassRegistry emptyReg;
    Persistent::LoadContext ctx(emptyReg);
    return read(sio, version, user, ctx);
}

Persistent::Error GameBase::writeLegacy(StreamIO& sio, Int32 version, Int32 user) const {
    Persistent::ClassRegistry emptyReg;
    Persistent::StoreContext ctx(emptyReg);
    return write(sio, version, user, ctx);
}

Persistent::Error
GameBase::read(StreamIO& sio, Int32 /*version*/, Int32 /*user*/, Persistent::LoadContext& ctx) {
    const Int32 startPos = sio.getPosition();
    const auto savedId = getId();
    const auto savedNetFlags = netFlags();
    const auto savedLocked = isLocked();
    const auto savedTeam = teamId_;
    const auto savedDatFileName = datFileName_;

    const auto rollback = [&]() noexcept {
        setId(savedId);
        setNetFlags(savedNetFlags);
        setLocked(savedLocked);
        teamId_ = savedTeam;
        datFileName_ = savedDatFileName;
        sio.setPosition(startPos);
        return Persistent::Error::ReadError;
    };

    Int32 fileVersion = 0;
    if (!sio.read(fileVersion)) {
        return rollback();
    }
    if (Parent::read(sio, 0, 0, ctx) != Persistent::Error::Ok) {
        return rollback();
    }
    if (!sio.read(teamId_)) {
        return rollback();
    }
    std::string scriptData;
    std::string loadedDatFile;
    if (!sio.readString(scriptData) || !sio.readString(loadedDatFile)) {
        return rollback();
    }
    if (!loadedDatFile.empty()) {
        datFileName_ = std::move(loadedDatFile);
    }
    return sio.getStatus() == STRM_OK ? Persistent::Error::Ok : rollback();
}

Persistent::Error GameBase::write(StreamIO& sio,
                                  Int32 /*version*/,
                                  Int32 /*user*/,
                                  Persistent::StoreContext& ctx) const {
    const Int32 startPos = sio.getPosition();
    const Int32 fileVersion = 0;
    if (!sio.write(fileVersion) || Parent::write(sio, 0, 0, ctx) != Persistent::Error::Ok ||
        !sio.write(teamId_) || !sio.writeString("") || !sio.writeString(datFileName_)) {
        sio.setPosition(startPos);
        return Persistent::Error::WriteError;
    }
    return sio.getStatus() == STRM_OK ? Persistent::Error::Ok : Persistent::Error::WriteError;
}

bool GameBase::getCommandStatus(CommandStatus* status) const {
    if (status) {
        status->active = false;
        status->damageLevel = 0;
    }
    return false;
}

void GameBase::setSensorPinged(UInt8 fPinged) noexcept {
    sensorInfoBits_ = (sensorInfoBits_ & ((1u << SensorPingedBase) - 1u)) |
                      (static_cast<UInt32>(fPinged) << SensorPingedBase);
}

UInt8 GameBase::getSensorPinged() const noexcept {
    return static_cast<UInt8>(sensorInfoBits_ >> SensorPingedBase);
}

void GameBase::setAlwaysVisibleToTeam(Int32 teamId, bool visible) noexcept {
    if (teamId < 0 || teamId >= 8) {
        return;
    }
    if (visible) {
        sensorInfoBits_ |= (1u << (teamId + AlwaysVisBase));
    } else {
        sensorInfoBits_ &= ~(1u << (teamId + AlwaysVisBase));
    }
}

bool GameBase::getAlwaysVisibleToTeam(Int32 teamId) const noexcept {
    if (teamId < 0 || teamId >= 8) {
        return false;
    }
    return (sensorInfoBits_ & (1u << (teamId + AlwaysVisBase))) != 0;
}

void GameBase::setNeverVisibleToTeam(Int32 teamId, bool visible) noexcept {
    if (teamId < 0 || teamId >= 8) {
        return;
    }
    if (visible) {
        sensorInfoBits_ |= (1u << (teamId + NeverVisBase));
    } else {
        sensorInfoBits_ &= ~(1u << (teamId + NeverVisBase));
    }
}

bool GameBase::getNeverVisibleToTeam(Int32 teamId) const noexcept {
    if (teamId < 0 || teamId >= 8) {
        return false;
    }
    return (sensorInfoBits_ & (1u << (teamId + NeverVisBase))) != 0;
}

void GameBase::setVisibleToTeam(Int32 teamId, bool visible) noexcept {
    if (teamId < 0 || teamId >= 8) {
        return;
    }
    if (visible) {
        sensorInfoBits_ |=
            (1u << (teamId + TeamVisBase)) & ~(sensorInfoBits_ >> (NeverVisBase - TeamVisBase));
    } else {
        sensorInfoBits_ &= ~(1u << (teamId + TeamVisBase));
    }
}

bool GameBase::getVisibleToTeam(Int32 teamId) const noexcept {
    if (teamId < 0 || teamId >= 8) {
        return false;
    }
    return (sensorInfoBits_ &
            ((1u << (teamId + AlwaysVisBase)) | (1u << (teamId + TeamVisBase)))) != 0;
}

Flt32 GameBase::getHaze(const Point3F& cameraPos) const {
    if (!getContainer()) {
        return 1.0f;
    }
    if (const auto* manager = getManager()) {
        // Terrain/place haze is delegated through containing interiors as in
        // SimContainer::getHazeValue; missionless containers have no haze.
        if (const auto* terrain = dynamic_cast<const world::SimMissionTerrain*>(
                manager->findObject(sim::TerrainId))) {
            return terrain->policy().haze(cameraPos, getLinearPosition());
        }
    }
    return 0.0f;
}

Flt32 GameBase::validateEyePoint(TMat3F* trans, Flt32 camDist) const {
    if (!trans) {
        return camDist;
    }
    sim::SimContainerQuery cq{};
    cq.id = getId();
    cq.type = AllObjectTypes;
    cq.mask = SimTerrainObjectType | SimInteriorObjectType | VehicleObjectType |
              MoveableObjectType | StaticObjectType;
    cq.box.fMin = trans->p;

    Point3F dir{};
    trans->getRow(1, dir);

    cq.box.fMax = dir * (-camDist * 2.5f) + trans->p;

    sim::SimCollisionInfo info{};
    auto* mgr = getManager();
    sim::SimContainer* root =
        mgr ? dynamic_cast<sim::SimContainer*>(mgr->findObject(sim::RootContainerId)) : nullptr;

    if (!root || !root->findLOS(cq, info) || info.surfaces.surfaces.empty()) {
        trans->p -= dir * camDist;
        return camDist;
    }

    const auto& surfaceList = info.surfaces;
    Point3F endpos{};
    Point3F normal{};
    m_mul(surfaceList.surfaces[0].position, surfaceList.tWorld, endpos);
    m_mul(surfaceList.surfaces[0].normal, static_cast<const RMat3F&>(surfaceList.tWorld), normal);

    const Flt32 dot = m_dot(dir, normal);
    if (dot >= 0.01f) {
        Flt32 colDist = m_dot(trans->p - endpos, dir) - (1.0f / dot) * kCameraRadius;
        if (colDist > camDist) {
            colDist = camDist;
        }
        if (colDist < 0.0f) {
            colDist = 0.0f;
        }
        trans->p -= dir * colDist;
        camDist = colDist;
    } else {
        trans->p -= dir * camDist;
    }
    return camDist;
}

void GameBase::buildScopeAndCameraInfo(sim::network::GhostManager& cr,
                                       sim::SimNetObject::CameraInfo& camInfo) {
    const TMat3F eyeTransform = getEyeTransform();
    camInfo.visibleDistance =
        getManager() ? world::sceneVisibility(*getManager()).visibleDistance : 1500.0f;
    eyeTransform.getRow(1, camInfo.orientation);
    camInfo.camera = address();
    camInfo.position = eyeTransform.p;
    camInfo.fov = fov_ * 0.5f;
    camInfo.sinFov = std::sin(fov_ * 0.5f);
    camInfo.cosFov = std::cos(fov_ * 0.5f);

    cr.objectInScope(*this);

    if (auto* mgr = getManager()) {
        if (auto* observers = dynamic_cast<sim::SimSet*>(mgr->findObject(ObserverSetId))) {
            for (auto* object : observers->objects()) {
                if (auto* observer = dynamic_cast<ObserverCamera*>(object);
                    observer && observer->clientId() == ctrlClientId_) {
                    cr.objectInScope(*observer);
                }
            }
        }
        if (auto* root = dynamic_cast<sim::SimContainer*>(mgr->findObject(sim::RootContainerId))) {
            sim::SimContainerQuery query{};
            query.id = getId();
            query.type = AllObjectTypes;
            query.mask = AllObjectTypes;
            const Point3F ext{
                camInfo.visibleDistance, camInfo.visibleDistance, camInfo.visibleDistance};
            query.box = Box3F{camInfo.position - ext, camInfo.position + ext};
            const auto candidates = root->scope(query);
            const auto selected = world::scopedSceneObjects(
                *mgr, camInfo.position, camInfo.visibleDistance, candidates);
            if (!selected) {
                return;
            }
            for (const auto& addr : *selected) {
                if (auto* netObj = dynamic_cast<sim::SimNetObject*>(addr.resolve(*mgr))) {
                    if ((netObj->netFlags() & sim::SimNetObject::Ghostable) != 0 &&
                        !netObj->isGhost()) {
                        cr.objectInScope(*netObj);
                    }
                }
            }
        }
    }
}

} // namespace opent1::game
