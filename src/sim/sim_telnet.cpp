#include <opent1/sim/sim_telnet.hpp>
#include <algorithm>
#include <charconv>
#include <stdexcept>

namespace opent1::sim {
struct SimTelnetPlugin::State {
    struct Client {
        std::unique_ptr<net::TcpStream> socket;
        std::string line, output;
        unsigned attempts{};
        bool authenticated{}, closing{}, dead{};
    };

    Limits limits;
    Console* console{};
    std::unique_ptr<net::TcpListener> listener;
    std::vector<std::shared_ptr<Client>> clients;
    bool busy{};
    std::array<UInt8, 4> bindHost{127, 0, 0, 1};

    explicit State(Limits l) : limits(l) {}

    void queue(Client& client, std::string_view text) const {
        if (client.dead) {
            return;
        }
        if (text.size() > limits.queuedBytes - client.output.size()) {
            client.dead = true;
            client.socket->close();
            return;
        }
        client.output.append(text);
    }

    bool listen(net::IPv4Address address) {
        try {
            auto next = net::makeTcpListener(address);
            listener = std::move(next);
            return true;
        } catch (const std::runtime_error&) {
            return false;
        }
    }

    void stop() {
        listener.reset();
        std::ranges::for_each(clients, [](const auto& c) {
            c->dead = true;
            c->socket->close();
        });
        clients.clear();
    }
};

SimTelnetPlugin::SimTelnetPlugin() : SimTelnetPlugin(Limits{}) {}

SimTelnetPlugin::SimTelnetPlugin(Limits limits) : state_(std::make_shared<State>(limits)) {
    if (!limits.clients || limits.clients > 1024 || !limits.lineBytes || limits.lineBytes > 65535 ||
        limits.queuedBytes < 256 || limits.queuedBytes > 1048576 || !limits.bytesPerFrame ||
        limits.bytesPerFrame > 65536) {
        throw std::invalid_argument("invalid telnet limits");
    }
}

SimTelnetPlugin::~SimTelnetPlugin() {
    stop();
}

bool SimTelnetPlugin::listen(net::IPv4Address address) {
    return state_->console && state_->listen(address);
}

void SimTelnetPlugin::stop() {
    state_->stop();
}

std::size_t SimTelnetPlugin::connections() const {
    return state_->clients.size();
}

std::optional<net::IPv4Address> SimTelnetPlugin::address() const {
    return state_->listener ? std::optional(state_->listener->address()) : std::nullopt;
}

void SimTelnetPlugin::init(Console& console, SimManager*) {
    if (state_->console) {
        throw std::logic_error("telnet plugin already initialized");
    }
    state_->console = &console;
    const std::weak_ptr<State> weak = state_;
    output_ = console.subscribeOutput([weak](std::string_view line) {
        if (const auto state = weak.lock()) {
            const auto clients = state->clients;
            std::ranges::for_each(clients, [&](const auto& client) {
                if (client->authenticated && !client->closing) {
                    state->queue(*client, line);
                    state->queue(*client, std::string_view("\0\r\n", 3));
                }
            });
        }
    });
    console.bindVariable(
        "TelnetBindAddress",
        [weak] {
            const auto state = weak.lock();
            if (!state) {
                return std::string("127.0.0.1");
            }
            auto text = net::formatIPv4Address({state->bindHost, 1});
            text.erase(text.rfind(':'));
            return text.substr(3);
        },
        [weak](std::string_view text) {
            const auto state = weak.lock();
            if (!state) {
                return;
            }
            const auto address = net::parseIPv4Address(std::string(text) + ":1");
            if (!address) {
                return;
            }
            if (state->listener && state->listener->address().host != address->host) {
                const auto previous = state->listener->address();
                state->listener.reset();
                if (!state->listen({address->host, previous.port})) {
                    (void)state->listen(previous);
                    return;
                }
            }
            state->bindHost = address->host;
        });
    console.bindVariable(
        "TelnetPort",
        [weak] {
            const auto state = weak.lock();
            return state && state->listener ? std::to_string(state->listener->address().port) : "0";
        },
        [weak](std::string_view text) {
            const auto state = weak.lock();
            if (!state) {
                return;
            }
            Int32 port{};
            const auto r = std::from_chars(text.data(), text.data() + text.size(), port);
            if (r.ec != std::errc{} || r.ptr != text.data() + text.size() || port < -1 ||
                port > 65535) {
                return;
            }
            if (port <= 0) {
                state->stop();
            } else if (!state->listener || state->listener->address().port != port) {
                (void)state->listen({state->bindHost, UInt16(port)});
            }
        });
}

void SimTelnetPlugin::endFrame() {
    const auto state = state_;
    if (!state->console || state->busy) {
        return;
    }
    state->busy = true;

    struct Guard {
        bool& b;

        ~Guard() {
            b = false;
        }
    } guard{state->busy};

    if (state->listener) {
        // Bound accept work even when already at capacity.
        std::size_t accepted{};
        while (accepted++ < 8 && state->listener) {
            std::unique_ptr<net::TcpStream> socket;
            const auto result = state->listener->accept(socket);
            if (result != net::Result::Ok) {
                break;
            }
            if (state->clients.size() >= state->limits.clients) {
                socket->close();
                continue;
            }
            auto client = std::make_shared<State::Client>();
            client->socket = std::move(socket);
            constexpr std::string_view banner = "SimEngine Telnet Provider\r\n\r\nEnter Password:";
            state->queue(*client, banner);
            state->queue(*client, std::string_view("\0", 1));
            state->clients.push_back(std::move(client));
        }
    }
    const auto clients = state->clients;
    std::ranges::for_each(clients, [&](const auto& client) {
        if (client->dead) {
            return;
        }
        std::array<std::byte, 1024> bytes{};
        std::size_t received{};
        while (!client->dead && !client->closing && received < state->limits.bytesPerFrame) {
            const auto result = client->socket->read(std::span(bytes).first(
                std::min(bytes.size(), state->limits.bytesPerFrame - received)));
            if (result.result == net::Result::WouldBlock) {
                break;
            }
            if (result.result != net::Result::Ok || !result.count) {
                client->dead = true;
                break;
            }
            received += result.count;
            std::ranges::for_each(std::span(bytes).first(result.count), [&](auto b) {
                if (client->dead || client->closing) {
                    return;
                }
                const char c = char(std::to_integer<UInt8>(b));
                if (c == '\r') {
                    return;
                }
                if (c == '\n') {
                    auto line = std::move(client->line);
                    client->line.clear();
                    if (client->authenticated) {
                        state->queue(*client, "\r\n");
                        (void)state->console->evaluate(line, "<telnet>");
                        if (!client->dead) {
                            state->queue(*client, state->console->getVariable("Console::Prompt"));
                        }
                    } else if (const auto password = state->console->getVariable("TelnetPassword");
                               !password.empty() && line == password) {
                        client->authenticated = true;
                        state->queue(*client, "\r\n");
                        state->queue(*client, state->console->getVariable("Console::Prompt"));
                    } else {
                        if (++client->attempts > 3) {
                            state->queue(*client, "Too many tries... cya.");
                            client->closing = true;
                        } else {
                            state->queue(*client, "Nope... try agian.\r\nEnter Password:\r\n");
                        }
                    }
                } else if (c == '\b' || c == 127) {
                    if (!client->line.empty()) {
                        client->line.pop_back();
                        if (client->authenticated) {
                            state->queue(*client, "\b \b");
                        }
                    }
                } else if (c == '\0' || client->line.size() >= state->limits.lineBytes) {
                    client->dead = true; // Never evaluate a silently truncated command.
                } else {
                    client->line.push_back(c);
                    if (client->authenticated) {
                        state->queue(*client, std::string_view(&c, 1));
                    }
                }
            });
        }
        if (!client->dead && !client->output.empty()) {
            const auto data =
                std::as_bytes(std::span(client->output.data(), client->output.size()));
            const auto result = client->socket->write(
                data.first(std::min(data.size(), state->limits.bytesPerFrame)));
            if (result.result == net::Result::Ok) {
                client->output.erase(0, result.count);
            } else if (result.result != net::Result::WouldBlock) {
                client->dead = true;
            }
        }
        if (client->dead || (client->closing && client->output.empty())) {
            client->dead = true;
            client->socket->close();
        }
    });
    std::erase_if(state->clients, [](const auto& c) {
        return c->dead;
    });
}
}
