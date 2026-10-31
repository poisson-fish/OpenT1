#pragma once

#include "opent1/game/shell_controls.hpp"

#include <cstdint>
#include <deque>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>
#include <filesystem>
#include <opent1/sim/sim_irc.hpp>

namespace opent1::game {

// -----------------------------------------------------------------------------
// ServerListCtrl (fearGuiServerList.cpp)
// -----------------------------------------------------------------------------
class ServerListCtrl : public FGArrayCtrl {
    using Parent = FGArrayCtrl;

public:
    enum ConnectionStatus : Int32 {
        STAT_NORESPONSE = 0,
        STAT_QUERYING = 1,
        STAT_RESPONDING = 2,
        STAT_TIME_OUT = 3,
    };

    enum ServerStatusFlags : UInt32 {
        SERVER_FAVORITE = (1U << 0),
        SERVER_PASSWORD = (1U << 1),
        SERVER_DEDICATED = (1U << 2),
        SERVER_BUDDY = (1U << 3),
    };

    enum ColumnId : Int32 {
        Favorites = 0,
        Connection = 1,
        Name = 2,
        Ping = 3,
        MissionName = 4,
        MissionType = 5,
        Players = 6,
        ServerType = 7,
        Version = 8,
        Address = 9,
        CPU = 10,
        ColumnCount = 11,
    };

    struct PlayerInfo {
        std::string name{};
        std::string tribe{};
        Int32 score{0};
        Int32 ping{0};
        Int32 team{0};
    };

    struct TeamInfo {
        std::string name{};
        Int32 score{0};
    };

    struct ServerInfo {
        Int32 status{STAT_NORESPONSE};
        UInt32 statusFlags{0};
        std::string name{};
        std::string modType{"base"};
        std::string version{"1.11"};
        std::string transportAddress{};
        bool dedicated{false};
        bool password{false};
        bool favorite{false};
        bool hasBuddy{false};
        bool neverPing{false};
        Int32 pingTime{9999};
        Int32 numPlayers{0};
        Int32 maxPlayers{32};
        Int32 cpuSpeed{450};
        std::string missionName{};
        std::string missionType{};
        std::string hostInfo{};
        std::vector<PlayerInfo> players{};
        std::vector<TeamInfo> teams{};
        std::string teamHeading{}, playerHeading{};
        std::vector<std::string> detailLines{};
    };

    ServerListCtrl();
    ~ServerListCtrl() override = default;

    void addOrUpdateServer(ServerInfo info);
    void clearServers();
    void applyFilterSpec(std::string_view spec, std::string_view buddyList = {});
    void sortServers(Int32 columnIndex, bool ascending);
    bool select(Point2I) override;
    void tick(double time) override;
    void wake() override;
    void close() override;
    bool event(const gui::Event&) override;
    std::function<std::vector<ServerInfo>()> discoverServers;
    std::function<void()> refreshServers;
    std::function<void()> saveServers;
    std::function<void(const ServerInfo&, bool)> serverSelected;
    std::function<void(const ServerInfo&)> preferenceChanged;
    bool setServerPreference(std::string_view address, bool favorite, bool neverPing);
    bool draw(gfx::SoftwareSurface&, Point2I, gui::RectI) const override;

    [[nodiscard]] static bool matchesFilter(const ServerInfo& server,
                                            std::span<const FilterCtrl::FilterInfo> conditions,
                                            std::string_view buddyList = {});

    [[nodiscard]] std::span<const ServerInfo> servers() const noexcept {
        return servers_;
    }

    [[nodiscard]] std::span<const std::size_t> visibleIndices() const noexcept {
        return visibleIndices_;
    }

    [[nodiscard]] const ServerInfo* selectedServer() const noexcept;
    std::string getCellText(Point2I cell) const override;

private:
    bool drawCell(gfx::SoftwareSurface&, Point2I, Point2I, bool, bool) const override;
    void rebuildVisibleList();

    std::vector<ServerInfo> servers_{};
    std::vector<std::size_t> visibleIndices_{};
    std::vector<FilterCtrl::FilterInfo> activeConditions_{};
    std::string activeBuddyList_{};
};

// -----------------------------------------------------------------------------
// FGMasterList (fearGuiMasterList.cpp)
// -----------------------------------------------------------------------------
class FGMasterList : public FGArrayCtrl {
    using Parent = FGArrayCtrl;

public:
    struct MasterServerEntry {
        std::string name{};
        std::string address{};
    };

    FGMasterList();
    ~FGMasterList() override = default;

    void addMasterServer(std::string_view name, std::string_view address);

    void clearMasters() {
        masters_.clear();
        setSize({1, 0});
        selected = {-1, -1};
    }

    void loadFromConsole(Console& console);

    [[nodiscard]] std::span<const MasterServerEntry> masters() const noexcept {
        return masters_;
    }

    [[nodiscard]] std::string selectedAddress() const;
    std::string getCellText(Point2I cell) const override;

private:
    std::vector<MasterServerEntry> masters_{};
};

// -----------------------------------------------------------------------------
// LobbyListCtrl (FearGuiLobbyList.cpp)
// -----------------------------------------------------------------------------
class LobbyListCtrl : public FGArrayCtrl {
    using Parent = FGArrayCtrl;

public:
    struct RoomEntry {
        std::string name{};
        std::string topic{};
        Int32 userCount{0};
    };

    LobbyListCtrl();
    ~LobbyListCtrl() override = default;

    void addRoom(std::string_view name, std::string_view topic, Int32 userCount);
    void clearRooms();

    [[nodiscard]] std::span<const RoomEntry> rooms() const noexcept {
        return rooms_;
    }

    std::string getCellText(Point2I cell) const override;

private:
    std::vector<RoomEntry> rooms_{};
};

// -----------------------------------------------------------------------------
// ServerInfoCtrl (FearGuiServerInfo.cpp) & ServerDetailCtrl (FearGuiServerDetail.cpp)
// -----------------------------------------------------------------------------
class ServerInfoCtrl : public FGArrayCtrl {
    using Parent = FGArrayCtrl;

public:
    ServerInfoCtrl();
    ~ServerInfoCtrl() override = default;

    void setServer(const ServerListCtrl::ServerInfo& server);
    std::string getCellText(Point2I cell) const override;

private:
    bool drawCell(gfx::SoftwareSurface&, Point2I, Point2I, bool, bool) const override;
    ServerListCtrl::ServerInfo currentServer_{};
};

class ServerDetailCtrl : public FGArrayCtrl {
    using Parent = FGArrayCtrl;

public:
    ServerDetailCtrl();
    ~ServerDetailCtrl() override = default;

    void setServer(const ServerListCtrl::ServerInfo& server);
    std::string getCellText(Point2I cell) const override;

private:
    ServerListCtrl::ServerInfo currentServer_{};
};

// -----------------------------------------------------------------------------
// Recordings Controls (FearGuiRecordings.cpp & FearGuiRecList.cpp)
// -----------------------------------------------------------------------------
class FGRecordingList : public FGTextList {
    using Parent = FGTextList;

public:
    struct PlaybackRep {
        std::string name{};
        bool alreadyPlayed{false};
    };

    FGRecordingList();
    ~FGRecordingList() override = default;

    void addRecording(std::string_view name);
    void clearRecordings();
    void setSelectedRecording(std::string_view name);
    [[nodiscard]] std::string getSelectedRecording();
    [[nodiscard]] std::string getSelectedRecordingName() const;
    void selectRandomRecording(UInt32 seed = 1U);

    [[nodiscard]] std::span<const PlaybackRep> recordings() const noexcept {
        return recordings_;
    }

    [[nodiscard]] std::string getCellText(Point2I cell) const override;

private:
    std::vector<PlaybackRep> recordings_{};
};

class FGRecordingPopUp : public FGComboPopUp {};

class FGRecordingComboBox : public FGStandardComboBox {
    using Parent = FGStandardComboBox;

public:
    FGRecordingComboBox() = default;
    ~FGRecordingComboBox() override = default;

    [[nodiscard]] FGRecordingList& recordingList() noexcept {
        return recordingList_;
    }

    [[nodiscard]] const FGRecordingList& recordingList() const noexcept {
        return recordingList_;
    }

    void selectRandomRecording(UInt32 seed = 1U);

private:
    FGRecordingList recordingList_{};
};

class FGRecordingListCtrl : public FGArrayCtrl {
    using Parent = FGArrayCtrl;

public:
    struct PlaybackRep {
        std::string name{};
        bool alreadyPlayed{false};
    };

    FGRecordingListCtrl();
    ~FGRecordingListCtrl() override = default;
    bool onAdd() override;
    void wake() override;
    bool refreshRecordings();

    void setRecordingDirectory(std::filesystem::path path) {
        recordingDirectory_ = std::move(path);
    }

    void addRecording(std::string_view name);
    void clearRecordings();
    bool renameSelected(std::string_view newName);
    void setSelectedRecording(std::string_view name);
    [[nodiscard]] std::string getSelectedRecording();
    [[nodiscard]] std::string getSelectedRecordingName() const;
    void selectRandomRecording(UInt32 seed = 1U);
    void selectNextRecording();

    [[nodiscard]] std::span<const PlaybackRep> recordings() const noexcept {
        return recordings_;
    }

    [[nodiscard]] std::string getCellText(Point2I cell) const;

private:
    void sortRecordings();

    std::vector<PlaybackRep> recordings_{};
    std::filesystem::path recordingDirectory_{"recordings"};
};

// -----------------------------------------------------------------------------
// IRC Client & Shell (fearGuiIRCClient.cpp & ircShell.cpp)
// -----------------------------------------------------------------------------
class IRCClient : public sim::SimObject {
    using Parent = sim::SimObject;

public:
    enum ConnectionState : UInt32 {
        IDIRC_DISCONNECTED = 0,
        IDIRC_CONNECTING_SOCKET = 1,
        IDIRC_CONNECTING_WAITING = 2,
        IDIRC_CONNECTED = 3,
    };

    enum PersonFlags : UInt32 {
        PERSON_SPEAKER = (1U << 0),
        PERSON_OPERATOR = (1U << 1),
        PERSON_IGNORE = (1U << 2),
        PERSON_AWAY = (1U << 3),
    };

    enum ChannelFlags : UInt32 {
        CHANNEL_PRIVATE = (1U << 0),
        CHANNEL_SECRET = (1U << 1),
        CHANNEL_MODERATED = (1U << 2),
        CHANNEL_INVITE = (1U << 3),
        CHANNEL_LIMITED = (1U << 4),
        CHANNEL_HAS_TOPIC = (1U << 5),
        CHANNEL_NEW_MESSAGE = (1U << 6),
        CHANNEL_TOPIC_LIMITED = (1U << 7),
        CHANNEL_HAS_KEY = (1U << 8),
    };

    struct Person {
        std::string nick{};
        std::string real{};
        std::string ident{};
        std::string pingAddress{};
        UInt32 flags{0};
    };

    struct PersonInfo {
        std::size_t personIndex{0};
        UInt32 flags{0};
    };

    struct Message {
        std::string text{};
        bool incoming{true};
    };

    struct Channel {
        std::string name{};
        std::string topic{};
        UInt32 id{0};
        UInt32 flags{0};
        UInt32 personLimit{0};
        std::string key{};
        std::vector<PersonInfo> members{};
        std::deque<Message> messages{};
    };

    IRCClient();
    ~IRCClient() override;

    bool onAdd() override;
    void onRemove() override;
    bool processEvent(const sim::SimEvent&) override;
    void process(UInt32 now);
    bool attachTransport(std::unique_ptr<net::TcpStream>,
                         UInt32 now,
                         std::string_view nick,
                         std::string_view real = "TribesPlayer");
    [[nodiscard]] std::vector<sim::IrcChannel> channelList() const;
    [[nodiscard]] std::vector<std::string> banList() const;
    bool requestBanList(std::string_view channel);

    [[nodiscard]] bool channelListReady() const {
        return channelListReady_;
    }

    [[nodiscard]] bool banListReady() const {
        return banListReady_;
    }

    void connect(std::string_view serverAddress,
                 Int32 port,
                 std::string_view nickName,
                 std::string_view realName = "TribesPlayer");
    void disconnect(std::string_view reason = "Leaving");

    [[nodiscard]] ConnectionState state() const noexcept {
        return state_;
    }

    [[nodiscard]] const std::string& nickName() const noexcept {
        return nickName_;
    }

    void setNickName(std::string_view nick);

    // Outgoing slash-command or chat text processor
    std::string sendInput(std::string_view line, std::string_view channelName = {});
    // Incoming raw IRC protocol line parser (e.g. ":nick!user@host PRIVMSG #chan :hello")
    void receiveProtocolLine(std::string_view rawLine);

    Channel* findChannel(std::string_view name) noexcept;
    const Channel* findChannel(std::string_view name) const noexcept;
    Channel& ensureChannel(std::string_view name);
    bool leaveChannel(std::string_view name);
    void setCurrentChannel(std::string_view name);
    [[nodiscard]] Channel* currentChannel() noexcept;
    [[nodiscard]] const Channel* currentChannel() const noexcept;

    [[nodiscard]] std::span<const Channel> channels() const noexcept {
        return channels_;
    }

    [[nodiscard]] std::span<const Person> people() const noexcept {
        return people_;
    }

    [[nodiscard]] std::span<const std::string> outgoingWireLog() const noexcept {
        return outgoingWireLog_;
    }

    void pushHistory(std::string_view cmd);
    [[nodiscard]] std::string prevHistory();
    [[nodiscard]] std::string nextHistory();

    static IRCClient* find(sim::SimManager* manager) noexcept;

private:
    std::size_t ensurePerson(std::string_view nick, std::string_view ident = {});
    void queueWire(std::string wire);
    void synchronizeNetwork();
    std::unique_ptr<sim::IRCClient> network_;
    std::unique_ptr<net::AddressLookup> lookup_;
    bool networkMode_{};
    bool channelListReady_{}, banListReady_{};

    ConnectionState state_{IDIRC_DISCONNECTED};
    std::string serverAddress_{};
    Int32 port_{6667};
    std::string nickName_{"TribesPlayer"};
    std::string realName_{"TribesPlayer"};
    std::vector<Person> people_{};
    std::vector<Channel> channels_{};
    std::string currentChannelName_{"STATUS"};
    UInt32 nextChannelId_{1};
    std::deque<std::string> history_{};
    std::size_t historyCursor_{0};
    std::vector<std::string> outgoingWireLog_{};
};

class IRCShell : public sim::SimObject {
    using Parent = sim::SimObject;

public:
    IRCShell() = default;
    ~IRCShell() override = default;

    static void registerConsoleCommands(Console& console, IRCClient& client);
    static void registerConsoleCommands(Console& console, std::function<IRCClient*()> client);
};

// -----------------------------------------------------------------------------
// IRC GUI Controls (FearGuiIRCView.cpp .. fearGuiIRCJoinControl.cpp)
// -----------------------------------------------------------------------------
class FGIRCView : public gui::Control {
    using Parent = gui::Control;

public:
    FGIRCView() = default;
    ~FGIRCView() override = default;
};

class FGIRCChannelCtrl : public gui::Active {
    using Parent = gui::Active;

public:
    struct TabEntry {
        std::string name{};
        UInt32 flags{0};
        Int32 width{80};
    };

    FGIRCChannelCtrl() = default;
    ~FGIRCChannelCtrl() override = default;
    std::function<IRCClient*()> resolveClient;
    void tick(double) override;
    bool event(const gui::Event&) override;
    bool draw(gfx::SoftwareSurface&, Point2I, gui::RectI) const override;

    void syncFromClient(const IRCClient& client);

    [[nodiscard]] bool canScrollLeft() const noexcept {
        return firstChannel_ > 0;
    }

    [[nodiscard]] bool canScrollRight() const noexcept;
    [[nodiscard]] bool msgWaitingLeft() const noexcept;
    [[nodiscard]] bool msgWaitingRight() const noexcept;
    void scrollLeft() noexcept;
    void scrollRight() noexcept;

    [[nodiscard]] Int32 firstChannel() const noexcept {
        return firstChannel_;
    }

    [[nodiscard]] Int32 selectedChannel() const noexcept {
        return selectedChannel_;
    }

    void selectChannel(Int32 index);
    std::shared_ptr<const gfx::BitmapFont> tabFont;

    [[nodiscard]] std::span<const TabEntry> tabs() const noexcept {
        return tabs_;
    }

private:
    std::vector<TabEntry> tabs_{};
    Int32 firstChannel_{0};
    Int32 selectedChannel_{0};
};

class FGIRCChannelList : public FGArrayCtrl {
    using Parent = FGArrayCtrl;

public:
    struct ChannelListEntry {
        std::string name{};
        Int32 userCount{0};
        std::string topic{};
    };

    FGIRCChannelList();
    ~FGIRCChannelList() override = default;
    std::function<IRCClient*()> resolveClient;
    void wake() override;
    void tick(double) override;
    bool event(const gui::Event&) override;
    void setShowAll(bool value);
    void setFilter(std::string_view value);

    void addChannelEntry(std::string_view name, Int32 userCount, std::string_view topic);
    void clearChannelEntries();

    [[nodiscard]] std::span<const ChannelListEntry> entries() const noexcept {
        return entries_;
    }

    std::string getCellText(Point2I cell) const override;

private:
    std::vector<ChannelListEntry> entries_{};
    std::string filter_;
    bool showAll_{};
};

class FGIRCChannelPropsCtrl : public gui::Control {
    using Parent = gui::Control;

public:
    FGIRCChannelPropsCtrl() = default;
    ~FGIRCChannelPropsCtrl() override = default;
    std::function<IRCClient*()> resolveClient;
    void wake() override;
    bool message(gui::Control&, UInt32) override;

    void loadFromChannel(const IRCClient::Channel& channel);
    void applyToChannel(IRCClient::Channel& channel) const;

    [[nodiscard]] const std::string& topic() const noexcept {
        return topic_;
    }

    void setTopic(std::string_view t) {
        topic_ = std::string(t);
    }

    [[nodiscard]] UInt32 flags() const noexcept {
        return flags_;
    }

    void setFlags(UInt32 f) noexcept {
        flags_ = f;
    }

    [[nodiscard]] UInt32 userLimit() const noexcept {
        return userLimit_;
    }

    void setUserLimit(UInt32 limit) noexcept {
        userLimit_ = limit;
    }

    [[nodiscard]] const std::string& key() const noexcept {
        return key_;
    }

    void setKey(std::string_view k) {
        key_ = std::string(k);
    }

private:
    std::string topic_{};
    UInt32 flags_{0};
    UInt32 userLimit_{0};
    std::string key_{};
    bool canSetTopic_{}, canSetProps_{};
};

class FGIRCNicks : public FGTextList {
    using Parent = FGTextList;

public:
    FGIRCNicks() = default;
    ~FGIRCNicks() override = default;
    std::function<IRCClient*()> resolveClient;
    void tick(double) override;

    void syncFromChannel(const IRCClient& client, const IRCClient::Channel& channel);

    [[nodiscard]] std::span<const IRCClient::Person> nicks() const noexcept {
        return nicks_;
    }

private:
    std::vector<IRCClient::Person> nicks_{};
};

class FGIRCTopic : public FGSimpleText {
    using Parent = FGSimpleText;

public:
    FGIRCTopic() = default;
    ~FGIRCTopic() override = default;
    std::function<IRCClient*()> resolveClient;
    void tick(double) override;

    void syncFromChannel(const IRCClient::Channel& channel);
};

class FGIRCTextEdit : public TestEdit {
    using Parent = TestEdit;

public:
    FGIRCTextEdit() = default;
    ~FGIRCTextEdit() override = default;
    std::function<IRCClient*()> resolveClient;
    bool event(const gui::Event&) override;

    std::string submitToClient(IRCClient& client);
};

class FGIRCActiveTextEdit : public FGIRCTextEdit {
    using Parent = FGIRCTextEdit;

public:
    FGIRCActiveTextEdit() = default;
    ~FGIRCActiveTextEdit() override = default;
};

class FGIRCActiveTextFormat : public FGTextFormat {
    using Parent = FGTextFormat;

public:
    FGIRCActiveTextFormat() = default;
    ~FGIRCActiveTextFormat() override = default;
    std::function<IRCClient*()> resolveClient;
    void tick(double) override;

    void appendChannelMessages(const IRCClient::Channel& channel);

    [[nodiscard]] std::size_t lineCount() const noexcept {
        return lines_.size();
    }

private:
    std::vector<std::string> lines_{};
};

class FGIRCScrollChannel : public gui::Active {
    using Parent = gui::Active;

public:
    FGIRCScrollChannel() = default;
    ~FGIRCScrollChannel() override = default;

    [[nodiscard]] bool scrollLeft() const noexcept {
        return mbScrollLeft_;
    }

    void setScrollLeft(bool left) noexcept {
        mbScrollLeft_ = left;
    }

    std::function<void()> checkpoint() override;
    bool inspectRead(sim::Inspect& inspector) override;
    bool inspectWrite(sim::Inspect& inspector) const override;

    Persistent::Error
    read(StreamIO& sio, Int32 version, Int32 user, Persistent::LoadContext& ctx) override;
    Persistent::Error
    write(StreamIO& sio, Int32 version, Int32 user, Persistent::StoreContext& ctx) const override;

private:
    bool mbScrollLeft_{true};
};

class FGIRCServersList : public FGComboList {};

class FGIRCServersComboBox : public FGStandardComboBox {
    using Parent = FGStandardComboBox;

public:
    FGIRCServersComboBox() = default;
    ~FGIRCServersComboBox() override = default;
};

class FGIRCBanCtrl : public gui::Control {
    using Parent = gui::Control;

public:
    FGIRCBanCtrl() = default;
    ~FGIRCBanCtrl() override = default;
    std::function<IRCClient*()> resolveClient;
    void wake() override;
    void tick(double) override;
    bool message(gui::Control&, UInt32) override;

    void addBanMask(std::string_view mask);
    bool removeBanMask(std::string_view mask);

    [[nodiscard]] std::span<const std::string> banMasks() const noexcept {
        return banMasks_;
    }

private:
    std::vector<std::string> banMasks_{};
    bool ready_{};
};

class FGIRCHelpCtrl : public gui::Control {
    using Parent = gui::Control;

public:
    FGIRCHelpCtrl() = default;
    ~FGIRCHelpCtrl() override = default;
};

class FGIRCJoinCtrl : public gui::Control {
    using Parent = gui::Control;

public:
    FGIRCJoinCtrl() = default;
    ~FGIRCJoinCtrl() override = default;
    std::function<IRCClient*()> resolveClient;
    void wake() override;
    void tick(double) override;
    bool message(gui::Control&, UInt32) override;

    [[nodiscard]] const std::string& channelToJoin() const noexcept {
        return channelToJoin_;
    }

    void setChannelToJoin(std::string_view chan) {
        channelToJoin_ = std::string(chan);
    }

    [[nodiscard]] const std::string& password() const noexcept {
        return password_;
    }

    void setPassword(std::string_view pass) {
        password_ = std::string(pass);
    }

private:
    std::string channelToJoin_{};
    std::string password_{};
};

} // namespace opent1::game
