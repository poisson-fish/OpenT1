#pragma once
#include <opent1/sim/sim_world_game.hpp>
#include <opent1/platform/tcp_stream.hpp>

namespace opent1::sim {
// Legacy line-oriented console service, driven by endFrame. The supplied
// Console must outlive this plugin, as with the other SimConsolePlugins.
// Authentication requires a nonempty $TelnetPassword.
class SimTelnetPlugin : public SimConsolePlugin {
public:
    struct Limits {
        std::size_t clients{16}, lineBytes{1023}, queuedBytes{65536}, bytesPerFrame{4096};
    };

    SimTelnetPlugin();
    explicit SimTelnetPlugin(Limits);
    ~SimTelnetPlugin() override;
    void init(Console&, SimManager*) override;
    void endFrame() override;
    // Loopback by default, including $TelnetPort listeners. Explicitly supply
    // a different interface here or through $TelnetBindAddress when hosting.
    // Port zero requests an ephemeral port; $TelnetPort=0 stops the listener.
    bool listen(net::IPv4Address = {{127, 0, 0, 1}, 0});
    void stop();
    [[nodiscard]] std::optional<net::IPv4Address> address() const;
    [[nodiscard]] std::size_t connections() const;

private:
    struct State;
    std::shared_ptr<State> state_;
    std::shared_ptr<Console::Output> output_;
};
}
