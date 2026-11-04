#pragma once
#include <opent1/game/static_base.hpp>
#include <opent1/world/scene_objects.hpp>
#include <memory>
#include <string_view>

namespace opent1::game {

class Moveable final : public StaticBase {
    using Parent = StaticBase;

public:
    enum Constants : Int32 {
        WaypointBits = 4,
        PathIdBits = 8,
        UndefinedPathId = 255,
        UndefinedWaypoint = 15,
    };

    enum State : Int32 {
        Undefined = 0,
        ForwardState = 1,
        BackwardState = 2,
        StopState = 3,
    };

    enum NetMaskBits : UInt32 {
        PathMask = Parent::NextFreeMask,
        StateMask = Parent::NextFreeMask << 1,
        NextFreeMask = Parent::NextFreeMask << 2,
    };

    Moveable();
    ~Moveable() override;

    bool onAdd() override;
    void onRemove() override;
    void onGroupAdd() override;
    bool initResources(GameBaseData* dat) override;
    bool processEvent(const sim::SimEvent& event) override;

    [[nodiscard]] Int32 getDatGroup() const noexcept override {
        return DataBlockManager::MoveableDataType;
    }

    [[nodiscard]] UInt16 getGhostTag() const noexcept override {
        return static_cast<UInt16>(FearMoveablePersTag);
    }

    void serverProcess(UInt32 curTime) override;
    void clientProcess(UInt32 curTime) override;

    // Path & waypoint control
    void bindPathManager(world::SimPathManager* pm) noexcept {
        boundPathMgr_ = pm;
    }

    [[nodiscard]] world::SimPathManager* resolvePathManager() const noexcept;
    [[nodiscard]] Int32 findPathId() const;
    void setPathId(Int32 pid);

    [[nodiscard]] Int32 pathId() const noexcept {
        return pathId_;
    }

    bool moveToWaypoint(Int32 wp);
    bool moveForward();
    bool moveBackward();
    void stop();
    bool setWaypoint(Flt32 wp);
    bool setWaypointPosition(Int32 wp);

    [[nodiscard]] State getMoveState() const noexcept {
        return moveState_;
    }

    void setMoveStateDirect(State st) noexcept {
        moveState_ = st;
    }

    [[nodiscard]] std::string_view getMoveStateName() const noexcept;
    [[nodiscard]] Int32 getWaypointCount() noexcept;

    void setWaypointCount(Int32 count) noexcept {
        waypointCount_ = count;
    }

    [[nodiscard]] Int32 currentWaypoint() const noexcept {
        return currentWaypoint_;
    }

    void setCurrentWaypoint(Int32 wp) noexcept {
        currentWaypoint_ = wp;
    }

    [[nodiscard]] Int32 targetWaypoint() const noexcept {
        return waypoint_;
    }

    void setTargetWaypoint(Int32 wp) noexcept {
        waypoint_ = wp;
    }

    [[nodiscard]] Flt32 waypointParameter() const noexcept {
        return position_;
    }

    void setWaypointParameter(Flt32 param) noexcept {
        position_ = param;
    }

    [[nodiscard]] Flt32 pathTime() const noexcept {
        return pathTime_;
    }

    void setPathTime(Flt32 t) noexcept {
        pathTime_ = t;
    }

    [[nodiscard]] Flt32 getWaypointPosition() const noexcept {
        return static_cast<Flt32>(currentWaypoint_) + position_;
    }

    [[nodiscard]] GameBase* blockedObject() const noexcept {
        return blocked_;
    }

    [[nodiscard]] bool isBlocked() const noexcept {
        return blocked_ != nullptr || blockedFlag_;
    }

    void setBlockedFlag(bool b) noexcept {
        blockedFlag_ = b;
    }

    bool buildMovementVector(Int32 index, bool backward);
    bool updatePosition(bool force);
    void updateMoveState(State newState);

    // Network ghosting & persistence
    UInt32
    packUpdate(sim::network::GhostManager& gm, UInt32 mask, BitStream& stream) const override;
    bool unpackUpdate(sim::network::GhostManager& gm, BitStream& stream) override;
    bool packMoveableUpdate(UInt32 mask, BitStream& stream, UInt32 currentTime = 0) const;
    bool unpackMoveableUpdate(BitStream& stream, UInt32 currentTime = 0);

    Persistent::Error
    read(StreamIO& sio, Int32 version, Int32 user, Persistent::LoadContext& ctx) override;
    Persistent::Error
    write(StreamIO& sio, Int32 version, Int32 user, Persistent::StoreContext& ctx) const override;

protected:
    bool processBlocker(sim::SimObject& obj) override;

private:
    MoveableData* moveableData_{nullptr};
    world::SimPathManager* boundPathMgr_{nullptr};
    UInt32 runSound_{0};
    State moveState_{Undefined};
    Int32 pathId_{UndefinedPathId};
    Int32 waypointCount_{0};
    Int32 currentWaypoint_{0};
    Int32 waypoint_{UndefinedWaypoint};
    Flt32 position_{0.0f};
    Flt32 pathTime_{1.0f};
    Point3F lVector_{0.0f, 0.0f, 0.0f};
    Point3F aVector_{0.0f, 0.0f, 0.0f};
    Point3F startPos_{0.0f, 0.0f, 0.0f};
    Point3F startRot_{0.0f, 0.0f, 0.0f};
    Flt32 ivScale_{0.0f};
    GameBase* blocked_{nullptr};
    bool blockedFlag_{false};
    bool hasUnpackedInitialMoveable_{false};
    UInt32 lastBlockedTime_{0};
};

struct MoveableBaseData final : StaticBaseData {
    Int32 iStartSoundTag{0};
    Int32 iStopSoundTag{0};
    Int32 iRunSoundTag{0};
    Flt32 rForwardDelay{0.0f};
    Flt32 rBackwardDelay{0.0f};
    Int32 iLinearVelocity{0};
    Flt32 rCrushDamage{0.0f};
    bool fInvincible{false};

    MoveableBaseData() = default;

    [[nodiscard]] std::unique_ptr<GameBaseData> clone() const override {
        return std::make_unique<MoveableBaseData>(*this);
    }

    void pack(BitStream& stream, DataBlockManager* dbm) const override;
    bool unpack(BitStream& stream, DataBlockManager* dbm) override;
};

class MoveableBase final : public StaticBase {
    using Parent = StaticBase;

public:
    enum MoveState : Int32 {
        STATE_STOP = 0,
        STATE_WAYPOINT = 1,
        STATE_FORWARD = 2,
        STATE_BACKWARD = 3,
    };

    enum NetMaskBits : UInt32 {
        NETMASK_EDIT = Parent::NextFreeMask << 0,
        NETMASK_WAYPOINT = Parent::NextFreeMask << 1,
        NETMASK_UPDATE = Parent::NextFreeMask << 2,
        NETMASK_SOUND = Parent::NextFreeMask << 3,
        NextFreeMask = Parent::NextFreeMask << 4,
        NETMASK_INIT = 0xffffffffu,
    };

    MoveableBase();
    ~MoveableBase() override;

    bool onAdd() override;
    void onGroupAdd() override;
    bool initResources(GameBaseData* dat) override;
    bool processEvent(const sim::SimEvent& event) override;
    bool processQuery(sim::SimQuery& query) override;

    [[nodiscard]] Int32 getDatGroup() const noexcept override {
        return DataBlockManager::MoveableDataType;
    }

    [[nodiscard]] UInt16 getGhostTag() const noexcept override {
        return static_cast<UInt16>(MoveableBasePersTag);
    }

    void serverProcess(UInt32 curTime) override;
    void clientProcess(UInt32 curTime) override;

    void bindPathManager(world::SimPathManager* pm) noexcept {
        boundPathMgr_ = pm;
    }

    [[nodiscard]] world::SimPathManager* resolvePathManager() const noexcept;

    void move(MoveState newState, Int32 waypoint = 0);
    void updateServerMoveState(Flt32 dt);
    void updateMovement(Flt32 dt);

    [[nodiscard]] MoveState moveState() const noexcept {
        return state_;
    }

    [[nodiscard]] Int32 pathId() const noexcept {
        return iPathID_;
    }

    void setPathId(Int32 id) noexcept {
        iPathID_ = id;
        fPathUpdated_ = (id != -1);
    }

    [[nodiscard]] Int32 waypoint() const noexcept {
        return iWaypoint_;
    }

    void setWaypoint(Int32 wp) noexcept {
        iWaypoint_ = wp;
    }

    [[nodiscard]] Int32 lastWaypoint() const noexcept {
        return iLastWaypoint_;
    }

    void setLastWaypoint(Int32 wp) noexcept {
        iLastWaypoint_ = wp;
    }

    [[nodiscard]] Int32 targetWaypoint() const noexcept {
        return iTargetWaypoint_;
    }

    [[nodiscard]] Flt32 waypointTime() const noexcept {
        return rTime_;
    }

    void setWaypointTime(Flt32 t) noexcept {
        rTime_ = t;
    }

    [[nodiscard]] Flt32 travelTime() const noexcept {
        return rTotalTime_;
    }

    void setTravelTime(Flt32 t) noexcept {
        rTotalTime_ = t;
    }

    [[nodiscard]] const Point3F& localPosition() const noexcept {
        return position_;
    }

    [[nodiscard]] const Point3F& localRotation() const noexcept {
        return rotationVec_;
    }

    [[nodiscard]] const Point3F& linearDistance() const noexcept {
        return ldistance_;
    }

    [[nodiscard]] const Point3F& angularDistance() const noexcept {
        return adistance_;
    }

    void setMotionVectors(const Point3F& pos,
                          const Point3F& rot,
                          const Point3F& ldist,
                          const Point3F& adist) noexcept {
        position_ = pos;
        rotationVec_ = rot;
        ldistance_ = ldist;
        adistance_ = adist;
    }

    [[nodiscard]] bool soundStartFlag() const noexcept {
        return fSoundStart_;
    }

    [[nodiscard]] bool soundStopFlag() const noexcept {
        return fSoundStop_;
    }

    [[nodiscard]] bool soundRunFlag() const noexcept {
        return fSoundRun_;
    }

    void setSoundFlags(bool start, bool stop, bool run) noexcept {
        fSoundStart_ = start;
        fSoundStop_ = stop;
        fSoundRun_ = run;
        setMaskBits(NETMASK_SOUND);
    }

    UInt32
    packUpdate(sim::network::GhostManager& gm, UInt32 mask, BitStream& stream) const override;
    bool unpackUpdate(sim::network::GhostManager& gm, BitStream& stream) override;
    bool packMoveableBaseUpdate(UInt32 mask,
                                BitStream& stream,
                                UInt32 currentTime = 0,
                                bool forceDelta = false) const;
    bool unpackMoveableBaseUpdate(BitStream& stream, UInt32 currentTime = 0);

    Persistent::Error
    read(StreamIO& sio, Int32 version, Int32 user, Persistent::LoadContext& ctx) override;
    Persistent::Error
    write(StreamIO& sio, Int32 version, Int32 user, Persistent::StoreContext& ctx) const override;

private:
    MoveableBaseData* mbData_{nullptr};
    world::SimPathManager* boundPathMgr_{nullptr};
    MoveState state_{STATE_WAYPOINT};
    Int32 iPathID_{-1};
    mutable bool fPathUpdated_{false};
    bool hasUnpackedInitialMoveableBase_{false};
    Int32 iWaypoint_{0};
    Int32 iTargetWaypoint_{0};
    Int32 iLastWaypoint_{0};
    Flt32 rTime_{0.0f};
    Flt32 rTotalTime_{0.0f};
    mutable Point3F position_{0.0f, 0.0f, 0.0f};
    mutable Point3F rotationVec_{0.0f, 0.0f, 0.0f};
    Point3F ldistance_{0.0f, 0.0f, 0.0f};
    Point3F adistance_{0.0f, 0.0f, 0.0f};
    bool fSoundStart_{false};
    bool fSoundStop_{false};
    bool fSoundRun_{false};
};

bool registerBaseSimClasses(Persistent::ClassRegistry& registry);
bool registerBaseSimNetwork(sim::network::Registry& registry);

} // namespace opent1::game
