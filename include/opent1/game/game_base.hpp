#pragma once
#include <opent1/console/console.hpp>
#include <opent1/core/bit_stream.hpp>
#include <opent1/core/stream_io.hpp>
#include <opent1/game/data_block_manager.hpp>
#include <opent1/game/f_base.hpp>
#include <opent1/math/collision.hpp>
#include <opent1/sim/common_irc_shadow.hpp>
#include <opent1/sim/sim_collision_movement.hpp>
#include <opent1/sim/sim_editor_events.hpp>
#include <opent1/sim/sim_environment_render.hpp>
#include <opent1/sim/sim_net.hpp>
#include <opent1/world/ts_shape_instance.hpp>
#include <array>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace opent1::game {

struct PlayerMove;

inline constexpr SimTime TimerCollisionTimeout = 1.0f;

class FearRenderImage {
public:
    Box3F m_containerBox{};
    bool m_attenuateSun{false};
    Flt32 m_attenuationFactor{0.0f};
    ColorF m_attenuationColor{1.0f, 1.0f, 1.0f};
    TMat3F transform{};
    bool castShadow{false};
    Flt32 wsd{0.0f};
    FearRenderImage* link{nullptr};

    FearRenderImage() = default;
    virtual ~FearRenderImage() = default;

    void setShapeInstance(world::TsShapeInstance* instance,
                          std::shared_ptr<const world::TsShapeInstance> snapshot = {}) noexcept {
        shapeInst_ = instance;
        retainedShape_ = std::move(snapshot);
    }

    void clearShapeInstance() noexcept {
        shapeInst_ = nullptr;
    }

    [[nodiscard]] const world::TsShapeInstance* shapeInstance() const noexcept {
        return shapeInst_ ? shapeInst_ : retainedShape_.get();
    }

    [[nodiscard]] world::TsShapeInstance* mutableShapeInstance() const noexcept {
        return shapeInst_;
    }

    [[nodiscard]] std::shared_ptr<const world::TsShapeInstance>
    sharedShapeInstance() const noexcept {
        return retainedShape_;
    }

    void updateSunOverride(bool attenuate, Flt32 factor, const ColorF& color) noexcept;

    // Applies the exact legacy 1.11 FearRenderImage::render directional & ambient light overrides.
    void applySunAttenuation(ColorF& inOutSunDirectionalColor,
                             ColorF& inOutAmbientIntensity) const noexcept;

    [[nodiscard]] std::array<Point3F, 8> computeBoundingBoxVertices() const noexcept;

private:
    world::TsShapeInstance* shapeInst_{nullptr};
    std::shared_ptr<const world::TsShapeInstance> retainedShape_;
};

class GameBase : public sim::SimMovement {
    using Parent = sim::SimMovement;

public:
    enum Constants : Int32 {
        TeamIdBits = 4,
        SensorPingedBase = 24,
        NeverVisBase = 16,
        AlwaysVisBase = 8,
        TeamVisBase = 0,
        PingStatusClear = 0,
        PingStatusOn = 1,
        PingStatusSupressed = 2,
    };

    enum NetMaskBits : UInt32 {
        InitialUpdate = 1u << 0,
        InfoMask = 1u << 1,
        BaseStateMask = 1u << 2,
        NextFreeMask = 1u << 3,
    };

    struct CommandStatus {
        Int32 damageLevel{0};
        bool active{false};
    };

    class Timer {
    public:
        static constexpr std::size_t kReservedSize = 4;

        struct Entry {
            sim::SimObject* object{nullptr};
            sim::SimObjectId objectId{0};
            SimTime collisionTime{0.0f};
        };

        Timer();
        ~Timer() = default;

        [[nodiscard]] static std::unique_ptr<Timer> acquire();
        static void release(std::unique_ptr<Timer> timer);
        static void clearPool() noexcept;

        void reset() noexcept;
        Entry* find(sim::SimObject* obj, SimTime time);
        bool isCollisionExpired(sim::SimObject* obj, SimTime time);
        [[nodiscard]] bool allExpired(SimTime time) const noexcept;

        [[nodiscard]] SimTime maxTime() const noexcept {
            return maxTime_;
        }

        [[nodiscard]] std::span<const Entry> entries() const noexcept {
            return entryList_;
        }

    private:
        SimTime maxTime_{0.0f};
        std::vector<Entry> entryList_;
    };

    static inline Flt32 upFov = 1.0f;
    static inline Flt32 upDistance = 0.4f;
    static inline Flt32 upVelocity = 0.4f;
    static inline Flt32 upSkips = 0.2f;
    static inline Flt32 upOwnership = 0.2f;
    static inline Flt32 upInterest = 0.2f;

    Int32 sensorKey{-1};

    GameBase();
    ~GameBase() override;

    void setDatFileName(std::string_view inName);

    [[nodiscard]] std::string_view getDatFileName() const noexcept {
        return datFileName_;
    }

    [[nodiscard]] Int32 getDatFileId() const noexcept {
        return datFileId_;
    }

    void setDatFileId(Int32 id) noexcept {
        datFileId_ = id;
    }

    [[nodiscard]] GameBaseData* getDatPtr() const noexcept {
        return data_ ? data_ : retainedData_.get();
    }

    void bindDataBlockManager(DataBlockManager* dbm) noexcept {
        boundDbm_ = dbm;
    }

    [[nodiscard]] DataBlockManager* resolveDataBlockManager() const noexcept;

    bool loadDatFile();
    void packDatFile(BitStream& stream) const;
    bool unpackDatFile(BitStream& stream);

    bool processArguments(std::span<const std::string> arguments) override;
    bool processArgumentsView(std::span<const std::string_view> arguments);
    bool onAdd() override;
    void onRemove() override;
    void onGroupAdd() override;
    void onGroupRemove() override;
    bool processEvent(const sim::SimEvent& event) override;
    bool processQuery(sim::SimQuery& query) override;

    bool setPosition(TMat3F target, bool force = false) {
        if (!target.p.isZero()) {
            target.flags |= RMat3F::Matrix_HasTranslation;
        }
        return sim::SimMovement::setPosition(target, force);
    }

    virtual bool initResources(GameBaseData* dat);
    virtual void updateTeamId();
    [[nodiscard]] virtual std::string_view getScriptDefaultClass() const noexcept;
    virtual void onCollisionNotify(GameBase* object);
    virtual void setOwnerClient(Int32 clientId);
    virtual void setControlClient(Int32 clientId);
    virtual void setTeam(Int32 team);

    [[nodiscard]] virtual Flt32 getDamageLevel() const noexcept {
        return 0.0f;
    }

    [[nodiscard]] virtual Flt32 getEnergyLevel() const noexcept {
        return 0.0f;
    }

    [[nodiscard]] virtual Flt32 getDamageFlash() const noexcept {
        return 0.0f;
    }

    virtual void getThrowVector(Point3F* pos, Point3F* vec) const;
    virtual void throwObject(GameBase* object, Flt32 speed, bool careless);
    virtual bool getMuzzleTransform(Int32 slot, TMat3F* mat) const;

    [[nodiscard]] virtual const TMat3F& getEyeTransform() const {
        return getTransform();
    }

    virtual bool getWeaponAmmoType(ProjectileDataType& outPrimary,
                                   ProjectileDataType& outSecondary) const {
        outPrimary = {};
        outSecondary = {};
        return false;
    }

    virtual bool getTarget(Point3F* outTarget, Int32* outTeam) const {
        if (outTarget) {
            *outTarget = {};
        }
        if (outTeam) {
            *outTeam = -1;
        }
        return false;
    }

    [[nodiscard]] virtual bool mousePitch() const noexcept {
        return true;
    }

    virtual void onPowerStateChange(GameBase* generator);

    [[nodiscard]] virtual Int32 getDatGroup() const noexcept {
        return -1;
    }

    [[nodiscard]] virtual bool getCommandStatus(CommandStatus* status) const;

    // Power management
    void generatePower(bool power);
    void forcePowerState(bool on) noexcept;

    [[nodiscard]] bool isGenerator() const noexcept {
        return powerGenerator_;
    }

    [[nodiscard]] bool isPowerGenerator() const noexcept {
        return powerGenerator_;
    }

    [[nodiscard]] bool isPowered() const noexcept {
        return powerCount_ > 0;
    }

    [[nodiscard]] Int32 powerCount() const noexcept {
        return powerCount_;
    }

    [[nodiscard]] virtual bool isActive() const noexcept {
        return true;
    }

    virtual bool setActive(bool active) {
        return active;
    }

    virtual void setPlayerControl(bool ctrl) {
        (void)ctrl;
    }

    [[nodiscard]] virtual bool isControlable() const noexcept {
        return false;
    }

    [[nodiscard]] virtual const Point3F& getLeadVelocity() const noexcept {
        return getLinearVelocity();
    }

    virtual void serverUpdateMove(std::span<const PlayerMove> moves) {
        (void)moves;
    }

    [[nodiscard]] virtual bool mountRender() const noexcept {
        return true;
    }

    [[nodiscard]] virtual Int32 getMountPose() const noexcept {
        return 0;
    }

    virtual void getObjectMountTransform(Int32 mountPoint, TMat3F* mat) const;
    virtual void getCameraTransform(Flt32 pos, TMat3F* mat) const;

    [[nodiscard]] virtual bool cameraZoomAndCrosshairsEnabled() const noexcept {
        return false;
    }

    void updateTimers(SimTime explicitCurrentTime = -1.0f);

    // Team & ownership
    [[nodiscard]] Int32 getTeam() const noexcept {
        return teamId_;
    }

    [[nodiscard]] sim::SimObjectId getOwnerId() const noexcept {
        return ownerId_;
    }

    void setOwnerId(sim::SimObjectId id) noexcept {
        ownerId_ = id;
    }

    [[nodiscard]] Int32 getOwnerClient() const noexcept {
        return ownerClientId_;
    }

    [[nodiscard]] Int32 getLastOwnerClient() const noexcept {
        return lastOwnerClientId_;
    }

    [[nodiscard]] Int32 getControlClient() const noexcept {
        return ctrlClientId_;
    }

    // Map name & console variable expansion (<vVarName>)
    void setName(std::string_view name, Console* explicitConsole = nullptr);

    [[nodiscard]] std::string_view getName() const noexcept {
        return mapName_;
    }

    // Script resolution & lifecycle hooks
    void bindConsole(Console* console) noexcept {
        boundConsole_ = console;
    }

    [[nodiscard]] Console* resolveConsole() const noexcept;
    [[nodiscard]] std::string scriptThis() const;
    [[nodiscard]] std::string scriptName(std::string_view callback) const;
    void scriptOnAdd();
    void scriptOnInit();
    void scriptOnRemove();
    void scriptOnCollision(GameBase* obj, SimTime explicitCurrentTime = -1.0f);

    // Sensor visibility bitfield operations
    void setSensorPinged(UInt8 fPinged) noexcept;
    [[nodiscard]] UInt8 getSensorPinged() const noexcept;
    void setAlwaysVisibleToTeam(Int32 teamId, bool visible) noexcept;
    [[nodiscard]] bool getAlwaysVisibleToTeam(Int32 teamId) const noexcept;
    void setNeverVisibleToTeam(Int32 teamId, bool visible) noexcept;
    [[nodiscard]] bool getNeverVisibleToTeam(Int32 teamId) const noexcept;
    void setVisibleToTeam(Int32 teamId, bool visible) noexcept;
    [[nodiscard]] bool getVisibleToTeam(Int32 teamId) const noexcept;

    [[nodiscard]] UInt32 sensorInfoBits() const noexcept {
        return sensorInfoBits_;
    }

    void setSensorInfoBits(UInt32 bits) noexcept {
        sensorInfoBits_ = bits;
    }

    [[nodiscard]] virtual bool getImageEnhanceable() const noexcept {
        return false;
    }

    [[nodiscard]] virtual bool isCloaked() const noexcept {
        return false;
    }

    [[nodiscard]] virtual bool isJammingSensor() const noexcept {
        return false;
    }

    [[nodiscard]] Flt32 getHaze(const Point3F& cameraPos) const;

    // Camera eye validation & scope/update priority
    Flt32 validateEyePoint(TMat3F* trans, Flt32 camDist) const;
    // halfExtents describes the near rectangle at the requested nearDistance.
    [[nodiscard]] Flt32 validateCameraNearPlane(const Point3F& position,
                                                Flt32 nearDistance,
                                                const Point2F& halfExtents) const;
    void buildScopeAndCameraInfo(sim::network::GhostManager& cr,
                                 sim::SimNetObject::CameraInfo& camInfo) override;
    [[nodiscard]] Flt32 getUpdatePriority(const sim::SimNetObject::CameraInfo& camInfo,
                                          UInt32 updateMask,
                                          UInt32 updateSkips) const override;

    virtual void serverProcess(UInt32 curTime) {
        lastProcessTime_ = curTime;
    }

    virtual void clientProcess(UInt32 curTime) {
        lastProcessTime_ = curTime;
    }

    [[nodiscard]] UInt32 getLastProcessTime() const noexcept {
        return lastProcessTime_;
    }

    void setLastProcessTime(UInt32 t) noexcept {
        lastProcessTime_ = t;
    }

    [[nodiscard]] UInt32 getLastAnimateTime() const noexcept {
        return lastAnimateTime_;
    }

    void setLastAnimateTime(UInt32 t) noexcept {
        lastAnimateTime_ = t;
    }

    [[nodiscard]] Flt32 getFov() const noexcept {
        return fov_;
    }

    void setFov(Flt32 fov) noexcept {
        fov_ = fov;
    }

    virtual void writePacketData(BitStream& stream) const {
        (void)stream;
    }

    virtual bool readPacketData(BitStream& stream) {
        (void)stream;
        return true;
    }

    // Network ghosting & persistence
    UInt32
    packUpdate(sim::network::GhostManager& gm, UInt32 mask, BitStream& stream) const override;
    bool unpackUpdate(sim::network::GhostManager& gm, BitStream& stream) override;
    bool packGameBaseUpdate(UInt32 mask, BitStream& stream) const;
    bool unpackGameBaseUpdate(BitStream& stream);

    Persistent::Error
    read(StreamIO& sio, Int32 version, Int32 user, Persistent::LoadContext& ctx) override;
    Persistent::Error
    write(StreamIO& sio, Int32 version, Int32 user, Persistent::StoreContext& ctx) const override;
    Persistent::Error readLegacy(StreamIO& sio, Int32 version = 0, Int32 user = 0);
    Persistent::Error writeLegacy(StreamIO& sio, Int32 version = 0, Int32 user = 0) const;

    void updatePowerCount();

protected:
    void updatePowerClients(sim::SimGroup* group, bool power);

    UInt32 sensorInfoBits_{0};
    bool powerGenerator_{false};
    Int32 powerCount_{0};
    Int32 teamId_{-1};
    sim::SimObjectId ownerId_{0};
    std::unique_ptr<Timer> timer_;
    UInt32 lastProcessTime_{0};
    UInt32 lastAnimateTime_{0};
    Int32 datFileId_{-1};
    std::string datFileName_;
    GameBaseData* data_{nullptr};
    std::shared_ptr<GameBaseData> retainedData_;
    DataBlockManager* boundDbm_{nullptr};
    Console* boundConsole_{nullptr};
    Flt32 fov_{1.0f};
    Int32 ctrlClientId_{0};
    Int32 lastOwnerClientId_{0};
    Int32 ownerClientId_{0};
    std::string mapName_;
};

} // namespace opent1::game
