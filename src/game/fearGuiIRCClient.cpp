#include "opent1/game/server_irc_controls.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <chrono>
#include <sstream>

namespace opent1::game {

namespace {

std::string toUpperAscii(std::string_view sv) {
    std::string out(sv);
    for (auto& c : out) {
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    return out;
}

std::string_view trimSpaces(std::string_view sv) noexcept {
    while (!sv.empty() && (sv.front() == ' ' || sv.front() == '\r' || sv.front() == '\n')) {
        sv.remove_prefix(1);
    }
    while (!sv.empty() && (sv.back() == ' ' || sv.back() == '\r' || sv.back() == '\n')) {
        sv.remove_suffix(1);
    }
    return sv;
}

std::pair<std::string_view, std::string_view> splitFirstWord(std::string_view sv) noexcept {
    sv = trimSpaces(sv);
    const auto sp = sv.find(' ');
    if (sp == std::string_view::npos) {
        return {sv, {}};
    }
    return {sv.substr(0, sp), trimSpaces(sv.substr(sp + 1))};
}

} // namespace

IRCClient::IRCClient() : network_(std::make_unique<sim::IRCClient>()) {
    setId(2040);
    ensureChannel("STATUS");
}

IRCClient::~IRCClient() = default;

namespace {
UInt32 ircMilliseconds() {
    return static_cast<UInt32>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                   std::chrono::steady_clock::now().time_since_epoch())
                                   .count());
}
}

bool IRCClient::onAdd() {
    if (!Parent::onAdd()) {
        return false;
    }
    if (auto* set =
            dynamic_cast<sim::SimSet*>(getManager()->findObject(sim::FrameEndNotifySetId))) {
        return set->addObject(*this);
    }
    return true;
}

void IRCClient::onRemove() {
    disconnect();
    Parent::onRemove();
}

bool IRCClient::processEvent(const sim::SimEvent& event) {
    if (event.type == sim::EventTypeFirst + 6) {
        process(ircMilliseconds());
        return true;
    }
    return Parent::processEvent(event);
}

void IRCClient::connect(std::string_view serverAddress,
                        Int32 port,
                        std::string_view nickName,
                        std::string_view realName) {
    disconnect();
    serverAddress_ = serverAddress;
    port_ = port;
    nickName_ = nickName;
    realName_ = realName;
    networkMode_ = true;
    sim::IRCClient::Config settings{nickName_, realName_};
    if (const auto* manager = getManager(); manager && manager->console()) {
        const auto& console = *manager->console();
        settings.room = console.getVariable("IRC::room");
        settings.autoReconnect = console.getVariable("IRC::autoReconnect") == "true" ||
                                 console.getVariable("IRC::autoReconnect") == "TRUE";
        settings.showJoinLeave = console.getVariable("IRC::ShowJoinLeave") == "true" ||
                                 console.getVariable("IRC::ShowJoinLeave") == "TRUE";
        std::istringstream words(console.getVariable("IRC::censorChannel"));
        std::string word;
        while (words >> word) {
            settings.censoredTopics.push_back(std::move(word));
        }
        const auto history = console.getVariable("IRC::MsgHistory");
        std::size_t count{};
        const auto parsed = std::from_chars(history.data(), history.data() + history.size(), count);
        if (parsed.ec == std::errc{} && parsed.ptr == history.data() + history.size()) {
            (void)network_->setHistorySize(count);
        }
    }
    if (port <= 0 || port > 65535 || !network_->configure(std::move(settings))) {
        ensureChannel("STATUS").messages.push_back({"Invalid IRC connection settings", true});
        return;
    }
    const auto address = net::canonicalIPv4Endpoint(serverAddress_ + ":" + std::to_string(port));
    if (address.empty()) {
        ensureChannel("STATUS").messages.push_back({"Invalid IRC address", true});
        return;
    }
    if (const auto numeric = net::parseIPv4Address(address)) {
        (void)network_->connect(*numeric, ircMilliseconds());
        synchronizeNetwork();
    } else {
        lookup_ = net::resolveIPv4AddressAsync(address);
        state_ = lookup_ ? IDIRC_CONNECTING_SOCKET : IDIRC_DISCONNECTED;
    }
}

bool IRCClient::attachTransport(std::unique_ptr<net::TcpStream> socket,
                                UInt32 now,
                                std::string_view nick,
                                std::string_view real) {
    disconnect();
    networkMode_ = true;
    nickName_ = nick;
    realName_ = real;
    if (!network_->configure({nickName_, realName_}) || !network_->attach(std::move(socket), now)) {
        return false;
    }
    synchronizeNetwork();
    return true;
}

void IRCClient::process(UInt32 now) {
    if (!networkMode_) {
        return;
    }
    if (lookup_) {
        std::string result;
        if (!lookup_->poll(result)) {
            return;
        }
        lookup_.reset();
        const auto parsed = net::parseIPv4Address(result);
        if (!parsed || !network_->connect(*parsed, now)) {
            state_ = IDIRC_DISCONNECTED;
            ensureChannel("STATUS").messages.push_back(
                {"Could not resolve or connect to IRC server", true});
            return;
        }
    }
    network_->process(now);
    synchronizeNetwork();
}

void IRCClient::disconnect(std::string_view reason) {
    lookup_.reset();
    if (state_ != IDIRC_DISCONNECTED) {
        queueWire("QUIT :" + std::string(reason));
    }
    network_->disconnect();
    state_ = IDIRC_DISCONNECTED;
}

std::vector<sim::IrcChannel> IRCClient::channelList() const {
    return network_->channelList();
}

std::vector<std::string> IRCClient::banList() const {
    return network_->banList();
}

bool IRCClient::requestBanList(std::string_view channel) {
    banListReady_ = false;
    return network_->requestBanList(channel);
}

void IRCClient::queueWire(std::string wire) {
    if (networkMode_) {
        const auto parsed = sim::parseIrcPacket(wire);
        if (!parsed) {
            return;
        }
        const auto& args = parsed->parameters;
        bool sent{};
        if (parsed->command == "list") {
            channelListReady_ = false;
            sent = network_->requestChannelList();
        } else if (parsed->command == "nick" && args.size() == 1) {
            sent = network_->nick(args[0]);
        } else if (parsed->command == "privmsg" && args.size() == 2) {
            if (args[1].starts_with("\1ACTION ") && args[1].back() == '\1') {
                (void)network_->setCurrentChannel(args[0]);
                sent =
                    network_->sendAction(std::string_view(args[1]).substr(8, args[1].size() - 9));
            } else {
                sent = network_->send(args[1], args[0]);
            }
        } else {
            sent = network_->send("/" + wire);
        }
        if (!sent) {
            return;
        }
    }
    if (outgoingWireLog_.size() >= 1024) {
        outgoingWireLog_.erase(outgoingWireLog_.begin());
    }
    outgoingWireLog_.push_back(std::move(wire));
}

void IRCClient::synchronizeNetwork() {
    switch (network_->state()) {
        case sim::IRCClient::State::Connecting:
        case sim::IRCClient::State::RetryWait:
            state_ = IDIRC_CONNECTING_SOCKET;
            break;
        case sim::IRCClient::State::Registering:
            state_ = IDIRC_CONNECTING_WAITING;
            break;
        case sim::IRCClient::State::Connected:
            state_ = IDIRC_CONNECTED;
            break;
        default:
            state_ = IDIRC_DISCONNECTED;
            break;
    }
    const auto persons = network_->people();
    people_.clear();
    for (const auto& person : persons) {
        people_.push_back({person.nick, person.real, person.identity, {}, person.flags});
    }
    nickName_ = network_->config().nickname;
    std::vector<Channel> channels;
    for (const auto& source : network_->channels()) {
        Channel channel;
        channel.name = source.name;
        channel.topic = source.topic;
        channel.key = source.key;
        channel.personLimit = static_cast<UInt32>(std::max(0, source.personLimit));
        const auto* old = findChannel(source.name);
        channel.id = old ? old->id : nextChannelId_++;
        for (const auto [sourceFlag, guiFlag] :
             {std::pair{sim::IrcChannel::Private, CHANNEL_PRIVATE},
              {sim::IrcChannel::Secret, CHANNEL_SECRET},
              {sim::IrcChannel::Moderated, CHANNEL_MODERATED},
              {sim::IrcChannel::InviteOnly, CHANNEL_INVITE},
              {sim::IrcChannel::Limited, CHANNEL_LIMITED},
              {sim::IrcChannel::NewMessage, CHANNEL_NEW_MESSAGE},
              {sim::IrcChannel::TopicLimited, CHANNEL_TOPIC_LIMITED},
              {sim::IrcChannel::HasKey, CHANNEL_HAS_KEY}}) {
            if (source.flags & sourceFlag) {
                channel.flags |= guiFlag;
            }
        }
        if (!channel.topic.empty()) {
            channel.flags |= CHANNEL_HAS_TOPIC;
        }
        for (const auto& member : source.members) {
            const auto person = std::ranges::find(persons, member.person, &sim::IrcPerson::id);
            if (person != persons.end()) {
                channel.members.push_back(
                    {static_cast<std::size_t>(person - persons.begin()), member.flags});
            }
        }
        for (const auto& message : source.messages) {
            channel.messages.push_back({message.text, true});
        }
        channels.push_back(std::move(channel));
    }
    channels_ = std::move(channels);
    currentChannelName_ = network_->currentChannel();
    if (auto* channel = currentChannel()) {
        channel->flags &= ~CHANNEL_NEW_MESSAGE;
    }
    sim::IrcEvent event;
    while (network_->pollEvent(event)) {
        if (event.kind == sim::IrcEvent::Kind::ChannelList) {
            channelListReady_ = true;
        }
        if (event.kind == sim::IrcEvent::Kind::BanList) {
            banListReady_ = true;
        }
    }
    if (!network_->error().empty()) {
        auto& messages = ensureChannel("Status").messages;
        if (messages.empty() || messages.back().text != network_->error()) {
            messages.push_back({network_->error(), true});
        }
    }
}

void IRCClient::setNickName(std::string_view nick) {
    nickName_ = std::string(nick);
    ensurePerson(nickName_);
}

std::string IRCClient::sendInput(std::string_view line, std::string_view channelName) {
    line = trimSpaces(line);
    if (line.empty() || line.size() > 510 ||
        line.find_first_of(std::string_view("\0\r\n", 3)) != line.npos ||
        (networkMode_ && state_ != IDIRC_CONNECTED)) {
        return {};
    }
    pushHistory(line);

    const std::string targetChan =
        channelName.empty() ? currentChannelName_ : std::string(channelName);

    if (line.front() != '/') {
        const std::string wire = "PRIVMSG " + targetChan + " :" + std::string(line);
        queueWire(wire);
        auto& ch = ensureChannel(targetChan);
        ch.messages.push_back(Message{"<" + nickName_ + "> " + std::string(line), false});
        return wire;
    }

    auto [cmdRaw, rest] = splitFirstWord(line.substr(1));
    const std::string cmd = toUpperAscii(cmdRaw);
    if (cmd == "JOIN") {
        auto [chanName, key] = splitFirstWord(rest);
        if (!chanName.empty()) {
            ensureChannel(chanName);
            setCurrentChannel(chanName);
            std::string wire = "JOIN " + std::string(chanName);
            if (!key.empty()) {
                wire += " " + std::string(key);
            }
            queueWire(wire);
            return wire;
        }
    } else if (cmd == "PART" || cmd == "LEAVE") {
        const std::string_view partChan =
            rest.empty() ? std::string_view(targetChan) : splitFirstWord(rest).first;
        const std::string wire = "PART " + std::string(partChan);
        leaveChannel(partChan);
        queueWire(wire);
        return wire;
    } else if (cmd == "NICK") {
        auto [newNick, unused] = splitFirstWord(rest);
        if (!newNick.empty()) {
            setNickName(newNick);
            const std::string wire = "NICK " + std::string(newNick);
            queueWire(wire);
            return wire;
        }
    } else if (cmd == "MSG" || cmd == "PRIVMSG") {
        auto [recipient, msgBody] = splitFirstWord(rest);
        if (!recipient.empty() && !msgBody.empty()) {
            const std::string wire =
                "PRIVMSG " + std::string(recipient) + " :" + std::string(msgBody);
            queueWire(wire);
            auto& ch = ensureChannel(recipient);
            ch.messages.push_back(Message{"<" + nickName_ + "> " + std::string(msgBody), false});
            return wire;
        }
    } else if (cmd == "ME") {
        const std::string wire =
            "PRIVMSG " + targetChan + " :\x01" + "ACTION " + std::string(rest) + "\x01";
        queueWire(wire);
        auto& ch = ensureChannel(targetChan);
        ch.messages.push_back(Message{"* " + nickName_ + " " + std::string(rest), false});
        return wire;
    } else if (cmd == "TOPIC") {
        auto topicChannel = std::string_view(targetChan);
        auto topicText = rest;
        if (!rest.empty() && (rest.front() == '#' || rest.front() == '&')) {
            const auto parts = splitFirstWord(rest);
            topicChannel = parts.first;
            topicText = parts.second;
        }
        if (!topicText.empty() && topicText.front() == ':') {
            topicText.remove_prefix(1);
        }
        const std::string wire = "TOPIC " + std::string(topicChannel) +
                                 (rest.empty() ? "" : " :" + std::string(topicText));
        queueWire(wire);
        if (!topicText.empty() && !networkMode_) {
            auto& ch = ensureChannel(topicChannel);
            ch.topic = std::string(topicText);
            ch.flags |= CHANNEL_HAS_TOPIC;
        }
        return wire;
    } else if (cmd == "WHOIS") {
        auto [whoNick, unused] = splitFirstWord(rest);
        const std::string wire = "WHOIS " + std::string(whoNick);
        queueWire(wire);
        return wire;
    } else if (cmd == "AWAY") {
        const std::string wire = rest.empty() ? "AWAY" : ("AWAY :" + std::string(rest));
        queueWire(wire);
        return wire;
    } else if (cmd == "LIST") {
        const std::string wire = "LIST";
        queueWire(wire);
        return wire;
    } else if (cmd == "KICK") {
        auto [kickUser, reason] = splitFirstWord(rest);
        std::string wire = "KICK " + targetChan + " " + std::string(kickUser);
        if (!reason.empty()) {
            wire += " :" + std::string(reason);
        }
        queueWire(wire);
        return wire;
    } else if (cmd == "QUIT") {
        disconnect(rest.empty() ? "Leaving" : rest);
        return outgoingWireLog_.empty() ? std::string{} : outgoingWireLog_.back();
    }

    const std::string fallback = cmd + (rest.empty() ? "" : (" " + std::string(rest)));
    queueWire(fallback);
    return fallback;
}

void IRCClient::receiveProtocolLine(std::string_view rawLine) {
    rawLine = trimSpaces(rawLine);
    if (rawLine.empty()) {
        return;
    }

    if (rawLine.starts_with("PING ")) {
        const std::string pong = "PONG " + std::string(rawLine.substr(5));
        queueWire(pong);
        return;
    }

    std::string_view prefix{};
    std::string_view remainder = rawLine;
    if (remainder.front() == ':') {
        auto [pfx, rest] = splitFirstWord(remainder.substr(1));
        prefix = pfx;
        remainder = rest;
    }

    std::string_view senderNick = prefix;
    std::string_view senderIdent{};
    if (const auto bang = prefix.find('!'); bang != std::string_view::npos) {
        senderNick = prefix.substr(0, bang);
        senderIdent = prefix.substr(bang + 1);
    }

    auto [commandRaw, paramsRaw] = splitFirstWord(remainder);
    const std::string command = toUpperAscii(commandRaw);

    if (command == "001") {
        state_ = IDIRC_CONNECTED;
        auto& status = ensureChannel("STATUS");
        status.messages.push_back(Message{std::string(paramsRaw), true});
        return;
    }

    if (command == "JOIN") {
        std::string_view chanName = paramsRaw;
        if (!chanName.empty() && chanName.front() == ':') {
            chanName.remove_prefix(1);
        }
        auto& ch = ensureChannel(chanName);
        const std::size_t pIdx = ensurePerson(senderNick, senderIdent);
        const bool exists =
            std::any_of(ch.members.begin(), ch.members.end(), [pIdx](const PersonInfo& m) {
                return m.personIndex == pIdx;
            });
        if (!exists) {
            ch.members.push_back(PersonInfo{pIdx, 0});
        }
        ch.messages.push_back(
            Message{std::string(senderNick) + " has joined " + std::string(chanName), true});
        return;
    }

    if (command == "PART") {
        auto [chanName, reason] = splitFirstWord(paramsRaw);
        if (auto* ch = findChannel(chanName); ch != nullptr) {
            const std::size_t pIdx = ensurePerson(senderNick, senderIdent);
            std::erase_if(ch->members, [pIdx](const PersonInfo& m) {
                return m.personIndex == pIdx;
            });
            ch->messages.push_back(
                Message{std::string(senderNick) + " has left " + std::string(chanName), true});
        }
        return;
    }

    if (command == "NICK") {
        std::string_view newNick = paramsRaw;
        if (!newNick.empty() && newNick.front() == ':') {
            newNick.remove_prefix(1);
        }
        const std::size_t pIdx = ensurePerson(senderNick, senderIdent);
        people_[pIdx].nick = std::string(newNick);
        if (senderNick == nickName_) {
            nickName_ = std::string(newNick);
        }
        return;
    }

    if (command == "TOPIC" || command == "332") {
        std::string_view rest = paramsRaw;
        if (command == "332") {
            rest = splitFirstWord(rest).second;
        }
        auto [chanName, topicPart] = splitFirstWord(rest);
        if (!topicPart.empty() && topicPart.front() == ':') {
            topicPart.remove_prefix(1);
        }
        auto& ch = ensureChannel(chanName);
        ch.topic = std::string(topicPart);
        ch.flags |= CHANNEL_HAS_TOPIC;
        return;
    }

    if (command == "353") { // RPL_NAMREPLY: "<me> = #chan :@op +voice regular"
        const auto colon = paramsRaw.find(':');
        if (colon != std::string_view::npos) {
            const std::string_view header = trimSpaces(paramsRaw.substr(0, colon));
            const auto lastSp = header.rfind(' ');
            const std::string_view chanName =
                (lastSp == std::string_view::npos) ? header : header.substr(lastSp + 1);
            auto& ch = ensureChannel(chanName);
            std::string_view namesList = trimSpaces(paramsRaw.substr(colon + 1));
            while (!namesList.empty()) {
                auto [nickToken, next] = splitFirstWord(namesList);
                namesList = next;
                UInt32 memberFlags = 0;
                if (!nickToken.empty() && nickToken.front() == '@') {
                    memberFlags |= PERSON_OPERATOR;
                    nickToken.remove_prefix(1);
                } else if (!nickToken.empty() && nickToken.front() == '+') {
                    memberFlags |= PERSON_SPEAKER;
                    nickToken.remove_prefix(1);
                }
                if (!nickToken.empty()) {
                    const std::size_t pIdx = ensurePerson(nickToken);
                    people_[pIdx].flags |= memberFlags;
                    const bool exists = std::any_of(
                        ch.members.begin(), ch.members.end(), [pIdx](const PersonInfo& m) {
                            return m.personIndex == pIdx;
                        });
                    if (!exists) {
                        ch.members.push_back(PersonInfo{pIdx, memberFlags});
                    }
                }
            }
        }
        return;
    }

    if (command == "PRIVMSG" || command == "NOTICE") {
        auto [target, body] = splitFirstWord(paramsRaw);
        if (!body.empty() && body.front() == ':') {
            body.remove_prefix(1);
        }
        const std::string_view destChan =
            (!target.empty() && (target.front() == '#' || target.front() == '&'))
                ? target
                : (senderNick.empty() ? std::string_view{"STATUS"} : senderNick);
        auto& ch = ensureChannel(destChan);
        ch.messages.push_back(
            Message{"<" + std::string(senderNick) + "> " + std::string(body), true});
        if (ch.name != currentChannelName_) {
            ch.flags |= CHANNEL_NEW_MESSAGE;
        }
        return;
    }

    auto& status = ensureChannel("STATUS");
    status.messages.push_back(Message{std::string(rawLine), true});
}

IRCClient::Channel* IRCClient::findChannel(std::string_view name) noexcept {
    for (auto& ch : channels_) {
        if (toUpperAscii(ch.name) == toUpperAscii(name)) {
            return &ch;
        }
    }
    return nullptr;
}

const IRCClient::Channel* IRCClient::findChannel(std::string_view name) const noexcept {
    for (const auto& ch : channels_) {
        if (toUpperAscii(ch.name) == toUpperAscii(name)) {
            return &ch;
        }
    }
    return nullptr;
}

IRCClient::Channel& IRCClient::ensureChannel(std::string_view name) {
    if (auto* existing = findChannel(name); existing != nullptr) {
        return *existing;
    }
    Channel ch{};
    ch.name = std::string(name);
    ch.id = nextChannelId_++;
    channels_.push_back(std::move(ch));
    return channels_.back();
}

bool IRCClient::leaveChannel(std::string_view name) {
    if (name == "STATUS") {
        return false;
    }
    const auto erased = std::erase_if(channels_, [name](const Channel& ch) {
        return ch.name == name;
    });
    if (currentChannelName_ == name) {
        currentChannelName_ = "STATUS";
    }
    return erased > 0;
}

void IRCClient::setCurrentChannel(std::string_view name) {
    auto& ch = ensureChannel(name);
    ch.flags &= ~CHANNEL_NEW_MESSAGE;
    currentChannelName_ = ch.name;
    if (networkMode_) {
        (void)network_->setCurrentChannel(name);
    }
}

IRCClient::Channel* IRCClient::currentChannel() noexcept {
    return findChannel(currentChannelName_);
}

const IRCClient::Channel* IRCClient::currentChannel() const noexcept {
    return findChannel(currentChannelName_);
}

void IRCClient::pushHistory(std::string_view cmd) {
    if (cmd.empty()) {
        return;
    }
    history_.emplace_back(cmd);
    if (history_.size() > 50) {
        history_.pop_front();
    }
    historyCursor_ = history_.size();
}

std::string IRCClient::prevHistory() {
    if (history_.empty()) {
        return {};
    }
    if (historyCursor_ > 0) {
        --historyCursor_;
    }
    return history_[historyCursor_];
}

std::string IRCClient::nextHistory() {
    if (history_.empty()) {
        return {};
    }
    if (historyCursor_ + 1 < history_.size()) {
        ++historyCursor_;
        return history_[historyCursor_];
    }
    historyCursor_ = history_.size();
    return {};
}

IRCClient* IRCClient::find(sim::SimManager* manager) noexcept {
    if (manager == nullptr) {
        return nullptr;
    }
    if (auto* byName = dynamic_cast<IRCClient*>(manager->findObject("IRCClient"))) {
        return byName;
    }
    return dynamic_cast<IRCClient*>(manager->findObject(static_cast<sim::SimObjectId>(2040)));
}

std::size_t IRCClient::ensurePerson(std::string_view nick, std::string_view ident) {
    for (std::size_t i = 0; i < people_.size(); ++i) {
        if (people_[i].nick == nick) {
            if (!ident.empty()) {
                people_[i].ident = std::string(ident);
            }
            return i;
        }
    }
    Person p{};
    p.nick = std::string(nick);
    p.ident = std::string(ident);
    people_.push_back(std::move(p));
    return people_.size() - 1;
}

} // namespace opent1::game
