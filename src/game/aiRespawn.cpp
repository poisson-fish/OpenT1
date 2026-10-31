#include <opent1/game/ai.hpp>
#include <opent1/game/string_table.hpp>

#include <format>

namespace opent1::game {

Player* AIManager::spawnRespawnCorpse(AIObj& ai, bool respawnLivePlayer) {
    ++corpseCounter_;
    const std::string corpseName = std::format("Corpse{} {}", corpseCounter_, ai.name);

    TMat3F deathTransform{};
    deathTransform.identity();
    if (const Player* oldPlayer = ai.player()) {
        deathTransform = oldPlayer->getTransform();
    } else if (const Marker* m = ai.marker()) {
        deathTransform = m->getTransform();
    }

    auto corpse = std::make_unique<Player>();
    corpse->setPosition(deathTransform, true);
    corpse->setTeam(ai.team);
    corpse->setDead(true);
    corpse->setType(corpse->getType() | CorpseObjectType);

    Player* corpsePtr = corpse.get();
    if (auto* mgr = getManager()) {
        if (mgr->addObject(std::move(corpse))) {
            corpsePtr->assignName(corpseName);
            corpsePtr->addToSet(SimCleanupSetId);
        } else {
            corpsePtr = nullptr;
        }
    } else {
        corpsePtr = nullptr;
    }

    ai.invokeCallback(AIObj::AIKilled, corpseName);

    if (respawnLivePlayer && ai.player()) {
        Player* pl = ai.player();
        pl->setDead(false);
        pl->setDamageLevel(0.0f);
        if (const Marker* m = ai.marker()) {
            pl->setPosition(m->getTransform(), true);
        }
    }

    return corpsePtr;
}

} // namespace opent1::game
