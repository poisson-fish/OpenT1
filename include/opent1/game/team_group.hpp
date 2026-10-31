#pragma once

#include <opent1/core/types.hpp>
#include <opent1/sim/sim_base.hpp>

#include <string_view>

namespace opent1::game {

inline constexpr std::string_view FearMissionTeams = "MissionGroup/Teams";

class TeamGroup : public sim::SimGroup {
    using Parent = sim::SimGroup;

public:
    TeamGroup() = default;
    ~TeamGroup() override = default;

    [[nodiscard]] Int32 getTeam() const noexcept {
        return teamId_;
    }

    void setTeam(Int32 id) noexcept {
        teamId_ = id;
    }

    bool onAdd() override;

private:
    Int32 teamId_{-1};
};

} // namespace opent1::game
