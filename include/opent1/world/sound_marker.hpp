#pragma once
#include <opent1/world/scene_objects.hpp>
#include <opent1/sim/sim_audio.hpp>

namespace opent1::world {
// Prepare pose, material handles and spatial bounds before publishing the host.
// Queries copy owned render images; removal cannot invalidate an earlier image.
bool installSoundMarker(sim::SimSoundSource&, TsShapeResources);
[[nodiscard]] std::shared_ptr<void>
installSoundSourceFactory(sim::SimGame&,
                          std::function<bool(const sim::SimSoundSequenceEvent&)>,
                          std::string marker = "cube2.dts");
}
