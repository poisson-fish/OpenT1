#include <opent1/game/player_manager.hpp>
#include <opent1/math/random.hpp>
#include <opent1/world/scene_objects.hpp>
#include <opent1/game/sound.hpp>
#include <opent1/game/ai.hpp>
#include <opent1/game/chat_inventory_huds.hpp>
#include <opent1/sim/sim_resource_volume.hpp>
#include <opent1/console/eval.hpp>
#include <algorithm>
#include <cctype>

namespace opent1::game {

namespace {

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

[[nodiscard]] bool equalsIgnoreCase(std::string_view a, std::string_view b) noexcept {
    if (a.size() != b.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) !=
            std::tolower(static_cast<unsigned char>(b[i]))) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] std::string truncateTo(std::string_view text, std::size_t maxLen) {
    const std::size_t len = std::min(text.size(), maxLen);
    return std::string(text.substr(0, len));
}

constexpr std::array<std::string_view, 21> kBadWords = {
    "fuck",         "shit",         "cunt",     "cock",   "asshole", "fag",    "prick",
    "bitch",        "pussy",        "bastard",  "faggot", "slut",    "whore",  "cocksucker",
    "motherfucker", "mutherfucker", "cuntlick", "fuk",    "azzhole", "biotch", "phuck",
};

constexpr std::string_view kFilterString = "#@&*$@!*";

[[nodiscard]] char getBadChar() noexcept {
    static Random s_random;
    const UInt32 index = s_random.getInt(static_cast<UInt32>(kFilterString.size() - 1));
    return kFilterString[index];
}

} // namespace

// -----------------------------------------------------------------------------
// BaseRep / ClientRep / TeamRep
// -----------------------------------------------------------------------------

void PlayerManager::BaseRep::resetBase() noexcept {
    id = 0;
    name.clear();
    team = 0;
    selected = false;
    expanded = true;
    score = 0;
    scoreString.clear();
    scoreSortValue = 0;
    skinBase.clear();
    nextClient = nullptr;
    prevClient = nullptr;
    loginStatus = 0;
}

void PlayerManager::ClientRep::clean() noexcept {
    resetBase();
    playerSequence = 0;
    ping = 0;
    packetLoss = 0.0f;
    voiceBase.clear();
    curGuiMode = InitialGuiMode;
    controlObject = nullptr;
    ownedObject = nullptr;
    playerPSC = nullptr;
    ghostManager = nullptr;
    eventManager = nullptr;
    packetStream = nullptr;
    commanderId = 0;
    commander = nullptr;
    firstPeon = nullptr;
    nextPeon = nullptr;
    curCommand = -1;
    curCommandSequence = lastCommandSequence = lastObjectiveSequence = 0;
    lastCommandMessage.reset();
    curCommandTarget = -1;
    wayPoint.set(0, 0);
    curCommandString.clear();
    commandStatus = None;
    muted = false;
    sendScores = 0;
    gender.clear();
    curRadioChannel = 0;
    listenChannels = 0;
    positionUpdateTime = 0;
    position.set(0, 0);
    curCommander = -1;
    for (auto& df : dataFinished) {
        df.clear();
    }
}

void PlayerManager::TeamRep::clean() noexcept {
    resetBase();
    energy = 0.0f;
    firstCommander = nullptr;
    for (auto& obj : objectives) {
        obj.seqNum = 0;
        obj.text.clear();
    }
}

// -----------------------------------------------------------------------------
// PlayerManager Network Events
// -----------------------------------------------------------------------------

void PingPLEvent::pack(BitStream& stream) const {
    const auto numCl = static_cast<Int32>(std::min<std::size_t>(entries.size(), 127));
    stream.writeInt(numCl, 7);
    for (Int32 i = 0; i < numCl; ++i) {
        const Entry& entry = entries[static_cast<std::size_t>(i)];
        stream.writeInt(entry.clientId - PlayerManager::BaseClientId, 7);
        stream.writeInt(entry.ping >> 2, 8);
        stream.writeFloat(entry.packetLoss, 6);
    }
}

bool PingPLEvent::unpack(BitStream& stream) {
    BitStreamRollbackGuard guard(stream);
    const Int32 count = stream.readInt(7);
    if (!stream.isValid()) {
        return false;
    }
    std::vector<Entry> parsed{};
    parsed.reserve(static_cast<std::size_t>(count));
    for (Int32 i = 0; i < count; ++i) {
        const Int32 cid = stream.readInt(7) + PlayerManager::BaseClientId;
        const Int32 p = stream.readInt(8) << 2;
        const Flt32 loss = stream.readFloat(6);
        if (!stream.isValid()) {
            return false;
        }
        parsed.push_back(Entry{cid, p, loss});
    }
    entries = std::move(parsed);
    guard.committed = true;
    return true;
}

void DeltaScoreEvent::pack(BitStream& stream) const {
    if (stream.writeFlag(isTeam)) {
        stream.writeInt(id + 1, 4);
    } else {
        stream.writeInt(id - PlayerManager::BaseClientId, 7);
    }
    stream.writeInt(sortValue, 32);
    stream.writeString(score);
}

bool DeltaScoreEvent::unpack(BitStream& stream) {
    BitStreamRollbackGuard guard(stream);
    const bool teamFlag = stream.readFlag();
    Int32 parsedId = 0;
    if (teamFlag) {
        parsedId = stream.readInt(4) - 1;
    } else {
        parsedId = stream.readInt(7) + PlayerManager::BaseClientId;
    }
    const Int32 parsedSort = stream.readInt(32);
    std::string parsedScore{};
    if (!stream.readString(parsedScore) || !stream.isValid()) {
        return false;
    }
    isTeam = teamFlag;
    id = parsedId;
    sortValue = parsedSort;
    score = std::move(parsedScore);
    guard.committed = true;
    return true;
}

PlayerSkinEvent::PlayerSkinEvent(Int32 cid, std::string_view skin)
    : clientId(cid), skinBase(truncateTo(skin, 255)) {
    type = PlayerSkinEventType;
}

void PlayerSkinEvent::pack(BitStream& stream) const {
    stream.writeInt(clientId - PlayerManager::BaseClientId, 7);
    stream.writeString(skinBase);
}

bool PlayerSkinEvent::unpack(BitStream& stream) {
    BitStreamRollbackGuard guard(stream);
    const Int32 cid = stream.readInt(7) + PlayerManager::BaseClientId;
    std::string skin{};
    if (!stream.readString(skin) || !stream.isValid()) {
        return false;
    }
    clientId = cid;
    skinBase = std::move(skin);
    guard.committed = true;
    return true;
}

void PlayerSayEvent::pack(BitStream& stream) const {
    stream.writeInt(sender, 12);
    stream.writeInt(static_cast<Int32>(msgType), 5);
    stream.writeString(message);
}

bool PlayerSayEvent::unpack(BitStream& stream) {
    BitStreamRollbackGuard guard(stream);
    const Int32 parsedSender = stream.readInt(12);
    const auto parsedType = static_cast<UInt32>(stream.readInt(5));
    std::string msg{};
    if (!stream.readString(msg) || !stream.isValid()) {
        return false;
    }
    sender = parsedSender;
    msgType = parsedType;
    message = std::move(msg);
    guard.committed = true;
    return true;
}

void PlayerSelectCmdrEvent::pack(BitStream& stream) const {
    stream.writeInt(peon - PlayerManager::BaseClientId, 7);
    if (stream.writeFlag(cmdr != 0)) {
        stream.writeInt(cmdr - PlayerManager::BaseClientId, 7);
    }
}

bool PlayerSelectCmdrEvent::unpack(BitStream& stream) {
    BitStreamRollbackGuard guard(stream);
    const Int32 parsedPeon = stream.readInt(7) + PlayerManager::BaseClientId;
    Int32 parsedCmdr = 0;
    if (stream.readFlag()) {
        parsedCmdr = stream.readInt(7) + PlayerManager::BaseClientId;
    }
    if (!stream.isValid()) {
        return false;
    }
    peon = parsedPeon;
    cmdr = parsedCmdr;
    guard.committed = true;
    return true;
}

TeamObjectiveEvent::TeamObjectiveEvent(Int32 oNum, std::string_view objText)
    : objNum(oNum), text(truncateTo(objText, PlayerManager::MaxMsgLength)) {
    type = TeamObjectiveEventType;
}

void TeamObjectiveEvent::pack(BitStream& stream) const {
    stream.write(objNum);
    stream.writeString(text);
}

bool TeamObjectiveEvent::unpack(BitStream& stream) {
    BitStreamRollbackGuard guard(stream);
    Int32 onum = 0;
    std::string objText{};
    if (!stream.read(onum) || !stream.readString(objText) || !stream.isValid()) {
        return false;
    }
    objNum = onum;
    text = std::move(objText);
    guard.committed = true;
    return true;
}

void PlayerCommandEvent::pack(BitStream& stream) const {
    stream.writeInt(target - PlayerManager::BaseClientId, 10);
    stream.writeInt(status, 2);
    if (!stream.writeFlag(statusOnly)) {
        if (stream.writeFlag(command != -1)) {
            stream.writeInt(commander - PlayerManager::BaseClientId, 10);
            stream.writeInt(command, 6);
            if (stream.writeFlag(commandTarget != -1)) {
                stream.writeInt(commandTarget, 8);
            }
            stream.writeInt(wayPoint.x, 10);
            stream.writeInt(wayPoint.y, 10);
        }
    }
    if (stream.writeFlag(hasMessage)) {
        if (stream.writeFlag(sendMessageString)) {
            stream.writeString(message);
        }
    }
}

bool PlayerCommandEvent::unpack(BitStream& stream) {
    BitStreamRollbackGuard guard(stream);
    const Int32 parsedTarget = stream.readInt(10) + PlayerManager::BaseClientId;
    const Int32 parsedStatus = stream.readInt(2);
    const bool parsedStatusOnly = stream.readFlag();
    Int32 parsedCmdr = -1;
    Int32 parsedCmd = -1;
    Int32 parsedCmdTarget = -1;
    Point2I parsedWp = wayPoint;
    if (!parsedStatusOnly) {
        if (stream.readFlag()) {
            parsedCmdr = stream.readInt(10) + PlayerManager::BaseClientId;
            parsedCmd = stream.readInt(6);
            if (stream.readFlag()) {
                parsedCmdTarget = stream.readInt(8);
            }
            parsedWp.x = stream.readInt(10);
            parsedWp.y = stream.readInt(10);
        }
    }
    const bool parsedHasMsg = stream.readFlag();
    bool parsedSendMsg = false;
    std::string parsedMsg{};
    if (parsedHasMsg) {
        parsedSendMsg = stream.readFlag();
        if (parsedSendMsg && !stream.readString(parsedMsg)) {
            return false;
        }
    }
    if (!stream.isValid()) {
        return false;
    }
    target = parsedTarget;
    status = parsedStatus;
    statusOnly = parsedStatusOnly;
    if (!parsedStatusOnly) {
        commander = parsedCmdr;
        command = parsedCmd;
        commandTarget = parsedCmdTarget;
        wayPoint = parsedWp;
    }
    hasMessage = parsedHasMsg;
    sendMessageString = parsedSendMsg;
    message = std::move(parsedMsg);
    guard.committed = true;
    return true;
}

void PlayerAddEvent::pack(BitStream& stream) const {
    stream.write(id);
    stream.writeFlag(isBaseRepType);
    stream.writeString(name);
    stream.writeString(skinBase);
    stream.writeString(voiceBase);
    stream.writeInt(gender, 1);
    stream.write(commanderId);
    if (stream.writeFlag(team != -1)) {
        stream.writeInt(team, 3);
    }
}

bool PlayerAddEvent::unpack(BitStream& stream) {
    BitStreamRollbackGuard guard(stream);
    Int32 parsedId = 0;
    if (!stream.read(parsedId)) {
        return false;
    }
    const bool parsedIsBase = stream.readFlag();
    std::string parsedName{};
    std::string parsedSkin{};
    std::string parsedVoice{};
    if (!stream.readString(parsedName) || !stream.readString(parsedSkin) ||
        !stream.readString(parsedVoice)) {
        return false;
    }
    const Int32 parsedGender = stream.readInt(1);
    Int32 parsedCmdr = -1;
    if (!stream.read(parsedCmdr)) {
        return false;
    }
    Int32 parsedTeam = -1;
    if (stream.readFlag()) {
        parsedTeam = stream.readInt(3);
    }
    if (!stream.isValid()) {
        return false;
    }
    id = parsedId;
    isBaseRepType = parsedIsBase;
    name = std::move(parsedName);
    skinBase = std::move(parsedSkin);
    voiceBase = std::move(parsedVoice);
    gender = parsedGender;
    commanderId = parsedCmdr;
    team = parsedTeam;
    guard.committed = true;
    return true;
}

void TeamAddEvent::pack(BitStream& stream) const {
    stream.write(energy);
    stream.write(id);
    stream.writeString(name);
    stream.writeString(skinBase);
}

bool TeamAddEvent::unpack(BitStream& stream) {
    BitStreamRollbackGuard guard(stream);
    Flt32 parsedEnergy = 0.0f;
    Int32 parsedId = 0;
    std::string parsedName{};
    std::string parsedSkin{};
    if (!stream.read(parsedEnergy) || !stream.read(parsedId) || !stream.readString(parsedName) ||
        !stream.readString(parsedSkin) || !stream.isValid()) {
        return false;
    }
    energy = parsedEnergy;
    id = parsedId;
    name = std::move(parsedName);
    skinBase = std::move(parsedSkin);
    guard.committed = true;
    return true;
}

VoiceEvent::VoiceEvent(Int32 srcId, std::string_view file)
    : sourceId(srcId), soundFileName(truncateTo(file, 31)) {
    type = VoiceEventType;
}

void VoiceEvent::pack(BitStream& stream) const {
    stream.writeInt(sourceId - PlayerManager::BaseClientId, 7);
    stream.writeString(soundFileName);
}

bool VoiceEvent::unpack(BitStream& stream) {
    BitStreamRollbackGuard guard(stream);
    const Int32 cid = stream.readInt(7) + PlayerManager::BaseClientId;
    std::string fname{};
    if (!stream.readString(fname) || !stream.isValid()) {
        return false;
    }
    sourceId = cid;
    soundFileName = std::move(fname);
    guard.committed = true;
    return true;
}

void SoundEvent::pack(BitStream& stream) const {
    stream.writeInt(ghostIndex, 10);
    stream.writeInt(channel, 2);
    if (dbm) {
        dbm->writeBlockId(stream, DataBlockManager::SoundDataType, soundId);
    } else {
        stream.writeInt(soundId + 1, 8);
    }
}

bool SoundEvent::unpack(BitStream& stream) {
    BitStreamRollbackGuard guard(stream);
    const Int32 gi = stream.readInt(10);
    const Int32 ch = stream.readInt(2);
    const Int32 sid =
        dbm ? dbm->readBlockId(stream, DataBlockManager::SoundDataType) : (stream.readInt(8) - 1);
    if (!stream.isValid()) {
        return false;
    }
    ghostIndex = gi;
    channel = ch;
    soundId = sid;
    guard.committed = true;
    return true;
}

void PlayerRemoveEvent::pack(BitStream& stream) const {
    stream.writeInt(id - PlayerManager::BaseClientId, 7);
}

bool PlayerRemoveEvent::unpack(BitStream& stream) {
    BitStreamRollbackGuard guard(stream);
    const Int32 cid = stream.readInt(7) + PlayerManager::BaseClientId;
    if (!stream.isValid()) {
        return false;
    }
    id = cid;
    guard.committed = true;
    return true;
}

void PlayerTeamChangeEvent::pack(BitStream& stream) const {
    stream.writeInt(playerId - PlayerManager::BaseClientId, 7);
    stream.writeInt(teamId + 1, 4);
}

bool PlayerTeamChangeEvent::unpack(BitStream& stream) {
    BitStreamRollbackGuard guard(stream);
    const Int32 pid = stream.readInt(7) + PlayerManager::BaseClientId;
    const Int32 tid = stream.readInt(4) - 1;
    if (!stream.isValid()) {
        return false;
    }
    playerId = pid;
    teamId = tid;
    guard.committed = true;
    return true;
}

// -----------------------------------------------------------------------------
// PlayerManager
// -----------------------------------------------------------------------------

PlayerManager::PlayerManager(bool onServer) : isServer_(onServer) {
    setId(PlayerManagerId);
    for (auto& cl : clientList_) {
        cl.clean();
    }
    for (auto& tm : teamList_) {
        tm.clean();
    }
    reset();
}

PlayerManager::~PlayerManager() = default;

PlayerManager* PlayerManager::get(sim::SimManager* manager) noexcept {
    if (!manager) {
        return nullptr;
    }
    return dynamic_cast<PlayerManager*>(manager->findObject(PlayerManagerId));
}

void PlayerManager::reset() {
    curCommandSequence_ = 0;
    lastCommandMessage_.clear();
    freeList_ = &clientList_[1];
    clientList_[1].clean();
    for (std::size_t i = 1; i + 1 < MaxClients; ++i) {
        clientList_[i].nextClient = &clientList_[i + 1];
        clientList_[i + 1].clean();
    }
    clientList_[MaxClients - 1].nextClient = nullptr;

    numClients_ = 0;
    numBaseReps_ = 0;
    clientLink_ = nullptr;
    teamScoreHeading_.clear();
    clientScoreHeading_.clear();

    for (std::size_t i = 0; i <= MaxTeams; ++i) {
        teamList_[i].clean();
    }
    numTeams_ = -1;
}

void PlayerManager::teamAddedInternal(Int32 teamId) {
    teamAdded(teamId);
}

void PlayerManager::teamAdded(Int32 teamId) {
    if (numTeams_ >= MaxTeams) {
        return;
    }
    if (teamId >= 0 && numTeams_ == -1) {
        teamAdded(-1);
    }
    const char suffix = static_cast<char>('0' + numTeams_);
    std::string teamName{};
    std::string skinBase{};
    if (boundConsole_) {
        std::string nameKey = "Server::teamName";
        nameKey.push_back(suffix);
        teamName = boundConsole_->getVariable(nameKey);
        if (teamName.empty()) {
            std::string dollarNameKey = "$Server::teamName";
            dollarNameKey.push_back(suffix);
            teamName = boundConsole_->getVariable(dollarNameKey);
        }
        std::string skinKey = "Server::teamSkin";
        skinKey.push_back(suffix);
        skinBase = boundConsole_->getVariable(skinKey);
        if (skinBase.empty()) {
            std::string dollarSkinKey = "$Server::teamSkin";
            dollarSkinKey.push_back(suffix);
            skinBase = boundConsole_->getVariable(dollarSkinKey);
        }
    }
    TeamRep& team = teamList_[static_cast<std::size_t>(numTeams_ + 1)];
    ++numTeams_;
    team.clean();
    team.id = teamId;
    team.team = teamId;
    team.selected = false;
    team.expanded = true;
    team.energy = 0.0f;
    team.name = teamName.empty() ? "unnamed" : truncateTo(teamName, 255);
    team.skinBase = skinBase.empty() ? "base" : truncateTo(skinBase, 255);
    for (auto& obj : team.objectives) {
        obj.seqNum = 1;
        obj.text.clear();
    }
}

void PlayerManager::teamAdded(std::string_view teamName, std::string_view skinBase) {
    if (numTeams_ == -1) {
        teamAdded(-1);
    }
    if (numTeams_ >= MaxTeams) {
        return;
    }
    const Int32 teamId = numTeams_;
    ++numTeams_;
    TeamRep* team = findTeam(teamId);
    if (!team) {
        return;
    }
    team->clean();
    team->id = teamId;
    team->team = teamId;
    team->selected = false;
    team->expanded = true;
    team->energy = 0.0f;
    team->name = teamName.empty() ? "unnamed" : truncateTo(teamName, 255);
    team->skinBase = skinBase.empty() ? "base" : truncateTo(skinBase, 255);
    for (auto& obj : team->objectives) {
        obj.seqNum = 1;
        obj.text.clear();
    }
    buildCommandTrees();
}

PlayerManager::TeamRep* PlayerManager::findTeam(Int32 teamId) noexcept {
    if (teamId < -1 || teamId >= numTeams_) {
        return nullptr;
    }
    return &teamList_[static_cast<std::size_t>(teamId + 1)];
}

const PlayerManager::TeamRep* PlayerManager::findTeam(Int32 teamId) const noexcept {
    if (teamId < -1 || teamId >= numTeams_) {
        return nullptr;
    }
    return &teamList_[static_cast<std::size_t>(teamId + 1)];
}

PlayerManager::TeamRep* PlayerManager::findTeam(std::string_view teamName) noexcept {
    for (Int32 i = -1; i < numTeams_; ++i) {
        TeamRep& t = teamList_[static_cast<std::size_t>(i + 1)];
        if (equalsIgnoreCase(t.name, teamName)) {
            return &t;
        }
    }
    return nullptr;
}

const PlayerManager::TeamRep* PlayerManager::findTeam(std::string_view teamName) const noexcept {
    for (Int32 i = -1; i < numTeams_; ++i) {
        const TeamRep& t = teamList_[static_cast<std::size_t>(i + 1)];
        if (equalsIgnoreCase(t.name, teamName)) {
            return &t;
        }
    }
    return nullptr;
}

PlayerManager::ClientRep* PlayerManager::findClient(Int32 clientId) noexcept {
    if (clientId < BaseClientId + 1 || clientId >= BaseClientId + MaxClients) {
        return nullptr;
    }
    ClientRep* p = &clientList_[static_cast<std::size_t>(clientId - BaseClientId)];
    if (!p->id) {
        return nullptr;
    }
    return p;
}

const PlayerManager::ClientRep* PlayerManager::findClient(Int32 clientId) const noexcept {
    if (clientId < BaseClientId + 1 || clientId >= BaseClientId + MaxClients) {
        return nullptr;
    }
    const ClientRep* p = &clientList_[static_cast<std::size_t>(clientId - BaseClientId)];
    if (!p->id) {
        return nullptr;
    }
    return p;
}

PlayerManager::BaseRep* PlayerManager::findBaseRep(Int32 id) noexcept {
    return findClient(id);
}

const PlayerManager::BaseRep* PlayerManager::findBaseRep(Int32 id) const noexcept {
    return findClient(id);
}

Int32 PlayerManager::getFreeId() noexcept {
    ClientRep* ret = freeList_;
    if (!ret) {
        return 0;
    }
    freeList_ = static_cast<ClientRep*>(freeList_->nextClient);
    ret->id = static_cast<Int32>(ret - clientList_.data()) + BaseClientId;
    ret->nextClient = nullptr;
    return ret->id;
}

void PlayerManager::linkClientRep(ClientRep* cr) noexcept {
    cr->nextClient = clientLink_;
    cr->prevClient = nullptr;
    if (clientLink_) {
        clientLink_->prevClient = cr;
    }
    clientLink_ = cr;
}

void PlayerManager::unlinkClientRep(ClientRep* cr) noexcept {
    if (cr->nextClient) {
        cr->nextClient->prevClient = cr->prevClient;
    }
    if (cr->prevClient) {
        cr->prevClient->nextClient = cr->nextClient;
    } else {
        clientLink_ = cr->nextClient;
    }
    cr->nextClient = nullptr;
    cr->prevClient = nullptr;
}

Int32 PlayerManager::clientAdded(std::string_view name,
                                 std::string_view voiceBase,
                                 std::string_view skinBase,
                                 bool gender,
                                 Int32 explicitClientId) {
    Int32 cid = explicitClientId;
    ClientRep* cr = nullptr;
    if (cid == 0) {
        cid = getFreeId();
        if (cid == 0) {
            return 0;
        }
        cr = &clientList_[static_cast<std::size_t>(cid - BaseClientId)];
    } else {
        if (cid < BaseClientId + 1 || cid >= BaseClientId + MaxClients) {
            return 0;
        }
        cr = &clientList_[static_cast<std::size_t>(cid - BaseClientId)];
        if (cr->id != 0) {
            return cr->id;
        }
        // Remove from freeList_ if present
        ClientRep* prev = nullptr;
        ClientRep* walk = freeList_;
        while (walk) {
            if (walk == cr) {
                if (prev) {
                    prev->nextClient = walk->nextClient;
                } else {
                    freeList_ = static_cast<ClientRep*>(walk->nextClient);
                }
                break;
            }
            prev = walk;
            walk = static_cast<ClientRep*>(walk->nextClient);
        }
        cr->clean();
        cr->id = cid;
    }

    linkClientRep(cr);
    ++numClients_;
    ++numBaseReps_;

    cr->selected = false;
    cr->expanded = true;
    cr->playerSequence = 0;
    cr->ping = 0;
    cr->packetLoss = 0.0f;
    cr->commander = nullptr;
    cr->firstPeon = nullptr;
    cr->nextPeon = nullptr;
    cr->curCommand = -1;
    cr->curCommandTarget = 0;
    cr->score = 0;
    cr->muted = false;
    cr->curGuiMode = InitialGuiMode;
    cr->sendScores = 0;
    cr->controlObject = nullptr;
    cr->ownedObject = nullptr;
    cr->playerPSC = nullptr;
    cr->commandStatus = ClientRep::None;
    cr->curRadioChannel = 0;
    cr->listenChannels = 1;
    cr->positionUpdateTime = 0;
    cr->curCommander = -1;
    cr->commanderId = -1;
    cr->team = 0;
    cr->loginStatus = 1;

    // The connection parser limits the chosen base name to 16 bytes; duplicate
    // name suffixes are added afterwards and must survive replication.
    cr->name = name.empty() ? "Unnamed" : truncateTo(name, 255);
    cr->skinBase = skinBase.empty() ? "base" : truncateTo(skinBase, 255);
    cr->gender = gender ? "MALE" : "FEMALE";
    if (!voiceBase.empty()) {
        cr->voiceBase = truncateTo(voiceBase, 255);
    } else {
        cr->voiceBase = gender ? "male1" : "female1";
    }

    buildCommandTrees();
    return cr->id;
}

Int32 PlayerManager::aiAdded(std::string_view name,
                             std::string_view voiceBase,
                             std::string_view skinBase,
                             bool gender) {
    const Int32 cid = clientAdded(name, voiceBase, skinBase, gender, 0);
    if (cid == 0) {
        return 0;
    }
    if (ClientRep* cr = findClient(cid)) {
        cr->loginStatus = 0;
        if (numClients_ > 0) {
            --numClients_;
        }
    }
    return cid;
}

void PlayerManager::clientDropped(Int32 clientId) {
    ClientRep* dropClient = findClient(clientId);
    if (!dropClient) {
        return;
    }
    unlinkClientRep(dropClient);

    for (BaseRep* walk = clientLink_; walk != nullptr; walk = walk->nextClient) {
        auto* cl = static_cast<ClientRep*>(walk);
        if (cl->commanderId == clientId) {
            cl->commanderId = -1;
        }
        if (cl->curCommand != -1 && cl->curCommandTarget == clientId - BaseClientId) {
            cl->curCommand = -1;
            cl->curCommandTarget = -1;
            cl->curCommandString.clear();
        }
    }

    if (dropClient->loginStatus != 0 && numClients_ > 0) {
        --numClients_;
    }
    if (numBaseReps_ > 0) {
        --numBaseReps_;
    }

    dropClient->clean();
    dropClient->nextClient = freeList_;
    freeList_ = dropClient;

    buildCommandTrees();
}

void PlayerManager::serverClientTeamChange(Int32 clientId, Int32 teamId) {
    ClientRep* theClient = findClient(clientId);
    if (!theClient) {
        return;
    }
    theClient->team = teamId;
    theClient->commanderId = -1;
    theClient->curCommand = -1;
    theClient->curCommandTarget = -1;
    theClient->curCommandString.clear();

    for (BaseRep* walk = clientLink_; walk != nullptr; walk = walk->nextClient) {
        auto* cl = static_cast<ClientRep*>(walk);
        if (cl->commanderId == clientId) {
            cl->commanderId = -1;
        }
        if (cl->curCommand != -1 && cl->curCommandTarget == clientId - BaseClientId) {
            cl->curCommand = -1;
            cl->curCommandTarget = -1;
            cl->curCommandString.clear();
        }
    }

    if (theClient->ownedObject) {
        theClient->ownedObject->setTeam(teamId);
    }

    buildCommandTrees();
}

void PlayerManager::setTeamScoreHeading(std::string_view heading) {
    teamScoreHeading_ = truncateTo(heading, 255);
}

void PlayerManager::setClientScoreHeading(std::string_view heading) {
    clientScoreHeading_ = truncateTo(heading, 255);
}

void PlayerManager::setTeamScore(Int32 teamId, std::string_view scoreString, Int32 sortValue) {
    if (TeamRep* tr = findTeam(teamId)) {
        tr->scoreString = truncateTo(scoreString, 255);
        tr->scoreSortValue = sortValue;
    }
}

void PlayerManager::setClientScore(Int32 clientId, std::string_view scoreString, Int32 sortValue) {
    if (BaseRep* br = findBaseRep(clientId)) {
        br->scoreString = truncateTo(scoreString, 255);
        br->scoreSortValue = sortValue;
    }
}

void PlayerManager::setDisplayScores(Int32 clientId, bool display) {
    if (ClientRep* cr = findClient(clientId)) {
        cr->sendScores = display ? 1u : 0u;
    }
}

void PlayerManager::setClientSkin(Int32 clientId, std::string_view newSkin) {
    if (newSkin.empty() || newSkin.size() > 19 ||
        !std::ranges::all_of(newSkin, [](unsigned char c) {
            return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
        })) {
        return;
    }
    auto* client = findClient(clientId);
    if (!client || !client->loginStatus || client->skinBase == newSkin) {
        return;
    }
    client->skinBase = newSkin;
    if (isServer_) {
        for (auto* rep = getClientList(); rep; rep = static_cast<ClientRep*>(rep->nextClient)) {
            sendPlayerEvent(*rep, std::make_unique<PlayerSkinEvent>(clientId, newSkin));
        }
    }
}

void PlayerManager::missionReset() {
    for (auto* base = getBaseRepList(); base; base = base->nextClient) {
        auto* client = static_cast<ClientRep*>(base);
        if (isServer_ && client->packetStream) {
            auto event = std::make_unique<MissionResetEvent>();
            event->flags = sim::SimEvent::Guaranteed | sim::SimEvent::Ordered;
            (void)client->packetStream->sendRemoteEvent(std::move(event));
        }
        client->curCommand = -1;
        client->commandStatus = CMD_NONE;
        client->curCommandSequence = client->lastCommandSequence = client->lastObjectiveSequence =
            0;
        if (isServer_) {
            client->scoreString.clear();
        }
    }
    curCommandSequence_ = 0;
    numTeams_ = -1;
    if (auto* paths = getManager() ? dynamic_cast<world::SimPathManager*>(
                                         getManager()->findObject(world::PathManagerId))
                                   : nullptr) {
        paths->reset();
    }
    if (isServer_) {
        teamAdded(-1);
    }
}

void PlayerManager::setTeamSkin(Int32 teamId, std::string_view newSkin) {
    if (TeamRep* tr = findTeam(teamId)) {
        tr->skinBase = newSkin.empty() ? "base" : truncateTo(newSkin, 255);
    }
}

std::string_view PlayerManager::getTeamSkinBase(Int32 teamId) const noexcept {
    if (const TeamRep* tr = findTeam(teamId)) {
        return tr->skinBase.empty() ? std::string_view{"base"} : std::string_view{tr->skinBase};
    }
    return "base";
}

std::string_view PlayerManager::getPlayerSkinBase(Int32 clientId) const noexcept {
    const BaseRep* cl = findBaseRep(clientId);
    if (!cl) {
        return "base";
    }
    if (!cl->skinBase.empty()) {
        return cl->skinBase;
    }
    if (const TeamRep* clTeam = findTeam(cl->team)) {
        return clTeam->skinBase.empty() ? std::string_view{"base"}
                                        : std::string_view{clTeam->skinBase};
    }
    return "base";
}

void PlayerManager::setTeamObjective(Int32 teamId, Int32 objNum, std::string_view text) {
    if (objNum < 0 || objNum >= MaxObjectives) {
        return;
    }
    TeamRep* tr = findTeam(teamId);
    if (!tr) {
        return;
    }
    auto& obj = tr->objectives[static_cast<std::size_t>(objNum)];
    obj.seqNum = ++curObjectiveSeq_;
    obj.text = truncateTo(text, MaxMsgLength);
    if (isServer_) {
        for (auto* receiver = getClientList(); receiver;
             receiver = static_cast<ClientRep*>(receiver->nextClient)) {
            if (receiver->loginStatus && receiver->team == teamId && receiver->curGuiMode == 3) {
                sendPlayerEvent(*receiver, std::make_unique<TeamObjectiveEvent>(objNum, obj.text));
                receiver->lastObjectiveSequence = curObjectiveSeq_;
            }
        }
    }
}

void PlayerManager::clearTeamObjectives(Int32 teamId) {
    TeamRep* tr = findTeam(teamId);
    if (!tr) {
        return;
    }
    for (Int32 index = 0; index < MaxObjectives; ++index) {
        if (!tr->objectives[std::size_t(index)].text.empty()) {
            setTeamObjective(teamId, index, {});
        }
    }
}

void PlayerManager::buildCommandTrees() noexcept {
    for (auto& cl : clientList_) {
        cl.commander = nullptr;
        cl.firstPeon = nullptr;
        cl.nextPeon = nullptr;
    }
    for (auto& tm : teamList_) {
        tm.firstCommander = nullptr;
    }

    for (BaseRep* bwalk = clientLink_; bwalk != nullptr; bwalk = bwalk->nextClient) {
        auto* walk = static_cast<ClientRep*>(bwalk);
        walk->commander = findClient(walk->commanderId);
        if (walk->commander && walk->commander->team != walk->team) {
            walk->commander = nullptr;
            walk->commanderId = -1;
        }

        ClientRep* loopCheck = walk->commander;
        while (loopCheck != nullptr) {
            if (loopCheck == walk) {
                break;
            }
            loopCheck = findClient(loopCheck->commanderId);
        }
        if (loopCheck != nullptr) {
            walk->commander = nullptr;
            walk->commanderId = -1;
        }

        if (walk->commander) {
            walk->nextPeon = walk->commander->firstPeon;
            walk->commander->firstPeon = walk;
        } else if (TeamRep* team = findTeam(walk->team)) {
            walk->nextPeon = team->firstCommander;
            team->firstCommander = walk;
        }
    }
}

void PlayerManager::sendPlayerEvent(ClientRep& receiver, std::unique_ptr<sim::SimEvent> event) {
    event->flags = sim::SimEvent::Guaranteed | sim::SimEvent::Ordered;
    event->address.objectId = PlayerManagerId;
    event->address.managerId = UInt32(receiver.id);
    if (receiver.packetStream) {
        (void)receiver.packetStream->sendRemoteEvent(std::move(event));
    } else if (receiver.eventManager) {
        (void)receiver.eventManager->postRemoteEvent(std::move(event));
    } else if (onLocalEvent) {
        onLocalEvent(std::move(event));
    }
}

void PlayerManager::sendCommand(ClientRep& receiver,
                                const ClientRep& target,
                                const std::optional<std::string_view>& message,
                                bool statusOnly) {
    auto event = std::make_unique<PlayerCommandEvent>();
    event->target = target.id;
    event->status = target.commandStatus;
    event->statusOnly = statusOnly;
    event->commander = target.curCommander;
    event->command = target.curCommand;
    event->commandTarget = target.curCommandTarget;
    event->wayPoint = target.wayPoint;
    event->hasMessage = message.has_value();
    if (message) {
        // Native ordered command events can reuse the receiver's last string.
        event->sendMessageString =
            !receiver.lastCommandMessage || *receiver.lastCommandMessage != *message;
        if (event->sendMessageString) {
            event->message = truncateTo(*message, 255);
            receiver.lastCommandMessage = event->message;
        }
    }
    sendPlayerEvent(receiver, std::move(event));
}

bool PlayerManager::isInCommandChain(Int32 peonId, Int32 cmdrId) noexcept {
    auto* walk = findClient(peonId);
    for (Int32 count = 0; walk && count < MaxClients; ++count) {
        if (walk->commanderId == cmdrId) {
            return true;
        }
        walk = findClient(walk->commanderId);
    }
    return false;
}

void PlayerManager::setCommander(Int32 peonId, Int32 cmdrId) noexcept {
    auto* peon = findClient(peonId);
    auto* cmdr = findClient(cmdrId);
    if (!peon ||
        (cmdr && (cmdr == peon || cmdr->team != peon->team || isInCommandChain(cmdrId, peonId)))) {
        return;
    }
    peon->commanderId = cmdr ? cmdrId : -1;
    buildCommandTrees();
}

void PlayerManager::selectCommander(Int32 peonId, Int32 cmdrId) {
    if (!isServer_ || peonId == cmdrId) {
        return;
    }
    auto* peon = findClient(peonId);
    if (!peon) {
        return;
    }
    const auto previous = peon->commanderId;
    setCommander(peonId, cmdrId);
    if (previous == peon->commanderId) {
        return;
    }
    for (auto* receiver = getClientList(); receiver;
         receiver = static_cast<ClientRep*>(receiver->nextClient)) {
        if (receiver->loginStatus) {
            sendPlayerEvent(*receiver,
                            std::make_unique<PlayerSelectCmdrEvent>(
                                peonId, peon->commanderId < 0 ? 0 : peon->commanderId));
        }
    }
}

void PlayerManager::selectCommander(Int32 cmdrId) {
    for (auto* base = clientLink_; base; base = base->nextClient) {
        if (base->selected) {
            selectCommander(base->id, cmdrId);
        }
    }
}

void PlayerManager::issueCommand(Int32 cmdrId,
                                 Int32 targId,
                                 Int32 commandIcon,
                                 std::string_view commandString,
                                 const Point2I& wayPoint,
                                 Int32 objectIndex) {
    if (!isServer_ || objectIndex < -1 || objectIndex > 255) {
        return;
    }
    auto* cmdr = findClient(cmdrId);
    auto* target = findClient(targId);
    ++curCommandSequence_;
    if (!target || (cmdrId && (!cmdr || !cmdr->loginStatus || cmdr->team != target->team))) {
        return;
    }
    if (!(cmdrId == targId || !cmdrId || isInCommandChain(targId, cmdrId) ||
          (target->commanderId == -1 &&
           (target->curCommand == -1 || target->curCommander != targId)))) {
        return;
    }
    target->curCommandSequence = curCommandSequence_;
    target->curCommander = cmdrId;
    target->curCommand = commandIcon;
    target->curCommandTarget = objectIndex;
    target->wayPoint = wayPoint;
    target->curCommandString = truncateTo(commandString, 255);
    target->commandStatus = cmdrId && cmdrId != targId ? CMD_RECEIVED : CMD_ACKNOWLEDGED;
    if (!target->loginStatus) {
        if (objectIndex != -1) {
            return;
        }
        if (boundConsole_) {
            auto* aiManager = getManager()
                                  ? dynamic_cast<AIManager*>(getManager()->findObject(AIManagerId))
                                  : nullptr;
            const auto* ai = aiManager ? aiManager->findByPlayerId(targId) : nullptr;
            const std::array<std::string, 8> args{ai ? ai->name : target->name,
                                                  std::to_string(cmdrId),
                                                  std::to_string(commandIcon),
                                                  std::to_string(wayPoint.x) + " " +
                                                      std::to_string(wayPoint.y),
                                                  std::to_string(objectIndex),
                                                  target->curCommandString,
                                                  std::to_string(target->commandStatus),
                                                  std::to_string(target->curCommandSequence)};
            (void)boundConsole_->execute("AI::onCommand", args);
            // Script callbacks may delete or reassign the recipient.
            target = findClient(targId);
            if (!target) {
                return;
            }
        }
    }
    for (auto* receiver = getClientList(); receiver;
         receiver = static_cast<ClientRep*>(receiver->nextClient)) {
        if (!receiver->loginStatus) {
            continue;
        }
        if (receiver->curGuiMode == PlayerPSC::CommandGuiMode) {
            receiver->lastCommandSequence = curCommandSequence_;
        }
        if (receiver->team == target->team &&
            (receiver->curGuiMode == PlayerPSC::CommandGuiMode || receiver == target)) {
            const auto message = receiver->id == targId || receiver->id == cmdrId
                                     ? std::optional<std::string_view>(target->curCommandString)
                                     : std::nullopt;
            sendCommand(*receiver, *target, message, false);
        }
    }
}

bool PlayerManager::commandStatusReport(Int32 peonId, Int32 status, std::string_view message) {
    if (!isServer_) {
        return false;
    }
    auto* peon = findClient(peonId);
    if (!peon || !peon->loginStatus) {
        return false;
    }
    ++curCommandSequence_;
    if (peon->commandStatus != CMD_RECEIVED &&
        !(peon->commandStatus == CMD_ACKNOWLEDGED && !status)) {
        return false;
    }
    peon->curCommandSequence = curCommandSequence_;
    peon->commandStatus = status ? CMD_ACKNOWLEDGED : CMD_NONE;
    const auto oldCommander = peon->curCommander;
    if (!status) {
        peon->curCommand = -1;
        peon->curCommander = -1;
    }
    for (auto* receiver = getClientList(); receiver;
         receiver = static_cast<ClientRep*>(receiver->nextClient)) {
        if (!receiver->loginStatus) {
            continue;
        }
        if (receiver->curGuiMode == PlayerPSC::CommandGuiMode) {
            receiver->lastCommandSequence = curCommandSequence_;
        }
        if (receiver->team == peon->team &&
            (receiver->curGuiMode == PlayerPSC::CommandGuiMode || receiver == peon)) {
            sendCommand(*receiver,
                        *peon,
                        receiver == peon || receiver->id == oldCommander ? std::optional(message)
                                                                         : std::nullopt,
                        true);
        }
    }
    return true;
}

void PlayerManager::displayCommandMessage(ClientRep* speaker, Int32 type, std::string message) {
    const auto token = message.find("~w");
    if (token != std::string::npos) {
        const auto sound = message.substr(token + 2);
        message.resize(token);
        auto* console = boundConsole_  ? boundConsole_
                        : getManager() ? getManager()->console()
                                       : nullptr;
        bool enabled = true;
        if (console) {
            if (console->getVariable("pref::playVoices").empty()) {
                console->setVariable("pref::playVoices", "TRUE");
            }
            enabled = script::toBool(console->getVariable("pref::playVoices"));
        }
        auto* resources = getManager() ? sim::SimResource::find(*getManager()) : nullptr;
        auto* playback =
            getManager()
                ? dynamic_cast<SoundPlayback*>(getManager()->findObject("GameSoundPlayback"))
                : nullptr;
        if (enabled && !sound.empty() && resources && playback && playback->playVoice) {
            auto filename = speaker ? speaker->voiceBase + ".w" + sound + ".wav" : sound;
            if (!resources->get().contains(filename)) {
                filename = "w" + sound + ".wav";
            }
            if (resources->get().contains(filename)) {
                SoundProfileData profile;
                profile.profile.flags = 0;
                (void)playback->playVoice(
                    speaker ? speaker->id : 0, filename, profile, TMat3F(EulerF{}, {}), {});
            }
        }
    }
    if (onChatMessage) {
        onChatMessage(type, message);
    }
}

void PlayerManager::mute(Int32 clientId, bool muteFlag) noexcept {
    if (ClientRep* cr = findClient(clientId)) {
        cr->muted = muteFlag;
    }
}

void PlayerManager::selectClient(Int32 clientId, bool selectFlag) noexcept {
    if (BaseRep* br = findBaseRep(clientId)) {
        br->selected = selectFlag;
    }
}

void PlayerManager::expandClient(Int32 clientId, bool expandFlag) noexcept {
    if (BaseRep* br = findBaseRep(clientId)) {
        br->expanded = expandFlag;
    }
}

void PlayerManager::clientGuiModeChanged(Int32 clientId, UInt32 newGuiMode, UInt32 oldGuiMode) {
    auto* client = findClient(clientId);
    if (!client) {
        return;
    }
    if (isServer_ && newGuiMode == PlayerPSC::CommandGuiMode) {
        for (auto* target = getClientList(); target;
             target = static_cast<ClientRep*>(target->nextClient)) {
            if (target != client && target->team == client->team &&
                (client->lastCommandSequence < target->curCommandSequence ||
                 target->curCommandTarget != -1)) {
                sendCommand(*client, *target, std::nullopt, false);
            }
        }
        client->lastCommandSequence = curCommandSequence_;
    }
    // Native objective/victory modes are 3/5. Victory freezes future updates.
    if (isServer_ && oldGuiMode != 3 && oldGuiMode != 5 && (newGuiMode == 3 || newGuiMode == 5)) {
        if (const auto* team = findTeam(client->team)) {
            for (Int32 index = 0; index < MaxObjectives; ++index) {
                const auto& objective = team->objectives[std::size_t(index)];
                if (client->lastObjectiveSequence < objective.seqNum) {
                    sendPlayerEvent(*client,
                                    std::make_unique<TeamObjectiveEvent>(index, objective.text));
                }
            }
            client->lastObjectiveSequence = curObjectiveSeq_;
        }
    }
    client->curGuiMode = newGuiMode;
}

void PlayerManager::sendPingPackets() {
    auto evTemplate = std::make_unique<PingPLEvent>();
    for (BaseRep* pwalk = clientLink_; pwalk != nullptr; pwalk = pwalk->nextClient) {
        const auto* pcl = static_cast<const ClientRep*>(pwalk);
        evTemplate->entries.push_back(PingPLEvent::Entry{pcl->id, pcl->ping, pcl->packetLoss});
    }
    for (BaseRep* walk = clientLink_; walk != nullptr; walk = walk->nextClient) {
        auto* cl = static_cast<ClientRep*>(walk);
        if (!cl->eventManager || cl->sendScores == 0) {
            continue;
        }
        auto ev = std::make_unique<PingPLEvent>(*evTemplate);
        (void)cl->eventManager->postRemoteEvent(std::move(ev));
    }
}

void PlayerManager::applyLanguageFilter(std::string& message, std::string_view badWords) noexcept {
    auto filterWithWord = [&](std::string_view rawWord) noexcept {
        if (rawWord.empty()) {
            return;
        }
        std::array<char, 12> currentWord{};
        const std::size_t wordLen = std::min<std::size_t>(rawWord.size(), 11);
        for (std::size_t i = 0; i < wordLen; ++i) {
            currentWord[i] =
                static_cast<char>(std::tolower(static_cast<unsigned char>(rawWord[i])));
        }
        std::array<std::size_t, 12> charOffsets{};
        for (std::size_t index = 0; index < message.size(); ++index) {
            std::size_t it = index;
            std::size_t ct = 0;
            for (; ct < wordLen; ++ct) {
                while (it < message.size() &&
                       !std::isalnum(static_cast<unsigned char>(message[it]))) {
                    ++it;
                }
                if (it < message.size() &&
                    static_cast<char>(std::tolower(static_cast<unsigned char>(message[it]))) ==
                        currentWord[ct]) {
                    charOffsets[ct] = it;
                    ++it;
                } else {
                    break;
                }
            }
            if (ct == wordLen) {
                for (std::size_t i = 0; i < wordLen; ++i) {
                    message[charOffsets[i]] = getBadChar();
                }
            }
        }
    };

    if (!badWords.empty()) {
        std::size_t pos = 0;
        while (pos < badWords.size()) {
            while (pos < badWords.size() &&
                   std::isspace(static_cast<unsigned char>(badWords[pos]))) {
                ++pos;
            }
            if (pos >= badWords.size()) {
                break;
            }
            std::size_t end = pos;
            while (end < badWords.size() &&
                   !std::isspace(static_cast<unsigned char>(badWords[end])) && (end - pos) < 11) {
                ++end;
            }
            filterWithWord(badWords.substr(pos, end - pos));
            pos = end;
        }
    } else {
        for (const std::string_view word : kBadWords) {
            filterWithWord(word);
        }
    }
}

bool PlayerManager::onAdd() {
    return Parent::onAdd();
}

void PlayerManager::onRemove() {
    Parent::onRemove();
}

bool PlayerManager::processEvent(const sim::SimEvent& event) {
    switch (event.type) {
        case PlayerSayEventType: {
            if (isServer_) {
                return false;
            }
            const auto* ev = dynamic_cast<const PlayerSayEvent*>(&event);
            if (!ev) {
                return false;
            }
            auto* speaker = findClient(ev->sender);
            auto message = ev->message;
            auto* console = boundConsole_  ? boundConsole_
                            : getManager() ? getManager()->console()
                                           : nullptr;
            if (console && script::toBool(console->getVariable("pref::filterBadWords"))) {
                applyLanguageFilter(message);
            }
            if (console && console->isFunction("onClientMessage")) {
                const std::array<std::string, 2> args{speaker ? std::to_string(speaker->id) : "0",
                                                      message};
                if (!script::toBool(console->execute("onClientMessage", args).value)) {
                    return true;
                }
            }
            displayCommandMessage(speaker,
                                  static_cast<Int32>(ev->msgType),
                                  speaker ? speaker->name + ": " + message : message);
            return true;
        }
        case VoiceEventType: {
            if (isServer_) {
                return false;
            }
            const auto* voice = dynamic_cast<const VoiceEvent*>(&event);
            auto* speaker = voice ? findClient(voice->sourceId) : nullptr;
            auto* console = boundConsole_  ? boundConsole_
                            : getManager() ? getManager()->console()
                                           : nullptr;
            if (console) {
                if (console->getVariable("pref::playVoices").empty()) {
                    console->setVariable("pref::playVoices", "TRUE");
                }
                if (!script::toBool(console->getVariable("pref::playVoices"))) {
                    return true;
                }
            }
            if (!speaker || !speaker->ownedObject || !getManager()) {
                return true;
            }
            auto* resources = sim::SimResource::find(*getManager());
            auto* blocks =
                dynamic_cast<DataBlockManager*>(getManager()->findObject(DataBlockManagerId));
            auto* profile = blocks ? dynamic_cast<SoundProfileData*>(blocks->lookupDataBlock(
                                         0, DataBlockManager::SoundProfileDataType))
                                   : nullptr;
            auto* playback =
                dynamic_cast<SoundPlayback*>(getManager()->findObject("GameSoundPlayback"));
            if (!resources || !profile || !playback || !playback->playVoice) {
                return true;
            }
            auto filename = speaker->voiceBase + ".w" + voice->soundFileName + ".wav";
            if (!resources->get().contains(filename)) {
                filename = "w" + voice->soundFileName + ".wav";
                if (!resources->get().contains(filename)) {
                    return true;
                }
            }
            (void)playback->playVoice(speaker->id,
                                      filename,
                                      *profile,
                                      speaker->ownedObject->getTransform(),
                                      speaker->ownedObject->getLinearVelocity());
            return true;
        }
        case MissionResetEventType:
            if (isServer_) {
                return false;
            }
            missionReset();
            return true;
        case PingPLEventType: {
            if (const auto* ev = dynamic_cast<const PingPLEvent*>(&event)) {
                for (const auto& entry : ev->entries) {
                    if (ClientRep* cl = findClient(entry.clientId)) {
                        cl->ping = entry.ping;
                        cl->packetLoss = entry.packetLoss;
                    }
                }
                return true;
            }
            return false;
        }
        case DeltaScoreEventType: {
            if (const auto* ev = dynamic_cast<const DeltaScoreEvent*>(&event)) {
                if (ev->isTeam) {
                    if (ev->id == -1) {
                        setTeamScoreHeading(ev->score);
                    } else {
                        setTeamScore(ev->id, ev->score, ev->sortValue);
                    }
                } else {
                    if (ev->id == BaseClientId) {
                        setClientScoreHeading(ev->score);
                    } else {
                        setClientScore(ev->id, ev->score, ev->sortValue);
                    }
                }
                return true;
            }
            return false;
        }
        case PlayerSkinEventType: {
            if (const auto* ev = dynamic_cast<const PlayerSkinEvent*>(&event)) {
                setClientSkin(ev->clientId, ev->skinBase);
                return true;
            }
            return false;
        }
        case PlayerAddEventType: {
            if (isServer_) {
                return false;
            }
            if (const auto* ev = dynamic_cast<const PlayerAddEvent*>(&event)) {
                const bool joined = findBaseRep(ev->id) == nullptr;
                const Int32 cid =
                    clientAdded(ev->name, ev->voiceBase, ev->skinBase, ev->gender != 0, ev->id);
                if (ClientRep* cl = findClient(cid)) {
                    cl->team = ev->team;
                    cl->commanderId = ev->commanderId;
                    cl->loginStatus = ev->isBaseRepType ? 0u : 1u;
                    buildCommandTrees();
                    if (joined && cl->loginStatus) {
                        if (boundConsole_ && boundConsole_->isFunction("onClientJoin")) {
                            const std::array<std::string, 1> args{std::to_string(cid)};
                            (void)boundConsole_->execute("onClientJoin", args);
                        }
                        displayCommandMessage(nullptr,
                                              FearGuiChatDisplay::MSG_System,
                                              ev->name + " connected to the game.");
                    }
                }
                return true;
            }
            return false;
        }
        case TeamAddEventType: {
            if (const auto* ev = dynamic_cast<const TeamAddEvent*>(&event)) {
                if (ev->id == -1) {
                    if (TeamRep* tr = findTeam(-1)) {
                        tr->energy = ev->energy;
                        tr->name = ev->name;
                        tr->skinBase = ev->skinBase;
                    }
                } else {
                    while (numTeams_ <= ev->id && numTeams_ < MaxTeams) {
                        teamAdded(ev->name, ev->skinBase);
                    }
                    if (TeamRep* tr = findTeam(ev->id)) {
                        tr->energy = ev->energy;
                        tr->name = ev->name;
                        tr->skinBase = ev->skinBase;
                    }
                }
                return true;
            }
            return false;
        }
        case PlayerRemoveEventType: {
            if (isServer_) {
                return false;
            }
            if (const auto* ev = dynamic_cast<const PlayerRemoveEvent*>(&event)) {
                if (const auto* client = findClient(ev->id); client && client->loginStatus) {
                    const auto name = client->name;
                    if (boundConsole_ && boundConsole_->isFunction("onClientDrop")) {
                        const std::array<std::string, 1> args{std::to_string(ev->id)};
                        (void)boundConsole_->execute("onClientDrop", args);
                    }
                    displayCommandMessage(
                        nullptr, FearGuiChatDisplay::MSG_System, name + " dropped.");
                }
                clientDropped(ev->id);
                return true;
            }
            return false;
        }
        case PlayerTeamChangeEventType: {
            if (isServer_) {
                return false;
            }
            if (const auto* ev = dynamic_cast<const PlayerTeamChangeEvent*>(&event)) {
                serverClientTeamChange(ev->playerId, ev->teamId);
                if (const auto* client = findClient(ev->playerId); client && client->loginStatus) {
                    const auto name = client->name;
                    if (boundConsole_ && boundConsole_->isFunction("onClientChangeTeam")) {
                        const std::array<std::string, 2> args{std::to_string(ev->playerId),
                                                              std::to_string(ev->teamId)};
                        (void)boundConsole_->execute("onClientChangeTeam", args);
                    }
                    const auto* team = findTeam(ev->teamId);
                    displayCommandMessage(nullptr,
                                          FearGuiChatDisplay::MSG_System,
                                          name + (ev->teamId == -1 ? " is no longer on a team."
                                                  : team && getNumTeams() > 1
                                                      ? " joined team " + team->name + "."
                                                      : " joined the fray."));
                }
                return true;
            }
            return false;
        }
        case TeamObjectiveEventType: {
            if (isServer_) {
                return false;
            }
            if (const auto* ev = dynamic_cast<const TeamObjectiveEvent*>(&event)) {
                const auto* local =
                    getManager() ? findClient(Int32(getManager()->getId())) : nullptr;
                if (auto* team = local ? findTeam(local->team) : nullptr;
                    team && ev->objNum >= 0 && ev->objNum < MaxObjectives) {
                    team->objectives[std::size_t(ev->objNum)].text = ev->text;
                }
                return true;
            }
            return false;
        }
        case PlayerSelectCmdrEventType: {
            if (isServer_) {
                return false;
            }
            if (const auto* ev = dynamic_cast<const PlayerSelectCmdrEvent*>(&event)) {
                auto* peon = findClient(ev->peon);
                auto* commander = findClient(ev->cmdr);
                if (!peon) {
                    return true;
                }
                setCommander(ev->peon, ev->cmdr);
                const auto local = getManager() ? Int32(getManager()->getId()) : 0;
                if (commander && local == peon->id) {
                    displayCommandMessage(nullptr,
                                          FearGuiChatDisplay::MSG_TeamChat,
                                          "You have been placed under the command of " +
                                              commander->name + ".");
                } else if (commander && local == commander->id) {
                    displayCommandMessage(nullptr,
                                          FearGuiChatDisplay::MSG_TeamChat,
                                          "You are now in command of " + peon->name + ".");
                } else if (!commander && local == peon->id) {
                    displayCommandMessage(
                        nullptr, FearGuiChatDisplay::MSG_TeamChat, "You now have no commander.");
                }
                return true;
            }
            return false;
        }
        case PlayerCommandEventType: {
            if (isServer_) {
                return false;
            }
            if (const auto* ev = dynamic_cast<const PlayerCommandEvent*>(&event)) {
                if (ev->hasMessage && ev->sendMessageString) {
                    lastCommandMessage_ = ev->message;
                }
                auto* target = findClient(ev->target);
                if (!target) {
                    return true;
                }
                const auto local = getManager() ? Int32(getManager()->getId()) : 0;
                if (ev->statusOnly) {
                    const auto commander = target->curCommander;
                    if (ev->status == CMD_NONE) {
                        target->curCommand = -1;
                        target->curCommander = -1;
                    }
                    target->commandStatus = ev->status;
                    if (ev->hasMessage && (commander == local || ev->target == local)) {
                        displayCommandMessage(target,
                                              FearGuiChatDisplay::MSG_TeamChat,
                                              target->name + ": " + lastCommandMessage_);
                    }
                } else {
                    auto* commander = findClient(ev->commander);
                    if (!commander && ev->command != -1) {
                        return true;
                    }
                    target->curCommand = ev->command;
                    target->curCommander = ev->commander;
                    target->curCommandTarget = ev->commandTarget;
                    target->commandStatus = ev->status;
                    target->wayPoint = ev->wayPoint;
                    if (ev->hasMessage) {
                        target->curCommandString = lastCommandMessage_;
                    }
                    if (commander && ev->hasMessage && ev->target == local) {
                        displayCommandMessage(commander,
                                              FearGuiChatDisplay::MSG_Command,
                                              commander->name + ": " + lastCommandMessage_);
                    } else if (commander && ev->hasMessage && ev->commander == local) {
                        displayCommandMessage(commander,
                                              FearGuiChatDisplay::MSG_TeamChat,
                                              "Cmd. to " + target->name + ": " +
                                                  lastCommandMessage_);
                    }
                }
                return true;
            }
            return false;
        }
        default:
            return Parent::processEvent(event);
    }
}

bool registerPlayerManagerEvents(sim::network::Registry& registry) {
    const auto reg = [&]<typename EvT>(Int32 id) {
        return registry.registerEvent<EvT>(
            id,
            [](const EvT& ev, BitStream& bs, sim::network::EventContext& context) {
                if constexpr (std::is_same_v<EvT, SoundEvent>) {
                    auto sound = ev;
                    sound.dbm = dynamic_cast<DataBlockManager*>(
                        context.manager.findObject(DataBlockManagerId));
                    if (!sound.dbm) {
                        return false;
                    }
                    sound.pack(bs);
                } else {
                    ev.pack(bs);
                }
                return bs.isValid();
            },
            [](EvT& ev, BitStream& bs, sim::network::EventContext& context) {
                if constexpr (std::is_same_v<EvT, PlayerSayEvent>) {
                    if (context.server) {
                        return false;
                    }
                }
                if constexpr (std::is_same_v<EvT, SoundEvent>) {
                    if (context.server) {
                        return false;
                    }
                    ev.dbm = dynamic_cast<DataBlockManager*>(
                        context.manager.findObject(DataBlockManagerId));
                    if (!ev.dbm) {
                        return false;
                    }
                }
                if constexpr (std::is_same_v<EvT, VoiceEvent>) {
                    if (context.server) {
                        return false;
                    }
                }
                if (!ev.unpack(bs)) {
                    return false;
                }
                if constexpr (std::is_same_v<EvT, SoundEvent>) {
                    ev.address.objectId = static_cast<UInt32>(ev.ghostIndex);
                    ev.flags |= sim::SimEvent::ToGhost;
                } else {
                    ev.address.objectId = PlayerManagerId;
                }
                return true;
            });
    };
    return reg.template operator()<PlayerAddEvent>(PlayerAddEventType) &&
           reg.template operator()<PlayerRemoveEvent>(PlayerRemoveEventType) &&
           reg.template operator()<DeltaScoreEvent>(DeltaScoreEventType) &&
           reg.template operator()<TeamAddEvent>(TeamAddEventType) &&
           reg.template operator()<MissionResetEvent>(MissionResetEventType) &&
           reg.template operator()<PlayerTeamChangeEvent>(PlayerTeamChangeEventType) &&
           reg.template operator()<PlayerSayEvent>(PlayerSayEventType) &&
           reg.template operator()<PlayerCommandEvent>(PlayerCommandEventType) &&
           reg.template operator()<PlayerSelectCmdrEvent>(PlayerSelectCmdrEventType) &&
           reg.template operator()<TeamObjectiveEvent>(TeamObjectiveEventType) &&
           reg.template operator()<VoiceEvent>(VoiceEventType) &&
           reg.template operator()<SoundEvent>(SoundEventType) &&
           reg.template operator()<PingPLEvent>(PingPLEventType) &&
           reg.template operator()<PlayerSkinEvent>(PlayerSkinEventType);
}

} // namespace opent1::game
