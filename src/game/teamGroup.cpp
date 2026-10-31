#include <opent1/game/player_manager.hpp>
#include <opent1/game/team_group.hpp>

#include <format>

namespace opent1::game {

bool TeamGroup::onAdd() {
    PlayerManager* playerManager = nullptr;
    if (auto* mgr = getManager()) {
        playerManager = dynamic_cast<PlayerManager*>(mgr->findObject(PlayerManagerId));
    }
    if (!playerManager) {
        return false;
    }
    if (playerManager->getNumTeams() < 0) {
        playerManager->teamAdded(-1);
    }
    if (playerManager->getNumTeams() >= PlayerManager::MaxTeams) {
        teamId_ = PlayerManager::MaxTeams - 1;
        return Parent::onAdd();
    }
    teamId_ = playerManager->getNumTeams();
    const std::string name = std::format("team{}", teamId_);
    assignName(name);
    playerManager->teamAdded(teamId_);
    return Parent::onAdd();
}

} // namespace opent1::game
