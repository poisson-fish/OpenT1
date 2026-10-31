#include "test_support.hpp"
#include "test_registry.hpp"
#include <opent1/sim/sim_telnet.hpp>
#include <chrono>
#include <thread>

using namespace opent1;

namespace {
class TelnetPeer {
public:
    explicit TelnetPeer(sim::SimTelnetPlugin& plugin)
        : plugin_(plugin), socket_(net::makeTcpConnection(*plugin.address())) {
        wait([&] {
            return output.find("Enter Password:") != output.npos;
        });
        output.clear();
    }

    template <class Fn> void wait(Fn&& ready) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (!ready()) {
            CHECK(std::chrono::steady_clock::now() < deadline);
            plugin_.endFrame();
            std::array<std::byte, 4096> buffer{};
            const auto result = socket_->read(buffer);
            CHECK(result.result == net::Result::Ok || result.result == net::Result::WouldBlock);
            for (const auto byte : std::span(buffer).first(result.count)) {
                output += char(std::to_integer<UInt8>(byte));
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }

    void send(std::string_view text) {
        wait([&] {
            if (text.empty()) {
                return true;
            }
            const auto result = socket_->write(std::as_bytes(std::span(text)));
            CHECK(result.result == net::Result::Ok || result.result == net::Result::WouldBlock);
            text.remove_prefix(result.count);
            return text.empty();
        });
    }

    void rejected(std::string_view password) {
        output.clear();
        send(std::string(password) + "\n");
        wait([&] {
            return output.find("Enter Password:") != output.npos;
        });
    }

    std::string output;

private:
    sim::SimTelnetPlugin& plugin_;
    std::unique_ptr<net::TcpStream> socket_;
};

void authentication() {
    for (const bool explicitEmpty : {false, true}) {
        Console console;
        unsigned commands{};
        console.addCommand(0, "telnetProbe", [&](Console&, Int32, auto) -> std::string {
            ++commands;
            return {};
        });
        if (explicitEmpty) {
            console.setVariable("TelnetPassword", "");
        }
        sim::SimTelnetPlugin plugin;
        plugin.init(console, nullptr);
        CHECK(console.getVariable("TelnetBindAddress") == "127.0.0.1");
        CHECK(plugin.listen());
        const auto port = plugin.address()->port;
        plugin.stop();
        console.setVariable("TelnetPort", std::to_string(port));
        CHECK(plugin.address() == net::IPv4Address{{127, 0, 0, 1}, port});
        // Hosting on another interface still requires an explicit selection.
        console.setVariable("TelnetBindAddress", "0.0.0.0");
        CHECK(plugin.address() == net::IPv4Address{{0, 0, 0, 0}, port});
        console.setVariable("TelnetBindAddress", "127.0.0.1");
        CHECK(plugin.address() == net::IPv4Address{{127, 0, 0, 1}, port});
        TelnetPeer peer(plugin);
        peer.rejected("");
        peer.rejected("telnetProbe();");
        CHECK(commands == 0);
        console.setVariable("TelnetPassword", "secret");
        peer.rejected("wrong");
        CHECK(commands == 0);
        peer.send("secret\ntelnetProbe();\n");
        peer.wait([&] {
            return commands == 1;
        });
        console.setVariable("TelnetPort", "0");
        CHECK(!plugin.address() && plugin.connections() == 0);
    }
}

int telnetAuthentication() {
    return test::run(authentication);
}
}

OPENT1_REGISTER_TEST("simulation", "telnet_authentication", telnetAuthentication);
