#include <opent1/platform/tcp_stream.hpp>
#include <algorithm>
#include <limits>
#include <stdexcept>
#if !defined(__EMSCRIPTEN__)
#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#endif
#endif

namespace opent1::net {
#if defined(__EMSCRIPTEN__)
std::unique_ptr<TcpListener> makeTcpListener(IPv4Address) {
    throw std::runtime_error("Native TCP unavailable in browsers");
}

std::unique_ptr<TcpStream> makeTcpConnection(IPv4Address) {
    throw std::runtime_error("Native TCP unavailable in browsers");
}
#else
namespace {
#if defined(_WIN32)
using Socket = SOCKET;
using SockLen = int;
constexpr Socket Invalid = INVALID_SOCKET;

struct Runtime {
    Runtime() {
        WSADATA data{};
        if (WSAStartup(MAKEWORD(2, 2), &data)) {
            throw std::runtime_error("TCP WSAStartup failed");
        }
    }

    ~Runtime() {
        WSACleanup();
    }
};

void closeSocket(Socket s) {
    closesocket(s);
}

bool blocked() {
    const auto e = WSAGetLastError();
    return e == WSAEWOULDBLOCK || e == WSAEINPROGRESS || e == WSAEINTR;
}

bool nonblocking(Socket s) {
    u_long value = 1;
    return ioctlsocket(s, FIONBIO, &value) == 0;
}
#else
using Socket = int;
using SockLen = socklen_t;
constexpr Socket Invalid = -1;

struct Runtime {};

void closeSocket(Socket s) {
    ::close(s);
}

bool blocked() {
    return errno == EWOULDBLOCK || errno == EAGAIN || errno == EINPROGRESS || errno == EINTR;
}

bool nonblocking(Socket s) {
    const auto flags = fcntl(s, F_GETFL, 0);
    return flags >= 0 && fcntl(s, F_SETFL, flags | O_NONBLOCK) == 0;
}
#endif
struct Handle {
    Socket socket{Invalid};

    explicit Handle(Socket s) : socket(s) {}

    Handle(Handle&& other) noexcept : socket(other.socket) {
        other.socket = Invalid;
    }

    ~Handle() {
        close();
    }

    void close() {
        if (socket != Invalid) {
            closeSocket(socket);
            socket = Invalid;
        }
    }

    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
};

sockaddr_in native(IPv4Address a) {
    sockaddr_in result{};
    result.sin_family = AF_INET;
    result.sin_port = htons(a.port);
    UInt32 host{};
    std::ranges::for_each(a.host, [&](auto b) {
        host = (host << 8) | b;
    });
    result.sin_addr.s_addr = htonl(host);
    return result;
}

class Stream final : public TcpStream {
public:
    Stream(std::shared_ptr<Runtime> runtime, Handle socket, bool pending)
        : runtime_(std::move(runtime)), handle_(std::move(socket)), pending_(pending) {
        if (!nonblocking(handle_.socket)) {
            throw std::runtime_error("TCP nonblocking setup failed");
        }
#if defined(SO_NOSIGPIPE)
        const int value = 1;
        if (setsockopt(handle_.socket,
                       SOL_SOCKET,
                       SO_NOSIGPIPE,
                       reinterpret_cast<const char*>(&value),
                       sizeof(value)) != 0) {
            throw std::runtime_error("TCP signal setup failed");
        }
#endif
    }

    Result connected() override {
        if (handle_.socket == Invalid) {
            return Result::Closed;
        }
        if (!pending_) {
            return Result::Ok;
        }
#if !defined(_WIN32)
        if (handle_.socket >= FD_SETSIZE) {
            close();
            return Result::IoError;
        }
#endif
        fd_set writable, errors;
        FD_ZERO(&writable);
        FD_ZERO(&errors);
        FD_SET(handle_.socket, &writable);
        FD_SET(handle_.socket, &errors);
        timeval timeout{};
#if defined(_WIN32)
        const auto result = select(0, nullptr, &writable, &errors, &timeout);
#else
        const auto result = select(handle_.socket + 1, nullptr, &writable, &errors, &timeout);
#endif
        if (result < 0) {
            if (blocked()) {
                return Result::WouldBlock;
            }
            close();
            return Result::IoError;
        }
        if (!result) {
            return Result::WouldBlock;
        }
        int error{};
        SockLen size = sizeof(error);
        if (getsockopt(
                handle_.socket, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&error), &size) !=
                0 ||
            error) {
            close();
            return Result::IoError;
        }
        pending_ = false;
        return Result::Ok;
    }

    StreamResult read(std::span<std::byte> bytes) override {
        const auto state = connected();
        if (state != Result::Ok) {
            return {state};
        }
        if (bytes.empty()) {
            return {Result::Ok};
        }
        const auto count = recv(
            handle_.socket,
            reinterpret_cast<char*>(bytes.data()),
            static_cast<int>(std::min(bytes.size(), std::size_t(std::numeric_limits<int>::max()))),
            0);
        if (count > 0) {
            return {Result::Ok, static_cast<std::size_t>(count)};
        }
        if (count < 0 && blocked()) {
            return {Result::WouldBlock};
        }
        close();
        return {count == 0 ? Result::Closed : Result::IoError};
    }

    StreamResult write(std::span<const std::byte> bytes) override {
        const auto state = connected();
        if (state != Result::Ok) {
            return {state};
        }
        if (bytes.empty()) {
            return {Result::Ok};
        }
        int flags{};
#if defined(MSG_NOSIGNAL)
        flags = MSG_NOSIGNAL;
#endif
        const auto count = send(
            handle_.socket,
            reinterpret_cast<const char*>(bytes.data()),
            static_cast<int>(std::min(bytes.size(), std::size_t(std::numeric_limits<int>::max()))),
            flags);
        if (count > 0) {
            return {Result::Ok, static_cast<std::size_t>(count)};
        }
        if (count < 0 && blocked()) {
            return {Result::WouldBlock};
        }
        close();
        return {Result::IoError};
    }

    void close() override {
        handle_.close();
    }

private:
    std::shared_ptr<Runtime> runtime_;
    Handle handle_;
    bool pending_;
};

class Listener final : public TcpListener {
public:
    explicit Listener(IPv4Address address)
        : runtime_(std::make_shared<Runtime>()),
          handle_(::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP)) {
        if (handle_.socket == Invalid) {
            throw std::runtime_error("TCP socket failed");
        }
        const auto a = native(address);
        if (!nonblocking(handle_.socket) ||
            ::bind(handle_.socket, reinterpret_cast<const sockaddr*>(&a), sizeof(a)) != 0 ||
            listen(handle_.socket, 16) != 0) {
            throw std::runtime_error("TCP listen failed");
        }
        sockaddr_in bound{};
        SockLen length = sizeof(bound);
        if (getsockname(handle_.socket, reinterpret_cast<sockaddr*>(&bound), &length) != 0) {
            throw std::runtime_error("TCP getsockname failed");
        }
        address_ = address;
        address_.port = ntohs(bound.sin_port);
    }

    Result accept(std::unique_ptr<TcpStream>& stream) override {
        Handle s(::accept(handle_.socket, nullptr, nullptr));
        if (s.socket == Invalid) {
            return blocked() ? Result::WouldBlock : Result::IoError;
        }
        stream = std::make_unique<Stream>(runtime_, std::move(s), false);
        return Result::Ok;
    }

    IPv4Address address() const override {
        return address_;
    }

private:
    std::shared_ptr<Runtime> runtime_;
    Handle handle_;
    IPv4Address address_;
};
}

std::unique_ptr<TcpListener> makeTcpListener(IPv4Address address) {
    return std::make_unique<Listener>(address);
}

std::unique_ptr<TcpStream> makeTcpConnection(IPv4Address address) {
    if (!address.port) {
        throw std::invalid_argument("TCP port required");
    }
    auto runtime = std::make_shared<Runtime>();
    Handle socket(::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
    if (socket.socket == Invalid || !nonblocking(socket.socket)) {
        throw std::runtime_error("TCP socket setup failed");
    }
    const auto a = native(address);
    const auto result = ::connect(socket.socket, reinterpret_cast<const sockaddr*>(&a), sizeof(a));
    if (result != 0 && !blocked()) {
        throw std::runtime_error("TCP connect failed");
    }
    return std::make_unique<Stream>(std::move(runtime), std::move(socket), result != 0);
}
#endif
}
