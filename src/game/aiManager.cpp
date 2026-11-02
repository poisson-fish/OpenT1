#include <opent1/game/ai.hpp>
#include <opent1/core/text.hpp>
#include <opent1/game/string_table.hpp>
#include <opent1/game/data_block_manager.hpp>

#include <algorithm>

namespace opent1::game {

namespace {

constexpr std::array<std::string_view, 11> nativeVariables{"iq",
                                                           "pathType",
                                                           "spotDist",
                                                           "seekOff",
                                                           "attackMode",
                                                           "triggerWindup",
                                                           "triggerPct",
                                                           "smartGuyWeaponRange",
                                                           "dumbGuyWeaponRange",
                                                           "smartGuyMinAccuracy",
                                                           "jetNavigation"};

void removeVariables(Console* console, std::string_view name) {
    if (console)
        for (const auto variable : nativeVariables)
            console->removeVariable("AI::" + std::string(name) + "::" + std::string(variable));
}
} // namespace

AIManager::AIManager() = default;

AIManager::~AIManager() = default;

bool AIManager::onAdd() {
    if (!Parent::onAdd()) {
        return false;
    }
    if (auto* mgr = getManager()) {
        mgr->assignId(*this, AIManagerId);
    }
    return true;
}

void AIManager::onRemove() {
    for (auto& ai : aiList_) {
        removeVariables(getManager() ? getManager()->console() : nullptr, ai->name);
        if (getManager() && ai->scriptObjectId) {
            if (auto* object = getManager()->findObject(ai->scriptObjectId))
                clearNotify(*object);
        }
        if (ai->player()) {
            clearNotify(*ai->player());
        }
        if (ai->marker()) {
            clearNotify(*ai->marker());
        }
    }
    aiList_.clear();
    Parent::onRemove();
}

void AIManager::onDeleteNotify(sim::SimObject& object) {
    for (const auto& ai : aiList_) {
        if (ai->scriptObjectId == object.getId()) {
            const auto name = ai->name;
            ai->scriptObjectId = 0;
            remove(name, true);
            Parent::onDeleteNotify(object);
            return;
        }
    }
    for (auto& ai : aiList_) {
        if (ai->player() == &object) {
            ai->setPlayer(nullptr);
        }
        if (ai->marker() == &object) {
            ai->setMarker(nullptr);
        }
    }
    Parent::onDeleteNotify(object);
}

bool AIManager::processEvent(const sim::SimEvent& event) {
    return Parent::processEvent(event);
}

bool AIManager::add(std::string_view aiName,
                    std::string_view markerName,
                    Int32 teamId,
                    std::string_view skinBase) {
    auto* mgr = getManager();
    auto* players = PlayerManager::get(mgr);
    if (!players || aiName.empty() || findAI(aiName) != nullptr) {
        return false;
    }

    auto ai = std::make_unique<AIObj>(this);
    ai->id = players->aiAdded(aiName, "male1", skinBase, true);
    if (!ai->id) {
        return false;
    }
    ai->name.assign(aiName.data(), aiName.size());
    ai->team = teamId;
    ai->skinBase.assign(skinBase.data(), skinBase.size());

    if (!markerName.empty()) {
        if (auto* m = dynamic_cast<Marker*>(mgr->findObject(markerName))) {
            ai->setMarker(m);
            deleteNotify(*m);
            ai->setCurDest(m->getLinearPosition());
        }
    }
    players->serverClientTeamChange(ai->id, teamId);

    aiList_.push_back(std::move(ai));
    return true;
}

bool AIManager::spawn(Console& console,
                      std::string_view aiName,
                      std::string_view armor,
                      const Point3F& position,
                      const Point3F& rotation,
                      std::string_view displayName,
                      std::string_view voice) {
    auto* manager = getManager();
    auto* players = PlayerManager::get(manager);
    auto* blocks = manager
                       ? dynamic_cast<DataBlockManager*>(manager->findObject(DataBlockManagerId))
                       : nullptr;
    auto* cleanup =
        manager ? dynamic_cast<sim::SimGroup*>(manager->findObject("MissionCleanup")) : nullptr;
    Int32 dataId = -1;
    auto* data = blocks ? dynamic_cast<PlayerData*>(blocks->lookupDataBlock(
                              armor, DataBlockManager::PlayerDataType, &dataId))
                        : nullptr;
    if (!players || !cleanup || !data || aiName.empty() || findAI(aiName))
        return false;
    const std::string shown = displayName.empty() ? std::string(aiName) : std::string(displayName);
    auto body = std::make_unique<Player>();
    body->bindDataBlockManager(blocks);
    body->bindConsole(&console);
    body->setDatFileId(dataId);
    body->setDatFileName(armor);
    body->setAiControlled(true);
    body->assignName(shown);
    TMat3F transform(EulerF(rotation.x, rotation.y, rotation.z), position);
    body->setPosition(transform, true);
    if (!body->initResources(data))
        return false;
    auto* player = dynamic_cast<Player*>(manager->addObject(std::move(body), cleanup));
    if (!player)
        return false;
    player->setPosition(transform, true);
    player->setRot(rotation);
    player->setName(shown, &console);
    auto ai = std::make_unique<AIObj>(this);
    ai->name = aiName;
    ai->voice = voice;
    ai->spawnBodyOnTick = false;
    ai->setPlayer(player);
    auto handle = std::make_unique<sim::SimObject>();
    handle->setType(AIObjectType);
    auto* object = manager->addObject(std::move(handle), cleanup);
    if (!object) {
        manager->deleteObject(*player);
        return false;
    }
    ai->scriptObjectId = object->getId();
    ai->id = players->aiAdded(
        shown, voice, "base", voice.size() >= 4 && equalIgnoreCase(voice.substr(0, 4), "male"));
    if (ai->id <= 0) {
        manager->deleteObject(*object);
        manager->deleteObject(*player);
        return false;
    }
    player->setOwnerClient(ai->id);
    ai->setCurDest(position);
    deleteNotify(*player);
    deleteNotify(*object);
    aiList_.push_back(std::move(ai));
    for (const auto variable : nativeVariables) {
        const auto name = std::string(aiName), field = std::string(variable);
        console.bindVariable(
            "AI::" + name + "::" + field,
            [this, name, field] {
                const auto* value = findAI(name);
                return value ? value->getVar(field) : std::string{};
            },
            [this, name, field](std::string_view value) {
                if (auto* target = findAI(name))
                    target->setVar(field, value);
            });
    }
    return true;
}

bool AIManager::remove(std::string_view aiName, bool deletePlayer) {
    for (std::size_t i = 0; i < aiList_.size(); ++i) {
        if (equalIgnoreCase(aiList_[i]->name, aiName)) {
            AIObj& ai = *aiList_[i];
            removeVariables(getManager() ? getManager()->console() : nullptr, ai.name);
            if (auto* manager = getManager(); manager && ai.scriptObjectId) {
                if (auto* object = manager->findObject(ai.scriptObjectId)) {
                    clearNotify(*object);
                    ai.scriptObjectId = 0;
                    manager->deleteObject(*object);
                }
            }
            if (auto* mgr = getManager()) {
                if (auto* pm = dynamic_cast<PlayerManager*>(mgr->findObject(PlayerManagerId))) {
                    pm->clientDropped(ai.id);
                }
            }
            if (ai.marker()) {
                clearNotify(*ai.marker());
            }
            if (Player* pl = ai.player()) {
                clearNotify(*pl);
                if (deletePlayer && getManager() != nullptr) {
                    pl->deleteObject();
                }
            }
            aiList_.erase(aiList_.begin() + static_cast<std::ptrdiff_t>(i));
            return true;
        }
    }
    return false;
}

Int32 AIManager::removeMatched(std::string_view aiExpr, bool deletePlayer) {
    std::vector<AIObj*> matched{};
    buildExpressionList(matched, aiExpr);
    std::vector<std::string> names{};
    names.reserve(matched.size());
    for (const AIObj* ai : matched) {
        names.push_back(ai->name);
    }
    Int32 removed = 0;
    for (const std::string& nm : names) {
        if (remove(nm, deletePlayer)) {
            ++removed;
        }
    }
    return removed;
}

AIObj* AIManager::findAI(std::string_view aiName) const noexcept {
    const auto found = std::ranges::find_if(aiList_, [&](const auto& ai) {
        return equalIgnoreCase(ai->name, aiName);
    });
    return found == aiList_.end() ? nullptr : found->get();
}

AIObj* AIManager::findByPlayerId(Int32 playerId) const noexcept {
    const auto found = std::ranges::find(aiList_, playerId, &AIObj::id);
    return found == aiList_.end() ? nullptr : found->get();
}

AIObj* AIManager::findByPlayer(const Player* player) const noexcept {
    if (!player) {
        return nullptr;
    }
    const auto found = std::ranges::find_if(aiList_, [&](const auto& ai) {
        return ai->player() == player;
    });
    return found == aiList_.end() ? nullptr : found->get();
}

void AIManager::buildExpressionList(std::vector<AIObj*>& outList, std::string_view expr) const {
    outList.clear();
    for (const auto& ai : aiList_) {
        if (AIObj::wildCardMatch(ai->name, expr)) {
            outList.push_back(ai.get());
        }
    }
}

void AIManager::doServerProcesses(SimTime curTime) {
    // Deleting a body initiates removal on the next AI pass, not a death callback.
    std::vector<std::string> removed;
    for (const auto& ai : aiList_) {
        if (!ai->player() && !ai->spawnBodyOnTick) {
            removed.push_back(ai->name);
        }
    }
    for (const auto& name : removed) {
        remove(name, false);
    }
    std::vector<Int32> identities;
    for (const auto& ai : aiList_) {
        identities.push_back(ai->id);
    }
    const auto flushCallbacks = [&] {
        auto callbacks = std::move(pendingCallbacks_);
        pendingCallbacks_.clear();
        for (const auto& callback : callbacks) {
            if (auto* manager = getManager())
                if (auto* console = manager->console();
                    console && console->isFunction(callback.function)) {
                    if (callback.extraArg.empty()) {
                        (void)console->execute(callback.function,
                                               std::array<std::string, 1>{callback.aiName});
                    } else {
                        (void)console->execute(
                            callback.function,
                            std::array<std::string, 2>{callback.aiName, callback.extraArg});
                    }
                }
        }
    };
    for (const auto clientId : identities) {
        if (auto* ai = findByPlayerId(clientId); ai && ai->player()) {
            processing_ = true;
            ai->doServerProcesses(curTime);
            processing_ = false;
            flushCallbacks();
        }
    }
    // Original post-loop phase runs after every AI has generated its move.
    for (const auto clientId : identities) {
        auto* ai = findByPlayerId(clientId);
        if (!ai || !ai->player()) {
            continue;
        }
        ++ai->wrappedFireAngle_;
        if (ai->player()->isDead() && !ai->wasDead_) {
            ai->wasDead_ = true;
            ai->invokeCallback(AIObj::AIKilled);
            ai = findByPlayerId(clientId);
            if (!ai) {
                continue;
            }
            removeVariables(getManager() ? getManager()->console() : nullptr, ai->name);
            ai->name = ("Corpse" + std::to_string(corpseCounter_++) + " " + ai->name).substr(0, 31);
        }
        if (ai->periodicTicks_ >= 32 && ++ai->periodicCounter_ >= ai->periodicTicks_) {
            ai->periodicCounter_ = 0;
            ai->invokeCallback(AIObj::Periodic);
        }
    }
}

void AIManager::recordCallback(std::string_view fn,
                               std::string_view aiName,
                               std::string_view extraArg) {
    callbackHistory_.push_back(CallbackRecord{
        std::string(fn),
        std::string(aiName),
        std::string(extraArg),
    });

    if (processing_) {
        pendingCallbacks_.push_back(callbackHistory_.back());
        return;
    }
    if (auto* mgr = getManager()) {
        if (auto* con = mgr->console()) {
            if (con->isFunction(fn)) {
                if (extraArg.empty()) {
                    const std::array<std::string, 1> argv{std::string(aiName)};
                    (void)con->execute(fn, argv);
                } else {
                    const std::array<std::string, 2> argv{std::string(aiName),
                                                          std::string(extraArg)};
                    (void)con->execute(fn, argv);
                }
            }
        }
    }
}

} // namespace opent1::game
