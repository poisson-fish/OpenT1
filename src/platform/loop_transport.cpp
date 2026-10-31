#include <opent1/platform/net_transport.hpp>
#include <algorithm>
#include <deque>
#include <stdexcept>

namespace opent1::net {
namespace {
struct Link {
    std::array<std::deque<Datagram>, 2> queues;
    std::array<bool, 2> open{true, true};
    std::size_t capacity;

    explicit Link(std::size_t size) : capacity(size) {}
};

class LoopTransport final : public Transport {
public:
    LoopTransport(std::shared_ptr<Link> link, std::size_t side)
        : link_(std::move(link)), side_(side) {}

    ~LoopTransport() override {
        close();
    }

    Result send(std::string_view address, std::span<const std::byte> data) override {
        if (!link_->open[side_] || !link_->open[1 - side_]) {
            return Result::Closed;
        }
        if (address != "LOOPBACK" && address != name(1 - side_)) {
            return Result::InvalidAddress;
        }
        if (data.empty() || data.size() > MaxDatagram) {
            return Result::TooLarge;
        }
        auto& queue = link_->queues[1 - side_];
        if (queue.size() == link_->capacity) {
            return Result::WouldBlock;
        }
        Datagram packet;
        packet.address = localAddress();
        packet.size = data.size();
        std::ranges::copy(data, packet.bytes.begin());
        queue.push_back(std::move(packet));
        return Result::Ok;
    }

    Result receive(Datagram& packet) override {
        auto& queue = link_->queues[side_];
        if (!link_->open[side_]) {
            return Result::Closed;
        }
        if (queue.empty()) {
            return link_->open[1 - side_] ? Result::WouldBlock : Result::Closed;
        }
        packet = std::move(queue.front());
        queue.pop_front();
        return Result::Ok;
    }

    std::string localAddress() const override {
        return name(side_);
    }

    std::string canonicalAddress(std::string_view address) const override {
        return address == "LOOPBACK" || address == name(1 - side_) ? name(1 - side_)
                                                                   : std::string{};
    }

    void close() override {
        link_->open[side_] = false;
        link_->queues[side_].clear();
    }

private:
    static std::string name(std::size_t side) {
        return side == 0 ? "LOOPBACK:1" : "LOOPBACK:2";
    }

    std::shared_ptr<Link> link_;
    std::size_t side_;
};
}

std::pair<std::unique_ptr<Transport>, std::unique_ptr<Transport>>
makeLoopTransportPair(std::size_t capacity) {
    if (capacity == 0 || capacity > 65536) {
        throw std::invalid_argument("Invalid loop queue capacity");
    }
    auto link = std::make_shared<Link>(capacity);
    return {std::make_unique<LoopTransport>(link, 0), std::make_unique<LoopTransport>(link, 1)};
}
} // namespace opent1::net
