#include <opent1/game/fear_player_psc.hpp>
#include <opent1/game/player.hpp>
#include <opent1/game/sim_move_obj.hpp>
#include <opent1/game/commander_huds.hpp>
#include <opent1/game/observer_camera.hpp>
#include <opent1/core/mem_stream.hpp>
#include <algorithm>
#include <cmath>
#include <cstring>

namespace opent1::game {

namespace {

struct BitStreamRollbackGuard {
    BitStream& stream;
    Int32 savedBitPos;
    bool committed{false};

    explicit BitStreamRollbackGuard(BitStream& s) noexcept
        : stream(s), savedBitPos(s.getCurPos()) {}

    ~BitStreamRollbackGuard() noexcept {
        if (!committed) {
            stream.setCurPos(savedBitPos);
        }
    }
};

} // namespace

// -----------------------------------------------------------------------------
// PlayerMove
// -----------------------------------------------------------------------------

void PlayerMove::reset() noexcept {
    forwardAction = 0.0f;
    backwardAction = 0.0f;
    leftAction = 0.0f;
    rightAction = 0.0f;
    jetting = false;
    jumpAction = false;
    crouching = false;
    turnRot = 0.0f;
    pitch = 0.0f;
    trigger = false;
    useItem = -1;
}

void PlayerMove::write(BitStream& stream, bool full, const PlayerMove* prev) const {
    bool writeInfo = true;
    if (prev != nullptr) {
        writeInfo = !stream.writeFlag(
            forwardAction == prev->forwardAction && backwardAction == prev->backwardAction &&
            leftAction == prev->leftAction && rightAction == prev->rightAction &&
            jetting == prev->jetting && jumpAction == prev->jumpAction &&
            crouching == prev->crouching);
    }
    if (writeInfo) {
        stream.writeInt(static_cast<Int32>(forwardAction * 15.0f), 4);
        stream.writeInt(static_cast<Int32>(backwardAction * 15.0f), 4);
        stream.writeInt(static_cast<Int32>(leftAction * 15.0f), 4);
        stream.writeInt(static_cast<Int32>(rightAction * 15.0f), 4);
        stream.writeFlag(jetting);
        stream.writeFlag(jumpAction);
        stream.writeFlag(crouching);
    }
    if (full) {
        stream.writeFlag(trigger);
        if (stream.writeFlag(useItem != -1)) {
            stream.writeInt(useItem, 8);
        }
        if (stream.writeFlag(pitch != 0.0f)) {
            stream.write(pitch);
        }
        if (stream.writeFlag(turnRot != 0.0f)) {
            stream.write(turnRot);
        }
    }
}

bool PlayerMove::read(BitStream& stream, bool full, const PlayerMove* prev) {
    BitStreamRollbackGuard guard(stream);
    const PlayerMove saved = *this;

    bool readInfo = true;
    if (prev != nullptr) {
        readInfo = !stream.readFlag();
    }
    if (!readInfo) {
        forwardAction = prev->forwardAction;
        backwardAction = prev->backwardAction;
        leftAction = prev->leftAction;
        rightAction = prev->rightAction;
        jetting = prev->jetting;
        jumpAction = prev->jumpAction;
        crouching = prev->crouching;
    } else {
        forwardAction = static_cast<Flt32>(stream.readInt(4)) / 15.0f;
        backwardAction = static_cast<Flt32>(stream.readInt(4)) / 15.0f;
        leftAction = static_cast<Flt32>(stream.readInt(4)) / 15.0f;
        rightAction = static_cast<Flt32>(stream.readInt(4)) / 15.0f;
        jetting = stream.readFlag();
        jumpAction = stream.readFlag();
        crouching = stream.readFlag();
    }

    if (full) {
        trigger = stream.readFlag();
        if (stream.readFlag()) {
            useItem = stream.readInt(8);
        } else {
            useItem = -1;
        }
        if (stream.readFlag()) {
            UInt32 upitch = 0;
            if (!stream.read(upitch)) {
                *this = saved;
                return false;
            }
            if (((upitch >> 23) & 0xFFu) == 0xFFu) {
                upitch = 0;
            }
            std::memcpy(&pitch, &upitch, sizeof(Flt32));
            if (pitch > kMaxTurnSpeed) {
                pitch = kMaxTurnSpeed;
            } else if (pitch < -kMaxTurnSpeed) {
                pitch = -kMaxTurnSpeed;
            }
        } else {
            pitch = 0.0f;
        }

        if (stream.readFlag()) {
            UInt32 uturnRot = 0;
            if (!stream.read(uturnRot)) {
                *this = saved;
                return false;
            }
            if (((uturnRot >> 23) & 0xFFu) == 0xFFu) {
                uturnRot = 0;
            }
            std::memcpy(&turnRot, &uturnRot, sizeof(Flt32));
            if (turnRot > kMaxTurnSpeed) {
                turnRot = kMaxTurnSpeed;
            } else if (turnRot < -kMaxTurnSpeed) {
                turnRot = -kMaxTurnSpeed;
            }
        } else {
            turnRot = 0.0f;
        }
    } else {
        trigger = false;
        useItem = -1;
        pitch = 0.0f;
        turnRot = 0.0f;
    }

    if (!stream.isValid()) {
        *this = saved;
        return false;
    }

    guard.committed = true;
    return true;
}

// -----------------------------------------------------------------------------
// PlayerPSC
// -----------------------------------------------------------------------------

void PlayerPSC::validateFov() noexcept {
    if (playerFov < MinFovDeg) {
        playerFov = MinFovDeg;
    } else if (playerFov > MaxFovDeg) {
        playerFov = MaxFovDeg;
    }
    if (sniperFov < MinFovDeg) {
        sniperFov = MinFovDeg;
    } else if (sniperFov > MaxFovDeg) {
        sniperFov = MaxFovDeg;
    }
    if (zoomSpeed < 0.0f) {
        zoomSpeed = 0.0f;
    } else if (zoomSpeed > 1.0f) {
        zoomSpeed = 1.0f;
    }
}

PlayerPSC::PlayerPSC(bool inIsServer) : isServer_(inIsServer) {
    if (!isServer_) {
        setId(PlayerPSCId);
    }
    curMove_.reset();
    playbackMove_.reset();
    yawHandler_.reset();
    pitchHandler_.reset();
}

PlayerPSC::~PlayerPSC() = default;

void PlayerPSC::setControlObject(GameBase* obj) noexcept {
    if (controlObject_ == obj) {
        return;
    }
    if (controlObject_) {
        clearNotify(*controlObject_);
        if (!isServer_) {
            controlObject_->setPlayerControl(false);
        }
    }
    controlObject_ = obj;
    controlPlayer_ = dynamic_cast<Player*>(obj);
    if (controlObject_) {
        deleteNotify(*controlObject_);
        if (!isServer_) {
            controlObject_->setPlayerControl(true);
        }
    }
}

void PlayerPSC::toggleObserverCamera() {
    fInObserverMode_ = !fInObserverMode_;
    if (fInObserverMode_) {
        fInEditMode_ = false;
    }
}

void PlayerPSC::toggleEditCamera() {
    fInEditMode_ = !fInEditMode_;
    if (fInEditMode_) {
        fInObserverMode_ = false;
    }
}

void PlayerPSC::setTargetFov(Flt32 fov, UInt32 currentTimeMs) noexcept {
    validateFov();
    const Flt32 clamped = std::clamp(fov, MinFovDeg, MaxFovDeg);
    if (targetFov_ == clamped) {
        return;
    }
    startFov_ = curFov_;
    targetFov_ = clamped;
    fovStartTime_ = currentTimeMs;
    const Flt32 deltaFov = std::fabs(targetFov_ - startFov_);
    const auto zoomMs = static_cast<UInt32>(zoomSpeed * deltaFov * (1000.0f / 90.0f));
    fovTargetTime_ = currentTimeMs + zoomMs;
    if (fovTargetTime_ <= fovStartTime_) {
        curFov_ = targetFov_;
    }
}

void PlayerPSC::updateFov(UInt32 currentTimeMs) noexcept {
    if (curFov_ == targetFov_) {
        return;
    }
    if (currentTimeMs >= fovTargetTime_ || fovTargetTime_ <= fovStartTime_) {
        curFov_ = targetFov_;
    } else {
        const Flt32 frac = static_cast<Flt32>(currentTimeMs - fovStartTime_) /
                           static_cast<Flt32>(fovTargetTime_ - fovStartTime_);
        curFov_ = startFov_ + frac * (targetFov_ - startFov_);
    }
}

void PlayerPSC::clientCollectInput(UInt32 startTime, UInt32 endTime, Flt32 explicitInputDt) {
    clientInputTime_ = endTime;
    updateFov(endTime);
    if (owner() && owner()->demoState() == sim::network::PacketStream::DemoState::Playback) {
        return;
    }
    const bool timedMoves = moveTimes_.size() == moves_.size();
    if (!timedMoves) {
        moveTimes_.clear();
    }
    // A move belongs to the tick containing the frame, including its first
    // partial interval. Waiting until the tick ends leaves prediction empty.
    const auto tickCount = [](UInt32 time) {
        return (time >> 5) + UInt32((time & 31u) != 0);
    };
    auto tick = tickCount(startTime);
    auto ticks = tickCount(endTime) - tick;
    validateFov();
    // Camera presentation follows the rendered frame, including frames that
    // collect no new 32 ms simulation tick.
    const auto cameraStep = CamSpeed * Flt32(endTime - startTime) * .001f;
    camDist_ = firstPerson_ ? std::max(0.f, camDist_ - cameraStep)
                            : std::min(Flt32(CamMaxDist), camDist_ + cameraStep);
    while (ticks-- > 0) {
        if (moves_.size() >= MaxMoves) {
            break;
        }
        const auto moveTime = (tick++ << 5) + 1u;
        PlayerMove pm = curMove_;

        const Flt32 pitchScale = (controlObject_ && !controlObject_->mousePitch()) ? 0.0f : 1.0f;
        const Flt32 yawVal = (explicitInputDt >= 0.0f) ? yawHandler_.getInput(explicitInputDt)
                                                       : yawHandler_.getInput();
        const Flt32 pitchVal = (explicitInputDt >= 0.0f) ? pitchHandler_.getInput(explicitInputDt)
                                                         : pitchHandler_.getInput();
        pm.turnRot = yawVal;
        pm.pitch = pitchVal * pitchScale;

        if (sneak_) {
            pm.forwardAction = std::clamp(pm.forwardAction * 0.5f, 0.0f, 1.0f);
            pm.backwardAction = std::clamp(pm.backwardAction * 0.5f, 0.0f, 1.0f);
            pm.leftAction = std::clamp(pm.leftAction * 0.5f, 0.0f, 1.0f);
            pm.rightAction = std::clamp(pm.rightAction * 0.5f, 0.0f, 1.0f);
        } else {
            pm.forwardAction = std::clamp(pm.forwardAction, 0.0f, 1.0f);
            pm.backwardAction = std::clamp(pm.backwardAction, 0.0f, 1.0f);
            pm.leftAction = std::clamp(pm.leftAction, 0.0f, 1.0f);
            pm.rightAction = std::clamp(pm.rightAction, 0.0f, 1.0f);
        }

        if ((triggerCount_ & 1) != 0) {
            pm.trigger = true;
        } else if (prevTriggerCount_ != triggerCount_ && prevTriggerCount_ + 1 < triggerCount_) {
            pm.trigger = true;
        } else {
            pm.trigger = false;
        }
        prevTriggerCount_ = triggerCount_;

        curMove_.useItem = -1;
        curMove_.jumpAction = false;

        if (moves_.size() < MaxMoves) {
            // Legacy round-trips each move through the wire codec before using
            // it for prediction. In particular, half speed is 7/15 on the
            // server, and view deltas are clamped by PlayerMove::read.
            std::array<std::byte, 64> moveBytes{};
            BitStream encoded{std::span<std::byte>(moveBytes)};
            pm.write(encoded);
            BitStream decoded{std::span<const std::byte>(moveBytes)};
            (void)pm.read(decoded);
            moves_.push_back(pm);
            if (timedMoves) {
                moveTimes_.push_back(moveTime - 1u);
            }
            if (owner() &&
                owner()->demoState() == sim::network::PacketStream::DemoState::Recording) {
                std::array<std::byte, 33> bytes{};
                MemRWStream record{std::span(bytes)};
                record.write(UInt8{1});
                record.write(pm.forwardAction);
                record.write(pm.backwardAction);
                record.write(pm.leftAction);
                record.write(pm.rightAction);
                record.write(UInt8(pm.jetting));
                record.write(UInt8(pm.crouching));
                record.write(UInt8(pm.jumpAction));
                record.write(UInt8(pm.trigger));
                record.write(pm.useItem);
                record.write(pm.turnRot);
                record.write(pm.pitch);
                (void)owner()->recordClient(*this, bytes, moveTime);
            }
        }
    }
    // Scripted/recorded moves have no timestamp until input collection maps
    // them onto the current tick, as the original relative lookup did.
    if (!timedMoves) {
        const auto lastTick = (endTime ? endTime - 1u : 0u) & ~31u;
        for (std::size_t i = 0; i < moves_.size(); ++i) {
            moveTimes_.push_back(lastTick - UInt32(moves_.size() - 1u - i) * 32u);
        }
    }
}

PlayerMove* PlayerPSC::getClientMove(UInt32 curTime) noexcept {
    return const_cast<PlayerMove*>(std::as_const(*this).getClientMove(curTime));
}

const PlayerMove* PlayerPSC::getClientMove(UInt32 curTime) const noexcept {
    if (!controlObject_) {
        return nullptr;
    }
    if (moveTimes_.size() == moves_.size()) {
        const auto found = std::find(moveTimes_.begin(), moveTimes_.end(), curTime & ~31u);
        return found == moveTimes_.end() ? nullptr
                                         : &moves_[std::size_t(found - moveTimes_.begin())];
    }
    const Int32 movesBack =
        static_cast<Int32>((clientInputTime_ ? (clientInputTime_ - 1u) >> 5 : 0u)) -
        static_cast<Int32>(curTime >> 5);
    const Int32 moveIndex = static_cast<Int32>(moves_.size()) - 1 - movesBack;
    if (moveIndex < 0 || moveIndex >= static_cast<Int32>(moves_.size())) {
        return nullptr;
    }
    return &moves_[static_cast<std::size_t>(moveIndex)];
}

const PlayerMove* PlayerPSC::playerMove(Int32 index) const noexcept {
    const Int32 idx = static_cast<Int32>(moves_.size()) - lastPlayerMoveCount_ + index;
    if (idx < 0 || idx >= static_cast<Int32>(moves_.size())) {
        return nullptr;
    }
    return &moves_[static_cast<std::size_t>(idx)];
}

void PlayerPSC::setInShoppingList(Int32 item) noexcept {
    if (item < 0 || item >= MaxItemTypes) {
        return;
    }
    const auto word = static_cast<std::size_t>(item >> 5);
    const UInt32 bit = 1u << (item & 31);
    shoppingList_[word] |= bit;
}

void PlayerPSC::clearFromShoppingList(Int32 item) noexcept {
    if (item < 0 || item >= MaxItemTypes) {
        return;
    }
    const auto word = static_cast<std::size_t>(item >> 5);
    const UInt32 bit = ~(1u << (item & 31));
    shoppingList_[word] &= bit;
    clearFromBuyList(item);
}

bool PlayerPSC::isInShoppingList(Int32 item) const noexcept {
    if (item < 0 || item >= MaxItemTypes) {
        return false;
    }
    const auto word = static_cast<std::size_t>(item >> 5);
    const UInt32 bit = 1u << (item & 31);
    return (shoppingList_[word] & bit) != 0;
}

void PlayerPSC::clearShoppingList() noexcept {
    shoppingList_.fill(0);
    clearBuyList();
}

void PlayerPSC::setInBuyList(Int32 item) noexcept {
    setInShoppingList(item);
    if (item == -1) {
        buyList_.fill(0xFFFFFFFFu);
        return;
    }
    if (item < 0 || item >= MaxItemTypes) {
        return;
    }
    const auto word = static_cast<std::size_t>(item >> 5);
    const UInt32 bit = 1u << (item & 31);
    buyList_[word] |= bit;
}

void PlayerPSC::clearFromBuyList(Int32 item) noexcept {
    if (item < 0 || item >= MaxItemTypes) {
        return;
    }
    const auto word = static_cast<std::size_t>(item >> 5);
    const UInt32 bit = ~(1u << (item & 31));
    buyList_[word] &= bit;
}

bool PlayerPSC::isInBuyList(Int32 item) const noexcept {
    if (item < 0 || item >= MaxItemTypes) {
        return false;
    }
    const auto word = static_cast<std::size_t>(item >> 5);
    const UInt32 bit = 1u << (item & 31);
    return (buyList_[word] & bit) != 0;
}

void PlayerPSC::clearBuyList() noexcept {
    buyList_.fill(0);
}

Int32 PlayerPSC::itemCount(Int32 item) const noexcept {
    if (item < 0 || item >= MaxItemTypes) {
        return 0;
    }
    return itemTypeCount_[static_cast<std::size_t>(item)];
}

void PlayerPSC::setItemCount(Int32 item, Int32 count) noexcept {
    if (item < 0 || item >= MaxItemTypes) {
        return;
    }
    itemTypeCount_[static_cast<std::size_t>(item)] = static_cast<Int16>(std::max<Int32>(0, count));
}

Int32 PlayerPSC::deltaItemCount(Int32 item, Int32 delta) noexcept {
    if (item < 0 || item >= MaxItemTypes) {
        return 0;
    }
    const Int32 next = std::max<Int32>(
        0, static_cast<Int32>(itemTypeCount_[static_cast<std::size_t>(item)]) + delta);
    itemTypeCount_[static_cast<std::size_t>(item)] = static_cast<Int16>(next);
    return next;
}

void PlayerPSC::resetItems() noexcept {
    itemTypeCount_.fill(0);
    clientItemTypeCount_.fill(0);
    fReset_ = true;
}

void PlayerPSC::resetInput() noexcept {
    curMove_ = {};
    yawHandler_.reset();
    pitchHandler_.reset();
    if (triggerCount_ & 1) {
        ++triggerCount_;
    }
    prevTriggerCount_ = triggerCount_;
    setTargetFov(playerFov, clientInputTime_);
}

bool PlayerPSC::onSimActionEvent(const sim::SimActionEvent& event) {
    const Int32 action = event.action;
    const Flt32 val = event.fValue;
    if (owner() && owner()->demoState() == sim::network::PacketStream::DemoState::Recording) {
        std::array<std::byte, 9> bytes{};
        MemRWStream record{std::span(bytes)};
        record.write(UInt8{0});
        record.write(val);
        record.write(action);
        const auto now = getManager()
                             ? static_cast<UInt32>(getManager()->getCurrentTime() * 1000.0f)
                             : owner()->currentTime();
        (void)owner()->recordClient(*this, bytes, now);
    }
    switch (action) {
        case IDACTION_MOVEBACK:
            curMove_.backwardAction = val;
            return true;
        case IDACTION_MOVEFORWARD:
            curMove_.forwardAction = val;
            return true;
        case IDACTION_MOVEDOWN:
        case IDACTION_CROUCH:
            curMove_.crouching = true;
            return true;
        case IDACTION_STAND:
            curMove_.crouching = false;
            return true;
        case IDACTION_TOGGLE_CROUCH:
            curMove_.crouching = !curMove_.crouching;
            return true;
        case IDACTION_MOVEUP:
            curMove_.jumpAction = true;
            return true;
        case IDACTION_MOVELEFT:
            curMove_.leftAction = val;
            return true;
        case IDACTION_MOVERIGHT:
            curMove_.rightAction = val;
            return true;
        case IDACTION_JET:
            curMove_.jetting = (val != 0.0f);
            return true;
        case IDACTION_FIRE1:
            if ((triggerCount_ & 1) == 0) {
                ++triggerCount_;
            }
            return true;
        case IDACTION_BREAK1:
            if ((triggerCount_ & 1) != 0) {
                ++triggerCount_;
            }
            return true;
        case IDACTION_USEITEM:
            curMove_.useItem = static_cast<Int32>(val);
            return true;
        case IDACTION_YAW:
            yawHandler_.sample(val, event.device);
            return true;
        case IDACTION_PITCH:
            pitchHandler_.sample(val, event.device);
            return true;
        case IDACTION_TURNLEFT:
            yawHandler_.sample(val, sim::ActionDigital);
            return true;
        case IDACTION_TURNRIGHT:
            yawHandler_.sample(-val, sim::ActionDigital);
            return true;
        case IDACTION_LOOKUP:
            pitchHandler_.sample(val, sim::ActionDigital);
            return true;
        case IDACTION_LOOKDOWN:
            pitchHandler_.sample(-val, sim::ActionDigital);
            return true;
        case IDACTION_VIEW:
            firstPerson_ = !firstPerson_;
            return true;
        case IDACTION_RUN:
            sneak_ = (val == 0.0f);
            return true;
        case IDACTION_SNIPER_FOV:
            setTargetFov((val != 0.0f) ? sniperFov : playerFov, clientInputTime_);
            return true;
        case IDACTION_INC_SNIPER_FOV:
            zoomChangeTime_ = clientInputTime_;
            if (sniperFov <= 5.625f) {
                sniperFov = 45.0f;
            } else {
                sniperFov *= 0.5f;
            }
            if (targetFov_ != playerFov) {
                setTargetFov(sniperFov, clientInputTime_);
            }
            return true;
        default:
            return guiAction ? guiAction(event) : false;
    }
}

bool PlayerPSC::writePacket(BitStream& bstream, UInt32& key) {
    key = 0;
    if (!isServer_) {
        bstream.writeFloat(playerFov / 135.0f, 8);
        bstream.writeInt(firstMoveSeq_, 32);
        const PlayerMove* prev = nullptr;
        for (const auto& move : moves_) {
            bstream.writeFlag(true);
            move.write(bstream, true, prev);
            prev = &move;
        }
        bstream.writeFlag(false);
        return bstream.isValid();
    }

    auto* manager = getManager();
    auto* players = PlayerManager::get(manager);
    auto* me = players ? players->findClient(
                             owner() ? static_cast<Int32>(owner()->getId())
                                     : (controlObject_ ? controlObject_->getControlClient() : -1))
                       : nullptr;
    if (me) {
        guiMode_ = me->curGuiMode;
        scoresVisible_ = me->sendScores != 0;
    }
    bstream.writeInt(static_cast<Int32>(guiMode_), 3);
    bstream.writeFlag(scoresVisible_);
    bstream.writeInt(lastServerMove_ - 1, 32);
    if (!bstream.writeFlag(controlObject_ != nullptr)) {
        return bstream.isValid();
    }

    auto* ghosts = owner() ? owner()->ghostManager()
                           : (getManager() ? dynamic_cast<sim::network::GhostManager*>(
                                                 localGhostManager_.resolve(*getManager()))
                                           : nullptr);
    if (ghosts) {
        ghosts->setScopeObject(controlObject_);
    }
    controlPlayer_ = dynamic_cast<Player*>(controlObject_);
    if (!controlPlayer_) {
        if (auto* observer = dynamic_cast<ObserverCamera*>(controlObject_)) {
            controlPlayer_ = dynamic_cast<Player*>(observer->orbitObject());
        }
    }
    bstream.writeFloat(std::clamp(controlObject_->getDamageFlash(), 0.0f, 1.0f), 8);
    const auto index = ghosts ? ghosts->getGhostIndex(*controlObject_) : sim::InvalidGhostIndex;
    bstream.writeInt(index, 10);
    if (index != sim::InvalidGhostIndex) {
        if (auto* observer = dynamic_cast<ObserverCamera*>(controlObject_)) {
            observer->writePacketData(bstream, ghosts);
        } else {
            controlObject_->writePacketData(bstream);
        }
    }

    PacketHead ph{};
    if (guiMode_ == InventoryGuiMode) {
        for (std::size_t i = 0; i < ItemShoppingListWords; ++i) {
            ph.xorShoppingList[i] = shoppingList_[i] ^ clientShoppingList_[i];
            ph.xorBuyList[i] = buyList_[i] ^ clientBuyList_[i];
            if (ph.xorShoppingList[i] || ph.xorBuyList[i]) {
                bstream.writeFlag(true);
                bstream.writeInt(static_cast<Int32>(i), 4);
                if (bstream.writeFlag(ph.xorShoppingList[i] != 0)) {
                    bstream.writeInt(static_cast<Int32>(ph.xorShoppingList[i]), 32);
                }
                if (bstream.writeFlag(ph.xorBuyList[i] != 0)) {
                    bstream.writeInt(static_cast<Int32>(ph.xorBuyList[i]), 32);
                }
            }
        }
        bstream.writeFlag(false);
    }
    ph.resetting = fReset_;
    bstream.writeFlag(ph.resetting);
    for (Int32 i = 0; i < MaxItemTypes; ++i) {
        const auto slot = static_cast<std::size_t>(i);
        const Int32 count = controlPlayer_ ? controlPlayer_->getItemCount(i) : itemTypeCount_[slot];
        const Int32 previous = ph.resetting ? 0 : clientItemTypeCount_[slot];
        const Int32 delta = count - previous;
        if (delta) {
            bstream.writeFlag(true);
            bstream.writeInt(i, ItemTypeBits);
            bstream.writeSignedInt(delta, 10);
            ph.invUpdates.push_back({i, delta});
        }
    }
    bstream.writeFlag(false);

    auto* sensors =
        manager ? dynamic_cast<SensorManager*>(manager->findObject(SensorManagerId)) : nullptr;
    auto* center = manager
                       ? dynamic_cast<MissionCenterPos*>(manager->findObject(MissionCenterPosId))
                       : nullptr;
    auto* blocks = manager
                       ? dynamic_cast<DataBlockManager*>(manager->findObject(DataBlockManagerId))
                       : nullptr;
    const auto targetAt = [&](UInt32 target) -> GameBase* {
        if (target < 128) {
            auto* client =
                players ? players->findBaseRep(static_cast<Int32>(2048 + target)) : nullptr;
            return client ? client->ownedObject : nullptr;
        }
        return sensors ? sensors->target(static_cast<Int32>(target - 128)).obj : nullptr;
    };
    const auto visible = [&](GameBase* obj) {
        return obj && me && obj->getVisibleToTeam(me->team);
    };
    if (guiMode_ == CommandGuiMode) {
        const UInt32 first = (lastSentTarget_ + 8) & 255;
        UInt32 cursor = first;
        bstream.writeInt(static_cast<Int32>(first), 8);
        const Int32 start = bstream.getCurPos();
        do {
            bstream.writeFlag(true);
            for (UInt32 i = 0; i < 8; ++i) {
                const auto target = (cursor + i) & 255;
                auto* obj = targetAt(target);
                auto* client = players && target < 128
                                   ? players->findBaseRep(static_cast<Int32>(2048 + target))
                                   : nullptr;
                bool present = center && center->extent.x > 0 && center->extent.y > 0 && blocks &&
                               obj && (visible(obj) || client == me);
                Int32 x{}, y{};
                if (present) {
                    const auto pos = obj->getLinearPosition();
                    x = static_cast<Int32>(1023 * (pos.x - center->pos.x + center->extent.x / 2) /
                                           center->extent.x);
                    y = static_cast<Int32>(1023 * (pos.y - center->pos.y + center->extent.y / 2) /
                                           center->extent.y);
                    present = ((x | y) & ~2047) == 0 && obj->getDatGroup() >= 0;
                }
                if (!bstream.writeFlag(present)) {
                    continue;
                }
                bstream.writeInt(x, 11);
                bstream.writeInt(y, 11);
                bstream.writeInt(obj->getTeam(), 3);
                GameBase::CommandStatus status{};
                (void)obj->getCommandStatus(&status);
                bstream.writeInt(status.damageLevel, 2);
                if (target < 128) {
                    bstream.writeInt(client->commandStatus + 1, 3);
                } else {
                    bstream.writeFlag(obj->isControlable());
                    bstream.writeFlag(status.active);
                }
                const auto group = obj->getDatGroup();
                bstream.writeInt(group, 6);
                bstream.writeInt(obj->getDatFileId(), blocks->getDataGroupBitSize(group));
            }
            cursor = (cursor + 8) & 255;
            if (limitCommandBandwidth_ && bstream.getCurPos() > start + 400) {
                break;
            }
        } while (cursor != first && !bstream.isFull());
        bstream.writeFlag(false);
        if (bstream.isValid()) {
            lastSentTarget_ = cursor;
        }
    } else if (guiMode_ == PlayGuiMode) {
        Point2I next = me ? me->wayPoint : waypoint_;
        if (me && center && center->extent.x > 0 && center->extent.y > 0 && me->curCommand != -1 &&
            me->curCommandTarget >= 0 && me->curCommandTarget < 256) {
            if (auto* obj = targetAt(static_cast<UInt32>(me->curCommandTarget)); visible(obj)) {
                const auto pos = obj->getLinearPosition();
                const auto x =
                    static_cast<Int32>(1023 * (pos.x - center->pos.x) / center->extent.x);
                const auto y =
                    static_cast<Int32>(1023 * (pos.y - center->pos.y) / center->extent.y);
                if (((x | y) & ~1023) == 0) {
                    next = {x, y};
                    me->wayPoint = next;
                }
            }
        }
        if (bstream.writeFlag(next != waypoint_)) {
            bstream.writeInt(next.x, 10);
            bstream.writeInt(next.y, 10);
        }
        std::array<UInt32, VisibleListWords> visibleNow{};
        for (UInt32 i = 0; i < 256; ++i) {
            if (visible(targetAt(i))) {
                visibleNow[i >> 5] |= 1u << (i & 31);
            }
        }
        for (std::size_t i = 0; i < visibleNow.size(); ++i) {
            ph.xorVisibleList[i] = visibleNow[i] ^ visibleList_[i];
            if (ph.xorVisibleList[i]) {
                bstream.writeFlag(true);
                bstream.writeInt(static_cast<Int32>(i), 3);
                bstream.writeInt(static_cast<Int32>(ph.xorVisibleList[i]), 32);
            }
        }
        bstream.writeFlag(false);
        if (bstream.isValid()) {
            waypoint_ = next;
        }
    }
    if (!bstream.isValid()) {
        return false;
    }
    for (std::size_t i = 0; i < ItemShoppingListWords; ++i) {
        clientShoppingList_[i] ^= ph.xorShoppingList[i];
        clientBuyList_[i] ^= ph.xorBuyList[i];
    }
    for (std::size_t i = 0; i < VisibleListWords; ++i) {
        visibleList_[i] ^= ph.xorVisibleList[i];
    }
    if (ph.resetting) {
        clientItemTypeCount_.fill(0);
        fReset_ = false;
    }
    for (const auto& update : ph.invUpdates) {
        clientItemTypeCount_[static_cast<std::size_t>(update.item)] +=
            static_cast<Int16>(update.delta);
    }
    key = nextPacketKey_++;
    pendingPackets_.emplace(key, std::move(ph));
    return true;
}

bool PlayerPSC::readPacket(BitStream& bstream, UInt32 currentTime) {
    BitStreamRollbackGuard guard(bstream);

    const auto savedShopping = shoppingList_;
    const auto savedBuy = buyList_;
    const auto savedItems = itemTypeCount_;
    const bool savedFirstPerson = firstPerson_;
    const Flt32 savedFov = curFov_;
    const Int32 savedLastServerMove = lastServerMove_;
    const auto savedMoves = moves_;
    const auto savedMoveTimes = moveTimes_;

    const auto rollback = [&]() noexcept {
        shoppingList_ = savedShopping;
        buyList_ = savedBuy;
        itemTypeCount_ = savedItems;
        firstPerson_ = savedFirstPerson;
        curFov_ = savedFov;
        lastServerMove_ = savedLastServerMove;
        moves_ = savedMoves;
        moveTimes_ = savedMoveTimes;
        return false;
    };

    if (!isServer_) {
        const auto mode = static_cast<UInt32>(bstream.readInt(3));
        const bool scores = bstream.readFlag();
        const Int32 lastMove = bstream.readInt(32);
        const Int64 pop = Int64(lastMove) - firstMoveSeq_;
        if (!bstream.isValid() || pop > static_cast<Int64>(moves_.size())) {
            return rollback();
        }
        auto* controlled = controlObject_;
        const bool hasControl = bstream.readFlag();
        auto targets = commandTargets_;
        auto visibility = visibleList_;
        Point2I waypoint = waypoint_;
        Flt32 flash = damageFlash_;
        if (hasControl) {
            flash = bstream.readFloat(8);
            const auto ghostIndex = static_cast<UInt16>(bstream.readInt(10));
            controlled = nullptr;
            if (ghostIndex != sim::InvalidGhostIndex) {
                auto* ghosts = owner()
                                   ? owner()->ghostManager()
                                   : (getManager() ? dynamic_cast<sim::network::GhostManager*>(
                                                         localGhostManager_.resolve(*getManager()))
                                                   : nullptr);
                controlled =
                    ghosts ? dynamic_cast<GameBase*>(ghosts->resolveGhost(ghostIndex)) : nullptr;
                if (!controlled) {
                    return rollback();
                }
                const bool read =
                    dynamic_cast<ObserverCamera*>(controlled)
                        ? static_cast<ObserverCamera*>(controlled)->readPacketData(bstream, ghosts)
                        : controlled->readPacketData(bstream);
                if (!read) {
                    return rollback();
                }
            }
            if (mode == InventoryGuiMode) {
                while (bstream.readFlag()) {
                    const auto index = static_cast<std::size_t>(bstream.readInt(4));
                    if (index >= ItemShoppingListWords) {
                        return rollback();
                    }
                    if (bstream.readFlag()) {
                        shoppingList_[index] ^= static_cast<UInt32>(bstream.readInt(32));
                    }
                    if (bstream.readFlag()) {
                        buyList_[index] ^= static_cast<UInt32>(bstream.readInt(32));
                    }
                }
            }
            if (bstream.readFlag()) {
                itemTypeCount_.fill(0);
            }
            while (bstream.readFlag()) {
                const Int32 item = bstream.readInt(ItemTypeBits);
                const Int32 delta = bstream.readSignedInt(10);
                if (!bstream.isValid() || item < 0 || item >= MaxItemTypes) {
                    return rollback();
                }
                itemTypeCount_[static_cast<std::size_t>(item)] += static_cast<Int16>(delta);
            }
            if (mode == CommandGuiMode) {
                auto* blocks = getManager() ? dynamic_cast<DataBlockManager*>(
                                                  getManager()->findObject(DataBlockManagerId))
                                            : nullptr;
                UInt32 target = static_cast<UInt32>(bstream.readInt(8));
                std::size_t total{};
                while (bstream.readFlag()) {
                    if ((total += 8) > 256) {
                        return rollback();
                    }
                    for (Int32 i = 0; i < 8; ++i) {
                        auto& info = targets[target];
                        if (bstream.readFlag()) {
                            info.position = {bstream.readInt(11) - 512, bstream.readInt(11) - 512};
                            info.team = bstream.readInt(3);
                            info.damage = bstream.readInt(2);
                            if (target < 128) {
                                info.inUse = false;
                                info.status = bstream.readInt(3);
                            } else {
                                info.inUse = bstream.readFlag();
                                info.status = bstream.readFlag();
                            }
                            info.dataGroup = bstream.readInt(6);
                            if (!blocks || info.dataGroup >= DataBlockManager::NumDataTypes) {
                                return rollback();
                            }
                            info.dataId =
                                bstream.readInt(blocks->getDataGroupBitSize(info.dataGroup));
                        } else {
                            info.dataId = -1;
                        }
                        target = (target + 1) & 255;
                    }
                }
            } else if (mode == PlayGuiMode) {
                if (bstream.readFlag()) {
                    waypoint = {bstream.readInt(10), bstream.readInt(10)};
                }
                while (bstream.readFlag()) {
                    const auto index = static_cast<std::size_t>(bstream.readInt(3));
                    visibility[index] ^= static_cast<UInt32>(bstream.readInt(32));
                }
            }
        }
        if (!bstream.isValid()) {
            return rollback();
        }
        if (pop > 0) {
            if (moveTimes_.size() == moves_.size()) {
                moveTimes_.erase(moveTimes_.begin(),
                                 moveTimes_.begin() + static_cast<std::ptrdiff_t>(pop));
            }
            moves_.erase(moves_.begin(), moves_.begin() + static_cast<std::ptrdiff_t>(pop));
            firstMoveSeq_ += static_cast<Int32>(pop);
        }
        const bool guiChanged = guiMode_ != mode || scoresVisible_ != scores;
        guiMode_ = mode;
        scoresVisible_ = scores;
        damageFlash_ = flash;
        waypoint_ = waypoint;
        commandTargets_ = targets;
        visibleList_ = visibility;
        if (hasControl) {
            const bool changed = controlled != controlObject_;
            setControlObject(controlled);
            if (controlled) {
                const UInt32 tick = (currentTime - 1u) & ~31u;
                controlled->setLastProcessTime(
                    !moveTimes_.empty() && moveTimes_.size() == moves_.size()
                        ? moveTimes_.front()
                        : tick - (static_cast<UInt32>(moves_.size()) - 1u) * 32u);
            }
            if (changed && onControlObject) {
                onControlObject(controlled);
            }
        }
        if (guiChanged && onGuiState) {
            onGuiState(mode, scores);
        }
    } else {
        const Flt32 fov =
            bstream.readFloat(8) * static_cast<Flt32>(std::numbers::pi) * 135.0f / 180.0f;
        curFov_ = fov;
        if (controlObject_) {
            controlObject_->setFov(fov);
        }
        const Int32 moveSeq = bstream.readInt(32);
        if (!bstream.isValid()) {
            return rollback();
        }
        Int32 skipCount = lastServerMove_ - moveSeq;
        if (skipCount < 0) {
            return rollback();
        }

        std::vector<PlayerMove> decodedMoves;
        PlayerMove pm{};
        const PlayerMove* pmp = nullptr;
        while (bstream.readFlag()) {
            if (!pm.read(bstream, true, pmp)) {
                return rollback();
            }
            pmp = &pm;
            if (skipCount > 0) {
                --skipCount;
            } else {
                decodedMoves.push_back(pm);
                ++lastServerMove_;
                if (decodedMoves.size() > MaxMoves) {
                    return rollback();
                }
            }
        }
        if (!bstream.isValid()) {
            return rollback();
        }

        if (deferMoveProcessing_) {
            if (moves_.size() + decodedMoves.size() > MaxMoves) {
                return rollback();
            }
            moves_.insert(moves_.end(), decodedMoves.begin(), decodedMoves.end());
        } else {
            moves_ = std::move(decodedMoves);
        }
        lastPlayerMoveCount_ = static_cast<Int32>(moves_.size());
        if (controlObject_ && !moves_.empty() && !deferMoveProcessing_) {
            controlObject_->serverUpdateMove(moves_);
        }
    }

    guard.committed = true;
    return true;
}

void PlayerPSC::packetReceived(UInt32 key) {
    const auto it = pendingPackets_.find(key);
    if (it == pendingPackets_.end()) {
        return;
    }
    pendingPackets_.erase(it);
}

void PlayerPSC::packetDropped(UInt32 key) {
    const auto it = pendingPackets_.find(key);
    if (it == pendingPackets_.end()) {
        return;
    }
    const PacketHead& ph = it->second;
    if (isServer_) {
        if (ph.resetting) {
            fReset_ = true;
        }
        for (std::size_t i = 0; i < ItemShoppingListWords; ++i) {
            clientShoppingList_[i] ^= ph.xorShoppingList[i];
            clientBuyList_[i] ^= ph.xorBuyList[i];
        }
        for (std::size_t i = 0; i < VisibleListWords; ++i) {
            visibleList_[i] ^= ph.xorVisibleList[i];
        }
        for (const auto& inv : ph.invUpdates) {
            if (inv.item >= 0 && inv.item < MaxItemTypes) {
                clientItemTypeCount_[static_cast<std::size_t>(inv.item)] = static_cast<Int16>(
                    clientItemTypeCount_[static_cast<std::size_t>(inv.item)] - inv.delta);
            }
        }
    } else {
        if (ph.fov != 0) {
            prevFov_ = 0;
        }
    }
    pendingPackets_.erase(it);
}

bool PlayerPSC::processRecorderStream(StreamIO& sio, UInt32 /*currentTime*/) {
    const Int32 startPos = sio.getPosition();
    UInt8 flags = 0;
    if (!sio.read(flags)) {
        sio.setPosition(startPos);
        return false;
    }
    if (flags == 0) {
        sim::SimActionEvent action;
        action.device = sim::ActionDigital;
        if (!sio.read(action.fValue) || !sio.read(action.action) || !std::isfinite(action.fValue)) {
            sio.setPosition(startPos);
            return false;
        }
        (void)onSimActionEvent(action);
        return true;
    }
    // Original 32-bit PlayerMove is 32 bytes: four floats, four one-byte
    // bools, useItem, turnRot and pitch. Never read the modern C++ object ABI.
    PlayerMove move;
    UInt8 jet{}, crouch{}, jump{}, trigger{};
    if (!sio.read(move.forwardAction) || !sio.read(move.backwardAction) ||
        !sio.read(move.leftAction) || !sio.read(move.rightAction) || !sio.read(jet) ||
        !sio.read(crouch) || !sio.read(jump) || !sio.read(trigger) || !sio.read(move.useItem) ||
        !sio.read(move.turnRot) || !sio.read(move.pitch) || !std::isfinite(move.forwardAction) ||
        !std::isfinite(move.backwardAction) || !std::isfinite(move.leftAction) ||
        !std::isfinite(move.rightAction) || !std::isfinite(move.turnRot) ||
        !std::isfinite(move.pitch) || moves_.size() >= MaxMoves) {
        sio.setPosition(startPos);
        return false;
    }
    move.jetting = jet != 0;
    move.crouching = crouch != 0;
    move.jumpAction = jump != 0;
    move.trigger = trigger != 0;
    moves_.push_back(move);
    return true;
}

void PlayerPSC::notifyPlaybackOver() {
    moves_.clear();
    auto* console = boundConsole_  ? boundConsole_
                    : getManager() ? getManager()->console()
                                   : nullptr;
    if (console) {
        console->setVariable("playingDemo", "false");
        const std::array<std::string, 2> args{"onPlaybackFinished();", "0"};
        (void)console->execute("schedule", args);
    }
}

bool PlayerPSC::onAdd() {
    return Parent::onAdd();
}

void PlayerPSC::onDeleteNotify(sim::SimObject& object) {
    if (&object == controlObject_) {
        controlObject_ = nullptr;
        controlPlayer_ = nullptr;
    }
    Parent::onDeleteNotify(object);
}

bool PlayerPSC::processEvent(const sim::SimEvent& event) {
    if (event.type == sim::ActionEventType) {
        if (const auto* actEv = dynamic_cast<const sim::SimActionEvent*>(&event)) {
            return onSimActionEvent(*actEv);
        }
    }
    return Parent::processEvent(event);
}

bool PlayerPSC::processQuery(sim::SimQuery& query) {
    return Parent::processQuery(query);
}

} // namespace opent1::game
