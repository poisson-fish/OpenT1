#include <opent1/sim/sim_net_browser.hpp>
#include <algorithm>
#include <ranges>
#include <stdexcept>

namespace opent1::sim::network {
namespace {
constexpr std::size_t MaxData = 1492;
constexpr UInt32 HeartbeatInterval = 120000;

UInt8 byte(std::byte b) {
    return std::to_integer<UInt8>(b);
}

UInt16 shortWord(std::span<const std::byte> b) {
    return UInt16(byte(b[0]) | (UInt16(byte(b[1])) << 8));
}

bool broadcast(std::string_view address) {
    if (const auto ipx = net::parseIpxAddress(address)) {
        return std::ranges::all_of(ipx->node, [](auto byte) {
            return byte == 255;
        });
    }
    const auto a = net::parseIPv4Address(address);
    return a && a->host == std::array<UInt8, 4>{255, 255, 255, 255};
}

struct Guard {
    bool& busy;

    explicit Guard(bool& b) : busy(b) {
        busy = true;
    }

    ~Guard() {
        busy = false;
    }
};
}

std::vector<std::byte> ServerBrowser::Packet::encode() const {
    if (data.size() > MaxData || UInt8(type) < 3 || UInt8(type) > 8) {
        return {};
    }
    std::vector<std::byte> bytes{std::byte{16},
                                 std::byte(type),
                                 std::byte(number),
                                 std::byte(total),
                                 std::byte(key & 255),
                                 std::byte(key >> 8),
                                 std::byte(id & 255),
                                 std::byte(id >> 8)};
    bytes.insert(bytes.end(), data.begin(), data.end());
    return bytes;
}

std::optional<ServerBrowser::Packet>
ServerBrowser::Packet::decode(std::span<const std::byte> bytes) {
    if (bytes.size() < 8 || bytes.size() > 1500 || byte(bytes[0]) != 16 || byte(bytes[1]) < 3 ||
        byte(bytes[1]) > 8) {
        return {};
    }
    return Packet{Type(byte(bytes[1])),
                  byte(bytes[2]),
                  byte(bytes[3]),
                  shortWord(bytes.subspan(4, 2)),
                  shortWord(bytes.subspan(6, 2)),
                  {bytes.begin() + 8, bytes.end()}};
}

ServerBrowser::ServerBrowser(net::Transport& t, bool s) : ServerBrowser(t, s, Config{}) {}

ServerBrowser::ServerBrowser(net::Transport& t, bool s, Config config)
    : transport_(t), server_(s), config_(config) {
    if (!config.capacity || config.capacity > 65536 || !config.concurrentPings ||
        config.concurrentPings > config.capacity || !config.concurrentRequests ||
        config.concurrentRequests > config.capacity || !config.pingTimeout ||
        config.pingTimeout > 3600000 || !config.requestTimeout || config.requestTimeout > 3600000 ||
        config.pingRetries > 16 || config.requestRetries > 16) {
        throw std::invalid_argument("invalid browser limits");
    }
}

std::size_t ServerBrowser::pending() const {
    return pings_.size() + queries_.size() + lookups_.size();
}

bool ServerBrowser::enqueue(std::vector<Request>& list,
                            std::string_view address,
                            UInt8 number,
                            UInt32 pingTime) {
    const auto canonical = transport_.canonicalAddress(address);
    if (server_ || pending() >= config_.capacity) {
        return false;
    }
    if (canonical.empty()) {
        const auto endpoint = net::canonicalIPv4Endpoint(address);
        if (endpoint.empty() || lookups_.size() >= 8) {
            return false;
        }
        const bool queryRequest = &list == &queries_;
        if (std::ranges::any_of(lookups_, [&](const auto& lookup) {
                return lookup.address == endpoint && lookup.query == queryRequest;
            })) {
            return true;
        }
        auto lookup = net::resolveIPv4AddressAsync(endpoint);
        if (!lookup) {
            return false;
        }
        lookups_.push_back({endpoint, std::move(lookup), queryRequest, pingTime});
        return true;
    }
    if (std::ranges::any_of(list, [&](const auto& r) {
            return r.address == canonical && r.number == number;
        })) {
        return true;
    }
    list.push_back({canonical, number, 0, 0, 0, pingTime});
    return true;
}

bool ServerBrowser::ping(std::string_view address) {
    return (!shouldPing || shouldPing(address)) && enqueue(pings_, address, 0, 65535);
}

bool ServerBrowser::query(std::string_view address, UInt32 pingTime) {
    return enqueue(queries_, address, 0, pingTime);
}

void ServerBrowser::clear() {
    lookups_.clear();
    pings_.clear();
    queries_.clear();
    responses_.clear();
    broadcasts_.clear();
    completed_.clear();
}

bool ServerBrowser::setHeartbeat(std::span<const std::string> masters,
                                 bool publicHost,
                                 UInt32 now) {
    if (!server_ || masters.size() > 64) {
        return false;
    }
    std::vector<std::string> validated;
    if (!std::ranges::all_of(masters, [&](const auto& address) {
            auto a = transport_.canonicalAddress(address);
            if (a.empty()) {
                a = net::canonicalIPv4Endpoint(address);
            }
            if (a.empty()) {
                return false;
            }
            if (std::ranges::find(validated, a) == validated.end()) {
                validated.push_back(std::move(a));
            }
            return true;
        })) {
        return false;
    }
    if (publicHost_ == publicHost &&
        std::ranges::equal(validated, masters_, {}, std::identity{}, &HeartbeatMaster::address)) {
        return true;
    }
    masters_.clear();
    for (auto& address : validated) {
        auto resolved = transport_.canonicalAddress(address);
        masters_.push_back({std::move(address), std::move(resolved), {}, now, false});
    }
    publicHost_ = publicHost;
    return true;
}

void ServerBrowser::process(UInt32 now) {
    if (busy_) {
        return;
    }
    Guard guard(busy_);
    for (std::size_t i = 0; i < lookups_.size();) {
        std::string result;
        if (!lookups_[i].lookup->poll(result)) {
            ++i;
            continue;
        }
        auto request = std::move(lookups_[i]);
        lookups_.erase(lookups_.begin() + static_cast<std::ptrdiff_t>(i));
        if (result.empty()) {
            if (onTimeout) {
                onTimeout(request.address);
            }
        } else {
            if (resolvedAddress) {
                resolvedAddress(request.address, result);
            }
            if (request.query) {
                (void)query(result, request.ping);
            } else {
                (void)ping(result);
            }
        }
    }

    if (server_) {
        if (publicHost_) {
            const auto bytes = Packet{Type::Heartbeat, 255, 0, 0, 0, {}}.encode();
            for (auto& master : masters_) {
                if (master.lookup) {
                    std::string result;
                    if (!master.lookup->poll(result)) {
                        continue;
                    }
                    master.lookup.reset();
                    master.resolved = std::move(result);
                    master.attempted = master.resolved.empty();
                    master.time = now;
                }
                if (master.attempted && now - master.time < HeartbeatInterval) {
                    continue;
                }
                if (master.resolved.empty()) {
                    // Native DNS concurrency is bounded. If all slots are busy,
                    // retry acquiring one on the next tick. Failed lookups back
                    // off for the normal heartbeat interval.
                    master.lookup = net::resolveIPv4AddressAsync(master.address);
                    continue;
                }
                (void)transport_.send(master.resolved, bytes);
                master.time = now;
                master.attempted = true;
            }
        }
        return;
    }
    std::erase_if(responses_, [&](const auto& r) {
        return now - r.sent > config_.requestTimeout;
    });
    std::erase_if(broadcasts_, [&](const auto& r) {
        return now - r.sent > config_.pingTimeout;
    });
    const auto drive = [&](std::vector<Request>& list,
                           std::size_t concurrent,
                           UInt32 timeout,
                           UInt32 retries,
                           Type type) {
        std::vector<Request> work;
        work.swap(list);
        std::size_t active{};
        std::vector<std::string> timeouts;
        std::ranges::for_each(work, [&](auto& r) {
            if (r.tries && now - r.sent > timeout && r.tries > retries) {
                timeouts.push_back(r.address);
                return;
            }
            if (active >= concurrent) {
                list.push_back(std::move(r));
                return;
            }
            if (!r.tries || now - r.sent > timeout) {
                const auto key = nextKey_;
                const auto bytes =
                    Packet{type, r.number ? r.number : UInt8{255}, 0, key, 0, {}}.encode();
                const auto result = transport_.send(r.address, bytes);
                if (result == net::Result::UnsupportedTransport ||
                    result == net::Result::InvalidAddress || result == net::Result::Closed) {
                    if (!broadcast(r.address)) {
                        timeouts.push_back(r.address);
                    }
                    return;
                }
                if (result == net::Result::Ok) {
                    r.key = nextKey_++;
                    r.sent = now;
                    ++r.tries;
                }
                if (result == net::Result::Ok && broadcast(r.address)) {
                    if (broadcasts_.size() < config_.capacity) {
                        broadcasts_.push_back(r);
                    }
                    return;
                }
            }
            active += r.tries != 0;
            list.push_back(std::move(r));
        });
        // Retire before callbacks so handlers can enqueue replacements safely.
        const auto callback = onTimeout;
        std::ranges::for_each(timeouts, [&](const auto& a) {
            if (completed_.size() < config_.capacity) {
                completed_.push_back(a);
            }
            if (callback) {
                callback(a);
            }
        });
    };
    drive(
        pings_, config_.concurrentPings, config_.pingTimeout, config_.pingRetries, Type::PingQuery);
    if (pings_.empty()) {
        drive(queries_,
              config_.concurrentRequests,
              config_.requestTimeout,
              config_.requestRetries,
              Type::GameQuery);
    }
}

bool ServerBrowser::receive(std::string_view from, std::span<const std::byte> bytes, UInt32 now) {
    if (bytes.empty() || byte(bytes[0]) != 16) {
        return false;
    }
    if (busy_) {
        return true;
    }
    Guard guard(busy_);
    const auto decoded = Packet::decode(bytes);
    const auto address = transport_.canonicalAddress(from);
    if (!decoded || address.empty()) {
        return true;
    }
    const auto& p = *decoded;
    const auto send = [&](Packet packet) {
        const auto data = packet.encode();
        if (!data.empty()) {
            (void)transport_.send(address, data);
        }
    };
    if (server_) {
        if (p.type == Type::PingQuery && p.data.empty()) {
            const auto callback = getPingInfo;
            const auto info = callback ? callback() : PingInfo{{}, "The Central Scrutinizer"};
            Packet out{Type::PingResponse,
                       255,
                       info.gameType,
                       p.key,
                       UInt16((UInt16(info.players) << 8) | info.maxPlayers),
                       std::vector<std::byte>(32)};
            const std::size_t offset = info.gameType == 0xf0 ? 2 : 0;
            if (offset) {
                out.data[0] = std::byte(info.protocolVersion & 255);
                out.data[1] = std::byte(info.protocolVersion >> 8);
            }
            const auto name = std::string_view(info.name).substr(
                0, std::min<std::size_t>(info.name.find('\0'), 31 - offset));
            std::ranges::transform(
                name, out.data.begin() + static_cast<std::ptrdiff_t>(offset), [](char c) {
                    return std::byte(static_cast<unsigned char>(c));
                });
            send(std::move(out));
        } else if (p.type == Type::GameQuery && p.data.empty()) {
            const auto callback = getGameInfo;
            const auto chunks = callback ? callback() : std::vector<std::vector<std::byte>>{{}};
            if (chunks.size() <= 64 && std::ranges::all_of(chunks, [](const auto& c) {
                    return c.size() <= MaxData;
                })) {
                std::ranges::for_each(chunks, [&](const auto& data) {
                    send({Type::GameResponse, 255, 0, p.key, 0, data});
                });
            }
        }
        return true;
    }
    if (p.type == Type::PingResponse) {
        if (p.data.size() != 32) {
            return true;
        }
        auto found = std::ranges::find_if(pings_, [&](const auto& r) {
            return r.address == address && r.number == 0 && r.tries && r.key == p.key;
        });
        if (found == pings_.end()) {
            if (std::ranges::find(completed_, address) == completed_.end() &&
                std::ranges::any_of(broadcasts_, [&](const auto& r) {
                    return r.key == p.key && now - r.sent <= config_.pingTimeout;
                })) {
                ping(address);
            }
            return true;
        }
        const auto pingTime = now - found->sent;
        pings_.erase(found);
        const auto offset = p.total == 0xf0 ? 2 : 0;
        std::string name;
        std::ranges::for_each(p.data | std::views::drop(offset) | std::views::take(31 - offset) |
                                  std::views::take_while([](auto b) {
                                      return b != std::byte{};
                                  }),
                              [&](auto b) {
                                  name.push_back(char(byte(b)));
                              });
        if (completed_.size() < config_.capacity) {
            completed_.push_back(address);
        }
        const PingInfo info{address,
                            std::move(name),
                            p.total,
                            UInt8(p.id >> 8),
                            UInt8(p.id & 255),
                            pingTime,
                            p.total == 0xf0 ? shortWord(p.data) : UInt16{0}};
        const auto callback = gotPingInfo;
        if (callback) {
            callback(info);
        } else {
            query(address, pingTime);
        }
    } else if (p.type == Type::GameResponse) {
        const auto found = std::ranges::find_if(queries_, [&](const auto& r) {
            return r.address == address && r.tries && r.key == p.key;
        });
        if (found != queries_.end()) {
            if (responses_.size() >= config_.capacity) {
                responses_.erase(responses_.begin());
            }
            responses_.push_back({address, p.key, now, found->ping});
            queries_.erase(found);
        }
        const auto response = std::ranges::find_if(responses_, [&](const auto& r) {
            return r.address == address && r.key == p.key && now - r.sent <= config_.requestTimeout;
        });
        if (response == responses_.end()) {
            return true;
        }
        const auto callback = gotGameInfo;
        const GameInfo info{address, p.key, response->ping, p.data};
        if (callback) {
            callback(info);
        }
    } else if (p.type == Type::MasterList) {
        if (!p.total || !p.number || p.number > p.total) {
            return true;
        }
        auto found = std::ranges::find_if(pings_, [&](const auto& r) {
            return r.address == address && (r.number == 0 || r.number == p.number) && r.tries &&
                   r.key == p.key;
        });
        if (found == pings_.end()) {
            return true;
        }
        std::size_t cursor{};
        MasterInfo info{address, "(no-name)", "(no-motd)", p.number == 1};
        std::vector<std::string> servers;
        const auto text = [&](std::string& value) {
            if (cursor >= p.data.size()) {
                return false;
            }
            const auto size = byte(p.data[cursor++]);
            if (size > p.data.size() - cursor) {
                return false;
            }
            if (size) {
                value.clear();
                std::ranges::for_each(std::span(p.data).subspan(cursor, size), [&](auto b) {
                    value.push_back(char(byte(b)));
                });
            }
            cursor += size;
            return value.find('\0') == value.npos;
        };
        if ((info.first && (!text(info.name) || !text(info.motd))) || p.data.size() - cursor < 2) {
            return true;
        }
        const auto count = (UInt32(byte(p.data[cursor])) << 8) | byte(p.data[cursor + 1]);
        cursor += 2;
        if (count > config_.capacity) {
            return true;
        }
        bool valid = true;
        std::ranges::for_each(std::views::iota(UInt32{0}, count), [&](auto) {
            if (!valid) {
                return;
            }
            if (p.data.size() - cursor < 7 || byte(p.data[cursor]) != 6) {
                valid = false;
                return;
            }
            net::IPv4Address a{{byte(p.data[cursor + 1]),
                                byte(p.data[cursor + 2]),
                                byte(p.data[cursor + 3]),
                                byte(p.data[cursor + 4])},
                               shortWord(std::span(p.data).subspan(cursor + 5, 2))};
            cursor += 7;
            if (!a.port) {
                valid = false;
                return;
            }
            servers.push_back(net::formatIPv4Address(a));
        });
        if (!valid || cursor != p.data.size()) {
            return true;
        }
        const bool general = found->number == 0;
        if (general && pending() - 1 + std::size_t(p.total) - 1 > config_.capacity) {
            return true;
        }
        pings_.erase(found);
        if (general) {
            std::ranges::for_each(std::views::iota(1U, UInt32(p.total) + 1), [&](auto n) {
                if (n != p.number) {
                    pings_.push_back({address, UInt8(n), p.key, now, 1, 65535});
                }
            });
        }
        std::ranges::for_each(servers, [&](const auto& a) {
            if (std::ranges::find(completed_, a) == completed_.end()) {
                ping(a);
            }
        });
        const auto callback = gotMaster;
        if (callback) {
            callback(info);
        }
    }
    return true;
}
}
