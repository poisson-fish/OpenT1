#pragma once

#include <opent1/console/console.hpp>
#include <opent1/game/fear_player_psc.hpp>
#include <opent1/game/marker.hpp>
#include <opent1/game/player.hpp>
#include <opent1/game/player_manager.hpp>
#include <opent1/math/random.hpp>
#include <opent1/sim/sim_world_game.hpp>

#include <array>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace opent1::game {

class AIManager;
class AIObj;

class AIGraph {
public:
    enum Constants : Int32 {
        MaxConnections = 5,
    };

    struct GraphPoint {
        Point3F pos{0.0f, 0.0f, 0.0f};
        Int32 maxDirections{4};
        Flt32 radius{314159265358979.f};
        std::array<Int32, MaxConnections> connections{-1, -1, -1, -1, -1};
        std::array<Flt32, MaxConnections> distances{0.0f, 0.0f, 0.0f, 0.0f, 0.0f};
    };

    AIGraph() = default;
    ~AIGraph() = default;

    void clear() noexcept {
        nodes_.clear();
    }

    Int32 addNode(const Point3F& pos, Int32 maxDirections = 4);
    Int32 loadNode(std::string_view text);
    [[nodiscard]] std::string printNode(Int32 index) const;
    bool connectNodes(Int32 a, Int32 b);
    void buildGraph(std::span<const Point3F> points, Flt32 maxLinkDist = 150.0f);

    [[nodiscard]] Int32 findClosestNode(const Point3F& pos) const noexcept;
    [[nodiscard]] bool
    getShortestPath(Int32 startNode, Int32 endNode, std::vector<Int32>& outPath) const;

    [[nodiscard]] const std::vector<GraphPoint>& nodes() const noexcept {
        return nodes_;
    }

private:
    std::vector<GraphPoint> nodes_{};
};

class AIObj : public PlayerManager::BaseRep {
public:
    sim::SimObjectId scriptObjectId{};
    bool spawnBodyOnTick{true};
    std::string voice{"male1"};

    enum Constants : Int32 {
        MaxMoveStack = 6,
        OneWay = 0,
        TwoWay = 1,
        Circular = 2,
    };

    enum TriggerType : Int32 {
        Standard = 0,
        Jetting = 1,
        Jumping = 2,
        TargetPoint = 3,
        TargetLaser = 4,
        TargetObject = 5,
    };

    enum CallbackTypes : Int32 {
        TargetDestroyed = 0,
        TargetAcquired = 1,
        TargetLOSAcquired = 2,
        TargetLOSLost = 3,
        AIKilled = 4,
        Periodic = 5,
        WaypointReached = 6,
        Bored = 7,
        NumCallbackTypes = 8,
        TargetKilled = TargetDestroyed,
        TargetLOSRegained = TargetLOSAcquired,
    };

    enum TargetType : Int32 {
        NoTarget = -1,
        PlayerTarget = 0,
        LaserTarget = 1,
        PointTarget = 2,
    };

    enum DirectiveMask : UInt32 {
        WaypointDirective = 1U << 0,
        FollowDirective = 1U << 1,
        TargetDirective = 1U << 2,
        GuardDirective = 1U << 3,
        ListDirective = 1U << 4,
    };

    struct DynamicTarget {
        Int32 id{-1};
        Point3F pos{0.0f, 0.0f, 0.0f};
        Point3F vel{0.0f, 0.0f, 0.0f};
        Flt32 dist{0.0f};
        GameBase* obj{nullptr};
    };

    struct Directive {
        UInt32 orderMask{0};
        Int32 order{-1};
        Point3F pos{0.0f, 0.0f, 0.0f};
        Int32 playerId{-1};
        Flt32 rad{0.0f};
        std::array<std::string, NumCallbackTypes> callbacks{};
        std::array<std::string, 4> nativeCallbacks{};

        // Waypoint state
        std::vector<Point3F> pts{};
        Int32 curWaypoint{0};
        Int32 incWaypoint{1};
        Int32 patrolDir{1};

        // Follow state
        bool following{false};
        DynamicTarget followTarget{};

        // Guard state
        Flt32 guardTimer{0.0f};
        bool justStarted{false};
        Point3F guardPoint{0.0f, 0.0f, 0.0f};
        Int32 guardTargetPlayerId{-1};

        // Target state
        Int32 targetMode{NoTarget};
        Int32 targetPlayerId{-1};
        bool targetDirectly{true};
        bool hadLOS{false};
        bool haveLOS{false};
        bool validLOS{false};
        bool targetWasDead{false};
        UInt64 identity{0};
        DynamicTarget activeTarget{};
    };

    struct Incrementor {
        Flt32 increment{0.0f};
        Flt32 reduce{0.0f};
        Flt32 min_inc{0.0f};

        void init(Flt32 inc) noexcept;
        [[nodiscard]] Flt32 getInc(Flt32 absDiff) const noexcept;
    };

    struct IQVariables {
        Int32 thinkWait{20};
        Flt32 spotDist{50.0f};
        Flt32 spotDistOverride{-1.0f};
        Flt32 maxSpeed{0.4f};
        Incrementor pitchInc{};
        Incrementor turnInc{};
        std::array<Flt32, 2> vertDope{1.3f, 4.0f};
        Flt32 smartGuyWeaponRange{145.0f};
        Flt32 dumbGuyWeaponRange{55.0f};
        Flt32 smartGuyMinAccuracy{0.2f};
        Flt32 curWeaponRange{55.0f};
        Flt32 curWeaponMinPct{0.0f};
        Flt32 curWeaponMaxPct{0.0f};

        Flt32 maxTurnSpeed{1.0f};
        Flt32 maxPitchSpeed{1.0f};
        Flt32 dopeBase{0.0f};
        Flt32 triggerPct{1.0f};
        Int32 vertEvade{0};
        Int32 horizEvade{0};

        explicit IQVariables(bool isBest = false) noexcept;
        void scale(Flt32 unitScale) noexcept;
        [[nodiscard]] Flt32 getWeaponAccuracy(Flt32 range) const noexcept;
        [[nodiscard]] Flt32 spotDistance() const noexcept;
    };

    struct Trigger {
        Int32 windup{0};
        Int32 counter{0};
        Int32 wasPressed{0};
        Flt32 percent{1.0f};

        bool press(Random& rng) noexcept;
        void preLoop() noexcept;
    };

    struct AngleSeek {
        Flt32 desired{0.0f};
        Flt32 current{0.0f};
        Flt32 diff{0.0f};
        Flt32 absDiff{0.0f};

        Flt32 curAngle{0.0f};
        Flt32 targetAngle{0.0f};
        Flt32 maxSpeed{0.1f};

        AngleSeek() = default;

        explicit AngleSeek(Flt32 d) noexcept : desired(d), targetAngle(d) {}

        [[nodiscard]] Flt32 getStep(Flt32* delta = nullptr) const noexcept;
        Flt32 step(Flt32* delta = nullptr) noexcept;
    };

    struct JetSkill {
        enum Navigation : Int32 {
            NoNav = 0,
            Jump = 1,
            Jumped = 2,
            StartFlight = 3,
            Jet = StartFlight,
        };

        Int32 stage{NoNav};
        Point3F startPos{0.0f, 0.0f, 0.0f};
        Point3F destPos{0.0f, 0.0f, 0.0f};
        Flt32 jetTimer{0.0f};
        Point3F midpoint{}, savedVelocity{};
        Int32 jumpCount{};
        bool flightStarted{};

        void init(const AIObj& ai, const Point3F& dest) noexcept;
        bool jetToPoint(AIObj& ai, PlayerMove& pm, const Point3F& dest) noexcept;
    };

    using SeekMethod = void (AIObj::*)(PlayerMove*);
    using MovePtr = bool (AIObj::*)(PlayerMove*);

    explicit AIObj(AIManager* mgr = nullptr);
    ~AIObj();

    [[nodiscard]] Player* player() const noexcept {
        return player_;
    }

    void setPlayer(Player* p) noexcept {
        player_ = p;
    }

    [[nodiscard]] Marker* marker() const noexcept {
        return marker_;
    }

    void setMarker(Marker* m) noexcept {
        marker_ = m;
    }

    [[nodiscard]] Flt32 iq() const noexcept {
        return iq_;
    }

    void setIq(Flt32 v) noexcept;

    [[nodiscard]] Int32 attackMode() const noexcept {
        return attackMode_;
    }

    void setAttackMode(Int32 mode) noexcept {
        attackMode_ = mode;
    }

    [[nodiscard]] Int32 pathType() const noexcept {
        return pathType_;
    }

    void setPathType(Int32 pt) noexcept {
        pathType_ = pt;
    }

    [[nodiscard]] const Point3F& seekOff() const noexcept {
        return seekOff_;
    }

    void setSeekOff(const Point3F& offset) noexcept {
        seekOff_ = offset;
    }

    [[nodiscard]] bool automaticTargets() const noexcept {
        return automaticTargets_;
    }

    void setAutomaticTargets(bool autoTarg) noexcept;

    [[nodiscard]] const IQVariables& iqVars() const noexcept {
        return iqVars_;
    }

    [[nodiscard]] AngleSeek& yawSeek() noexcept {
        return yawSeek_;
    }

    [[nodiscard]] AngleSeek& pitchSeek() noexcept {
        return pitchSeek_;
    }

    [[nodiscard]] JetSkill& jetSkill() noexcept {
        return jetSkill_;
    }

    [[nodiscard]] const Point3F& curDest() const noexcept {
        return curDest_;
    }

    void setCurDest(const Point3F& dest) noexcept {
        curDest_ = dest;
    }

    [[nodiscard]] const DynamicTarget& currentTarget() const noexcept {
        return currentTarget_;
    }

    [[nodiscard]] Int32 attackedTargetId() const noexcept {
        return currentTargetId_;
    }

    void attackPlayer(Int32 clientId) noexcept {
        seekFlag_ = clientId;
    }

    [[nodiscard]] const std::vector<Directive>& directives() const noexcept {
        return directives_;
    }

    [[nodiscard]] const PlayerMove& lastGeneratedMove() const noexcept {
        return lastMove_;
    }

    void scaleIQVariables() noexcept;
    bool setVar(std::string_view varName, std::string_view value);
    [[nodiscard]] std::string getVar(std::string_view varName) const;

    void addDirective(const Directive& newDir, bool overwrite = true);
    Int32 removeDirectives(std::string_view ordExpr);
    [[nodiscard]] std::vector<Directive> listDirectives(UInt32 mask = ~0U) const;

    void setPeriodicDelay(Flt32 period) noexcept {
        periodicInterval_ = period >= 1 ? period : 0;
        periodicTicks_ = Int32(periodicInterval_ * 32);
        periodicCounter_ = 0;
    }

    bool setCallback(Int32 callbackType, std::string_view funcName, Flt32 period = 0.0f);
    bool installDirectiveCallback(Int32 number,
                                  std::string_view function,
                                  std::string_view orderExpression = {},
                                  UInt32 typeMask = ~UInt32{0});
    void invokeCallback(Int32 callbackType, std::string_view extraArg = {});

    void seekLocation(PlayerMove* pm);
    void seekDirection(PlayerMove* pm);
    void seekSpin(PlayerMove* pm);
    void seekNothing(PlayerMove* pm);

    bool getTargetPosition(Point3F& pos, const DynamicTarget& targ) const noexcept;
    [[nodiscard]] Flt32 getProjectileSpeed(Int32 weaponImageType = -1) const noexcept;
    bool leadPosition(const DynamicTarget& targ, Flt32 projSpeed, Point3F& outPos) const noexcept;
    void dopeOffset(Flt32 dist, Point3F& offset) noexcept;

    void fireAtPlayerTarget(PlayerMove* pm, const DynamicTarget& targ);
    bool fireAtLaserTarget(PlayerMove* pm, const Point3F& targetPos);
    bool fireAtLaserOwner(PlayerMove* pm, Int32 playerId);
    void fireAtPointTarget(PlayerMove* pm, const Point3F& targetPos);

    [[nodiscard]] static Flt32 wrapRotation(Flt32 r, Flt32 rangeMin = 0.0f) noexcept;
    [[nodiscard]] static Flt32 getDesiredRot(const Point3F& dest, const Point3F& source) noexcept;
    [[nodiscard]] static Flt32
    getDesiredPitch(const Point3F& dest, const Point3F& source, bool accurateMath = false) noexcept;
    static void setMovementAction(PlayerMove& pm, const AngleSeek& heading, Flt32 speed) noexcept;
    [[nodiscard]] Flt32 getSpeed(Flt32 dist) const noexcept;
    [[nodiscard]] AngleSeek getAngleSeekZ(const Point3F& seekLoc,
                                          const Point3F& ourCenter,
                                          Flt32 playerRotZ) const noexcept;

    void findTargets();
    void refreshAutomaticTargets();
    void preProcess();
    void updateDirectives(SimTime dt, PlayerMove& pm);
    void doServerProcesses(SimTime curTime);

    static bool wildCardMatch(std::string_view key, std::string_view pattern) noexcept;
    static bool orderMatch(Int32 order, std::string_view expr) noexcept;

private:
    friend class AIManager;
    enum class State { NeedToThink, FollowingPath, AttackStill, AttackTarget, DoNothing };
    State state_{State::NeedToThink};
    Int32 thinkCounter_{}, seekFlag_{}, currentTargetId_{-1};
    bool wasDead_{};
    Point3F targetDope(const Point3F& target, Flt32 width, Flt32 height) noexcept;
    void aimAt(PlayerMove& move, const Point3F& target, Flt32 threshold);
    Directive* findDirectiveIdentity(UInt64 identity) noexcept;
    Flt32 distance3(const Point3F& a, const Point3F& b) const noexcept;
    Trigger trigger_;
    AIManager* aiManager_{nullptr};
    Player* player_{nullptr};
    Marker* marker_{nullptr};

    Flt32 iq_{0.0f};
    Int32 attackMode_{1};
    Int32 pathType_{Circular};
    Point3F seekOff_{3, 3, 0};
    bool automaticTargets_{false};
    Flt32 windup_{0.0f};
    IQVariables iqVars_{};

    AngleSeek yawSeek_{};
    AngleSeek pitchSeek_{};
    JetSkill jetSkill_{};
    Flt32 jetNavigation_{0};
    Int32 pathOrder_{-1}, pathPoint_{}, pathDirection_{1};
    bool pathActive_{}, pathDone_{};
    Point3F lastWaypoint_{};
    SeekMethod currentSeek_{&AIObj::seekLocation};

    Point3F curDest_{0.0f, 0.0f, 0.0f};

    UInt64 nextDirectiveIdentity_{0};
    bool frameAccurate_{false};
    Int32 lastLosCheckId_{0};
    Int32 losThinkCounter_{};
    UInt8 wrappedFireAngle_{};
    DynamicTarget currentTarget_{};
    std::vector<Directive> directives_{};
    std::array<std::string, NumCallbackTypes> callbacks_{};
    Flt32 periodicInterval_{0.0f};
    Int32 periodicTicks_{}, periodicCounter_{};
    SimTime nextPeriodicTime_{0.0f};
    SimTime lastProcessTime_{0.0f};
    PlayerMove lastMove_{};
    static Random rng_;
};

class AIManager : public sim::SimObject {
    using Parent = sim::SimObject;

public:
    struct CallbackRecord {
        std::string function{};
        std::string aiName{};
        std::string extraArg{};
    };

    AIManager();
    ~AIManager() override;

    bool add(std::string_view aiName,
             std::string_view markerName = {},
             Int32 teamId = 0,
             std::string_view skinBase = "base");
    bool spawn(Console&,
               std::string_view aiName,
               std::string_view armor,
               const Point3F& position,
               const Point3F& rotation,
               std::string_view displayName,
               std::string_view voice);
    bool remove(std::string_view aiName, bool deletePlayer = true);
    Int32 removeMatched(std::string_view aiExpr, bool deletePlayer = true);

    [[nodiscard]] AIObj* findAI(std::string_view aiName) const noexcept;
    [[nodiscard]] AIObj* findByPlayerId(Int32 playerId) const noexcept;
    [[nodiscard]] AIObj* findByPlayer(const Player* player) const noexcept;
    void buildExpressionList(std::vector<AIObj*>& outList, std::string_view expr) const;

    [[nodiscard]] const std::vector<std::unique_ptr<AIObj>>& aiList() const noexcept {
        return aiList_;
    }

    [[nodiscard]] AIGraph& graph() noexcept {
        return graph_;
    }

    [[nodiscard]] const AIGraph& graph() const noexcept {
        return graph_;
    }

    void doServerProcesses(SimTime curTime);

    void requestRecomputation() noexcept {
        recomputePulse_ = true;
    }

    Player* spawnRespawnCorpse(AIObj& ai, bool respawnLivePlayer = true);

    [[nodiscard]] Int32 corpseCount() const noexcept {
        return corpseCounter_;
    }

    void
    recordCallback(std::string_view fn, std::string_view aiName, std::string_view extraArg = {});

    [[nodiscard]] const std::vector<CallbackRecord>& callbackHistory() const noexcept {
        return callbackHistory_;
    }

    void clearCallbackHistory() noexcept {
        callbackHistory_.clear();
    }

    bool onAdd() override;
    void onRemove() override;
    void onDeleteNotify(sim::SimObject& object) override;
    bool processEvent(const sim::SimEvent& event) override;

private:
    std::vector<std::unique_ptr<AIObj>> aiList_{};
    AIGraph graph_{};
    Int32 corpseCounter_{0};
    std::vector<CallbackRecord> callbackHistory_{};
    std::vector<std::unique_ptr<AIObj>> retiredAIs_{};
    UInt32 cyclingVar_{};
    bool recomputePulse_{true};
    bool processing_{};
};

class AIPlugin : public sim::SimConsolePlugin {
public:
    AIPlugin() = default;
    ~AIPlugin() override = default;

    void init(Console& console, sim::SimManager* manager) override;

    void setManager(sim::SimManager* manager) override {
        manager_ = manager;
    }

    void endFrame() override;

    [[nodiscard]] AIManager* resolveAIManager() const noexcept;

private:
    std::optional<std::string>
    handleCommand(Console& console, Int32 id, std::span<const std::string> argv);

    Console* console_{nullptr};
    sim::SimManager* manager_{nullptr};
};

} // namespace opent1::game
