#include <opent1/sim/sim_irc.hpp>
#include <algorithm>
#include <charconv>
#include <chrono>
#include <stdexcept>

namespace opent1::sim {
namespace {
enum Command {
    Connect,
    Disconnect,
    Nick,
    Name,
    Send,
    Who,
    ListChannels,
    ListPeople,
    SetChannel,
    SetEcho
};

std::string fold(std::string_view value) {
    std::string s(value);
    std::ranges::transform(s, s.begin(), [](char c) {
        return char(c >= 'A' && c <= 'Z' ? c + 32 : c);
    });
    return s;
}

bool truth(std::string_view s) {
    return fold(s) == "true" || s == "1";
}

UInt32 millis() {
    return static_cast<UInt32>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                   std::chrono::steady_clock::now().time_since_epoch())
                                   .count());
}
}

struct IRCPlugin::Binding {
    SimManager* manager{};
    Console* console{};

    std::optional<std::string> command(Int32 id, std::span<const std::string> args) {
        if (!manager) {
            return "False";
        }
        std::optional<net::IPv4Address> address;
        IRCClient::Config config;
        // Console getters/setters are arbitrary host callbacks: read them before
        // borrowing a world. They may replace the focused world or this plugin.
        if (id == Connect) {
            if (args.size() != 2 && args.size() != 3) {
                return "False";
            }
            UInt16 port = 6667;
            if (args.size() == 3) {
                unsigned value{};
                const auto r =
                    std::from_chars(args[2].data(), args[2].data() + args[2].size(), value);
                if (r.ec != std::errc{} || r.ptr != args[2].data() + args[2].size() || !value ||
                    value > 65535) {
                    return "False";
                }
                port = UInt16(value);
            }
            address = net::parseIPv4Address("IP:" + args[1] + ":" + std::to_string(port));
            if (!address) {
                return "False";
            }
            config.nickname = console->getVariable("IRC::nickname");
            config.realname = console->getVariable("IRC::realname");
            config.room = console->getVariable("IRC::room");
            config.autoReconnect = truth(console->getVariable("IRC::autoReconnect"));
            config.showJoinLeave = truth(console->getVariable("IRC::ShowJoinLeave"));
            const auto censor = console->getVariable("IRC::censorChannel");
            std::string_view remaining = censor;
            while (!remaining.empty()) {
                const auto begin = remaining.find_first_not_of(" ,");
                if (begin == remaining.npos) {
                    break;
                }
                remaining.remove_prefix(begin);
                const auto end = remaining.find_first_of(" ,");
                config.censoredTopics.emplace_back(remaining.substr(0, end));
                if (config.censoredTopics.size() > 64) {
                    return "False";
                }
                remaining = end == remaining.npos ? std::string_view{} : remaining.substr(end + 1);
            }
            console->setVariable("IRC::server", args[1]);
            console->setVariable("IRC::serverPort", std::to_string(port));
        }
        if (!manager) {
            return "False";
        }
        auto* target = IRCClient::find(*manager);
        if (!target) {
            return "False";
        }
        bool success = false;
        std::vector<std::string> lines;
        std::optional<std::pair<std::string, std::string>> variable;
        manager->dispatch([&] {
            const auto size = args.size();
            if (id == Connect) {
                target->disconnect();
                if (!target->configure(std::move(config))) {
                    return;
                }
                success = target->connect(*address, millis());
                return;
            }
            if (id == Disconnect && size == 1) {
                target->disconnect();
                success = true;
            } else if (id == Nick && size == 2) {
                success = target->nick(args[1]);
                if (success) {
                    variable = {{"IRC::nickname", target->config().nickname}};
                }
            } else if (id == Name && size == 2) {
                success = target->name(args[1]);
                if (success) {
                    variable = {{"IRC::realname", args[1]}};
                }
            } else if (id == Send && (size == 2 || size == 3)) {
                success = target->send(args[1],
                                       size == 3 ? std::string_view(args[2]) : std::string_view{});
            } else if (id == Who && size <= 2) {
                success = target->send(size == 2 ? "/WHO " + args[1]
                                                 : "/WHO " + target->currentChannel());
            } else if (id == SetChannel && size == 2) {
                success = target->setCurrentChannel(args[1]);
            } else if (id == SetEcho && size == 2) {
                const auto value = fold(args[1]);
                target->setEcho(value == "all"       ? IRCClient::Echo::All
                                : value == "current" ? IRCClient::Echo::Current
                                : value == "status"  ? IRCClient::Echo::Status
                                                     : IRCClient::Echo::None);
                success = true;
            } else if (id == ListChannels && size == 1) {
                lines.push_back("IRC Channels:");
                std::ranges::for_each(target->channels(), [&](const auto& c) {
                    lines.push_back("  " + c.name);
                });
                success = true;
            } else if (id == ListPeople && (size == 1 || size == 2)) {
                const auto people = target->people();
                if (size == 1) {
                    lines.push_back("IRC People:");
                    std::ranges::for_each(people, [&](const auto& p) {
                        lines.push_back("  " + p.nick + " (" + p.identity + ") -- " + p.real);
                    });
                    success = true;
                } else {
                    const auto channels = target->channels();
                    const auto c = std::ranges::find_if(channels, [&](const auto& value) {
                        return fold(value.name) == fold(args[1]);
                    });
                    if (c == channels.end()) {
                        return;
                    }
                    lines.push_back("IRC People on: " + args[1]);
                    std::ranges::for_each(c->members, [&](const auto& m) {
                        const auto p = std::ranges::find(people, m.person, &IrcPerson::id);
                        if (p != people.end()) {
                            lines.push_back("  " +
                                            std::string(m.flags & IrcPerson::Operator  ? "@"
                                                        : m.flags & IrcPerson::Speaker ? "+"
                                                                                       : " ") +
                                            p->nick + " (" + p->identity + ") -- " + p->real);
                        }
                    });
                    success = true;
                }
            }
        });
        // All output values are owned before an output sink can remove objects,
        // the plugin, or its world. No borrowed target is used after this point.
        if (variable) {
            console->setVariable(variable->first, variable->second);
        }
        std::ranges::for_each(lines, [&](const auto& line) {
            console->print(line);
        });
        return success ? "True" : "False";
    }
};

IRCPlugin::IRCPlugin() : binding_(std::make_shared<Binding>()) {}

IRCPlugin::~IRCPlugin() {
    binding_->manager = nullptr;
}

void IRCPlugin::init(Console& console, SimManager* manager) {
    if (binding_->console) {
        throw std::logic_error("IRC plugin already initialized");
    }
    binding_->console = &console;
    binding_->manager = manager;
    const std::weak_ptr<Binding> weak = binding_;
    std::ranges::for_each(
        std::to_array<std::pair<Int32, std::string_view>>({{Connect, "ircConnect"},
                                                           {Disconnect, "ircDisconnect"},
                                                           {Nick, "ircNick"},
                                                           {Name, "ircName"},
                                                           {Send, "ircSend"},
                                                           {Who, "ircWho"},
                                                           {ListChannels, "ircListChannels"},
                                                           {ListPeople, "ircListPeople"},
                                                           {SetChannel, "ircSetChannel"},
                                                           {SetEcho, "ircEcho"}}),
        [&](const auto& item) {
            console.addCommand(item.first,
                               item.second,
                               [weak](Console&, Int32 id, std::span<const std::string> args)
                                   -> std::optional<std::string> {
                                   const auto binding = weak.lock();
                                   return binding ? binding->command(id, args)
                                                  : std::optional<std::string>("False");
                               });
        });
}

void IRCPlugin::setManager(SimManager* manager) {
    binding_->manager = manager;
}

void IRCPlugin::endFrame() {
    const auto binding = binding_;
    if (!binding->manager || !binding->console) {
        return;
    }
    const auto history = binding->console->getVariable("IRC::MsgHistory");
    std::size_t historySize{};
    const auto parsed =
        std::from_chars(history.data(), history.data() + history.size(), historySize);
    if (!binding->manager) {
        return;
    }
    auto* client = IRCClient::find(*binding->manager);
    if (!client) {
        return;
    }
    std::vector<std::string> output;
    std::optional<IrcEvent> invitation;
    std::string nickname;
    binding->manager->dispatch([&] {
        if (parsed.ec == std::errc{} && parsed.ptr == history.data() + history.size()) {
            (void)client->setHistorySize(historySize);
        }
        // SimWorld's frame notification drives network IO. This pass drains
        // owned notifications, applies echo and publishes invite/nickname state.
        const auto echo = client->echo();
        const auto current = fold(client->currentChannel());
        IrcEvent event;
        while (client->pollEvent(event)) {
            if (event.kind == IrcEvent::Kind::Message &&
                (echo == IRCClient::Echo::All ||
                 (echo == IRCClient::Echo::Status && fold(event.channel) == "status") ||
                 (echo == IRCClient::Echo::Current && fold(event.channel) == current))) {
                output.push_back(std::move(event.text));
            } else if (event.kind == IrcEvent::Kind::Invite) {
                invitation = event;
            }
        }
        nickname = client->config().nickname;
    });
    binding->console->setVariable("IRC::nickname", nickname);
    if (invitation) {
        binding->console->setVariable("IRC::InviteChannel", invitation->channel);
        binding->console->setVariable("IRC::InvitePerson", invitation->sender);
    }
    std::ranges::for_each(output, [&](const auto& line) {
        binding->console->print(line);
    });
}
} // namespace opent1::sim
