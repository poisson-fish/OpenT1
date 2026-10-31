#pragma once

#include <opent1/game/fear_player_psc.hpp>
#include <opent1/game/game_base.hpp>

#include <span>
#include <string>
#include <vector>

namespace opent1::game {

class ObserverCamera : public GameBase {
    using Parent = GameBase;

public:
    enum Mode : Int32 {
        StationaryMode = 0,
        FreeRotateMode = 1,
        FlyMode = 2,
        OrbitObjectMode = 3,
        OrbitPointMode = 4,
    };

    static inline constexpr Flt32 MaxPitch = 1.3962f;

    struct ScriptCallbackRecord {
        std::string function{};
        Int32 clientId{0};
    };

    explicit ObserverCamera(Int32 in_clientId = 0);
    ~ObserverCamera() override;

    void setFlyMode(bool canMove, bool canRotate, const Point3F& pos, const EulerF& rot);
    void setOrbitMode(GameBase* obj,
                      const Point3F& pos,
                      const EulerF& rot,
                      Flt32 minDist,
                      Flt32 maxDist,
                      Flt32 curDist);

    void update(UInt32 startTime, UInt32 endTime, const PlayerMove* move);
    void serverUpdateMove(std::span<const PlayerMove> moves) override;
    void clientProcess(UInt32 curTime) override;

    void setOrbitGhostIndex(Int32 ghostIndex) noexcept {
        explicitGhostIndex_ = ghostIndex;
    }

    [[nodiscard]] Int32 orbitGhostIndex() const noexcept {
        return explicitGhostIndex_;
    }

    [[nodiscard]] Int32 clientId() const noexcept {
        return clientId_;
    }

    void setClientId(Int32 id) noexcept {
        clientId_ = id;
    }

    [[nodiscard]] Int32 mode() const noexcept {
        return mode_;
    }

    [[nodiscard]] const Point3F& curOrbitPoint() const noexcept {
        return curOrbitPoint_;
    }

    [[nodiscard]] const EulerF& rotation() const noexcept {
        return rotation_;
    }

    [[nodiscard]] Flt32 orbitDist() const noexcept {
        return orbitDist_;
    }

    [[nodiscard]] Flt32 minOrbitDist() const noexcept {
        return minOrbitDist_;
    }

    [[nodiscard]] Flt32 maxOrbitDist() const noexcept {
        return maxOrbitDist_;
    }

    [[nodiscard]] GameBase* orbitObject() const noexcept {
        return orbitObject_;
    }

    [[nodiscard]] bool isInMissionArea() const noexcept {
        return inMissionArea_;
    }

    void setInMissionArea(bool inside) noexcept {
        inMissionArea_ = inside;
    }

    [[nodiscard]] const std::vector<ScriptCallbackRecord>& callbackHistory() const noexcept {
        return callbackHistory_;
    }

    void clearCallbackHistory() noexcept {
        callbackHistory_.clear();
    }

    [[nodiscard]] const TMat3F& getEyeTransform() const override;
    void getCameraTransform(Flt32 camDist, TMat3F* transform) const override;
    [[nodiscard]] Flt32 getDamageFlash() const noexcept override;
    [[nodiscard]] Flt32 getEnergyLevel() const noexcept override;
    [[nodiscard]] Flt32 getDamageLevel() const noexcept override;

    void setPlayerControl(bool ctrl) override;
    void buildScopeAndCameraInfo(sim::network::GhostManager& cr,
                                 sim::SimNetObject::CameraInfo& camInfo) override;

    void writePacketData(BitStream& bstream) const override;
    void writePacketData(BitStream& bstream, const sim::network::GhostManager* ghosts) const;
    bool readPacketData(BitStream& bstream) override;
    bool readPacketData(BitStream& bstream, sim::network::GhostManager* ghosts);

    [[nodiscard]] UInt16 getGhostTag() const override {
        return static_cast<UInt16>(ObserverCamPersTag);
    }

    UInt32
    packUpdate(sim::network::GhostManager& gm, UInt32 mask, BitStream& stream) const override;
    bool unpackUpdate(sim::network::GhostManager& gm, BitStream& stream) override;

    bool onAdd() override;
    void onRemove() override;
    void onDeleteNotify(sim::SimObject& object) override;
    bool processEvent(const sim::SimEvent& event) override;
    bool processQuery(sim::SimQuery& query) override;

private:
    void invokeObserverCallback(std::string_view fn);

    Int32 clientId_{0};
    Int32 mode_{FlyMode};
    Point3F curOrbitPoint_{0.0f, 0.0f, 0.0f};
    EulerF rotation_{0.0f, 0.0f, 0.0f};
    Flt32 orbitDist_{0.0f};
    Flt32 minOrbitDist_{0.0f};
    Flt32 maxOrbitDist_{0.0f};
    GameBase* orbitObject_{nullptr};
    Int32 explicitGhostIndex_{-1};
    Flt32 damageFlash_{0.0f};
    Flt32 energyLevel_{0.0f};
    Flt32 damageLevel_{0.0f};
    PlayerMove pmLast_{};
    bool inMissionArea_{false};
    mutable TMat3F eyeTransformCache_{};
    std::vector<ScriptCallbackRecord> callbackHistory_{};
};

} // namespace opent1::game
