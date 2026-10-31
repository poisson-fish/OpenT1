#include "opent1/game/server_irc_controls.hpp"
#include <opent1/console/console.hpp>

#include <charconv>

namespace opent1::game {

void IRCShell::registerConsoleCommands(Console& console, IRCClient& client) {
    registerConsoleCommands(console, [&client] {
        return &client;
    });
}

void IRCShell::registerConsoleCommands(Console& console, std::function<IRCClient*()> resolve) {
    console.addCommand(
        0,
        "ircConnect",
        [resolve](Console& con,
                  Int32 /*id*/,
                  std::span<const std::string> argv) -> std::optional<std::string> {
            auto* target = resolve();
            if (!target) {
                return "false";
            }
            auto& client = *target;
            const std::string_view host =
                argv.size() > 1 ? std::string_view(argv[1]) : std::string_view{"irc.dynamix.com"};
            Int32 port = 6667;
            if (argv.size() > 2) {
                std::from_chars(argv[2].data(), argv[2].data() + argv[2].size(), port);
            }
            const std::string nick = argv.size() > 3 ? argv[3] : con.getVariable("IRC::nickname");
            const auto real = con.getVariable("IRC::realname");
            client.connect(host,
                           port,
                           nick.empty() ? client.nickName() : nick,
                           real.empty() ? "TribesPlayer" : real);
            return client.state() != IRCClient::IDIRC_DISCONNECTED ? "true" : "false";
        });

    console.addCommand(0,
                       "ircDisconnect",
                       [resolve](Console& /*con*/,
                                 Int32 /*id*/,
                                 std::span<const std::string> argv) -> std::optional<std::string> {
                           auto* target = resolve();
                           if (!target) {
                               return "false";
                           }
                           auto& client = *target;
                           const std::string_view reason = argv.size() > 1
                                                               ? std::string_view(argv[1])
                                                               : std::string_view{"Leaving"};
                           client.disconnect(reason);
                           return "true";
                       });

    console.addCommand(0,
                       "ircSend",
                       [resolve](Console& /*con*/,
                                 Int32 /*id*/,
                                 std::span<const std::string> argv) -> std::optional<std::string> {
                           auto* target = resolve();
                           if (!target) {
                               return "false";
                           }
                           auto& client = *target;
                           if (argv.size() <= 1) {
                               return std::string{};
                           }
                           return client.sendInput(argv[1]);
                       });

    console.addCommand(
        0,
        "ircListChannels",
        [resolve](Console& /*con*/,
                  Int32 /*id*/,
                  std::span<const std::string> /*argv*/) -> std::optional<std::string> {
            auto* target = resolve();
            if (!target) {
                return "false";
            }
            auto& client = *target;
            return client.sendInput("/list");
        });

    console.addCommand(0,
                       "ircEcho",
                       [resolve](Console& /*con*/,
                                 Int32 /*id*/,
                                 std::span<const std::string> argv) -> std::optional<std::string> {
                           auto* target = resolve();
                           if (!target) {
                               return "false";
                           }
                           auto& client = *target;
                           if (argv.size() > 1) {
                               if (auto* ch = client.currentChannel(); ch != nullptr) {
                                   ch->messages.push_back(IRCClient::Message{argv[1], true});
                               }
                           }
                           return std::string{};
                       });
}

} // namespace opent1::game
