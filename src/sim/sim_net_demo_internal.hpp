#pragma once
#include <opent1/sim/sim_net.hpp>

namespace opent1::sim::network {
struct PacketStream::Demo {
    static constexpr std::size_t MaxBytes = 64 * 1024 * 1024;
    DemoState state{DemoState::Idle};
    DemoTiming timing{};
    std::vector<std::byte> bytes;
    std::vector<SimAddress> clients;
    std::size_t cursor{8};
    UInt32 offset{}, lastTime{};
    bool busy{};
};
}
