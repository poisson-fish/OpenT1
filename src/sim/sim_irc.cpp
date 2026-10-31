#include <opent1/sim/sim_irc.hpp>
#include <algorithm>
#include <charconv>
#include <chrono>
#include <stdexcept>

namespace opent1::sim {
namespace {
std::string fold(std::string_view text) {
    std::string result(text);
    std::ranges::transform(result, result.begin(), [](unsigned char c) {
        return char(c >= 'A' && c <= 'Z' ? c + 32 : c);
    });
    return result;
}

bool equal(std::string_view a, std::string_view b) {
    return fold(a) == fold(b);
}

bool textValid(std::string_view s, std::size_t max = 510) {
    return s.size() <= max && s.find_first_of(std::string_view("\0\r\n", 3)) == s.npos;
}

bool atom(std::string_view s) {
    return !s.empty() && textValid(s, 200) && s.find_first_of(" :,") == s.npos;
}

std::string_view take(std::string_view& text, std::string_view delimiters = " ") {
    const auto begin = text.find_first_not_of(delimiters);
    if (begin == text.npos) {
        text = {};
        return {};
    }
    text.remove_prefix(begin);
    const auto end = text.find_first_of(delimiters);
    const auto result = text.substr(0, end);
    text = end == text.npos ? std::string_view{} : text.substr(end + 1);
    return result;
}

std::string nickOf(std::string_view prefix) {
    if (!prefix.empty() && (prefix.front() == '@' || prefix.front() == '+')) {
        prefix.remove_prefix(1);
    }
    return std::string(prefix.substr(0, prefix.find('!')));
}

UInt32 millis() {
    return static_cast<UInt32>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                   std::chrono::steady_clock::now().time_since_epoch())
                                   .count());
}

Int32 integer(std::string_view text) {
    Int32 value{};
    const auto r = std::from_chars(text.data(), text.data() + text.size(), value);
    return r.ec == std::errc{} && r.ptr == text.data() + text.size() ? value : 0;
}
}

std::optional<IrcPacket> parseIrcPacket(std::string_view line) {
    if (line.empty() || !textValid(line)) {
        return {};
    }
    IrcPacket result;
    if (line.front() == ':') {
        line.remove_prefix(1);
        result.prefix = take(line);
        if (result.prefix.empty() || line.empty()) {
            return {};
        }
    }
    const auto command = take(line);
    const bool letters = !command.empty() && std::ranges::all_of(command, [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
    });
    const bool numeric = command.size() == 3 && std::ranges::all_of(command, [](char c) {
                             return c >= '0' && c <= '9';
                         });
    if (!letters && !numeric) {
        return {};
    }
    result.command = fold(command);
    // A trailing parameter consumes the remainder, including spaces and colons.
    while (!line.empty()) {
        const auto begin = line.find_first_not_of(' ');
        if (begin == line.npos) {
            break;
        }
        line.remove_prefix(begin);
        if (result.parameters.size() == 15) {
            return {};
        }
        if (line.front() == ':') {
            result.parameters.emplace_back(line.substr(1));
            break;
        }
        result.parameters.emplace_back(take(line));
    }
    return result;
}

struct IRCClient::Impl {
    Limits limits;
    Config config;
    State state{State::Disconnected};
    Echo echo{Echo::None};
    std::unique_ptr<net::TcpStream> socket;
    std::optional<net::IPv4Address> address;
    UInt32 now{}, started{}, retryAt{}, nextPerson{1};
    unsigned retries{};
    UInt64 nextMessage{};
    std::string input, output, error, current{"Status"};
    std::vector<IrcPerson> people{{0, 0, "", "", ""}};
    std::vector<IrcChannel> channels{IrcChannel{"Status", "", "", IrcChannel::New, 0, {}, {}}},
        list;
    std::vector<std::string> bans;
    std::unordered_map<UInt32, UInt32> pings;
    std::deque<IrcEvent> events;
    bool busy{}, listing{}, listingBans{};
    std::optional<UInt32> lastVersion, lastPing;

    explicit Impl(Limits l) : limits(l) {}

    void event(IrcEvent::Kind kind,
               std::string channel = {},
               std::string sender = {},
               std::string text = {}) {
        if (events.size() == limits.events) {
            events.pop_front();
        }
        events.push_back({kind, std::move(channel), std::move(sender), std::move(text)});
    }

    void change(State next) {
        if (state != next) {
            state = next;
            event(IrcEvent::Kind::State, {}, {}, std::to_string(static_cast<unsigned>(next)));
        }
    }

    void clearSession() {
        socket.reset();
        input.clear();
        output.clear();
        pings.clear();
        list.clear();
        bans.clear();
        listing = listingBans = false;
        lastVersion.reset();
        lastPing.reset();
        people.resize(1);
        people.front().flags = 0;
        std::erase_if(channels, [](const auto& c) {
            return !equal(c.name, "Status");
        });
        channels.front().members.clear();
        current = "Status";
    }

    void fail(std::string message, bool retry = true) {
        error = std::move(message);
        clearSession();
        if (retry && config.autoReconnect && address && retries < config.maxRetries) {
            ++retries;
            retryAt = now;
            change(State::RetryWait);
        } else {
            change(State::Failed);
        }
    }

    bool queue(std::string_view line) {
        if (!socket || !textValid(line) || line.empty()) {
            return false;
        }
        if (line.size() + 2 > limits.queuedBytes - output.size()) {
            fail("IRC output queue exceeded", false);
            return false;
        }
        output.append(line);
        output.append("\r\n");
        return true;
    }

    IrcPerson* person(std::string_view prefix, bool create = false) {
        const auto nick = nickOf(prefix);
        if (!atom(nick)) {
            return nullptr;
        }
        const auto found = std::ranges::find_if(people, [&](const auto& p) {
            return equal(p.nick, nick);
        });
        if (found != people.end()) {
            return &*found;
        }
        if (!create || people.size() >= limits.people || nextPerson == 0) {
            return nullptr;
        }
        const auto bang = prefix.find('!');
        people.push_back({nextPerson++,
                          0,
                          nick,
                          "",
                          bang == prefix.npos ? "" : std::string(prefix.substr(bang + 1))});
        return &people.back();
    }

    IrcChannel* channel(std::string_view name, bool create = false) {
        const auto found = std::ranges::find_if(channels, [&](const auto& c) {
            return equal(c.name, name);
        });
        if (found != channels.end()) {
            return &*found;
        }
        if (!create || !atom(name) || channels.size() >= limits.channels) {
            return nullptr;
        }
        IrcChannel c;
        c.name = name;
        if (name.front() != '#' && name.front() != '&') {
            c.flags |= IrcChannel::Private;
        }
        channels.push_back(std::move(c));
        return &channels.back();
    }

    void sort(IrcChannel& c) {
        std::ranges::sort(c.members, [&](const auto& a, const auto& b) {
            const auto af = a.flags & 3, bf = b.flags & 3;
            if (af != bf) {
                return af > bf;
            }
            const auto pa = std::ranges::find(people, a.person, &IrcPerson::id),
                       pb = std::ranges::find(people, b.person, &IrcPerson::id);
            return fold(pa->nick) < fold(pb->nick);
        });
    }

    void member(IrcChannel& c, const IrcPerson& p, UInt32 flags = 0) {
        const auto found = std::ranges::find(c.members, p.id, &IrcMember::person);
        if (found == c.members.end()) {
            c.members.push_back({p.id, flags});
        } else {
            found->flags = flags;
        }
        sort(c);
    }

    void message(std::string_view name, std::string text, std::string sender = {}) {
        auto* c = channel(name, true);
        if (!c) {
            return;
        }
        c->flags |= IrcChannel::NewMessage;
        c->messages.push_back({nextMessage++, text});
        if (c->messages.size() > limits.history) {
            c->messages.pop_front();
        }
        event(IrcEvent::Kind::Message, c->name, std::move(sender), std::move(text));
    }

    void removeChannel(std::string_view name) {
        if (equal(name, "Status")) {
            return;
        }
        if (equal(current, name)) {
            const auto found = std::ranges::find_if(channels, [&](const auto& c) {
                return equal(c.name, name);
            });
            current =
                found == channels.begin() || found == channels.end() ? "Status" : (found - 1)->name;
        }
        std::erase_if(channels, [&](const auto& c) {
            return equal(c.name, name);
        });
    }

    void mode(const IrcPacket& packet, std::size_t offset) {
        const auto& p = packet.parameters;
        if (p.size() < offset + 2) {
            return;
        }
        auto* c = channel(p[offset]);
        if (!c) {
            return;
        }
        bool enable = true;
        std::size_t argument = offset + 2;
        std::ranges::for_each(p[offset + 1], [&](char flag) {
            if (flag == '+' || flag == '-') {
                enable = flag == '+';
                return;
            }
            const bool needs =
                flag == 'o' || flag == 'v' || flag == 'b' || flag == 'k' || (flag == 'l' && enable);
            if (needs && argument == p.size()) {
                return;
            }
            const std::string arg = needs ? p[argument++] : "";
            if (flag == 'o' || flag == 'v') {
                const auto* personValue = person(arg);
                if (!personValue) {
                    return;
                }
                const auto m = std::ranges::find(c->members, personValue->id, &IrcMember::person);
                if (m != c->members.end()) {
                    const UInt32 bit = flag == 'o' ? IrcPerson::Operator : IrcPerson::Speaker;
                    m->flags = enable ? m->flags | bit : m->flags & ~bit;
                }
                return;
            }
            UInt32 bit{};
            switch (flag) {
                case 'i':
                    bit = IrcChannel::InviteOnly;
                    break;
                case 'm':
                    bit = IrcChannel::Moderated;
                    break;
                case 'n':
                    bit = IrcChannel::IgnoreExternal;
                    break;
                case 'p':
                    bit = IrcChannel::Private;
                    break;
                case 's':
                    bit = IrcChannel::Secret;
                    break;
                case 't':
                    bit = IrcChannel::TopicLimited;
                    break;
                case 'l':
                    bit = IrcChannel::Limited;
                    c->personLimit = enable ? std::max(0, integer(arg)) : 0;
                    break;
                case 'k':
                    bit = IrcChannel::HasKey;
                    c->key = enable ? arg : "";
                    break;
                default:
                    break;
            }
            c->flags = enable ? c->flags | bit : c->flags & ~bit;
        });
        sort(*c);
    }

    void ctcp(const IrcPacket& packet,
              const std::string& sender,
              const std::string& target,
              std::string_view text) {
        if (text.size() < 2 || text.back() != 1) {
            return;
        }
        text.remove_prefix(1);
        text.remove_suffix(1);
        const auto command = fold(take(text));
        if (command == "action") {
            if (packet.command == "privmsg") {
                message(target, "<f6>" + sender + " " + std::string(text), sender);
            }
            return;
        }
        if (command == "away") {
            if (auto* p = person(sender)) {
                p->flags = text.empty() ? p->flags & ~IrcPerson::Away : p->flags | IrcPerson::Away;
            }
            return;
        }
        if (command == "ping") {
            if (packet.command == "notice") {
                if (auto* p = person(sender)) {
                    const auto found = pings.find(p->id);
                    if (found != pings.end()) {
                        message(current,
                                "<f4>" + sender + " roundtrip delay: " +
                                    std::to_string((now - found->second) / 1000) + " seconds.");
                        pings.erase(found);
                    }
                }
            } else if (!lastPing || UInt32(now - *lastPing) >= 5000) {
                lastPing = now;
                (void)queue("NOTICE " + sender + " :\1PING " + std::string(text) + "\1");
            }
            return;
        }
        if (command == "version") {
            if (packet.command == "notice") {
                message(current, "<f4>" + sender + "'s version: " + std::string(text) + ".");
            } else if (!lastVersion || UInt32(now - *lastVersion) >= 5000) {
                lastVersion = now;
                (void)queue("NOTICE " + sender + " :\1VERSION Dynamix IRC Chat 1.2.0\1");
            }
        }
    }

    void dispatch(const IrcPacket& packet) {
        const auto& p = packet.parameters;
        const auto& cmd = packet.command;
        const auto sender = nickOf(packet.prefix);
        if (cmd == "ping") {
            if (!p.empty()) {
                (void)queue("PONG " + p.front() + (p.size() > 1 ? " :" + p.back() : ""));
            }
            return;
        }
        if (cmd == "error" || cmd == "465" || cmd == "468") {
            fail(p.empty() ? "IRC server error" : p.back(), false);
            return;
        }
        if (cmd == "376" || cmd == "422") {
            if (state == State::Registering) {
                change(State::Connected);
                retries = 0;
                if (!config.room.empty()) {
                    (void)queue("JOIN " + config.room + " ");
                }
            }
            return;
        }
        if (cmd == "join" && p.size() == 1) {
            auto* personValue = person(packet.prefix, true);
            if (!personValue) {
                return;
            }
            auto* c = channel(p[0], true);
            if (!c) {
                return;
            }
            member(*c, *personValue);
            if (config.showJoinLeave && !(personValue->flags & IrcPerson::Ignore)) {
                message(c->name,
                        "<f4>" + sender + " (" +
                            (personValue->identity.empty() ? sender : personValue->identity) +
                            ") has joined the conversation.",
                        sender);
            }
            if (personValue->id == 0) {
                current = c->name;
                (void)queue("MODE " + current);
            }
            return;
        }
        if (cmd == "353" && p.size() >= 4) {
            auto* c = channel(p[2]);
            if (!c) {
                return;
            }
            std::string_view names = p[3];
            while (!names.empty()) {
                const auto entry = take(names);
                auto* personValue = person(entry, true);
                if (!personValue) {
                    continue;
                }
                member(*c,
                       *personValue,
                       entry.front() == '@'   ? UInt32(IrcPerson::Operator)
                       : entry.front() == '+' ? UInt32(IrcPerson::Speaker)
                                              : 0U);
            }
            return;
        }
        if ((cmd == "privmsg" || cmd == "notice") && p.size() == 2) {
            auto* personValue = person(packet.prefix, true);
            if (!personValue || (personValue->flags & IrcPerson::Ignore)) {
                return;
            }
            const auto target = equal(p[0], people.front().nick) ? sender : p[0];
            if (!p[1].empty() && p[1].front() == 1) {
                ctcp(packet, sender, target, p[1]);
            } else if (cmd == "privmsg") {
                message(target, "<f1>" + sender + " says:<f4> " + p[1], sender);
            } else {
                message("Status", "<f4>NOTICE FROM " + packet.prefix + ": " + p[1], sender);
            }
            return;
        }
        if (cmd == "nick" && p.size() == 1 && atom(p[0])) {
            auto* personValue = person(packet.prefix);
            if (!personValue) {
                return;
            }
            if (auto* collision = person(p[0]); collision && collision != personValue) {
                return;
            }
            if (!(personValue->flags & IrcPerson::Ignore)) {
                message(current, "<f4>" + sender + " is now known as " + p[0] + ".");
            }
            if (auto* c = channel(sender); c && (c->flags & IrcChannel::Private)) {
                if (equal(current, c->name)) {
                    current = p[0];
                }
                c->name = p[0];
            }
            personValue->nick = p[0];
            if (personValue->id == 0) {
                config.nickname = p[0];
            }
            std::ranges::for_each(channels, [&](auto& c) {
                sort(c);
            });
            return;
        }
        if ((cmd == "part" && !p.empty()) || (cmd == "kick" && p.size() >= 2)) {
            const auto target = cmd == "kick" ? p[1] : sender;
            auto* personValue = person(target);
            auto* c = channel(p[0]);
            if (!personValue || !c) {
                return;
            }
            const auto id = personValue->id;
            const bool memberPresent =
                std::ranges::find(c->members, id, &IrcMember::person) != c->members.end();
            if (memberPresent && cmd == "part" && config.showJoinLeave &&
                !(personValue->flags & IrcPerson::Ignore)) {
                message(c->name,
                        "<f4>" + target + " (" +
                            (personValue->identity.empty() ? target : personValue->identity) +
                            ") has left the conversation.",
                        target);
            }
            if (memberPresent && cmd == "kick") {
                message(id == 0 ? "Status" : current,
                        "<f5>Host " + sender + " kicks " + target +
                            " out of the chat room, saying \"" + (p.size() > 2 ? p[2] : "") +
                            "\".");
                c = channel(p[0]);
            }
            std::erase_if(c->members, [&](const auto& m) {
                return m.person == id;
            });
            if (id == 0) {
                removeChannel(p[0]);
            }
            return;
        }
        if (cmd == "quit") {
            const auto* personValue = person(sender);
            if (!personValue || personValue->id == 0) {
                return;
            }
            const auto id = personValue->id;
            if (channel(sender) && !(personValue->flags & IrcPerson::Ignore)) {
                message(sender,
                        "<F4>" + sender + " (" +
                            (personValue->identity.empty() ? sender : personValue->identity) +
                            ") has disconnected from IRC.");
            }
            std::ranges::for_each(channels, [&](auto& c) {
                std::erase_if(c.members, [&](const auto& m) {
                    return m.person == id;
                });
            });
            std::erase_if(people, [&](const auto& v) {
                return v.id == id;
            });
            pings.erase(id);
            return;
        }
        if ((cmd == "topic" && p.size() == 2) || (cmd == "332" && p.size() == 3)) {
            if (auto* c = channel(p[p.size() - 2])) {
                c->topic = p.back();
            }
            return;
        }
        if (cmd == "331" && p.size() >= 2) {
            if (auto* c = channel(p[1])) {
                c->topic.clear();
            }
            return;
        }
        if (cmd == "mode" || cmd == "324") {
            mode(packet, cmd == "324" ? 1 : 0);
            return;
        }
        if (cmd == "352" && p.size() >= 8) {
            if (auto* v = person(p[5])) {
                v->identity = p[2] + "@" + p[3];
                const auto split = p[7].find(' ');
                v->real = split == p[7].npos ? "" : p[7].substr(split + 1);
            }
            return;
        }
        if (cmd == "311" && p.size() >= 6) {
            if (auto* v = person(p[1])) {
                v->identity = p[2] + "@" + p[3];
                v->real = p.back();
            }
            return;
        }
        if (cmd == "322" && p.size() >= 4 && listing && list.size() < limits.listings) {
            const auto topic = fold(p[3]);
            if (std::ranges::any_of(config.censoredTopics, [&](const auto& word) {
                    return topic.find(fold(word)) != topic.npos;
                })) {
                return;
            }
            IrcChannel c;
            c.name = p[1];
            c.topic = p[3];
            c.personLimit = integer(p[2]);
            list.push_back(std::move(c));
            return;
        }
        if (cmd == "323") {
            listing = false;
            event(IrcEvent::Kind::ChannelList);
            return;
        }
        if (cmd == "367" && p.size() >= 3 && listingBans && bans.size() < limits.bans) {
            bans.push_back(p[2]);
            return;
        }
        if (cmd == "368") {
            listingBans = false;
            event(IrcEvent::Kind::BanList);
            return;
        }
        if (cmd == "invite" && p.size() == 2) {
            const auto* personValue = person(packet.prefix, true);
            if (personValue && !(personValue->flags & IrcPerson::Ignore)) {
                event(IrcEvent::Kind::Invite, p[1], sender);
            }
            return;
        }
        if (cmd == "305" || cmd == "306") {
            people.front().flags = cmd == "306" ? people.front().flags | IrcPerson::Away
                                                : people.front().flags & ~IrcPerson::Away;
        }
        if (!p.empty()) {
            event(IrcEvent::Kind::Protocol, {}, sender, p.back());
            if (cmd == "372" || cmd == "433" || cmd == "301") {
                message("Status", "<f4>" + p.back(), sender);
            }
        }
    }
};

IRCClient::IRCClient() : IRCClient(Limits{}) {}

IRCClient::IRCClient(Limits limits) : impl_(std::make_shared<Impl>(limits)) {
    if (limits.history < 50 || limits.history > 500 || !limits.channels || limits.channels > 200 ||
        limits.people < 1 || limits.people > 65536 || !limits.listings || limits.listings > 65536 ||
        !limits.bans || limits.bans > 65536 || limits.queuedBytes < 1024 ||
        limits.queuedBytes > 1048576 || !limits.bytesPerFrame || limits.bytesPerFrame > 65536 ||
        !limits.events || limits.events > 65536) {
        throw std::invalid_argument("invalid IRC limits");
    }
    setId(IRCClientObjectId);
}

IRCClient::~IRCClient() = default;

bool IRCClient::configure(Config config) {
    if (!atom(config.nickname) || !textValid(config.realname, 200) || config.realname.empty() ||
        (!config.room.empty() && !atom(config.room)) || config.censoredTopics.size() > 64 ||
        !std::ranges::all_of(config.censoredTopics,
                             [](const auto& s) {
                                 return !s.empty() && textValid(s, 100);
                             }) ||
        config.maxRetries > 16 || !config.retryDelay || config.retryDelay > 60000 ||
        !config.timeout || config.timeout > 300000) {
        return false;
    }
    if (impl_->state != State::Disconnected && impl_->state != State::Failed) {
        return false;
    }
    impl_->config = std::move(config);
    impl_->people.front().nick = impl_->config.nickname;
    impl_->people.front().real = impl_->config.realname;
    return true;
}

IRCClient::Config IRCClient::config() const {
    return impl_->config;
}

bool IRCClient::attach(std::unique_ptr<net::TcpStream> socket, UInt32 now) {
    if (!socket || impl_->config.nickname.empty()) {
        return false;
    }
    disconnect();
    impl_->address.reset();
    impl_->socket = std::move(socket);
    impl_->now = impl_->started = now;
    impl_->change(State::Connecting);
    return true;
}

bool IRCClient::connect(net::IPv4Address address, UInt32 now) {
    if (!address.port || impl_->config.nickname.empty()) {
        return false;
    }
    disconnect();
    impl_->address = address;
    impl_->now = impl_->started = now;
    try {
        impl_->socket = net::makeTcpConnection(address);
        impl_->change(State::Connecting);
        return true;
    } catch (const std::runtime_error& e) {
        impl_->fail(e.what());
        return false;
    }
}

void IRCClient::disconnect() {
    impl_->clearSession();
    impl_->retries = 0;
    impl_->error.clear();
    impl_->change(State::Disconnected);
}

void IRCClient::process(UInt32 now) {
    const auto s = impl_;
    if (s->busy) {
        return;
    }
    s->busy = true;

    struct Guard {
        bool& b;

        ~Guard() {
            b = false;
        }
    } guard{s->busy};

    s->now = now;
    if (s->state == State::RetryWait) {
        if (UInt32(now - s->retryAt) < s->config.retryDelay) {
            return;
        }
        try {
            s->socket = net::makeTcpConnection(*s->address);
            s->started = now;
            s->change(State::Connecting);
        } catch (const std::runtime_error& e) {
            s->fail(e.what());
            return;
        }
    }
    if (!s->socket) {
        return;
    }
    if ((s->state == State::Connecting || s->state == State::Registering) &&
        UInt32(now - s->started) > s->config.timeout) {
        s->fail("IRC connection timed out", false);
        return;
    }
    if (s->state == State::Connecting) {
        const auto status = s->socket->connected();
        if (status == net::Result::WouldBlock) {
            return;
        }
        if (status != net::Result::Ok) {
            s->fail("IRC connection failed");
            return;
        }
        s->change(State::Registering);
        if (!s->queue("USER " + s->config.nickname + " ignore ignore :" + s->config.realname) ||
            !s->queue("NICK " + s->config.nickname)) {
            return;
        }
    }
    // Partial writes retain their suffix; bounded work prevents a busy peer from
    // starving simulation. Sending first also bounds server response latency.
    std::size_t sent{};
    while (s->socket && !s->output.empty() && sent < s->limits.bytesPerFrame) {
        const auto bytes = std::as_bytes(std::span(
            s->output.data(), std::min(s->output.size(), s->limits.bytesPerFrame - sent)));
        const auto result = s->socket->write(bytes);
        if (result.result == net::Result::WouldBlock) {
            break;
        }
        if (result.result != net::Result::Ok || !result.count || result.count > bytes.size()) {
            s->fail("IRC send failed");
            return;
        }
        sent += result.count;
        s->output.erase(0, result.count);
    }
    std::array<std::byte, 1024> bytes{};
    std::size_t received{};
    while (s->socket && received < s->limits.bytesPerFrame) {
        const auto buffer =
            std::span(bytes).first(std::min(bytes.size(), s->limits.bytesPerFrame - received));
        const auto result = s->socket->read(buffer);
        if (result.result == net::Result::WouldBlock) {
            break;
        }
        if (result.result != net::Result::Ok || !result.count || result.count > buffer.size()) {
            s->fail("IRC connection dropped");
            return;
        }
        received += result.count;
        std::ranges::for_each(std::span(bytes).first(result.count), [&](auto byte) {
            if (!s->socket) {
                return;
            }
            const char c = char(std::to_integer<UInt8>(byte));
            if (c == '\n') {
                auto line = std::move(s->input);
                s->input.clear();
                if (!line.empty() && line.back() == '\r') {
                    line.pop_back();
                }
                if (line.empty()) {
                    return;
                }
                const auto packet = parseIrcPacket(line);
                if (!packet) {
                    s->fail("Invalid IRC line", false);
                    return;
                }
                s->dispatch(*packet);
            } else if (c == '\0' || s->input.size() >= 511 ||
                       (!s->input.empty() && s->input.back() == '\r')) {
                s->fail("Invalid or oversized IRC line", false);
            } else {
                s->input.push_back(c);
            }
        });
    }
}

IRCClient::State IRCClient::state() const {
    return impl_->state;
}

std::string IRCClient::error() const {
    return impl_->error;
}

std::vector<IrcPerson> IRCClient::people() const {
    return impl_->people;
}

std::vector<IrcChannel> IRCClient::channels() const {
    return impl_->channels;
}

std::vector<IrcChannel> IRCClient::channelList() const {
    return impl_->list;
}

std::vector<std::string> IRCClient::banList() const {
    return impl_->bans;
}

std::string IRCClient::currentChannel() const {
    return impl_->current;
}

bool IRCClient::setCurrentChannel(std::string_view name) {
    if (const auto* c = impl_->channel(name)) {
        impl_->current = c->name;
        return true;
    }
    return false;
}

bool IRCClient::setHistorySize(std::size_t size) {
    if (size < 50 || size > 500) {
        return false;
    }
    impl_->limits.history = size;
    std::ranges::for_each(impl_->channels, [&](auto& c) {
        while (c.messages.size() > size) {
            c.messages.pop_front();
        }
    });
    return true;
}

void IRCClient::setEcho(Echo echo) {
    impl_->echo = echo;
}

IRCClient::Echo IRCClient::echo() const {
    return impl_->echo;
}

bool IRCClient::pollEvent(IrcEvent& event) {
    if (impl_->events.empty()) {
        return false;
    }
    event = std::move(impl_->events.front());
    impl_->events.pop_front();
    return true;
}

bool IRCClient::send(std::string_view message, std::string_view to) {
    if (impl_->state != State::Connected || message.empty() || !textValid(message)) {
        return false;
    }
    if (message.front() == '/') {
        message.remove_prefix(1);
        auto params = message;
        const auto command = fold(take(params));
        if (command == "join") {
            return join(params);
        }
        if (command == "part") {
            return part(params);
        }
        if (command == "away") {
            return away(params);
        }
        if (command == "me" || command == "action") {
            return sendAction(params);
        }
        if (command == "ping") {
            return ping(params);
        }
        if (command == "quit") {
            const bool result = impl_->queue("QUIT");
            process(impl_->now);
            disconnect();
            return result;
        }
        return impl_->queue(message);
    }
    const auto target = to.empty() ? impl_->current : std::string(to);
    if (!atom(target) || equal(target, "Status")) {
        return false;
    }
    if (!impl_->queue("PRIVMSG " + target + " :" + std::string(message))) {
        return false;
    }
    impl_->message(target,
                   "<f2>" + impl_->people.front().nick + " says:<f4> " + std::string(message),
                   impl_->people.front().nick);
    return true;
}

bool IRCClient::sendAction(std::string_view text) {
    if (impl_->state != State::Connected || text.empty() || !textValid(text) ||
        equal(impl_->current, "Status")) {
        return false;
    }
    if (!impl_->queue("PRIVMSG " + impl_->current + " :\1ACTION " + std::string(text) + "\1")) {
        return false;
    }
    impl_->message(impl_->current, "<f6>" + impl_->people.front().nick + " " + std::string(text));
    return true;
}

bool IRCClient::nick(std::string_view text) {
    if (!atom(text)) {
        return false;
    }
    if (impl_->state == State::Connected || impl_->state == State::Registering) {
        return equal(text, impl_->people.front().nick) || impl_->queue("NICK " + std::string(text));
    }
    impl_->config.nickname = text;
    impl_->people.front().nick = text;
    return true;
}

bool IRCClient::name(std::string_view text) {
    if (text.empty() || !textValid(text, 200)) {
        return false;
    }
    impl_->config.realname = text;
    impl_->people.front().real = text;
    return true;
}

bool IRCClient::join(std::string_view text) {
    if (impl_->state != State::Connected || !textValid(text)) {
        return false;
    }
    auto channel = take(text), key = take(text);
    channel = channel.substr(0, channel.find(','));
    key = key.substr(0, key.find(','));
    if (!atom(channel) || (!key.empty() && !atom(key))) {
        return false;
    }
    if (impl_->channel(channel)) {
        return setCurrentChannel(channel);
    }
    return impl_->queue("JOIN " + std::string(channel) + " " + std::string(key));
}

bool IRCClient::part(std::string_view text) {
    if (!textValid(text)) {
        return false;
    }
    auto rest = text;
    const auto name = take(rest);
    const auto* c = impl_->channel(name);
    if (!c || equal(name, "Status")) {
        return false;
    }
    if (c->flags & IrcChannel::Private) {
        impl_->removeChannel(name);
        return true;
    }
    return impl_->state == State::Connected && impl_->queue("PART " + std::string(text));
}

bool IRCClient::away(std::string_view text) {
    if (impl_->state != State::Connected || !textValid(text)) {
        return false;
    }
    const auto a = text.empty() ? "AWAY" : "AWAY " + std::string(text);
    const auto b = "PRIVMSG " + impl_->current + " :\1AWAY" +
                   (text.empty() ? "" : " " + std::string(text)) + "\1";
    if (a.size() + b.size() + 4 > impl_->limits.queuedBytes - impl_->output.size() ||
        b.size() > 510) {
        return false;
    }
    if (!impl_->queue(a) || !impl_->queue(b)) {
        return false;
    }
    auto& flags = impl_->people.front().flags;
    flags = text.empty() ? flags & ~IrcPerson::Away : flags | IrcPerson::Away;
    return true;
}

bool IRCClient::ping(std::string_view text) {
    if (impl_->state != State::Connected || !textValid(text)) {
        return false;
    }
    bool any = false;
    while (!text.empty()) {
        const auto nick = take(text);
        if (const auto* p = impl_->person(nick)) {
            if (!impl_->queue("PRIVMSG " + p->nick + " :\1PING 0\1")) {
                return false;
            }
            impl_->pings[p->id] = impl_->now;
            any = true;
        }
    }
    return any;
}

bool IRCClient::setOperator(std::string_view text) {
    return impl_->state == State::Connected && atom(text) && impl_->person(text) &&
           impl_->queue("MODE " + impl_->current + " +o " + std::string(text));
}

bool IRCClient::setSpeaker(std::string_view text) {
    if (impl_->state != State::Connected || !atom(text)) {
        return false;
    }
    const auto* p = impl_->person(text);
    const auto* c = impl_->channel(impl_->current);
    if (!p || !c) {
        return false;
    }
    const auto member = std::ranges::find(c->members, p->id, &IrcMember::person);
    if (member == c->members.end()) {
        return false;
    }
    if ((member->flags & IrcPerson::Operator) &&
        !impl_->queue("MODE " + impl_->current + " -o " + p->nick)) {
        return false;
    }
    return impl_->queue("MODE " + impl_->current + " +v " + p->nick);
}

bool IRCClient::setSpectator(std::string_view text) {
    if (impl_->state != State::Connected || !atom(text)) {
        return false;
    }
    const auto* p = impl_->person(text);
    const auto* c = impl_->channel(impl_->current);
    if (!p || !c) {
        return false;
    }
    const auto member = std::ranges::find(c->members, p->id, &IrcMember::person);
    if (member == c->members.end()) {
        return false;
    }
    if ((member->flags & IrcPerson::Operator) &&
        !impl_->queue("MODE " + impl_->current + " -o " + p->nick)) {
        return false;
    }
    return !(member->flags & IrcPerson::Speaker) ||
           impl_->queue("MODE " + impl_->current + " -v " + p->nick);
}

bool IRCClient::ignore(std::string_view name, bool value) {
    if (auto* p = impl_->person(name)) {
        p->flags = value ? p->flags | IrcPerson::Ignore : p->flags & ~IrcPerson::Ignore;
        return true;
    }
    return false;
}

bool IRCClient::requestChannelList() {
    if (impl_->state != State::Connected || !impl_->queue("LIST")) {
        return false;
    }
    impl_->list.clear();
    impl_->listing = true;
    return true;
}

bool IRCClient::requestBanList(std::string_view channel) {
    if (impl_->state != State::Connected || !atom(channel) ||
        !impl_->queue("MODE " + std::string(channel) + " +b")) {
        return false;
    }
    impl_->bans.clear();
    impl_->listingBans = true;
    return true;
}

bool IRCClient::onAdd() {
    if (!getManager()) {
        return false;
    }
    if (auto* set = dynamic_cast<SimSet*>(getManager()->findObject(FrameEndNotifySetId))) {
        return set->addObject(*this);
    }
    return true;
}

void IRCClient::onRemove() {
    disconnect();
}

bool IRCClient::processEvent(const SimEvent& event) {
    if (event.type == EventTypeFirst + 6) {
        process(millis());
        return true;
    }
    return SimObject::processEvent(event);
}

IRCClient* IRCClient::find(SimManager& manager) {
    return dynamic_cast<IRCClient*>(manager.findObject(IRCClientObjectId));
}
} // namespace opent1::sim
