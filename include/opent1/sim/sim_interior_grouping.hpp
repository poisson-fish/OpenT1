#pragma once
#include <opent1/sim/sim_world_game.hpp>

namespace opent1::sim {
struct InteriorGroupingScript {
    std::vector<std::string> lines;
    static std::unique_ptr<InteriorGroupingScript> load(StreamIO&, UInt32);
};

// Legacy EMF files describe a base interior plus named groups and positioned
// simulation objects. The host supplies its normal class factories.
class SimInteriorGrouping final : public SimGroup {
public:
    explicit SimInteriorGrouping(SimGame* game = nullptr) : game_(game) {}

    bool processArguments(std::span<const std::string>) override;

private:
    SimGame* game_{};
};

bool registerInteriorGroupingResource(ResourceManager&);
} // namespace opent1::sim
