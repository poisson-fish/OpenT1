#pragma once

#include <opent1/game/data_block_manager.hpp>
#include <opent1/game/explosion.hpp>
#include <opent1/game/f_base.hpp>
#include <opent1/game/game_base.hpp>
#include <opent1/game/shape_base.hpp>
#include <opent1/game/shape_collision.hpp>
#include <opent1/math/random.hpp>
#include <opent1/sim/sim_collision_movement.hpp>
#include <opent1/sim/sim_world_game.hpp>
#include <opent1/world/ts_shape.hpp>
#include <opent1/world/ts_shape_instance.hpp>

#include <array>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace opent1::game {

class DebrisCloud;

struct DebrisTable {
    struct ShortListEntry {
        Flt32 mass{0.0f};
        Int32 index{-1};
        Flt32 p{0.0f};
    };

    using ShortList = std::vector<ShortListEntry>;

    static Flt32
    getShortList(const DataBlockManager& dbm, Flt32 mass, Int32 typeMask, ShortList& sl);
};

class Debris : public GameBase {
    using Parent = GameBase;

public:
    using DebrisData = opent1::game::DebrisData;

    enum CollisionDetail : Int32 {
        SphereCollisions = 0,
        BoundingBoxCollisions = 1,
        DetailedCollisions = 2,
    };

    enum NetMaskBits : UInt32 {
        netInit = 1u << 0,
        netUpdate = 1u << 1,
    };

    Debris();
    explicit Debris(Int32 index, Flt32 spawnTO = 3.0f, bool ghost = false);
    Debris(std::string_view shapeFile,
           Int32 index,
           Flt32 spawnTO = 3.0f,
           bool ghost = false,
           Flt32 mass = -1.0f,
           Int32 animSeq = -1,
           Flt32 restartTime = -1.0f);
    Debris(const Debris& other);
    ~Debris() override;

    [[nodiscard]] Int32 getDatGroup() const noexcept override {
        return DataBlockManager::DebrisDataType;
    }

    bool initResources(GameBaseData* in_data) override;
    bool onAdd() override;
    void onRemove() override;
    bool processEvent(const sim::SimEvent& event) override;
    bool processQuery(sim::SimQuery& query) override;

    void finishAddToManager();
    bool onSimTimerEvent(const sim::SimTimerEvent& event);
    virtual bool onSimMessageEvent(const sim::SimMessageEvent& event);

    bool bounce(TMat3F& objectToWorld, Point3F& normal, Flt32 k);
    bool bounceSphere(const Point3F& normal);
    bool bounce(sim::SimMovement::MovementInfo& minfo);

    [[nodiscard]] const std::string& shapeName() const noexcept {
        return shapeName_;
    }

    void setShapeName(std::string_view s) {
        shapeName_.assign(s);
    }

    [[nodiscard]] Flt32 spawnTimeout() const noexcept {
        return spawnTimeout_;
    }

    void setSpawnTimeout(Flt32 t) noexcept {
        spawnTimeout_ = t;
    }

    [[nodiscard]] Flt32 debrisMass() const noexcept {
        return debrisMass_;
    }

    void setDebrisMass(Flt32 m) noexcept {
        debrisMass_ = m;
    }

    [[nodiscard]] Int32 animationSequence() const noexcept {
        return animationSequence_;
    }

    void setAnimationSequence(Int32 seq) noexcept {
        animationSequence_ = seq;
    }

    [[nodiscard]] Flt32 animRestartTime() const noexcept {
        return animRestartTime_;
    }

    void setAnimRestartTime(Flt32 t) noexcept {
        animRestartTime_ = t;
    }

    [[nodiscard]] Flt32 damageRemaining() const noexcept {
        return damage_;
    }

    [[nodiscard]] bool isExpired() const noexcept {
        return expire_;
    }

    void setExpired(bool e) noexcept {
        expire_ = e;
    }

    [[nodiscard]] bool inCloud() const noexcept {
        return inCloud_;
    }

    void setInCloud(bool val, DebrisCloud* cloud = nullptr) noexcept {
        inCloud_ = val;
        parentCloud_ = cloud;
    }

    [[nodiscard]] DebrisCloud* parentCloud() const noexcept {
        return parentCloud_;
    }

    [[nodiscard]] const DebrisData* debrisData() const noexcept {
        return m_debrisData_;
    }

    [[nodiscard]] const Point3F& radii() const noexcept {
        return radii_;
    }

    bool packDebrisUpdate(UInt32 mask, BitStream& stream) const;
    bool unpackDebrisUpdate(BitStream& stream);

    UInt32
    packUpdate(sim::network::GhostManager& gm, UInt32 mask, BitStream& stream) const override;
    bool unpackUpdate(sim::network::GhostManager& gm, BitStream& stream) override;

    static void resetRandomSeed(UInt32 seed = Random::defaultSeed) noexcept;
    static Random& sharedRandom() noexcept;

    [[nodiscard]] std::string_view persistClassName() const noexcept {
        return "Debris";
    }

    [[nodiscard]] UInt32 persistTag() const noexcept {
        return DebrisPersTag;
    }

    [[nodiscard]] UInt16 getGhostTag() const override {
        return static_cast<UInt16>(DebrisPersTag);
    }

protected:
    std::shared_ptr<sim::SimCollisionImage> getSelfImage() override;
    bool processCollision(MovementInfo& moveInfo) override;

    DebrisData* m_debrisData_{nullptr};
    std::string shapeName_{};
    Resource<world::TsShape> shape_{};
    std::shared_ptr<world::TsShapeInstance> shapeInstance_{};
    std::optional<UInt32> animThread_{};
    FearRenderImage image_{};
    std::shared_ptr<ShapeCollisionImage> collisionImage_;

    Point3F radii_{0.5f, 0.5f, 0.5f};
    Point3F avgLV_{0.0f, 0.0f, 0.0f};
    Point3F avgAV_{0.0f, 0.0f, 0.0f};
    Flt32 debrisMass_{-1.0f};
    Flt32 elastic_{0.5f};
    Flt32 damage_{100.0f};
    Flt32 timeout_{2.0f};
    Flt32 spawnTimeout_{3.0f};
    Flt32 spawnImpulse_{50.0f};
    Int32 spawnedExplosionId_{-1};
    Int32 animationSequence_{-1};
    Flt32 animRestartTime_{-1.0f};
    SimTime debrisTime_{0.0f};
    SimTime lastUpdateTime_{0.0f};
    SimTime reanimateTime_{-1.0f};
    Flt32 sinkDepth_{0.0f};
    Flt32 potentialEnergyDeficit_{0.0f};
    UInt32 timerCounter_{0};
    bool ghostMe_{false};
    bool inCloud_{false};
    bool hitThisTime_{false};
    bool expire_{false};
    bool underSurfaceLastTime_{false};
    bool underSurfaceThisTime_{false};
    DebrisCloud* parentCloud_{nullptr};
};

class DebrisCloud : public sim::SimContainer {
    using Parent = sim::SimContainer;

public:
    static inline Flt32 detailScale = 1.0f;

    DebrisCloud();
    DebrisCloud(Flt32 kg,
                Int32 typeMask,
                const Point3F& p,
                Flt32 r,
                const Point3F& v,
                Flt32 outJ,
                const Point3F& ax = Point3F{0.0f, 0.0f, 0.0f},
                Flt32 angleR = 3.14159265f,
                const Point3F& inJ = Point3F{0.0f, 0.0f, 0.0f},
                Flt32 spawnTO = 3.0f);
    DebrisCloud(const DebrisCloud& other);
    ~DebrisCloud() override;

    void bindDataBlockManager(DataBlockManager* dbm) noexcept {
        boundDbm_ = dbm;
    }

    bool onAdd() override;
    bool processEvent(const sim::SimEvent& event) override;
    void finishAddToManager();
    bool onSimTimerEvent(const sim::SimTimerEvent& event);
    bool onSimMessageEvent(const sim::SimMessageEvent& event);

    void setSpawnedFrom(DebrisCloud* sf) noexcept {
        spawnedFrom_ = sf;
    }

    void setSoundId(Int32 sid) noexcept {
        soundId_ = sid;
    }

    [[nodiscard]] Int32 soundId() const noexcept {
        return soundId_;
    }

    [[nodiscard]] Flt32 mass() const noexcept {
        return mass_;
    }

    [[nodiscard]] Int32 debrisMask() const noexcept {
        return debrisMask_;
    }

    [[nodiscard]] const Point3F& position() const noexcept {
        return pos_;
    }

    [[nodiscard]] Flt32 radius() const noexcept {
        return radius_;
    }

    [[nodiscard]] const Point3F& velocity() const noexcept {
        return vel_;
    }

    [[nodiscard]] Flt32 outwardImpulse() const noexcept {
        return outwardImpulse_;
    }

    [[nodiscard]] const Point3F& axis() const noexcept {
        return axis_;
    }

    [[nodiscard]] Flt32 angleRange() const noexcept {
        return angleRange_;
    }

    [[nodiscard]] const Point3F& incomingImpulse() const noexcept {
        return incomingImpulse_;
    }

    [[nodiscard]] Flt32 spawnTimeout() const noexcept {
        return spawnTimeout_;
    }

    [[nodiscard]] const std::vector<Debris*>& debrisList() const noexcept {
        return debrisList_;
    }

    static void
    getAxes(const Point3F& axis, std::span<Point3F, 3> axes, Flt32& angleRange) noexcept;

    static void toss(Debris& debris,
                     const Point3F& pos,
                     Flt32 radius,
                     const Point3F& vel,
                     Flt32 outwardImpulse,
                     std::span<const Point3F, 3> axes,
                     Flt32 angleRange,
                     const Point3F& incomingImpulse,
                     sim::SimManager* mgr);

    bool packCloudUpdate(BitStream& stream) const;
    bool unpackCloudUpdate(BitStream& stream);

    UInt32
    packUpdate(sim::network::GhostManager& gm, UInt32 mask, BitStream& stream) const override;
    bool unpackUpdate(sim::network::GhostManager& gm, BitStream& stream) override;

    [[nodiscard]] std::string_view persistClassName() const noexcept {
        return "DebrisCloud";
    }

    [[nodiscard]] UInt32 persistTag() const noexcept {
        return DebrisCloudPersTag;
    }

    [[nodiscard]] UInt16 getGhostTag() const override {
        return static_cast<UInt16>(DebrisCloudPersTag);
    }

protected:
    DataBlockManager* resolveDbm() const noexcept;
    void addManagedDebris(std::unique_ptr<Debris> debris);

    DataBlockManager* boundDbm_{nullptr};
    Flt32 mass_{0.0f};
    Int32 debrisMask_{0};
    Point3F pos_{0.0f, 0.0f, 0.0f};
    Flt32 radius_{0.0f};
    Point3F vel_{0.0f, 0.0f, 0.0f};
    Flt32 outwardImpulse_{0.0f};
    Point3F axis_{0.0f, 0.0f, 0.0f};
    Flt32 angleRange_{3.14159265f};
    Point3F incomingImpulse_{0.0f, 0.0f, 0.0f};
    Flt32 spawnTimeout_{3.0f};
    Int32 soundId_{0};
    DebrisCloud* spawnedFrom_{nullptr};
    bool dataReceived_{false};
    std::array<Point3F, 3> axes_{};
    std::vector<Debris*> debrisList_{};
    std::vector<std::unique_ptr<Debris>> ownedDebris_{};
};

bool registerDebrisClasses(Persistent::ClassRegistry& registry);
bool registerDebrisNetwork(sim::network::Registry& registry);

} // namespace opent1::game
