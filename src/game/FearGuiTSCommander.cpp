#include <opent1/game/commander_huds.hpp>
#include <ranges>
#include <opent1/game/fear_player_psc.hpp>
#include <opent1/game/chat_menu.hpp>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <format>
#include <numbers>
#include <cstdio>
#include <cstdlib>
#include <tuple>

namespace opent1::game {

namespace {

inline constexpr Flt32 kTwoPi = 6.28318530717958647692f;
inline constexpr Int32 kIdItgPosition = 1100338;
inline constexpr Int32 kIdItgExtentDim = 1010004;

struct WhereAmIEntry {
    UInt8 endByte;
    Int32 bmpIndex;
};

constexpr std::array<WhereAmIEntry, 13> kWhereAmIBmpTable{{
    {11, 0},
    {32, 1},
    {53, 2},
    {75, 3},
    {96, 4},
    {117, 5},
    {139, 6},
    {160, 7},
    {181, 6},
    {203, 5},
    {224, 4},
    {245, 3},
    {255, 0},
}};

[[nodiscard]] Flt32 parseFloatValue(std::string_view sv) noexcept {
    Flt32 out = 0.0f;
    std::from_chars(sv.data(), sv.data() + sv.size(), out);
    return out;
}

} // namespace

MissionCenterPos::MissionCenterPos() {
    setNetFlags(Ghostable | ScopeAlways);
    setId(MissionCenterPosId);
    pos = {1230.0f, 1230.0f};
    extent = {1400.0f, 1400.0f};
}

void MissionCenterPos::initPersistFields() {}

bool MissionCenterPos::onAdd() {
    if (const auto xVal = getFieldValue("x"); xVal && !xVal->empty()) {
        pos.x = parseFloatValue(*xVal);
    }
    if (const auto yVal = getFieldValue("y"); yVal && !yVal->empty()) {
        pos.y = parseFloatValue(*yVal);
    }
    if (const auto wVal = getFieldValue("w"); wVal && !wVal->empty()) {
        extent.x = parseFloatValue(*wVal);
    }
    if (const auto hVal = getFieldValue("h"); hVal && !hVal->empty()) {
        extent.y = parseFloatValue(*hVal);
    }
    if (!Parent::onAdd()) {
        return false;
    }
    return true;
}

bool MissionCenterPos::processArguments(std::span<const std::string> args) {
    if (args.empty()) {
        return true;
    }
    if (args.size() == 4) {
        pos.x = parseFloatValue(args[0]);
        pos.y = parseFloatValue(args[1]);
        extent.x = parseFloatValue(args[2]);
        extent.y = parseFloatValue(args[3]);
        return true;
    }
    return false;
}

bool MissionCenterPos::inspectWrite(sim::Inspect& inspector) const {
    if (!Parent::inspectWrite(inspector)) {
        return false;
    }
    inspector.write(kIdItgPosition, pos);
    inspector.write(kIdItgExtentDim, extent);
    return true;
}

bool MissionCenterPos::inspectRead(sim::Inspect& inspector) {
    if (!Parent::inspectRead(inspector)) {
        return false;
    }
    sim::Inspect::Value v{};
    if (!inspector.read(sim::Inspect::Type::Point2F, kIdItgPosition, v)) {
        return false;
    }
    const Point2F p = std::get<Point2F>(v);
    if (!inspector.read(sim::Inspect::Type::Point2F, kIdItgExtentDim, v)) {
        return false;
    }
    const Point2F e = std::get<Point2F>(v);
    pos = p;
    extent = e;
    setMaskBits(1);
    return true;
}

UInt32 MissionCenterPos::packUpdate(sim::network::GhostManager&, UInt32, BitStream& stream) const {
    stream.writeBytes(std::as_bytes(std::span(&pos.x, 1)));
    stream.writeBytes(std::as_bytes(std::span(&pos.y, 1)));
    stream.writeBytes(std::as_bytes(std::span(&extent.x, 1)));
    stream.writeBytes(std::as_bytes(std::span(&extent.y, 1)));
    return 0;
}

bool MissionCenterPos::unpackUpdate(sim::network::GhostManager&, BitStream& stream) {
    const auto savedPos = stream.getCurPos();
    Point2F p{};
    Point2F e{};
    if (!stream.readBytes(std::as_writable_bytes(std::span(&p.x, 1))) ||
        !stream.readBytes(std::as_writable_bytes(std::span(&p.y, 1))) ||
        !stream.readBytes(std::as_writable_bytes(std::span(&e.x, 1))) ||
        !stream.readBytes(std::as_writable_bytes(std::span(&e.y, 1))) || !stream.isValid() ||
        !std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(e.x) || !std::isfinite(e.y)) {
        stream.setCurPos(savedPos);
        return false;
    }
    pos = p;
    extent = e;
    return true;
}

Persistent::Error
MissionCenterPos::read(StreamIO& sio, Int32 version, Int32 user, Persistent::LoadContext& ctx) {
    Point2F p{};
    Point2F e{};
    if (!sio.read(p.x) || !sio.read(p.y) || !sio.read(e.x) || !sio.read(e.y)) {
        return Persistent::Error::ReadError;
    }
    if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(e.x) || !std::isfinite(e.y)) {
        return Persistent::Error::ReadError;
    }
    const auto err = Parent::read(sio, version, user, ctx);
    if (err != Persistent::Error::Ok) {
        return err;
    }
    pos = p;
    extent = e;
    return Persistent::Error::Ok;
}

Persistent::Error MissionCenterPos::write(StreamIO& sio,
                                          Int32 version,
                                          Int32 user,
                                          Persistent::StoreContext& ctx) const {
    if (!std::isfinite(pos.x) || !std::isfinite(pos.y) || !std::isfinite(extent.x) ||
        !std::isfinite(extent.y)) {
        return Persistent::Error::WriteError;
    }
    if (!sio.write(pos.x) || !sio.write(pos.y) || !sio.write(extent.x) || !sio.write(extent.y)) {
        return Persistent::Error::WriteError;
    }
    return Parent::write(sio, version, user, ctx);
}

TSCommander::TSCommander() {
    fields_.extent = {512, 384};
}

std::function<void()> TSCommander::checkpoint() {
    auto base = Parent::checkpoint();
    const auto state = std::tuple{targets_,
                                  mode_,
                                  mapFilter_,
                                  curZoom_,
                                  zoomMode_,
                                  zoomTransitionTimeMs_,
                                  selectedTargKey_,
                                  selectedPlayerId_,
                                  map_,
                                  mapRevision_,
                                  missionExtent_,
                                  mapCenter_,
                                  mouseStart_,
                                  mousePoint_,
                                  dragging_,
                                  scrolling_,
                                  additiveSelection_,
                                  clickedTarget_,
                                  starredCommand_,
                                  currentCommand_,
                                  commandText_,
                                  currentScale_,
                                  startScale_,
                                  lastTick_,
                                  zoomStarted_,
                                  localTeam,
                                  localClient,
                                  selectType,
                                  showNames,
                                  showSensorRange,
                                  translucentSensors,
                                  centerMode,
                                  facing};
    return [this, base = std::move(base), state] {
        base();
        std::tie(targets_,
                 mode_,
                 mapFilter_,
                 curZoom_,
                 zoomMode_,
                 zoomTransitionTimeMs_,
                 selectedTargKey_,
                 selectedPlayerId_,
                 map_,
                 mapRevision_,
                 missionExtent_,
                 mapCenter_,
                 mouseStart_,
                 mousePoint_,
                 dragging_,
                 scrolling_,
                 additiveSelection_,
                 clickedTarget_,
                 starredCommand_,
                 currentCommand_,
                 commandText_,
                 currentScale_,
                 startScale_,
                 lastTick_,
                 zoomStarted_,
                 localTeam,
                 localClient,
                 selectType,
                 showNames,
                 showSensorRange,
                 translucentSensors,
                 centerMode,
                 facing) = state;
    };
}

Point2I TSCommander::normalizedToScreen(Point2I normPos, Point2I viewExtent) noexcept {
    const Int32 sx = (normPos.x * viewExtent.x) >> 10;
    const Int32 sy = viewExtent.y - ((normPos.y * viewExtent.y) >> 10);
    return {sx, sy};
}

Point2I TSCommander::worldToNormalized(Point2F worldPos,
                                       Point2F missionCenterPos,
                                       Point2F missionCenterExt) noexcept {
    const Flt32 extX = std::max(1.0f, missionCenterExt.x);
    const Flt32 extY = std::max(1.0f, missionCenterExt.y);
    const Flt32 nx = ((worldPos.x - missionCenterPos.x) / extX) * 1024.0f;
    const Flt32 ny = ((worldPos.y - missionCenterPos.y) / extY) * 1024.0f;
    return {std::clamp<Int32>(static_cast<Int32>(nx), 0, 1023),
            std::clamp<Int32>(static_cast<Int32>(ny), 0, 1023)};
}

Int32 TSCommander::whereAmIBitmapSector(Flt32 yawRadians) noexcept {
    const Int32 raw = static_cast<Int32>(256.0f - (yawRadians * 256.0f / kTwoPi));
    const UInt8 byteAngle = static_cast<UInt8>(raw & 0xFF);
    for (const auto& entry : kWhereAmIBmpTable) {
        if (byteAngle <= entry.endByte) {
            return entry.bmpIndex;
        }
    }
    return 0;
}

void TSCommander::setZoomMode(bool zoomIn, UInt32 transitionTimeMs) noexcept {
    zoomMode_ = zoomIn;
    zoomTransitionTimeMs_ = std::clamp<UInt32>(transitionTimeMs, 1U, 2000U);
}

void TSCommander::stepZoom(bool zoomIn) noexcept {
    startScale_ = currentScale_;
    zoomStarted_ = lastTick_;
    if (zoomIn) {
        curZoom_ = std::min<Int32>(4, curZoom_ + 1);
    } else {
        curZoom_ = std::max<Int32>(0, curZoom_ - 1);
    }
}

Point2F TSCommander::effectiveWorldExtent(Point2F baseMissionExt) const noexcept {
    const Int32 shift = std::clamp<Int32>(curZoom_, 0, 4);
    const Flt32 scale = 1.0f / static_cast<Flt32>(1 << shift);
    return {baseMissionExt.x * scale, baseMissionExt.y * scale};
}

void TSCommander::addTarget(SensorTargetInfo target) {
    if (targets_.size() >= static_cast<std::size_t>(MaxNumTargs)) {
        return;
    }
    targets_.push_back(std::move(target));
}

void TSCommander::clearTargets() noexcept {
    targets_.clear();
    selectedTargKey_ = -1;
    selectedPlayerId_ = -1;
}

bool TSCommander::selectTargetByKey(Int32 keyIndex) {
    if (keyIndex < 0 || static_cast<std::size_t>(keyIndex) >= targets_.size() ||
        targets_[std::size_t(keyIndex)].datId < 0 ||
        targets_[std::size_t(keyIndex)].team != localTeam) {
        return false;
    }
    clearSelection();
    setSelected(std::size_t(keyIndex), true);
    return true;
}

bool TSCommander::selectTargetsInBox(RectI screenBox) {
    bool any = false;
    selectedTargKey_ = -1;
    selectedPlayerId_ = -1;
    for (auto i : std::views::iota(std::size_t{0}, targets_.size())) {
        auto& t = targets_[i];
        const Point2I sp = screenPosition(t.pos);
        const bool inBox = (sp.x >= screenBox.left && sp.x <= screenBox.right &&
                            sp.y >= screenBox.top && sp.y <= screenBox.bottom && t.datId >= 0 &&
                            t.team == localTeam && t.datGroup == selectType && targetVisible(t));
        setSelected(i, inBox || (additiveSelection_ && t.selected));
        if (inBox && !any) {
            any = true;
            selectedTargKey_ = static_cast<Int32>(i);
            selectedPlayerId_ = t.playerId;
        }
    }
    return any;
}

std::string TSCommander::buildIssueCommandScript(Int32 commandId,
                                                 std::string_view commandText,
                                                 Point2I normWayPoint) const {
    std::string quoted;
    for (const char c : commandText) {
        if (c == '\\' || c == '\"') {
            quoted += '\\';
        }
        if (c == '\n') {
            quoted += "\\n";
        } else if (c == '\r') {
            quoted += "\\r";
        } else {
            quoted += c;
        }
    }
    auto result = std::format("remoteEval(2048, IssueCommand, {}, \"{}\", {}, {}",
                              commandId,
                              quoted,
                              normWayPoint.x,
                              normWayPoint.y);
    for (const auto& target : targets_) {
        if (target.selected && target.playerId >= 0 && target.team == localTeam) {
            result += ", " + std::to_string(target.playerId);
        }
    }
    return result + ");";
}

void TSCommander::bindResources(std::shared_ptr<ResourceManager> resources) {
    resources_ = std::move(resources);
    if (!resources_) {
        return;
    }
    if (auto font = resources_->load<gfx::BitmapFont>("if_small.pft")) {
        textFont_ = std::make_shared<gfx::BitmapFont>(*font);
    }
    const auto array = [&](std::string_view name) -> std::shared_ptr<const gfx::BitmapArray> {
        const auto loaded = resources_->load<gfx::BitmapArray>(name);
        if (!loaded) {
            return {};
        }
        auto copy = std::make_shared<gfx::BitmapArray>(*loaded);
        for (auto& bitmap : copy->bitmaps) {
            bitmap.attributes |= gfx::BitmapTransparent;
        }
        return copy;
    };
    orderIcons_ = array("CmdIconsCtrl.pba");
    facingIcons_ = array("CmdWhereAmI.pba");
}

void TSCommander::refreshTargets(const PlayerPSC& psc,
                                 const PlayerManager& players,
                                 const DataBlockManager& data,
                                 const SensorManager* sensors,
                                 Int32 clientId) {
    localClient = clientId;
    if (const auto* me = players.findClient(clientId)) {
        localTeam = me->team;
    }
    const auto old = std::move(targets_);
    targets_.resize(MaxNumTargs);
    for (auto i : std::views::iota(std::size_t{0}, targets_.size())) {
        auto& target = targets_[i];
        const auto& source = psc.commandTargets()[i];
        target.datId = source.dataId;
        target.datGroup = source.dataGroup;
        target.pos = source.position;
        target.team = source.team;
        target.damageStatus = source.damage;
        target.activeStatus = source.status != 0;
        if (target.datId < 0) {
            continue;
        }
        if (i < 128) {
            target.playerId = 2048 + Int32(i);
            if (const auto* player = players.findBaseRep(target.playerId)) {
                target.name = player->name;
                target.selected = player->selected;
                target.command = player->curCommand;
                target.commandStatus = player->commandStatus;
                target.commandTarget = player->curCommandTarget;
                target.waypoint = player->wayPoint;
            }
        } else {
            if (sensors) {
                target.name = sensors->target(Int32(i) - 128).name;
            }
            if (i < old.size() && old[i].datId == target.datId &&
                old[i].datGroup == target.datGroup) {
                target.selected = old[i].selected;
            }
        }
        if (const auto* block = data.lookupDataBlock(target.datId, target.datGroup)) {
            target.filter = block->mapFilter;
            if (target.name.empty()) {
                target.name = block->description;
            }
            if (const auto* sensor = dynamic_cast<const SensorData*>(block)) {
                target.sensorRange = Flt32(sensor->iRange);
            }
            if (!block->mapIcon.empty() && resources_) {
                const auto icon = resources_->load<gfx::Bitmap>(
                    block->mapIcon + (target.team == localTeam ? "_green.bmp" : "_red.bmp"));
                if (icon) {
                    auto copy = std::make_shared<gfx::Bitmap>(*icon);
                    copy->attributes |= gfx::BitmapTransparent;
                    target.icon = std::move(copy);
                }
            }
        }
    }
}

bool TSCommander::rebuildMap(sim::SimWorld& model,
                             const gfx::Palette& palette,
                             Point2F origin,
                             Point2F extent) {
    if (!std::isfinite(extent.x) || !std::isfinite(extent.y) || extent.x <= 0 || extent.y <= 0) {
        return false;
    }
    auto output = gfx::SoftwareSurface::create(1024, 1024);
    auto tile = gfx::SoftwareSurface::create(128, 128);
    if (!output || !tile || !tile->setPalette(palette)) {
        if (std::getenv("OPENT1_SCENE_DIAGNOSTICS")) {
            std::fprintf(stderr, "Command map surface/palette rejected\n");
        }
        return false;
    }
    sim::EnvironmentCamera view;
    TMat3F cameraToWorld(EulerF{-std::numbers::pi_v<Flt32> * .5f, 0, 0},
                         {origin.x + extent.x * .5f, origin.y + extent.y * .5f, 500});
    cameraToWorld.flags |= TMat3F::Matrix_HasRotation | TMat3F::Matrix_HasTranslation;
    view.worldToCamera = cameraToWorld;
    view.worldToCamera.inverse();
    view.perspective = false;
    view.viewport = {0, 0, 127, 127};
    view.nearDistance = 1;
    view.farDistance = 1000;
    view.focal = {1024 / extent.x, 1024 / extent.y};
    const sim::VolumetricView volume{cameraToWorld.p, 1000, std::nextafter(1000.f, 0.f)};
    for (auto y : std::views::iota(Int32{0}, Int32{8})) {
        for (auto x : std::views::iota(Int32{0}, Int32{8})) {
            view.center = {512.f - Flt32(x * 128), 512.f - Flt32(y * 128)};
            tile->clear(0);
            const auto frame = world::WorldSceneFrame::captureContainers(model, view, volume, true);
            if (!frame) {
                if (std::getenv("OPENT1_SCENE_DIAGNOSTICS")) {
                    std::fprintf(stderr,
                                 "Command map capture rejected tile %d %d determinant %g\n",
                                 x,
                                 y,
                                 double(view.worldToCamera.determinant()));
                }
                return false;
            }
            if (!frame->draw(*tile) || !output->blit(tile->bitmap(), {x * 128, y * 128})) {
                if (std::getenv("OPENT1_SCENE_DIAGNOSTICS")) {
                    std::fprintf(stderr, "Command map drawing rejected tile %d %d\n", x, y);
                }
                return false;
            }
        }
    }
    map_ = std::make_shared<gfx::Bitmap>(output->bitmap());
    missionExtent_ = extent;
    ++mapRevision_;
    curZoom_ = 0;
    currentScale_ = startScale_ = 1;
    zoomMode_ = centerMode = false;
    mapCenter_ = {512, 512};
    return true;
}

bool TSCommander::targetVisible(const SensorTargetInfo& target) const {
    return target.datId >= 0 && ((mapFilter_ & MapFilterAll) || target.filter == -1 ||
                                 (mapFilter_ & UInt32(target.filter)));
}

Point2I TSCommander::screenPosition(Point2I normalized) const {
    return {Int32((Flt32(normalized.x) - mapCenter_.x) * Flt32(fields_.extent.x) * currentScale_ /
                      1024 +
                  Flt32(fields_.extent.x) * .5f),
            Int32((mapCenter_.y - Flt32(normalized.y)) * Flt32(fields_.extent.y) * currentScale_ /
                      1024 +
                  Flt32(fields_.extent.y) * .5f)};
}

Point2I TSCommander::normalizedPosition(Point2I screen) const {
    return {std::clamp(Int32(mapCenter_.x + (Flt32(screen.x) - Flt32(fields_.extent.x) * .5f) *
                                                1023 / (Flt32(fields_.extent.x) * currentScale_)),
                       0,
                       1023),
            std::clamp(Int32(mapCenter_.y - (Flt32(screen.y) - Flt32(fields_.extent.y) * .5f) *
                                                1023 / (Flt32(fields_.extent.y) * currentScale_)),
                       0,
                       1023)};
}

void TSCommander::setSelected(std::size_t index, bool value) {
    auto& target = targets_[index];
    if (target.selected == value) {
        return;
    }
    target.selected = value;
    if (value) {
        selectedTargKey_ = Int32(index);
        selectedPlayerId_ = target.playerId;
    } else if (selectedTargKey_ == Int32(index)) {
        selectedTargKey_ = selectedPlayerId_ = -1;
        for (auto i : std::views::iota(std::size_t{0}, targets_.size())) {
            if (targets_[i].selected) {
                selectedTargKey_ = Int32(i);
                selectedPlayerId_ = targets_[i].playerId;
                break;
            }
        }
    }
    if (target.playerId >= 0 && playerSelectionChanged) {
        playerSelectionChanged(target.playerId, value);
    }
}

void TSCommander::clearSelection() {
    for (auto i : std::views::iota(std::size_t{0}, targets_.size())) {
        setSelected(i, false);
    }
    selectedTargKey_ = selectedPlayerId_ = -1;
}

bool TSCommander::selectPlayer(Int32 id, bool value, bool additive) {
    if (!additive) {
        clearSelection();
    }
    for (auto i : std::views::iota(std::size_t{0}, targets_.size())) {
        const auto& target = targets_[i];
        if (target.playerId == id && target.datId >= 0 && target.team == localTeam) {
            selectType = DataBlockManager::PlayerDataType;
            setSelected(i, value);
            if (value && executeCommand) {
                const std::array args{std::to_string(id)};
                executeCommand("Client::cmdObservePlayer", args);
            }
            return true;
        }
    }
    return false;
}

bool TSCommander::selectObject(Int32 index, bool additive) {
    if (index < 0 || std::size_t(index) >= targets_.size()) {
        return false;
    }
    const auto& target = targets_[std::size_t(index)];
    if (target.datId < 0 || target.team != localTeam) {
        return false;
    }
    if (mode_ == WaypointMode) {
        return issueWaypoint(target.pos, index);
    }
    selectType = target.datGroup == DataBlockManager::SensorDataType
                     ? DataBlockManager::StaticShapeDataType
                     : target.datGroup;
    const bool value = !additive || !target.selected;
    clearSelection();
    setSelected(std::size_t(index), value);
    return true;
}

void TSCommander::issueCommand(Int32 command, std::string_view text) {
    currentCommand_ = command;
    commandText_ = text;
    if (starredCommand_) {
        return;
    }
    if (commandDisplayChanged) {
        commandDisplayChanged(text.substr(0, text.find('~')));
    }
    mode_ = SelectingMode;
    if (command != -1) {
        for (const auto& target : targets_) {
            if (target.selected && target.playerId >= 0 && target.team == localTeam) {
                mode_ = WaypointMode;
                break;
            }
        }
    }
}

bool TSCommander::issueWaypoint(Point2I position, Int32 index) {
    if (currentCommand_ < 0 || !executeCommand || index >= Int32(targets_.size())) {
        return false;
    }
    if (index >= 0 && targets_[std::size_t(index)].datId < 0) {
        return false;
    }
    std::string type = "waypoint", name;
    if (index >= 0) {
        const auto& target = targets_[std::size_t(index)];
        name = target.name;
        const char* kind = nullptr;
        switch (target.datGroup) {
            case DataBlockManager::PlayerDataType:
                kind = "player";
                break;
            case DataBlockManager::StaticShapeDataType:
                kind = "static";
                break;
            case DataBlockManager::TurretDataType:
                kind = "turret";
                break;
            case DataBlockManager::SensorDataType:
                kind = "sensor";
                break;
            case DataBlockManager::FlierDataType:
                kind = "vehicle";
                break;
            default:
                position = target.pos;
                index = -1;
                break;
        }
        if (kind) {
            type = (target.team == localTeam ? "friendly " : "enemy ") + std::string(kind);
        }
    }
    if (setVariable) {
        setVariable("CommandTarget", type);
        setVariable("CommandTargetName", name);
    }
    const auto savedCommand = currentCommand_;
    const auto savedText = commandText_;
    if (commandText_.starts_with('*')) {
        starredCommand_ = true;
        const std::array args{commandText_};
        executeCommand("Commander::StarCommand", args);
        starredCommand_ = false;
    }
    std::vector<std::string> args{"2048",
                                  index < 0 ? "IssueCommand" : "IssueTargCommand",
                                  std::to_string(currentCommand_),
                                  commandText_};
    if (index < 0) {
        args.push_back(std::to_string(position.x));
        args.push_back(std::to_string(position.y));
    } else {
        args.push_back(std::to_string(index));
    }
    std::size_t recipients = 0;
    for (const auto& target : targets_) {
        if (target.selected && target.playerId >= 0 && target.team == localTeam) {
            args.push_back(std::to_string(target.playerId));
            ++recipients;
        }
    }
    if (recipients) {
        executeCommand("remoteEval", args);
    }
    currentCommand_ = savedCommand;
    commandText_ = savedText;
    return recipients > 0;
}

void TSCommander::tick(double seconds) {
    lastTick_ = seconds;
    const auto progress = std::clamp(
        Flt32((seconds - zoomStarted_) * 1000 / double(zoomTransitionTimeMs_)), 0.f, 1.f);
    currentScale_ = startScale_ + (Flt32(1 << curZoom_) - startScale_) * progress;
    if (centerMode) {
        Point2F sum{};
        Int32 count = 0;
        for (const auto& target : targets_) {
            if (target.selected) {
                sum.x += Flt32(target.pos.x);
                sum.y += Flt32(target.pos.y);
                ++count;
            }
        }
        if (count) {
            mapCenter_ = {sum.x / Flt32(count), sum.y / Flt32(count)};
        }
    }
}

bool TSCommander::event(const gui::Event& e) {
    const auto point = globalToLocal(e.point);
    if (e.type == gui::EventType::Down) {
        mouseStart_ = mousePoint_ = point;
        centerMode = false;
        if (e.button == 2 || e.button == 3) {
            if (zoomMode_) {
                stepZoom(false);
                return true;
            }
            scrolling_ = true;
        } else if (zoomMode_) {
            const auto center = normalizedPosition(point);
            mapCenter_ = {Flt32(center.x), Flt32(center.y)};
            stepZoom(true);
            return true;
        } else if (mode_ == WaypointMode) {
            Int32 nearest = -1;
            Flt32 distance = 4;
            for (auto i : std::views::iota(std::size_t{0}, targets_.size())) {
                if (targets_[i].datId < 0) {
                    continue;
                }
                const auto delta = screenPosition(targets_[i].pos) - point;
                const auto length = std::hypot(Flt32(delta.x), Flt32(delta.y));
                if (length < distance) {
                    distance = length;
                    nearest = Int32(i);
                }
            }
            return issueWaypoint(normalizedPosition(point), nearest);
        } else {
            additiveSelection_ = e.shift;
            clickedTarget_ = false;
            for (auto i : std::views::iota(std::size_t{0}, targets_.size())) {
                const auto& target = targets_[i];
                const auto delta = screenPosition(target.pos) - point;
                if (target.team != localTeam || !targetVisible(target) ||
                    (target.datGroup != selectType &&
                     !(selectType == DataBlockManager::StaticShapeDataType &&
                       target.datGroup == DataBlockManager::SensorDataType)) ||
                    std::abs(delta.x) > 3 || std::abs(delta.y) > 3) {
                    continue;
                }
                if (target.playerId >= 0) {
                    (void)selectPlayer(target.playerId, !e.shift || !target.selected, e.shift);
                } else {
                    (void)selectObject(Int32(i), e.shift);
                }
                clickedTarget_ = true;
                break;
            }
            dragging_ = true;
            if (commandDisplayChanged) {
                commandDisplayChanged("");
            }
        }
        if (canvas()) {
            (void)canvas()->lockMouse(this);
        }
        return true;
    }
    if (e.type == gui::EventType::Drag || e.type == gui::EventType::Move) {
        if (scrolling_) {
            mapCenter_.x -=
                Flt32(point.x - mousePoint_.x) * 1024 / (Flt32(fields_.extent.x) * currentScale_);
            mapCenter_.y +=
                Flt32(point.y - mousePoint_.y) * 1024 / (Flt32(fields_.extent.y) * currentScale_);
        }
        mousePoint_ = point;
        return scrolling_ || dragging_;
    }
    if (e.type == gui::EventType::Up || e.type == gui::EventType::Cancel) {
        if (dragging_ && e.type != gui::EventType::Cancel &&
            (!clickedTarget_ || std::abs(point.x - mouseStart_.x) > 3 ||
             std::abs(point.y - mouseStart_.y) > 3)) {
            (void)selectTargetsInBox({std::min(point.x, mouseStart_.x),
                                      std::min(point.y, mouseStart_.y),
                                      std::max(point.x, mouseStart_.x),
                                      std::max(point.y, mouseStart_.y)});
        }
        const bool handled = dragging_ || scrolling_;
        dragging_ = scrolling_ = false;
        if (canvas() && canvas()->capture() == this) {
            canvas()->unlockMouse();
        }
        return handled;
    }
    return gui::Control::event(e);
}

bool TSCommander::draw(gfx::SoftwareSurface& surface, Point2I offset, RectI clip) const {
    const auto before = surface.clip();

    struct Restore {
        gfx::SoftwareSurface& surface;
        RectI clip;

        ~Restore() {
            surface.setClip(clip);
        }
    } restore{surface, before};

    clip = {std::max({clip.left, before.left, offset.x}),
            std::max({clip.top, before.top, offset.y}),
            std::min({clip.right, before.right, offset.x + fields_.extent.x - 1}),
            std::min({clip.bottom, before.bottom, offset.y + fields_.extent.y - 1})};
    if (clip.left > clip.right || clip.top > clip.bottom) {
        return true;
    }
    surface.setClip(clip);
    surface.fill(clip, 0);
    if (map_) {
        const auto top = screenPosition({0, 1024}) + offset,
                   bottom = screenPosition({1024, 0}) + offset;
        if (!surface.stretch(*map_, {top.x, top.y, bottom.x - 1, bottom.y - 1})) {
            return false;
        }
    }
    const auto bitmap = [&](const gfx::Bitmap& image,
                            Point2I center,
                            gfx::Flip flip = gfx::Flip::None) {
        return surface.blit(image, {center.x - image.width / 2, center.y - image.height / 2}, flip);
    };
    if (showSensorRange) {
        for (const auto& target : targets_) {
            if (!targetVisible(target) || target.team != localTeam || !target.activeStatus ||
                target.sensorRange <= 0) {
                continue;
            }
            const auto center = screenPosition(target.pos) + offset;
            const Point2F radius{
                target.sensorRange * Flt32(fields_.extent.x) * currentScale_ / missionExtent_.x,
                target.sensorRange * Flt32(fields_.extent.y) * currentScale_ / missionExtent_.y};
            Point2I previous{center.x + Int32(radius.x), center.y};
            for (auto i : std::views::iota(1) | std::views::take_while([&](int i) {
                              return i <= 64;
                          })) {
                const auto angle = Flt32(i) * std::numbers::pi_v<Flt32> / 32;
                const Point2I next{center.x + Int32(radius.x * std::cos(angle)),
                                   center.y + Int32(radius.y * std::sin(angle))};
                surface.line(previous, next, 245);
                previous = next;
            }
        }
    }
    // Native draws items before players, names after icons, and the local
    // player's directional marker last so dense groups remain selectable.
    for (auto pass : std::views::iota(0, 5)) {
        for (const auto& target : targets_) {
            if (!targetVisible(target)) {
                continue;
            }
            const bool player = target.playerId >= 0, self = target.playerId == localClient;
            if ((pass < 2 && player) || (pass >= 2 && !player) || (pass == 4 && !self) ||
                (pass == 2 && self)) {
                continue;
            }
            const auto position = screenPosition(target.pos) + offset;
            if (pass == 0 || pass == 2 || pass == 4) {
                if (self && pass == 4 && facingIcons_) {
                    constexpr std::array<Int32, 12> ends{
                        12, 32, 52, 76, 96, 116, 140, 160, 180, 204, 224, 244};
                    constexpr std::array<Int32, 12> indices{0, 1, 2, 3, 2, 1, 0, 1, 2, 3, 2, 1};
                    constexpr std::array<gfx::Flip, 12> flips{gfx::Flip::None,
                                                              gfx::Flip::None,
                                                              gfx::Flip::None,
                                                              gfx::Flip::None,
                                                              gfx::Flip::Y,
                                                              gfx::Flip::Y,
                                                              gfx::Flip::Y,
                                                              gfx::Flip::Both,
                                                              gfx::Flip::Both,
                                                              gfx::Flip::X,
                                                              gfx::Flip::X,
                                                              gfx::Flip::X};
                    const auto angle = Int32(256 - facing * 256 / kTwoPi) & 255;
                    std::size_t sector = 0;
                    for (auto i : std::views::iota(std::size_t{0}, ends.size())) {
                        if (angle <= ends[i]) {
                            sector = i;
                            break;
                        }
                    }
                    const auto index = std::size_t(indices[sector] + (target.selected ? 4 : 0));
                    if (index < facingIcons_->bitmaps.size() &&
                        !bitmap(facingIcons_->bitmaps[index], position, flips[sector])) {
                        return false;
                    }
                } else if (target.icon && !bitmap(*target.icon, position)) {
                    return false;
                }
                if (target.selected && !self) {
                    surface.rectangle(
                        {position.x - 6, position.y - 6, position.x + 6, position.y + 6}, 255);
                }
                if (player && target.command >= 0) {
                    auto waypoint = target.waypoint;
                    if (target.commandTarget >= 0 &&
                        std::size_t(target.commandTarget) < targets_.size() &&
                        targets_[std::size_t(target.commandTarget)].datId >= 0) {
                        waypoint = targets_[std::size_t(target.commandTarget)].pos;
                    }
                    const auto destination = screenPosition(waypoint) + offset;
                    surface.line(position,
                                 destination,
                                 target.commandStatus == PlayerManager::CMD_RECEIVED ? 244 : 255);
                    if (orderIcons_ && target.command > 0 &&
                        std::size_t(target.command) <= orderIcons_->bitmaps.size() &&
                        !bitmap(orderIcons_->bitmaps[std::size_t(target.command - 1)],
                                destination)) {
                        return false;
                    }
                }
            }
            if ((pass == 1 || pass == 3 || pass == 4) && textFont_ &&
                (player || showNames || target.selected ||
                 (std::abs(position.x - offset.x - mousePoint_.x) <= 4 &&
                  std::abs(position.y - offset.y - mousePoint_.y) <= 4))) {
                if (!surface.text(*textFont_, position + Point2I{4, -4}, target.name)) {
                    return false;
                }
            }
        }
    }
    if (dragging_) {
        surface.rectangle({offset.x + std::min(mouseStart_.x, mousePoint_.x),
                           offset.y + std::min(mouseStart_.y, mousePoint_.y),
                           offset.x + std::max(mouseStart_.x, mousePoint_.x),
                           offset.y + std::max(mouseStart_.y, mousePoint_.y)},
                          255);
    }
    return drawChildren(surface, offset, clip);
}

bool TSCommander::onSimActionEvent(const sim::SimActionEvent& event) {
    switch (event.action) {
        case 220207: // IDACTION_ZOOM_IN
            stepZoom(true);
            return true;
        case 220208: // IDACTION_ZOOM_OUT
            stepZoom(false);
            return true;
        case 220210: // IDACTION_ZOOM_MODE_ON
            setZoomMode(true);
            return true;
        case 220211: // IDACTION_ZOOM_MODE_OFF
            setZoomMode(false);
            return true;
        case 220219:
            centerMode = true;
            return true;
        case 220220:
            centerMode = false;
            return true;
        default:
            return false;
    }
}

bool TSCommander::processEvent(const sim::SimEvent& event) {
    if (event.type == sim::ActionEventType || event.type == SimActionEventType) {
        if (const auto* act = dynamic_cast<const sim::SimActionEvent*>(&event)) {
            if (onSimActionEvent(*act)) {
                return true;
            }
        }
    }
    return Parent::processEvent(event);
}

} // namespace opent1::game
