#pragma once
#include <opent1/world/object_plugins.hpp>

namespace opent1::world {
// Game outlives its plugin. Commands resolve the current delegate/streams on
// each call; frame drives transport and bounded native demo playback.
[[nodiscard]] ObjectHost nativeNetworkHost(sim::SimGame&, std::optional<UInt32> packetFrame = 32);
}
