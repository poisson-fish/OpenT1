#pragma once
#include <opent1/console/console.hpp>
#include <opent1/core/bit_stream.hpp>
#include <opent1/game/f_base.hpp>
#include <opent1/game/game_base.hpp>
#include <opent1/math/point.hpp>
#include <opent1/sim/sim_base.hpp>
#include <opent1/sim/sim_net.hpp>
#include <array>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace opent1::game {

class PlayerPSC;

class PlayerManager : public sim::SimObject {
    using Parent = sim::SimObject;

public:
    enum Constants : Int32 {
        MaxTeams = 8,
        MaxClients = 128,
        MaxCommandOrders = 8,
        MaxMsgLength = 250,
        DefaultSkinId = 0,
        ServerEventTime = 0x7FFFFFFF,
        MaxAccessProtocols = 8,
        MaxObjectives = 12,
        BaseClientId = 2048,
        CmdListSize = MaxClients / 32,
        InitialGuiMode = 0,
        CMD_NONE = 0,
        CMD_RECEIVED = 1,
        CMD_ACKNOWLEDGED = 2,
        Female = 0,
        Male = 1,
    };

    struct BaseRep {
        virtual ~BaseRep() = default;
        Int32 id{0};
        std::string name{};
        Int32 team{0};
        bool selected{false};
        bool expanded{true};
        Int32 score{0};
        std::string scoreString{};
        Int32 scoreSortValue{0};
        std::string skinBase{};
        BaseRep* nextClient{nullptr};
        BaseRep* prevClient{nullptr};
        bool isBaseRepType{false};
        UInt32 loginId{0};

        [[nodiscard]] bool isClientRep() const noexcept {
            return !isBaseRepType;
        }

        void resetBase() noexcept;
    };

    struct ClientRep : public BaseRep {
        // Vanilla maintains independent links for connected clients and for
        // all representatives (players plus AI) in the BaseRep subobject.
        ClientRep* nextClient{nullptr};
        enum CommandStatus : Int32 {
            None = CMD_NONE,
            Received = CMD_RECEIVED,
            Acknowledged = CMD_ACKNOWLEDGED,
            Ack = CMD_ACKNOWLEDGED,
            Unable = 1,
            Completed = 2,
            Clear = 3,
        };

        Int32 playerSequence{0};
        Int32 ping{0};
        Flt32 packetLoss{0.0f};
        std::string voiceBase{};
        UInt32 curGuiMode{InitialGuiMode};
        GameBase* controlObject{nullptr};
        GameBase* ownedObject{nullptr};
        PlayerPSC* playerPSC{nullptr};
        sim::network::GhostManager* ghostManager{nullptr};
        sim::network::EventManager* eventManager{nullptr};
        sim::network::PacketStream* packetStream{nullptr};

        Int32 commanderId{0};
        ClientRep* commander{nullptr};
        ClientRep* firstPeon{nullptr};
        ClientRep* nextPeon{nullptr};

        Int32 curCommand{-1};
        UInt32 curCommandSequence{};
        UInt32 lastCommandSequence{};
        UInt32 lastObjectiveSequence{};
        std::optional<std::string> lastCommandMessage;
        Int32 curCommandTarget{-1};
        Point2I wayPoint{0, 0};
        std::string curCommandString{};
        Int32 commandStatus{None};

        bool muted{false};
        UInt32 sendScores{0};
        std::string gender{};
        Int32 curRadioChannel{0};
        UInt32 listenChannels{0};

        UInt32 positionUpdateTime{0};
        Point2I position{0, 0};
        Int32 curCommander{-1};

        std::array<std::string, MaxAccessProtocols> dataFinished{};

        void clean() noexcept;
    };

    struct TeamRep : public BaseRep {
        struct Objective {
            UInt32 seqNum{0};
            std::string text{};
        };

        Flt32 energy{0.0f};
        ClientRep* firstCommander{nullptr};
        std::array<Objective, MaxObjectives> objectives{};

        void clean() noexcept;
    };

    explicit PlayerManager(bool onServer);
    ~PlayerManager() override;

    static PlayerManager* get(sim::SimManager* manager) noexcept;

    void bindConsole(Console* console) noexcept {
        boundConsole_ = console;
    }

    [[nodiscard]] Console* resolveConsole() const noexcept {
        return boundConsole_;
    }

    std::function<void(Int32, std::string_view)> onChatMessage;
    std::function<void(std::unique_ptr<sim::SimEvent>)> onLocalEvent;

    void reset();
    void missionReset();

    [[nodiscard]] bool isServer() const noexcept {
        return isServer_;
    }

    [[nodiscard]] Int32 getNumClients() const noexcept {
        return numClients_;
    }

    [[nodiscard]] Int32 getNumBaseReps() const noexcept {
        return numBaseReps_;
    }

    [[nodiscard]] Int32 getTeamCount() const noexcept {
        return numTeams_;
    }

    [[nodiscard]] Int32 getNumTeams() const noexcept {
        return numTeams_;
    }

    [[nodiscard]] TeamRep* findTeam(Int32 teamId) noexcept;
    [[nodiscard]] const TeamRep* findTeam(Int32 teamId) const noexcept;
    [[nodiscard]] TeamRep* findTeam(std::string_view teamName) noexcept;
    [[nodiscard]] const TeamRep* findTeam(std::string_view teamName) const noexcept;

    [[nodiscard]] ClientRep* findClient(Int32 clientId) noexcept;
    [[nodiscard]] const ClientRep* findClient(Int32 clientId) const noexcept;
    [[nodiscard]] ClientRep* findBaseRep(Int32 id) noexcept;
    [[nodiscard]] const ClientRep* findBaseRep(Int32 id) const noexcept;

    [[nodiscard]] ClientRep* getClientList() const noexcept {
        return clientLink_;
    }

    [[nodiscard]] BaseRep* getBaseRepList() const noexcept {
        return baseRepLink_;
    }

    [[nodiscard]] Int32 getFreeId() const noexcept;
    Int32 clientAdded(std::string_view name,
                      std::string_view voiceBase,
                      std::string_view skinBase,
                      bool gender,
                      Int32 explicitClientId = 0);
    Int32 aiAdded(std::string_view name,
                  std::string_view voiceBase,
                  std::string_view skinBase,
                  bool gender,
                  Int32 explicitClientId = 0);
    void clientDropped(Int32 clientId);
    void teamAdded(Int32 teamId);
    void teamAdded(std::string_view teamName, std::string_view skinBase = "base");
    void serverClientTeamChange(Int32 clientId, Int32 teamId);

    void setTeamScoreHeading(std::string_view heading);
    void setClientScoreHeading(std::string_view heading);

    [[nodiscard]] std::string_view teamScoreHeading() const noexcept {
        return teamScoreHeading_;
    }

    [[nodiscard]] std::string_view clientScoreHeading() const noexcept {
        return clientScoreHeading_;
    }

    void setTeamScore(Int32 teamId, std::string_view scoreString, Int32 sortValue);
    void setClientScore(Int32 clientId, std::string_view scoreString, Int32 sortValue);
    void setDisplayScores(Int32 clientId, bool display);

    void setClientSkin(Int32 clientId, std::string_view newSkin);
    void setTeamSkin(Int32 teamId, std::string_view newSkin);
    [[nodiscard]] std::string_view getTeamSkinBase(Int32 teamId) const noexcept;
    [[nodiscard]] std::string_view getPlayerSkinBase(Int32 clientId) const noexcept;

    void setTeamObjective(Int32 teamId, Int32 objNum, std::string_view text);
    void clearTeamObjectives(Int32 teamId);

    void buildCommandTrees() noexcept;
    [[nodiscard]] bool isInCommandChain(Int32 peonId, Int32 cmdrId) noexcept;
    void setCommander(Int32 peonId, Int32 cmdrId) noexcept;
    void selectCommander(Int32 peonId, Int32 cmdrId);
    void selectCommander(Int32 cmdrId);
    void issueCommand(Int32 cmdrId,
                      Int32 targId,
                      Int32 commandIcon,
                      std::string_view commandString,
                      const Point2I& wayPoint,
                      Int32 objectIndex);
    bool commandStatusReport(Int32 peonId, Int32 status, std::string_view message);

    void mute(Int32 clientId, bool muteFlag) noexcept;
    void selectClient(Int32 clientId, bool selectFlag) noexcept;
    void expandClient(Int32 clientId, bool expandFlag) noexcept;
    void clientGuiModeChanged(Int32 clientId, UInt32 newGuiMode, UInt32 oldGuiMode);

    void sendPingPackets();
    static void applyLanguageFilter(std::string& message, std::string_view badWords = "") noexcept;

    bool onAdd() override;
    void onRemove() override;
    bool processEvent(const sim::SimEvent& event) override;

private:
    Int32 addRep(std::string_view name,
                 std::string_view voiceBase,
                 std::string_view skinBase,
                 bool gender,
                 Int32 explicitClientId,
                 bool isBaseRepType);
    void linkClientRep(ClientRep* cr) noexcept;
    void unlinkClientRep(ClientRep* cr) noexcept;
    void teamAddedInternal(Int32 teamId);
    void sendPlayerEvent(ClientRep&, std::unique_ptr<sim::SimEvent>);
    void sendCommand(ClientRep& receiver,
                     const ClientRep& target,
                     const std::optional<std::string_view>& message,
                     bool statusOnly);
    void displayCommandMessage(ClientRep* speaker, Int32 type, std::string message);

    Int32 numClients_{0};
    Int32 numBaseReps_{0};
    Int32 numTeams_{0};
    std::array<ClientRep, MaxClients> clientList_{};
    std::array<TeamRep, MaxTeams + 1> teamList_{};
    ClientRep* clientLink_{nullptr};
    BaseRep* baseRepLink_{nullptr};
    ClientRep* freeList_{nullptr};
    UInt32 nextLoginId_{1};
    bool isServer_{false};
    UInt32 curObjectiveSeq_{1};
    UInt32 curCommandSequence_{};
    std::string lastCommandMessage_;
    std::string teamScoreHeading_{};
    std::string clientScoreHeading_{};
    Console* boundConsole_{nullptr};
};

// -----------------------------------------------------------------------------
// PlayerManager Network Events (PlayerManager.cpp)
// -----------------------------------------------------------------------------

struct MissionResetEvent final : sim::SimEvent {
    MissionResetEvent() noexcept {
        type = MissionResetEventType;
    }

    void pack(BitStream& stream) const noexcept {
        (void)stream;
    }

    bool unpack(BitStream& stream) noexcept {
        return stream.isValid();
    }
};

struct PingPLEvent final : sim::SimEvent {
    struct Entry {
        Int32 clientId{PlayerManager::BaseClientId};
        Int32 ping{0};
        Flt32 packetLoss{0.0f};
    };

    std::vector<Entry> entries{};

    PingPLEvent() {
        type = PingPLEventType;
    }

    PingPLEvent(Int32 cid, Int32 p, Flt32 loss) {
        type = PingPLEventType;
        entries.push_back(Entry{cid, p, loss});
    }

    void pack(BitStream& stream) const;
    bool unpack(BitStream& stream);
};

struct DeltaScoreEvent final : sim::SimEvent {
    bool isTeam{false};
    Int32 id{PlayerManager::BaseClientId};
    Int32 sortValue{0};
    std::string score{};

    DeltaScoreEvent() {
        type = DeltaScoreEventType;
    }

    void pack(BitStream& stream) const;
    bool unpack(BitStream& stream);
};

struct PlayerSkinEvent final : sim::SimEvent {
    Int32 clientId{PlayerManager::BaseClientId};
    std::string skinBase{};

    PlayerSkinEvent() {
        type = PlayerSkinEventType;
    }

    PlayerSkinEvent(Int32 cid, std::string_view skin);
    void pack(BitStream& stream) const;
    bool unpack(BitStream& stream);
};

struct PlayerSayEvent final : sim::SimEvent {
    Int32 sender{PlayerManager::BaseClientId};
    UInt32 msgType{0};
    std::string message{};

    PlayerSayEvent() {
        type = PlayerSayEventType;
    }

    void pack(BitStream& stream) const;
    bool unpack(BitStream& stream);
};

struct PlayerSelectCmdrEvent final : sim::SimEvent {
    Int32 peon{PlayerManager::BaseClientId};
    Int32 cmdr{0};

    PlayerSelectCmdrEvent() noexcept {
        type = PlayerSelectCmdrEventType;
    }

    PlayerSelectCmdrEvent(Int32 peonId, Int32 cmdrId) noexcept : peon(peonId), cmdr(cmdrId) {
        type = PlayerSelectCmdrEventType;
    }

    void pack(BitStream& stream) const;
    bool unpack(BitStream& stream);
};

struct TeamObjectiveEvent final : sim::SimEvent {
    Int32 objNum{0};
    std::string text{};

    TeamObjectiveEvent() {
        type = TeamObjectiveEventType;
    }

    TeamObjectiveEvent(Int32 oNum, std::string_view objText);
    void pack(BitStream& stream) const;
    bool unpack(BitStream& stream);
};

struct PlayerCommandEvent final : sim::SimEvent {
    Int32 commander{PlayerManager::BaseClientId};
    Int32 target{PlayerManager::BaseClientId};
    Int32 command{-1};
    Int32 commandTarget{-1};
    Point2I wayPoint{0, 0};
    Int32 status{0};
    bool statusOnly{false};
    bool hasMessage{false};
    bool sendMessageString{true};
    std::string message{};

    PlayerCommandEvent() {
        type = PlayerCommandEventType;
    }

    void pack(BitStream& stream) const;
    bool unpack(BitStream& stream);
};

struct PlayerAddEvent final : sim::SimEvent {
    Int32 id{PlayerManager::BaseClientId};
    bool isBaseRepType{false};
    std::string name{};
    std::string skinBase{};
    std::string voiceBase{};
    Int32 gender{0};
    Int32 commanderId{-1};
    Int32 team{-1};

    PlayerAddEvent() {
        type = PlayerAddEventType;
    }

    void pack(BitStream& stream) const;
    bool unpack(BitStream& stream);
};

struct TeamAddEvent final : sim::SimEvent {
    Flt32 energy{0.0f};
    Int32 id{0};
    std::string name{};
    std::string skinBase{};

    TeamAddEvent() {
        type = TeamAddEventType;
    }

    void pack(BitStream& stream) const;
    bool unpack(BitStream& stream);
};

struct VoiceEvent final : sim::SimEvent {
    Int32 sourceId{PlayerManager::BaseClientId};
    std::string soundFileName{};

    VoiceEvent() {
        type = VoiceEventType;
    }

    VoiceEvent(Int32 srcId, std::string_view file);
    void pack(BitStream& stream) const;
    bool unpack(BitStream& stream);
};

struct SoundEvent final : sim::SimEvent {
    Int32 ghostIndex{0};
    Int32 channel{0};
    Int32 soundId{0};
    DataBlockManager* dbm{nullptr};

    SoundEvent() noexcept {
        type = SoundEventType;
    }

    SoundEvent(Int32 gi, Int32 ch, Int32 sid, DataBlockManager* managerDbm = nullptr) noexcept
        : ghostIndex(gi), channel(ch), soundId(sid), dbm(managerDbm) {
        type = SoundEventType;
    }

    void pack(BitStream& stream) const;
    bool unpack(BitStream& stream);
};

struct PlayerRemoveEvent final : sim::SimEvent {
    Int32 id{PlayerManager::BaseClientId};

    PlayerRemoveEvent() noexcept {
        type = PlayerRemoveEventType;
    }

    explicit PlayerRemoveEvent(Int32 cid) noexcept : id(cid) {
        type = PlayerRemoveEventType;
    }

    void pack(BitStream& stream) const;
    bool unpack(BitStream& stream);
};

struct PlayerTeamChangeEvent final : sim::SimEvent {
    Int32 playerId{PlayerManager::BaseClientId};
    Int32 teamId{-1};

    PlayerTeamChangeEvent() noexcept {
        type = PlayerTeamChangeEventType;
    }

    PlayerTeamChangeEvent(Int32 pid, Int32 tid) noexcept : playerId(pid), teamId(tid) {
        type = PlayerTeamChangeEventType;
    }

    void pack(BitStream& stream) const;
    bool unpack(BitStream& stream);
};

bool registerPlayerManagerEvents(sim::network::Registry& registry);

} // namespace opent1::game
