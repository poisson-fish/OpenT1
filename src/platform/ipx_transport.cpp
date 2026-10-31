#include <opent1/platform/net_transport.hpp>
#include <algorithm>
#include <charconv>
#include <format>
#include <stdexcept>
#if defined(_WIN32)
#include <winsock2.h>
#include <wsipx.h>
#endif

namespace opent1::net {
std::optional<IpxAddress> parseIpxAddress(std::string_view text) {
    if (text.size() > MaxAddress || text.find('\0') != text.npos || text.size() < 4) {
        return {};
    }
    std::string prefix(text.substr(0, 4));
    std::ranges::transform(prefix, prefix.begin(), [](char c) {
        return c >= 'a' && c <= 'z' ? char(c - 'a' + 'A') : c;
    });
    if (prefix != "IPX:") {
        return {};
    }
    text.remove_prefix(4);
    const auto last = text.rfind(':');
    if (last == text.npos) {
        return {};
    }
    IpxAddress address;
    const auto port = text.substr(last + 1);
    const auto parsed = std::from_chars(port.data(), port.data() + port.size(), address.port);
    if (port.empty() || parsed.ec != std::errc{} || parsed.ptr != port.data() + port.size()) {
        return {};
    }
    auto host = text.substr(0, last);
    std::string folded(host);
    std::ranges::transform(folded, folded.begin(), [](char c) {
        return c >= 'a' && c <= 'z' ? char(c - 'a' + 'A') : c;
    });
    if (folded == "BROADCAST") {
        address.node.fill(255);
        return address;
    }
    if (host.size() != 21 || host[8] != ':') {
        return {};
    }
    const auto hex = [](std::string_view source, auto& bytes) {
        for (std::size_t i = 0; i < bytes.size(); ++i) {
            unsigned value{};
            const auto begin = source.data() + i * 2;
            const auto result = std::from_chars(begin, begin + 2, value, 16);
            if (result.ec != std::errc{} || result.ptr != begin + 2 || value > 255) {
                return false;
            }
            bytes[i] = static_cast<UInt8>(value);
        }
        return true;
    };
    return hex(host.substr(0, 8), address.network) && hex(host.substr(9), address.node)
               ? std::optional(address)
               : std::nullopt;
}

std::string formatIpxAddress(const IpxAddress& address) {
    if (std::ranges::all_of(address.node, [](auto byte) {
            return byte == 255;
        })) {
        return "IPX:broadcast:" + std::to_string(address.port);
    }
    std::string text = "IPX:";
    for (const auto byte : address.network) {
        text += std::format("{:02X}", byte);
    }
    text += ':';
    for (const auto byte : address.node) {
        text += std::format("{:02X}", byte);
    }
    return text + ':' + std::to_string(address.port);
}

namespace {
#if defined(_WIN32)
struct IpxRuntime {
    IpxRuntime() {
        WSADATA data{};
        const auto result = WSAStartup(MAKEWORD(2, 2), &data);
        if (result) {
            throw std::runtime_error("IPX WSAStartup failed: " + std::to_string(result));
        }
    }

    ~IpxRuntime() {
        WSACleanup();
    }
};

SOCKADDR_IPX nativeIpx(const IpxAddress& address) {
    SOCKADDR_IPX result{};
    result.sa_family = AF_IPX;
    result.sa_socket = htons(address.port);
    for (std::size_t i = 0; i < address.network.size(); ++i) {
        result.sa_netnum[i] = static_cast<char>(address.network[i]);
    }
    for (std::size_t i = 0; i < address.node.size(); ++i) {
        result.sa_nodenum[i] = static_cast<char>(address.node[i]);
    }
    return result;
}

IpxAddress portableIpx(const SOCKADDR_IPX& address) {
    IpxAddress result;
    result.port = ntohs(address.sa_socket);
    for (std::size_t i = 0; i < result.network.size(); ++i) {
        result.network[i] = static_cast<UInt8>(address.sa_netnum[i]);
    }
    for (std::size_t i = 0; i < result.node.size(); ++i) {
        result.node[i] = static_cast<UInt8>(address.sa_nodenum[i]);
    }
    return result;
}

class IpxTransport final : public Transport {
public:
    explicit IpxTransport(UInt16 port) {
        socket_ = ::socket(AF_IPX, SOCK_DGRAM, NSPROTO_IPX);
        if (socket_ == INVALID_SOCKET) {
            throw std::runtime_error("IPX provider unavailable (Winsock " +
                                     std::to_string(WSAGetLastError()) + ")");
        }
        IpxAddress address;
        address.port = port;
        auto native = nativeIpx(address);
        u_long nonblocking = 1;
        int broadcast = 1;
        if (::bind(socket_, reinterpret_cast<const sockaddr*>(&native), sizeof(native)) ||
            ioctlsocket(socket_, FIONBIO, &nonblocking) ||
            setsockopt(socket_,
                       SOL_SOCKET,
                       SO_BROADCAST,
                       reinterpret_cast<const char*>(&broadcast),
                       sizeof(broadcast))) {
            const auto error = WSAGetLastError();
            close();
            throw std::runtime_error("IPX bind/setup failed (Winsock " + std::to_string(error) +
                                     ")");
        }
        int size = sizeof(native);
        if (getsockname(socket_, reinterpret_cast<sockaddr*>(&native), &size)) {
            const auto error = WSAGetLastError();
            close();
            throw std::runtime_error("IPX address lookup failed (Winsock " + std::to_string(error) +
                                     ")");
        }
        address_ = portableIpx(native);
    }

    ~IpxTransport() override {
        close();
    }

    Result send(std::string_view to, std::span<const std::byte> data) override {
        if (socket_ == INVALID_SOCKET) {
            return Result::Closed;
        }
        const auto address = parseIpxAddress(to);
        if (!address || !address->port) {
            return Result::InvalidAddress;
        }
        if (data.empty() || data.size() > MaxDatagram) {
            return Result::TooLarge;
        }
        const auto native = nativeIpx(*address);
        const auto sent = sendto(socket_,
                                 reinterpret_cast<const char*>(data.data()),
                                 static_cast<int>(data.size()),
                                 0,
                                 reinterpret_cast<const sockaddr*>(&native),
                                 sizeof(native));
        if (sent == SOCKET_ERROR) {
            const auto error = WSAGetLastError();
            return error == WSAEWOULDBLOCK || error == WSAEINTR ? Result::WouldBlock
                                                                : Result::IoError;
        }
        return static_cast<std::size_t>(sent) == data.size() ? Result::Ok : Result::IoError;
    }

    Result receive(Datagram& packet) override {
        if (socket_ == INVALID_SOCKET) {
            return Result::Closed;
        }
        SOCKADDR_IPX source{};
        int size = sizeof(source);
        const auto received = recvfrom(socket_,
                                       reinterpret_cast<char*>(packet.bytes.data()),
                                       static_cast<int>(packet.bytes.size()),
                                       0,
                                       reinterpret_cast<sockaddr*>(&source),
                                       &size);
        if (received == SOCKET_ERROR) {
            const auto error = WSAGetLastError();
            return error == WSAEWOULDBLOCK || error == WSAEINTR ? Result::WouldBlock
                   : error == WSAEMSGSIZE                       ? Result::TooLarge
                                                                : Result::IoError;
        }
        if (!received || source.sa_family != AF_IPX) {
            return Result::WouldBlock;
        }
        packet.address = formatIpxAddress(portableIpx(source));
        packet.size = static_cast<std::size_t>(received);
        return Result::Ok;
    }

    std::string canonicalAddress(std::string_view text) const override {
        const auto address = parseIpxAddress(text);
        return address && address->port ? formatIpxAddress(*address) : std::string{};
    }

    std::string localAddress() const override {
        return formatIpxAddress(address_);
    }

    void close() override {
        if (socket_ != INVALID_SOCKET) {
            closesocket(socket_);
            socket_ = INVALID_SOCKET;
        }
    }

private:
    IpxRuntime runtime_;
    SOCKET socket_{INVALID_SOCKET};
    IpxAddress address_;
};
#endif
class GameTransport final : public Transport {
public:
    explicit GameTransport(IPv4Address address) : udp_(makeUdpTransport(address)) {
        const auto local = parseIPv4Address(udp_->localAddress());
        try {
            ipx_ = makeIpxTransport(local ? local->port : address.port);
        } catch (const std::runtime_error&) {
        }
    }

    Result send(std::string_view to, std::span<const std::byte> data) override {
        if (parseIpxAddress(to)) {
            return ipx_ ? ipx_->send(to, data) : Result::UnsupportedTransport;
        }
        return udp_->send(to, data);
    }

    Result receive(Datagram& packet) override {
        ipxFirst_ = !ipxFirst_;
        if (ipx_ && ipxFirst_) {
            const auto result = ipx_->receive(packet);
            if (result != Result::WouldBlock) {
                return result;
            }
        }
        const auto result = udp_->receive(packet);
        if (result != Result::WouldBlock) {
            return result;
        }
        return ipx_ && !ipxFirst_ ? ipx_->receive(packet) : Result::WouldBlock;
    }

    std::string canonicalAddress(std::string_view text) const override {
        if (const auto address = parseIpxAddress(text)) {
            return address->port ? formatIpxAddress(*address) : std::string{};
        }
        return udp_->canonicalAddress(text);
    }

    std::string localAddress() const override {
        return udp_->localAddress();
    }

    void close() override {
        udp_->close();
        if (ipx_) {
            ipx_->close();
        }
    }

private:
    std::unique_ptr<Transport> udp_, ipx_;
    bool ipxFirst_{};
};
}

std::unique_ptr<Transport> makeIpxTransport(UInt16 port) {
#if defined(_WIN32)
    return std::make_unique<IpxTransport>(port);
#else
    (void)port;
    throw std::runtime_error("Native IPX provider is unavailable on this platform");
#endif
}

std::unique_ptr<Transport> makeGameTransport(IPv4Address address) {
    return std::make_unique<GameTransport>(address);
}
}
