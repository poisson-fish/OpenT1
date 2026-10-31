#pragma once
#include <opent1/sim/sim_world_game.hpp>
#include <opent1/platform/tcp_stream.hpp>
#include <deque>

namespace opent1::sim {
inline constexpr SimObjectId IRCClientObjectId = 46;

struct IrcPacket {
    std::string prefix, command;
    std::vector<std::string> parameters;
};

// One complete line, excluding CRLF. No borrowed parser storage escapes.
[[nodiscard]] std::optional<IrcPacket> parseIrcPacket(std::string_view);

struct IrcPerson {
    enum Flag : UInt32 { Speaker = 1, Operator = 2, Ignore = 4, Away = 8 };

    UInt32 id{}, flags{};
    std::string nick, real, identity;
};

struct IrcMember {
    UInt32 person{}, flags{};
};

struct IrcText {
    UInt64 id{};
    std::string text;
};

struct IrcChannel {
    enum Flag : UInt32 {
        Private = 1,
        Moderated = 2,
        InviteOnly = 4,
        Limited = 8,
        NewMessage = 16,
        IgnoreExternal = 32,
        Secret = 64,
        TopicLimited = 128,
        HasKey = 256,
        New = 512
    };

    std::string name, topic, key;
    UInt32 flags{New};
    Int32 personLimit{};
    std::vector<IrcMember> members;
    std::deque<IrcText> messages;
};

struct IrcEvent {
    enum class Kind { State, Message, Invite, ChannelList, BanList, Protocol };
    Kind kind{};
    std::string channel, sender, text;
};

// Single owner thread, explicit modulo-2^32 millisecond clock, bounded queues.
// Snapshots own all values. Poll events regularly; the oldest event is evicted
// on overflow, independently of retained channel history and current state.
class IRCClient : public SimObject {
public:
    enum class State { Disconnected, Connecting, Registering, Connected, RetryWait, Failed };
    enum class Echo { None, Current, Status, All };

    struct Limits {
        std::size_t history{50}, channels{200}, people{4096}, listings{4096}, bans{4096},
            queuedBytes{65536}, bytesPerFrame{4096}, events{1024};
    };

    struct Config {
        std::string nickname{}, realname{}, room{};
        std::vector<std::string> censoredTopics{};
        bool autoReconnect{}, showJoinLeave{};
        unsigned maxRetries{4};
        UInt32 retryDelay{1000}, timeout{35000};
    };

    IRCClient();
    explicit IRCClient(Limits);
    ~IRCClient() override;
    bool configure(Config);
    [[nodiscard]] Config config() const;
    bool connect(net::IPv4Address, UInt32 now);
    // Injection seam for host transports and deterministic protocol tests.
    bool attach(std::unique_ptr<net::TcpStream>, UInt32 now);
    void disconnect();
    void process(UInt32 now);
    [[nodiscard]] State state() const;
    [[nodiscard]] std::string error() const;
    [[nodiscard]] std::vector<IrcPerson> people() const;
    [[nodiscard]] std::vector<IrcChannel> channels() const;
    [[nodiscard]] std::vector<IrcChannel> channelList() const;
    [[nodiscard]] std::vector<std::string> banList() const;
    [[nodiscard]] std::string currentChannel() const;
    bool setCurrentChannel(std::string_view);
    bool setHistorySize(std::size_t);
    void setEcho(Echo);
    [[nodiscard]] Echo echo() const;
    bool pollEvent(IrcEvent&);
    bool send(std::string_view message, std::string_view to = {});
    bool sendAction(std::string_view);
    bool nick(std::string_view);
    bool name(std::string_view);
    bool join(std::string_view);
    bool part(std::string_view);
    bool away(std::string_view);
    bool ping(std::string_view);
    bool setOperator(std::string_view);
    bool setSpeaker(std::string_view);
    bool setSpectator(std::string_view);
    bool ignore(std::string_view, bool);
    bool requestChannelList();
    bool requestBanList(std::string_view);
    bool onAdd() override;
    void onRemove() override;
    bool processEvent(const SimEvent&) override;
    static IRCClient* find(SimManager&);

private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
};

// Registers the original irc* commands. It finds the focused world's client;
// hosts create that object explicitly. Connections are never started on init.
class IRCPlugin final : public SimConsolePlugin {
public:
    IRCPlugin();
    ~IRCPlugin() override;
    void init(Console&, SimManager*) override;
    void setManager(SimManager*) override;
    void endFrame() override;

private:
    struct Binding;
    std::shared_ptr<Binding> binding_;
};
} // namespace opent1::sim
