#pragma once
#include <opent1/platform/net_transport.hpp>

namespace opent1::net {
struct StreamResult {
    Result result;
    std::size_t count{};
};

// Move-only ownership through unique_ptr. Each operation is nonblocking and may
// transfer a prefix; callers retain unsent bytes. Native handles never escape.
class TcpStream {
public:
    virtual ~TcpStream() = default;
    virtual Result connected() = 0;
    virtual StreamResult read(std::span<std::byte>) = 0;
    virtual StreamResult write(std::span<const std::byte>) = 0;
    virtual void close() = 0;
};

class TcpListener {
public:
    virtual ~TcpListener() = default;
    virtual Result accept(std::unique_ptr<TcpStream>&) = 0;
    [[nodiscard]] virtual IPv4Address address() const = 0;
};

[[nodiscard]] std::unique_ptr<TcpListener> makeTcpListener(IPv4Address bindAddress = {
                                                               {127, 0, 0, 1}, 0});
// Numeric IPv4 only; host DNS resolution belongs outside the frame loop.
[[nodiscard]] std::unique_ptr<TcpStream> makeTcpConnection(IPv4Address);
}
