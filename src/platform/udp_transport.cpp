#include <opent1/platform/net_transport.hpp>
#include <algorithm>
#include <charconv>
#include <format>
#include <stdexcept>
#include <atomic>
#include <chrono>
#include <thread>
#if !defined(__EMSCRIPTEN__)
#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <netinet/in.h>
#include <netdb.h>
#include <sys/socket.h>
#include <unistd.h>
#endif
#endif

namespace opent1::net {
std::optional<IPv4Address> parseIPv4Address(std::string_view text) {
    if (text.size() > MaxAddress || text.find('\0') != text.npos) {
        return {};
    }
    if (text.starts_with("IP:") || text.starts_with("ip:") || text.starts_with("Ip:") ||
        text.starts_with("iP:")) {
        text.remove_prefix(3);
    }
    const auto colon = text.find(':');
    if (colon == text.npos) {
        return {};
    }
    const auto number = [](std::string_view value, unsigned max) -> std::optional<unsigned> {
        unsigned n{};
        const auto r = std::from_chars(value.data(), value.data() + value.size(), n);
        if (value.empty() || r.ec != std::errc{} || r.ptr != value.data() + value.size() ||
            n > max) {
            return {};
        }
        return n;
    };
    const auto port = number(text.substr(colon + 1), 65535);
    if (!port) {
        return {};
    }
    IPv4Address result;
    result.port = static_cast<UInt16>(*port);
    auto host = text.substr(0, colon);
    std::string lower(host);
    std::ranges::transform(lower, lower.begin(), [](unsigned char c) {
        return static_cast<char>(c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c);
    });
    if (lower == "broadcast") {
        result.host.fill(255);
        return result;
    }
    for (std::size_t i = 0; i < result.host.size(); ++i) {
        const auto dot = host.find('.');
        if ((i < 3) != (dot != host.npos)) {
            return {};
        }
        const auto part = number(host.substr(0, dot), 255);
        if (!part) {
            return {};
        }
        result.host[i] = static_cast<UInt8>(*part);
        if (dot != host.npos) {
            host.remove_prefix(dot + 1);
        }
    }
    return result;
}

std::string formatIPv4Address(const IPv4Address& a) {
    if (std::ranges::all_of(a.host, [](UInt8 b) {
            return b == 255;
        })) {
        return std::format("IP:Broadcast:{}", a.port);
    }
    return std::format("IP:{}.{}.{}.{}:{}", a.host[0], a.host[1], a.host[2], a.host[3], a.port);
}

std::string canonicalIPv4Endpoint(std::string_view text) {
    if (const auto numeric = parseIPv4Address(text)) {
        return numeric->port ? formatIPv4Address(*numeric) : std::string{};
    }
    if (text.empty() || text.size() > MaxAddress || text.find('\0') != text.npos) {
        return {};
    }
    if (text.size() >= 3 && (text[0] == 'i' || text[0] == 'I') &&
        (text[1] == 'p' || text[1] == 'P') && text[2] == ':') {
        text.remove_prefix(3);
    }
    const auto colon = text.find(':');
    if (colon == text.npos || !colon || text.find(':', colon + 1) != text.npos) {
        return {};
    }
    auto host = text.substr(0, colon);
    const auto service = text.substr(colon + 1);
    UInt16 port{};
    const auto parsed = std::from_chars(service.data(), service.data() + service.size(), port);
    if (!port || parsed.ec != std::errc{} || parsed.ptr != service.data() + service.size()) {
        return {};
    }
    bool alphabetic = false;
    std::string normalized;
    for (char c : host) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c + ('a' - 'A'));
        }
        if (c >= 'a' && c <= 'z') {
            alphabetic = true;
        } else if (!(c >= '0' && c <= '9') && c != '-' && c != '.') {
            return {};
        }
        normalized += c;
    }
    if (!alphabetic) {
        return {};
    } // Do not turn malformed numeric addresses into DNS queries.
    std::size_t begin{};
    while (begin < normalized.size()) {
        const auto end = normalized.find('.', begin);
        const auto label =
            std::string_view(normalized)
                .substr(begin, end == normalized.npos ? normalized.size() - begin : end - begin);
        if (label.empty() || label.size() > 63 || label.front() == '-' || label.back() == '-') {
            return {};
        }
        if (end == normalized.npos) {
            break;
        }
        begin = end + 1;
    }
    return "IP:" + normalized + ":" + std::to_string(port);
}
#if defined(__EMSCRIPTEN__)
std::unique_ptr<AddressLookup> resolveIPv4AddressAsync(std::string_view) {
    return {};
}

std::unique_ptr<Transport> makeUdpTransport(IPv4Address) {
    throw std::runtime_error("Native UDP unavailable: provide a browser datagram adapter");
}
#else
namespace {
#if defined(_WIN32)
using Socket = SOCKET;
constexpr Socket InvalidSocket = INVALID_SOCKET;
using SockLen = int;

void closeSocket(Socket s) {
    closesocket(s);
}

bool wouldBlock() {
    return WSAGetLastError() == WSAEWOULDBLOCK;
}

bool interrupted() {
    return WSAGetLastError() == WSAEINTR;
}

struct SocketRuntime {
    SocketRuntime() {
        WSADATA data{};
        if (WSAStartup(MAKEWORD(2, 2), &data)) {
            throw std::runtime_error("WSAStartup failed");
        }
    }

    ~SocketRuntime() {
        WSACleanup();
    }
};
#else
using Socket = int;
constexpr Socket InvalidSocket = -1;
using SockLen = socklen_t;

void closeSocket(Socket s) {
    ::close(s);
}

bool wouldBlock() {
    return errno == EAGAIN || errno == EWOULDBLOCK;
}

bool interrupted() {
    return errno == EINTR;
}

struct SocketRuntime {};
#endif
sockaddr_in native(const IPv4Address& a) {
    sockaddr_in result{};
    result.sin_family = AF_INET;
    result.sin_port = htons(a.port);
    UInt32 host{};
    std::ranges::for_each(a.host, [&](UInt8 b) {
        host = (host << 8) | b;
    });
    result.sin_addr.s_addr = htonl(host);
    return result;
}

IPv4Address portable(const sockaddr_in& a) {
    const auto h = ntohl(a.sin_addr.s_addr);
    return {{{static_cast<UInt8>(h >> 24),
              static_cast<UInt8>(h >> 16),
              static_cast<UInt8>(h >> 8),
              static_cast<UInt8>(h)}},
            ntohs(a.sin_port)};
}

struct LookupState {
    std::atomic<bool> ready{};
    std::string result;
};

class NativeLookup final : public AddressLookup {
public:
    explicit NativeLookup(std::shared_ptr<LookupState> state) : state_(std::move(state)) {}

    bool poll(std::string& result) const override {
        if (timedOut_) {
            result.clear();
            return true;
        }
        if (!state_->ready.load(std::memory_order_acquire)) {
            if (std::chrono::steady_clock::now() - started_ < std::chrono::seconds(3)) {
                return false;
            }
            timedOut_ = true;
            result.clear();
            return true;
        }
        result = state_->result;
        return true;
    }

private:
    std::shared_ptr<LookupState> state_;
    std::chrono::steady_clock::time_point started_{std::chrono::steady_clock::now()};
    mutable bool timedOut_{};
};

class UdpTransport final : public Transport {
public:
    explicit UdpTransport(IPv4Address address) {
        socket_ = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (socket_ == InvalidSocket) {
            throw std::runtime_error("UDP socket failed");
        }
        const auto a = native(address);
        int bufferSize = 32768, broadcast = 1;
#if defined(_WIN32)
        u_long nonblocking = 1;
        const bool nonblockingOk = ioctlsocket(socket_, FIONBIO, &nonblocking) == 0;
#else
        const int flags = fcntl(socket_, F_GETFL, 0);
        const bool nonblockingOk = flags >= 0 && fcntl(socket_, F_SETFL, flags | O_NONBLOCK) == 0;
#endif
        if (!nonblockingOk ||
            ::bind(socket_, reinterpret_cast<const sockaddr*>(&a), sizeof(a)) != 0 ||
            setsockopt(socket_,
                       SOL_SOCKET,
                       SO_RCVBUF,
                       reinterpret_cast<const char*>(&bufferSize),
                       sizeof(bufferSize)) != 0 ||
            setsockopt(socket_,
                       SOL_SOCKET,
                       SO_SNDBUF,
                       reinterpret_cast<const char*>(&bufferSize),
                       sizeof(bufferSize)) != 0 ||
            setsockopt(socket_,
                       SOL_SOCKET,
                       SO_BROADCAST,
                       reinterpret_cast<const char*>(&broadcast),
                       sizeof(broadcast)) != 0) {
            close();
            throw std::runtime_error("UDP bind/nonblocking setup failed");
        }
        sockaddr_in bound{};
        SockLen size = sizeof(bound);
        if (getsockname(socket_, reinterpret_cast<sockaddr*>(&bound), &size) != 0) {
            close();
            throw std::runtime_error("UDP getsockname failed");
        }
        address_ = portable(bound);
    }

    ~UdpTransport() override {
        close();
    }

    Result send(std::string_view address, std::span<const std::byte> data) override {
        if (socket_ == InvalidSocket) {
            return Result::Closed;
        }
        if (data.empty() || data.size() > MaxDatagram) {
            return Result::TooLarge;
        }
        const auto parsed = parseIPv4Address(address);
        if (!parsed || !parsed->port) {
            return Result::InvalidAddress;
        }
        const auto a = native(*parsed);
        const auto sent = sendto(socket_,
                                 reinterpret_cast<const char*>(data.data()),
                                 static_cast<int>(data.size()),
                                 0,
                                 reinterpret_cast<const sockaddr*>(&a),
                                 sizeof(a));
        if (sent < 0) {
            return wouldBlock() || interrupted() ? Result::WouldBlock : Result::IoError;
        }
        return static_cast<std::size_t>(sent) == data.size() ? Result::Ok : Result::IoError;
    }

    Result receive(Datagram& packet) override {
        if (socket_ == InvalidSocket) {
            return Result::Closed;
        }
        // Extra byte detects POSIX truncation. Winsock returns WSAEMSGSIZE.
        std::array<char, MaxDatagram + 1> buffer{};
        sockaddr_in from{};
        SockLen length = sizeof(from);
        const auto count = recvfrom(socket_,
                                    buffer.data(),
                                    static_cast<int>(buffer.size()),
                                    0,
                                    reinterpret_cast<sockaddr*>(&from),
                                    &length);
        if (count < 0) {
#if defined(_WIN32)
            if (WSAGetLastError() == WSAEMSGSIZE) {
                return Result::TooLarge;
            }
#endif
            return wouldBlock() || interrupted() ? Result::WouldBlock : Result::IoError;
        }
        if (count == 0 || static_cast<std::size_t>(count) > MaxDatagram) {
            return Result::TooLarge;
        }
        packet.address = formatIPv4Address(portable(from));
        packet.size = static_cast<std::size_t>(count);
        std::ranges::transform(
            std::span(buffer).first(packet.size), packet.bytes.begin(), [](char b) {
                return static_cast<std::byte>(b);
            });
        return Result::Ok;
    }

    std::string localAddress() const override {
        return formatIPv4Address(address_);
    }

    std::string canonicalAddress(std::string_view address) const override {
        const auto parsed = parseIPv4Address(address);
        return parsed && parsed->port ? formatIPv4Address(*parsed) : std::string{};
    }

    void close() override {
        if (socket_ != InvalidSocket) {
            closeSocket(socket_);
            socket_ = InvalidSocket;
        }
    }

private:
    [[maybe_unused]] SocketRuntime runtime_;
    Socket socket_{InvalidSocket};
    IPv4Address address_;
};
}

std::unique_ptr<Transport> makeUdpTransport(IPv4Address address) {
    return std::make_unique<UdpTransport>(address);
}

std::unique_ptr<AddressLookup> resolveIPv4AddressAsync(std::string_view address) {
    const auto endpoint = canonicalIPv4Endpoint(address);
    if (endpoint.empty()) {
        return {};
    }
    auto state = std::make_shared<LookupState>();
    if (parseIPv4Address(endpoint)) {
        state->result = endpoint;
        state->ready = true;
        return std::make_unique<NativeLookup>(std::move(state));
    }
    static const auto active = std::make_shared<std::atomic<unsigned>>(0);
    auto count = active->load();
    do {
        if (count >= 8) {
            return {};
        }
    } while (!active->compare_exchange_weak(count, count + 1));
    const auto separator = endpoint.rfind(':');
    const auto host = endpoint.substr(3, separator - 3), service = endpoint.substr(separator + 1);
    try {
        std::thread([state, host, service, counter = active] {
            try {
                [[maybe_unused]] SocketRuntime runtime;
                addrinfo hints{};
                hints.ai_family = AF_INET;
                hints.ai_socktype = SOCK_DGRAM;
                hints.ai_protocol = IPPROTO_UDP;
                addrinfo* addresses{};
                if (getaddrinfo(host.c_str(), service.c_str(), &hints, &addresses) == 0) {
                    const std::unique_ptr<addrinfo, decltype(&freeaddrinfo)> cleanup(addresses,
                                                                                     &freeaddrinfo);
                    for (auto* entry = addresses; entry; entry = entry->ai_next) {
                        if (entry->ai_family == AF_INET &&
                            entry->ai_addrlen >= sizeof(sockaddr_in)) {
                            state->result = formatIPv4Address(
                                portable(*reinterpret_cast<const sockaddr_in*>(entry->ai_addr)));
                            break;
                        }
                    }
                }
            } catch (...) {
                state->result.clear();
            }
            state->ready.store(true, std::memory_order_release);
            counter->fetch_sub(1);
        }).detach();
    } catch (...) {
        active->fetch_sub(1);
        return {};
    }
    return std::make_unique<NativeLookup>(std::move(state));
}
#endif
} // namespace opent1::net
