#pragma once
#include <opent1/game/game_base.hpp>
#include <opent1/world/ts_shape_instance.hpp>
#include <opent1/world/world_scene.hpp>
#include <array>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace opent1::game {

class ShapeBase : public GameBase {
    using Parent = GameBase;

public:
    enum Constants : Int32 {
        MaxScriptThreads = 3,
        MaxSequenceIndex = 16,
        ThreadSequenceBits = 4,
        MaxSoundChannels = 2,
    };

    enum NetMaskBits : UInt32 {
        PositionMask = 1u << 3,
        RotationMask = 1u << 4,
        NoInterpMask = 1u << 5,
        VelocityMask = 1u << 5,
        ShieldMask = 1u << 7,
        DamageMask = 1u << 8,
        SBFadeMask = 1u << 9,
        ThreadMask = 1u << 10,
        ThreadNMask = 1u << 11, // MaxScriptThreads bits (bits 11, 12, 13)
        NextFreeMask = 1u << 14,
    };

    enum FadeState : Int32 {
        NotFading = 0,
        FadingOut = 1,
        FadingIn = 2,
    };

    static inline constexpr UInt32 csm_fadeLength = 2500;
    static inline Int32 prefDamageSkinDetail = 2;
    static inline Int32 prefShadowDetailMask = 1;
    static inline Flt32 prefShadowDetailScale = 1.0f;
    static inline bool renderBoundingBox = false;
    static inline bool renderLightRays = false;
    static inline bool g_lowDetailObjectLighting = true;

    struct SThread {
        enum State : Int32 {
            Play = 0,
            Stop = 1,
            Pause = 2,
        };

        std::optional<UInt32> threadIndex{};
        UInt32 sound{0};
        Int32 state{Play};
        Int32 sequence{-1};
        bool atEnd{false};
        bool forward{true};
    };

    class RenderImage final : public FearRenderImage {
    public:
        ShapeBase* pOwner{nullptr};
        sim::SimContainer* root{nullptr};
        Flt32 alphaLevel{1.0f};
        Flt32 hazeValue{0.0f};
        Point3F lightDirection{0.0f, 0.0f, -1.0f};
        Flt32 shadowDetailScale{1.0f};

        RenderImage() = default;
        ~RenderImage() override = default;

        void setContainerBox(const Box3F& box) noexcept {
            m_containerBox = box;
        }
    };

    ShapeBase();
    ~ShapeBase() override;

    bool onAdd() override;
    void onRemove() override;
    bool initResources(GameBaseData* dat) override;
    bool processEvent(const sim::SimEvent& event) override;
    bool processQuery(sim::SimQuery& query) override;

    [[nodiscard]] std::string_view getScriptDefaultClass() const noexcept override {
        return "ShapeBase";
    }

    // Position & rotation
    void setPos(const Point3F& pos);
    void setRot(const Point3F& rot);

    [[nodiscard]] const Point3F& getPos() const noexcept {
        return getLinearPosition();
    }

    [[nodiscard]] const Point3F& getRot() const noexcept {
        return rotation_;
    }

    void updateMovement(Flt32 slice);

    // Shape & animation threads
    [[nodiscard]] world::TsShapeInstance* shapeInstance() const noexcept {
        return shapeInstance_.get();
    }

    [[nodiscard]] std::shared_ptr<const world::TsShapeInstance>
    retainedShapeInstance() const noexcept {
        return shapeInstance_;
    }

    void setShapeInstance(std::shared_ptr<world::TsShapeInstance> inst);
    [[nodiscard]] const world::TsShape* getTSShape() const noexcept;
    [[nodiscard]] Flt32 getRadius() const noexcept;
    [[nodiscard]] Point3F getCenter() const noexcept;
    [[nodiscard]] const Point3F& getAnimTransform() const noexcept;
    void clearAnimTransform() noexcept;
    [[nodiscard]] const TMat3F& rootDeltaTransform() const noexcept;

    void setRootDeltaTransform(const TMat3F& mat) noexcept {
        rootDeltaTransform_ = mat;
    }

    [[nodiscard]] std::optional<UInt32> createThread(Int32 seqNum);
    bool setSequence(Int32 slot, Int32 seq, bool reset = true);
    void updateSequence(SThread& st);
    void stopSequenceSound(SThread& st);
    void startSequenceSound(SThread& st);
    bool stopSequence(Int32 slot);
    bool pauseSequence(Int32 slot);
    bool playSequence(Int32 slot);
    bool setSequenceDir(Int32 slot, bool forward);
    void updateScriptThreads(SimTime t);

    [[nodiscard]] const SThread& getScriptThread(Int32 slot) const noexcept {
        return scriptThread_[static_cast<std::size_t>(slot)];
    }

    [[nodiscard]] SThread& getScriptThread(Int32 slot) noexcept {
        return scriptThread_[static_cast<std::size_t>(slot)];
    }

    // Energy, damage, repair & shield
    void setEnergy(Flt32 energy);
    void setRechargeRate(Flt32 rate);

    [[nodiscard]] Flt32 getRechargeRate() const noexcept {
        return rechargeRate_;
    }

    void updateEnergy(SimTime t);

    [[nodiscard]] Flt32 getEnergy() const noexcept {
        return energy_;
    }

    [[nodiscard]] Flt32 getEnergyLevel() const noexcept override;

    virtual void setDamageLevel(Flt32 level);
    bool repairDamage(Flt32 amount);

    void setAutoRepairRate(Flt32 rate) noexcept {
        autoRepairRate_ = rate;
    }

    [[nodiscard]] Flt32 getAutoRepairRate() const noexcept {
        return autoRepairRate_;
    }

    [[nodiscard]] Flt32 getRepairReserve() const noexcept {
        return repairReserve_;
    }

    void updateDamage(SimTime t);
    virtual void applyDamage(Int32 type,
                             Flt32 value,
                             const Point3F& pos,
                             const Point3F& vec,
                             const Point3F& mom,
                             sim::SimObjectId objectId);

    [[nodiscard]] Flt32 getDamageLevel() const noexcept override {
        return damageLevel_;
    }

    [[nodiscard]] virtual std::string_view getDamageState() const noexcept {
        return "Enabled";
    }

    [[nodiscard]] bool getCommandStatus(CommandStatus* status) const override;

    [[nodiscard]] bool isDestroyable() const noexcept {
        return destroyable_;
    }

    void setDestroyable(bool destroyable) noexcept {
        destroyable_ = destroyable;
    }

    [[nodiscard]] virtual Int32 getDamageSkinLevel(Int32 detailLevel) const noexcept;

    [[nodiscard]] virtual std::string_view getSkinBase() const noexcept;

    void reSkin(bool force = false);

    [[nodiscard]] Int32 currentSkinDamage() const noexcept {
        return curSkinDamage_;
    }

    [[nodiscard]] std::string_view currentSkin() const noexcept {
        return curSkin_;
    }

    void setShieldVec(Point3F hv, Flt32 zoff = 0.0f);
    void setShieldThread(Flt32 pos);
    void updateShieldThread(SimTime t);

    [[nodiscard]] const Point3F& shieldVec() const noexcept {
        return shieldVec_;
    }

    [[nodiscard]] Flt32 shieldZOffset() const noexcept {
        return zOffset_;
    }

    [[nodiscard]] const RenderImage& renderImage() const noexcept {
        return image_;
    }

    [[nodiscard]] RenderImage& renderImage() noexcept {
        return image_;
    }

    [[nodiscard]] const FearRenderImage& shieldRenderImage() const noexcept {
        return shieldImage_;
    }

    // Explosion / Hulk state
    void blowUp();
    void unBlowUp();

    [[nodiscard]] bool isBlownUp() const noexcept {
        return blowedUpGood_;
    }

    // Fade in / out
    void startFadeOut(UInt32 explicitCurrentTime = 0);
    void startFadeIn(UInt32 explicitCurrentTime = 0);

    [[nodiscard]] FadeState fadeState() const noexcept {
        return m_fading_;
    }

    [[nodiscard]] UInt32 fadeStartedTime() const noexcept {
        return m_fadeStarted_;
    }

    [[nodiscard]] Flt32 computeFadeFactor(UInt32 currentTime) noexcept;

    // Sun attenuation & LOS
    void updateSunRollAv() noexcept;
    void updateSunOverride(bool overrideSun,
                           Flt32 sunAttenuation,
                           const ColorF& positionalColor) noexcept;

    [[nodiscard]] bool isAttenuatingSun() const noexcept {
        return m_attenuateSun_;
    }

    [[nodiscard]] Flt32 sunAttenuationFactor() const noexcept {
        return m_attenuationFactor_;
    }

    [[nodiscard]] const ColorF& sunAttenuationColor() const noexcept {
        return m_attenuationColor_;
    }

    [[nodiscard]] UInt32 sunAttenuationTicks() const noexcept {
        return m_ticks_;
    }

    [[nodiscard]] Flt32 coverage(Point3F eye) const;
    [[nodiscard]] virtual TMat3F getLOSTransform() const noexcept;
    sim::SimObject*
    findLOSObject(const TMat3F& mat, Flt32 range, UInt32 mask, Point3F* pos, Point3F* normal) const;
    bool aimedTransform(TMat3F* eyeMat, Point3F startPoint) const;
    bool getAimedMuzzleTransform(Int32 slot, TMat3F* mat) const;

    bool getLOSColor(const Point3F& shootPoint,
                     const Point3F& vec,
                     Flt32 dist,
                     ColorF& outColor,
                     Flt32& outFactor,
                     sim::SimContainer*& outContainer) const;
    bool getPositionalColor(ColorF& outColor, Flt32& outSunAttenuation) const;

    // Network ghosting & persistence
    UInt32
    packUpdate(sim::network::GhostManager& gm, UInt32 mask, BitStream& stream) const override;
    bool unpackUpdate(sim::network::GhostManager& gm, BitStream& stream) override;
    bool packShapeBaseUpdate(UInt32 mask, BitStream& stream, UInt32 currentTime = 0) const;
    bool unpackShapeBaseUpdate(BitStream& stream, UInt32 currentTime = 0);

    Persistent::Error
    read(StreamIO& sio, Int32 version, Int32 user, Persistent::LoadContext& ctx) override;
    Persistent::Error
    write(StreamIO& sio, Int32 version, Int32 user, Persistent::StoreContext& ctx) const override;

protected:
    bool onSimObjectTransformEvent(const sim::SimObjectTransformEvent& event);
    bool onSimObjectTransformQuery(sim::SimObjectTransformQuery& query) const;
    bool onSimImageTransformQuery(sim::SimImageTransformQuery& query) const;
    bool onWorldShapeQuery(world::WorldShapeQuery& query) const;
    [[nodiscard]] std::optional<world::TsMaterialResources>
        damageMaterials(world::TsMaterialResources) const;

    ShapeBaseData* shapeData_{nullptr};
    Point3F rotation_{0.0f, 0.0f, 0.0f};
    Point3F shieldVec_{0.0f, 1.0f, 0.0f};
    Flt32 zOffset_{0.0f};
    Flt32 energy_{0.0f};
    Flt32 rechargeRate_{0.0f};
    Flt32 damageLevel_{0.0f};
    Flt32 autoRepairRate_{0.0f};
    Flt32 repairReserve_{0.0f};
    Flt32 movementStepSize_{1.0f};
    bool destroyable_{true};
    bool blowedUpGood_{false};

    Int32 curSkinDamage_{0};
    std::string curSkin_{"base"};

    mutable FadeState m_fading_{NotFading};
    UInt32 m_fadeStarted_{0};

    UInt32 m_lastSunUpdated_{0};
    ColorF m_attenuationColor_{1.0f, 1.0f, 1.0f};
    Flt32 m_attenuationFactor_{0.0f};
    ColorF m_rollColor_{1.0f, 1.0f, 1.0f};
    Flt32 m_rollFactor_{0.0f};
    UInt32 m_ticks_{0};
    bool m_attenuateSun_{false};

    TMat3F rootDeltaTransform_{};
    std::shared_ptr<world::TsShapeInstance> shapeInstance_;
    std::shared_ptr<world::TsShapeInstance> shieldShapeInstance_;
    std::optional<UInt32> shieldThreadIndex_{};
    std::array<SThread, MaxScriptThreads> scriptThread_{};
    std::array<UInt32, MaxSoundChannels> soundChannel_{};
    RenderImage image_{};
    FearRenderImage shieldImage_{};
    mutable std::optional<world::TsShapeResources> cachedWorldResources_{};
    mutable std::optional<world::TsShapeResources> cachedShieldResources_{};
};

} // namespace opent1::game
