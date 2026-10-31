#include <opent1/sim/sim_net.hpp>
#include <opent1/sim/sim_world_game.hpp>
#include <algorithm>
#include <stdexcept>

namespace opent1::sim::network {
namespace {
net::Transport& requireTransport(const std::unique_ptr<net::Transport>& transport) {
    if (!transport) {
        throw std::invalid_argument("network transport required");
    }
    return *transport;
}
}

CSDelegate::CSDelegate(std::unique_ptr<net::Transport> transport,
                       std::shared_ptr<const Registry> registry,
                       bool server)
    : transport_(std::move(transport)), session_(requireTransport(transport_)),
      browser_(*transport_, server), registry_(std::move(registry)), server_(server) {
    if (!registry_) {
        throw std::invalid_argument("network registry required");
    }
    setId(CSDelegateId);
}

CSDelegate::~CSDelegate() = default;

bool CSDelegate::onAdd() {
    if (server_ && getManager()->getId() != 0 && getManager()->getId() != 2048) {
        return false;
    }
    return !server_ || getManager()->setNetworkId(2048);
}

void CSDelegate::onRemove() {
    pendingLookup_.reset();
    pendingRequest_.clear();
    pendingAddress_.clear();
    const auto snapshot = streams_;
    streams_.clear();
    std::ranges::for_each(snapshot, [&](const auto& pair) {
        if (getManager()) {
            if (auto* stream = dynamic_cast<PacketStream*>(pair.second.resolve(*getManager()))) {
                stream->disconnect(now_);
                stream->deleteObject();
            }
        }
    });
}

std::string CSDelegate::localAddress() const {
    return transport_->localAddress();
}

std::string CSDelegate::translateAddress(std::string_view text, UInt16 defaultPort) const {
    auto canonical = transport_->canonicalAddress(text);
    if (!canonical.empty()) {
        return canonical;
    }
    if (text.size() >= 3 && (text[0] == 'I' || text[0] == 'i') &&
        (text[1] == 'P' || text[1] == 'p') && text[2] == ':') {
        text.remove_prefix(3);
    }
    const auto endpoint = text.find(':') == text.npos
                              ? std::string(text) + ":" + std::to_string(defaultPort)
                              : std::string(text);
    canonical = transport_->canonicalAddress(endpoint);
    return !canonical.empty() ? canonical : net::canonicalIPv4Endpoint(endpoint);
}

PacketStream* CSDelegate::stream(SimObjectId peer) const {
    if (!getManager()) {
        return nullptr;
    }
    const auto found = std::ranges::find_if(streams_, [&](const auto& pair) {
        return pair.second.objectId == peer && pair.second.resolve(*getManager());
    });
    return found == streams_.end()
               ? nullptr
               : dynamic_cast<PacketStream*>(found->second.resolve(*getManager()));
}

PacketStream* CSDelegate::createStream(net::ConnectionId connection, SimObjectId peer) {
    if (!getManager() || isRemoved() || getManager()->findObject(peer)) {
        return nullptr;
    }
    auto object = std::make_unique<PacketStream>(
        registry_, server_, server_ ? PacketStream::Mode::Reply : PacketStream::Mode::Normal);
    object->setId(peer);
    object->attach(session_, connection);
    auto* stream = static_cast<PacketStream*>(getManager()->addObject(std::move(object)));
    if (!stream) {
        return nullptr;
    }
    streams_.emplace(connection, stream->address());
    const auto addClient = [&](auto client) {
        auto* raw = getManager()->addObject(std::move(client));
        if (!raw || !stream->addObject(*raw)) {
            if (raw) {
                raw->deleteObject();
            }
            return false;
        }
        return true;
    };
    if (!addClient(std::make_unique<EventManager>()) ||
        !addClient(std::make_unique<GhostManager>(!server_))) {
        removeStream(connection);
        return nullptr;
    }
    const auto configure = configureStream;
    try {
        if (configure) {
            configure(*stream);
        }
    } catch (...) {
        const auto info = session_.info(connection);
        if (info && info->state == net::State::AwaitingDecision) {
            (void)session_.reject(connection, {}, now_);
        } else {
            (void)session_.disconnect(connection, {}, now_);
        }
        removeStream(connection);
        throw;
    }
    return isRemoved() || !getManager() || stream->isRemoved() ? nullptr : stream;
}

void CSDelegate::removeStream(net::ConnectionId id) {
    const auto found = streams_.find(id);
    if (found == streams_.end()) {
        return;
    }
    const auto address = found->second;
    streams_.erase(found);
    if (getManager()) {
        if (auto* stream = dynamic_cast<PacketStream*>(address.resolve(*getManager()))) {
            stream->connectionDied();
            stream->deleteObject();
        }
    }
}

net::Result
CSDelegate::connect(std::string_view address, UInt32 now, std::span<const std::byte> request) {
    if (server_ || !getManager() || isRemoved() || session_.connectionCount() || pendingLookup_) {
        return net::Result::InvalidState;
    }
    if (request.size() > net::MaxPayload) {
        return net::Result::TooLarge;
    }
    if (transport_->canonicalAddress(address).empty()) {
        auto lookup = net::resolveIPv4AddressAsync(address);
        if (!lookup) {
            return net::Result::InvalidAddress;
        }
        pendingAddress_ = address;
        pendingRequest_.assign(request.begin(), request.end());
        pendingLookup_ = std::move(lookup);
        now_ = now;
        return net::Result::Ok;
    }
    net::ConnectionId connection{};
    now_ = now;
    return session_.connect(address, request, now, connection);
}

bool CSDelegate::disconnect(SimObjectId peer, UInt32 now, std::span<const std::byte> reason) {
    if (!getManager() || reason.size() > net::MaxPayload) {
        return false;
    }
    if (pendingLookup_ && peer == 2048) {
        pendingLookup_.reset();
        pendingRequest_.clear();
        pendingAddress_.clear();
        return true;
    }
    bool removed = false;
    getManager()->dispatch([&] {
        const auto found = std::ranges::find_if(streams_, [&](const auto& pair) {
            return pair.second.objectId == peer;
        });
        if (found == streams_.end()) {
            return;
        }
        const auto id = found->first;
        now_ = now;
        if (auto* ps = dynamic_cast<PacketStream*>(found->second.resolve(*getManager()))) {
            ps->disconnect(now, reason);
        }
        removeStream(id);
        removed = true;
    });
    return removed;
}

net::Result CSDelegate::process(UInt32 now) {
    if (!getManager() || isRemoved() || driving_) {
        return net::Result::InvalidState;
    }
    net::Result result = net::Result::Ok;
    getManager()->dispatch([&] {
        driving_ = true;
        struct Guard {
            bool& b;
            ~Guard() {
                b = false;
            }
        } guard{driving_};
        now_ = now;
        if (pendingLookup_) {
            std::string address;
            if (pendingLookup_->poll(address)) {
                pendingLookup_.reset();
                net::ConnectionId connection{};
                const auto connected =
                    address.empty() ? net::Result::InvalidAddress
                                    : session_.connect(address, pendingRequest_, now, connection);
                pendingRequest_.clear();
                if (connected != net::Result::Ok) {
                    net::Event failed;
                    failed.type = net::EventType::Rejected;
                    failed.address = std::move(pendingAddress_);
                    constexpr std::string_view reason =
                        "Unable to resolve or connect to the server hostname.";
                    failed.size = reason.size();
                    std::ranges::transform(reason, failed.bytes.begin(), [](char c) {
                        return std::byte(c);
                    });
                    const auto callback = onConnection;
                    if (callback) {
                        callback(failed);
                    }
                }
                pendingAddress_.clear();
            }
        }
        if (isRemoved() || !getManager()) {
            return;
        }
        std::erase_if(streams_, [&](const auto& pair) {
            if (pair.second.resolve(*getManager())) {
                return false;
            }
            (void)session_.disconnect(pair.first, {}, now);
            return true;
        });
        browser_.process(now);
        if (isRemoved() || !getManager()) {
            return;
        }
        result = session_.process(now);
        net::Event event;
        while (!isRemoved() && getManager() && session_.poll(event)) {
            if (event.type == net::EventType::Requested) {
                const auto accept = acceptRequest;
                const auto addressed = acceptAddressedRequest;
                const auto allowed = server_ &&
                                     (!addressed || addressed(event.address, event.data())) &&
                                     (!accept || accept(event.data()));
                if (isRemoved() || !getManager()) {
                    break;
                }
                if (!allowed) {
                    (void)session_.reject(event.connection, {}, now);
                } else {
                    const auto candidates = std::views::iota(SimObjectId{2049}, SimObjectId{3072});
                    const auto slot = std::ranges::find_if(candidates, [&](auto id) {
                        return !getManager()->findObject(id);
                    });
                    if (slot == candidates.end()) {
                        (void)session_.reject(event.connection, {}, now);
                    } else {
                        const auto peer = *slot;
                        auto* ps = createStream(event.connection, peer);
                        if (!ps || isRemoved()) {
                            (void)session_.reject(event.connection, {}, now);
                        } else {
                            std::array<std::byte, 8> response{};
                            BitStream output{std::span(response)};
                            output.writeInt(static_cast<Int32>(peer), 32);
                            output.writeInt(2048, 32);
                            if (session_.accept(event.connection, response, now) !=
                                net::Result::Ok) {
                                removeStream(event.connection);
                            }
                        }
                    }
                }
            } else if (event.type == net::EventType::Accepted) {
                BitStream input(event.data());
                const auto assigned = static_cast<UInt32>(input.readInt(32)),
                           peer = static_cast<UInt32>(input.readInt(32));
                if (server_ || event.size != 8 || !input.isValid() || assigned <= 2048 ||
                    assigned >= 3072 || peer != 2048 || !getManager()->setNetworkId(assigned) ||
                    !createStream(event.connection, peer)) {
                    (void)session_.disconnect(event.connection, {}, now);
                    removeStream(event.connection);
                }
            } else if (event.type == net::EventType::Data || event.type == net::EventType::Notify) {
                const auto found = streams_.find(event.connection);
                if (found != streams_.end()) {
                    auto* ps = dynamic_cast<PacketStream*>(found->second.resolve(*getManager()));
                    if (!ps) {
                        (void)session_.disconnect(event.connection, {}, now);
                        removeStream(event.connection);
                    } else if (event.type == net::EventType::Notify) {
                        ps->handleNotify(event.notifyKey, event.delivered);
                    } else {
                        try {
                            if (!ps->readPacket(event.data(), now)) {
                                const std::string error(ps->error());
                                event.type = net::EventType::Disconnected;
                                event.size = std::min(error.size(), event.bytes.size());
                                std::ranges::transform(
                                    std::string_view(error).substr(0, event.size),
                                    event.bytes.begin(),
                                    [](char c) {
                                        return std::byte(static_cast<unsigned char>(c));
                                    });
                                ps->disconnect(now, event.data());
                                removeStream(event.connection);
                            }
                        } catch (...) {
                            if (!ps->isRemoved()) {
                                ps->disconnect(now);
                            }
                            removeStream(event.connection);
                            throw;
                        }
                    }
                }
            } else if (event.type == net::EventType::TimedOut ||
                       event.type == net::EventType::Disconnected ||
                       event.type == net::EventType::Rejected) {
                removeStream(event.connection);
            } else if (event.type == net::EventType::Raw) {
                if (!browser_.receive(event.address, event.data(), now)) {
                    const auto callback = onRawPacket;
                    if (callback) {
                        callback(event.address, event.data());
                    }
                }
            }
            if (!getManager() || isRemoved()) {
                break;
            }
            const auto callback = onConnection;
            if (callback) {
                callback(event);
            }
        }
    });
    return result;
}

bool CSDelegate::processEvent(const SimEvent& event) {
    if (const auto* message = dynamic_cast<const SimMessageEvent*>(&event);
        message && message->message == 0) {
        const auto readClock = clock;
        (void)process(readClock ? readClock() : now_);
        return true;
    }
    return SimObject::processEvent(event);
}
} // namespace opent1::sim::network
