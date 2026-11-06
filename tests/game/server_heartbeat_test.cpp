#include "test_registry.hpp"
#include "test_support.hpp"
#include <opent1/game/game_app.hpp>
#include <algorithm>
#include <chrono>
#include <fstream>
#include <thread>

namespace {
using namespace opent1;
using Browser = sim::network::ServerBrowser;

struct HostedServer {
    std::filesystem::path root =
        std::filesystem::temp_directory_path() /
        std::format("opent1-heartbeat-{}",
                    std::chrono::steady_clock::now().time_since_epoch().count());
    std::unique_ptr<game::FearGameApp> app;

    HostedServer(std::string_view master,
                 bool dedicated = true,
                 bool publicHost = true,
                 bool heartbeats = true,
                 UInt16 finalPort = 0) {
        std::filesystem::create_directories(root / "assets/base");
        {
            std::ofstream script(root / "assets/console.cs");
            // The bootstrap itself stays headless even when it creates a
            // non-dedicated hosted world. Preferences select the final port
            // after newServer, matching installation startup ordering.
            script << "$dedicated = true; $Server::Port = 0;"
                   << "$Server::HostPublicGame = " << publicHost << ";"
                   << "$Server::Heartbeats = " << heartbeats << ";"
                   << "$Server::numMasters = 1; $Server::MasterAddressN0 = \"Test\\t" << master
                   << "\"; newServer(\"\", " << dedicated << "); focusServer();"
                   << "$Server::Port = " << finalPort << ";";
            CHECK(bool(script));
        }
        game::FearGameApp::Config config;
        config.assetRoot = root / "assets";
        config.writeRoot = root / "writes";
        app = std::make_unique<game::FearGameApp>(config);
        CHECK(app->init());
        CHECK(app->fearPlugin().serverGlobals().csDelegate->config().dedicated == dedicated);
    }

    ~HostedServer() {
        app.reset();
        std::error_code ec;
        std::filesystem::remove_all(root, ec);
    }

    sim::network::CSDelegate& delegate() {
        auto* gameDelegate = app->fearPlugin().serverGlobals().csDelegate;
        CHECK(gameDelegate && gameDelegate->delegate());
        return *gameDelegate->delegate();
    }

    UInt16 port() {
        const auto address = net::parseIPv4Address(delegate().localAddress());
        CHECK(address && address->port);
        return address->port;
    }

    void process(UInt32 now) {
        CHECK(delegate().process(now) == net::Result::Ok);
    }
};

void expectHeartbeats(net::Transport& master, UInt16 sourcePort, unsigned expected) {
    unsigned count{};
    const auto deadline =
        std::chrono::steady_clock::now() +
        (expected ? std::chrono::milliseconds(5000) : std::chrono::milliseconds(20));
    auto quietUntil = deadline;
    do {
        net::Datagram datagram;
        while (master.receive(datagram) == net::Result::Ok) {
            const auto packet = Browser::Packet::decode(datagram.data());
            CHECK(packet && packet->type == Browser::Type::Heartbeat);
            CHECK(datagram.size == 8 && packet->number == 255 && packet->total == 0 &&
                  packet->key == 0 && packet->id == 0);
            const auto source = net::parseIPv4Address(datagram.address);
            CHECK(source && source->host == std::array<UInt8, 4>{127, 0, 0, 1} &&
                  source->port == sourcePort);
            CHECK(++count <= expected);
            if (count == expected) {
                quietUntil = std::chrono::steady_clock::now() + std::chrono::milliseconds(20);
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    } while (std::chrono::steady_clock::now() < quietUntil);
    CHECK(count == expected);
}

void hostingPreferences() {
    for (const bool dedicated : {false, true}) {
        for (const bool publicHost : {false, true}) {
            for (const bool heartbeats : {false, true}) {
                auto master = net::makeUdpTransport({{127, 0, 0, 1}, 0});
                HostedServer server(master->localAddress(), dedicated, publicHost, heartbeats);
                server.process(0);
                const unsigned enabled = publicHost && heartbeats ? 1u : 0u;
                expectHeartbeats(*master, server.port(), enabled);
                server.process(1);
                server.process(119999);
                expectHeartbeats(*master, server.port(), 0);
                server.process(120000);
                expectHeartbeats(*master, server.port(), enabled);
            }
        }
    }
}

void reconfigureHeartbeats() {
    auto first = net::makeUdpTransport({{127, 0, 0, 1}, 0});
    auto second = net::makeUdpTransport({{127, 0, 0, 1}, 0});
    HostedServer server(first->localAddress());
    auto& console = server.app->console();
    server.process(0);
    expectHeartbeats(*first, server.port(), 1);
    // Unchanged preferences and unrelated query callbacks must not restart
    // the interval on every tick.
    server.process(1);
    server.delegate().browser().getPingInfo();
    server.process(119999);
    expectHeartbeats(*first, server.port(), 0);
    server.process(120000);
    expectHeartbeats(*first, server.port(), 1);
    UInt32 changeTime = 240000;
    for (const auto preference : {"Server::HostPublicGame", "Server::Heartbeats"}) {
        console.setVariable(preference, "false");
        server.process(changeTime++);
        expectHeartbeats(*first, server.port(), 0);
        console.setVariable(preference, "true");
        server.process(changeTime++);
        expectHeartbeats(*first, server.port(), 1);
    }
    console.setVariable("Server::MasterAddressN0", "Changed\t" + second->localAddress());
    console.setVariable("Server::numMasters", "4");
    console.setVariable("Server::MasterAddressN1", "invalid/address");
    console.setVariable("Server::MasterAddressN2", "");
    console.setVariable("Server::MasterAddressN3", second->localAddress());
    server.process(changeTime);
    expectHeartbeats(*first, server.port(), 0);
    expectHeartbeats(*second, server.port(), 1);
    console.setVariable("Server::numMasters", "0");
    server.process(360002);
    expectHeartbeats(*second, server.port(), 0);
    console.setVariable("Server::numMasters", "1");
    console.setVariable("OpenT1::ConsoleCSExecuted", "false");
    server.process(360003);
    expectHeartbeats(*second, server.port(), 0);
    console.setVariable("OpenT1::ConsoleCSExecuted", "true");
    server.process(360004);
    expectHeartbeats(*second, server.port(), 1);
    server.app->fearPlugin().destroyServerWorld();
    expectHeartbeats(*second, 0, 0);
    CHECK(server.app->fearPlugin().createServerWorld({}, false));
    server.process(360005);
    expectHeartbeats(*second, server.port(), 1);
}

void hostnameMastersAndFinalPort() {
    auto reservation = net::makeUdpTransport({{0, 0, 0, 0}, 0});
    const auto selectedPort = net::parseIPv4Address(reservation->localAddress())->port;
    reservation.reset();
    auto master = net::makeUdpTransport({{127, 0, 0, 1}, 0});
    const auto masterPort = net::parseIPv4Address(master->localAddress())->port;
    HostedServer server(
        "IP:localhost:" + std::to_string(masterPort), true, true, true, selectedPort);
    CHECK(server.port() == selectedPort);
    // Pump DNS completion without advancing the heartbeat clock.
    net::Datagram heartbeat;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    do {
        CHECK(std::chrono::steady_clock::now() < deadline);
        server.process(0);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    } while (master->receive(heartbeat) == net::Result::WouldBlock);
    const auto packet = Browser::Packet::decode(heartbeat.data());
    CHECK(packet && packet->type == Browser::Type::Heartbeat);
    CHECK(net::parseIPv4Address(heartbeat.address)->port == selectedPort);
    expectHeartbeats(*master, selectedPort, 0);
    // A master can ping/query the exact socket that announced itself.
    Browser browser(*master, false);
    bool pinged{}, queried{};
    browser.gotPingInfo = [&](const auto&) {
        pinged = true;
    };
    browser.gotGameInfo = [&](const auto&) {
        queried = true;
    };
    CHECK(browser.ping(heartbeat.address) && browser.query(heartbeat.address));
    while (!pinged || !queried) {
        CHECK(std::chrono::steady_clock::now() < deadline);
        browser.process(1);
        server.process(1);
        net::Datagram datagram;
        while (master->receive(datagram) == net::Result::Ok) {
            CHECK(browser.receive(datagram.address, datagram.data(), 1));
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    server.process(120000);
    expectHeartbeats(*master, selectedPort, 1);
    // Cancel a newly queued DNS lookup before it can announce.
    auto& console = server.app->console();
    console.setVariable("Server::MasterAddressN0", "localhost:" + std::to_string(masterPort));
    console.setVariable("Server::numMasters", "0");
    server.process(120001);
    console.setVariable("Server::numMasters", "1");
    server.process(120002);
    console.setVariable("Server::Heartbeats", "false");
    for (UInt32 now : {120003u, 240003u, 360003u}) {
        server.process(now);
    }
    expectHeartbeats(*master, selectedPort, 0);
}

void manyHostnameMasters() {
    // More configured masters than the native resolver's eight worker slots
    // must eventually announce to every destination without blocking a tick.
    std::array<std::unique_ptr<net::Transport>, 10> masters;
    for (auto& master : masters) {
        master = net::makeUdpTransport({{127, 0, 0, 1}, 0});
    }
    HostedServer server(masters.front()->localAddress());
    auto& console = server.app->console();
    console.setVariable("Server::numMasters", "10");
    for (std::size_t i = 0; i < masters.size(); ++i) {
        const auto port = net::parseIPv4Address(masters[i]->localAddress())->port;
        console.setVariable("Server::MasterAddressN" + std::to_string(i),
                            "Master\tlocalhost:" + std::to_string(port));
    }
    std::array<bool, 10> received{};
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!std::ranges::all_of(received, [](bool value) {
        return value;
    })) {
        CHECK(std::chrono::steady_clock::now() < deadline);
        server.process(0);
        for (std::size_t i = 0; i < masters.size(); ++i) {
            net::Datagram datagram;
            if (masters[i]->receive(datagram) == net::Result::Ok) {
                CHECK(!received[i]);
                const auto packet = Browser::Packet::decode(datagram.data());
                CHECK(packet && packet->type == Browser::Type::Heartbeat);
                CHECK(net::parseIPv4Address(datagram.address)->port == server.port());
                received[i] = true;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

void heartbeatClockWrap() {
    auto master = net::makeUdpTransport({{127, 0, 0, 1}, 0});
    HostedServer server(master->localAddress());
    server.process(0xffffffffu - 100u);
    expectHeartbeats(*master, server.port(), 1);
    server.process(119898);
    expectHeartbeats(*master, server.port(), 0);
    server.process(119899);
    expectHeartbeats(*master, server.port(), 1);
}
}

int opent1_test_server_heartbeats() {
    return opent1::test::run([] {
        hostingPreferences();
        reconfigureHeartbeats();
        hostnameMastersAndFinalPort();
        manyHostnameMasters();
        heartbeatClockWrap();
    });
}

OPENT1_REGISTER_TEST("game", "server_heartbeats", opent1_test_server_heartbeats);
