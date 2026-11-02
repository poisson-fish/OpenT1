#pragma once

#include <opent1/game/game_base.hpp>
#include <opent1/world/interior_light_player.hpp>
#include <opent1/world/interior_shape.hpp>
#include <opent1/world/world_mission.hpp>

#include <array>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace opent1::game {

class InteriorShape : public GameBase {
    using Parent = GameBase;

public:
    enum Constants : Int32 {
        sm_fileVersion = 2,
    };

    enum NetMaskBits : UInt32 {
        PosRotMask = 1U << 0,
        LightActiveMask = 1U << 1,
        EditModeMask = 1U << 2,
        InitialUpdate = 1U << 31,
    };

    struct LightAnimParam {
        Flt32 time{1.0f};
        bool active{true};
        std::array<UInt8, 3> padding{};
        UInt32 dummy1{0};
        UInt32 dummy2{0};
        constexpr bool operator==(const LightAnimParam&) const noexcept = default;
    };

    static_assert(sizeof(LightAnimParam) == 16);

    static inline std::string sm_missionName{};

    InteriorShape();
    ~InteriorShape() override;

    static void initPersistFields();

    void setFileName(std::string_view in_pFileName);

    [[nodiscard]] std::string_view getFileName() const noexcept {
        return fileName_;
    }

    [[nodiscard]] std::string getBaseFilename() const;
    [[nodiscard]] std::string missionLitName() const;

    [[nodiscard]] Int32 isContainer() const noexcept {
        return isContainer_;
    }

    void setIsContainer(Int32 c) noexcept {
        isContainer_ = c;
    }

    [[nodiscard]] const Point3F& shapePosition() const noexcept {
        return shapePosition_;
    }

    [[nodiscard]] const EulerF& shapeRotation() const noexcept {
        return shapeRotation_;
    }

    void setShapePositionAndRotation(const Point3F& pos, const EulerF& rot);

    [[nodiscard]] const std::vector<LightAnimParam>& lightAnimParams() const noexcept {
        return lightAnimParams_;
    }

    [[nodiscard]] std::vector<LightAnimParam>& lightAnimParams() noexcept {
        return lightAnimParams_;
    }

    void setLightAnimParams(std::vector<LightAnimParam> params);

    [[nodiscard]] world::InteriorLightPlayer* lightPlayer() noexcept {
        return lightPlayer_.get();
    }

    void invalidateLightMaps() {
        lightMapsInvalidated_ = true;
    }

    [[nodiscard]] const std::vector<UInt32>& checkVals() const noexcept {
        return checkVals_;
    }

    void setCheckVals(std::vector<UInt32> vals) {
        checkVals_ = std::move(vals);
    }

    [[nodiscard]] bool disableCollisions() const noexcept {
        return disableCollisions_;
    }

    void setDisableCollisions(bool disable) noexcept {
        disableCollisions_ = disable;
    }

    [[nodiscard]] bool inEditMode() const noexcept {
        return inEditMode_;
    }

    void setInEditMode(bool edit) noexcept {
        if (inEditMode_ != edit) {
            inEditMode_ = edit;
            setMaskBits(EditModeMask);
        }
    }

    [[nodiscard]] bool isDestroyed() const noexcept {
        return destroyed_;
    }

    void setDestroyed(bool destroyed) noexcept {
        destroyed_ = destroyed;
    }

    [[nodiscard]] UInt32 currentState() const noexcept {
        return currentState_;
    }

    void setCurrentState(UInt32 st) noexcept {
        currentState_ = st;
    }

    [[nodiscard]] std::string_view teamDmlName() const noexcept {
        return teamDmlName_;
    }

    void setShapeResources(std::shared_ptr<world::InteriorShapeResources> res);

    [[nodiscard]] const std::shared_ptr<world::InteriorShapeResources>&
    shapeResources() const noexcept {
        return shapeResources_;
    }

    [[nodiscard]] world::InteriorMissionState toMissionState() const;
    bool fromMissionState(const world::InteriorMissionState& state);

    void setTeam(Int32 team) override;
    void remapTeamTextures(Int32 in_newTeam);

    bool getWeatherDistance(const Point3F& in_rWorldPt, Flt32& out_rDistance);
    void getITRSpaceBBox(Box3F& out_rBox) const;
    bool getAbuttingFace(Int32 in_outsideBits,
                         const Point2F& in_rMinPt,
                         const Point2F& in_rMaxPt,
                         Point3F* out_pV0,
                         Point3F* out_pV1,
                         Point3F* out_pV2,
                         Point3F* out_pV3) const;

    bool processArguments(std::span<const std::string> args) override;
    bool onAdd() override;
    void onRemove() override;
    void clientProcess(UInt32 curTime) override;
    bool processEvent(const sim::SimEvent& event) override;
    bool processQuery(sim::SimQuery& query) override;

    bool inspectRead(sim::Inspect& inspector) override;
    bool inspectWrite(sim::Inspect& inspector) const override;

    [[nodiscard]] UInt16 getGhostTag() const override {
        return static_cast<UInt16>(InteriorShapePersTag);
    }

    UInt32
    packUpdate(sim::network::GhostManager& gm, UInt32 mask, BitStream& stream) const override;
    bool unpackUpdate(sim::network::GhostManager& gm, BitStream& stream) override;

    Int32 version() const noexcept override {
        return sm_fileVersion;
    }

    Persistent::Error
    read(StreamIO& sio, Int32 version, Int32 user, Persistent::LoadContext& ctx) override;
    Persistent::Error
    write(StreamIO& sio, Int32 version, Int32 user, Persistent::StoreContext& ctx) const override;

protected:
    Box3F buildContainerBox(const TMat3F& in_rMat) const override;
    void setShapeTransform();
    void initLightParams();
    void shutdownLights();
    void restartLights();

    void writeCheckVals(BitStream& stream) const;
    bool readCheckVals(BitStream& stream, std::vector<UInt32>& outVals) const;
    void writeInitialPacket(BitStream& stream) const;
    bool readInitialPacket(BitStream& stream);

private:
    std::string fileName_{};
    Int32 isContainer_{-1};
    Point3F shapePosition_{0.0f, 0.0f, 0.0f};
    EulerF shapeRotation_{0.0f, 0.0f, 0.0f};
    std::vector<LightAnimParam> lightAnimParams_{};
    std::vector<UInt32> checkVals_{};
    bool disableCollisions_{false};
    bool inEditMode_{false};
    bool destroyed_{false};
    UInt32 lastUpdatedTime_{0};
    UInt32 currentState_{0};
    std::string teamDmlName_{};
    std::shared_ptr<world::InteriorShapeResources> shapeResources_{};
    std::unique_ptr<world::InteriorLightPlayer> lightPlayer_{};
    mutable std::shared_ptr<const world::WorldSurfaceMesh> cachedSurfaceMesh_{};
    bool lightMapsInvalidated_{};
    mutable std::shared_ptr<const world::WorldSurfaceMesh> visibleSource_, visibleMesh_;
    mutable std::vector<world::InteriorLightSlot> cachedLightSlots_;
    mutable std::vector<UInt32> visibleSurfaces_;
    std::shared_ptr<const world::SimInteriorCollisionImage> collisionImage_{};
};

} // namespace opent1::game
